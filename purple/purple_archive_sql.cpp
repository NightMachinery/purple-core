/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_archive_sql.h"

#include "purple/purple_archive_sql_internal.h"

#include <QtCore/QSet>
#include <QtCore/QCryptographicHash>

#include <algorithm>
#include <limits>

namespace Purple {
namespace {

bool InRange(int64 value, const ArchiveSqlCountRange &range) {
	return value >= range.minimum && (range.maximum < 0 || value <= range.maximum);
}

void Field(QByteArray &out, const QByteArray &value) {
	for (auto i = 3; i >= 0; --i) { out.append(char(uint64_t(value.size()) >> (8 * i))); }
	out.append(value);
}

QString LogicalMessage(const ArchiveMessageKey &key) {
	return QString::number(int(key.chat.kind)) + '/' + QString::number(key.chat.id) + '/'
		+ QString::number(int(key.collection)) + '/' + QString::number(key.collectionId)
		+ '/' + QString::number(key.messageId) + '/' + key.representation;
}

QString ChildToken(const QString &compound) {
	return compound.mid(compound.lastIndexOf('/') + 1);
}

ArchiveState Header(ArchiveState state) {
	state.messages.clear();
	state.blobs.clear();
	state.jobs.clear();
	state.leases.clear();
	state.chatGenerations.clear();
	return state;
}

size_t RowCount(const ArchiveState &state) {
	auto result = size_t(1) + state.messages.size() + state.blobs.size()
		+ state.jobs.size() + state.leases.size() + state.chatGenerations.size();
	for (const auto &message : state.messages) { result += message.versions.size() + message.receipts.size(); }
	return result;
}

class Builder {
public:
	Builder(const AccountKey &account, const ArchiveSqlSecurity &security,
			std::vector<ArchiveSqlVersionMetadata> metadata = {})
	: account(account), security(security), metadata(std::move(metadata)) {
		program.status = ArchiveSqlStatus::Ready;
		if (token(ArchiveIndexKind::Account, "account").isEmpty()) { program.status = ArchiveSqlStatus::Invalid; }
	}

	QString token(ArchiveIndexKind kind, const QByteArray &id) {
		const auto result = ArchiveSqlToken(account, kind, id, security);
		if (result.isEmpty()) { program.status = ArchiveSqlStatus::EncodingFailed; }
		return result;
	}

	QByteArray seal(const QString &id, ArchiveRowValue value) {
		if (!program) { return {}; }
		const auto bytes = SealArchiveSqlRow(account, id, std::move(value), security);
		if (!bytes) { program.status = ArchiveSqlStatus::EncodingFailed; }
		return bytes.value_or(QByteArray());
	}

	void add(ArchiveSqlStatement statement) {
		if (!program) { return; }
		program.statements.push_back(std::move(statement));
		if (!IsBoundedArchiveSqlProgram(program)) { program.status = ArchiveSqlStatus::TooLarge; program.statements.clear(); }
	}

	void message(const ArchiveMessage &message, const ArchiveMessage *before) {
		const auto id = ArchiveSqlMessageId(message.key, security);
		const auto chat = ArchiveSqlChatId(message.key.chat, account, security);
		if (id.isEmpty() || chat.isEmpty()) { program.status = ArchiveSqlStatus::EncodingFailed; return; }
		auto shell = message;
		shell.versions.clear();
		shell.receipts.clear();
		const auto date = message.versions.empty() ? message.deletedObservedAt
			: message.versions.front().observation.serverTime > 0 ? message.versions.front().observation.serverTime
			: message.versions.front().firstObservedAt;
		const auto head = message.versions.empty() ? QString()
			: ChildToken(ArchiveSqlChildId(message.key, ArchiveIndexKind::Version, message.versions.back().id, security));
		const auto topic = message.versions.empty() ? QString()
			: ArchiveSqlTopicId(message.key.chat, account, message.versions.back().observation.topicId, security);
		const auto representation = token(ArchiveIndexKind::Representation, message.key.representation.toUtf8());
		if ((!message.versions.empty() && topic.isEmpty()) || representation.isEmpty()) { program.status = ArchiveSqlStatus::EncodingFailed; return; }
		add({ u"INSERT INTO messages(message_key,chat_kind,chat_token,message_id,deleted,purged,transcript_date,head_version,causal_order,envelope,topic_token,collection,representation_token) "
			"VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(message_key) DO UPDATE SET deleted=excluded.deleted,purged=excluded.purged,"
			"head_version=excluded.head_version,causal_order=excluded.causal_order,envelope=excluded.envelope,"
			"topic_token=excluded.topic_token,collection=excluded.collection,representation_token=excluded.representation_token"_q,
			{ id, int64(message.key.chat.kind), chat, message.key.messageId, int64(message.deleted), int64(message.payloadPurged),
				date, head, ArchiveSqlOrder(message.order), seal(id, std::move(shell)), topic,
				int64(message.key.collection), representation }, ArchiveSqlExpectation::OneChange });
		for (const auto &version : message.versions) {
			const auto old = before ? std::find_if(before->versions.begin(), before->versions.end(),
				[&](const auto &entry) { return entry.id == version.id; }) : message.versions.end();
			if (before && old != before->versions.end() && *old == version) { continue; }
			const auto rowId = ArchiveSqlChildId(message.key, ArchiveIndexKind::Version, version.id, security);
			const auto versionId = ChildToken(rowId);
			const auto snapshot = ArchiveSqlVersionSnapshot(message.key, version, metadata);
			const auto prepared = PrepareArchiveSqlVersion(snapshot);
			const auto sealed = prepared ? SealArchiveSqlStoredRow(account, rowId, *prepared, security) : std::nullopt;
			if (!sealed) {
				program.status = ArchiveSqlStatus::RetentionGap;
				program.retentionGapReason = u"archive-version-limit"_q;
				return;
			}
			add({ u"INSERT INTO versions VALUES(?,?,?,?,?) ON CONFLICT(message_key,version_id) DO UPDATE SET "
				"order_key=excluded.order_key,envelope=excluded.envelope"_q,
				{ id, versionId, ArchiveSqlOrder(version.order), chat,
					*sealed }, ArchiveSqlExpectation::OneChange });
			const auto body = StoreArchiveSqlVersionBody(*prepared, snapshot, security);
			if (!body) { program.status = ArchiveSqlStatus::EncodingFailed; return; }
			for (const auto &statement : body.statements) { add(statement); }
			add({ u"DELETE FROM media_refs WHERE message_key=? AND version_id=?"_q,
				{ id, versionId }, ArchiveSqlExpectation::AnyChanges });
			for (auto i = size_t(0); i != version.media.size(); ++i) {
				const auto &ref = version.media[i];
				const auto descriptor = std::find_if(version.content.media.begin(), version.content.media.end(),
					[&](const auto &entry) { return entry.identity == ref.identity; });
				const auto media = token(ArchiveIndexKind::Media, (ref.identity + '/' + descriptor->kind).toUtf8());
				const auto blob = ref.blobId.isEmpty() ? ArchiveSqlValue(nullptr)
					: ArchiveSqlValue(token(ArchiveIndexKind::Blob, ref.blobId.toUtf8()));
				add({ u"INSERT INTO media_refs VALUES(?,?,?,?,?,?)"_q,
					{ id, versionId, media, int64(i), int64(ref.state), blob }, ArchiveSqlExpectation::OneChange });
			}
		}
		for (const auto &event : message.receipts) {
			if (before && std::find(before->receipts.begin(), before->receipts.end(), event) != before->receipts.end()) { continue; }
			const auto rowId = ArchiveSqlChildId(message.key, ArchiveIndexKind::Event, event.id, security);
			add({ u"INSERT INTO events(message_key,event_id,fingerprint,envelope) VALUES(?,?,?,?) "
				"ON CONFLICT(message_key,event_id) DO UPDATE SET event_id=excluded.event_id WHERE events.fingerprint=excluded.fingerprint"_q,
				{ id, ChildToken(rowId), token(ArchiveIndexKind::Event, (u"fingerprint/"_q + event.fingerprint).toUtf8()), seal(rowId, event) }, ArchiveSqlExpectation::OneChange });
		}
	}

	ArchiveSqlProgram finish() {
		if (program && !IsBoundedArchiveSqlProgram(program)) { program.status = ArchiveSqlStatus::TooLarge; }
		if (!program) { program.statements.clear(); }
		return std::move(program);
	}

	AccountKey account;
	const ArchiveSqlSecurity &security;
	ArchiveSqlProgram program;
	std::vector<ArchiveSqlVersionMetadata> metadata;
};

}

ArchiveSqlStatement::ArchiveSqlStatement(QString query, std::vector<ArchiveSqlValue> values,
		ArchiveSqlExpectation expected)
: sql(std::move(query)), binds(std::move(values)), expectation(expected) {
	switch (expected) {
	case ArchiveSqlExpectation::Done: break;
	case ArchiveSqlExpectation::OneRow: rows = { 1, 1 }; break;
	case ArchiveSqlExpectation::Rows: rows = { 0, kArchiveSqlPageLimit }; break;
	case ArchiveSqlExpectation::OneChange: changes = { 1, 1 }; counter = ArchiveSqlChangeCounter::Direct; break;
	case ArchiveSqlExpectation::AnyChanges: changes = { 0, -1 }; counter = ArchiveSqlChangeCounter::Direct; break;
	case ArchiveSqlExpectation::AtMostOneChange: changes = { 0, 1 }; counter = ArchiveSqlChangeCounter::Direct; break;
	}
}

bool CheckArchiveSqlResult(const ArchiveSqlStatement &statement, bool reachedDone,
		int64 rows, int64 directChanges, int64 totalChangesDelta) {
	return reachedDone && rows >= 0 && directChanges >= 0 && totalChangesDelta >= 0
		&& InRange(rows, statement.rows)
		&& InRange(statement.counter == ArchiveSqlChangeCounter::Direct ? directChanges : totalChangesDelta, statement.changes);
}

bool AcknowledgeArchiveSqlCommit(bool allResultsChecked, bool commitDurable) {
	return allResultsChecked && commitDurable;
}

QString ArchiveSqlToken(const AccountKey &account, ArchiveIndexKind kind,
		const QByteArray &identity, const ArchiveSqlSecurity &security) {
	const auto accountId = AccountKeyText(account.environment, account.userId);
	if (!accountId || !security.hmacSha256 || !IsArchiveToken(security.storeId)
		|| security.storeId.size() > 128 || identity.isEmpty()
		|| int(kind) < 0 || int(kind) > int(ArchiveIndexKind::Representation)) { return {}; }
	auto input = QByteArray("purple-archive-index-v1");
	Field(input, accountId->toUtf8());
	Field(input, security.storeId.toUtf8());
	Field(input, QByteArray::number(int(kind)));
	Field(input, identity);
	const auto mac = security.hmacSha256(input);
	return mac && mac->size() == 32 ? QString::fromLatin1(mac->toHex()) : QString();
}

QString ArchiveSqlMessageId(const ArchiveMessageKey &key, const ArchiveSqlSecurity &security) {
	return IsArchiveMessageKey(key) ? ArchiveSqlToken(key.account, ArchiveIndexKind::Message,
		LogicalMessage(key).toUtf8(), security) : QString();
}

QString ArchiveSqlChatId(const ArchiveChatKey &key, const AccountKey &account, const ArchiveSqlSecurity &security) {
	return IsArchiveChatKey(key) ? ArchiveSqlToken(account, ArchiveIndexKind::Chat,
		(QString::number(int(key.kind)) + '/' + QString::number(key.id)).toUtf8(), security) : QString();
}

QString ArchiveSqlTopicId(const ArchiveChatKey &chat, const AccountKey &account,
		int64 topicId, const ArchiveSqlSecurity &security) {
	if (topicId < 0) { return {}; }
	const auto parent = ArchiveSqlChatId(chat, account, security);
	return parent.isEmpty() ? QString() : ArchiveSqlToken(account, ArchiveIndexKind::Topic,
		(parent + '/' + QString::number(topicId)).toUtf8(), security);
}

QString ArchiveSqlChildId(const ArchiveMessageKey &key, ArchiveIndexKind kind,
		const QString &id, const ArchiveSqlSecurity &security) {
	const auto parent = ArchiveSqlMessageId(key, security);
	if (parent.isEmpty() || !IsArchiveToken(id)
		|| (kind != ArchiveIndexKind::Version && kind != ArchiveIndexKind::Event)) { return {}; }
	const auto child = ArchiveSqlToken(key.account, kind, (parent + '/' + id).toUtf8(), security);
	return child.isEmpty() ? QString() : parent + '/' + child;
}

QString ArchiveSqlOrder(uint64_t order) { return QString::number(order).rightJustified(20, '0'); }

ArchiveVersionSnapshot ArchiveSqlVersionSnapshot(const ArchiveMessageKey &key, const ArchiveVersion &version,
		const std::vector<ArchiveSqlVersionMetadata> &metadata) {
	const auto found = std::find_if(metadata.begin(), metadata.end(), [&](const auto &entry) {
		return entry.key == key && entry.versionId == version.id;
	});
	return { key, version, found == metadata.end() ? QJsonObject() : found->preserved };
}

std::optional<QByteArray> SealArchiveSqlRow(const AccountKey &account, const QString &id,
		ArchiveRowValue value, const ArchiveSqlSecurity &security) {
	return SealArchiveSqlStoredRow(account, id, ArchiveRow{ std::move(value) }, security);
}

std::vector<QString> ArchiveSqlConnectionSetup() {
	return { u"PRAGMA foreign_keys=ON"_q, u"PRAGMA journal_mode=WAL"_q, u"PRAGMA synchronous=FULL"_q };
}

namespace {

constexpr auto kArchiveSqlPreviousSchemaVersion = 2;

std::vector<QString> PreviousArchiveSqlSchema() {
	return {
		u"CREATE TABLE archive_meta(singleton INTEGER PRIMARY KEY CHECK(singleton=1),schema_version INTEGER NOT NULL,"
		"account_token TEXT NOT NULL,store_id TEXT NOT NULL,revision TEXT NOT NULL,generation TEXT NOT NULL,"
		"maintenance_id TEXT NOT NULL,envelope BLOB NOT NULL,schema_fingerprint TEXT NOT NULL)"_q,
		u"CREATE TABLE messages(position INTEGER PRIMARY KEY,message_key TEXT UNIQUE NOT NULL,chat_kind INTEGER NOT NULL,"
		"chat_token TEXT NOT NULL,message_id INTEGER NOT NULL,deleted INTEGER NOT NULL,purged INTEGER NOT NULL,"
		"transcript_date INTEGER NOT NULL,head_version TEXT NOT NULL,causal_order TEXT NOT NULL,envelope BLOB NOT NULL,"
		"topic_token TEXT NOT NULL DEFAULT '',collection INTEGER NOT NULL DEFAULT 0,representation_token TEXT NOT NULL DEFAULT '')"_q,
		u"CREATE INDEX deleted_transcript ON messages(chat_token,deleted,purged,transcript_date DESC,message_id DESC,message_key DESC)"_q,
		u"CREATE INDEX deleted_topic ON messages(chat_token,topic_token,deleted,purged,transcript_date,message_id,message_key)"_q,
		u"CREATE INDEX message_purge ON messages(chat_token,message_key)"_q,
		u"CREATE TABLE versions(message_key TEXT NOT NULL,version_id TEXT NOT NULL,order_key TEXT NOT NULL,"
		"chat_token TEXT NOT NULL,envelope BLOB NOT NULL,PRIMARY KEY(message_key,version_id),"
		"FOREIGN KEY(message_key) REFERENCES messages(message_key) ON DELETE CASCADE)"_q,
		u"CREATE TABLE body_refs(message_key TEXT NOT NULL,version_id TEXT NOT NULL,blob_id TEXT NOT NULL,bytes INTEGER NOT NULL,"
		"chunk_count INTEGER NOT NULL,PRIMARY KEY(message_key,version_id),"
		"FOREIGN KEY(message_key,version_id) REFERENCES versions(message_key,version_id) ON DELETE CASCADE)"_q,
		u"CREATE TABLE body_chunks(message_key TEXT NOT NULL,version_id TEXT NOT NULL,chunk_index INTEGER NOT NULL,envelope BLOB NOT NULL,"
		"PRIMARY KEY(message_key,version_id,chunk_index),FOREIGN KEY(message_key,version_id) REFERENCES body_refs(message_key,version_id) ON DELETE CASCADE)"_q,
		u"CREATE INDEX version_cursor ON versions((message_key||'/'||version_id))"_q,
		u"CREATE INDEX version_order ON versions(message_key,order_key DESC)"_q,
		u"CREATE INDEX version_chat ON versions(chat_token,message_key,version_id)"_q,
		u"CREATE TABLE events(position INTEGER PRIMARY KEY,message_key TEXT NOT NULL,event_id TEXT NOT NULL,fingerprint TEXT NOT NULL,envelope BLOB NOT NULL,"
		"UNIQUE(message_key,event_id),FOREIGN KEY(message_key) REFERENCES messages(message_key) ON DELETE CASCADE)"_q,
		u"CREATE INDEX event_cursor ON events((message_key||'/'||event_id))"_q,
		u"CREATE TABLE blobs(position INTEGER PRIMARY KEY,blob_id TEXT UNIQUE NOT NULL,bytes TEXT NOT NULL,state INTEGER NOT NULL,envelope BLOB NOT NULL)"_q,
		u"CREATE INDEX blob_state ON blobs(state,blob_id)"_q,
		u"CREATE TABLE media_refs(message_key TEXT NOT NULL,version_id TEXT NOT NULL,media_token TEXT NOT NULL,"
		"position INTEGER NOT NULL,state INTEGER NOT NULL,blob_id TEXT,PRIMARY KEY(message_key,version_id,media_token),"
		"FOREIGN KEY(message_key,version_id) REFERENCES versions(message_key,version_id) ON DELETE CASCADE,"
		"FOREIGN KEY(blob_id) REFERENCES blobs(blob_id) DEFERRABLE INITIALLY DEFERRED)"_q,
		u"CREATE INDEX blob_references ON media_refs(blob_id)"_q,
		u"CREATE INDEX media_copies ON media_refs(media_token,state,blob_id)"_q,
		u"CREATE TABLE jobs(position INTEGER PRIMARY KEY,job_id TEXT UNIQUE NOT NULL,message_key TEXT NOT NULL,version_id TEXT NOT NULL,"
		"chat_token TEXT NOT NULL,state INTEGER NOT NULL,generation TEXT NOT NULL,chat_generation TEXT NOT NULL,envelope BLOB NOT NULL)"_q,
		u"CREATE INDEX pending_jobs ON jobs(chat_token,state,job_id)"_q,
		u"CREATE TABLE leases(position INTEGER PRIMARY KEY,lease_id TEXT UNIQUE NOT NULL,blob_id TEXT NOT NULL,chat_token TEXT NOT NULL,"
		"revoked INTEGER NOT NULL,envelope BLOB NOT NULL,FOREIGN KEY(blob_id) REFERENCES blobs(blob_id) DEFERRABLE INITIALLY DEFERRED)"_q,
		u"CREATE INDEX blob_readers ON leases(blob_id)"_q,
		u"CREATE INDEX chat_readers ON leases(chat_token,revoked,lease_id)"_q,
		u"CREATE TABLE purge_epochs(position INTEGER PRIMARY KEY,chat_token TEXT UNIQUE NOT NULL,generation TEXT NOT NULL,envelope BLOB NOT NULL)"_q,
		u"CREATE TABLE maintenance(singleton INTEGER PRIMARY KEY CHECK(singleton=1),operation_id TEXT NOT NULL,kind INTEGER NOT NULL,"
		"scope_token TEXT NOT NULL,phase INTEGER NOT NULL,cursor TEXT NOT NULL,envelope BLOB NOT NULL)"_q,
		u"CREATE TABLE capture_gaps(gap_id TEXT PRIMARY KEY,observed_at INTEGER NOT NULL,reason TEXT NOT NULL,"
		"fingerprint TEXT NOT NULL,envelope BLOB NOT NULL)"_q,
		u"CREATE INDEX gap_order ON capture_gaps(observed_at,gap_id)"_q,
	};
}

std::vector<QString> ArchiveSqlObservationSchema() {
	return {
		u"CREATE TABLE observations(message_key TEXT NOT NULL,observation_id TEXT NOT NULL,"
		"order_key TEXT NOT NULL,chat_token TEXT NOT NULL,bytes INTEGER NOT NULL,envelope BLOB NOT NULL,"
		"PRIMARY KEY(message_key,observation_id),"
		"FOREIGN KEY(message_key) REFERENCES messages(message_key) ON DELETE CASCADE)"_q,
		u"CREATE TABLE observation_chunks(message_key TEXT NOT NULL,observation_id TEXT NOT NULL,"
		"chunk_index INTEGER NOT NULL,envelope BLOB NOT NULL,PRIMARY KEY(message_key,observation_id,chunk_index),"
		"FOREIGN KEY(message_key,observation_id) REFERENCES observations(message_key,observation_id) ON DELETE CASCADE)"_q,
		u"CREATE INDEX observation_order ON observations(message_key,order_key DESC,observation_id DESC)"_q,
		u"CREATE INDEX observation_chat ON observations(chat_token,message_key,observation_id)"_q,
	};
}

}

std::vector<QString> ArchiveSqlSchema() {
	auto schema = PreviousArchiveSqlSchema();
	const auto observations = ArchiveSqlObservationSchema();
	schema.insert(schema.end(), observations.begin(), observations.end());
	return schema;
}

ArchiveSqlStatus CheckArchiveSqlVersion(int64 version) {
	return version == kArchiveSqlSchemaVersion ? ArchiveSqlStatus::Ready
		: version > kArchiveSqlSchemaVersion ? ArchiveSqlStatus::ReadOnly
		: version > 0 ? ArchiveSqlStatus::SchemaMismatch : ArchiveSqlStatus::Invalid;
}

QString ArchiveSqlSchemaFingerprint(const std::vector<QString> &definitions) {
	auto normalized = std::vector<QString>();
	for (const auto &definition : definitions) { normalized.push_back(definition.trimmed()); }
	std::sort(normalized.begin(), normalized.end());
	auto bytes = QByteArray();
	for (const auto &definition : normalized) { Field(bytes, definition.toUtf8()); }
	return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString ArchiveSqlExpectedSchemaFingerprint() {
	static const auto fingerprint = ArchiveSqlSchemaFingerprint(ArchiveSqlSchema());
	return fingerprint;
}

ArchiveSqlStatement ArchiveSqlReadSchema() {
	const auto limit = int64(ArchiveSqlSchema().size()) + 1;
	auto result = ArchiveSqlStatement{
		u"SELECT sql FROM sqlite_schema WHERE sql IS NOT NULL AND name NOT GLOB 'sqlite_*' ORDER BY sql LIMIT ?"_q,
		{ limit }, ArchiveSqlExpectation::Rows };
	result.rows.maximum = limit;
	return result;
}

ArchiveSqlStatus CheckArchiveSqlSchema(int64 version, const QString &recordedFingerprint,
		const std::vector<QString> &definitions) {
	const auto status = CheckArchiveSqlVersion(version);
	if (status != ArchiveSqlStatus::Ready) { return status; }
	const auto expected = ArchiveSqlExpectedSchemaFingerprint();
	return recordedFingerprint == expected && definitions.size() == ArchiveSqlSchema().size()
		&& ArchiveSqlSchemaFingerprint(definitions) == expected
		? ArchiveSqlStatus::Ready : ArchiveSqlStatus::SchemaMismatch;
}

ArchiveSqlObservationUpgradePlan PlanArchiveSqlObservationUpgrade(
		const AccountKey &account,
		const QByteArray &headerEnvelope,
		const ArchiveSqlSecurity &security,
		const QString &maintenanceId,
		const QByteArray &maintenanceEnvelope) {
	const auto opened = OpenArchiveEnvelope(
		{ account, security.storeId, ArchiveRecordKind::Account, QByteArray("account") },
		security.keyId, headerEnvelope, security.crypto);
	if (!opened) {
		return {};
	}
	const auto decoded = DecodeArchiveRow(opened.bytes);
	const auto header = decoded.row
		? std::get_if<ArchiveState>(&decoded.row->value)
		: nullptr;
	if (decoded.status != ArchiveCodecStatus::Valid || !header
		|| !decoded.row->preserved.isEmpty() || decoded.row->body
		|| header->account != account || RowCount(*header) != 1
		|| !IsArchiveState(*header)) {
		return {};
	}
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &checked)
	-> ArchiveSqlObservationUpgradePlan {
		const auto previous = PreviousArchiveSqlSchema();
		const auto previousFingerprint = ArchiveSqlSchemaFingerprint(previous);
		const auto maintenance = ArchiveSqlLegacyMaintenanceGuard(
			*header, maintenanceId, maintenanceEnvelope, checked);
		if (maintenance.sql.isEmpty()) {
			return {};
		}
		auto builder = Builder(account, checked);
		auto preflight = std::vector<ArchiveSqlStatement>();
		preflight.push_back({ u"SELECT 1 FROM archive_meta WHERE singleton=1 AND schema_version=? AND schema_fingerprint=? "
			"AND account_token=? AND store_id=? AND revision=? AND generation=? AND maintenance_id=? AND envelope=?"_q,
			{ int64(kArchiveSqlPreviousSchemaVersion), previousFingerprint,
				builder.token(ArchiveIndexKind::Account, "account"), checked.storeId,
				QString::number(header->revision), QString::number(header->accountGeneration), maintenanceId, headerEnvelope },
			ArchiveSqlExpectation::OneRow });
		preflight.push_back(maintenance);
		preflight.push_back({ u"SELECT 1 WHERE (SELECT COUNT(*) FROM sqlite_schema WHERE sql IS NOT NULL "
			"AND name NOT GLOB 'sqlite_*')=?"_q,
			{ int64(previous.size()) }, ArchiveSqlExpectation::OneRow });
		for (const auto &definition : previous) {
			preflight.push_back({ u"SELECT 1 FROM sqlite_schema WHERE sql=?"_q,
				{ definition }, ArchiveSqlExpectation::OneRow });
		}
		builder.add({ u"BEGIN IMMEDIATE"_q });
		for (const auto &guard : preflight) {
			builder.add(guard);
		}
		for (const auto &definition : ArchiveSqlObservationSchema()) {
			builder.add({ definition });
		}
		builder.add({ u"UPDATE archive_meta SET schema_version=?,schema_fingerprint=? WHERE singleton=1"_q,
			{ int64(kArchiveSqlSchemaVersion), ArchiveSqlExpectedSchemaFingerprint() },
			ArchiveSqlExpectation::OneChange });
		builder.add({ u"COMMIT"_q });
		return { std::move(preflight), builder.finish() };
	});
}

QString ArchiveSqlSchemaGuard() {
	return u"schema_version="_q + QString::number(kArchiveSqlSchemaVersion)
		+ u" AND schema_fingerprint='"_q + ArchiveSqlExpectedSchemaFingerprint() + u"'"_q;
}

bool IsBoundedArchiveSqlProgram(const ArchiveSqlProgram &program) {
	if (program.statements.size() > kArchiveSqlStatementLimit) { return false; }
	auto bytes = uint64_t(0);
	for (const auto &statement : program.statements) {
		for (const auto &bind : statement.binds) {
			bytes += std::visit([](const auto &value) -> uint64_t {
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, QByteArray>) { return value.size(); }
				else if constexpr (std::is_same_v<T, QString>) { return value.toUtf8().size(); }
				else { return sizeof(value); }
			}, bind);
			if (bytes > kArchiveSqlBindByteLimit) { return false; }
		}
	}
	return true;
}

ArchiveSqlProgram InitializeArchiveSql(const ArchiveState &empty, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlProgram {
		if (RowCount(empty) != 1 || !IsArchiveState(empty)) { return {}; }
		auto builder = Builder(empty.account, security);
		builder.add({ u"BEGIN IMMEDIATE"_q });
		for (const auto &sql : ArchiveSqlSchema()) { builder.add({ sql }); }
		builder.add({ u"INSERT INTO archive_meta VALUES(1,?,?,?,?,?,'',?,?)"_q,
			{ int64(kArchiveSqlSchemaVersion), builder.token(ArchiveIndexKind::Account, "account"), security.storeId,
				QString::number(empty.revision), QString::number(empty.accountGeneration), builder.seal(u"account"_q, empty), ArchiveSqlExpectedSchemaFingerprint() },
			ArchiveSqlExpectation::OneChange });
		builder.add({ u"COMMIT"_q });
		return builder.finish();
	});
}

static ArchiveSqlProgram TranslateArchiveSqlMetadata(const ArchiveTransaction &transaction,
		const std::vector<ArchiveSqlVersionMetadata> &metadata, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlProgram {
		const auto &before = transaction.expected;
		const auto &next = transaction.next;
		if (before.version > kArchiveSchemaVersion || next.version > kArchiveSchemaVersion) { return { ArchiveSqlStatus::ReadOnly }; }
		if (before.messages.size() > 1 || next.messages.size() > 1
			|| RowCount(before) > kArchiveSqlSliceRowLimit || RowCount(next) > kArchiveSqlSliceRowLimit) {
			return { ArchiveSqlStatus::TooLarge };
		}
		if (CompleteArchiveTransaction(before, transaction, true).status != ArchiveCommitStatus::Applied
			|| before.revision == std::numeric_limits<uint64_t>::max() || next.revision != before.revision + 1
			|| before.accountGeneration != next.accountGeneration || before.chatGenerations != next.chatGenerations) { return {}; }
		for (const auto &message : before.messages) {
			const auto after = FindArchiveMessage(next, message.key);
			if (!after || message.payloadPurged != after->payloadPurged) { return {}; }
			for (const auto &version : message.versions) {
				if (!FindArchiveVersion(next, message.key, version.id)) { return {}; }
			}
		}
		auto builder = Builder(next.account, security, metadata);
		builder.add({ u"BEGIN IMMEDIATE"_q });
		builder.add({ u"SELECT 1 FROM archive_meta WHERE singleton=1 AND schema_version=? AND schema_fingerprint=? AND account_token=? "
			"AND store_id=? AND revision=? AND generation=? AND maintenance_id=''"_q,
			{ int64(kArchiveSqlSchemaVersion), ArchiveSqlExpectedSchemaFingerprint(), builder.token(ArchiveIndexKind::Account, "account"), security.storeId,
				QString::number(before.revision), QString::number(before.accountGeneration) }, ArchiveSqlExpectation::OneRow });
		for (const auto &message : next.messages) {
			const auto chat = ArchiveSqlChatId(message.key.chat, next.account, security);
			builder.add({ u"SELECT 1 WHERE COALESCE((SELECT generation FROM purge_epochs WHERE chat_token=?),'0')=?"_q,
				{ chat, QString::number(ArchiveGenerationFor(before, message.key.chat).chat) }, ArchiveSqlExpectation::OneRow });
			const auto old = FindArchiveMessage(before, message.key);
			const auto messageId = ArchiveSqlMessageId(message.key, security);
			if (old) {
				const auto head = old->versions.empty() ? QString()
					: ChildToken(ArchiveSqlChildId(old->key, ArchiveIndexKind::Version, old->versions.back().id, security));
				builder.add({ u"SELECT 1 FROM messages WHERE message_key=? AND causal_order=? AND head_version=?"_q,
					{ messageId, ArchiveSqlOrder(old->order), head }, ArchiveSqlExpectation::OneRow });
			} else {
				builder.add({ u"SELECT 1 WHERE NOT EXISTS(SELECT 1 FROM messages WHERE message_key=?)"_q,
					{ messageId }, ArchiveSqlExpectation::OneRow });
			}
			if (!old || *old != message) { builder.message(message, old); }
		}
		for (const auto &blob : next.blobs) {
			if (std::find(before.blobs.begin(), before.blobs.end(), blob) != before.blobs.end()) { continue; }
			const auto id = builder.token(ArchiveIndexKind::Blob, blob.id.toUtf8());
			const auto gate = blob.state == ArchiveBlobState::GarbagePending
				? u" WHERE NOT EXISTS(SELECT 1 FROM media_refs WHERE blob_id=excluded.blob_id) AND NOT EXISTS(SELECT 1 FROM leases WHERE blob_id=excluded.blob_id)"_q
				: u" WHERE blobs.state=0 AND blobs.bytes=excluded.bytes"_q;
			builder.add({ u"INSERT INTO blobs(blob_id,bytes,state,envelope) VALUES(?,?,?,?) ON CONFLICT(blob_id) DO UPDATE SET "
				"bytes=excluded.bytes,state=excluded.state,envelope=excluded.envelope"_q + gate,
				{ id, QString::number(blob.size), int64(blob.state), builder.seal(id, blob) }, ArchiveSqlExpectation::OneChange });
		}
		for (const auto &blob : before.blobs) {
			if (std::none_of(next.blobs.begin(), next.blobs.end(), [&](const auto &entry) { return entry.id == blob.id; })) {
				const auto id = builder.token(ArchiveIndexKind::Blob, blob.id.toUtf8());
				builder.add({ u"DELETE FROM blobs WHERE blob_id=? AND state=1 AND NOT EXISTS(SELECT 1 FROM media_refs WHERE blob_id=?) "
					"AND NOT EXISTS(SELECT 1 FROM leases WHERE blob_id=?)"_q, { id, id, id }, ArchiveSqlExpectation::OneChange });
			}
		}
		for (const auto &job : next.jobs) {
			if (std::find(before.jobs.begin(), before.jobs.end(), job) != before.jobs.end()) { continue; }
			const auto id = builder.token(ArchiveIndexKind::Job, job.id.toUtf8());
			builder.add({ u"INSERT INTO jobs(job_id,message_key,version_id,chat_token,state,generation,chat_generation,envelope) "
				"VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(job_id) DO UPDATE SET state=excluded.state,envelope=excluded.envelope"_q,
				{ id, ArchiveSqlMessageId(job.key, security), ChildToken(ArchiveSqlChildId(job.key, ArchiveIndexKind::Version, job.version, security)),
					ArchiveSqlChatId(job.key.chat, next.account, security), int64(job.state), QString::number(job.generation.account),
					QString::number(job.generation.chat), builder.seal(id, job) }, ArchiveSqlExpectation::OneChange });
		}
		for (const auto &lease : next.leases) {
			if (std::find(before.leases.begin(), before.leases.end(), lease) != before.leases.end()) { continue; }
			const auto id = builder.token(ArchiveIndexKind::Lease, lease.id.toUtf8());
			builder.add({ u"INSERT INTO leases(lease_id,blob_id,chat_token,revoked,envelope) VALUES(?,?,?,?,?) "
				"ON CONFLICT(lease_id) DO UPDATE SET revoked=excluded.revoked,envelope=excluded.envelope"_q,
				{ id, builder.token(ArchiveIndexKind::Blob, lease.blobId.toUtf8()), ArchiveSqlChatId(lease.key.chat, next.account, security),
					int64(lease.revoked), builder.seal(id, lease) }, ArchiveSqlExpectation::OneChange });
		}
		for (const auto &lease : before.leases) {
			if (std::none_of(next.leases.begin(), next.leases.end(), [&](const auto &entry) { return entry.id == lease.id; })) {
				builder.add({ u"DELETE FROM leases WHERE lease_id=?"_q,
					{ builder.token(ArchiveIndexKind::Lease, lease.id.toUtf8()) }, ArchiveSqlExpectation::OneChange });
			}
		}
		builder.add({ u"SELECT 1 WHERE NOT EXISTS(SELECT 1 FROM blobs b WHERE b.state=1 AND "
			"(EXISTS(SELECT 1 FROM media_refs r WHERE r.blob_id=b.blob_id) OR EXISTS(SELECT 1 FROM leases l WHERE l.blob_id=b.blob_id)))"_q,
			{}, ArchiveSqlExpectation::OneRow });
		builder.add({ u"UPDATE archive_meta SET revision=?,envelope=? WHERE singleton=1 AND revision=? AND maintenance_id=''"_q,
			{ QString::number(next.revision), builder.seal(u"account"_q, Header(next)), QString::number(before.revision) }, ArchiveSqlExpectation::OneChange });
		builder.add({ u"COMMIT"_q });
		return builder.finish();
	});
}

ArchiveSqlProgram TranslateArchiveSql(const ArchiveTransaction &transaction, const ArchiveSqlSecurity &security) {
	return TranslateArchiveSqlMetadata(transaction, {}, security);
}

ArchiveSqlProgram TranslateArchiveSql(const ArchiveTransaction &transaction, const ArchiveSqlSlice &restored,
		const ArchiveSqlSecurity &security) {
	if (transaction.expected != restored.state || restored.versionMetadata.size() > kArchiveSqlSliceRowLimit) { return {}; }
	for (auto i = size_t(0); i != restored.versionMetadata.size(); ++i) {
		const auto &entry = restored.versionMetadata[i];
		const auto version = FindArchiveVersion(restored.state, entry.key, entry.versionId);
		if (!version || !EncodeArchiveVersionSnapshot({ entry.key, *version, entry.preserved })) { return {}; }
		for (auto j = size_t(0); j != i; ++j) {
			if (restored.versionMetadata[j].key == entry.key && restored.versionMetadata[j].versionId == entry.versionId) { return {}; }
		}
	}
	return TranslateArchiveSqlMetadata(transaction, restored.versionMetadata, security);
}

ArchiveSqlStatement ArchiveSqlReadMessage(const ArchiveMessageKey &key, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlStatement {
		const auto id = ArchiveSqlMessageId(key, security);
		if (id.isEmpty()) { return {}; }
		auto result = ArchiveSqlStatement{ u"SELECT message_key,head_version,envelope FROM messages WHERE message_key=? "
			"AND EXISTS(SELECT 1 FROM archive_meta WHERE maintenance_id='')"_q, { id }, ArchiveSqlExpectation::Rows };
		result.rows.maximum = 1;
		return result;
	});
}

ArchiveSqlStatement ArchiveSqlReadReceipt(const ArchiveMessageKey &key, const QString &eventId,
		const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlStatement {
		const auto compound = ArchiveSqlChildId(key, ArchiveIndexKind::Event, eventId, security);
		if (compound.isEmpty()) { return {}; }
		auto result = ArchiveSqlStatement{ u"SELECT envelope FROM events WHERE message_key=? AND event_id=? "
			"AND EXISTS(SELECT 1 FROM archive_meta WHERE maintenance_id='')"_q,
			{ ArchiveSqlMessageId(key, security), ChildToken(compound) }, ArchiveSqlExpectation::Rows };
		result.rows.maximum = 1;
		return result;
	});
}

ArchiveSqlStatement ArchiveSqlReadRecord(const AccountKey &account, ArchiveRecordKind kind,
		const QString &logicalId, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlStatement {
		auto table = QString();
		auto column = QString();
		auto indexKind = ArchiveIndexKind::Blob;
		if (kind == ArchiveRecordKind::Blob && IsArchiveHash(logicalId)) { table = u"blobs"_q; column = u"blob_id"_q; }
		else if (kind == ArchiveRecordKind::Job && IsArchiveToken(logicalId)) { table = u"jobs"_q; column = u"job_id"_q; indexKind = ArchiveIndexKind::Job; }
		else if (kind == ArchiveRecordKind::Lease && IsArchiveToken(logicalId)) { table = u"leases"_q; column = u"lease_id"_q; indexKind = ArchiveIndexKind::Lease; }
		else { return {}; }
		const auto token = ArchiveSqlToken(account, indexKind, logicalId.toUtf8(), security);
		if (token.isEmpty()) { return {}; }
		auto result = ArchiveSqlStatement{ u"SELECT "_q + column + u",envelope FROM "_q + table + u" WHERE "_q + column
			+ u"=? AND EXISTS(SELECT 1 FROM archive_meta WHERE maintenance_id='')"_q, { token }, ArchiveSqlExpectation::Rows };
		result.rows.maximum = 1;
		return result;
	});
}

namespace {

QString ObservationToken(const ArchiveVersionSnapshot &snapshot, const ArchiveSqlSecurity &security) {
	const auto &version = snapshot.version;
	const auto message = ArchiveSqlMessageId(snapshot.key, security);
	const auto content = ArchiveContentHash(version.content);
	if (message.isEmpty() || content.isEmpty()) {
		return {};
	}
	auto identity = QByteArray("unordered-observation/v1/") + message.toUtf8() + '/' + content.toUtf8();
	identity += '/' + QByteArray::number(int(version.completeness));
	identity += '/' + QByteArray::number(version.observation.serverTime);
	identity += '/' + QByteArray::number(version.observation.context.value(u"editDate"_q).toInteger());
	if (version.source) {
		identity += '/' + QByteArray::number(int(version.source->kind)) + '/'
			+ QCryptographicHash::hash(version.source->bytes, QCryptographicHash::Sha256).toHex();
	}
	return ArchiveSqlToken(snapshot.key.account, ArchiveIndexKind::Event, identity, security);
}

ArchiveEnvelopeContext ObservationContext(const ArchiveMessageKey &key, const QString &message,
		const QString &token, const ArchiveSqlSecurity &security) {
	return { key.account, security.storeId, ArchiveRecordKind::UnorderedObservation,
		(message + '/' + token).toUtf8() };
}

ArchiveEnvelopeContext ObservationChunkContext(const ArchiveMessageKey &key, const QString &message,
		const QString &token, int64 bytes, int64 index, const ArchiveSqlSecurity &security) {
	return { key.account, security.storeId, ArchiveRecordKind::UnorderedObservationChunk,
		(message + '/' + token + '/' + QString::number(bytes) + '/' + QString::number(index)).toUtf8() };
}

} // namespace

ArchiveSqlObservationWrite PlanArchiveSqlUnorderedObservation(const ArchiveState &state,
		const ArchiveEvent &event, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security)
	-> ArchiveSqlObservationWrite {
		const auto owner = FindArchiveMessage(state, event.key);
		if (!IsArchiveState(state) || !owner || owner->deleted || owner->payloadPurged || owner->versions.empty()
			|| state.account != event.key.account || state.revision == std::numeric_limits<uint64_t>::max()
			|| event.generation != ArchiveGenerationFor(state, event.key.chat)
			|| event.key.collection != ArchiveCollection::Received
			|| event.origin != ArchiveOrigin::Baseline || !event.content || !event.order
			|| !event.targetVersion.isEmpty() || !event.targetRevision.isEmpty() || !event.expectedContentHash.isEmpty()
			|| event.lifecycleEpoch || !event.lifecycleMediaIdentity.isEmpty()
			|| !event.observation.payloadFormat.startsWith(u"telegram-message-tl:"_q)
			|| event.observation.context.contains(u"desktopLifecycle"_q)
			|| event.observation.context.value(u"desktopLifecycleFallback"_q).toBool()
			|| event.observation.context.value(u"desktopLivePreimage"_q).toBool()
			|| event.observation.context.value(u"desktopSendMetadata"_q).toBool()
			|| SameArchiveContent(owner->versions.back().content, *event.content)) {
			return {};
		}
		auto version = ArchiveVersion();
		version.id = event.id;
		version.revision = event.missing.revision.isEmpty() ? event.id : event.missing.revision;
		version.order = event.order;
		version.origin = event.origin;
		version.firstObservedAt = event.observation.observedAt;
		version.content = *event.content;
		version.missing = event.missing;
		version.missing.revision = version.revision;
		version.completeness = event.completeness;
		version.observation = event.observation;
		version.source = event.source;
		for (const auto &descriptor : version.content.media) {
			version.media.push_back({ descriptor.identity });
		}
		const auto snapshot = ArchiveVersionSnapshot{ event.key, std::move(version) };
		const auto bytes = EncodeArchiveVersionSnapshot(snapshot);
		const auto token = ObservationToken(snapshot, security);
		const auto message = ArchiveSqlMessageId(event.key, security);
		const auto chat = ArchiveSqlChatId(event.key.chat, event.key.account, security);
		const auto head = ChildToken(ArchiveSqlChildId(event.key, ArchiveIndexKind::Version, owner->versions.back().id, security));
		if (!bytes || bytes->isEmpty() || token.isEmpty() || message.isEmpty() || chat.isEmpty() || head.isEmpty()) {
			return {};
		}
		const auto header = SealArchiveEnvelope(ObservationContext(event.key, message, token, security),
			security.keyId, QCryptographicHash::hash(*bytes, QCryptographicHash::Sha256), security.crypto);
		if (!header) {
			return { {}, { ArchiveSqlStatus::EncodingFailed } };
		}
		auto out = Builder(state.account, security);
		out.add({ u"BEGIN IMMEDIATE"_q });
		out.add({ u"SELECT 1 FROM archive_meta WHERE singleton=1 AND schema_version=? AND schema_fingerprint=? AND account_token=? "
			"AND store_id=? AND revision=? AND generation=? AND maintenance_id=''"_q,
			{ int64(kArchiveSqlSchemaVersion), ArchiveSqlExpectedSchemaFingerprint(), out.token(ArchiveIndexKind::Account, "account"),
				security.storeId, QString::number(state.revision), QString::number(state.accountGeneration) }, ArchiveSqlExpectation::OneRow });
		out.add({ u"SELECT 1 WHERE COALESCE((SELECT generation FROM purge_epochs WHERE chat_token=?),'0')=?"_q,
			{ chat, QString::number(event.generation.chat) }, ArchiveSqlExpectation::OneRow });
		out.add({ u"SELECT 1 FROM messages WHERE message_key=? AND chat_token=? AND causal_order=? "
			"AND head_version=? AND deleted=0 AND purged=0"_q,
			{ message, chat, ArchiveSqlOrder(owner->order), head }, ArchiveSqlExpectation::OneRow });
		const auto size = int64(bytes->size());
		out.add({ u"INSERT INTO observations(message_key,observation_id,order_key,chat_token,bytes,envelope) VALUES(?,?,?,?,?,?)"_q,
			{ message, token, ArchiveSqlOrder(event.order), chat, size, header.bytes }, ArchiveSqlExpectation::OneChange });
		const auto count = (size + kArchiveMediaChunkBytes - 1) / kArchiveMediaChunkBytes;
		for (auto i = int64(0); i != count; ++i) {
			const auto chunk = SealArchiveEnvelope(ObservationChunkContext(event.key, message, token, size, i, security),
				security.keyId, bytes->mid(i * kArchiveMediaChunkBytes, kArchiveMediaChunkBytes), security.crypto);
			if (!chunk) {
				return { {}, { ArchiveSqlStatus::EncodingFailed } };
			}
			out.add({ u"INSERT INTO observation_chunks(message_key,observation_id,chunk_index,envelope) VALUES(?,?,?,?)"_q,
				{ message, token, i, chunk.bytes }, ArchiveSqlExpectation::OneChange });
		}
		auto next = Header(state);
		++next.revision;
		out.add({ u"UPDATE archive_meta SET revision=?,envelope=? WHERE singleton=1 AND revision=? AND maintenance_id=''"_q,
			{ QString::number(next.revision), out.seal(u"account"_q, next), QString::number(state.revision) }, ArchiveSqlExpectation::OneChange });
		out.add({ u"COMMIT"_q });
		return { token, out.finish() };
	});
}

ArchiveSqlStatement ArchiveSqlListUnorderedObservations(const ArchiveMessageKey &key,
		const QString &before, const ArchiveSqlSecurity &security) {
	const auto message = ArchiveSqlMessageId(key, security);
	if (message.isEmpty()) {
		return {};
	}
	auto result = ArchiveSqlStatement{ u"SELECT order_key,observation_id FROM observations WHERE message_key=?"_q,
		{ message }, ArchiveSqlExpectation::Rows };
	if (!before.isEmpty()) {
		auto valid = false;
		const auto order = before.left(20).toULongLong(&valid);
		const auto token = before.mid(21);
		if (before.size() != 85 || !valid || ArchiveSqlOrder(order) != before.left(20)
			|| before[20] != '/' || !IsArchiveHash(token)) {
			return {};
		}
		result.sql += u" AND (order_key,observation_id)<(?,?)"_q;
		result.binds.insert(result.binds.end(), { before.left(20), token });
	}
	result.sql += u" ORDER BY order_key DESC,observation_id DESC LIMIT 2"_q;
	result.rows.maximum = 2;
	return result;
}

ArchiveSqlStatement ArchiveSqlReadUnorderedObservation(const ArchiveMessageKey &key,
		const QString &token, const ArchiveSqlSecurity &security) {
	const auto message = ArchiveSqlMessageId(key, security);
	if (message.isEmpty() || !IsArchiveHash(token)) {
		return {};
	}
	auto result = ArchiveSqlStatement{ u"SELECT order_key,bytes,envelope FROM observations WHERE message_key=? "
		"AND observation_id=? AND EXISTS(SELECT 1 FROM archive_meta WHERE maintenance_id='')"_q,
		{ message, token }, ArchiveSqlExpectation::Rows };
	result.rows.maximum = 1;
	return result;
}

ArchiveSqlStatement ArchiveSqlReadUnorderedObservationChunks(const ArchiveMessageKey &key,
		const QString &token, int64 bytes, const ArchiveSqlSecurity &security) {
	const auto message = ArchiveSqlMessageId(key, security);
	if (message.isEmpty() || !IsArchiveHash(token) || bytes <= 0 || bytes > kArchiveSqlBodyByteLimit) {
		return {};
	}
	const auto count = (bytes + kArchiveMediaChunkBytes - 1) / kArchiveMediaChunkBytes;
	auto result = ArchiveSqlStatement{ u"SELECT chunk_index,envelope FROM observation_chunks WHERE message_key=? "
		"AND observation_id=? AND EXISTS(SELECT 1 FROM archive_meta WHERE maintenance_id='') ORDER BY chunk_index LIMIT ?"_q,
		{ message, token, count + 1 }, ArchiveSqlExpectation::Rows };
	result.rows = { count, count };
	return result;
}

std::optional<ArchiveVersionSnapshot> RestoreArchiveSqlUnorderedObservation(const ArchiveMessageKey &key,
		const QString &token, const QString &order, int64 size, const QByteArray &header,
		const std::vector<QByteArray> &chunks, const ArchiveSqlSecurity &security) {
	const auto message = ArchiveSqlMessageId(key, security);
	if (message.isEmpty() || !IsArchiveHash(token) || size <= 0 || size > kArchiveSqlBodyByteLimit
		|| chunks.size() != size_t((size + kArchiveMediaChunkBytes - 1) / kArchiveMediaChunkBytes)) {
		return {};
	}
	const auto expected = OpenArchiveEnvelope(ObservationContext(key, message, token, security),
		security.keyId, header, security.crypto);
	if (!expected || expected.bytes.size() != 32) {
		return {};
	}
	auto bytes = QByteArray();
	for (auto i = size_t(0); i != chunks.size(); ++i) {
		const auto chunk = OpenArchiveEnvelope(ObservationChunkContext(key, message, token, size, i, security),
			security.keyId, chunks[i], security.crypto);
		const auto expectedSize = ArchiveMediaChunkSize(size, i);
		if (!chunk || !expectedSize || chunk.bytes.size() != *expectedSize) {
			return {};
		}
		bytes += chunk.bytes;
	}
	if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != expected.bytes) {
		return {};
	}
	const auto decoded = DecodeArchiveVersionSnapshot(bytes);
	const auto &snapshot = decoded.snapshot;
	if (!snapshot || snapshot->key != key || snapshot->version.origin != ArchiveOrigin::Baseline
		|| ArchiveSqlOrder(snapshot->version.order) != order || ObservationToken(*snapshot, security) != token) {
		return {};
	}
	return snapshot;
}

ArchiveSqlStatement ArchiveSqlReadChatEpoch(const AccountKey &account, const ArchiveChatKey &chat,
		const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlStatement {
		const auto token = ArchiveSqlChatId(chat, account, security);
		if (token.isEmpty()) { return {}; }
		auto result = ArchiveSqlStatement{ u"SELECT generation,envelope FROM purge_epochs WHERE chat_token=?"_q,
			{ token }, ArchiveSqlExpectation::Rows };
		result.rows.maximum = 1;
		return result;
	});
}

ArchiveSqlStatement ArchiveSqlVersionJobs(const ArchiveMessageKey &key, const QString &version,
		int limit, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlStatement {
		const auto compound = ArchiveSqlChildId(key, ArchiveIndexKind::Version, version, security);
		if (compound.isEmpty() || limit < 1 || limit > kArchiveSqlPageLimit) { return {}; }
		auto result = ArchiveSqlStatement{ u"SELECT job_id,envelope FROM jobs WHERE message_key=? AND version_id=? AND state=0 "
			"AND EXISTS(SELECT 1 FROM archive_meta WHERE maintenance_id='') ORDER BY job_id LIMIT ?"_q,
			{ ArchiveSqlMessageId(key, security), ChildToken(compound), int64(limit) }, ArchiveSqlExpectation::Rows };
		result.rows.maximum = limit;
		return result;
	});
}

ArchiveSqlStatement ArchiveSqlReadVersion(const ArchiveMessageKey &key, const QString &version, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlStatement {
		const auto id = ArchiveSqlChildId(key, ArchiveIndexKind::Version, version, security);
		if (id.isEmpty()) { return {}; }
		auto result = ArchiveSqlStatement{ u"SELECT envelope FROM versions WHERE message_key=? AND version_id=? AND EXISTS(SELECT 1 FROM archive_meta WHERE maintenance_id='')"_q,
			{ ArchiveSqlMessageId(key, security), ChildToken(id) }, ArchiveSqlExpectation::Rows };
		result.rows.maximum = 1;
		return result;
	});
}

ArchiveSqlStatement ArchiveSqlReadHead(const ArchiveMessageKey &key, const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlStatement {
		const auto id = ArchiveSqlMessageId(key, security);
		if (id.isEmpty()) { return {}; }
		auto result = ArchiveSqlStatement{ u"SELECT v.envelope FROM messages m JOIN versions v ON v.message_key=m.message_key AND v.version_id=m.head_version WHERE m.message_key=? AND EXISTS(SELECT 1 FROM archive_meta WHERE maintenance_id='')"_q,
			{ id }, ArchiveSqlExpectation::Rows };
		result.rows.maximum = 1;
		return result;
	});
}

ArchiveSqlStatement ArchiveSqlReadVersions(const ArchiveMessageKey &key, int limit,
		const ArchiveSqlSecurity &security, const QString &beforeOrder) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlStatement {
		const auto id = ArchiveSqlMessageId(key, security);
		if (id.isEmpty() || limit < 1 || limit > kArchiveSqlPageLimit
			|| (!beforeOrder.isEmpty() && (beforeOrder.size() != 20
				|| std::any_of(beforeOrder.begin(), beforeOrder.end(), [](auto ch) { return ch < '0' || ch > '9'; })))) { return {}; }
		auto statement = ArchiveSqlStatement{ u"SELECT version_id,order_key,envelope FROM versions WHERE message_key=? AND EXISTS(SELECT 1 FROM archive_meta WHERE maintenance_id='') AND order_key<? ORDER BY order_key DESC LIMIT ?"_q,
			{ id, beforeOrder.isEmpty() ? QString(21, '9') : beforeOrder, int64(limit) }, ArchiveSqlExpectation::Rows };
		statement.rows.maximum = limit;
		return statement;
	});
}

ArchiveSqlStatement ArchiveSqlDeletedTranscript(const AccountKey &account, const ArchiveChatKey &chat,
		int limit, const ArchiveSqlSecurity &security, const std::optional<ArchiveTranscriptCursor> &before) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> ArchiveSqlStatement {
		const auto token = ArchiveSqlChatId(chat, account, security);
		if (token.isEmpty() || limit < 1 || limit > kArchiveSqlPageLimit || (before && (before->date < 0 || !IsArchiveHash(before->messageKey)))) { return {}; }
		auto statement = ArchiveSqlStatement{ u"SELECT message_key,transcript_date,message_id,envelope FROM messages WHERE chat_token=? AND deleted=1 AND purged=0 AND EXISTS(SELECT 1 FROM archive_meta WHERE maintenance_id='')"_q,
			{ token }, ArchiveSqlExpectation::Rows };
		if (before) {
			statement.sql += u" AND (transcript_date,message_id,message_key)<(?,?,?)"_q;
			statement.binds.insert(statement.binds.end(), { before->date, before->messageId, before->messageKey });
		}
		statement.sql += u" ORDER BY transcript_date DESC,message_id DESC,message_key DESC LIMIT ?"_q;
		statement.binds.push_back(int64(limit));
		statement.rows.maximum = limit;
		return statement;
	});
}

ArchiveSqlStatement ArchiveSqlReadPage(ArchiveRecordKind kind, int limit, const QString &after) {
	if (limit < 1 || limit > kArchiveSqlPageLimit || after.size() > 129) { return {}; }
	auto table = QString();
	auto id = QString();
	auto position = u"position"_q;
	switch (kind) {
	case ArchiveRecordKind::Account: return { u"SELECT 'account',0,envelope FROM archive_meta WHERE singleton=1"_q, {}, ArchiveSqlExpectation::OneRow };
	case ArchiveRecordKind::Message: table = u"messages"_q; id = u"message_key"_q; break;
	case ArchiveRecordKind::Version: table = u"versions"_q; id = u"message_key||'/'||version_id"_q; position = u"0"_q; break;
	case ArchiveRecordKind::Event: table = u"events"_q; id = u"message_key||'/'||event_id"_q; break;
	case ArchiveRecordKind::Blob: table = u"blobs"_q; id = u"blob_id"_q; break;
	case ArchiveRecordKind::Job: table = u"jobs"_q; id = u"job_id"_q; break;
	case ArchiveRecordKind::Lease: table = u"leases"_q; id = u"lease_id"_q; break;
	case ArchiveRecordKind::Generation: table = u"purge_epochs"_q; id = u"chat_token"_q; break;
	default: return {};
	}
	auto result = ArchiveSqlStatement{ u"SELECT "_q + id + ',' + position + u",envelope FROM "_q + table
		+ u" WHERE EXISTS(SELECT 1 FROM archive_meta WHERE maintenance_id='') AND ("_q + id + u")>? ORDER BY "_q + id + u" LIMIT ?"_q,
		{ after, int64(limit) }, ArchiveSqlExpectation::Rows };
	result.rows.maximum = limit;
	return result;
}

std::optional<ArchiveSqlSlice> RestoreArchiveSqlSlice(std::vector<ArchiveStoredRow> rows,
		const ArchiveSqlSecurity &security) {
	return CheckArchiveSqlDerivations(security, [&](const ArchiveSqlSecurity &security) -> std::optional<ArchiveSqlSlice> {
		if (rows.size() > kArchiveSqlSliceRowLimit) { return {}; }
		std::stable_sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.position < b.position; });
		auto result = ArchiveSqlSlice();
		auto &state = result.state;
		auto headerFound = false;
		auto identities = QSet<QString>();
		for (const auto &stored : rows) {
			const auto kind = ArchiveRowKind(stored.row);
			const auto identity = QString::number(int(kind)) + '/' + stored.id;
			if (stored.position < 0 || stored.row.body || !stored.row.preserved.isEmpty() || identities.contains(identity)) { return {}; }
			identities.insert(identity);
			if (kind == ArchiveRecordKind::Account) {
				if (headerFound || stored.id != u"account"_q) { return {}; }
				state = std::get<ArchiveState>(stored.row.value);
				if (!state.messages.empty() || !state.blobs.empty() || !state.jobs.empty() || !state.leases.empty() || !state.chatGenerations.empty()) { return {}; }
				headerFound = true;
			}
		}
		if (!headerFound) { return {}; }
		for (const auto &stored : rows) {
			const auto token = [&](ArchiveIndexKind kind, const QString &id) {
				return ArchiveSqlToken(state.account, kind, id.toUtf8(), security);
			};
			switch (ArchiveRowKind(stored.row)) {
			case ArchiveRecordKind::Message: {
				const auto &message = std::get<ArchiveMessage>(stored.row.value);
				if (ArchiveSqlMessageId(message.key, security) != stored.id || !message.versions.empty() || !message.receipts.empty()) { return {}; }
				state.messages.push_back(message);
			} break;
			case ArchiveRecordKind::Blob: {
				const auto &blob = std::get<ArchiveBlob>(stored.row.value);
				if (token(ArchiveIndexKind::Blob, blob.id) != stored.id) { return {}; }
				state.blobs.push_back(blob);
			} break;
			case ArchiveRecordKind::Job: {
				const auto &job = std::get<ArchiveMediaJob>(stored.row.value);
				if (token(ArchiveIndexKind::Job, job.id) != stored.id) { return {}; }
				state.jobs.push_back(job);
			} break;
			case ArchiveRecordKind::Lease: {
				const auto &lease = std::get<ArchiveReaderLease>(stored.row.value);
				if (token(ArchiveIndexKind::Lease, lease.id) != stored.id) { return {}; }
				state.leases.push_back(lease);
			} break;
			case ArchiveRecordKind::Generation: {
				const auto &epoch = std::get<ArchiveChatGeneration>(stored.row.value);
				if (ArchiveSqlChatId(epoch.chat, state.account, security) != stored.id) { return {}; }
				state.chatGenerations.push_back(epoch);
			} break;
			default: break;
			}
		}
		for (const auto &stored : rows) {
			const auto kind = ArchiveRowKind(stored.row);
			if (kind != ArchiveRecordKind::Version && kind != ArchiveRecordKind::Event) { continue; }
			const auto slash = stored.id.lastIndexOf('/');
			if (slash < 0) { return {}; }
			const auto message = std::find_if(state.messages.begin(), state.messages.end(), [&](const auto &entry) {
				return ArchiveSqlMessageId(entry.key, security) == stored.id.left(slash);
			});
			if (message == state.messages.end()) { return {}; }
			if (kind == ArchiveRecordKind::Version) {
				const auto &snapshot = std::get<ArchiveVersionSnapshot>(stored.row.value);
				if (snapshot.key != message->key
					|| ArchiveSqlChildId(snapshot.key, ArchiveIndexKind::Version, snapshot.version.id, security) != stored.id) { return {}; }
				message->versions.push_back(snapshot.version);
				if (!snapshot.preserved.isEmpty()) {
					result.versionMetadata.push_back({ snapshot.key, snapshot.version.id, snapshot.preserved });
				}
			} else {
				const auto &event = std::get<ArchiveEventReceipt>(stored.row.value);
				if (ArchiveSqlChildId(message->key, ArchiveIndexKind::Event, event.id, security) != stored.id) { return {}; }
				message->receipts.push_back(event);
			}
		}
		for (auto &message : state.messages) {
			std::sort(message.versions.begin(), message.versions.end(), [](const auto &a, const auto &b) { return a.order < b.order; });
		}
		return IsArchiveState(state) ? std::optional(std::move(result)) : std::nullopt;
	});
}

}
