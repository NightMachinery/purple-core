/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_chat_journal.h"

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

[[nodiscard]] bool RequestId(const QString &text) {
	if (text.isEmpty() || text[0] < u'1' || text[0] > u'9') {
		return false;
	}
	for (const auto ch : text) {
		if (ch < u'0' || ch > u'9') {
			return false;
		}
	}
	auto ok = false;
	const auto id = text.toULongLong(&ok);
	return ok && id != 0;
}

[[nodiscard]] std::optional<PeerToken> Normalize(
		const PeerToken &chat,
		const AccountKey &account) {
	const auto text = AccountKeyText(account.environment, account.userId)
		? PeerTokenText(chat, uint64_t(account.userId)) : std::nullopt;
	return text ? ParsePeerToken(*text) : std::nullopt;
}

[[nodiscard]] bool CacheValid(const SyncChatCache &cache, bool implicit) {
	return PeerTokenText(cache.chat).has_value() && cache.markId >= 0 && cache.markDate >= 0
		&& uint64_t(cache.markDate) <= kSafeInteger
		&& ((cache.markId > 0 && cache.markDate > 0)
			|| (implicit && cache.markId == 0 && cache.markDate == 0));
}

[[nodiscard]] QJsonValue CacheJson(
		const std::optional<SyncChatCache> &cache,
		QJsonObject object) {
	if (!cache) {
		return QJsonValue(QJsonValue::Null);
	}
	object.insert(u"chat"_q, PeerTokenText(cache->chat).value_or(QString()));
	object.insert(u"id"_q, cache->markId);
	object.insert(u"date"_q, QJsonValue(qint64(cache->markDate)));
	return object;
}

[[nodiscard]] std::optional<SyncChatCache> ReadCache(const QJsonValue &value) {
	const auto o = value.toObject();
	const auto peer = ParsePeerToken(o.value(u"chat"_q).toString());
	const auto id = Integer(o.value(u"id"_q));
	const auto date = Integer(o.value(u"date"_q));
	return value.isObject() && peer && id && date
		&& *id <= uint64_t(std::numeric_limits<int32_t>::max())
		? std::optional(SyncChatCache{ *peer, int32_t(*id), int64_t(*date) })
		: std::nullopt;
}

[[nodiscard]] std::optional<QString> StateHash(
		const std::optional<SyncLocalState> &state,
		bool absent) {
	if (!state) {
		return absent ? std::optional(QString()) : std::nullopt;
	}
	const auto serialized = SerializeSyncLocalState(*state);
	return serialized ? std::optional(SyncCarryRecordHash(serialized.canonical)) : std::nullopt;
}

[[nodiscard]] bool SwitchMarkOutranksKnown(
		const SyncCarryJournal &j,
		const SyncChatCache &mark) {
	for (const auto &known : { j.oldMark, j.expectedWinner }) {
		if (known && !SyncChatMarkOutranks(mark, *known)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool Valid(const SyncCarryJournal &j) {
	if (j.version != 1 || !IsSyncBindingToken(j.operationId.toLatin1())
		|| Normalize(j.source, j.account) != j.source
		|| Normalize(j.target, j.account) != j.target || j.source == j.target
		|| j.mode < SyncCarryMode::Switch || j.mode > SyncCarryMode::Fill
		|| j.phase < SyncCarryPhase::ResolvePending || j.phase > SyncCarryPhase::Aborted
		|| j.revision > kSafeInteger || j.pendingSeq > kSafeInteger
		|| j.lastOutcome < SyncCarryOutcome::Success || j.lastOutcome > SyncCarryOutcome::Unknown
		|| !RequestId(j.ownRequestId)
		|| (j.mode == SyncCarryMode::Switch
			&& (!RequestId(j.markRequestId) || j.markText.size() > 4096
				|| ParseSyncChatMark(j.markText, false, SyncChatMessageOwnership::Own).status
					!= SyncChatMarkStatus::Supported))) {
		return false;
	}
	for (const auto &hash : { j.stateHash, j.pendingStateHash, j.pendingStageHash, j.nextStateHash }) {
		if (!hash.isEmpty() && !IsSyncCarryHash(hash)) {
			return false;
		}
	}
	for (const auto &cache : { j.oldMark, j.expectedWinner, j.targetMark }) {
		if (cache && (!CacheValid(*cache, true)
			|| Normalize(cache->chat, j.account) != cache->chat)) {
			return false;
		}
	}
	if ((j.oldMark && j.oldMark->chat != j.source)
		|| (j.targetMark && (!CacheValid(*j.targetMark, false) || j.targetMark->chat != j.target))
		|| (j.mode != SyncCarryMode::Switch && !j.targetMark)
		|| (j.mode == SyncCarryMode::Switch && j.targetMark
			&& !SwitchMarkOutranksKnown(j, *j.targetMark))
		|| (j.mode == SyncCarryMode::Follow && j.expectedAction != SyncChatAction::Follow)
		|| (j.mode == SyncCarryMode::Fill && j.expectedAction != SyncChatAction::Fill)
		|| (j.mode == SyncCarryMode::Switch && j.expectedAction != SyncChatAction::Use
			&& j.expectedAction != SyncChatAction::Searching && j.expectedAction != SyncChatAction::Lost)) {
		return false;
	}
	if (j.plan) {
		if (!ParseSyncCarryPlan(SyncCarryPlanJson(*j.plan)) || j.plan->account != j.account
			|| j.plan->source != j.source || j.plan->target != j.target
			|| j.plan->mode != j.mode || j.plan->stateHash != j.stateHash
			|| (j.mode != SyncCarryMode::Switch && j.plan->winner != j.targetMark)) {
			return false;
		}
	} else if (j.phase >= SyncCarryPhase::StageOwn && j.phase <= SyncCarryPhase::Done) {
		return false;
	}
	if ((j.phase == SyncCarryPhase::ResolvePending && (!j.pendingSeq || j.stateHash.isEmpty()))
		|| ((j.phase == SyncCarryPhase::WritePendingState || j.phase == SyncCarryPhase::RetirePendingStage)
			&& (!j.pendingSeq || j.pendingStateHash.isEmpty() || j.pendingStageHash.isEmpty()))
		|| (j.phase == SyncCarryPhase::RetirePendingStage && j.stateHash != j.pendingStateHash)
		|| ((j.phase == SyncCarryPhase::StageOwn || j.phase == SyncCarryPhase::RetireOwnStage)
			&& (!j.plan || j.plan->ownRecord.isEmpty()))
		|| (j.markVerified && (!j.targetMark || !j.destinationVerified))
		|| (j.cachePrepared && (!j.markVerified || !j.commitPrepared))
		|| (j.stateCommitted && !j.cachePrepared)
		|| (j.cacheCompleted && !j.stateCommitted)
		|| (j.destinationVerified && !j.commitPrepared)
		|| (j.commitPrepared && (j.stateHash.isEmpty() != j.nextStateHash.isEmpty()))
		|| (j.phase == SyncCarryPhase::Mark && (!j.destinationVerified || !j.commitPrepared))
		|| (j.phase == SyncCarryPhase::PrepareCache && !j.markVerified)
		|| (j.phase == SyncCarryPhase::CommitState && !j.cachePrepared)
		|| ((j.phase == SyncCarryPhase::RetireOwnStage || j.phase == SyncCarryPhase::CompleteCache)
			&& !j.stateCommitted)
		|| ((j.phase == SyncCarryPhase::Cleanup || j.phase == SyncCarryPhase::Done) && !j.cacheCompleted)
		|| (j.phase == SyncCarryPhase::Aborted && (j.markAttempted || j.stateCommitted
			|| j.pendingSeq || (j.plan && !j.plan->ownRecord.isEmpty())))) {
		return false;
	}
	if (!std::is_sorted(j.missingHashes.begin(), j.missingHashes.end())
		|| std::adjacent_find(j.missingHashes.begin(), j.missingHashes.end()) != j.missingHashes.end()
		|| (j.destinationVerified && !j.missingHashes.empty())) {
		return false;
	}
	for (const auto &hash : j.missingHashes) {
		if (!IsSyncCarryHash(hash) || !j.plan
			|| !std::binary_search(j.plan->requiredHashes.begin(), j.plan->requiredHashes.end(), hash)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] QJsonObject Json(const SyncCarryJournal &j) {
	auto o = j.preserved;
	o.insert(u"version"_q, j.version);
	o.insert(u"operation"_q, j.operationId);
	o.insert(u"account"_q, AccountKeyText(j.account.environment, j.account.userId).value_or(QString()));
	o.insert(u"source"_q, PeerTokenText(j.source).value_or(QString()));
	o.insert(u"target"_q, PeerTokenText(j.target).value_or(QString()));
	o.insert(u"mode"_q, int(j.mode));
	o.insert(u"phase"_q, int(j.phase));
	o.insert(u"revision"_q, QJsonValue(qint64(j.revision)));
	o.insert(u"state_hash"_q, j.stateHash);
	o.insert(u"pending_state_hash"_q, j.pendingStateHash);
	o.insert(u"pending_stage_hash"_q, j.pendingStageHash);
	o.insert(u"pending_seq"_q, QJsonValue(qint64(j.pendingSeq)));
	o.insert(u"expected_action"_q, int(j.expectedAction));
	o.insert(u"expected_winner"_q, CacheJson(j.expectedWinner, o.value(u"expected_winner"_q).toObject()));
	o.insert(u"old_mark"_q, CacheJson(j.oldMark, o.value(u"old_mark"_q).toObject()));
	o.insert(u"target_mark"_q, CacheJson(j.targetMark, o.value(u"target_mark"_q).toObject()));
	o.insert(u"mark_text"_q, j.markText);
	o.insert(u"mark_request"_q, j.markRequestId);
	o.insert(u"own_request"_q, j.ownRequestId);
	o.insert(u"mark_attempted"_q, j.markAttempted);
	o.insert(u"remove_old_mark_authorized"_q, j.removeOldMarkAuthorized);
	o.insert(u"plan"_q, j.plan ? QJsonValue(SyncCarryPlanJson(*j.plan)) : QJsonValue(QJsonValue::Null));
	auto hashes = QJsonArray();
	for (const auto &hash : j.missingHashes) {
		hashes.append(hash);
	}
	o.insert(u"missing"_q, hashes);
	o.insert(u"next_state_hash"_q, j.nextStateHash);
	o.insert(u"commit_prepared"_q, j.commitPrepared);
	o.insert(u"destination_verified"_q, j.destinationVerified);
	o.insert(u"mark_verified"_q, j.markVerified);
	o.insert(u"cache_prepared"_q, j.cachePrepared);
	o.insert(u"state_committed"_q, j.stateCommitted);
	o.insert(u"cache_completed"_q, j.cacheCompleted);
	o.insert(u"last_outcome"_q, int(j.lastOutcome));
	return o;
}

[[nodiscard]] std::optional<SyncCarryJournal> Read(const QJsonObject &o) {
	const auto account = ParseAccountKey(o.value(u"account"_q).toString());
	const auto source = ParsePeerToken(o.value(u"source"_q).toString());
	const auto target = ParsePeerToken(o.value(u"target"_q).toString());
	if (!account || !source || !target) {
		return std::nullopt;
	}
	auto j = SyncCarryJournal();
	j.account = *account;
	j.source = *source;
	j.target = *target;
	const auto mode = Integer(o.value(u"mode"_q));
	const auto phase = Integer(o.value(u"phase"_q));
	const auto revision = Integer(o.value(u"revision"_q));
	const auto seq = Integer(o.value(u"pending_seq"_q));
	const auto action = Integer(o.value(u"expected_action"_q));
	const auto outcome = Integer(o.value(u"last_outcome"_q));
	if (!mode || *mode > uint64_t(SyncCarryMode::Fill)
		|| !phase || *phase > uint64_t(SyncCarryPhase::Aborted) || !revision || !seq
		|| !action || *action > uint64_t(SyncChatAction::ReadWinnerInventory)
		|| !outcome || *outcome > uint64_t(SyncCarryOutcome::Unknown)) {
		return std::nullopt;
	}
	j.mode = SyncCarryMode(*mode);
	j.phase = SyncCarryPhase(*phase);
	j.revision = *revision;
	j.pendingSeq = *seq;
	j.expectedAction = SyncChatAction(*action);
	j.lastOutcome = SyncCarryOutcome(*outcome);
	for (const auto &[name, field] : std::vector<std::pair<QString, QString*>>{
		{ u"operation"_q, &j.operationId }, { u"state_hash"_q, &j.stateHash },
		{ u"pending_state_hash"_q, &j.pendingStateHash }, { u"pending_stage_hash"_q, &j.pendingStageHash },
		{ u"mark_text"_q, &j.markText }, { u"mark_request"_q, &j.markRequestId },
		{ u"own_request"_q, &j.ownRequestId }, { u"next_state_hash"_q, &j.nextStateHash },
	}) {
		if (!o.value(name).isString()) {
			return std::nullopt;
		}
		*field = o.value(name).toString();
	}
	for (const auto &[name, field] : std::vector<std::pair<QString, bool*>>{
		{ u"mark_attempted"_q, &j.markAttempted },
		{ u"remove_old_mark_authorized"_q, &j.removeOldMarkAuthorized },
		{ u"commit_prepared"_q, &j.commitPrepared },
		{ u"destination_verified"_q, &j.destinationVerified },
		{ u"mark_verified"_q, &j.markVerified }, { u"cache_prepared"_q, &j.cachePrepared },
		{ u"state_committed"_q, &j.stateCommitted }, { u"cache_completed"_q, &j.cacheCompleted },
	}) {
		if (!o.value(name).isBool()) {
			return std::nullopt;
		}
		*field = o.value(name).toBool();
	}
	for (const auto &[name, field] : std::vector<std::pair<QString, std::optional<SyncChatCache>*>>{
		{ u"expected_winner"_q, &j.expectedWinner }, { u"old_mark"_q, &j.oldMark },
		{ u"target_mark"_q, &j.targetMark },
	}) {
		const auto value = o.value(name);
		if (!value.isNull()) {
			*field = ReadCache(value);
			if (!*field) {
				return std::nullopt;
			}
		}
	}
	const auto plan = o.value(u"plan"_q);
	if (!plan.isNull()) {
		if (!plan.isObject()) {
			return std::nullopt;
		}
		j.plan = ParseSyncCarryPlan(plan.toObject());
		if (!j.plan) {
			return std::nullopt;
		}
	}
	if (!o.value(u"missing"_q).isArray()) {
		return std::nullopt;
	}
	for (const auto &value : o.value(u"missing"_q).toArray()) {
		if (!value.isString()) {
			return std::nullopt;
		}
		j.missingHashes.push_back(value.toString());
	}
	j.preserved = o;
	return Valid(j) ? std::optional(j) : std::nullopt;
}

[[nodiscard]] SyncCarryJournalResult Finish(
		const SyncCarryJournal &journal,
		SyncCarryJournalStatus status = SyncCarryJournalStatus::Valid) {
	auto result = SerializeSyncCarryJournal(journal);
	if (result) {
		result.status = status;
	}
	return result;
}

[[nodiscard]] bool CanDeliver(const SyncCarryJournal &j, const QString &hash) {
	return j.plan && (j.plan->ownRecordHash == hash
		|| std::any_of(j.plan->selected.begin(), j.plan->selected.end(), [&](const auto &record) {
			return record.reference.recordHash == hash;
		}));
}

[[nodiscard]] bool CacheReceipt(
		const SyncCarryJournal &j,
		const SyncCarryCacheReceipt &receipt,
		bool complete) {
	return j.targetMark && receipt.operationId == j.operationId && receipt.account == j.account
		&& receipt.mark == *j.targetMark && receipt.complete == complete;
}

[[nodiscard]] std::optional<SyncChatCache> MarkCache(
		const SyncCarryMarkReadback &mark,
		const AccountKey &account,
		const PeerToken &chat) {
	if (mark.outcome != SyncCarryMarkOutcome::Valid || mark.account != account
		|| Normalize(mark.chat, account) != chat
		|| ParseSyncChatMark(mark.text, mark.hasMedia, mark.ownership).status
			!= SyncChatMarkStatus::Supported) {
		return std::nullopt;
	}
	const auto cache = SyncChatCache{ chat, mark.messageId, mark.date };
	return CacheValid(cache, false) ? std::optional(cache) : std::nullopt;
}

}

SyncCarryJournalResult SerializeSyncCarryJournal(const SyncCarryJournal &journal) {
	if (journal.version > 1) {
		return { SyncCarryJournalStatus::NewerVersion, journal, {} };
	}
	if (!Valid(journal)) {
		return { SyncCarryJournalStatus::Invalid, journal, {} };
	}
	const auto canonical = CanonicalizeSyncJson(QJsonDocument(Json(journal)).toJson());
	return canonical
		? SyncCarryJournalResult{ SyncCarryJournalStatus::Valid, journal, canonical.canonical }
		: SyncCarryJournalResult{ SyncCarryJournalStatus::Invalid, journal, {} };
}

SyncCarryJournalResult ParseSyncCarryJournal(const QByteArray &json) {
	const auto canonical = CanonicalizeSyncJson(json);
	if (!canonical) {
		return {};
	}
	const auto document = QJsonDocument::fromJson(canonical.canonical);
	if (!document.isObject()) {
		return {};
	}
	const auto version = Integer(document.object().value(u"version"_q));
	if (!version || !*version) {
		return {};
	} else if (*version > 1) {
		return { SyncCarryJournalStatus::NewerVersion, {}, {} };
	}
	const auto journal = Read(document.object());
	return journal
		? SyncCarryJournalResult{ SyncCarryJournalStatus::Valid, *journal, canonical.canonical }
		: SyncCarryJournalResult();
}

SyncCarryJournalResult BeginSyncCarryJournal(const SyncCarryJournalStart &input) {
	const auto source = Normalize(input.source, input.account);
	const auto target = Normalize(input.target, input.account);
	const auto stateHash = StateHash(input.state, input.stateAbsentConfirmed);
	if (!source || !target || source == target || !stateHash || !input.operationConfirmed
		|| !input.targetEligibility || (input.targetEligibility.readers.warningRequired
			&& !input.readerConsentAccepted) || input.otherStagesPresent) {
		return {};
	}
	if (input.state && (input.state->chat != source || input.state->library.pendingSeq
		|| (!input.state->config.pendingSeq && !input.state->configData.pending.isEmpty())
		|| CheckSyncAccountBinding(*input.state, input.preferenceToken)
			!= SyncAccountBindingVerdict::Bound)) {
		return {};
	}
	const auto &r = input.resolution;
	if ((input.mode == SyncCarryMode::Switch
			&& (!r.cache || Normalize(r.cache->chat, input.account) != source
				|| (r.action != SyncChatAction::Use && r.action != SyncChatAction::Searching
					&& r.action != SyncChatAction::Lost)))
		|| (input.mode == SyncCarryMode::Follow
			&& (r.action != SyncChatAction::Follow || !r.cache
				|| Normalize(r.cache->chat, input.account) != source))
		|| (input.mode == SyncCarryMode::Fill
			&& (r.action != SyncChatAction::Fill || r.cache || input.state
				|| !r.source || Normalize(*r.source, input.account) != source))
		|| (input.mode != SyncCarryMode::Switch
			&& (!r.winner || Normalize(r.winner->chat, input.account) != target))) {
		return {};
	}
	auto j = SyncCarryJournal();
	j.operationId = input.operationId;
	j.account = input.account;
	j.source = *source;
	j.target = *target;
	j.mode = input.mode;
	j.stateHash = *stateHash;
	j.pendingSeq = input.state ? input.state->config.pendingSeq : 0;
	j.phase = j.pendingSeq ? SyncCarryPhase::ResolvePending : SyncCarryPhase::ResolveAgain;
	j.expectedAction = r.action;
	j.expectedWinner = r.winner;
	j.oldMark = r.cache;
	if (j.expectedWinner) {
		const auto peer = Normalize(j.expectedWinner->chat, input.account);
		if (!peer) {
			return {};
		}
		j.expectedWinner->chat = *peer;
	}
	if (j.oldMark) {
		j.oldMark->chat = *source;
	}
	if (input.mode != SyncCarryMode::Switch) {
		j.targetMark = *r.winner;
		j.targetMark->chat = *target;
	}
	j.markText = input.markText;
	j.markRequestId = input.markRequestId;
	j.ownRequestId = input.ownRequestId;
	j.removeOldMarkAuthorized = input.removeOldMarkAuthorized && input.mode == SyncCarryMode::Switch;
	return SerializeSyncCarryJournal(j);
}

SyncCarryJournalAction NextSyncCarryJournalAction(const SyncCarryJournal &j) {
	if (!Valid(j)) {
		return SyncCarryJournalAction::None;
	}
	switch (j.phase) {
	case SyncCarryPhase::ResolvePending: return SyncCarryJournalAction::ResolvePending;
	case SyncCarryPhase::WritePendingState: return SyncCarryJournalAction::WritePendingState;
	case SyncCarryPhase::RetirePendingStage: return SyncCarryJournalAction::RetirePendingStage;
	case SyncCarryPhase::ResolveAgain: return SyncCarryJournalAction::ResolveAgain;
	case SyncCarryPhase::Plan: return SyncCarryJournalAction::Plan;
	case SyncCarryPhase::StageOwn: return SyncCarryJournalAction::StageOwn;
	case SyncCarryPhase::Deliver: return SyncCarryJournalAction::Deliver;
	case SyncCarryPhase::VerifyDestination: return SyncCarryJournalAction::VerifyDestination;
	case SyncCarryPhase::Mark:
		return j.mode == SyncCarryMode::Switch && !j.markAttempted && !j.targetMark
			? SyncCarryJournalAction::PostMark : SyncCarryJournalAction::ReadMark;
	case SyncCarryPhase::PrepareCache: return SyncCarryJournalAction::PrepareCache;
	case SyncCarryPhase::CommitState: return SyncCarryJournalAction::CommitState;
	case SyncCarryPhase::RetireOwnStage: return SyncCarryJournalAction::RetireOwnStage;
	case SyncCarryPhase::CompleteCache: return SyncCarryJournalAction::CompleteCache;
	case SyncCarryPhase::Cleanup: return SyncCarryJournalAction::Cleanup;
	case SyncCarryPhase::Done:
	case SyncCarryPhase::Aborted: return SyncCarryJournalAction::None;
	}
	return SyncCarryJournalAction::None;
}

SyncCarryJournalEvent SyncCarryEvent(const SyncCarryJournal &j, SyncCarryEventKind kind) {
	auto event = SyncCarryJournalEvent();
	event.operationId = j.operationId;
	event.revision = j.revision;
	event.account = j.account;
	event.source = j.source;
	event.target = j.target;
	event.kind = kind;
	return event;
}

std::vector<SyncCarryReference> SyncCarryJournalForwards(const SyncCarryJournal &j) {
	auto result = std::vector<SyncCarryReference>();
	if (!Valid(j) || !j.plan || j.phase != SyncCarryPhase::Deliver) {
		return result;
	}
	for (const auto &record : j.plan->selected) {
		if (std::binary_search(j.missingHashes.begin(), j.missingHashes.end(), record.reference.recordHash)) {
			result.push_back(record.reference);
		}
	}
	return result;
}

bool SyncCarryJournalPostsOwnRecord(const SyncCarryJournal &j) {
	return Valid(j) && j.plan && j.phase == SyncCarryPhase::Deliver
		&& !j.plan->ownRecord.isEmpty()
		&& std::binary_search(j.missingHashes.begin(), j.missingHashes.end(), j.plan->ownRecordHash);
}

SyncCarryJournalResult AdvanceSyncCarryJournal(
		const SyncCarryJournal &journal,
		const SyncCarryJournalEvent &event) {
	if (!Valid(journal) || journal.revision == kSafeInteger
		|| event.operationId != journal.operationId || event.revision != journal.revision
		|| event.account != journal.account || event.source != journal.source
		|| event.target != journal.target || event.outcome < SyncCarryOutcome::Success
		|| event.outcome > SyncCarryOutcome::Unknown
		|| journal.phase == SyncCarryPhase::Done || journal.phase == SyncCarryPhase::Aborted) {
		return { SyncCarryJournalStatus::Invalid, journal, {} };
	}
	auto j = journal;
	++j.revision;
	j.lastOutcome = event.outcome;
	const auto observed = StateHash(event.state, event.stateAbsentConfirmed);
	if (event.kind == SyncCarryEventKind::Cancel) {
		if (!event.noRequestInFlight || !event.noPendingFiles || !observed
			|| *observed != j.stateHash || j.markAttempted || j.markVerified || j.stateCommitted
			|| (j.phase != SyncCarryPhase::ResolveAgain && j.phase != SyncCarryPhase::Plan
				&& j.phase != SyncCarryPhase::Deliver && j.phase != SyncCarryPhase::VerifyDestination)
			|| (j.plan && !j.plan->ownRecord.isEmpty())) {
			return { SyncCarryJournalStatus::NeedsReview, journal, {} };
		}
		j.phase = SyncCarryPhase::Aborted;
		return Finish(j);
	}
	const auto expected = [&] {
		switch (j.phase) {
		case SyncCarryPhase::ResolvePending: return event.kind == SyncCarryEventKind::PendingPrepared;
		case SyncCarryPhase::WritePendingState:
		case SyncCarryPhase::CommitState: return event.kind == SyncCarryEventKind::StateObserved;
		case SyncCarryPhase::RetirePendingStage:
		case SyncCarryPhase::RetireOwnStage: return event.kind == SyncCarryEventKind::StageRetired;
		case SyncCarryPhase::ResolveAgain: return event.kind == SyncCarryEventKind::Resolution;
		case SyncCarryPhase::Plan: return event.kind == SyncCarryEventKind::PlanPrepared;
		case SyncCarryPhase::StageOwn: return event.kind == SyncCarryEventKind::OwnStagePersisted;
		case SyncCarryPhase::Deliver: return event.kind == SyncCarryEventKind::DeliveryPrepared;
		case SyncCarryPhase::VerifyDestination:
			return event.kind == SyncCarryEventKind::DestinationRead
				|| event.kind == SyncCarryEventKind::DeliveryResult;
		case SyncCarryPhase::Mark:
			return event.kind == SyncCarryEventKind::MarkRead
				|| (event.kind == SyncCarryEventKind::MarkPostResult && j.markAttempted)
				|| (event.kind == SyncCarryEventKind::MarkPostPrepared
					&& NextSyncCarryJournalAction(journal) == SyncCarryJournalAction::PostMark);
		case SyncCarryPhase::PrepareCache: return event.kind == SyncCarryEventKind::CachePrepared;
		case SyncCarryPhase::CompleteCache: return event.kind == SyncCarryEventKind::CacheCompleted;
		case SyncCarryPhase::Cleanup: return event.kind == SyncCarryEventKind::CleanupCompleted;
		case SyncCarryPhase::Done:
		case SyncCarryPhase::Aborted: return false;
		}
		return false;
	}();
	if (!expected) {
		return { SyncCarryJournalStatus::Invalid, journal, {} };
	}
	if (j.phase == SyncCarryPhase::Deliver && event.kind == SyncCarryEventKind::DeliveryPrepared) {
		j.phase = SyncCarryPhase::VerifyDestination;
		return Finish(j, event.outcome == SyncCarryOutcome::Success
			? SyncCarryJournalStatus::Valid : SyncCarryJournalStatus::Wait);
	}
	if (j.phase == SyncCarryPhase::Mark && event.kind == SyncCarryEventKind::MarkPostPrepared
		&& NextSyncCarryJournalAction(journal) == SyncCarryJournalAction::PostMark) {
		j.markAttempted = true;
		return Finish(j, event.outcome == SyncCarryOutcome::Success
			? SyncCarryJournalStatus::Valid : SyncCarryJournalStatus::Wait);
	}
	if (event.kind == SyncCarryEventKind::DeliveryResult
		|| event.kind == SyncCarryEventKind::MarkPostResult) {
		return Finish(j, event.outcome == SyncCarryOutcome::Success
			? SyncCarryJournalStatus::Valid : event.outcome == SyncCarryOutcome::Unknown
			? SyncCarryJournalStatus::Wait : SyncCarryJournalStatus::Retry);
	}
	if (event.outcome != SyncCarryOutcome::Success) {
		return Finish(j, event.outcome == SyncCarryOutcome::Unknown
			? SyncCarryJournalStatus::Wait : SyncCarryJournalStatus::Retry);
	}
	const auto invalid = [&] {
		return SyncCarryJournalResult{ SyncCarryJournalStatus::Invalid, journal, {} };
	};
	switch (j.phase) {
	case SyncCarryPhase::ResolvePending: {
		if (event.kind != SyncCarryEventKind::PendingPrepared || !event.objectsDurable
			|| !event.state || observed != j.stateHash) {
			return invalid();
		}
		const auto pending = event.pendingReadback
			? FinishPendingStageForCarry(j.account, *event.state, event.stagedRecord, *event.pendingReadback)
			: AbandonStageForLostChat(j.account, *event.state, j.source, event.oldRead,
				event.stagedRecord, event.retainedLocalText);
		const auto image = pending ? SerializeSyncLocalState(pending.state) : SyncLocalWriteResult();
		if (!pending || pending.status != SyncCarryPendingStatus::Ready || !image
			|| pending.beforeHash != j.stateHash || pending.stageSeq != j.pendingSeq
			|| event.preparedImageHash != SyncCarryRecordHash(image.canonical)) {
			return invalid();
		}
		j.pendingStateHash = event.preparedImageHash;
		j.pendingStageHash = pending.stageHash;
		j.phase = SyncCarryPhase::WritePendingState;
	} break;
	case SyncCarryPhase::WritePendingState:
		if (event.kind != SyncCarryEventKind::StateObserved || !observed || *observed != j.pendingStateHash) {
			return invalid();
		}
		if (!event.objectsDurable) {
			return Finish(j, SyncCarryJournalStatus::Wait);
		}
		j.stateHash = j.pendingStateHash;
		j.phase = SyncCarryPhase::RetirePendingStage;
		break;
	case SyncCarryPhase::RetirePendingStage:
		if (event.kind != SyncCarryEventKind::StageRetired
			|| event.retiredStageHash != j.pendingStageHash || !event.noPendingFiles) {
			return invalid();
		}
		j.pendingStateHash.clear();
		j.pendingStageHash.clear();
		j.pendingSeq = 0;
		j.phase = SyncCarryPhase::ResolveAgain;
		break;
	case SyncCarryPhase::ResolveAgain:
		if (event.kind != SyncCarryEventKind::Resolution) {
			return invalid();
		}
		if (event.resolution.action != j.expectedAction || event.resolution.winner != j.expectedWinner) {
			j.phase = SyncCarryPhase::Aborted;
			return Finish(j, SyncCarryJournalStatus::Superseded);
		}
		j.phase = SyncCarryPhase::Plan;
		break;
	case SyncCarryPhase::Plan:
		if (event.kind != SyncCarryEventKind::PlanPrepared || !event.objectsDurable || !event.plan
			|| observed != j.stateHash || !ParseSyncCarryPlan(SyncCarryPlanJson(*event.plan))
			|| event.plan->account != j.account || event.plan->source != j.source
			|| event.plan->target != j.target || event.plan->mode != j.mode
			|| event.plan->stateHash != j.stateHash
			|| !SyncCarryPlanUsesState(*event.plan, event.state)
			|| (j.mode != SyncCarryMode::Switch && event.plan->winner != j.targetMark)
			|| (!event.plan->ownRecord.isEmpty()
				&& (!event.state || !PrepareSyncCarryStage(*event.plan, *event.state)))) {
			return invalid();
		}
		j.plan = *event.plan;
		j.missingHashes.clear();
		for (const auto &reference : j.plan->forwards) {
			j.missingHashes.push_back(reference.recordHash);
		}
		if (!j.plan->ownRecord.isEmpty()) {
			j.missingHashes.push_back(j.plan->ownRecordHash);
		}
		std::sort(j.missingHashes.begin(), j.missingHashes.end());
		j.missingHashes.erase(std::unique(j.missingHashes.begin(), j.missingHashes.end()), j.missingHashes.end());
		j.phase = j.plan->ownRecord.isEmpty() ? SyncCarryPhase::Deliver : SyncCarryPhase::StageOwn;
		break;
	case SyncCarryPhase::StageOwn:
		if (event.kind != SyncCarryEventKind::OwnStagePersisted || !event.objectsDurable
			|| !event.state || observed != j.plan->reservedStateHash
			|| event.stagedRecord != j.plan->ownRecord || !PrepareSyncCarryStage(*j.plan, *event.state)) {
			return invalid();
		}
		j.phase = SyncCarryPhase::Deliver;
		break;
	case SyncCarryPhase::Deliver:
		return invalid();
	case SyncCarryPhase::VerifyDestination: {
		if (event.kind != SyncCarryEventKind::DestinationRead || !event.destination) {
			return invalid();
		}
		const auto verified = VerifySyncCarryReadback(*j.plan, *event.destination, event.playlist);
		if (!verified) {
			if (verified.status == SyncCarryStatus::MissingRecords) {
				j.missingHashes = verified.missingHashes;
				const auto deliverable = std::all_of(j.missingHashes.begin(), j.missingHashes.end(),
					[&](const auto &hash) { return CanDeliver(j, hash); });
				if (deliverable) {
					j.phase = SyncCarryPhase::Deliver;
				} else if (j.plan->ownRecord.isEmpty() && !j.markAttempted) {
					j.phase = SyncCarryPhase::Plan;
				} else {
					return Finish(j, SyncCarryJournalStatus::NeedsReview);
				}
				return Finish(j, SyncCarryJournalStatus::Retry);
			}
			return Finish(j, verified.status == SyncCarryStatus::ReadDestination
				|| verified.status == SyncCarryStatus::PlaylistRequired
				? SyncCarryJournalStatus::Wait : SyncCarryJournalStatus::NeedsReview);
		}
		const auto commit = PrepareSyncCarryCommit(*j.plan, event.state, *event.destination, event.playlist);
		if (!commit || !event.objectsDurable || event.preparedImageHash != commit.stateHash) {
			return invalid();
		}
		j.nextStateHash = commit.stateHash;
		j.commitPrepared = true;
		j.destinationVerified = true;
		j.missingHashes.clear();
		j.phase = SyncCarryPhase::Mark;
	} break;
	case SyncCarryPhase::Mark: {
		if (event.kind != SyncCarryEventKind::MarkRead) {
			return invalid();
		}
		if (event.mark.outcome == SyncCarryMarkOutcome::Unknown) {
			return Finish(j, SyncCarryJournalStatus::Wait);
		}
		if (event.mark.outcome == SyncCarryMarkOutcome::Absent && j.mode == SyncCarryMode::Switch
			&& j.markAttempted && !j.targetMark && event.mark.account == j.account
			&& Normalize(event.mark.chat, j.account) == j.target) {
			j.markAttempted = false;
			return Finish(j, SyncCarryJournalStatus::Retry);
		}
		const auto mark = MarkCache(event.mark, j.account, j.target);
		if (!mark || (j.targetMark && mark != j.targetMark)
			|| (j.mode == SyncCarryMode::Switch && event.mark.text != j.markText)) {
			return Finish(j, SyncCarryJournalStatus::NeedsReview);
		}
		if (j.mode == SyncCarryMode::Switch && !SwitchMarkOutranksKnown(j, *mark)) {
			return Finish(j, SyncCarryJournalStatus::NeedsReview);
		}
		j.targetMark = *mark;
		j.markVerified = true;
		j.phase = SyncCarryPhase::PrepareCache;
	} break;
	case SyncCarryPhase::PrepareCache:
		if (event.kind != SyncCarryEventKind::CachePrepared || !event.objectsDurable
			|| !CacheReceipt(j, event.cache, false)) {
			return invalid();
		}
		j.cachePrepared = true;
		j.phase = SyncCarryPhase::CommitState;
		break;
	case SyncCarryPhase::CommitState:
		if (event.kind != SyncCarryEventKind::StateObserved || observed != j.nextStateHash) {
			return invalid();
		}
		if (!event.objectsDurable) {
			return Finish(j, SyncCarryJournalStatus::Wait);
		}
		j.stateCommitted = true;
		j.phase = j.plan->ownRecord.isEmpty() ? SyncCarryPhase::CompleteCache : SyncCarryPhase::RetireOwnStage;
		break;
	case SyncCarryPhase::RetireOwnStage:
		if (event.kind != SyncCarryEventKind::StageRetired
			|| event.retiredStageHash != j.plan->ownRecordHash || !event.noPendingFiles) {
			return invalid();
		}
		j.phase = SyncCarryPhase::CompleteCache;
		break;
	case SyncCarryPhase::CompleteCache:
		if (event.kind != SyncCarryEventKind::CacheCompleted || !event.objectsDurable
			|| observed != j.nextStateHash || !CacheReceipt(j, event.cache, true)) {
			return invalid();
		}
		j.cacheCompleted = true;
		j.phase = SyncCarryPhase::Cleanup;
		break;
	case SyncCarryPhase::Cleanup: {
		if (event.kind != SyncCarryEventKind::CleanupCompleted || !event.destination
			|| observed != j.nextStateHash || !VerifySyncCarryReadback(*j.plan, *event.destination, event.playlist)) {
			return invalid();
		}
		const auto allowed = SyncCarryCleanupCandidates(*j.plan, *event.destination, event.playlist);
		if (allowed.size() != event.deleted.size()) {
			return invalid();
		}
		auto seen = std::set<int32_t>();
		for (const auto &reference : event.deleted) {
			if (!seen.insert(reference.messageId).second
				|| std::find(allowed.begin(), allowed.end(), reference) == allowed.end()) {
				return invalid();
			}
		}
		if (j.removeOldMarkAuthorized && j.oldMark && j.oldMark->markId) {
			const auto absent = (event.mark.outcome == SyncCarryMarkOutcome::Absent
					|| event.mark.outcome == SyncCarryMarkOutcome::ChatGone)
				&& event.mark.account == j.account && Normalize(event.mark.chat, j.account) == j.source
				&& event.mark.messageId == j.oldMark->markId;
			if (!absent) {
				return Finish(j, SyncCarryJournalStatus::Wait);
			}
		}
		j.phase = SyncCarryPhase::Done;
	} break;
	case SyncCarryPhase::Done:
	case SyncCarryPhase::Aborted:
		return invalid();
	}
	return Finish(j);
}

SyncCarryJournalResult ResumeSyncCarryJournal(
		const SyncCarryJournal &journal,
		const std::optional<SyncLocalState> &state,
		bool stateAbsentConfirmed,
		bool stateDurableConfirmed) {
	if (!Valid(journal) || journal.revision == kSafeInteger) {
		return { SyncCarryJournalStatus::Invalid, journal, {} };
	}
	if (journal.phase == SyncCarryPhase::Done || journal.phase == SyncCarryPhase::Aborted) {
		return Finish(journal);
	}
	const auto observed = StateHash(state, stateAbsentConfirmed);
	if (!observed) {
		return { SyncCarryJournalStatus::NeedsReview, journal, {} };
	}
	auto j = journal;
	++j.revision;
	if (j.cachePrepared && j.commitPrepared && *observed == j.nextStateHash) {
		if (!stateDurableConfirmed) {
			return { SyncCarryJournalStatus::Wait, journal, {} };
		}
		j.stateCommitted = true;
		j.phase = j.cacheCompleted ? SyncCarryPhase::Cleanup
			: (!j.plan->ownRecord.isEmpty() && j.phase <= SyncCarryPhase::RetireOwnStage)
			? SyncCarryPhase::RetireOwnStage : SyncCarryPhase::CompleteCache;
		return Finish(j);
	}
	if (j.stateCommitted) {
		return { SyncCarryJournalStatus::NeedsReview, journal, {} };
	}
	if (j.phase == SyncCarryPhase::WritePendingState && *observed == j.pendingStateHash) {
		if (!stateDurableConfirmed) {
			return { SyncCarryJournalStatus::Wait, journal, {} };
		}
		j.stateHash = j.pendingStateHash;
		j.phase = SyncCarryPhase::RetirePendingStage;
		return Finish(j);
	}
	if (*observed != j.stateHash && (!j.plan || j.plan->reservedStateHash.isEmpty()
		|| *observed != j.plan->reservedStateHash)) {
		return { SyncCarryJournalStatus::NeedsReview, journal, {} };
	}
	if (j.plan && j.phase >= SyncCarryPhase::StageOwn) {
		j.phase = !j.plan->ownRecord.isEmpty()
			&& (*observed == j.stateHash || j.phase == SyncCarryPhase::StageOwn)
			? SyncCarryPhase::StageOwn : SyncCarryPhase::VerifyDestination;
		j.destinationVerified = false;
		j.commitPrepared = false;
		j.markVerified = false;
		j.cachePrepared = false;
		j.nextStateHash.clear();
	}
	return Finish(j);
}

bool CheckSyncCarryDeletion(
		const SyncCarryJournal &j,
		const SyncCarryMessage &freshSource,
		const SyncCarryInventory &destination,
		const SyncPlaylistCarryReceipt &playlist) {
	return Valid(j) && j.phase == SyncCarryPhase::Cleanup && j.stateCommitted && j.cacheCompleted
		&& j.plan && SyncCarrySourceMatchesCleanup(*j.plan, freshSource, destination, playlist);
}

bool CheckSyncCarryOldMarkDeletion(
		const SyncCarryJournal &j,
		const SyncCarryMarkReadback &oldMark,
		const SyncCarryInventory &destination,
		const SyncPlaylistCarryReceipt &playlist) {
	return Valid(j) && j.phase == SyncCarryPhase::Cleanup && j.cacheCompleted && j.stateCommitted
		&& j.removeOldMarkAuthorized && j.mode == SyncCarryMode::Switch && j.oldMark
		&& j.oldMark->markId > 0 && j.plan && MarkCache(oldMark, j.account, j.source) == j.oldMark
		&& bool(VerifySyncCarryReadback(*j.plan, destination, playlist));
}

}
