/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_chat_carry.h"

#include "purple/purple_sync_json.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace Purple {
namespace {

constexpr auto kSafeInteger = uint64_t(9007199254740991ULL);

[[nodiscard]] std::optional<uint64_t> Integer(const QJsonValue &value) {
	const auto n = value.toDouble(-1);
	return value.isDouble() && std::isfinite(n) && n >= 0
		&& n <= double(kSafeInteger) && std::floor(n) == n
		? std::optional(uint64_t(n)) : std::nullopt;
}

[[nodiscard]] QString AccountText(const AccountKey &account) {
	return AccountKeyText(account.environment, account.userId).value_or(QString());
}

[[nodiscard]] QString PeerText(const PeerToken &chat) {
	return PeerTokenText(chat).value_or(QString());
}

[[nodiscard]] std::optional<SyncCarryReference> ReadReference(const QJsonValue &value) {
	if (!value.isObject()) {
		return std::nullopt;
	}
	const auto o = value.toObject();
	const auto account = ParseAccountKey(o.value(u"account"_q).toString());
	const auto chat = ParsePeerToken(o.value(u"chat"_q).toString());
	const auto id = Integer(o.value(u"message_id"_q));
	const auto hash = o.value(u"hash"_q).toString();
	if (!account || !chat || !id || !*id
		|| *id > uint64_t(std::numeric_limits<int32_t>::max()) || !IsSyncCarryHash(hash)
		|| PeerTokenText(*chat, uint64_t(account->userId)) != PeerText(*chat)) {
		return std::nullopt;
	}
	return SyncCarryReference{ *account, *chat, int32_t(*id), hash };
}

[[nodiscard]] QJsonObject PreviousReference(
		const QJsonArray &array,
		const SyncCarryReference &reference,
		bool wrapped) {
	for (const auto &value : array) {
		const auto object = value.toObject();
		const auto parsed = ReadReference(wrapped
			? object.value(u"reference"_q) : value);
		if (parsed == reference) {
			return object;
		}
	}
	return {};
}

[[nodiscard]] QJsonObject ReferenceJson(
		const SyncCarryReference &reference,
		QJsonObject object = {}) {
	object.insert(u"account"_q, AccountText(reference.account));
	object.insert(u"chat"_q, PeerText(reference.chat));
	object.insert(u"message_id"_q, reference.messageId);
	object.insert(u"hash"_q, reference.recordHash);
	return object;
}

[[nodiscard]] QJsonObject HeaderJson(
		const SyncEnvelopeHeader &header,
		QJsonObject object) {
	object.insert(u"space"_q, header.space);
	object.insert(u"stream"_q, header.stream);
	object.insert(u"install"_q, header.writerInstall);
	object.insert(u"device"_q, header.writerDevice);
	object.insert(u"platform"_q, header.writerPlatform);
	object.insert(u"app"_q, header.writerApp);
	object.insert(u"seq"_q, QJsonValue(qint64(header.seq)));
	object.insert(u"at"_q, QJsonValue(qint64(header.at)));
	return object;
}

[[nodiscard]] std::optional<SyncEnvelopeHeader> ReadHeader(const QJsonValue &value) {
	if (!value.isObject()) {
		return std::nullopt;
	}
	const auto o = value.toObject();
	auto header = SyncEnvelopeHeader();
	header.space = o.value(u"space"_q).toString();
	header.stream = o.value(u"stream"_q).toString();
	header.writerInstall = o.value(u"install"_q).toString();
	header.writerDevice = o.value(u"device"_q).toString();
	header.writerPlatform = o.value(u"platform"_q).toString();
	header.writerApp = o.value(u"app"_q).toString();
	const auto seq = Integer(o.value(u"seq"_q));
	const auto at = Integer(o.value(u"at"_q));
	if (!IsSyncSpaceId(header.space) || !IsSyncInstallId(header.writerInstall)
		|| header.stream.isEmpty() || header.stream.size() > 64
		|| !seq || !*seq || !at) {
		return std::nullopt;
	}
	for (const auto &name : { u"device"_q, u"platform"_q, u"app"_q }) {
		if (!o.value(name).isString() || o.value(name).toString().toUtf8().size() > 256) {
			return std::nullopt;
		}
	}
	header.seq = *seq;
	header.at = *at;
	return header;
}

[[nodiscard]] QJsonArray SelectedJson(
		const std::vector<SyncCarrySelected> &records,
		const QJsonArray &previous) {
	auto array = QJsonArray();
	for (const auto &record : records) {
		auto o = PreviousReference(previous, record.reference, true);
		o.insert(u"reference"_q,
			ReferenceJson(record.reference, o.value(u"reference"_q).toObject()));
		o.insert(u"date"_q, QJsonValue(qint64(record.date)));
		o.insert(u"status"_q, int(record.status));
		o.insert(u"header"_q, record.header
			? QJsonValue(HeaderJson(*record.header, o.value(u"header"_q).toObject()))
			: QJsonValue(QJsonValue::Null));
		array.append(o);
	}
	return array;
}

[[nodiscard]] std::optional<std::vector<SyncCarrySelected>> ReadSelected(
		const QJsonValue &value,
		const AccountKey &account,
		const PeerToken &source) {
	if (!value.isArray()) {
		return std::nullopt;
	}
	auto result = std::vector<SyncCarrySelected>();
	auto ids = std::set<int32_t>();
	for (const auto &item : value.toArray()) {
		if (!item.isObject()) {
			return std::nullopt;
		}
		const auto o = item.toObject();
		const auto reference = ReadReference(o.value(u"reference"_q));
		const auto date = Integer(o.value(u"date"_q));
		const auto status = Integer(o.value(u"status"_q));
		if (!reference || reference->account != account || reference->chat != source
			|| !date || !*date || !status
			|| *status > uint64_t(SyncCandidateStatus::UnsupportedLibrary)
			|| !ids.insert(reference->messageId).second) {
			return std::nullopt;
		}
		auto record = SyncCarrySelected{ *reference, int64_t(*date), SyncCandidateStatus(*status), std::nullopt };
		if (record.status == SyncCandidateStatus::NewerMajor) {
			if (!o.value(u"header"_q).isNull()) {
				return std::nullopt;
			}
		} else {
			record.header = ReadHeader(o.value(u"header"_q));
			if (!record.header || record.header->stream == u"library"_q) {
				return std::nullopt;
			}
		}
		result.push_back(std::move(record));
	}
	return result;
}

[[nodiscard]] QJsonObject CacheJson(const SyncChatCache &cache, QJsonObject object) {
	object.insert(u"chat"_q, PeerText(cache.chat));
	object.insert(u"id"_q, cache.markId);
	object.insert(u"date"_q, QJsonValue(qint64(cache.markDate)));
	return object;
}

[[nodiscard]] std::optional<SyncChatCache> ReadCache(const QJsonValue &value) {
	if (!value.isObject()) {
		return std::nullopt;
	}
	const auto o = value.toObject();
	const auto chat = ParsePeerToken(o.value(u"chat"_q).toString());
	const auto id = Integer(o.value(u"id"_q));
	const auto date = Integer(o.value(u"date"_q));
	return chat && id && *id && *id <= uint64_t(std::numeric_limits<int32_t>::max())
		&& date && *date
		? std::optional(SyncChatCache{ *chat, int32_t(*id), int64_t(*date) })
		: std::nullopt;
}

}

QJsonObject SyncCarryPlanJson(const SyncCarryPlan &plan) {
	auto document = plan.preserved;
	document.insert(u"version"_q, 1);
	document.insert(u"account"_q, AccountText(plan.account));
	document.insert(u"source"_q, PeerText(plan.source));
	document.insert(u"target"_q, PeerText(plan.target));
	document.insert(u"mode"_q, int(plan.mode));
	document.insert(u"access"_q, int(plan.access));
	document.insert(u"source_stamp"_q, plan.sourceStamp);
	document.insert(u"state_hash"_q, plan.stateHash);
	document.insert(u"reserved_state_hash"_q, plan.reservedStateHash);
	document.insert(u"owner_install"_q, plan.ownerInstall);
	document.insert(u"owner_device"_q, plan.ownerDevice);
	document.insert(u"confirmed_at_commit"_q, QJsonValue(qint64(plan.confirmedAtCommit)));
	auto issued = QJsonArray();
	for (const auto &record : plan.issued) {
		auto o = record.preserved;
		o.insert(u"seq"_q, QJsonValue(qint64(record.seq)));
		o.insert(u"hash"_q, record.recordHash);
		issued.append(o);
	}
	document.insert(u"issued"_q, issued);
	document.insert(u"selected"_q, SelectedJson(plan.selected,
		document.value(u"selected"_q).toArray()));
	document.insert(u"cleanup_candidates"_q, SelectedJson(plan.cleanupCandidates,
		document.value(u"cleanup_candidates"_q).toArray()));
	auto forwards = QJsonArray();
	for (const auto &reference : plan.forwards) {
		forwards.append(ReferenceJson(reference, PreviousReference(
			document.value(u"forwards"_q).toArray(), reference, false)));
	}
	document.insert(u"forwards"_q, forwards);
	auto required = QJsonArray();
	for (const auto &hash : plan.requiredHashes) {
		required.append(hash);
	}
	document.insert(u"required"_q, required);
	document.insert(u"own_record"_q, QString::fromUtf8(plan.ownRecord));
	document.insert(u"own_record_hash"_q, plan.ownRecordHash);
	document.insert(u"winner"_q, plan.winner
		? QJsonValue(CacheJson(*plan.winner, document.value(u"winner"_q).toObject())) : QJsonValue(QJsonValue::Null));
	document.insert(u"cleanup"_q, int(plan.cleanup));
	document.insert(u"cleanup_authorized"_q, plan.cleanupAuthorized);
	document.insert(u"playlist_required"_q, plan.playlistRequired);
	return document;
}

std::optional<SyncCarryPlan> ParseSyncCarryPlan(const QJsonObject &document) {
	if (!CanonicalizeSyncJson(QJsonDocument(document).toJson())
		|| Integer(document.value(u"version"_q)) != 1) {
		return std::nullopt;
	}
	const auto account = ParseAccountKey(document.value(u"account"_q).toString());
	const auto source = ParsePeerToken(document.value(u"source"_q).toString());
	const auto target = ParsePeerToken(document.value(u"target"_q).toString());
	const auto mode = Integer(document.value(u"mode"_q));
	const auto access = Integer(document.value(u"access"_q));
	const auto cleanup = Integer(document.value(u"cleanup"_q));
	const auto confirmed = Integer(document.value(u"confirmed_at_commit"_q));
	if (!account || !source || !target || source == target || !mode
		|| *mode > uint64_t(SyncCarryMode::Fill) || !access || !*access
		|| *access > uint64_t(SyncCarrySourceAccess::ChatGone) || !cleanup
		|| *cleanup > uint64_t(SyncCarryCleanup::AllSettings) || !confirmed
		|| PeerTokenText(*source, uint64_t(account->userId)) != PeerText(*source)
		|| PeerTokenText(*target, uint64_t(account->userId)) != PeerText(*target)) {
		return std::nullopt;
	}
	auto plan = SyncCarryPlan();
	plan.account = *account;
	plan.source = *source;
	plan.target = *target;
	plan.mode = SyncCarryMode(*mode);
	plan.access = SyncCarrySourceAccess(*access);
	plan.cleanup = SyncCarryCleanup(*cleanup);
	plan.confirmedAtCommit = *confirmed;
	for (const auto &name : {
		u"source_stamp"_q, u"state_hash"_q, u"reserved_state_hash"_q, u"owner_install"_q,
		u"owner_device"_q, u"own_record"_q, u"own_record_hash"_q,
	}) {
		if (!document.value(name).isString()) {
			return std::nullopt;
		}
	}
	for (const auto &name : { u"cleanup_authorized"_q, u"playlist_required"_q }) {
		if (!document.value(name).isBool()) {
			return std::nullopt;
		}
	}
	plan.sourceStamp = document.value(u"source_stamp"_q).toString();
	plan.stateHash = document.value(u"state_hash"_q).toString();
	plan.reservedStateHash = document.value(u"reserved_state_hash"_q).toString();
	plan.ownerInstall = document.value(u"owner_install"_q).toString();
	plan.ownerDevice = document.value(u"owner_device"_q).toString();
	plan.ownRecord = document.value(u"own_record"_q).toString().toUtf8();
	plan.ownRecordHash = document.value(u"own_record_hash"_q).toString();
	plan.cleanupAuthorized = document.value(u"cleanup_authorized"_q).toBool();
	plan.playlistRequired = document.value(u"playlist_required"_q).toBool();
	if ((plan.mode == SyncCarryMode::Fill && plan.access != SyncCarrySourceAccess::Readable)
		|| (plan.access == SyncCarrySourceAccess::Readable
			? !IsSyncCarryHash(plan.sourceStamp) : !plan.sourceStamp.isEmpty())
		|| (!plan.stateHash.isEmpty() && !IsSyncCarryHash(plan.stateHash))
		|| (!plan.reservedStateHash.isEmpty() && !IsSyncCarryHash(plan.reservedStateHash))
		|| (plan.cleanup != SyncCarryCleanup::None && !plan.cleanupAuthorized)
		|| (plan.cleanup == SyncCarryCleanup::AllSettings && plan.mode != SyncCarryMode::Switch)
		|| (plan.cleanup == SyncCarryCleanup::OwnLatePosts && plan.mode != SyncCarryMode::Follow)) {
		return std::nullopt;
	}
	const auto selected = ReadSelected(document.value(u"selected"_q), plan.account, plan.source);
	const auto candidates = ReadSelected(document.value(u"cleanup_candidates"_q), plan.account, plan.source);
	if (!selected || !candidates || !document.value(u"forwards"_q).isArray()
		|| !document.value(u"required"_q).isArray()
		|| !document.value(u"issued"_q).isArray()) {
		return std::nullopt;
	}
	plan.selected = *selected;
	plan.cleanupCandidates = *candidates;
	auto required = std::set<QString>();
	for (const auto &value : document.value(u"required"_q).toArray()) {
		const auto hash = value.toString();
		if (!IsSyncCarryHash(hash) || !required.insert(hash).second) {
			return std::nullopt;
		}
		plan.requiredHashes.push_back(hash);
	}
	if (!std::is_sorted(plan.requiredHashes.begin(), plan.requiredHashes.end())) {
		return std::nullopt;
	}
	auto selectedHashes = std::set<QString>();
	for (const auto &record : plan.selected) {
		if (!required.contains(record.reference.recordHash)
			|| !selectedHashes.insert(record.reference.recordHash).second) {
			return std::nullopt;
		}
	}
	auto forwardHashes = std::set<QString>();
	for (const auto &value : document.value(u"forwards"_q).toArray()) {
		const auto reference = ReadReference(value);
		if (!reference || reference->account != plan.account || reference->chat != plan.source
			|| !selectedHashes.contains(reference->recordHash)
			|| std::none_of(plan.selected.begin(), plan.selected.end(), [&](const auto &record) {
				return record.reference == *reference;
			})
			|| !forwardHashes.insert(reference->recordHash).second) {
			return std::nullopt;
		}
		plan.forwards.push_back(*reference);
	}
	if (plan.access != SyncCarrySourceAccess::Readable
		&& (!plan.selected.empty() || !plan.forwards.empty() || !plan.cleanupCandidates.empty())) {
		return std::nullopt;
	}
	const auto winner = document.value(u"winner"_q);
	if (!winner.isNull()) {
		plan.winner = ReadCache(winner);
		if (!plan.winner || plan.winner->chat != plan.target) {
			return std::nullopt;
		}
	} else if (plan.mode != SyncCarryMode::Switch) {
		return std::nullopt;
	}
	auto previous = uint64_t(0);
	for (const auto &value : document.value(u"issued"_q).toArray()) {
		const auto o = value.toObject();
		const auto seq = Integer(o.value(u"seq"_q));
		const auto hash = o.value(u"hash"_q).toString();
		if (!value.isObject() || !seq || *seq <= previous || !IsSyncCarryHash(hash)
			|| plan.issued.size() == 32) {
			return std::nullopt;
		}
		plan.issued.push_back({ *seq, hash, o });
		previous = *seq;
	}
	if (plan.stateHash.isEmpty()) {
		if (!plan.ownerInstall.isEmpty() || !plan.ownerDevice.isEmpty()
			|| plan.confirmedAtCommit || !plan.issued.empty()
			|| !plan.reservedStateHash.isEmpty() || !plan.ownRecord.isEmpty()) {
			return std::nullopt;
		}
	} else if (!IsSyncInstallId(plan.ownerInstall) || plan.ownerDevice.isEmpty()) {
		return std::nullopt;
	}
	if (plan.ownRecord.isEmpty()) {
		if (!plan.ownRecordHash.isEmpty() || !plan.reservedStateHash.isEmpty()) {
			return std::nullopt;
		}
	} else {
		const auto parsed = ParseSyncEnvelope(plan.ownRecord);
		const auto written = parsed ? SerializeSyncEnvelope(parsed.envelope) : SyncEnvelopeWriteResult();
		if (!parsed || !parsed.header || !written || written.canonical != plan.ownRecord
			|| InspectConfigPayload(parsed).status != ConfigPayloadStatus::Valid
			|| parsed.header->stream != u"config"_q
			|| parsed.header->writerInstall != plan.ownerInstall
			|| parsed.header->writerDevice != plan.ownerDevice
			|| parsed.header->seq != plan.confirmedAtCommit
			|| plan.ownRecordHash != SyncCarryRecordHash(plan.ownRecord)
			|| !required.contains(plan.ownRecordHash) || plan.reservedStateHash.isEmpty()
			|| std::none_of(plan.issued.begin(), plan.issued.end(), [&](const auto &record) {
				return record.seq == parsed.header->seq && record.recordHash == plan.ownRecordHash;
			})) {
			return std::nullopt;
		}
	}
	plan.preserved = document;
	return plan;
}

QString SyncCarryPlanStamp(const SyncCarryPlan &plan) {
	const auto canonical = CanonicalizeSyncJson(QJsonDocument(SyncCarryPlanJson(plan)).toJson());
	return canonical ? SyncCarryRecordHash(canonical.canonical) : QString();
}

}
