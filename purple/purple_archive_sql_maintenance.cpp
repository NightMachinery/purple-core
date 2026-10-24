/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_archive_sql_maintenance.h"

#include "purple/purple_archive_sql_internal.h"

#include "purple/purple_sync_json.h"

#include <QtCore/QJsonDocument>

#include <algorithm>
#include <limits>
#include <map>

namespace Purple {
namespace {

using Values = std::vector<ArchiveSqlValue>;

bool IsHeader(const ArchiveState &state) {
	return state.messages.empty() && state.blobs.empty() && state.jobs.empty()
		&& state.leases.empty() && state.chatGenerations.empty() && IsArchiveState(state);
}

QString Operation(const ArchiveSqlPurge &purge, const ArchiveSqlSecurity &security) {
	return ArchiveSqlToken(purge.account, ArchiveIndexKind::Maintenance, purge.operationId.toUtf8(), security);
}

bool ScopedChat(const ArchiveSqlPurge &purge, const ArchiveChatKey &chat) {
	return !purge.chat || SameArchiveChat(*purge.chat, chat)
		|| std::any_of(purge.migrationAliases.begin(), purge.migrationAliases.end(), [&](const auto &alias) { return SameArchiveChat(alias, chat); });
}

bool IsScope(const std::optional<ArchiveChatKey> &chat, const std::vector<ArchiveChatKey> &aliases) {
	if (!chat) { return aliases.empty(); }
	if (!IsArchiveChatKey(*chat) || aliases.size() > 31) { return false; }
	for (auto i = size_t(0); i != aliases.size(); ++i) {
		if (!IsArchiveChatKey(aliases[i]) || SameArchiveChat(*chat, aliases[i])) { return false; }
		for (auto j = size_t(0); j != i; ++j) { if (SameArchiveChat(aliases[i], aliases[j])) { return false; } }
	}
	return true;
}

QString ScopeToken(const ArchiveSqlPurge &purge, const ArchiveSqlSecurity &security) {
	auto input = QByteArray::number(int(purge.kind)) + '/';
	if (purge.chat) {
		const auto chat = ArchiveSqlChatId(*purge.chat, purge.account, security);
		if (chat.isEmpty()) { return {}; }
		input += chat.toUtf8();
		for (const auto &alias : purge.migrationAliases) {
			const auto token = ArchiveSqlChatId(alias, purge.account, security);
			if (token.isEmpty()) { return {}; }
			input += '/' + token.toUtf8();
		}
	}
	return ArchiveSqlToken(purge.account, ArchiveIndexKind::Maintenance, QByteArray("scope/") + input, security);
}

bool MatchesFrame(const AccountKey &account, const QString &id, const ArchiveRow &row,
		const QByteArray &frame, const ArchiveSqlSecurity &security) {
	const auto plaintext = OpenArchiveEnvelope({ account, security.storeId, ArchiveRowKind(row), id.toUtf8() },
		security.keyId, frame, security.crypto);
	const auto encoded = EncodeArchiveRow(row);
	return plaintext && encoded && plaintext.bytes == *encoded;
}

bool IsPurge(const ArchiveSqlPurge &purge, const ArchiveSqlSecurity &security) {
	return purge.storeId == security.storeId && IsArchiveToken(purge.operationId)
		&& (purge.kind == ArchivePurgeKind::Trashbin || purge.kind == ArchivePurgeKind::EditHistory)
		&& IsScope(purge.chat, purge.migrationAliases) && purge.revision > 0
		&& int(purge.phase) >= 0 && int(purge.phase) <= int(ArchivePurgePhase::Finish)
		&& (purge.cursor.isEmpty() || IsArchiveHash(purge.cursor)
			|| (purge.cursor.size() == 129 && purge.cursor[64] == '/'
				&& IsArchiveHash(purge.cursor.left(64)) && IsArchiveHash(purge.cursor.mid(65))))
		&& !Operation(purge, security).isEmpty();
}

std::optional<QByteArray> PurgeBytes(const ArchiveSqlPurge &purge) {
	auto object = QJsonObject{
		{ u"format"_q, u"purple-archive-purge"_q }, { u"version"_q, 1 },
		{ u"account"_q, AccountKeyText(purge.account.environment, purge.account.userId).value_or(QString()) },
		{ u"store"_q, purge.storeId }, { u"operation"_q, purge.operationId },
		{ u"kind"_q, int(purge.kind) }, { u"revision"_q, QString::number(purge.revision) },
		{ u"generation"_q, QString::number(purge.accountGeneration) },
		{ u"phase"_q, int(purge.phase) }, { u"cursor"_q, purge.cursor },
	};
	object.insert(u"chat"_q, purge.chat ? QJsonValue(QJsonArray{ int(purge.chat->kind), QString::number(purge.chat->id) })
		: QJsonValue(QJsonValue::Null));
	auto aliases = QJsonArray();
	for (const auto &alias : purge.migrationAliases) { aliases.push_back(QJsonArray{ int(alias.kind), QString::number(alias.id) }); }
	object.insert(u"aliases"_q, aliases);
	const auto bytes = CanonicalizeSyncJson(QJsonDocument(object).toJson(QJsonDocument::Compact));
	return bytes ? std::optional(bytes.canonical) : std::nullopt;
}

std::optional<QByteArray> SealPurge(const ArchiveSqlPurge &purge, const ArchiveSqlSecurity &security) {
	const auto plaintext = PurgeBytes(purge);
	if (!plaintext) { return {}; }
	const auto sealed = SealArchiveEnvelope({ purge.account, purge.storeId, ArchiveRecordKind::Maintenance,
		Operation(purge, security).toUtf8() }, security.keyId, *plaintext, security.crypto);
	return sealed ? std::optional(sealed.bytes) : std::nullopt;
}

void Guard(ArchiveSqlProgram &program, const ArchiveState &header, const ArchiveSqlSecurity &security) {
	program.statements.push_back({ u"BEGIN IMMEDIATE"_q });
	program.statements.push_back({ u"SELECT 1 FROM archive_meta WHERE singleton=1 AND schema_version=? AND schema_fingerprint=? AND account_token=? "
		"AND store_id=? AND revision=? AND generation=? AND maintenance_id=''"_q,
		{ int64(kArchiveSqlSchemaVersion), ArchiveSqlExpectedSchemaFingerprint(), ArchiveSqlToken(header.account, ArchiveIndexKind::Account, "account", security), security.storeId,
			QString::number(header.revision), QString::number(header.accountGeneration) }, ArchiveSqlExpectation::OneRow });
}

bool AdvanceHeader(ArchiveSqlProgram &program, const ArchiveState &before, const ArchiveState &next,
		const ArchiveSqlSecurity &security, const QString &maintenance = {}) {
	const auto encrypted = SealArchiveSqlRow(next.account, u"account"_q, next, security);
	if (!encrypted) { return false; }
	program.statements.push_back({ u"UPDATE archive_meta SET revision=?,generation=?,maintenance_id=?,envelope=? "
		"WHERE singleton=1 AND revision=?"_q,
		{ QString::number(next.revision), QString::number(next.accountGeneration), maintenance,
			*encrypted, QString::number(before.revision) }, ArchiveSqlExpectation::OneChange });
	return true;
}

struct PageQuery {
	QString table;
	QString id;
	QString predicate;
	Values values;
};

PageQuery Query(const ArchiveSqlPurge &purge, const ArchiveSqlSecurity &security) {
	auto result = PageQuery();
	switch (purge.phase) {
	case ArchivePurgePhase::Messages:
		result.table = u"messages"_q;
		result.id = u"message_key"_q;
		result.predicate = purge.kind == ArchivePurgeKind::Trashbin ? u"deleted=1 AND purged=0"_q : u"head_version<>''"_q;
		break;
	case ArchivePurgePhase::Versions:
		result.table = u"versions"_q;
		result.id = u"message_key||'/'||version_id"_q;
		result.predicate = u"EXISTS(SELECT 1 FROM media_refs r WHERE r.message_key=versions.message_key AND r.version_id=versions.version_id AND r.state<>2)"_q;
		result.predicate += purge.kind == ArchivePurgeKind::Trashbin
			? u" AND EXISTS(SELECT 1 FROM messages m WHERE m.message_key=versions.message_key AND m.deleted=0)"_q
			: u" AND EXISTS(SELECT 1 FROM messages m WHERE m.message_key=versions.message_key AND m.head_version=versions.version_id)"_q;
		break;
	case ArchivePurgePhase::Jobs:
		result.table = u"jobs"_q; result.id = u"job_id"_q; result.predicate = u"state=0"_q; break;
	case ArchivePurgePhase::Leases:
		result.table = u"leases"_q; result.id = u"lease_id"_q; result.predicate = u"revoked=0"_q; break;
	case ArchivePurgePhase::Finish: return {};
	}
	if (purge.chat) {
		result.predicate += u" AND chat_token IN (?"_q;
		result.values.push_back(ArchiveSqlChatId(*purge.chat, purge.account, security));
		for (const auto &alias : purge.migrationAliases) {
			result.predicate += u",?"_q;
			result.values.push_back(ArchiveSqlChatId(alias, purge.account, security));
		}
		result.predicate += ')';
	}
	return result;
}

bool PutVersion(ArchiveSqlProgram &program, const ArchiveVersionSnapshot &snapshot,
		const QByteArray &originalFrame, const ArchiveSqlSecurity &security) {
	const auto &key = snapshot.key;
	const auto &version = snapshot.version;
	const auto message = ArchiveSqlMessageId(key, security);
	const auto rowId = ArchiveSqlChildId(key, ArchiveIndexKind::Version, version.id, security);
	const auto versionToken = rowId.mid(rowId.lastIndexOf('/') + 1);
	const auto plaintext = OpenArchiveEnvelope({ key.account, security.storeId, ArchiveRecordKind::Version, rowId.toUtf8() },
		security.keyId, originalFrame, security.crypto);
	const auto original = plaintext ? DecodeArchiveRow(plaintext.bytes) : ArchiveRowDecodeResult();
	if (!original.row || ArchiveRowKind(*original.row) != ArchiveRecordKind::Version) { return false; }
	auto rewritten = *original.row;
	rewritten.value = snapshot;
	const auto encrypted = SealArchiveSqlStoredRow(key.account, rowId, rewritten, security);
	if (!encrypted) { return false; }
	program.statements.push_back({ u"UPDATE versions SET envelope=? WHERE message_key=? AND version_id=?"_q,
		{ *encrypted, message, versionToken }, ArchiveSqlExpectation::OneChange });
	program.statements.push_back({ u"DELETE FROM media_refs WHERE message_key=? AND version_id=?"_q,
		{ message, versionToken }, ArchiveSqlExpectation::AnyChanges });
	for (auto i = size_t(0); i != version.media.size(); ++i) {
		const auto &ref = version.media[i];
		const auto descriptor = std::find_if(version.content.media.begin(), version.content.media.end(),
			[&](const auto &entry) { return entry.identity == ref.identity; });
		if (descriptor == version.content.media.end()) { return false; }
		const auto media = ArchiveSqlToken(key.account, ArchiveIndexKind::Media,
			(ref.identity + '/' + descriptor->kind).toUtf8(), security);
		const auto blob = ref.blobId.isEmpty() ? ArchiveSqlValue(nullptr)
			: ArchiveSqlValue(ArchiveSqlToken(key.account, ArchiveIndexKind::Blob, ref.blobId.toUtf8(), security));
		program.statements.push_back({ u"INSERT INTO media_refs VALUES(?,?,?,?,?,?)"_q,
			{ message, versionToken, media, int64(i), int64(ref.state), blob }, ArchiveSqlExpectation::OneChange });
	}
	return true;
}

struct PageEvidence {
	std::map<QString, std::pair<QByteArray, ArchiveVersionSnapshot>> versions;
	std::map<QString, std::pair<QByteArray, ArchiveBlob>> blobs;
};

bool CheckVersionMediaIndex(ArchiveSqlProgram &program, const ArchiveVersionSnapshot &snapshot,
		const ArchiveSqlSecurity &security) {
	const auto message = ArchiveSqlMessageId(snapshot.key, security);
	const auto row = ArchiveSqlChildId(snapshot.key, ArchiveIndexKind::Version, snapshot.version.id, security);
	const auto version = row.mid(row.lastIndexOf('/') + 1);
	if (message.isEmpty() || version.isEmpty() || snapshot.version.media.size() > kArchiveSqlPurgeMediaLimit) { return false; }
	program.statements.push_back({ u"SELECT 1 WHERE (SELECT COUNT(*) FROM media_refs WHERE message_key=? AND version_id=?)=?"_q,
		{ message, version, int64(snapshot.version.media.size()) }, ArchiveSqlExpectation::OneRow });
	if (!snapshot.version.media.empty()) {
		auto predicate = QString();
		auto values = Values{ message, version };
		for (auto i = size_t(0); i != snapshot.version.media.size(); ++i) {
			const auto &ref = snapshot.version.media[i];
			const auto descriptor = std::find_if(snapshot.version.content.media.begin(), snapshot.version.content.media.end(),
				[&](const auto &entry) { return entry.identity == ref.identity; });
			if (descriptor == snapshot.version.content.media.end()) { return false; }
			const auto media = ArchiveSqlToken(snapshot.key.account, ArchiveIndexKind::Media,
				(ref.identity + '/' + descriptor->kind).toUtf8(), security);
			const auto blob = ref.blobId.isEmpty() ? ArchiveSqlValue(nullptr)
				: ArchiveSqlValue(ArchiveSqlToken(snapshot.key.account, ArchiveIndexKind::Blob, ref.blobId.toUtf8(), security));
			if (media.isEmpty() || (!ref.blobId.isEmpty() && std::get<QString>(blob).isEmpty())) { return false; }
			if (!predicate.isEmpty()) { predicate += u" OR "_q; }
			predicate += u"(media_token=? AND position=? AND state=? AND blob_id IS ?)"_q;
			values.insert(values.end(), { media, int64(i), int64(ref.state), blob });
		}
		values.push_back(int64(snapshot.version.media.size()));
		program.statements.push_back({ u"SELECT 1 WHERE (SELECT COUNT(*) FROM media_refs WHERE message_key=? AND version_id=? AND ("_q
			+ predicate + u"))=?"_q, std::move(values), ArchiveSqlExpectation::OneRow });
	}
	if (!IsBoundedArchiveSqlProgram(program)) { program.status = ArchiveSqlStatus::TooLarge; return false; }
	return true;
}

const ArchiveVersionSnapshot *CheckVersionEvidence(ArchiveSqlProgram &program, const AccountKey &account,
		const QString &id, const QByteArray &frame, PageEvidence &evidence, const ArchiveSqlSecurity &security) {
	const auto found = evidence.versions.find(id);
	if (found != evidence.versions.end()) { return found->second.first == frame ? &found->second.second : nullptr; }
	if (id.size() != 129) { return nullptr; }
	const auto bytes = OpenArchiveEnvelope({ account, security.storeId, ArchiveRecordKind::Version,
		id.toUtf8() }, security.keyId, frame, security.crypto);
	if (!bytes) { return nullptr; }
	const auto row = DecodeArchiveRow(bytes.bytes);
	if (!row.row || ArchiveRowKind(*row.row) != ArchiveRecordKind::Version) { return nullptr; }
	const auto &snapshot = std::get<ArchiveVersionSnapshot>(row.row->value);
	if (snapshot.key.account != account || ArchiveSqlChildId(snapshot.key, ArchiveIndexKind::Version, snapshot.version.id, security) != id
		|| !CheckVersionMediaIndex(program, snapshot, security)) { return nullptr; }
	program.statements.push_back({ u"SELECT 1 FROM versions WHERE message_key=? AND version_id=? AND envelope=?"_q,
		{ id.left(64), id.mid(65), frame }, ArchiveSqlExpectation::OneRow });
	if (!IsBoundedArchiveSqlProgram(program)) { program.status = ArchiveSqlStatus::TooLarge; return nullptr; }
	return &evidence.versions.emplace(id, std::make_pair(frame, snapshot)).first->second.second;
}

bool CheckOwnedMediaCopy(ArchiveSqlProgram &program, const AccountKey &account,
		const ArchiveOwnedMediaCopy &copy, PageEvidence &evidence, const ArchiveSqlSecurity &security) {
	if (copy.position < 0) { return false; }
	const auto owner = CheckVersionEvidence(program, account, copy.ownerVersionId, copy.ownerFrame, evidence, security);
	if (!owner || uint64_t(copy.position) >= owner->version.media.size()) { return false; }
	const auto &ref = owner->version.media[size_t(copy.position)];
	const auto descriptor = std::find_if(owner->version.content.media.begin(), owner->version.content.media.end(),
		[&](const auto &entry) { return entry.identity == ref.identity; });
	if (descriptor == owner->version.content.media.end() || ref.identity != copy.identity
		|| descriptor->kind != copy.kind || ref.blobId != copy.blobId || ref.state != ArchiveMediaState::Complete) { return false; }
	const auto found = evidence.blobs.find(copy.blobId);
	if (found != evidence.blobs.end()) { return found->second.first == copy.blobFrame; }
	const auto token = ArchiveSqlToken(account, ArchiveIndexKind::Blob, copy.blobId.toUtf8(), security);
	const auto bytes = OpenArchiveEnvelope({ account, security.storeId, ArchiveRecordKind::Blob,
		token.toUtf8() }, security.keyId, copy.blobFrame, security.crypto);
	if (!bytes) { return false; }
	const auto row = DecodeArchiveRow(bytes.bytes);
	if (!row.row || ArchiveRowKind(*row.row) != ArchiveRecordKind::Blob) { return false; }
	const auto &blob = std::get<ArchiveBlob>(row.row->value);
	if (blob.id != copy.blobId || blob.state != ArchiveBlobState::Complete) { return false; }
	program.statements.push_back({ u"SELECT 1 FROM blobs WHERE blob_id=? AND envelope=? AND state=0"_q,
		{ token, copy.blobFrame }, ArchiveSqlExpectation::OneRow });
	if (!IsBoundedArchiveSqlProgram(program)) { program.status = ArchiveSqlStatus::TooLarge; return false; }
	evidence.blobs.emplace(copy.blobId, std::make_pair(copy.blobFrame, blob));
	return true;
}

bool ReconcilePurgeMedia(ArchiveSqlProgram &program, ArchiveVersionSnapshot &snapshot,
		const QByteArray &frame, const std::vector<ArchiveOwnedMediaCopy> &copies,
		PageEvidence &evidence, const ArchiveSqlSecurity &security) {
	const auto id = ArchiveSqlChildId(snapshot.key, ArchiveIndexKind::Version, snapshot.version.id, security);
	const auto original = CheckVersionEvidence(program, snapshot.key.account, id, frame, evidence, security);
	if (!original || *original != snapshot) { return false; }
	for (auto &ref : snapshot.version.media) {
		if (ref.state == ArchiveMediaState::Complete) { continue; }
		const auto descriptor = std::find_if(snapshot.version.content.media.begin(), snapshot.version.content.media.end(),
			[&](const auto &entry) { return entry.identity == ref.identity; });
		if (descriptor == snapshot.version.content.media.end()) { return false; }
		const auto copy = std::find_if(copies.begin(), copies.end(), [&](const auto &entry) {
			return entry.identity == ref.identity && entry.kind == descriptor->kind && IsArchiveHash(entry.blobId);
		});
		if (copy != copies.end()) {
			if (!CheckOwnedMediaCopy(program, snapshot.key.account, *copy, evidence, security)) { return false; }
			ref.state = ArchiveMediaState::Complete;
			ref.blobId = copy->blobId;
		} else {
			const auto token = ArchiveSqlToken(snapshot.key.account, ArchiveIndexKind::Media,
				(ref.identity + '/' + descriptor->kind).toUtf8(), security);
			if (token.isEmpty()) { return false; }
			program.statements.push_back({ u"SELECT 1 WHERE NOT EXISTS(SELECT 1 FROM media_refs r JOIN blobs b ON b.blob_id=r.blob_id "
				"WHERE r.media_token=? AND r.state=2 AND b.state=0)"_q, { token }, ArchiveSqlExpectation::OneRow });
			if (ref.state == ArchiveMediaState::Staged) { ref.state = ArchiveMediaState::Missing; }
		}
		if (!IsBoundedArchiveSqlProgram(program)) { program.status = ArchiveSqlStatus::TooLarge; return false; }
	}
	return true;
}

void FinishProgram(ArchiveSqlProgram &program) {
	if (program && !IsBoundedArchiveSqlProgram(program)) { program.status = ArchiveSqlStatus::TooLarge; }
	if (!program) { program.statements.clear(); }
}

}

ArchiveSqlMaintenancePlan BeginArchiveSqlPurge(const ArchiveState &header,
		const ArchivePurgeRequest &request, const std::vector<ArchiveChatGeneration> &chatEpochs,
		const QString &operationId, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlMaintenancePlan {
		auto result = ArchiveSqlMaintenancePlan();
		if (!IsHeader(header) || header.revision == std::numeric_limits<uint64_t>::max()
			|| request.scope.account != header.account || request.expectedRevision != header.revision
			|| !IsScope(request.scope.chat, request.scope.migrationAliases)) { return result; }
		auto next = header;
		++next.revision;
		if (!request.scope.chat) {
			if (header.accountGeneration == std::numeric_limits<uint64_t>::max() || !chatEpochs.empty()) { return result; }
			++next.accountGeneration;
		} else if (chatEpochs.size() != request.scope.migrationAliases.size() + 1) { return result; }
		result.next = { header.account, security.storeId, operationId, request.kind, request.scope.chat, request.scope.migrationAliases,
			next.revision, next.accountGeneration };
		if (!IsPurge(result.next, security)) { return {}; }
		const auto journal = SealPurge(result.next, security);
		if (!journal) { result.program.status = ArchiveSqlStatus::EncodingFailed; return result; }
		result.program.status = ArchiveSqlStatus::Ready;
		Guard(result.program, header, security);
		auto scopePredicate = QString();
		auto scopeValues = Values();
		if (request.scope.chat) {
			scopePredicate = u" AND chat_token IN (?"_q;
			scopeValues.push_back(ArchiveSqlChatId(*request.scope.chat, header.account, security));
			for (const auto &alias : request.scope.migrationAliases) {
				scopePredicate += u",?"_q;
				scopeValues.push_back(ArchiveSqlChatId(alias, header.account, security));
			}
			scopePredicate += ')';
		}
		for (const auto &table : { u"versions"_q, u"messages"_q, u"jobs"_q, u"leases"_q }) {
			auto values = Values{ int64(kArchiveSqlPurgeEnvelopeLimit) };
			values.insert(values.end(), scopeValues.begin(), scopeValues.end());
			result.program.statements.push_back({ u"SELECT 1 WHERE NOT EXISTS(SELECT 1 FROM "_q + table
				+ u" WHERE length(envelope)>?"_q + scopePredicate + ')', values, ArchiveSqlExpectation::OneRow });
		}
		auto mediaValues = scopeValues;
		mediaValues.push_back(int64(kArchiveSqlPurgeMediaLimit));
		result.program.statements.push_back({ u"SELECT 1 WHERE NOT EXISTS(SELECT 1 FROM media_refs r JOIN versions v USING(message_key,version_id) "
			"WHERE 1=1"_q + scopePredicate + u" GROUP BY r.message_key,r.version_id HAVING COUNT(*)>?)"_q,
			mediaValues, ArchiveSqlExpectation::OneRow });
		auto scopedChats = request.scope.migrationAliases;
		if (request.scope.chat) { scopedChats.push_back(*request.scope.chat); }
		for (const auto &chat : scopedChats) {
			const auto oldEpoch = std::find_if(chatEpochs.begin(), chatEpochs.end(), [&](const auto &entry) { return SameArchiveChat(entry.chat, chat); });
			if (oldEpoch == chatEpochs.end() || oldEpoch->generation == std::numeric_limits<uint64_t>::max()) { return {}; }
			auto epoch = *oldEpoch;
			const auto token = ArchiveSqlChatId(epoch.chat, header.account, security);
			result.program.statements.push_back({ u"SELECT 1 WHERE COALESCE((SELECT generation FROM purge_epochs WHERE chat_token=?),'0')=?"_q,
				{ token, QString::number(epoch.generation) }, ArchiveSqlExpectation::OneRow });
			++epoch.generation;
			const auto encrypted = SealArchiveSqlRow(header.account, token, epoch, security);
			if (!encrypted) { result.program.status = ArchiveSqlStatus::EncodingFailed; FinishProgram(result.program); return result; }
			result.program.statements.push_back({ u"INSERT INTO purge_epochs(chat_token,generation,envelope) VALUES(?,?,?) "
				"ON CONFLICT(chat_token) DO UPDATE SET generation=excluded.generation,envelope=excluded.envelope"_q,
				{ token, QString::number(epoch.generation), *encrypted }, ArchiveSqlExpectation::OneChange });
			if (!IsBoundedArchiveSqlProgram(result.program)) {
				result.program.status = ArchiveSqlStatus::TooLarge;
				FinishProgram(result.program);
				return result;
			}
		}
		const auto operation = Operation(result.next, security);
		result.program.statements.push_back({ u"INSERT INTO maintenance VALUES(1,?,?,?,?,?,?)"_q,
			{ operation, int64(request.kind), ScopeToken(result.next, security),
				int64(ArchivePurgePhase::Versions), QString(), *journal }, ArchiveSqlExpectation::OneChange });
		if (!AdvanceHeader(result.program, header, next, security, operation)) { result.program.status = ArchiveSqlStatus::EncodingFailed; }
		result.program.statements.push_back({ u"COMMIT"_q });
		FinishProgram(result.program);
		return result;
	});
}

std::optional<ArchiveSqlPurge> DecodeArchiveSqlPurge(const QByteArray &plaintext) {
	if (plaintext.size() > 4096) { return {}; }
	const auto canonical = CanonicalizeSyncJson(plaintext);
	if (!canonical) { return {}; }
	auto object = QJsonDocument::fromJson(canonical.canonical).object();
	if (object.take(u"format"_q) != QJsonValue(u"purple-archive-purge"_q)
		|| object.take(u"version"_q) != QJsonValue(1)) { return {}; }
	const auto accountValue = object.take(u"account"_q);
	const auto store = object.take(u"store"_q);
	const auto operation = object.take(u"operation"_q);
	const auto kind = object.take(u"kind"_q);
	const auto revision = object.take(u"revision"_q);
	const auto generation = object.take(u"generation"_q);
	const auto phase = object.take(u"phase"_q);
	const auto cursor = object.take(u"cursor"_q);
	const auto chat = object.take(u"chat"_q);
	const auto aliases = object.take(u"aliases"_q);
	if (!accountValue.isString() || !store.isString() || !operation.isString() || !kind.isDouble()
		|| !aliases.isArray() || aliases.toArray().size() > 31 || !revision.isString() || !generation.isString() || !phase.isDouble() || !cursor.isString() || !object.isEmpty()) { return {}; }
	const auto account = ParseAccountKey(accountValue.toString());
	if (!account || kind.toInteger(-1) < 0 || kind.toInteger() > 1 || phase.toInteger(-1) < 0 || phase.toInteger() > 4) { return {}; }
	auto valid = false;
	const auto rev = revision.toString().toULongLong(&valid);
	if (!valid || QString::number(rev) != revision.toString()) { return {}; }
	const auto gen = generation.toString().toULongLong(&valid);
	if (!valid || QString::number(gen) != generation.toString()) { return {}; }
	auto result = ArchiveSqlPurge{ *account, store.toString(), operation.toString(), ArchivePurgeKind(kind.toInteger()),
		{}, {}, rev, gen, ArchivePurgePhase(phase.toInteger()), cursor.toString() };
	if (!chat.isNull()) {
		const auto array = chat.toArray();
		if (array.size() != 2 || !array[0].isDouble() || !array[1].isString()
			|| array[0].toInteger(-1) < 0 || array[0].toInteger() > 3) { return {}; }
		const auto id = array[1].toString().toLongLong(&valid);
		if (!valid || QString::number(id) != array[1].toString()) { return {}; }
		result.chat = ArchiveChatKey{ ArchiveChatKind(array[0].toInteger()), id };
	}
	for (const auto &alias : aliases.toArray()) {
		const auto array = alias.toArray();
		if (array.size() != 2 || !array[0].isDouble() || !array[1].isString()
			|| array[0].toInteger(-1) < 0 || array[0].toInteger() > 3) { return {}; }
		const auto id = array[1].toString().toLongLong(&valid);
		if (!valid || QString::number(id) != array[1].toString()) { return {}; }
		result.migrationAliases.push_back({ ArchiveChatKind(array[0].toInteger()), id });
	}
	if (!IsArchiveToken(result.storeId) || result.storeId.size() > 128 || !IsArchiveToken(result.operationId)
		|| result.revision == 0 || !IsScope(result.chat, result.migrationAliases)) { return {}; }
	return result;
}

ArchiveSqlStatement ArchiveSqlPurgePage(const ArchiveSqlPurge &purge, int limit,
		const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlStatement {
		if (!IsPurge(purge, security) || limit < 1 || limit > kArchiveSqlPageLimit || purge.phase == ArchivePurgePhase::Finish) { return {}; }
		const auto query = Query(purge, security);
		auto values = query.values;
		values.insert(values.end(), { purge.cursor, int64(limit) });
		auto statement = ArchiveSqlStatement{ u"SELECT "_q + query.id + u",envelope FROM "_q + query.table
			+ u" WHERE "_q + query.predicate + u" AND ("_q + query.id + u")>? ORDER BY "_q + query.id + u" LIMIT ?"_q,
			std::move(values), ArchiveSqlExpectation::Rows };
		statement.rows.maximum = limit;
		return statement;
	});
}

static ArchiveSqlMaintenancePlan ContinueArchiveSqlPurgePage(const ArchiveSqlPurge &purge,
		const std::vector<ArchiveSqlPurgeRow> &rows, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlMaintenancePlan {
		auto result = ArchiveSqlMaintenancePlan();
		result.next = purge;
		auto writes = ArchiveSqlProgram{ ArchiveSqlStatus::Ready };
		auto evidence = PageEvidence();
		if (!IsPurge(purge, security) || rows.size() > kArchiveSqlPageLimit
			|| (purge.phase == ArchivePurgePhase::Finish && !rows.empty())) { return result; }
		const auto operation = Operation(purge, security);
		result.program.status = ArchiveSqlStatus::Ready;
		result.program.statements.push_back({ u"BEGIN IMMEDIATE"_q });
		result.program.statements.push_back({ u"SELECT 1 FROM archive_meta WHERE singleton=1 AND schema_version=? AND schema_fingerprint=? AND account_token=? "
			"AND store_id=? AND revision=? AND generation=? AND maintenance_id=?"_q,
			{ int64(kArchiveSqlSchemaVersion), ArchiveSqlExpectedSchemaFingerprint(), ArchiveSqlToken(purge.account, ArchiveIndexKind::Account, "account", security), purge.storeId,
				QString::number(purge.revision), QString::number(purge.accountGeneration), operation }, ArchiveSqlExpectation::OneRow });
		result.program.statements.push_back({ u"SELECT 1 FROM maintenance WHERE singleton=1 AND operation_id=? AND phase=? AND cursor=? AND kind=? AND scope_token=?"_q,
			{ operation, int64(purge.phase), purge.cursor, int64(purge.kind), ScopeToken(purge, security) }, ArchiveSqlExpectation::OneRow });
		if (purge.phase == ArchivePurgePhase::Finish) {
			writes.statements.push_back({ u"DELETE FROM maintenance WHERE singleton=1 AND operation_id=?"_q, { operation }, ArchiveSqlExpectation::OneChange });
			writes.statements.push_back({ u"UPDATE archive_meta SET maintenance_id='' WHERE singleton=1 AND maintenance_id=?"_q,
				{ operation }, ArchiveSqlExpectation::OneChange });
			result.complete = true;
		} else {
			const auto query = Query(purge, security);
			auto values = query.values;
			if (rows.empty()) {
				values.push_back(purge.cursor);
				result.program.statements.push_back({ u"SELECT 1 WHERE NOT EXISTS(SELECT 1 FROM "_q + query.table
					+ u" WHERE "_q + query.predicate + u" AND ("_q + query.id + u")>?)"_q,
					std::move(values), ArchiveSqlExpectation::OneRow });
				result.next.phase = ArchivePurgePhase(int(purge.phase) + 1);
				result.next.cursor.clear();
			} else {
				values.insert(values.end(), { purge.cursor, rows.back().stored.id, int64(rows.size()) });
				result.program.statements.push_back({ u"SELECT 1 WHERE (SELECT COUNT(*) FROM "_q + query.table
					+ u" WHERE "_q + query.predicate + u" AND ("_q + query.id + u")>? AND ("_q + query.id + u")<=?)=?"_q,
					std::move(values), ArchiveSqlExpectation::OneRow });
				result.next.cursor = rows.back().stored.id;
			}
			auto previous = purge.cursor;
			for (const auto &input : rows) {
				const auto &stored = input.stored;
				if (stored.id <= previous || !stored.row.preserved.isEmpty() || input.copies.size() > kArchiveSqlPageLimit) { return {}; }
				previous = stored.id;
				if (!MatchesFrame(purge.account, stored.id, stored.row, input.frame, security)) { return {}; }
				auto exactValues = query.values;
				exactValues.insert(exactValues.end(), { stored.id, input.frame });
				result.program.statements.push_back({ u"SELECT 1 FROM "_q + query.table + u" WHERE "_q + query.predicate
					+ u" AND ("_q + query.id + u")=? AND envelope=?"_q, std::move(exactValues), ArchiveSqlExpectation::OneRow });
				if (purge.phase == ArchivePurgePhase::Messages) {
					if (ArchiveRowKind(stored.row) != ArchiveRecordKind::Message) { return {}; }
					auto message = std::get<ArchiveMessage>(stored.row.value);
					if (message.key.account != purge.account || !message.versions.empty() || !message.receipts.empty()
						|| ArchiveSqlMessageId(message.key, security) != stored.id
						|| !ScopedChat(purge, message.key.chat)) { return {}; }
					if (purge.kind == ArchivePurgeKind::Trashbin) {
						if (!message.deleted || message.payloadPurged) { return {}; }
						message.payloadPurged = true;
						const auto sealed = SealArchiveSqlRow(purge.account, stored.id, message, security);
						if (!sealed) { return {}; }
						writes.statements.push_back({ u"DELETE FROM versions WHERE message_key=?"_q, { stored.id }, ArchiveSqlExpectation::AnyChanges });
						writes.statements.push_back({ u"UPDATE messages SET purged=1,head_version='',envelope=? WHERE message_key=? AND deleted=1 AND purged=0"_q,
							{ *sealed, stored.id }, ArchiveSqlExpectation::OneChange });
					} else {
						if (!input.head || input.head->key != message.key || input.head->version.order > message.order) { return {}; }
						auto head = *input.head;
						if (!ReconcilePurgeMedia(result.program, head, input.headFrame, input.copies, evidence, security)) {
							return { { result.program.status == ArchiveSqlStatus::TooLarge ? ArchiveSqlStatus::TooLarge : ArchiveSqlStatus::Invalid } };
						}
						const auto headId = ArchiveSqlChildId(head.key, ArchiveIndexKind::Version, head.version.id, security);
						const auto headToken = headId.mid(headId.lastIndexOf('/') + 1);
						const auto originalHead = CheckVersionEvidence(result.program, purge.account, headId, input.headFrame, evidence, security);
						if (!originalHead || *originalHead != *input.head) { return {}; }
						result.program.statements.push_back({ u"SELECT 1 FROM versions WHERE message_key=? AND version_id=? AND envelope=?"_q,
							{ stored.id, headToken, input.headFrame }, ArchiveSqlExpectation::OneRow });
						result.program.statements.push_back({ u"SELECT 1 FROM messages WHERE message_key=? AND head_version=?"_q,
							{ stored.id, headToken }, ArchiveSqlExpectation::OneRow });
						if (!PutVersion(writes, head, input.headFrame, security)) { return {}; }
						writes.statements.push_back({ u"DELETE FROM versions WHERE message_key=? AND version_id<>?"_q,
							{ stored.id, headToken }, ArchiveSqlExpectation::AnyChanges });
					}
				} else if (purge.phase == ArchivePurgePhase::Versions) {
					if (ArchiveRowKind(stored.row) != ArchiveRecordKind::Version) { return {}; }
					auto snapshot = std::get<ArchiveVersionSnapshot>(stored.row.value);
					if (snapshot.key.account != purge.account || !ScopedChat(purge, snapshot.key.chat)
						|| ArchiveSqlChildId(snapshot.key, ArchiveIndexKind::Version, snapshot.version.id, security) != stored.id) { return {}; }
					if (!ReconcilePurgeMedia(result.program, snapshot, input.frame, input.copies, evidence, security)) {
						return { { result.program.status == ArchiveSqlStatus::TooLarge ? ArchiveSqlStatus::TooLarge : ArchiveSqlStatus::Invalid } };
					}
					if (!PutVersion(writes, snapshot, input.frame, security)) { return {}; }
				} else if (purge.phase == ArchivePurgePhase::Jobs) {
					if (ArchiveRowKind(stored.row) != ArchiveRecordKind::Job) { return {}; }
					auto job = std::get<ArchiveMediaJob>(stored.row.value);
					if (job.key.account != purge.account || job.state != ArchiveJobState::Staged
						|| !ScopedChat(purge, job.key.chat)
						|| ArchiveSqlToken(purge.account, ArchiveIndexKind::Job, job.id.toUtf8(), security) != stored.id) { return {}; }
					job.state = ArchiveJobState::Cancelled;
					const auto sealed = SealArchiveSqlRow(purge.account, stored.id, job, security);
					if (!sealed) { return {}; }
					writes.statements.push_back({ u"UPDATE jobs SET state=3,envelope=? WHERE job_id=? AND state=0"_q,
						{ *sealed, stored.id }, ArchiveSqlExpectation::OneChange });
					result.releaseStagedJobs.push_back(job.id);
				} else {
					if (ArchiveRowKind(stored.row) != ArchiveRecordKind::Lease) { return {}; }
					auto lease = std::get<ArchiveReaderLease>(stored.row.value);
					if (lease.key.account != purge.account || lease.revoked
						|| !ScopedChat(purge, lease.key.chat)
						|| ArchiveSqlToken(purge.account, ArchiveIndexKind::Lease, lease.id.toUtf8(), security) != stored.id) { return {}; }
					lease.revoked = true;
					const auto sealed = SealArchiveSqlRow(purge.account, stored.id, lease, security);
					if (!sealed) { return {}; }
					writes.statements.push_back({ u"UPDATE leases SET revoked=1,envelope=? WHERE lease_id=? AND revoked=0"_q,
						{ *sealed, stored.id }, ArchiveSqlExpectation::OneChange });
					result.revokeReaders.push_back(lease.id);
				}
				if (!IsBoundedArchiveSqlProgram(result.program) || !IsBoundedArchiveSqlProgram(writes)) {
					result.program.status = ArchiveSqlStatus::TooLarge;
					FinishProgram(result.program);
					result.releaseStagedJobs.clear();
					result.revokeReaders.clear();
					return result;
				}
			}
			const auto journal = SealPurge(result.next, security);
			if (!journal) { return {}; }
			writes.statements.push_back({ u"UPDATE maintenance SET phase=?,cursor=?,envelope=? WHERE singleton=1 AND operation_id=? AND phase=? AND cursor=?"_q,
				{ int64(result.next.phase), result.next.cursor, *journal, operation, int64(purge.phase), purge.cursor }, ArchiveSqlExpectation::OneChange });
		}
		result.program.statements.insert(result.program.statements.end(),
			std::make_move_iterator(writes.statements.begin()), std::make_move_iterator(writes.statements.end()));
		result.program.statements.push_back({ u"COMMIT"_q });
		FinishProgram(result.program);
		if (result.program) { result.processedRows = rows.size(); }
		return result;
	});
}

ArchiveSqlMaintenancePlan ContinueArchiveSqlPurge(const ArchiveSqlPurge &purge,
		const std::vector<ArchiveSqlPurgeRow> &rows, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlMaintenancePlan {
		if (rows.size() > kArchiveSqlPageLimit) { return {}; }
		auto count = rows.size();
		for (;;) {
			const auto page = std::vector<ArchiveSqlPurgeRow>(rows.begin(), rows.begin() + count);
			auto result = ContinueArchiveSqlPurgePage(purge, page, security);
			if (result.program.status != ArchiveSqlStatus::TooLarge) { return result; }
			if (count <= 1) { return { { ArchiveSqlStatus::TooLarge } }; }
			count = (count + 1) / 2;
		}
	});
}

ArchiveSqlStatement ArchiveSqlPurgeHead(const ArchiveSqlPurge &purge,
		const ArchiveMessageKey &key, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlStatement {
		if (!IsPurge(purge, security) || !IsArchiveMessageKey(key)
			|| key.account != purge.account || !ScopedChat(purge, key.chat)) { return {}; }
		auto result = ArchiveSqlStatement{ u"SELECT v.envelope FROM messages m JOIN versions v ON v.message_key=m.message_key AND v.version_id=m.head_version "
			"WHERE m.message_key=? AND EXISTS(SELECT 1 FROM archive_meta WHERE maintenance_id=?)"_q,
			{ ArchiveSqlMessageId(key, security), Operation(purge, security) }, ArchiveSqlExpectation::Rows };
		result.rows.maximum = 1;
		return result;
	});
}

ArchiveSqlStatement ArchiveSqlFindOwnedMedia(const AccountKey &account, const QString &identity,
		const QString &kind, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlStatement {
		const auto token = ArchiveSqlToken(account, ArchiveIndexKind::Media, (identity + '/' + kind).toUtf8(), security);
		if (token.isEmpty()) { return {}; }
		auto result = ArchiveSqlStatement{ u"SELECT b.blob_id,b.envelope,r.message_key||'/'||r.version_id,v.envelope,r.position FROM media_refs r JOIN blobs b ON b.blob_id=r.blob_id "
			"JOIN messages m ON m.message_key=r.message_key JOIN versions v ON v.message_key=r.message_key AND v.version_id=r.version_id "
			"WHERE r.media_token=? AND r.state=2 AND b.state=0 ORDER BY m.position,v.order_key,r.position LIMIT 1"_q,
			{ token }, ArchiveSqlExpectation::Rows };
		result.rows.maximum = 1;
		return result;
	});
}

ArchiveSqlStatement ArchiveSqlGarbagePage(int limit, const QString &after) {
	if (limit < 1 || limit > kArchiveSqlPageLimit || (!after.isEmpty() && !IsArchiveHash(after))) { return {}; }
	auto statement = ArchiveSqlStatement{ u"SELECT blob_id,envelope FROM blobs b WHERE blob_id>? AND state IN (0,1) "
		"AND NOT EXISTS(SELECT 1 FROM media_refs r WHERE r.blob_id=b.blob_id) "
		"AND NOT EXISTS(SELECT 1 FROM leases l WHERE l.blob_id=b.blob_id) AND EXISTS(SELECT 1 FROM archive_meta WHERE maintenance_id='') ORDER BY blob_id LIMIT ?"_q,
		{ after, int64(limit) }, ArchiveSqlExpectation::Rows };
	statement.rows.maximum = limit;
	return statement;
}

namespace {

ArchiveSqlGcPlan Garbage(const ArchiveState &header, const std::vector<ArchiveBlob> &blobs,
		const ArchiveSqlSecurity &security, bool complete) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlGcPlan {
		auto result = ArchiveSqlGcPlan();
		if (!IsHeader(header) || header.revision == std::numeric_limits<uint64_t>::max()
			|| blobs.empty() || blobs.size() > kArchiveSqlPageLimit) { return result; }
		result.program.status = ArchiveSqlStatus::Ready;
		result.nextHeader = header;
		++result.nextHeader.revision;
		Guard(result.program, header, security);
		auto ordered = std::vector<std::pair<QString, ArchiveBlob>>();
		for (const auto &blob : blobs) {
			const auto id = ArchiveSqlToken(header.account, ArchiveIndexKind::Blob, blob.id.toUtf8(), security);
			if (id.isEmpty()) { return {}; }
			ordered.emplace_back(id, blob);
		}
		std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
		for (const auto &entry : ordered) {
			const auto &id = entry.first;
			auto blob = entry.second;
			if ((!result.cursor.isEmpty() && id <= result.cursor) || !IsArchiveHash(blob.id) || id.isEmpty() || (complete && blob.state != ArchiveBlobState::GarbagePending)
				|| (!complete && blob.state != ArchiveBlobState::Complete && blob.state != ArchiveBlobState::GarbagePending)) { return {}; }
			if (complete) {
				result.program.statements.push_back({ u"DELETE FROM blobs WHERE blob_id=? AND state=1 "
					"AND NOT EXISTS(SELECT 1 FROM media_refs WHERE blob_id=?) AND NOT EXISTS(SELECT 1 FROM leases WHERE blob_id=?)"_q,
					{ id, id, id }, ArchiveSqlExpectation::OneChange });
			} else {
				const auto oldState = blob.state;
				blob.state = ArchiveBlobState::GarbagePending;
				const auto encrypted = SealArchiveSqlRow(header.account, id, blob, security);
				if (!encrypted) { return {}; }
				result.program.statements.push_back({ u"UPDATE blobs SET state=1,envelope=? WHERE blob_id=? AND bytes=? AND state=? "
					"AND NOT EXISTS(SELECT 1 FROM media_refs WHERE blob_id=?) AND NOT EXISTS(SELECT 1 FROM leases WHERE blob_id=?)"_q,
					{ *encrypted, id, QString::number(blob.size), int64(oldState), id, id }, ArchiveSqlExpectation::OneChange });
				result.deleteOwnedBlobs.push_back(blob.id);
			}
			result.cursor = id;
			if (!IsBoundedArchiveSqlProgram(result.program)) {
				result.program.status = ArchiveSqlStatus::TooLarge;
				FinishProgram(result.program);
				result.deleteOwnedBlobs.clear();
				return result;
			}
		}
		if (!AdvanceHeader(result.program, header, result.nextHeader, security)) { return {}; }
		result.program.statements.push_back({ u"COMMIT"_q });
		FinishProgram(result.program);
		return result;
	});
}

}

ArchiveSqlGcPlan PlanArchiveSqlGarbage(const ArchiveState &header,
		const std::vector<ArchiveBlob> &candidates, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlGcPlan {
		return Garbage(header, candidates, security, false);
	});
}

ArchiveSqlGcPlan CompleteArchiveSqlGarbage(const ArchiveState &header,
		const std::vector<ArchiveBlob> &removed, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlGcPlan {
		return Garbage(header, removed, security, true);
	});
}

}
