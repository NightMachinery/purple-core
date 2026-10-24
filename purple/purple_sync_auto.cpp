/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_auto.h"

#include "purple/purple_config_payload.h"
#include "purple/purple_sync_json.h"

#include <QtCore/QCryptographicHash>

#include <algorithm>
#include <array>
#include <limits>

namespace Purple {
namespace {

QString Hash(const QByteArray &bytes) {
	return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool SameAccount(const AccountKey &a, const AccountKey &b) {
	return a == b && AccountKeyText(a.environment, a.userId).has_value();
}

SyncAutoPlan Hold(SyncAutoRecord record, SyncAutoReason reason) {
	auto result = SyncAutoPlan();
	result.action = SyncAutoAction::Hold;
	result.reason = reason;
	record.active = false;
	record.held = reason;
	result.notice = std::find(record.noticed.begin(), record.noticed.end(), reason) == record.noticed.end();
	if (result.notice) {
		record.noticed.push_back(reason);
	}
	result.record = std::move(record);
	return result;
}

SyncAutoPlan Emit(SyncAutoRecord record, SyncAutoAction action,
		const SyncAutoInput &input, bool commitOnly = false) {
	if (record.ticket == std::numeric_limits<uint64_t>::max()) {
		return Hold(std::move(record), SyncAutoReason::InvalidRecord);
	}
	record.active = true;
	record.held = SyncAutoReason::None;
	record.pending = action;
	record.commitOnly = commitOnly;
	record.session = input.facts.session;
	record.retryAt = 0;
	if (commitOnly || (action != SyncAutoAction::Check && action != SyncAutoAction::Reconcile)) {
		record.retries = 0;
	}
	if (action == SyncAutoAction::Check) {
		record.expectedState.clear();
		record.expectedFile.clear();
	} else if (action == SyncAutoAction::AutoApply) {
		record.record.clear();
	} else {
		record.expectedFile.clear();
	}
	++record.ticket;
	if (action == SyncAutoAction::Check || (action == SyncAutoAction::Reconcile && !commitOnly)) {
		record.lastCheck = input.now;
		record.checked = true;
	}
	auto result = SyncAutoPlan();
	result.action = action;
	result.ticket = record.ticket;
	result.commitOnly = commitOnly;
	result.messageId = record.messageId;
	result.bytes = action == SyncAutoAction::AutoApply ? record.expectedFile : record.record;
	if (commitOnly || action == SyncAutoAction::Publish || action == SyncAutoAction::AutoApply) {
		const auto parsed = ParseSyncLocalState(record.expectedState);
		if (!parsed) {
			return Hold(std::move(record), SyncAutoReason::StoreMismatch);
		}
		result.nextState = parsed.state;
	}
	result.record = std::move(record);
	return result;
}

SyncAutoPlan Retry(SyncAutoRecord record, const SyncAutoInput &input) {
	constexpr auto delays = std::array<int64_t, 3>{ 30, 120, 600 };
	if (record.retries >= int(delays.size())
		|| input.now > std::numeric_limits<int64_t>::max() - delays[record.retries]) {
		record.runStopped = true;
		record.retryAt = 0;
		return Hold(std::move(record), SyncAutoReason::RetryStopped);
	}
	record.active = false;
	record.retryAt = input.now + delays[record.retries++];
	auto result = SyncAutoPlan();
	result.action = SyncAutoAction::RetryAt;
	result.retryAt = record.retryAt;
	result.record = std::move(record);
	return result;
}

SyncAutoReason Gate(const SyncAutoFacts &facts) {
	if (!facts.signedIn) { return SyncAutoReason::AccountUnavailable; }
	if (!facts.ownerAnswered) { return SyncAutoReason::OwnershipUnanswered; }
	if (!SameAccount(facts.account, facts.fileAccount)) { return SyncAutoReason::WrongFileAccount; }
	if (!facts.state || CheckSyncAccountBinding(*facts.state, facts.bindingToken)
		!= SyncAccountBindingVerdict::Bound || facts.state->space.isEmpty()) {
		return SyncAutoReason::AccountUnbound;
	}
	if (!facts.chatResolved || facts.chat != facts.state->chat
		|| facts.chat.kind != PeerTokenKind::Self) {
		return SyncAutoReason::SyncChatNotResolved;
	}
	if (!facts.chatWritable) { return SyncAutoReason::ChatNotWritable; }
	if (!SerializeSyncLocalState(*facts.state)) { return SyncAutoReason::StoreMismatch; }
	if (facts.currentDevice != facts.state->createdDevice) { return SyncAutoReason::CloneDetected; }
	if (facts.local.status == SyncSettingsFileStatus::Absent) { return SyncAutoReason::FileAbsent; }
	if (facts.local.status != SyncSettingsFileStatus::Present
		|| !SameSyncSettingsFile(facts.local, MakeSyncSettingsFile(facts.local.status,
			facts.local.text, facts.local.usingLastGood))
		|| !SyncSettingsTextWritable(facts.local.text)) {
		return SyncAutoReason::InvalidSettings;
	}
	if (facts.local.usingLastGood) { return SyncAutoReason::LastGood; }
	return SyncAutoReason::None;
}

SyncAutoPlan Done(SyncAutoRecord record, const SyncAutoInput &input) {
	record.active = false;
	record.retries = 0;
	record.retryAt = 0;
	record.pending = SyncAutoAction::NoOp;
	record.commitOnly = false;
	record.expectedState.clear();
	record.expectedFile.clear();
	record.record.clear();
	record.messageId = 0;
	record.uncertain = false;
	record.retiring = false;
	const auto checkDue = !record.checked || (input.now >= record.lastCheck
		&& input.now - record.lastCheck >= kSyncAutoCheckSeconds);
	if (record.queuedSave || (record.queuedCheck && checkDue)) {
		record.queuedSave = false;
		record.queuedCheck = false;
		return Emit(std::move(record), SyncAutoAction::Check, input);
	}
	record.queuedCheck = false;
	auto result = SyncAutoPlan();
	result.record = std::move(record);
	return result;
}

const SyncAutoServerRead *FreshRead(const SyncAutoInventory &inventory, int32_t id) {
	const SyncAutoServerRead *result = nullptr;
	for (const auto &read : inventory.reads) {
		if (read.messageId != id) { continue; }
		if (result || !read.fresh) { return nullptr; }
		result = &read;
	}
	return result;
}

const SyncAutoServerRead *OwnedRead(const SyncAutoInventory &inventory, int32_t id) {
	const auto read = FreshRead(inventory, id);
	return read && read->owned && read->presence == SyncAutoMessagePresence::Present ? read : nullptr;
}

SyncAutoPlan Commit(SyncAutoRecord record, const SyncLocalState &next,
		const SyncAutoInput &input) {
	const auto serialized = SerializeSyncLocalState(next);
	if (!serialized) { return Hold(std::move(record), SyncAutoReason::StoreMismatch); }
	record.expectedState = serialized.canonical;
	return Emit(std::move(record), SyncAutoAction::Reconcile, input, true);
}

bool UnseenHeads(const SyncLocalState &state, const SyncConfigHeads &heads) {
	for (const auto &record : heads.heads) {
		const auto &key = record.head.key;
		if (key != state.configData.base
			&& std::find(state.configData.baseLineage.begin(), state.configData.baseLineage.end(), key)
				== state.configData.baseLineage.end()
			&& std::find(state.configData.equiv.begin(), state.configData.equiv.end(), key)
				== state.configData.equiv.end()) {
			return true;
		}
	}
	return false;
}

SyncAutoPlan Inventory(SyncAutoRecord record, const SyncAutoInput &input) {
	const auto &facts = input.facts;
	const auto &state = *facts.state;
	if (!input.inventory) { return Retry(std::move(record), input); }
	const auto &snapshot = *input.inventory;
	if (!snapshot.fresh || !SameAccount(snapshot.account, facts.account)
		|| snapshot.chat != facts.chat || snapshot.session != facts.session
		|| snapshot.ticket != input.completedTicket || snapshot.readAt < record.lastCheck
		|| snapshot.readAt > input.now
		|| snapshot.inventory.accountUserId != uint64_t(facts.account.userId)) {
		return Hold(std::move(record), SyncAutoReason::OwnershipChanged);
	}
	auto inventory = snapshot.inventory;
	SelectSyncSpaceIfEmpty(inventory, state.space);
	if (inventory.status == SyncAccountInventoryStatus::Incomplete) {
		return Retry(std::move(record), input);
	} else if (inventory.status != SyncAccountInventoryStatus::Complete) {
		return Hold(std::move(record), SyncAutoReason::NeedsReview);
	}
	const auto own = ReconcileOwnConfigInventory(state, inventory, uint64_t(facts.account.userId), facts.stagedRecord);
	if (own.status == SyncOwnInventoryStatus::CloneDetected) {
		return Hold(std::move(record), SyncAutoReason::CloneDetected);
	}
	if (own.status == SyncOwnInventoryStatus::Incomplete) { return Retry(std::move(record), input); }
	if (own.status != SyncOwnInventoryStatus::Absent && own.status != SyncOwnInventoryStatus::Present
		&& own.status != SyncOwnInventoryStatus::PendingFound) {
		return Hold(std::move(record), SyncAutoReason::NeedsReview);
	}
	if ((own.status == SyncOwnInventoryStatus::Absent && state.config.confirmedSeq != 0)
		|| (own.status == SyncOwnInventoryStatus::Present && own.observation.seq < state.config.confirmedSeq)) {
		return Hold(std::move(record), SyncAutoReason::NeedsReview);
	}
	if (record.retiring && record.messageId > 0) {
		if (!inventory.read) { return Retry(std::move(record), input); }
		const auto found = std::find_if(inventory.read->records.begin(), inventory.read->records.end(),
			[&](const auto &entry) { return entry.id == record.messageId; });
		const auto read = FreshRead(snapshot, record.messageId);
		if (!read || read->presence == SyncAutoMessagePresence::Unknown) {
			return Retry(std::move(record), input);
		}
		if (read->presence == SyncAutoMessagePresence::Absent) {
			if (!read->record.isEmpty() || found != inventory.read->records.end()) {
				return Hold(std::move(record), SyncAutoReason::OwnershipChanged);
			}
			if (SerializeSyncLocalState(state).canonical == record.expectedState) {
				record.retiring = false;
				record.messageId = 0;
				record.record.clear();
			} else {
				const auto removed = RemoveAbsentOwnConfigMessage(state, record.messageId, SyncOwnMessagePresence::Absent);
				if (!removed) { return Hold(std::move(record), SyncAutoReason::StoreMismatch); }
				return Commit(std::move(record), removed.state, input);
			}
		} else if (read->presence != SyncAutoMessagePresence::Present || !read->owned
			|| read->record != record.record
			|| !CheckOwnConfigMessageDeletion(state, record.messageId, read->record)) {
			return Hold(std::move(record), SyncAutoReason::OwnershipChanged);
		}
	}
	if (state.config.pendingSeq != 0) {
		if (facts.stagedRecord.isEmpty() || (!record.record.isEmpty() && record.record != facts.stagedRecord)) {
			return Hold(std::move(record), SyncAutoReason::StoreMismatch);
		}
		record.record = facts.stagedRecord;
		if (own.status == SyncOwnInventoryStatus::PendingFound) {
			if (!own.pendingMessageId) { return Hold(std::move(record), SyncAutoReason::StoreMismatch); }
			const auto read = OwnedRead(snapshot, *own.pendingMessageId);
			if (!read || read->record != facts.stagedRecord) {
				return Hold(std::move(record), SyncAutoReason::OwnershipChanged);
			}
			const auto payload = InspectConfigPayload(ParseSyncEnvelope(facts.stagedRecord));
			const auto confirmed = ConfirmConfigReadBack(state, facts.currentDevice, own.observation, payload.version);
			if (confirmed.verdict != SyncCloneVerdict::NoClone) {
				return Hold(std::move(record), SyncAutoReason::CloneDetected);
			}
			const auto issued = std::find_if(state.config.issuedRecords.begin(), state.config.issuedRecords.end(),
				[&](const auto &entry) { return entry.seq == state.config.pendingSeq
					&& entry.recordHash == Hash(facts.stagedRecord); });
			if (issued == state.config.issuedRecords.end()) {
				return Hold(std::move(record), SyncAutoReason::StoreMismatch);
			}
			const auto adopted = RecordConfirmedOwnConfigMessage(confirmed.state, *own.pendingMessageId, facts.stagedRecord);
			if (!adopted) { return Hold(std::move(record), SyncAutoReason::StoreMismatch); }
			record.uncertain = false;
			return Commit(std::move(record), adopted.state, input);
		}
		const auto heads = ExtractSyncConfigHeads(inventory, state.space, state.install);
		if (heads.status != SyncConfigHeadsStatus::Complete || UnseenHeads(state, heads)) {
			return Hold(std::move(record), SyncAutoReason::Conflict);
		}
		if (facts.queue != SyncConfigSendQueue::Empty) { return Retry(std::move(record), input); }
		auto observations = SyncPublishObservations();
		observations.discoveryComplete = true;
		observations.ready = true;
		observations.stagedRecordMatches = true;
		observations.stagedRecordHash = Hash(facts.stagedRecord);
		observations.ownRecord = own.observation;
		observations.ownHead = own.head;
		observations.attempt = SyncPublishAttempt::ReconciledAbsent;
		observations.editRefused = record.editRefused;
		if (own.head.messageId > 0) {
			const auto read = OwnedRead(snapshot, own.head.messageId);
			if (!read || Hash(read->record) != own.head.recordHash) {
				return Hold(std::move(record), SyncAutoReason::OwnershipChanged);
			}
		}
		const auto decision = PlanSyncPublish(state, facts.bindingToken, facts.currentDevice,
			SyncLocalStream::Config, state.config.ownHash, { true, facts.editSupported, false }, observations);
		if (decision.action != SyncPublishAction::Edit && decision.action != SyncPublishAction::Post) {
			return Hold(std::move(record), SyncAutoReason::NeedsReview);
		}
		record.messageId = decision.messageId;
		record.uncertain = true;
		return Emit(std::move(record), decision.action == SyncPublishAction::Edit
			? SyncAutoAction::EditInPlace : SyncAutoAction::Post, input);
	}
	if (!facts.stagedRecord.isEmpty()) { return Hold(std::move(record), SyncAutoReason::StoreMismatch); }
	const auto review = ReviewSyncConfigInventory(inventory, &state, {}, facts.local);
	if (review.status != SyncConfigReviewStatus::Ready || !review.bound) {
		return Hold(std::move(record), SyncAutoReason::NeedsReview);
	}
	if (review.plan.verdict == ConfigSyncVerdict::UpdateReady) {
		if (review.plan.offered.size() != 1 || state.configData.base.isEmpty()) {
			return Hold(std::move(record), SyncAutoReason::Conflict);
		}
		const auto &head = review.plan.offered[0];
		if (head.key == state.configData.base
			|| std::find(head.lineage.begin(), head.lineage.end(), state.configData.base) == head.lineage.end()
			|| facts.local.fingerprint != SyncConfigKeyFingerprint(state.configData.base)) {
			return Hold(std::move(record), SyncAutoReason::Conflict);
		}
		const auto apply = PlanSyncConfigApply(review, SyncConfigReviewStamp(review), head.key);
		if (apply.status != SyncConfigApplyPlanStatus::Ready || apply.join || !apply.source) {
			return Hold(std::move(record), SyncAutoReason::NeedsReview);
		}
		auto impactInput = facts.impactContext;
		impactInput.current = { facts.local.text, true };
		impactInput.proposed = { apply.source->text, true };
		const auto impact = AnalyzeConfigChange(impactInput);
		if (!facts.impactContextComplete || impact.status != ConfigImpactStatus::Ready || impact.dangerous
			|| std::any_of(impact.findings.begin(), impact.findings.end(), [](const auto &finding) {
				return finding.dangerous || (finding.kind != ConfigImpactKind::ValueChanged
					&& finding.kind != ConfigImpactKind::Pinned && finding.kind != ConfigImpactKind::Unpinned);
			})) {
			return Hold(std::move(record), SyncAutoReason::UnsafeImpact);
		}
		if (impactInput.workMode != facts.proposedWorkMode
			|| ParseSettings(QString::fromUtf8(facts.local.text), {}).settings.version
				!= ParseSettings(QString::fromUtf8(apply.source->text), {}).settings.version) {
			return Hold(std::move(record), SyncAutoReason::WorkModeScopeChanged);
		}
		const auto completed = CompleteSyncConfigApply(apply, MakeSyncSettingsFile(SyncSettingsFileStatus::Present, apply.source->text));
		if (completed.status != SyncConfigApplyCompletionStatus::Ready || !completed.adopted) {
			return Hold(std::move(record), SyncAutoReason::StoreMismatch);
		}
		const auto checked = CheckSyncConfigDataCommit(state, SyncLocalConfigOf(*completed.adopted));
		if (checked.status != SyncConfigCommitStatus::Ready && checked.status != SyncConfigCommitStatus::Unchanged) {
			return Hold(std::move(record), SyncAutoReason::StoreMismatch);
		}
		record.expectedState = checked.canonical;
		record.expectedFile = apply.source->text;
		auto result = Emit(std::move(record), SyncAutoAction::AutoApply, input);
		result.apply = apply;
		return result;
	}
	if (review.plan.verdict == ConfigSyncVerdict::Conflict || review.plan.verdict == ConfigSyncVerdict::Choose) {
		return Hold(std::move(record), SyncAutoReason::Conflict);
	}
	if (review.plan.verdict == ConfigSyncVerdict::LocalChanges || review.plan.verdict == ConfigSyncVerdict::Empty) {
		const auto heads = ExtractSyncConfigHeads(inventory, state.space, state.install);
		if (heads.status != SyncConfigHeadsStatus::Complete || UnseenHeads(state, heads)) {
			return Hold(std::move(record), SyncAutoReason::Conflict);
		}
		if (record.savedFingerprint != facts.local.fingerprint && facts.savedFingerprint != facts.local.fingerprint) {
			return Hold(std::move(record), SyncAutoReason::NoSavedBytes);
		}
		const auto choice = PlanConfigChoice(review.state, review.plan, std::nullopt);
		if (!choice || !choice->publish || !choice->adopt.empty()) {
			return Hold(std::move(record), SyncAutoReason::Conflict);
		}
		const auto stage = PlanSyncConfigPost(state, facts.bindingToken, {}, inventory, facts.local,
			{ false, facts.local.fingerprint, SyncConfigVersionKeys(choice->parents) }, input.now, facts.writer, facts.queue);
		if (stage.step != SyncConfigPostStep::Stage) { return Hold(std::move(record), SyncAutoReason::NeedsReview); }
		const auto envelope = ParseSyncEnvelope(stage.record);
		const auto payloadHash = envelope.envelope.document.value(u"payload_sha256"_q).toString();
		const auto reservation = ReserveSyncSeq(state, SyncLocalStream::Config, payloadHash);
		if (!reservation) { return Hold(std::move(record), SyncAutoReason::StoreMismatch); }
		auto reserved = reservation.state;
		reserved.configData = stage.nextConfigData;
		const auto issued = AppendIssuedConfigRecord(reserved, stage.record);
		if (!issued) { return Hold(std::move(record), SyncAutoReason::StoreMismatch); }
		const auto &next = issued.state;
		const auto serialized = SerializeSyncLocalState(next);
		if (!serialized) { return Hold(std::move(record), SyncAutoReason::StoreMismatch); }
		record.expectedState = serialized.canonical;
		record.record = stage.record;
		record.editRefused = false;
		record.queuedSave = false;
		return Emit(std::move(record), SyncAutoAction::Publish, input);
	}
	if (review.plan.verdict != ConfigSyncVerdict::UpToDate) {
		return Hold(std::move(record), SyncAutoReason::NeedsReview);
	}
	if (record.retired >= kSyncAutoRetireLimit) { return Done(std::move(record), input); }
	auto observations = SyncPublishObservations();
	observations.discoveryComplete = true;
	observations.ready = true;
	observations.ownRecord = own.observation;
	const auto decision = PlanSyncPublish(state, facts.bindingToken, facts.currentDevice,
		SyncLocalStream::Config, state.config.ownHash, { true, facts.editSupported, false }, observations);
	if (decision.action == SyncPublishAction::RetireCandidate) {
		const auto ledger = std::find_if(state.ownMessages.begin(), state.ownMessages.end(),
			[&](const auto &message) { return message.messageId == decision.messageId; });
		const auto read = OwnedRead(snapshot, decision.messageId);
		if (ledger == state.ownMessages.end() || ledger->install != state.install
			|| ledger->device != state.createdDevice || ledger->space != state.space || !read
			|| !CheckOwnConfigMessageDeletion(state, decision.messageId, read->record)
			|| std::none_of(state.config.issuedRecords.begin(), state.config.issuedRecords.end(),
				[&](const auto &entry) { return entry.seq == ledger->seq && entry.recordHash == ledger->recordHash; })) {
			return Hold(std::move(record), SyncAutoReason::OwnershipChanged);
		}
		const auto removed = RemoveAbsentOwnConfigMessage(state, decision.messageId, SyncOwnMessagePresence::Absent);
		if (!removed) { return Hold(std::move(record), SyncAutoReason::StoreMismatch); }
		record.expectedState = SerializeSyncLocalState(removed.state).canonical;
		record.messageId = decision.messageId;
		record.record = read->record;
		record.retiring = true;
		++record.retired;
		return Emit(std::move(record), SyncAutoAction::Retire, input);
	}
	if (decision.action != SyncPublishAction::Wait) {
		return Hold(std::move(record), SyncAutoReason::NeedsReview);
	}
	return Done(std::move(record), input);
}

SyncAutoPlan PlanSyncAutoStep(const SyncAutoInput &input) {
	auto result = SyncAutoPlan();
	result.record = input.record;
	result.reason = input.record.held;
	if (!SerializeSyncAutoRecord(input.record) || !SameAccount(input.record.account, input.facts.account)
		|| input.now <= 0 || input.facts.session.isEmpty()
		|| int(input.event) < 0 || int(input.event) > int(SyncAutoEvent::Completed)
		|| int(input.outcome) < 0 || int(input.outcome) > int(SyncAutoOutcome::DeletionRefused)) {
		result.action = SyncAutoAction::Hold;
		result.reason = SyncAutoReason::InvalidRecord;
		return result;
	}
	if (!input.facts.accountLock || input.sequence <= input.record.event) { return result; }
	auto record = input.record;
	if (input.event == SyncAutoEvent::Completed && (!record.active
		|| input.completedTicket != record.ticket || record.session != input.facts.session)) {
		return result;
	}
	record.event = input.sequence;
	if (input.event == SyncAutoEvent::Disable) {
		record.enabled = false;
		record.held = SyncAutoReason::None;
		result.reason = SyncAutoReason::None;
		record.active = false;
		result.record = std::move(record);
		return result;
	}
	if (input.event == SyncAutoEvent::Enable) { record.enabled = true; }
	if (!record.enabled) { result.record = std::move(record); return result; }
	if (record.retryAt && input.event != SyncAutoEvent::Completed) {
		if (input.event == SyncAutoEvent::LocalSaved && input.facts.saveDebounceFinished
			&& Gate(input.facts) == SyncAutoReason::None) {
			record.savedFingerprint = input.facts.local.fingerprint;
			record.queuedSave = true;
		} else if (input.event != SyncAutoEvent::Retry && input.event != SyncAutoEvent::LocalSaved) {
			record.queuedCheck = true;
		}
		if (input.event != SyncAutoEvent::Retry || input.now < record.retryAt) {
			result.record = std::move(record);
			return result;
		}
	}
	const auto gate = Gate(input.facts);
	if (gate != SyncAutoReason::None) { return Hold(std::move(record), gate); }
	if (input.event == SyncAutoEvent::LocalSaved) {
		if (!input.facts.saveDebounceFinished) { result.record = std::move(record); return result; }
		record.savedFingerprint = input.facts.local.fingerprint;
		record.queuedSave = true;
	} else if (input.event != SyncAutoEvent::Completed && input.event != SyncAutoEvent::Retry) {
		record.queuedCheck = true;
	}
	if (!input.facts.inFlightSession.isEmpty()
		&& (input.event != SyncAutoEvent::Completed || input.facts.inFlightSession != input.facts.session)) {
		result.record = std::move(record);
		return result;
	}
	if (input.event == SyncAutoEvent::Completed) {
		if (input.outcome == SyncAutoOutcome::NetworkFailure || input.outcome == SyncAutoOutcome::OutcomeUnknown) {
			if (record.pending == SyncAutoAction::AutoApply || record.pending == SyncAutoAction::Publish || record.commitOnly) {
				return Hold(std::move(record), SyncAutoReason::StateChanged);
			}
			return Retry(std::move(record), input);
		}
		if (record.pending == SyncAutoAction::EditInPlace && input.outcome == SyncAutoOutcome::EditRefused) {
			record.editRefused = true;
			return Emit(std::move(record), SyncAutoAction::Reconcile, input);
		}
		if (record.pending == SyncAutoAction::Retire && input.outcome == SyncAutoOutcome::DeletionRefused) {
			return Hold(std::move(record), SyncAutoReason::DeletionRefused);
		}
		if ((record.pending == SyncAutoAction::Check || (record.pending == SyncAutoAction::Reconcile && !record.commitOnly))
			&& input.outcome == SyncAutoOutcome::Inventory) {
			record.active = false;
			return Inventory(std::move(record), input);
		}
		if ((record.pending == SyncAutoAction::Post || record.pending == SyncAutoAction::EditInPlace)
			&& input.outcome == SyncAutoOutcome::Sent) {
			record.retries = 0;
			return Emit(std::move(record), SyncAutoAction::Reconcile, input);
		}
		const auto expectedOutcome = record.pending == SyncAutoAction::Publish ? SyncAutoOutcome::Staged
			: record.pending == SyncAutoAction::AutoApply ? SyncAutoOutcome::Applied
			: record.pending == SyncAutoAction::Retire ? SyncAutoOutcome::Deleted : SyncAutoOutcome::Committed;
		if (input.outcome != expectedOutcome || record.expectedState.isEmpty()
			|| SerializeSyncLocalState(*input.facts.state).canonical != record.expectedState
			|| (record.pending == SyncAutoAction::AutoApply && input.facts.local.text != record.expectedFile)
			|| (record.pending == SyncAutoAction::Publish && input.facts.stagedRecord != record.record)
			|| (record.commitOnly && !input.facts.stagedRecord.isEmpty())) {
			return Hold(std::move(record), SyncAutoReason::StateChanged);
		}
		if (record.pending == SyncAutoAction::AutoApply) {
			record.savedFingerprint = input.facts.local.fingerprint;
		}
		if (record.commitOnly || record.pending == SyncAutoAction::Retire) {
			record.record.clear();
			record.messageId = 0;
			record.uncertain = false;
			record.retiring = false;
		}
		record.retries = 0;
		return Emit(std::move(record), SyncAutoAction::Check, input);
	}
	if (record.runStopped) {
		if (input.event == SyncAutoEvent::Retry) {
			result.record = std::move(record);
			return result;
		}
		if (record.run == std::numeric_limits<uint64_t>::max()) {
			return Hold(std::move(record), SyncAutoReason::InvalidRecord);
		}
		++record.run;
		record.runStopped = false;
		record.retries = 0;
		record.retired = 0;
		return Emit(std::move(record), SyncAutoAction::Reconcile, input);
	}
	if (record.active && record.session == input.facts.session) {
		result.record = std::move(record);
		return result;
	}
	if (record.pending == SyncAutoAction::AutoApply) {
		if (SerializeSyncLocalState(*input.facts.state).canonical == record.expectedState
			&& input.facts.local.text == record.expectedFile) {
			record.savedFingerprint = input.facts.local.fingerprint;
			return Emit(std::move(record), SyncAutoAction::Check, input);
		}
		return Hold(std::move(record), SyncAutoReason::InterruptedApply);
	}
	if (input.event == SyncAutoEvent::Retry) {
		if (!record.retryAt || input.now < record.retryAt) { result.record = std::move(record); return result; }
		return Emit(std::move(record), SyncAutoAction::Reconcile, input);
	}
	if (record.pending != SyncAutoAction::NoOp && (record.uncertain || record.active
		|| (input.facts.state && input.facts.state->config.pendingSeq))) {
		return Emit(std::move(record), SyncAutoAction::Reconcile, input);
	}
	if (!record.queuedSave && record.checked
		&& (input.now < record.lastCheck || input.now - record.lastCheck < kSyncAutoCheckSeconds)) {
		result.record = std::move(record);
		return result;
	}
	if (record.run == std::numeric_limits<uint64_t>::max()) {
		return Hold(std::move(record), SyncAutoReason::InvalidRecord);
	}
	++record.run;
	record.retired = 0;
	record.queuedCheck = false;
	record.queuedSave = false;
	return Emit(std::move(record), SyncAutoAction::Check, input);
}

}

SyncAutoPlan PlanSyncAuto(const SyncAutoInput &input) {
	auto result = PlanSyncAutoStep(input);
	if (SerializeSyncAutoRecord(result.record) || !SerializeSyncAutoRecord(input.record)) {
		return result;
	}
	auto held = Hold(input.record, SyncAutoReason::StoreMismatch);
	held.record.event = std::max(input.record.event, input.sequence);
	if (!SerializeSyncAutoRecord(held.record)) {
		held.record = input.record;
		held.notice = false;
	}
	return held;
}

}
