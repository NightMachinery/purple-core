/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_auto.h"
#include "purple/purple_config_payload.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <map>

using namespace Purple;

namespace {
auto Checks = 0;
auto Failures = 0;
const char *Section = "";
void Report(bool condition, const char *expression, int line) {
	++Checks;
	if (!condition) { ++Failures; std::printf("FAIL %s:%d %s\n", Section, line, expression); std::fflush(stdout); }
}
#define CHECK(condition) Report(bool(condition), #condition, __LINE__)

const auto T0 = QByteArray("version = 1\n# original\n");
const auto T1 = QByteArray("version = 1\n# changed\n");
const auto T2 = QByteArray("version = 1\n# changed again\n");

ConfigVersion Version(const QByteArray &record) {
	return InspectConfigPayload(ParseSyncEnvelope(record)).version;
}

struct Harness {
	SyncAutoRecord record;
	SyncAutoFacts facts;
	std::map<int32_t, QByteArray> cloud;
	int32_t nextId = 100;
	int64_t now = 1800000000;

	Harness() {
		facts.account = { 0, 777 };
		facts.fileAccount = facts.account;
		facts.signedIn = facts.ownerAnswered = facts.accountLock = true;
		facts.chatResolved = facts.chatWritable = true;
		facts.session = u"session-a"_q;
		facts.state = SyncLocalState();
		auto &state = *facts.state;
		state.install = *FormatSyncInstallId(QByteArray(16, 'a'));
		state.createdDevice = u"desktop:"_q + state.install;
		state.space = *FormatSyncSpaceId(QByteArray(16, 's'));
		state.bindingToken = *FormatSyncBindingToken(QByteArray(16, 't'));
		facts.bindingToken = state.bindingToken.toLatin1();
		facts.currentDevice = state.createdDevice;
		facts.writer = { u"macOS"_q, u"Purple test"_q };
		facts.impactContextComplete = true;
		facts.saveDebounceFinished = true;
		setLocal(T0);
		record.account = facts.account;
		CHECK(SerializeSyncLocalState(state));
	}

	void setLocal(const QByteArray &text, bool saved = true) {
		facts.local = MakeSyncSettingsFile(SyncSettingsFileStatus::Present, text);
		facts.savedFingerprint = saved ? facts.local.fingerprint : QString();
	}

	SyncAutoInventory snapshot() const {
		auto scan = SyncHistoryScanResult();
		scan.status = SyncHistoryScanStatus::Complete;
		auto read = SyncCandidateReadResult();
		auto result = SyncAutoInventory();
		for (const auto &[id, bytes] : cloud) {
			scan.candidateIds.push_back(id);
			auto candidate = ClassifySyncCandidate(id, bytes);
			candidate.documentId = uint64_t(id) + 5000;
			candidate.editDate = uint64_t(now);
			read.records.push_back(candidate);
			result.reads.push_back({ id, bytes, true, true, SyncAutoMessagePresence::Present });
		}
		if (record.retiring && !cloud.contains(record.messageId)) {
			result.reads.push_back({ record.messageId, {}, true, false, SyncAutoMessagePresence::Absent });
		}
		scan.scannedCount = cloud.size();
		read.status = AggregateSyncCandidateRead(read.records);
		result.inventory = FinishSyncAccountInventory(uint64_t(facts.account.userId), scan, read);
		result.account = facts.account;
		result.chat = facts.chat;
		result.session = facts.session;
		result.ticket = record.ticket;
		result.readAt = now;
		result.fresh = true;
		return result;
	}

	SyncAutoInput input(SyncAutoEvent event, SyncAutoOutcome outcome = SyncAutoOutcome::Inventory) const {
		auto result = SyncAutoInput();
		result.record = record;
		result.facts = facts;
		result.event = event;
		result.sequence = record.event + 1;
		result.now = now;
		result.completedTicket = record.ticket;
		result.outcome = outcome;
		if (event == SyncAutoEvent::Completed && outcome == SyncAutoOutcome::Inventory) {
			result.inventory = snapshot();
		}
		return result;
	}

	SyncAutoPlan run(SyncAutoInput input) {
		const auto plan = PlanSyncAuto(input);
		if (qEnvironmentVariableIsSet("PURPLE_TEST_TRACE")) {
			std::printf("%s event=%d outcome=%d action=%d reason=%d ticket=%llu\n", Section,
				int(input.event), int(input.outcome), int(plan.action), int(plan.reason),
				static_cast<unsigned long long>(plan.record.ticket));
			std::fflush(stdout);
		}
		const auto repeated = PlanSyncAuto(input);
		CHECK(plan.action == repeated.action && plan.record == repeated.record);
		CHECK(IsSyncAutoRecord(plan.record));
		const auto bytes = SerializeSyncAutoRecord(plan.record);
		CHECK(bytes);
		if (bytes) {
			const auto parsed = ParseSyncAutoRecord(*bytes);
			CHECK(parsed.status == SyncAutoCodecStatus::Valid);
			CHECK(parsed.record && *parsed.record == plan.record);
		}
		record = plan.record;
		if (record.event == input.sequence) {
			input.record = record;
			CHECK(PlanSyncAuto(input).action == SyncAutoAction::NoOp);
		}
		return plan;
	}

	SyncAutoPlan event(SyncAutoEvent event, SyncAutoOutcome outcome = SyncAutoOutcome::Inventory) {
		return run(input(event, outcome));
	}

	SyncAutoPlan inventory() { return event(SyncAutoEvent::Completed); }

	void commit(const SyncAutoPlan &plan) {
		CHECK(plan.nextState);
		if (plan.nextState) { facts.state = *plan.nextState; }
	}

	void seedOwn(const QByteArray &text) {
		const auto parents = cloud.empty() ? std::vector<ConfigVersion>()
			: std::vector<ConfigVersion>{ Version(cloud.rbegin()->second) };
		const auto built = BuildConfigRecord({ text, parents, std::nullopt, facts.state->space,
			facts.state->install, facts.currentDevice, u"macOS"_q, u"test"_q,
			facts.state->config.seq + 1, uint64_t(now) });
		CHECK(built);
		const auto reserved = ReserveSyncSeq(*facts.state, SyncLocalStream::Config, built.payloadHash);
		CHECK(reserved);
		auto next = reserved.state;
		next.configData.pending = built.version.key;
		const auto issued = AppendIssuedConfigRecord(next, built.canonical);
		CHECK(issued);
		const auto confirmed = ConfirmConfigReadBack(issued.state, facts.currentDevice,
			{ OwnRecordObservationKind::Present, issued.state.config.pendingSeq, built.payloadHash }, built.version);
		CHECK(confirmed.changed);
		const auto ledger = RecordConfirmedOwnConfigMessage(confirmed.state, nextId, built.canonical);
		CHECK(ledger);
		facts.state = ledger.state;
		cloud[nextId++] = built.canonical;
		setLocal(text);
	}

	void remote(const QByteArray &text, bool successor = true) {
		const auto install = *FormatSyncInstallId(QByteArray(16, 'b'));
		const auto parents = successor ? std::vector<ConfigVersion>{ Version(cloud.rbegin()->second) }
			: std::vector<ConfigVersion>();
		const auto built = BuildConfigRecord({ text, parents, std::nullopt, facts.state->space,
			install, u"android:"_q + install, u"Android"_q, u"test"_q, 1, uint64_t(now) });
		CHECK(built);
		cloud[nextId++] = built.canonical;
	}
};

SyncAutoPlan Stage(Harness &h) {
	CHECK(h.event(h.record.enabled ? SyncAutoEvent::LocalSaved : SyncAutoEvent::Enable).action == SyncAutoAction::Check);
	const auto stage = h.inventory();
	CHECK(stage.action == SyncAutoAction::Publish);
	CHECK(InspectConfigPayload(ParseSyncEnvelope(stage.bytes)).text == h.facts.local.text);
	h.commit(stage);
	h.facts.stagedRecord = stage.bytes;
	CHECK(h.event(SyncAutoEvent::Completed, SyncAutoOutcome::Staged).action == SyncAutoAction::Check);
	return h.inventory();
}

SyncAutoPlan SendAndConfirm(Harness &h, const SyncAutoPlan &send, bool edit = false) {
	CHECK(send.action == (edit ? SyncAutoAction::EditInPlace : SyncAutoAction::Post));
	const auto id = edit ? send.messageId : h.nextId++;
	h.cloud[id] = send.bytes;
	CHECK(h.event(SyncAutoEvent::Completed, SyncAutoOutcome::Sent).action == SyncAutoAction::Reconcile);
	const auto confirm = h.inventory();
	CHECK(confirm.action == SyncAutoAction::Reconcile && confirm.commitOnly);
	h.commit(confirm);
	h.facts.stagedRecord.clear();
	CHECK(h.event(SyncAutoEvent::Completed, SyncAutoOutcome::Committed).action == SyncAutoAction::Check);
	return h.inventory();
}

void TestPublishAndEdit() {
	Section = "publish exact bytes and confirmed edit";
	auto h = Harness();
	CHECK(!h.record.enabled);
	CHECK(h.event(SyncAutoEvent::SessionReady).action == SyncAutoAction::NoOp);
	auto send = Stage(h);
	CHECK(SendAndConfirm(h, send).action == SyncAutoAction::NoOp);
	CHECK(h.facts.state->config.pendingSeq == 0);
	CHECK(h.facts.state->ownMessages.size() == 1);
	if (h.facts.state->ownMessages.empty()) { return; }
	h.setLocal(T1);
	h.facts.editSupported = true;
	send = Stage(h);
	const auto id = send.messageId;
	CHECK(SendAndConfirm(h, send, true).action == SyncAutoAction::NoOp);
	CHECK(h.cloud.size() == 1);
	CHECK(h.facts.state->ownMessages.size() == 1);
	if (h.facts.state->ownMessages.empty()) { return; }
	CHECK(h.facts.state->ownMessages[0].messageId == id);
	CHECK(h.facts.state->ownMessages[0].seq == 2);
}

void TestEditRefusalAndRetire() {
	Section = "refused edit falls back only after fresh read";
	auto h = Harness();
	h.seedOwn(T0);
	h.setLocal(T1);
	h.facts.editSupported = true;
	const auto edit = Stage(h);
	CHECK(edit.action == SyncAutoAction::EditInPlace);
	CHECK(h.event(SyncAutoEvent::Completed, SyncAutoOutcome::EditRefused).action == SyncAutoAction::Reconcile);
	const auto post = h.inventory();
	CHECK(post.action == SyncAutoAction::Post);
	const auto retire = SendAndConfirm(h, post);
	CHECK(retire.action == SyncAutoAction::Retire);
	CHECK(retire.messageId == 100);
	CHECK(h.record.retired == 1);
	CHECK(CheckOwnConfigMessageDeletion(*h.facts.state, retire.messageId, retire.bytes));
	h.cloud.erase(retire.messageId);
	h.facts.state = ParseSyncLocalState(retire.record.expectedState).state;
	CHECK(h.event(SyncAutoEvent::Completed, SyncAutoOutcome::Deleted).action == SyncAutoAction::Check);
	CHECK(h.inventory().action == SyncAutoAction::NoOp);
	CHECK(h.facts.state->ownMessages.size() == 1);
}

void TestGateAndNotices() {
	Section = "default off binding Q1 exact account and moved stream";
	for (auto test = 0; test != 11; ++test) {
		auto h = Harness();
		auto reason = SyncAutoReason::None;
		switch (test) {
		case 0: h.facts.ownerAnswered = false; reason = SyncAutoReason::OwnershipUnanswered; break;
		case 1: h.facts.fileAccount.userId++; reason = SyncAutoReason::WrongFileAccount; break;
		case 2: h.facts.fileAccount.environment = 1; reason = SyncAutoReason::WrongFileAccount; break;
		case 3: h.facts.state.reset(); reason = SyncAutoReason::AccountUnbound; break;
		case 4: h.facts.bindingToken.clear(); reason = SyncAutoReason::AccountUnbound; break;
		case 5: h.facts.state->version = 2; h.facts.state->chat = { PeerTokenKind::Channel, 123 };
			h.facts.chat = h.facts.state->chat; reason = SyncAutoReason::SyncChatNotResolved; break;
		case 6: h.facts.chatWritable = false; reason = SyncAutoReason::ChatNotWritable; break;
		case 7: h.facts.local = MakeSyncSettingsFile(SyncSettingsFileStatus::Absent); reason = SyncAutoReason::FileAbsent; break;
		case 8: h.facts.local.usingLastGood = true; reason = SyncAutoReason::LastGood; break;
		case 9: h.facts.currentDevice = u"clone"_q; reason = SyncAutoReason::CloneDetected; break;
		case 10: h.facts.signedIn = false; reason = SyncAutoReason::AccountUnavailable; break;
		}
		const auto first = h.event(SyncAutoEvent::Enable);
		CHECK(first.action == SyncAutoAction::Hold && first.reason == reason && first.notice);
		const auto repeated = h.event(SyncAutoEvent::Foreground);
		CHECK(repeated.action == SyncAutoAction::Hold && repeated.reason == reason && !repeated.notice);
	}
	auto h = Harness();
	h.facts.accountLock = false;
	CHECK(h.event(SyncAutoEvent::Enable).action == SyncAutoAction::NoOp);
	CHECK(!h.record.enabled);
	h.facts.accountLock = true;
	h.facts.inFlightSession = u"other-session"_q;
	CHECK(h.event(SyncAutoEvent::Enable).action == SyncAutoAction::NoOp);
	CHECK(h.record.queuedCheck);
}

void TestTriggers() {
	Section = "15 minute gate debounce account-local save and coalescing";
	auto h = Harness();
	h.seedOwn(T0);
	CHECK(h.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
	CHECK(h.event(SyncAutoEvent::Foreground).action == SyncAutoAction::NoOp);
	CHECK(h.record.queuedCheck);
	CHECK(h.inventory().action == SyncAutoAction::NoOp);
	h.now += kSyncAutoCheckSeconds - 1;
	CHECK(h.event(SyncAutoEvent::Unlock).action == SyncAutoAction::NoOp);
	++h.now;
	CHECK(h.event(SyncAutoEvent::SessionReady).action == SyncAutoAction::Check);
	CHECK(h.inventory().action == SyncAutoAction::NoOp);
	h.setLocal(T1);
	h.facts.saveDebounceFinished = false;
	CHECK(h.event(SyncAutoEvent::LocalSaved).action == SyncAutoAction::NoOp);
	h.facts.saveDebounceFinished = true;
	CHECK(h.event(SyncAutoEvent::LocalSaved).action == SyncAutoAction::Check);
	h.setLocal(T2);
	CHECK(h.event(SyncAutoEvent::LocalSaved).action == SyncAutoAction::NoOp);
	CHECK(h.record.queuedSave);
	const auto stage = h.inventory();
	CHECK(stage.action == SyncAutoAction::Publish);
	CHECK(InspectConfigPayload(ParseSyncEnvelope(stage.bytes)).text == T2);
	auto created = Harness();
	created.setLocal(T0, false);
	CHECK(created.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
	CHECK(created.inventory().reason == SyncAutoReason::NoSavedBytes);
}

void TestApplyAndHolds() {
	Section = "strict successor safe impact only";
	for (auto test = 0; test != 6; ++test) {
		auto h = Harness();
		h.seedOwn(test == 1 ? QByteArray("version = 1\nfuture = 42\n") : T0);
		h.remote(test == 1 ? QByteArray("version = 1\n") : test == 3 ? QByteArray("version = 2\n") : T1, test != 4);
		if (test == 2) { h.facts.proposedWorkMode = AccountWorkMode::Follow; }
		if (test == 5) { h.facts.impactContextComplete = false; }
		CHECK(h.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		const auto plan = h.inventory();
		if (!test) {
			CHECK(plan.action == SyncAutoAction::AutoApply && plan.apply && !plan.apply->join);
			CHECK(plan.bytes == T1);
			h.commit(plan);
			h.setLocal(plan.bytes, false);
			CHECK(h.event(SyncAutoEvent::Completed, SyncAutoOutcome::Applied).action == SyncAutoAction::Check);
			CHECK(h.facts.state->configData.base == Version(h.cloud.rbegin()->second).key);
		} else {
			CHECK(plan.action == SyncAutoAction::Hold);
			CHECK(plan.reason == (test == 1 || test == 5 ? SyncAutoReason::UnsafeImpact
				: test == 4 ? SyncAutoReason::Conflict : SyncAutoReason::WorkModeScopeChanged));
			CHECK(h.facts.local.text != T1);
		}
	}
}

void TestFreshReadScope() {
	Section = "fresh inventory operation account chat and ownership";
	for (auto test = 0; test != 5; ++test) {
		auto h = Harness();
		CHECK(h.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		auto input = h.input(SyncAutoEvent::Completed);
		if (test == 0) { input.inventory->account.environment = 1; }
		if (test == 1) { input.inventory->chat = { PeerTokenKind::Channel, 123 }; }
		if (test == 2) { input.inventory->ticket++; }
		if (test == 3) { input.inventory->readAt = h.now - 1; }
		if (test == 4) { input.inventory->session = u"other"_q; }
		CHECK(h.run(input).reason == SyncAutoReason::OwnershipChanged);
	}
	for (auto test = 0; test != 3; ++test) {
		auto h = Harness();
		h.seedOwn(T0);
		h.setLocal(T1);
		h.facts.editSupported = true;
		CHECK(h.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		const auto stage = h.inventory();
		h.commit(stage);
		h.facts.stagedRecord = stage.bytes;
		CHECK(h.event(SyncAutoEvent::Completed, SyncAutoOutcome::Staged).action == SyncAutoAction::Check);
		auto input = h.input(SyncAutoEvent::Completed);
		if (test == 0) { input.inventory->reads[0].owned = false; }
		if (test == 1) { input.inventory->reads[0].record += ' '; }
		if (test == 2) { input.inventory->reads[0].fresh = false; }
		CHECK(h.run(input).reason == SyncAutoReason::OwnershipChanged);
	}
}

void TestRetriesAndDuplicatePosts() {
	Section = "30 120 600 then stop and fresh reconciliation";
	auto h = Harness();
	CHECK(h.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
	for (const auto delay : { 30, 120, 600 }) {
		const auto retry = h.event(SyncAutoEvent::Completed, SyncAutoOutcome::NetworkFailure);
		CHECK(retry.action == SyncAutoAction::RetryAt && retry.retryAt == h.now + delay);
		h.now += delay - 1;
		CHECK(h.event(SyncAutoEvent::Retry).action == SyncAutoAction::NoOp);
		++h.now;
		CHECK(h.event(SyncAutoEvent::Retry).action == SyncAutoAction::Reconcile);
	}
	CHECK(h.event(SyncAutoEvent::Completed, SyncAutoOutcome::NetworkFailure).reason == SyncAutoReason::RetryStopped);
	CHECK(h.event(SyncAutoEvent::Retry).action == SyncAutoAction::NoOp);
	h.now += 900;
	CHECK(h.event(SyncAutoEvent::Foreground).action == SyncAutoAction::Reconcile);
	CHECK(h.record.retries == 0);

	auto uncertain = Harness();
	const auto post = Stage(uncertain);
	CHECK(post.action == SyncAutoAction::Post);
	uncertain.cloud[uncertain.nextId++] = post.bytes;
	uncertain.cloud[uncertain.nextId++] = post.bytes;
	const auto wait = uncertain.event(SyncAutoEvent::Completed, SyncAutoOutcome::OutcomeUnknown);
	CHECK(wait.action == SyncAutoAction::RetryAt);
	uncertain.now = wait.retryAt;
	CHECK(uncertain.event(SyncAutoEvent::Retry).action == SyncAutoAction::Reconcile);
	const auto confirm = uncertain.inventory();
	CHECK(confirm.action == SyncAutoAction::Reconcile && confirm.commitOnly);
	CHECK(confirm.nextState && confirm.nextState->config.pendingSeq == 0);
	CHECK(uncertain.cloud.size() == 2);
}

void TestRestartAndStaleCallbacks() {
	Section = "restart uncertain send and reject old callback";
	auto h = Harness();
	const auto post = Stage(h);
	h.cloud[h.nextId++] = post.bytes;
	const auto oldTicket = h.record.ticket;
	h.facts.session = u"session-b"_q;
	CHECK(h.event(SyncAutoEvent::SessionReady).action == SyncAutoAction::Reconcile);
	auto old = h.input(SyncAutoEvent::Completed, SyncAutoOutcome::Sent);
	old.facts.session = u"session-a"_q;
	old.completedTicket = oldTicket;
	CHECK(h.run(old).action == SyncAutoAction::NoOp);
	CHECK(h.inventory().commitOnly);

	Section = "interrupted apply never repeats the write";
	auto apply = Harness();
	apply.seedOwn(T0);
	apply.remote(T1);
	CHECK(apply.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
	const auto plan = apply.inventory();
	CHECK(plan.action == SyncAutoAction::AutoApply);
	apply.facts.session = u"session-b"_q;
	CHECK(apply.event(SyncAutoEvent::SessionReady).reason == SyncAutoReason::InterruptedApply);
	CHECK(apply.event(SyncAutoEvent::Foreground).reason == SyncAutoReason::InterruptedApply);
	apply.commit(plan);
	apply.setLocal(plan.bytes, false);
	CHECK(apply.event(SyncAutoEvent::SessionReady).action == SyncAutoAction::Check);
}

void TestRetirementBoundAndRefusal() {
	Section = "only own issued obsolete records bounded per run";
	auto h = Harness();
	for (auto i = 0; i != 6; ++i) { h.seedOwn(T0 + "# " + QByteArray::number(i)); }
	CHECK(h.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
	for (auto i = 0; i != kSyncAutoRetireLimit; ++i) {
		const auto retire = h.inventory();
		CHECK(retire.action == SyncAutoAction::Retire);
		CHECK(retire.messageId == 100 + i);
		h.cloud.erase(retire.messageId);
		h.facts.state = ParseSyncLocalState(retire.record.expectedState).state;
		CHECK(h.event(SyncAutoEvent::Completed, SyncAutoOutcome::Deleted).action == SyncAutoAction::Check);
	}
	CHECK(h.inventory().action == SyncAutoAction::NoOp);
	CHECK(h.cloud.size() == 2);

	auto refused = Harness();
	refused.seedOwn(T0); refused.seedOwn(T1);
	CHECK(refused.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
	CHECK(refused.inventory().action == SyncAutoAction::Retire);
	CHECK(refused.event(SyncAutoEvent::Completed, SyncAutoOutcome::DeletionRefused).reason == SyncAutoReason::DeletionRefused);
	CHECK(refused.cloud.size() == 2 && refused.facts.state->ownMessages.size() == 2);

	auto unissued = Harness();
	unissued.seedOwn(T0); unissued.seedOwn(T1);
	unissued.facts.state->config.issuedRecords.erase(unissued.facts.state->config.issuedRecords.begin());
	CHECK(unissued.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
	CHECK(unissued.inventory().reason == SyncAutoReason::OwnershipChanged);
}

void TestCrashBoundaries() {
	Section = "crash before inventory";
	auto check = Harness();
	CHECK(check.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
	check.facts.session = u"restarted"_q;
	CHECK(check.event(SyncAutoEvent::SessionReady).action == SyncAutoAction::Reconcile);
	CHECK(check.inventory().action == SyncAutoAction::Publish);
	Section = "crash at stage boundaries";
	for (auto stageWritten = 0; stageWritten != 2; ++stageWritten) {
		auto h = Harness();
		CHECK(h.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		const auto stage = h.inventory();
		CHECK(stage.action == SyncAutoAction::Publish);
		if (stageWritten) { h.commit(stage); h.facts.stagedRecord = stage.bytes; }
		h.facts.session = u"restarted"_q;
		CHECK(h.event(SyncAutoEvent::SessionReady).action == SyncAutoAction::Reconcile);
		const auto next = h.inventory();
		CHECK(next.action == (stageWritten ? SyncAutoAction::Post : SyncAutoAction::Publish));
		CHECK(h.cloud.empty());
	}
	Section = "crash at confirmation commit boundary";
	for (auto committed = 0; committed != 2; ++committed) {
		auto h = Harness();
		const auto post = Stage(h);
		h.cloud[h.nextId++] = post.bytes;
		CHECK(h.event(SyncAutoEvent::Completed, SyncAutoOutcome::Sent).action == SyncAutoAction::Reconcile);
		const auto confirm = h.inventory();
		CHECK(confirm.commitOnly);
		if (committed) { h.commit(confirm); h.facts.stagedRecord.clear(); }
		h.facts.session = u"restarted"_q;
		CHECK(h.event(SyncAutoEvent::SessionReady).action == SyncAutoAction::Reconcile);
		const auto next = h.inventory();
		CHECK(next.action == (committed ? SyncAutoAction::NoOp : SyncAutoAction::Reconcile));
		CHECK(committed || next.commitOnly);
		CHECK(h.cloud.size() == 1);
	}
	Section = "crash at retirement and ledger commit boundaries";
	for (auto committed = 0; committed != 2; ++committed) {
		auto h = Harness();
		h.seedOwn(T0); h.seedOwn(T1);
		CHECK(h.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		const auto retire = h.inventory();
		CHECK(retire.action == SyncAutoAction::Retire);
		h.cloud.erase(retire.messageId);
		if (committed) { h.facts.state = ParseSyncLocalState(retire.record.expectedState).state; }
		h.facts.session = u"restarted"_q;
		CHECK(h.event(SyncAutoEvent::SessionReady).action == SyncAutoAction::Reconcile);
		const auto next = h.inventory();
		CHECK(next.action == (committed ? SyncAutoAction::NoOp : SyncAutoAction::Reconcile));
		CHECK(committed || next.commitOnly);
		CHECK(h.cloud.size() == 1);
	}
}

void TestCloneRewindAndRoleChange() {
	Section = "remote clone rewind and head deletion";
	for (auto test = 0; test != 4; ++test) {
		auto h = Harness();
		h.seedOwn(T0);
		if (test == 0) {
			const auto saved = *h.facts.state;
			h.seedOwn(T1);
			h.facts.state = saved;
			h.setLocal(T0);
		} else if (test == 1) {
			h.seedOwn(T1);
			h.cloud.erase(101);
		} else if (test == 2) {
			h.cloud.clear();
		} else {
			const auto foreign = BuildConfigRecord({ T1, {}, std::nullopt, h.facts.state->space,
				h.facts.state->install, u"cloned-device"_q, u"macOS"_q, u"test"_q, 1, uint64_t(h.now) });
			CHECK(foreign);
			h.cloud[100] = foreign.canonical;
		}
		CHECK(h.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		const auto held = h.inventory();
		CHECK(held.action == SyncAutoAction::Hold);
		CHECK(held.reason == (test == 0 || test == 3 ? SyncAutoReason::CloneDetected : SyncAutoReason::NeedsReview));
	}
	Section = "role change and same session manual in-flight work";
	auto h = Harness();
	const auto post = Stage(h);
	h.facts.chatWritable = false;
	CHECK(h.event(SyncAutoEvent::Completed, SyncAutoOutcome::Sent).reason == SyncAutoReason::ChatNotWritable);
	CHECK(h.cloud.empty());
	auto busy = Harness();
	busy.facts.inFlightSession = busy.facts.session;
	CHECK(busy.event(SyncAutoEvent::Enable).action == SyncAutoAction::NoOp);
	CHECK(busy.record.queuedCheck);
	busy.facts.inFlightSession.clear();
	CHECK(busy.event(SyncAutoEvent::Foreground).action == SyncAutoAction::Check);
	Section = "legacy send switch never opts in modern sync";
	auto legacy = Harness();
	legacy.setLocal(QByteArray("version = 1\n[sync]\nsend_after_save_p = true\n"));
	CHECK(legacy.event(SyncAutoEvent::LocalSaved).action == SyncAutoAction::NoOp);
	CHECK(!legacy.record.enabled);
}

void TestBoundedPlansAndExactCompletion() {
	Section = "large valid state cannot create an unpersistable action";
	auto large = Harness();
	large.facts.state->preserved.insert(u"future"_q, QString(1100000, QChar('x')));
	CHECK(SerializeSyncLocalState(*large.facts.state));
	CHECK(large.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
	const auto held = large.inventory();
	CHECK(held.action == SyncAutoAction::Hold && held.reason == SyncAutoReason::StoreMismatch);
	CHECK(SerializeSyncAutoRecord(held.record));
	CHECK(large.facts.state->config.pendingSeq == 0 && large.facts.stagedRecord.isEmpty());

	Section = "stage completion requires exact bytes";
	auto mismatch = Harness();
	CHECK(mismatch.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
	const auto stage = mismatch.inventory();
	CHECK(stage.action == SyncAutoAction::Publish);
	mismatch.commit(stage);
	mismatch.facts.stagedRecord = stage.bytes + ' ';
	CHECK(mismatch.event(SyncAutoEvent::Completed, SyncAutoOutcome::Staged).reason == SyncAutoReason::StateChanged);
	CHECK(mismatch.cloud.empty());

	Section = "state v2 self is eligible";
	auto self = Harness();
	self.facts.state->version = 2;
	CHECK(Stage(self).action == SyncAutoAction::Post);

	Section = "clock rollback and exhausted counters are safe";
	auto clock = Harness();
	clock.seedOwn(T0);
	CHECK(clock.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
	CHECK(clock.inventory().action == SyncAutoAction::NoOp);
	--clock.now;
	CHECK(clock.event(SyncAutoEvent::Foreground).action == SyncAutoAction::NoOp);
	auto exhausted = Harness();
	exhausted.record.ticket = std::numeric_limits<uint64_t>::max();
	CHECK(exhausted.event(SyncAutoEvent::Enable).reason == SyncAutoReason::InvalidRecord);
}


void TestBackoffBudgetAcrossTriggers() {
	Section = "backoff coalesces every trigger across sessions without resetting budget";
	auto h = Harness();
	const auto post = Stage(h);
	CHECK(post.action == SyncAutoAction::Post);
	auto count = 0;
	for (const auto delay : { 30, 120, 600 }) {
		const auto wait = h.event(SyncAutoEvent::Completed, SyncAutoOutcome::OutcomeUnknown);
		CHECK(wait.action == SyncAutoAction::RetryAt && wait.retryAt == h.now + delay);
		CHECK(wait.record.retries == ++count);
		++h.now;
		h.facts.session = QString::number(count);
		for (const auto event : { SyncAutoEvent::Enable, SyncAutoEvent::SessionReady,
			SyncAutoEvent::Foreground, SyncAutoEvent::Unlock, SyncAutoEvent::LocalSaved }) {
			h.setLocal(T1);
			const auto coalesced = h.event(event);
			CHECK(coalesced.action == SyncAutoAction::NoOp);
			CHECK(coalesced.record.retryAt == wait.retryAt && coalesced.record.retries == count);
		}
		CHECK(h.record.queuedCheck && h.record.queuedSave);
		CHECK(h.record.savedFingerprint == h.facts.local.fingerprint);
		h.now = wait.retryAt - 1;
		CHECK(h.event(SyncAutoEvent::Retry).action == SyncAutoAction::NoOp);
		h.now = wait.retryAt;
		CHECK(h.event(SyncAutoEvent::Foreground).action == SyncAutoAction::NoOp);
		CHECK(h.event(SyncAutoEvent::Retry).action == SyncAutoAction::Reconcile);
		CHECK(h.record.retries == count);
	}
	CHECK(h.event(SyncAutoEvent::Completed, SyncAutoOutcome::NetworkFailure).reason == SyncAutoReason::RetryStopped);
	++h.now;
	CHECK(h.event(SyncAutoEvent::Foreground).action == SyncAutoAction::Reconcile);
	CHECK(h.record.retries == 0);
	const auto retry = h.event(SyncAutoEvent::Completed, SyncAutoOutcome::NetworkFailure);
	CHECK(retry.retryAt == h.now + 30 && retry.record.retries == 1);
	h.now = retry.retryAt;
	CHECK(h.event(SyncAutoEvent::Retry).action == SyncAutoAction::Reconcile);
	const auto next = h.inventory();
	CHECK(next.action == SyncAutoAction::Post && next.bytes == post.bytes);
	CHECK(next.record.retries == 0 && next.record.queuedSave);
	const auto restartedBudget = h.event(SyncAutoEvent::Completed, SyncAutoOutcome::NetworkFailure);
	CHECK(restartedBudget.retryAt == h.now + 30 && restartedBudget.record.retries == 1);
	CHECK(h.event(SyncAutoEvent::Disable).action == SyncAutoAction::NoOp);
	CHECK(!h.record.enabled && h.record.retryAt == restartedBudget.retryAt);
}

void TestRetirementExactPresence() {
	Section = "retirement needs affirmative exact id absence and preserves unknown ledger";
	for (auto test = 0; test != 10; ++test) {
		auto h = Harness();
		h.seedOwn(T0); h.seedOwn(T1);
		CHECK(h.event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		const auto retire = h.inventory();
		CHECK(retire.action == SyncAutoAction::Retire);
		const auto wait = h.event(SyncAutoEvent::Completed, SyncAutoOutcome::OutcomeUnknown);
		h.now = wait.retryAt;
		CHECK(h.event(SyncAutoEvent::Retry).action == SyncAutoAction::Reconcile);
		if (test != 6) { h.cloud.erase(retire.messageId); }
		auto input = h.input(SyncAutoEvent::Completed);
		auto &reads = input.inventory->reads;
		auto found = std::find_if(reads.begin(), reads.end(), [&](const auto &read) {
			return read.messageId == retire.messageId;
		});
		CHECK(found != reads.end());
		if (found == reads.end()) { continue; }
		if (test == 0) { reads.erase(found); }
		if (test == 1) { found->presence = SyncAutoMessagePresence::Unknown; }
		if (test == 2) { found->fresh = false; }
		if (test == 3) { found->record = "not absent"; }
		if (test == 4 || test == 5 || test == 9) {
			found->presence = SyncAutoMessagePresence::Present;
			found->owned = test != 9;
			found->record = test == 4 ? QByteArray("existing unrelated document") : retire.bytes;
		}
		if (test == 6) { found->presence = SyncAutoMessagePresence::Absent; found->record.clear(); }
		if (test == 7) { const auto duplicate = *found; reads.push_back(duplicate); }
		const auto result = h.run(input);
		if (test < 3 || test == 7) {
			CHECK(result.action == SyncAutoAction::RetryAt && result.retryAt == h.now + 120);
			CHECK(result.record.retries == 2);
		} else if (test == 8) {
			CHECK(result.action == SyncAutoAction::Reconcile && result.commitOnly);
			CHECK(result.nextState && result.nextState->ownMessages.size() == 1);
			CHECK(result.record.retries == 0);
		} else if (test == 5) {
			CHECK(result.action == SyncAutoAction::Retire && result.record.retired == 2);
		} else {
			CHECK(result.action == SyncAutoAction::Hold && result.reason == SyncAutoReason::OwnershipChanged);
		}
		if (test != 8) {
			CHECK(!result.commitOnly && !result.nextState);
			CHECK(h.facts.state->ownMessages.size() == 2);
		}
	}
}

void TriggerDuringBackoff() {
 Section="lifecycle trigger must coalesce during backoff";
 auto h=Harness();
 const auto post=Stage(h);CHECK(post.action==SyncAutoAction::Post);
 const auto failure=h.event(SyncAutoEvent::Completed,SyncAutoOutcome::OutcomeUnknown);
 CHECK(failure.action==SyncAutoAction::RetryAt);
 const auto deadline=failure.retryAt;
 ++h.now;
 const auto triggered=h.event(SyncAutoEvent::Foreground);
 std::printf("backoff: now=%lld requiredDeadline=%lld action=%d retries=%d retryAt=%lld\n",(long long)h.now,(long long)deadline,int(triggered.action),triggered.record.retries,(long long)triggered.record.retryAt);
 CHECK(triggered.action==SyncAutoAction::NoOp);
 CHECK(triggered.record.retryAt==deadline);
 CHECK(triggered.record.retries==1);
 if(triggered.action==SyncAutoAction::Reconcile) {
  const auto again=h.event(SyncAutoEvent::Completed,SyncAutoOutcome::NetworkFailure);
  std::printf("next failure after trigger: delay=%lld retries=%d\n",(long long)(again.retryAt-h.now),again.record.retries);
  CHECK(again.retryAt==h.now+120);
 }
}

void SaveDuringBackoff() {
 Section="settled save must coalesce during check backoff";
 auto h=Harness();
 CHECK(h.event(SyncAutoEvent::Enable).action==SyncAutoAction::Check);
 const auto failure=h.event(SyncAutoEvent::Completed,SyncAutoOutcome::NetworkFailure);
 const auto deadline=failure.retryAt;
 ++h.now;h.setLocal(T1);
 const auto triggered=h.event(SyncAutoEvent::LocalSaved);
 std::printf("save during backoff: now=%lld requiredDeadline=%lld action=%d retries=%d\n",(long long)h.now,(long long)deadline,int(triggered.action),triggered.record.retries);
 CHECK(triggered.action==SyncAutoAction::NoOp);
 CHECK(triggered.record.retryAt==deadline);
 CHECK(triggered.record.queuedSave);
}

void RetireUncertainNoncandidate() {
 Section="uncertain retirement requires exact absence";
 auto h=Harness();h.seedOwn(T0);h.seedOwn(T1);
 CHECK(h.event(SyncAutoEvent::Enable).action==SyncAutoAction::Check);
 const auto retire=h.inventory();CHECK(retire.action==SyncAutoAction::Retire);
 const auto waiting=h.event(SyncAutoEvent::Completed,SyncAutoOutcome::OutcomeUnknown);
 CHECK(waiting.action==SyncAutoAction::RetryAt);
 h.now=waiting.retryAt;CHECK(h.event(SyncAutoEvent::Retry).action==SyncAutoAction::Reconcile);
 const auto id=retire.messageId;
 h.cloud[id]=QByteArray("existing unrelated document");
 const auto meta=SyncHistoryMessageMeta{true,false,true,u"manual replacement"_q,{u"unrelated.txt"_q}};
 CHECK(!IsSyncHistoryCandidate(meta));
 auto input=h.input(SyncAutoEvent::Completed);
 auto &inventory=*input.inventory;
 auto &candidates=inventory.inventory.scan.candidateIds;
 candidates.erase(std::remove(candidates.begin(),candidates.end(),id),candidates.end());
 auto &records=inventory.inventory.read->records;
 records.erase(std::remove_if(records.begin(),records.end(),[&](const auto &record){return record.id==id;}),records.end());
 inventory.inventory.read->status=AggregateSyncCandidateRead(records);
 inventory.inventory=FinishSyncAccountInventory(uint64_t(h.facts.account.userId),inventory.inventory.scan,*inventory.inventory.read);
 CHECK(inventory.inventory.status==SyncAccountInventoryStatus::Complete);
 for(const auto &read:inventory.reads)if(read.messageId==id) CHECK(read.fresh && read.owned && read.record==h.cloud[id]);
 const auto plan=h.run(input);
 std::printf("uncertain retire existing noncandidate: action=%d reason=%d commitOnly=%d ledger=%zu -> %zu\n",int(plan.action),int(plan.reason),plan.commitOnly,h.facts.state->ownMessages.size(),plan.nextState?plan.nextState->ownMessages.size():h.facts.state->ownMessages.size());
 CHECK(!(plan.commitOnly && plan.nextState && plan.nextState->ownMessages.size()<h.facts.state->ownMessages.size()));
}

void SafeAndHeldBoundaries() {
 Section="safe successor with a real value change";
 auto safe=Harness();
 safe.seedOwn(QByteArray("version=1\n[last_seen]\nreasons_p=true\n"));
 safe.remote(QByteArray("version=1\n[last_seen]\nreasons_p=false\n"));
 CHECK(safe.event(SyncAutoEvent::Enable).action==SyncAutoAction::Check);
 CHECK(safe.inventory().action==SyncAutoAction::AutoApply);
 Section="losses and incomplete context never auto-apply";
 for(int test=0;test!=5;++test) {
  auto h=Harness();
  QByteArray before,after;
  if(test==0) {before="version=1\n[lists.work]\nmembers=[1]\n";after="version=1\n[lists.work]\nmembers=[]\n";}
  else if(test==1) {before="version=1\nfuture_unknown=42\n";after="version=1\n";}
  else if(test==2) {before="version=1\n[[schedule.rules]]\nfrom='11:00'\ntill='12:00'\npreset='work'\n";after=before;after.replace("12:00","13:00");}
  else {before=T0;after=T1;}
  h.seedOwn(before);h.remote(after);
  if(test==3) {h.facts.impactContext.role=ConfigImpactFileRole::Layered;h.facts.impactContext.global={QByteArray("[invalid"),true};}
  if(test==4) h.setLocal(T2);
  CHECK(h.event(SyncAutoEvent::Enable).action==SyncAutoAction::Check);
  const auto plan=h.inventory();
  CHECK(plan.action==SyncAutoAction::Hold);
  CHECK(plan.reason==(test==4?SyncAutoReason::Conflict:SyncAutoReason::UnsafeImpact));
 }
 Section="unknown send found after fresh read never posts again";
 auto sent=Harness();const auto post=Stage(sent);
 sent.cloud[sent.nextId++]=post.bytes;
 const auto retry=sent.event(SyncAutoEvent::Completed,SyncAutoOutcome::OutcomeUnknown);
 sent.now=retry.retryAt;CHECK(sent.event(SyncAutoEvent::Retry).action==SyncAutoAction::Reconcile);
 const auto found=sent.inventory();CHECK(found.commitOnly && found.action==SyncAutoAction::Reconcile);
 CHECK(sent.cloud.size()==1);
 Section="future codec exact counters and unknown metadata";
 auto h=Harness();h.record.run=std::numeric_limits<uint64_t>::max();
 h.record.preserved=QJsonObject{{u"future"_q,QJsonArray{u"keep"_q,17,QJsonObject{{u"inner"_q,true}}}}};
 const auto bytes=SerializeSyncAutoRecord(h.record);CHECK(bytes);
 const auto parsed=ParseSyncAutoRecord(*bytes);CHECK(parsed.record && *parsed.record==h.record);
 auto object=QJsonDocument::fromJson(*bytes).object();
 for(const auto &counter:{u"run"_q,u"ticket"_q,u"event"_q}) {
  auto bad=object;bad.insert(counter,123);const auto raw=QJsonDocument(bad).toJson();
  auto decode=ParseSyncAutoRecord(raw);CHECK(decode.status==SyncAutoCodecStatus::Invalid && !decode.record && decode.opaque==raw);
  bad=object;bad.insert(counter,u"01"_q);CHECK(ParseSyncAutoRecord(QJsonDocument(bad).toJson()).status==SyncAutoCodecStatus::Invalid);
 }
 object.insert(u"version"_q,qint64(9223372036854775807LL));
 const auto raw=QJsonDocument(object).toJson();
 const auto future=ParseSyncAutoRecord(raw);CHECK(future.status!=SyncAutoCodecStatus::Valid && !future.record && future.opaque==raw);
 Section="account environment mismatch cannot execute";
 auto different=Harness();auto input=different.input(SyncAutoEvent::Enable);input.facts.account.environment=1;
 CHECK(PlanSyncAuto(input).action==SyncAutoAction::Hold);
 CHECK(!PlanSyncAuto(input).record.enabled);
}

void RecheckTerminalBudgetAfterGateAndDisable() {
 Section="terminal retry stop survives gating and opt-out before a new trigger";
 for(int test=0;test!=2;++test) {
  auto h=Harness();CHECK(Stage(h).action==SyncAutoAction::Post);
  for(const auto delay:{30,120,600}) {
   const auto wait=h.event(SyncAutoEvent::Completed,SyncAutoOutcome::OutcomeUnknown);
   CHECK(wait.action==SyncAutoAction::RetryAt && wait.retryAt==h.now+delay);
   h.now=wait.retryAt;CHECK(h.event(SyncAutoEvent::Retry).action==SyncAutoAction::Reconcile);
  }
  CHECK(h.event(SyncAutoEvent::Completed,SyncAutoOutcome::NetworkFailure).reason==SyncAutoReason::RetryStopped);
  if(test==0) {
   h.facts.signedIn=false;
   CHECK(h.event(SyncAutoEvent::Foreground).reason==SyncAutoReason::AccountUnavailable);
   h.facts.signedIn=true;
  } else {
   CHECK(h.event(SyncAutoEvent::Disable).action==SyncAutoAction::NoOp);
   CHECK(!h.record.enabled);
  }
  const auto resumed=h.event(test==0?SyncAutoEvent::SessionReady:SyncAutoEvent::Enable);
  std::printf("terminal resume test=%d action=%d retries=%d held=%d\n",test,int(resumed.action),resumed.record.retries,int(resumed.record.held));
  CHECK(resumed.action==SyncAutoAction::Reconcile);
  CHECK(resumed.record.retries==0);
  const auto retry=h.event(SyncAutoEvent::Completed,SyncAutoOutcome::NetworkFailure);
  std::printf("terminal resume failure test=%d action=%d retries=%d reason=%d delay=%lld\n",test,int(retry.action),retry.record.retries,int(retry.reason),(long long)(retry.retryAt-h.now));
  CHECK(retry.action==SyncAutoAction::RetryAt && retry.retryAt==h.now+30 && retry.record.retries==1);
 }
}


void ExhaustRetryRun(Harness &h) {
	CHECK(Stage(h).action == SyncAutoAction::Post);
	for (const auto delay : { 30, 120, 600 }) {
		const auto wait = h.event(SyncAutoEvent::Completed, SyncAutoOutcome::OutcomeUnknown);
		CHECK(wait.action == SyncAutoAction::RetryAt && wait.retryAt == h.now + delay);
		h.now = wait.retryAt;
		CHECK(h.event(SyncAutoEvent::Retry).action == SyncAutoAction::Reconcile);
	}
	CHECK(h.event(SyncAutoEvent::Completed, SyncAutoOutcome::NetworkFailure).reason == SyncAutoReason::RetryStopped);
	CHECK(h.record.runStopped && h.record.retries == 3 && h.record.retryAt == 0);
}

void TestStoppedRunSurvivesInterveningTransitions() {
	Section = "terminal run state survives holds opt-out restart and deferred triggers";
	for (auto test = 0; test != 6; ++test) {
		auto h = Harness();
		ExhaustRetryRun(h);
		const auto stoppedRun = h.record.run;
		if (test == 0) {
			h.facts.local.usingLastGood = true;
			CHECK(h.event(SyncAutoEvent::Foreground).reason == SyncAutoReason::LastGood);
			h.facts.local.usingLastGood = false;
		} else if (test == 1) {
			CHECK(h.event(SyncAutoEvent::Disable).action == SyncAutoAction::NoOp);
			CHECK(!h.record.enabled);
			CHECK(h.event(SyncAutoEvent::SessionReady).action == SyncAutoAction::NoOp);
		} else if (test == 2) {
			h.facts.session = u"restarted-stopped-run"_q;
			h.facts.signedIn = false;
			CHECK(h.event(SyncAutoEvent::SessionReady).reason == SyncAutoReason::AccountUnavailable);
			h.facts.signedIn = true;
		} else if (test == 3) {
			h.facts.chatWritable = false;
			CHECK(h.event(SyncAutoEvent::Foreground).reason == SyncAutoReason::ChatNotWritable);
			h.facts.ownerAnswered = false;
			CHECK(h.event(SyncAutoEvent::Foreground).reason == SyncAutoReason::OwnershipUnanswered);
			h.facts.chatWritable = h.facts.ownerAnswered = true;
		} else if (test == 4) {
			h.facts.accountLock = false;
			CHECK(h.event(SyncAutoEvent::Foreground).action == SyncAutoAction::NoOp);
			h.facts.accountLock = true;
		} else {
			h.facts.inFlightSession = u"manual"_q;
			CHECK(h.event(SyncAutoEvent::Foreground).action == SyncAutoAction::NoOp);
			h.facts.inFlightSession.clear();
		}
		const auto bytes = SerializeSyncAutoRecord(h.record);
		CHECK(bytes);
		if (!bytes) { continue; }
		const auto parsed = ParseSyncAutoRecord(*bytes);
		CHECK(parsed.record && parsed.record->runStopped && parsed.record->run == stoppedRun);
		if (!parsed.record) { continue; }
		h.record = *parsed.record;
		h.facts.session = u"another-restart"_q;
		CHECK(h.event(SyncAutoEvent::Retry).action == SyncAutoAction::NoOp);
		CHECK(h.record.runStopped && h.record.retries == 3 && h.record.run == stoppedRun);
		const auto fresh = h.event(test == 1 ? SyncAutoEvent::Enable : SyncAutoEvent::SessionReady);
		CHECK(fresh.action == SyncAutoAction::Reconcile);
		CHECK(!fresh.record.runStopped && fresh.record.run == stoppedRun + 1 && fresh.record.retries == 0);
		for (const auto delay : { 30, 120, 600 }) {
			const auto retry = h.event(SyncAutoEvent::Completed, SyncAutoOutcome::NetworkFailure);
			CHECK(retry.action == SyncAutoAction::RetryAt && retry.retryAt == h.now + delay);
			h.now = retry.retryAt;
			CHECK(h.event(SyncAutoEvent::Retry).action == SyncAutoAction::Reconcile);
		}
		CHECK(h.event(SyncAutoEvent::Completed, SyncAutoOutcome::NetworkFailure).reason == SyncAutoReason::RetryStopped);
		CHECK(h.record.runStopped && h.record.run == stoppedRun + 1);
	}
}

void TestHoldsAndDisablePreserveActiveBackoff() {
	Section = "gate holds and disable suspend an active run without resetting backoff";
	auto h = Harness();
	CHECK(Stage(h).action == SyncAutoAction::Post);
	const auto run = h.record.run;
	auto count = 0;
	for (const auto delay : { 30, 120, 600 }) {
		const auto wait = h.event(SyncAutoEvent::Completed, SyncAutoOutcome::OutcomeUnknown);
		CHECK(wait.action == SyncAutoAction::RetryAt && wait.retryAt == h.now + delay);
		++count;
		h.facts.signedIn = false;
		CHECK(h.event(SyncAutoEvent::Foreground).action == SyncAutoAction::NoOp);
		h.now = wait.retryAt;
		CHECK(h.event(SyncAutoEvent::Retry).reason == SyncAutoReason::AccountUnavailable);
		CHECK(h.record.retryAt == wait.retryAt && h.record.retries == count && !h.record.runStopped);
		CHECK(h.event(SyncAutoEvent::Disable).action == SyncAutoAction::NoOp);
		CHECK(!h.record.enabled && h.record.retryAt == wait.retryAt && h.record.retries == count);
		h.facts.session = u"restart-during-backoff"_q + QString::number(count);
		CHECK(h.event(SyncAutoEvent::SessionReady).action == SyncAutoAction::NoOp);
		CHECK(h.event(SyncAutoEvent::Enable).action == SyncAutoAction::NoOp);
		CHECK(h.event(SyncAutoEvent::Retry).reason == SyncAutoReason::AccountUnavailable);
		CHECK(h.record.retryAt == wait.retryAt && h.record.retries == count && h.record.run == run);
		h.facts.signedIn = true;
		CHECK(h.event(SyncAutoEvent::Foreground).action == SyncAutoAction::NoOp);
		CHECK(h.event(SyncAutoEvent::Retry).action == SyncAutoAction::Reconcile);
		CHECK(h.record.retries == count && h.record.run == run && !h.record.runStopped);
	}
	CHECK(h.event(SyncAutoEvent::Completed, SyncAutoOutcome::NetworkFailure).reason == SyncAutoReason::RetryStopped);
	CHECK(h.record.runStopped && h.record.retries == 3);
}

void TestCodec() {
	Section = "per-account codec exact integers unknown fields and refusal";
	auto h = Harness();
	h.record.event = std::numeric_limits<uint64_t>::max();
	h.record.ticket = std::numeric_limits<uint64_t>::max();
	h.record.preserved.insert(u"future"_q, QJsonObject{ { u"x"_q, u"keep"_q } });
	const auto bytes = SerializeSyncAutoRecord(h.record);
	CHECK(bytes);
	const auto parsed = ParseSyncAutoRecord(*bytes);
	CHECK(parsed.record && *parsed.record == h.record);
	CHECK(bytes->contains("18446744073709551615"));
	auto object = QJsonDocument::fromJson(*bytes).object();
	for (const auto version : { qint64(2), qint64(2147483648LL) }) {
		auto changed = object;
		changed.insert(u"version"_q, version);
		const auto raw = QJsonDocument(changed).toJson();
		const auto future = ParseSyncAutoRecord(raw);
		CHECK(future.status == SyncAutoCodecStatus::NewerVersion && !future.record && future.opaque == raw);
	}
	for (const auto &key : object.keys()) {
		if (key == u"future"_q) { continue; }
		auto broken = object;
		broken.remove(key);
		CHECK(ParseSyncAutoRecord(QJsonDocument(broken).toJson()).status == SyncAutoCodecStatus::Invalid);
	}
	CHECK(ParseSyncAutoRecord(QByteArray("{\"version\":1,\"version\":1}" )).status == SyncAutoCodecStatus::Invalid);
	CHECK(ParseSyncAutoRecord(QByteArray(kSyncAutoRecordLimit + 1, ' ')).status == SyncAutoCodecStatus::Invalid);
}

}

int main() {
	TestPublishAndEdit();
	TestEditRefusalAndRetire();
	TestGateAndNotices();
	TestTriggers();
	TestApplyAndHolds();
	TestFreshReadScope();
	TestRetriesAndDuplicatePosts();
	TestRestartAndStaleCallbacks();
	TestRetirementBoundAndRefusal();
	TestCrashBoundaries();
	TestCloneRewindAndRoleChange();
	TestBoundedPlansAndExactCompletion();
	TestBackoffBudgetAcrossTriggers();
	TestRetirementExactPresence();
	TriggerDuringBackoff();
	SaveDuringBackoff();
	RetireUncertainNoncandidate();
	SafeAndHeldBoundaries();
	RecheckTerminalBudgetAfterGateAndDisable();
	TestStoppedRunSurvivesInterveningTransitions();
	TestHoldsAndDisablePreserveActiveBackoff();
	TestCodec();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
