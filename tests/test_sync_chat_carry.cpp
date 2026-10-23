/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_chat_carry.h"
#include "purple/purple_sync_chat_journal.h"
#include "purple/purple_sync_json.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>

using namespace Purple;

namespace {

auto Checks = 0;
auto Failures = 0;
const char *Section = "";
const auto Account = AccountKey{ 0, 777 };

bool Check(bool good, const char *condition, int line) {
	++Checks;
	if (!good) {
		++Failures;
		std::printf("FAIL %s:%d %s\n", Section, line, condition);
		std::fflush(stdout);
	}
	return good;
}

#define CHECK(condition) Check(bool(condition), #condition, __LINE__)
#define REQUIRE(condition) do { if (!CHECK(condition)) { std::exit(1); } } while (false)

PeerToken Peer(const QString &text) {
	const auto peer = ParsePeerToken(text);
	REQUIRE(peer);
	return *peer;
}

SyncChatEligibilityResult Eligible(const PeerToken &peer = PeerToken()) {
	auto facts = SyncChatFacts();
	facts.kind = peer.kind == PeerTokenKind::Self
		? SyncChatKind::Self : peer.kind == PeerTokenKind::Channel
		? SyncChatKind::Supergroup : SyncChatKind::BasicGroup;
	facts.canSendFiles = true;
	facts.otherMembers = 0;
	facts.otherAdmins = 0;
	return SyncChatEligibility(facts, SyncChatBroadcastOwnership::Unselected);
}

QByteArray StateBytes(const SyncLocalState &state) {
	const auto result = SerializeSyncLocalState(state);
	REQUIRE(result);
	return result.canonical;
}

struct Cloud {
	std::map<QString, std::vector<SyncCarryMessage>> messages;
	std::map<QString, int32_t> nextId;
	std::map<QString, SyncCarryMarkReadback> marks;
	std::map<QString, SyncCarryMarkReadback> requests;
	int64_t clock = 1000;

	QString key(const PeerToken &peer) const {
		return *PeerTokenText(peer);
	}

	int32_t allocate(const PeerToken &peer) {
		auto &next = nextId[key(peer)];
		if (!next) {
			next = 1;
		}
		return next++;
	}

	SyncCarryMessage post(
			const PeerToken &peer,
			const QByteArray &bytes,
			SyncCarryFamily family = SyncCarryFamily::Settings,
			SyncChatMessageOwnership ownership = SyncChatMessageOwnership::Own) {
		const auto result = MakeSyncCarryMessage(Account, peer, allocate(peer), ++clock,
			bytes, ownership, family);
		REQUIRE(result);
		messages[key(peer)].push_back(*result);
		return *result;
	}

	SyncCarryMarkReadback mark(
			const PeerToken &peer,
			const QString &text = u"#purplechatsync v1"_q,
			const QString &request = {}) {
		if (!request.isEmpty() && requests.contains(request)) {
			return requests.at(request);
		}
		auto result = SyncCarryMarkReadback();
		result.outcome = SyncCarryMarkOutcome::Valid;
		result.account = Account;
		result.chat = peer;
		result.messageId = allocate(peer);
		result.date = ++clock;
		result.text = text;
		result.ownership = SyncChatMessageOwnership::Own;
		marks[key(peer)] = result;
		if (!request.isEmpty()) {
			requests[request] = result;
		}
		return result;
	}

	SyncChatCache cache(const PeerToken &peer) const {
		const auto &mark = marks.at(key(peer));
		return { peer, mark.messageId, mark.date };
	}

	SyncCarryInventory inventory(const PeerToken &peer, bool complete = true) const {
		const auto i = messages.find(key(peer));
		return { Account, peer, complete,
			i == messages.end() ? std::vector<SyncCarryMessage>() : i->second };
	}

	bool has(const PeerToken &peer, const QString &hash) const {
		const auto snapshot = inventory(peer);
		return std::any_of(snapshot.messages.begin(), snapshot.messages.end(), [&](const auto &message) {
			return message.reference.recordHash == hash && message.ownership == SyncChatMessageOwnership::Own;
		});
	}

	std::optional<SyncCarryMessage> find(const SyncCarryReference &reference) const {
		const auto snapshot = inventory(reference.chat);
		const auto i = std::find_if(snapshot.messages.begin(), snapshot.messages.end(), [&](const auto &message) {
			return message.reference == reference;
		});
		return i == snapshot.messages.end() ? std::nullopt : std::optional(*i);
	}

	bool forward(const SyncCarryReference &reference, const PeerToken &target) {
		const auto message = find(reference);
		if (!message) {
			return false;
		}
		post(target, message->bytes, message->family);
		return true;
	}

	void remove(const SyncCarryReference &reference) {
		auto &records = messages[key(reference.chat)];
		records.erase(std::remove_if(records.begin(), records.end(), [&](const auto &record) {
			return record.reference == reference;
		}), records.end());
	}
};

struct Device {
	SyncLocalState state;
	SyncChatCache cache;
	QByteArray baseText;
	QByteArray localText;
	ConfigVersion baseVersion;
	QByteArray stage;
	std::map<QString, QByteArray> images;
	std::optional<SyncCarryCacheReceipt> pendingCache;

	Device(char seed, const PeerToken &chat, const SyncChatCache &initial) : cache(initial) {
		state.install = *FormatSyncInstallId(QByteArray(16, seed));
		state.space = *FormatSyncSpaceId(QByteArray(16, 's'));
		state.createdDevice = u"test-%1"_q.arg(QChar(seed));
		state.bindingToken = *FormatSyncBindingToken(QByteArray(16, seed));
		state.chat = chat;
		state.version = chat == PeerToken() ? 1 : 2;
	}

	void prepare(const QByteArray &text) {
		const auto parents = baseVersion.key.isEmpty()
			? std::vector<ConfigVersion>() : std::vector{ baseVersion };
		const auto version = MakeConfigVersion(text, parents);
		REQUIRE(version);
		const auto record = BuildConfigRecord({
			.text = text,
			.version = *version,
			.space = state.space,
			.install = state.install,
			.device = state.createdDevice,
			.platform = u"test"_q,
			.app = u"Carry harness"_q,
			.seq = state.config.seq + 1,
			.at = 1000 + state.config.seq + 1,
		});
		REQUIRE(record);
		auto reserved = ReserveSyncSeq(state, SyncLocalStream::Config, record.payloadHash);
		REQUIRE(reserved);
		reserved.state.configData.pending = version->key;
		const auto issued = AppendIssuedConfigRecord(reserved.state, record.canonical);
		REQUIRE(issued);
		state = issued.state;
		stage = record.canonical;
		localText = text;
	}

	SyncCarryMessage publish(Cloud &cloud, const QByteArray &text) {
		prepare(text);
		const auto message = cloud.post(state.chat, stage);
		const auto finish = FinishPendingStageForCarry(Account, state, stage, message);
		REQUIRE(finish);
		const auto inspected = InspectConfigPayload(ParseSyncEnvelope(stage));
		baseText = text;
		baseVersion = inspected.version;
		state = finish.state;
		stage.clear();
		return message;
	}

	SyncCarryOwnVersion own() const {
		return { baseText, baseVersion, u"test"_q, u"Carry harness"_q, 5000 };
	}
};

QByteArray Text(char device, int revision = 1) {
	return QByteArray("version = 1\n# device ") + device + " revision " + QByteArray::number(revision) + '\n';
}

SyncCarryInput Input(
		const Device &device,
		const Cloud &cloud,
		const PeerToken &target,
		SyncCarrySourceAccess access = SyncCarrySourceAccess::Readable,
		SyncCarryMode mode = SyncCarryMode::Switch) {
	auto input = SyncCarryInput();
	input.account = Account;
	input.source = device.state.chat;
	input.target = target;
	input.mode = mode;
	input.access = access;
	input.targetEligibility = Eligible(target);
	input.state = device.state;
	input.preferenceToken = device.state.bindingToken.toLatin1();
	input.oldInventory = cloud.inventory(input.source);
	input.newInventory = cloud.inventory(target);
	input.ownVersion = device.own();
	if (mode != SyncCarryMode::Switch) {
		input.winner = cloud.cache(target);
	}
	return input;
}

void PendingContracts() {
	Section = "pending contracts";
	auto cloud = Cloud();
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	cloud.mark(old);
	auto device = Device('a', old, cloud.cache(old));
	device.publish(cloud, Text('a'));
	device.prepare(Text('a', 2));
	const auto original = StateBytes(device.state);
	for (const auto outcome : {
		SyncChatDirectRead::Unknown, SyncChatDirectRead::Valid,
		SyncChatDirectRead::MarkGone, SyncChatDirectRead::Migrated,
	}) {
		const auto result = AbandonStageForLostChat(Account, device.state, old,
			outcome, device.stage, device.localText);
		CHECK(result.status == SyncCarryPendingStatus::NotChatGone);
		CHECK(StateBytes(result.state) == original);
	}
	CHECK(AbandonStageForLostChat(Account, device.state, next, SyncChatDirectRead::ChatGone,
		device.stage, device.localText).status == SyncCarryPendingStatus::WrongChat);
	CHECK(AbandonStageForLostChat(Account, device.state, old, SyncChatDirectRead::ChatGone,
		device.stage, Text('z')).status == SyncCarryPendingStatus::LocalCopyRequired);
	CHECK(AbandonStageForLostChat(Account, device.state, old, SyncChatDirectRead::ChatGone,
		QByteArray("bad"), device.localText).status == SyncCarryPendingStatus::InvalidStage);
	const auto abandoned = AbandonStageForLostChat(Account, device.state, old,
		SyncChatDirectRead::ChatGone, device.stage, device.localText);
	REQUIRE(abandoned);
	CHECK(abandoned.state.config.seq == 2);
	CHECK(abandoned.state.config.confirmedSeq == 1);
	CHECK(abandoned.state.config.pendingSeq == 0);
	CHECK(abandoned.state.config.abandonedSeq == 2);
	CHECK(abandoned.state.config.ownHash == device.state.config.ownHash);
	CHECK(abandoned.state.configData.base == device.baseVersion.key);
	CHECK(abandoned.state.configData.pending.isEmpty());
	CHECK(abandoned.state.config.issuedRecords.size() == 2);
	CHECK(abandoned.state.ownMessages.size() == 1);
	auto observations = SyncPublishObservations();
	observations.discoveryComplete = observations.ready = true;
	observations.ownRecord = { OwnRecordObservationKind::Present, 1,
		abandoned.state.ownMessages.front().payloadHash };
	const auto decision = PlanSyncPublish(abandoned.state, abandoned.state.bindingToken.toLatin1(),
		abandoned.state.createdDevice, SyncLocalStream::Config, abandoned.state.config.ownHash,
		{ true, false, false }, observations);
	CHECK(decision.action == SyncPublishAction::ReserveAndStage);
	CHECK(PlanCarry(Input(device, cloud, next, SyncCarrySourceAccess::Unreadable)).status
		== SyncCarryStatus::UnknownSource);
	CHECK(PlanCarry(Input(device, cloud, next, SyncCarrySourceAccess::Protected)).status
		== SyncCarryStatus::FinishPendingOld);
	CHECK(PlanCarry(Input(device, cloud, next, SyncCarrySourceAccess::ChatGone)).status
		== SyncCarryStatus::AbandonPending);
	const auto bytes = StateBytes(abandoned.state);
	const auto parsed = ParseSyncLocalState(bytes);
	REQUIRE(parsed);
	CHECK(parsed.state.config.abandonedSeq == 2);
	CHECK(StateBytes(parsed.state) == bytes);
	const auto reserved = ReserveSyncSeq(parsed.state, SyncLocalStream::Config, QString(64, u'a'));
	REQUIRE(reserved);
	CHECK(reserved.seq == 3);
	CHECK(reserved.state.config.abandonedSeq == 0);
	CHECK(reserved.state.config.confirmedSeq == 1);
	CHECK(StateBytes(device.state) == original);
	auto wrong = cloud.post(next, device.stage);
	CHECK(FinishPendingStageForCarry(Account, device.state, device.stage, wrong).status
		== SyncCarryPendingStatus::WrongChat);
	wrong = cloud.post(old, device.stage);
	const auto finished = FinishPendingStageForCarry(Account, device.state, device.stage, wrong);
	REQUIRE(finished);
	CHECK(finished.state.config.confirmedSeq == 2);
	CHECK(finished.state.config.pendingSeq == 0);
	CHECK(finished.state.configData.base == InspectConfigPayload(ParseSyncEnvelope(device.stage)).version.key);
	wrong.ownership = SyncChatMessageOwnership::Other;
	CHECK(FinishPendingStageForCarry(Account, device.state, device.stage, wrong).status
		== SyncCarryPendingStatus::InvalidStage);
	auto library = ReserveSyncSeq(device.state, SyncLocalStream::Library, QString(64, u'b')).state;
	CHECK(AbandonStageForLostChat(Account, library, old, SyncChatDirectRead::ChatGone,
		device.stage, device.localText).status == SyncCarryPendingStatus::LibraryPending);
	for (auto variant = 0; variant != 5; ++variant) {
		auto invalid = abandoned.state;
		if (variant == 0) invalid.config.abandonedSeq = 1;
		if (variant == 1) invalid.config.pendingSeq = 2;
		if (variant == 2) invalid.config.issuedRecords.clear();
		if (variant == 3) invalid.library.abandonedSeq = 1;
		if (variant == 4) { invalid.version = 1; invalid.chat = PeerToken(); }
		CHECK(!SerializeSyncLocalState(invalid));
	}
	auto document = QJsonDocument::fromJson(bytes).object();
	auto streams = document.value(u"streams"_q).toObject();
	auto config = streams.value(u"config"_q).toObject();
	config.remove(u"abandoned_seq"_q);
	streams.insert(u"config"_q, config);
	document.insert(u"streams"_q, streams);
	CHECK(ParseSyncLocalState(QJsonDocument(document).toJson()).error == SyncLocalError::InvalidCounters);
}

void CarryContracts() {
	Section = "carry contracts";
	auto cloud = Cloud();
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	cloud.mark(old);
	auto a = Device('a', old, cloud.cache(old));
	auto b = Device('b', old, cloud.cache(old));
	a.publish(cloud, Text('a'));
	b.publish(cloud, Text('b'));
	const auto latest = b.publish(cloud, Text('b', 2));
	cloud.post(old, latest.bytes);
	auto opaque = QJsonDocument::fromJson(latest.bytes).object();
	opaque.insert(u"stream"_q, u"future.stream"_q);
	opaque.insert(u"seq"_q, 12);
	const auto unsupported = CanonicalizeSyncJson(QJsonDocument(opaque).toJson());
	REQUIRE(unsupported);
	const auto opaqueMessage = cloud.post(old, unsupported.canonical);
	opaque = QJsonDocument::fromJson(latest.bytes).object();
	opaque.insert(u"encoding"_q, u"future"_q);
	opaque.insert(u"seq"_q, 13);
	const auto encoded = cloud.post(old, CanonicalizeSyncJson(QJsonDocument(opaque).toJson()).canonical);
	const auto newer = cloud.post(old, QByteArray("{\"purple_sync\":2,\"future\":true}"));
	const auto selection = SelectSyncCarryHeads(cloud.inventory(old));
	REQUIRE(selection.status == SyncCarryStatus::Ready);
	CHECK(selection.heads.size() == 4);
	auto input = Input(a, cloud, next);
	const auto planned = PlanCarry(input);
	REQUIRE(planned);
	CHECK(planned.plan.forwards.size() == 4);
	CHECK(planned.plan.ownRecord.isEmpty());
	CHECK(planned.plan.requiredHashes.size() == 4);
	CHECK(VerifySyncCarryReadback(planned.plan, cloud.inventory(next)).status == SyncCarryStatus::MissingRecords);
	CHECK(!PrepareSyncCarryCommit(planned.plan, a.state, cloud.inventory(next)));
	CHECK(VerifySyncCarryReadback(planned.plan, cloud.inventory(old)).status == SyncCarryStatus::WrongScope);
	for (const auto &reference : planned.plan.forwards) {
		REQUIRE(cloud.forward(reference, next));
	}
	CHECK(cloud.has(next, opaqueMessage.reference.recordHash));
	CHECK(cloud.has(next, encoded.reference.recordHash));
	CHECK(cloud.has(next, newer.reference.recordHash));
	CHECK(VerifySyncCarryReadback(planned.plan, cloud.inventory(next)));
	const auto committed = PrepareSyncCarryCommit(planned.plan, a.state, cloud.inventory(next));
	REQUIRE(committed);
	CHECK(committed.state->chat == next);
	CHECK(committed.state->config.seq == a.state.config.seq);
	CHECK(committed.state->config.confirmedSeq == a.state.config.confirmedSeq);
	CHECK(committed.state->ownMessages.size() == 1);
	for (const auto &entry : committed.state->ownMessages) {
		CHECK(cloud.has(next, entry.recordHash));
	}
	auto reordered = cloud.inventory(next);
	std::reverse(reordered.messages.begin(), reordered.messages.end());
	CHECK(PrepareSyncCarryCommit(planned.plan, a.state, reordered).canonical == committed.canonical);
	CHECK(SelectSyncCarryHeads(reordered).stamp == SelectSyncCarryHeads(cloud.inventory(next)).stamp);
	input = Input(a, cloud, next);
	const auto repeated = PlanCarry(input);
	REQUIRE(repeated);
	CHECK(repeated.plan.forwards.empty());
	CHECK(repeated.plan.requiredHashes == planned.plan.requiredHashes);
	input.targetEligibility.readers.warningRequired = true;
	CHECK(PlanCarry(input).status == SyncCarryStatus::ConsentRequired);
	input.readerConsentAccepted = true;
	CHECK(PlanCarry(input));
	input.targetEligibility.readers.warningRequired = false;
	input.oldInventory.complete = false;
	CHECK(PlanCarry(input).status == SyncCarryStatus::ReadSource);
	input = Input(a, cloud, next);
	input.newInventory.complete = false;
	CHECK(PlanCarry(input).status == SyncCarryStatus::ReadDestination);
	input = Input(a, cloud, next, SyncCarrySourceAccess::Unknown);
	CHECK(PlanCarry(input).status == SyncCarryStatus::UnknownSource);
	input = Input(a, cloud, next);
	input.newInventory.account.environment = 1;
	CHECK(PlanCarry(input).status == SyncCarryStatus::WrongScope);
	input = Input(a, cloud, next);
	input.newInventory.messages[0].reference.chat = old;
	CHECK(PlanCarry(input).status == SyncCarryStatus::WrongScope);
	auto duplicate = cloud.inventory(old);
	duplicate.messages.push_back(duplicate.messages.front());
	CHECK(SelectSyncCarryHeads(duplicate).status == SyncCarryStatus::Ready);
	duplicate.messages.back().bytes += ' ';
	duplicate.messages.back().reference.recordHash = SyncCarryRecordHash(duplicate.messages.back().bytes);
	CHECK(SelectSyncCarryHeads(duplicate).status == SyncCarryStatus::NeedsReview);
	auto foreign = cloud.inventory(next);
	foreign.messages.push_back(*MakeSyncCarryMessage(Account, next, 999, 100,
		QByteArray("not a record"), SyncChatMessageOwnership::Other));
	CHECK(SelectSyncCarryHeads(foreign).status == SyncCarryStatus::Ready);
	foreign.messages.back().ownership = SyncChatMessageOwnership::Unknown;
	CHECK(SelectSyncCarryHeads(foreign).status == SyncCarryStatus::NeedsReview);
	input = Input(a, cloud, Peer(u"chat:30"_q), SyncCarrySourceAccess::Protected);
	const auto local = PlanCarry(input);
	REQUIRE(local);
	CHECK(local.plan.forwards.empty() && local.plan.selected.empty());
	CHECK(!local.plan.ownRecord.isEmpty());
	const auto own = ParseSyncEnvelope(local.plan.ownRecord);
	CHECK(own.header->writerInstall == a.state.install);
	CHECK(own.header->seq == a.state.config.seq + 1);
	CHECK(InspectConfigPayload(own).version.key == a.baseVersion.key);
	const auto staged = PrepareSyncCarryStage(local.plan, a.state);
	REQUIRE(staged);
	CHECK(StateBytes(*staged) == StateBytes(*local.reservedState));
	CHECK(StateBytes(*PrepareSyncCarryStage(local.plan, *staged)) == StateBytes(*staged));
	CHECK(!PrepareSyncCarryCommit(local.plan, *staged, cloud.inventory(input.target)));
	cloud.post(input.target, local.plan.ownRecord);
	const auto restored = PrepareSyncCarryCommit(local.plan, *staged, cloud.inventory(input.target));
	REQUIRE(restored);
	CHECK(restored.state->config.confirmedSeq == a.state.config.seq + 1);
	CHECK(restored.state->config.pendingSeq == 0);
	CHECK(restored.state->chat == input.target);
	input.ownVersion.reset();
	CHECK(PlanCarry(input).status == SyncCarryStatus::MissingLocalVersion);
	input = Input(a, cloud, next, SyncCarrySourceAccess::Protected);
	CHECK(PlanCarry(input).plan.ownRecord.isEmpty());
	auto json = SyncCarryPlanJson(planned.plan);
	json.insert(u"future"_q, QJsonArray{ 1, 2 });
	auto selected = json.value(u"selected"_q).toArray();
	auto entry = selected[0].toObject();
	auto ref = entry.value(u"reference"_q).toObject();
	ref.insert(u"future_ref"_q, u"retained"_q);
	entry.insert(u"reference"_q, ref);
	selected[0] = entry;
	json.insert(u"selected"_q, selected);
	const auto roundTrip = ParseSyncCarryPlan(json);
	REQUIRE(roundTrip);
	CHECK(SyncCarryPlanJson(*roundTrip) == json);
	json.insert(u"account"_q, u"1-777"_q);
	CHECK(!ParseSyncCarryPlan(json));
	json = SyncCarryPlanJson(planned.plan);
	auto forwards = json.value(u"forwards"_q).toArray();
	ref = forwards[0].toObject();
	ref.insert(u"message_id"_q, 9999);
	forwards[0] = ref;
	json.insert(u"forwards"_q, forwards);
	CHECK(!ParseSyncCarryPlan(json));
}

SyncChatResolution Resolution(const Device &device, const Cloud &cloud, bool reverse = false) {
	auto input = SyncChatResolveInput();
	input.cache = device.cache;
	input.cacheRead = SyncChatDirectRead::Valid;
	input.cachedEligibility = Eligible(device.cache.chat);
	for (const auto &[key, mark] : cloud.marks) {
		auto found = SyncChatMark();
		found.location = { mark.chat, mark.messageId, mark.date };
		found.status = SyncChatMarkStatus::Supported;
		found.eligibility = Eligible(mark.chat);
		found.records = cloud.inventory(mark.chat).messages.empty()
			? SyncChatRecordPresence::Empty : SyncChatRecordPresence::Present;
		found.readable = true;
		found.freshlyRead = true;
		input.marks.push_back(found);
	}
	if (reverse) std::reverse(input.marks.begin(), input.marks.end());
	return ResolveSyncChat(input);
}

struct Session {
	Device &device;
	Cloud &cloud;
	PeerToken source;
	PeerToken target;
	SyncCarryMode mode;
	SyncCarrySourceAccess access;
	SyncChatResolution initial;
	SyncCarryJournal journal;
	std::set<SyncCarryPhase> visited;
	std::optional<SyncCarryPhase> crashBefore;
	std::optional<SyncCarryPhase> crashBeforeAck;
	bool crashed = false;
	bool ambiguous = true;
	bool authorizedCleanup = false;
	bool held = false;

	Session(
			Device &d,
			Cloud &c,
			const PeerToken &n,
			SyncCarryMode m = SyncCarryMode::Switch,
			SyncCarrySourceAccess a = SyncCarrySourceAccess::Readable,
			bool cleanup = false,
			bool removeMark = false)
	: device(d), cloud(c), source(d.state.chat), target(n), mode(m), access(a)
	, initial(Resolution(d, c)), authorizedCleanup(cleanup) {
		if (mode == SyncCarryMode::Switch) {
			initial.action = SyncChatAction::Use;
			initial.cache = device.cache;
			initial.winner = device.cache;
			initial.source.reset();
		}
		auto start = SyncCarryJournalStart();
		start.operationId = *FormatSyncBindingToken(QByteArray::number(cloud.clock).leftJustified(16, 'x'));
		start.account = Account;
		start.source = source;
		start.target = target;
		start.mode = mode;
		start.state = device.state;
		start.preferenceToken = device.state.bindingToken.toLatin1();
		start.resolution = initial;
		start.targetEligibility = Eligible(target);
		start.operationConfirmed = true;
		start.removeOldMarkAuthorized = removeMark;
		start.markText = u"Synthetic carry test\n#purplechatsync v1"_q;
		start.markRequestId = QString::number(qulonglong(100 + cloud.clock));
		start.ownRequestId = QString::number(qulonglong(200 + cloud.clock));
		const auto begun = BeginSyncCarryJournal(start);
		REQUIRE(begun);
		journal = begun.journal;
	}

	void store(const SyncCarryJournalResult &result) {
		if (result.canonical.isEmpty()) {
			std::printf("journal phase=%d status=%d\n", int(journal.phase), int(result.status));
		}
		REQUIRE(!result.canonical.isEmpty());
		const auto parsed = ParseSyncCarryJournal(result.canonical);
		REQUIRE(parsed);
		CHECK(SerializeSyncCarryJournal(parsed.journal).canonical == result.canonical);
		journal = parsed.journal;
		held = result.status == SyncCarryJournalStatus::NeedsReview;
	}

	bool restart(SyncCarryPhase phase, bool afterEffect) {
		if (crashed || (afterEffect ? crashBeforeAck != phase : crashBefore != phase)) {
			return false;
		}
		crashed = true;
		const auto parsed = ParseSyncCarryJournal(SerializeSyncCarryJournal(journal).canonical);
		REQUIRE(parsed);
		store(ResumeSyncCarryJournal(parsed.journal, device.state, false, true));
		return true;
	}

	void step() {
		visited.insert(journal.phase);
		if (restart(journal.phase, false)) {
			return;
		}
		auto event = SyncCarryEvent(journal, SyncCarryEventKind::Resolution);
		event.outcome = SyncCarryOutcome::Success;
		event.state = device.state;
		event.objectsDurable = true;
		const auto phase = journal.phase;
		switch (phase) {
		case SyncCarryPhase::ResolvePending: {
			event.kind = SyncCarryEventKind::PendingPrepared;
			event.stagedRecord = device.stage;
			event.retainedLocalText = device.localText;
			SyncCarryPendingResult pending;
			if (access == SyncCarrySourceAccess::ChatGone) {
				event.oldRead = SyncChatDirectRead::ChatGone;
				pending = AbandonStageForLostChat(Account, device.state, source,
					event.oldRead, device.stage, device.localText);
			} else {
				const auto readback = cloud.post(source, device.stage);
				event.pendingReadback = readback;
				pending = FinishPendingStageForCarry(Account, device.state, device.stage, readback);
				device.baseVersion = InspectConfigPayload(ParseSyncEnvelope(device.stage)).version;
				device.baseText = device.localText;
			}
			REQUIRE(pending);
			const auto bytes = StateBytes(pending.state);
			event.preparedImageHash = SyncCarryRecordHash(bytes);
			device.images[event.preparedImageHash] = bytes;
		} break;
		case SyncCarryPhase::WritePendingState:
			event.kind = SyncCarryEventKind::StateObserved;
			device.state = ParseSyncLocalState(device.images.at(journal.pendingStateHash)).state;
			event.state = device.state;
			if (restart(phase, true)) return;
			break;
		case SyncCarryPhase::RetirePendingStage:
			event.kind = SyncCarryEventKind::StageRetired;
			event.retiredStageHash = device.stage.isEmpty() ? journal.pendingStageHash : SyncCarryRecordHash(device.stage);
			device.stage.clear();
			event.noPendingFiles = true;
			if (restart(phase, true)) return;
			break;
		case SyncCarryPhase::ResolveAgain:
			event.kind = SyncCarryEventKind::Resolution;
			event.resolution = initial;
			break;
		case SyncCarryPhase::Plan: {
			event.kind = SyncCarryEventKind::PlanPrepared;
			auto input = Input(device, cloud, target, access, mode);
			if (authorizedCleanup) {
				input.cleanup = mode == SyncCarryMode::Switch
					? SyncCarryCleanup::AllSettings : SyncCarryCleanup::OwnLatePosts;
				input.cleanupAuthorized = true;
			}
			const auto planned = PlanCarry(input);
			if (!planned) std::printf("plan status=%d\n", int(planned.status));
			REQUIRE(planned);
			event.plan = planned.plan;
		} break;
		case SyncCarryPhase::StageOwn: {
			event.kind = SyncCarryEventKind::OwnStagePersisted;
			const auto state = PrepareSyncCarryStage(*journal.plan, device.state);
			REQUIRE(state);
			device.state = *state;
			device.stage = journal.plan->ownRecord;
			event.state = device.state;
			event.stagedRecord = device.stage;
			if (restart(phase, true)) return;
		} break;
		case SyncCarryPhase::Deliver: {
			const auto forwards = SyncCarryJournalForwards(journal);
			const auto own = SyncCarryJournalPostsOwnRecord(journal);
			const auto ownBytes = journal.plan->ownRecord;
			event.kind = SyncCarryEventKind::DeliveryPrepared;
			event.outcome = SyncCarryOutcome::Unknown;
			store(AdvanceSyncCarryJournal(journal, event));
			CHECK(journal.phase == SyncCarryPhase::VerifyDestination);
			for (const auto &reference : forwards) {
				REQUIRE(cloud.forward(reference, target));
			}
			if (own) cloud.post(target, ownBytes);
			if (restart(phase, true)) return;
			auto result = SyncCarryEvent(journal, SyncCarryEventKind::DeliveryResult);
			result.outcome = ambiguous ? SyncCarryOutcome::Unknown : SyncCarryOutcome::Success;
			store(AdvanceSyncCarryJournal(journal, result));
			return;
		}
		case SyncCarryPhase::VerifyDestination: {
			event.kind = SyncCarryEventKind::DestinationRead;
			event.destination = cloud.inventory(target);
			if (VerifySyncCarryReadback(*journal.plan, *event.destination)) {
				const auto committed = PrepareSyncCarryCommit(*journal.plan, device.state, *event.destination);
				REQUIRE(committed);
				event.preparedImageHash = committed.stateHash;
				device.images[committed.stateHash] = committed.canonical;
			}
		} break;
		case SyncCarryPhase::Mark:
			if (NextSyncCarryJournalAction(journal) == SyncCarryJournalAction::PostMark) {
				const auto request = journal.operationId + journal.markRequestId;
				const auto text = journal.markText;
				event.kind = SyncCarryEventKind::MarkPostPrepared;
				event.outcome = SyncCarryOutcome::Unknown;
				store(AdvanceSyncCarryJournal(journal, event));
				CHECK(NextSyncCarryJournalAction(journal) == SyncCarryJournalAction::ReadMark);
				cloud.mark(target, text, request);
				if (restart(phase, true)) return;
				auto result = SyncCarryEvent(journal, SyncCarryEventKind::MarkPostResult);
				result.outcome = ambiguous ? SyncCarryOutcome::Unknown : SyncCarryOutcome::Success;
				store(AdvanceSyncCarryJournal(journal, result));
				return;
			}
			event.kind = SyncCarryEventKind::MarkRead;
			event.mark = cloud.marks.at(cloud.key(target));
			break;
		case SyncCarryPhase::PrepareCache:
			event.kind = SyncCarryEventKind::CachePrepared;
			event.cache = { journal.operationId, Account, *journal.targetMark, false };
			device.pendingCache = event.cache;
			if (restart(phase, true)) return;
			break;
		case SyncCarryPhase::CommitState:
			event.kind = SyncCarryEventKind::StateObserved;
			device.state = ParseSyncLocalState(device.images.at(journal.nextStateHash)).state;
			event.state = device.state;
			if (restart(phase, true)) return;
			break;
		case SyncCarryPhase::RetireOwnStage:
			event.kind = SyncCarryEventKind::StageRetired;
			event.retiredStageHash = device.stage.isEmpty() ? journal.plan->ownRecordHash : SyncCarryRecordHash(device.stage);
			device.stage.clear();
			event.noPendingFiles = true;
			if (restart(phase, true)) return;
			break;
		case SyncCarryPhase::CompleteCache:
			event.kind = SyncCarryEventKind::CacheCompleted;
			event.cache = { journal.operationId, Account, *journal.targetMark, true };
			device.cache = *journal.targetMark;
			device.pendingCache.reset();
			if (restart(phase, true)) return;
			break;
		case SyncCarryPhase::Cleanup:
			event.kind = SyncCarryEventKind::CleanupCompleted;
			event.destination = cloud.inventory(target);
			event.deleted = SyncCarryCleanupCandidates(*journal.plan, *event.destination);
			for (const auto &reference : event.deleted) {
				const auto message = cloud.find(reference);
				if (message) {
					REQUIRE(CheckSyncCarryDeletion(journal, *message, *event.destination));
					cloud.remove(reference);
				}
			}
			if (journal.removeOldMarkAuthorized && journal.oldMark && journal.oldMark->markId) {
				const auto mark = cloud.marks.find(cloud.key(source));
				if (mark != cloud.marks.end()) {
					REQUIRE(CheckSyncCarryOldMarkDeletion(journal, mark->second, *event.destination));
					cloud.marks.erase(mark);
				}
				event.mark.outcome = SyncCarryMarkOutcome::Absent;
				event.mark.account = Account;
				event.mark.chat = source;
				event.mark.messageId = journal.oldMark->markId;
			}
			if (restart(phase, true)) return;
			break;
		case SyncCarryPhase::Done:
		case SyncCarryPhase::Aborted:
			return;
		}
		store(AdvanceSyncCarryJournal(journal, event));
	}

	void run(std::optional<SyncCarryPhase> stop = std::nullopt) {
		for (auto stepCount = 0; stepCount != 100; ++stepCount) {
			if (held || journal.phase == SyncCarryPhase::Done || journal.phase == SyncCarryPhase::Aborted
				|| stop == journal.phase) {
				return;
			}
			step();
		}
		REQUIRE(false);
	}
};

void JournalContracts() {
	Section = "journal contracts";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	auto cloud = Cloud();
	cloud.mark(old);
	auto device = Device('a', old, cloud.cache(old));
	device.publish(cloud, Text('a'));
	auto session = Session(device, cloud, next, SyncCarryMode::Switch,
		SyncCarrySourceAccess::Readable, true);
	const auto initial = SerializeSyncCarryJournal(session.journal).canonical;
	CHECK(ParseSyncCarryJournal(initial));
	CHECK(!ParseSyncCarryJournal("{\"version\":1,\"version\":1}"));
	CHECK(ParseSyncCarryJournal("{\"version\":2}").status == SyncCarryJournalStatus::NewerVersion);
	CHECK(!ParseSyncCarryJournal(QByteArray(4 * 1024 * 1024 + 1, ' ')));
	for (auto variant = 0; variant != 6; ++variant) {
		auto event = SyncCarryEvent(session.journal, SyncCarryEventKind::Resolution);
		event.outcome = SyncCarryOutcome::Success;
		event.resolution = session.initial;
		if (variant == 0) ++event.revision;
		if (variant == 1) event.operationId = QString(32, u'b');
		if (variant == 2) event.account.environment = 1;
		if (variant == 3) event.source = next;
		if (variant == 4) event.target = old;
		if (variant == 5) event.kind = SyncCarryEventKind::StateObserved;
		const auto invalid = AdvanceSyncCarryJournal(session.journal, event);
		CHECK(invalid.status == SyncCarryJournalStatus::Invalid);
		CHECK(SerializeSyncCarryJournal(invalid.journal).canonical == initial);
	}
	auto cancel = SyncCarryEvent(session.journal, SyncCarryEventKind::Cancel);
	cancel.state = device.state;
	cancel.noRequestInFlight = true;
	cancel.noPendingFiles = true;
	const auto cancelled = AdvanceSyncCarryJournal(session.journal, cancel);
	REQUIRE(cancelled);
	CHECK(cancelled.journal.phase == SyncCarryPhase::Aborted);
	CHECK(StateBytes(device.state) == StateBytes(*cancel.state));
	auto newer = SyncCarryEvent(session.journal, SyncCarryEventKind::Resolution);
	newer.outcome = SyncCarryOutcome::Success;
	newer.resolution = session.initial;
	newer.resolution.winner = SyncChatCache{ Peer(u"chat:99"_q), 1, 9000 };
	const auto superseded = AdvanceSyncCarryJournal(session.journal, newer);
	CHECK(superseded.status == SyncCarryJournalStatus::Superseded);
	CHECK(superseded.journal.phase == SyncCarryPhase::Aborted);
	session.run(SyncCarryPhase::Deliver);
	const auto planned = session.journal;
	const auto message = cloud.inventory(old).messages.front();
	CHECK(!CheckSyncCarryDeletion(planned, message, cloud.inventory(next)));
	const auto sends = SyncCarryJournalForwards(session.journal);
	REQUIRE(!sends.empty());
	auto intent = SyncCarryEvent(session.journal, SyncCarryEventKind::DeliveryPrepared);
	intent.outcome = SyncCarryOutcome::Unknown;
	session.store(AdvanceSyncCarryJournal(session.journal, intent));
	CHECK(session.journal.phase == SyncCarryPhase::VerifyDestination);
	CHECK(SyncCarryJournalForwards(session.journal).empty());
	auto read = SyncCarryEvent(session.journal, SyncCarryEventKind::DestinationRead);
	read.outcome = SyncCarryOutcome::Success;
	read.state = device.state;
	read.destination = cloud.inventory(next);
	const auto missing = AdvanceSyncCarryJournal(session.journal, read);
	CHECK(missing.status == SyncCarryJournalStatus::Retry);
	CHECK(missing.journal.phase == SyncCarryPhase::Deliver);
	session.store(missing);
	session.step();
	REQUIRE(session.journal.phase == SyncCarryPhase::VerifyDestination);
	read = SyncCarryEvent(session.journal, SyncCarryEventKind::DestinationRead);
	read.outcome = SyncCarryOutcome::Success;
	read.state = device.state;
	read.destination = cloud.inventory(next, false);
	const auto incomplete = AdvanceSyncCarryJournal(session.journal, read);
	CHECK(incomplete.status == SyncCarryJournalStatus::Wait);
	CHECK(incomplete.journal.phase == SyncCarryPhase::VerifyDestination);
	CHECK(NextSyncCarryJournalAction(incomplete.journal) == SyncCarryJournalAction::VerifyDestination);
	CHECK(ResumeSyncCarryJournal(session.journal, std::nullopt, true).status
		== SyncCarryJournalStatus::NeedsReview);
	session.run(SyncCarryPhase::Mark);
	CHECK(NextSyncCarryJournalAction(session.journal) == SyncCarryJournalAction::PostMark);
	intent = SyncCarryEvent(session.journal, SyncCarryEventKind::MarkPostPrepared);
	intent.outcome = SyncCarryOutcome::Unknown;
	session.store(AdvanceSyncCarryJournal(session.journal, intent));
	CHECK(NextSyncCarryJournalAction(session.journal) == SyncCarryJournalAction::ReadMark);
	cancel = SyncCarryEvent(session.journal, SyncCarryEventKind::Cancel);
	cancel.state = device.state;
	cancel.noRequestInFlight = true;
	cancel.noPendingFiles = true;
	CHECK(AdvanceSyncCarryJournal(session.journal, cancel).status == SyncCarryJournalStatus::NeedsReview);
	auto markRead = SyncCarryEvent(session.journal, SyncCarryEventKind::MarkRead);
	markRead.outcome = SyncCarryOutcome::Success;
	markRead.mark.account = Account;
	markRead.mark.chat = next;
	markRead.mark.outcome = SyncCarryMarkOutcome::Unknown;
	const auto unknown = AdvanceSyncCarryJournal(session.journal, markRead);
	CHECK(unknown.status == SyncCarryJournalStatus::Wait);
	CHECK(NextSyncCarryJournalAction(unknown.journal) == SyncCarryJournalAction::ReadMark);
	markRead.mark.outcome = SyncCarryMarkOutcome::Absent;
	const auto absent = AdvanceSyncCarryJournal(session.journal, markRead);
	CHECK(absent.status == SyncCarryJournalStatus::Retry);
	CHECK(NextSyncCarryJournalAction(absent.journal) == SyncCarryJournalAction::PostMark);
	session.store(absent);
	session.run();
	CHECK(session.journal.phase == SyncCarryPhase::Done);
	CHECK(device.state.chat == next);
	CHECK(device.cache.chat == next);
	CHECK(cloud.inventory(old).messages.empty());
	CHECK(!cloud.inventory(next).messages.empty());
	const auto done = SerializeSyncCarryJournal(session.journal).canonical;
	auto json = QJsonDocument::fromJson(done).object();
	json.insert(u"future"_q, QJsonObject{ { u"kept"_q, true } });
	const auto parsed = ParseSyncCarryJournal(QJsonDocument(json).toJson());
	REQUIRE(parsed);
	CHECK(QJsonDocument::fromJson(SerializeSyncCarryJournal(parsed.journal).canonical).object() == json);
	json.insert(u"state_committed"_q, false);
	CHECK(!ParseSyncCarryJournal(QJsonDocument(json).toJson()));
}

void CloneAndPlanBinding() {
	Section = "clone and plan binding";
	auto cloud = Cloud();
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	cloud.mark(old);
	auto a = Device('a', old, cloud.cache(old));
	auto b = Device('b', old, cloud.cache(old));
	a.publish(cloud, Text('a'));
	const auto original = b.publish(cloud, Text('b'));
	auto changed = QJsonDocument::fromJson(original.bytes).object();
	auto writer = changed.value(u"writer"_q).toObject();
	writer.insert(u"app"_q, u"Other copy"_q);
	changed.insert(u"writer"_q, writer);
	const auto conflict = cloud.post(old, CanonicalizeSyncJson(QJsonDocument(changed).toJson()).canonical);
	const auto selection = SelectSyncCarryHeads(cloud.inventory(old));
	CHECK(selection.heads.size() == 3);
	CHECK(PlanCarry(Input(b, cloud, next)).status == SyncCarryStatus::NeedsReview);
	cloud.forward(conflict.reference, next);
	CHECK(PlanCarry(Input(b, cloud, next, SyncCarrySourceAccess::Protected)).status == SyncCarryStatus::NeedsReview);
	const auto planned = PlanCarry(Input(a, cloud, next));
	REQUIRE(planned);
	CHECK(planned.plan.requiredHashes.size() == 3);
	CHECK(SyncCarryPlanUsesState(planned.plan, a.state));
	auto forged = planned.plan;
	forged.issued.front().recordHash = original.reference.recordHash;
	CHECK(!SyncCarryPlanUsesState(forged, a.state));
	forged = planned.plan;
	++forged.confirmedAtCommit;
	CHECK(!SyncCarryPlanUsesState(forged, a.state));
	auto journal = Session(a, cloud, next);
	journal.run(SyncCarryPhase::Plan);
	auto prepared = SyncCarryEvent(journal.journal, SyncCarryEventKind::PlanPrepared);
	prepared.outcome = SyncCarryOutcome::Success;
	prepared.objectsDurable = true;
	prepared.state = a.state;
	prepared.plan = forged;
	CHECK(AdvanceSyncCarryJournal(journal.journal, prepared).status == SyncCarryJournalStatus::Invalid);
	for (const auto &reference : planned.plan.forwards) REQUIRE(cloud.forward(reference, next));
	CHECK(PrepareSyncCarryCommit(forged, a.state, cloud.inventory(next)).status == SyncCarryStatus::WrongScope);
}

void LegacyCarry() {
	Section = "legacy current head without issued log";
	auto cloud = Cloud();
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	cloud.mark(old);
	auto device = Device('a', old, cloud.cache(old));
	device.publish(cloud, Text('a'));
	device.state.config.issuedRecords.clear();
	REQUIRE(SerializeSyncLocalState(device.state));
	const auto planned = PlanCarry(Input(device, cloud, next));
	REQUIRE(planned);
	CHECK(!planned.plan.ownRecord.isEmpty());
	CHECK(planned.plan.confirmedAtCommit == 2);
	CHECK(planned.plan.issued.size() == 1);
	for (const auto &reference : planned.plan.forwards) REQUIRE(cloud.forward(reference, next));
	const auto stage = PrepareSyncCarryStage(planned.plan, device.state);
	REQUIRE(stage);
	CHECK(!VerifySyncCarryReadback(planned.plan, cloud.inventory(next)));
	cloud.post(next, planned.plan.ownRecord);
	const auto committed = PrepareSyncCarryCommit(planned.plan, *stage, cloud.inventory(next));
	REQUIRE(committed);
	CHECK(committed.state->config.confirmedSeq == 2);
	CHECK(committed.state->ownMessages.size() == 1);
	CHECK(committed.state->ownMessages.front().recordHash == planned.plan.ownRecordHash);
	auto runCloud = Cloud();
	runCloud.mark(old);
	auto runDevice = Device('c', old, runCloud.cache(old));
	runDevice.publish(runCloud, Text('c'));
	runDevice.state.config.issuedRecords.clear();
	auto session = Session(runDevice, runCloud, Peer(u"chat:30"_q));
	session.run();
	CHECK(session.journal.phase == SyncCarryPhase::Done);
	CHECK(runDevice.state.config.seq == 2);
	auto legacy = Device('b', old, cloud.cache(old));
	legacy.publish(cloud, Text('b'));
	legacy.state.config.issuedRecords.clear();
	const auto changedText = Text('b', 2);
	const auto changedVersion = MakeConfigVersion(changedText, { legacy.baseVersion });
	REQUIRE(changedVersion);
	const auto clone = BuildConfigRecord({
		.text = changedText,
		.version = *changedVersion,
		.space = legacy.state.space,
		.install = legacy.state.install,
		.device = legacy.state.createdDevice,
		.platform = u"test"_q,
		.app = u"Conflicting legacy head"_q,
		.seq = 1,
		.at = 1002,
	});
	REQUIRE(clone);
	cloud.post(old, clone.canonical);
	CHECK(PlanCarry(Input(legacy, cloud, next)).status == SyncCarryStatus::NeedsReview);
}

void ReviewDurability() {
	Section = "NON_DURABLE_STATE";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	for (const auto pending : { false, true }) {
		auto cloud = Cloud();
		cloud.mark(old);
		auto device = Device('a', old, cloud.cache(old));
		const auto source = device.publish(cloud, Text('a'));
		if (pending) device.prepare(Text('a', 2));
		auto session = Session(device, cloud, next, SyncCarryMode::Switch,
			pending ? SyncCarrySourceAccess::ChatGone : SyncCarrySourceAccess::Readable, true);
		const auto phase = pending ? SyncCarryPhase::WritePendingState : SyncCarryPhase::CommitState;
		session.run(phase);
		const auto hash = pending ? session.journal.pendingStateHash : session.journal.nextStateHash;
		const auto expected = ParseSyncLocalState(device.images.at(hash)).state;
		auto event = SyncCarryEvent(session.journal, SyncCarryEventKind::StateObserved);
		event.outcome = SyncCarryOutcome::Success;
		event.state = expected;
		event.objectsDurable = false;
		const auto blocked = AdvanceSyncCarryJournal(session.journal, event);
		CHECK(blocked.status == SyncCarryJournalStatus::Wait);
		CHECK(blocked.journal.phase == phase);
		CHECK(!blocked.journal.stateCommitted && !blocked.journal.cacheCompleted);
		CHECK(blocked.journal.stateHash == session.journal.stateHash);
		CHECK(!CheckSyncCarryDeletion(blocked.journal, source, cloud.inventory(next)));
		const auto unconfirmedRecovery = ResumeSyncCarryJournal(session.journal, expected, false);
		CHECK(unconfirmedRecovery.status == SyncCarryJournalStatus::Wait);
		CHECK(unconfirmedRecovery.canonical.isEmpty());
		CHECK(unconfirmedRecovery.journal.phase == phase);
		CHECK(!unconfirmedRecovery.journal.stateCommitted);
		const auto recovery = ResumeSyncCarryJournal(session.journal, expected, false, true);
		REQUIRE(recovery);
		CHECK(recovery.journal.phase == (pending ? SyncCarryPhase::RetirePendingStage : SyncCarryPhase::CompleteCache));
		event.objectsDurable = true;
		const auto durable = AdvanceSyncCarryJournal(session.journal, event);
		REQUIRE(durable);
		CHECK(durable.journal.phase == recovery.journal.phase);
		CHECK(durable.journal.stateCommitted == !pending);
	}
}

void ReviewCleanup() {
	Section = "OTHER_INSTALL_DELETE and frozen account cleanup";
	const auto next = Peer(u"chat:20"_q);
	for (const auto old : { PeerToken(), Peer(u"chat:10"_q) }) {
		for (const auto authorized : { false, true }) {
			auto cloud = Cloud();
			cloud.mark(old);
			auto a = Device('a', old, cloud.cache(old));
			auto b = Device('b', old, cloud.cache(old));
			a.publish(cloud, Text('a'));
			const auto other = b.publish(cloud, Text('b'));
			auto session = Session(a, cloud, next, SyncCarryMode::Switch,
				SyncCarrySourceAccess::Readable, authorized);
			session.run(SyncCarryPhase::Deliver);
			CHECK(!CheckSyncCarryDeletion(session.journal, other, cloud.inventory(next)));
			session.run(SyncCarryPhase::PrepareCache);
			CHECK(!CheckSyncCarryDeletion(session.journal, other, cloud.inventory(next)));
			session.run(SyncCarryPhase::Cleanup);
			CHECK(std::none_of(a.state.config.issuedRecords.begin(), a.state.config.issuedRecords.end(),
				[&](const auto &issued) { return issued.recordHash == other.reference.recordHash; }));
			if (!authorized) {
				CHECK(session.journal.plan->cleanup == SyncCarryCleanup::None);
				CHECK(!session.journal.plan->cleanupAuthorized);
				CHECK(!CheckSyncCarryDeletion(session.journal, other, cloud.inventory(next)));
				session.run();
				CHECK(cloud.has(old, other.reference.recordHash));
				continue;
			}
			CHECK(CheckSyncCarryDeletion(session.journal, other, cloud.inventory(next)));
			auto incomplete = cloud.inventory(next, false);
			CHECK(!CheckSyncCarryDeletion(session.journal, other, incomplete));
			auto missing = cloud.inventory(next);
			missing.messages.erase(std::remove_if(missing.messages.begin(), missing.messages.end(),
				[&](const auto &message) { return message.reference.recordHash == other.reference.recordHash; }),
				missing.messages.end());
			CHECK(!CheckSyncCarryDeletion(session.journal, other, missing));
			for (auto variant = 0; variant != 7; ++variant) {
				auto changed = other;
				if (variant == 0) changed.reference.account.userId = 778;
				if (variant == 1) changed.reference.chat = next;
				if (variant == 2) changed.ownership = SyncChatMessageOwnership::Unknown;
				if (variant == 3) changed.ownership = SyncChatMessageOwnership::Other;
				if (variant == 4) changed.bytes += ' ';
				if (variant == 5) ++changed.date;
				if (variant == 6) changed.family = SyncCarryFamily::Playlists;
				CHECK(!CheckSyncCarryDeletion(session.journal, changed, cloud.inventory(next)));
			}
			const auto late = b.publish(cloud, Text('b', 2));
			REQUIRE(cloud.forward(late.reference, next));
			CHECK(!CheckSyncCarryDeletion(session.journal, late, cloud.inventory(next)));
			auto backdated = late;
			backdated.date = other.date;
			CHECK(!CheckSyncCarryDeletion(session.journal, backdated, cloud.inventory(next)));
			for (const auto &bytes : { QByteArray("version = 1\n"), QByteArray("{\"message\":\"not Purple\"}") }) {
				const auto unrelated = cloud.post(old, bytes);
				CHECK(!CheckSyncCarryDeletion(session.journal, unrelated, cloud.inventory(next)));
				auto forged = session.journal;
				auto metadata = forged.plan->cleanupCandidates.front();
				metadata.reference = unrelated.reference;
				metadata.date = unrelated.date;
				forged.plan->cleanupCandidates.push_back(metadata);
				REQUIRE(SerializeSyncCarryJournal(forged));
				CHECK(!CheckSyncCarryDeletion(forged, unrelated, cloud.inventory(next)));
			}
			auto unticked = session.journal;
			unticked.plan->cleanup = SyncCarryCleanup::None;
			unticked.plan->cleanupAuthorized = false;
			CHECK(!CheckSyncCarryDeletion(unticked, other, cloud.inventory(next)));
		}
	}
}

void ReviewMarkOrdering() {
	Section = "SAME_SECOND winner ordering";
	for (const auto oldLarger : { false, true }) {
		for (const auto delta : { -1, 0, 1 }) {
			auto cloud = Cloud();
			const auto old = Peer(oldLarger ? u"chat:20"_q : u"chat:10"_q);
			const auto next = Peer(oldLarger ? u"chat:10"_q : u"chat:20"_q);
			cloud.mark(old);
			auto device = Device('a', old, cloud.cache(old));
			device.publish(cloud, Text('a'));
			auto session = Session(device, cloud, next);
			session.run(SyncCarryPhase::Mark);
			cloud.clock = session.journal.oldMark->markDate + delta - 1;
			session.run();
			const auto wins = delta > 0 || (delta == 0 && !oldLarger);
			const auto winner = Resolution(device, cloud).winner;
			REQUIRE(winner);
			CHECK(SyncChatMarkOutranks(cloud.cache(next), *session.journal.oldMark) == wins);
			CHECK(winner->chat == (wins ? next : old));
			CHECK((session.journal.phase == SyncCarryPhase::Done) == wins);
			CHECK(session.held == !wins);
			CHECK(device.state.chat == (wins ? next : old));
			CHECK(device.cache.chat == (wins ? next : old));
			if (wins) {
				auto corrupted = QJsonDocument::fromJson(SerializeSyncCarryJournal(session.journal).canonical).object();
				auto target = corrupted.value(u"target_mark"_q).toObject();
				target.insert(u"date"_q, QJsonValue(qint64(session.journal.oldMark->markDate - 1)));
				corrupted.insert(u"target_mark"_q, target);
				CHECK(!ParseSyncCarryJournal(QJsonDocument(corrupted).toJson()));
			}
			if (!wins) {
				CHECK(session.journal.phase == SyncCarryPhase::Mark);
				CHECK(!session.journal.stateCommitted && !session.journal.cacheCompleted);
			}
		}
	}
	const auto chat = Peer(u"chat:20"_q);
	CHECK(SyncChatMarkOutranks({ chat, 2, 1001 }, { chat, 1, 1001 }));
	CHECK(!SyncChatMarkOutranks({ chat, 1, 1001 }, { chat, 2, 1001 }));
	CHECK(!SyncChatMarkOutranks({ chat, 1, 1001 }, { chat, 1, 1001 }));
	CHECK(!SyncChatMarkOutranks({ chat, -1, 1001 }, { chat, 1, 1001 }));
}

void ReviewDuplicateLedger() {
	Section = "DUPLICATE_257 bounded ledger";
	auto cloud = Cloud();
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	cloud.mark(old);
	auto device = Device('a', old, cloud.cache(old));
	const auto first = device.publish(cloud, Text('a'));
	const auto current = device.publish(cloud, Text('a', 2));
	for (auto i = 0; i != 256; ++i) cloud.post(next, current.bytes);
	cloud.post(next, first.bytes);
	cloud.post(next, first.bytes);
	const auto planned = PlanCarry(Input(device, cloud, next));
	REQUIRE(planned);
	const auto before = PrepareSyncCarryCommit(planned.plan, device.state, cloud.inventory(next));
	REQUIRE(before);
	CHECK(before.state->ownMessages.size() == 2);
	cloud.post(next, current.bytes);
	const auto after = PrepareSyncCarryCommit(planned.plan, device.state, cloud.inventory(next));
	REQUIRE(after);
	CHECK(after.canonical == before.canonical);
	CHECK(after.state->ownMessages.size() <= after.state->config.issuedRecords.size());
	auto reversed = cloud.inventory(next);
	std::reverse(reversed.messages.begin(), reversed.messages.end());
	CHECK(PrepareSyncCarryCommit(planned.plan, device.state, reversed).canonical == before.canonical);
	CHECK(after.state->ownMessages.front().messageId == 1);
}

void OldMarkCleanup() {
	Section = "explicit old mark cleanup";
	auto cloud = Cloud();
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	cloud.mark(old);
	auto device = Device('a', old, cloud.cache(old));
	device.publish(cloud, Text('a'));
	auto session = Session(device, cloud, next, SyncCarryMode::Switch,
		SyncCarrySourceAccess::Readable, true, true);
	session.run(SyncCarryPhase::Cleanup);
	const auto originalMark = cloud.marks.at(cloud.key(old));
	CHECK(CheckSyncCarryOldMarkDeletion(session.journal, originalMark, cloud.inventory(next)));
	auto wrong = originalMark;
	wrong.chat = next;
	CHECK(!CheckSyncCarryOldMarkDeletion(session.journal, wrong, cloud.inventory(next)));
	wrong = originalMark;
	wrong.ownership = SyncChatMessageOwnership::Other;
	CHECK(!CheckSyncCarryOldMarkDeletion(session.journal, wrong, cloud.inventory(next)));
	auto event = SyncCarryEvent(session.journal, SyncCarryEventKind::CleanupCompleted);
	event.outcome = SyncCarryOutcome::Success;
	event.state = device.state;
	event.destination = cloud.inventory(next);
	event.deleted = SyncCarryCleanupCandidates(*session.journal.plan, *event.destination);
	event.mark = originalMark;
	const auto stillPresent = AdvanceSyncCarryJournal(session.journal, event);
	CHECK(stillPresent.status == SyncCarryJournalStatus::Wait);
	CHECK(stillPresent.journal.phase == SyncCarryPhase::Cleanup);
	session.store(stillPresent);
	session.crashBeforeAck = SyncCarryPhase::Cleanup;
	session.run();
	CHECK(session.journal.phase == SyncCarryPhase::Done);
	CHECK(!cloud.marks.contains(cloud.key(old)));
	CHECK(cloud.marks.contains(cloud.key(next)));
	CHECK(cloud.inventory(old).messages.empty());
}

void CrashMatrix() {
	Section = "journal crash matrix";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	auto visited = std::set<SyncCarryPhase>();
	for (auto scenario = 0; scenario != 3; ++scenario) {
		const auto lost = scenario == 2;
		for (auto phase = 0; phase <= int(SyncCarryPhase::Cleanup); ++phase) {
			auto cloud = Cloud();
			cloud.mark(old);
			auto device = Device('a', old, cloud.cache(old));
			device.publish(cloud, Text('a'));
			if (scenario != 0) device.prepare(Text('a', 2));
			auto session = Session(device, cloud, next, SyncCarryMode::Switch,
				lost ? SyncCarrySourceAccess::ChatGone : SyncCarrySourceAccess::Readable);
			session.crashBefore = SyncCarryPhase(phase);
			session.run();
			CHECK(session.journal.phase == SyncCarryPhase::Done);
			CHECK(device.state.chat == next && device.cache.chat == next);
			CHECK(device.state.config.pendingSeq == 0);
			CHECK(device.state.config.seq == (lost ? 3 : scenario == 1 ? 2 : 1));
			CHECK(device.state.config.confirmedSeq == device.state.config.seq);
			CHECK(device.stage.isEmpty());
			CHECK(device.localText == Text('a', scenario ? 2 : 1));
			visited.insert(session.visited.begin(), session.visited.end());
		}
	}
	for (auto phase = 0; phase <= int(SyncCarryPhase::Cleanup); ++phase) {
		CHECK(visited.contains(SyncCarryPhase(phase)));
	}
	for (const auto phase : {
		SyncCarryPhase::WritePendingState, SyncCarryPhase::RetirePendingStage,
		SyncCarryPhase::StageOwn, SyncCarryPhase::Deliver, SyncCarryPhase::Mark,
		SyncCarryPhase::PrepareCache, SyncCarryPhase::CommitState,
		SyncCarryPhase::RetireOwnStage, SyncCarryPhase::CompleteCache, SyncCarryPhase::Cleanup,
	}) {
		auto cloud = Cloud();
		cloud.mark(old);
		auto device = Device('a', old, cloud.cache(old));
		device.publish(cloud, Text('a'));
		device.prepare(Text('a', 2));
		auto session = Session(device, cloud, next, SyncCarryMode::Switch, SyncCarrySourceAccess::ChatGone);
		session.crashBeforeAck = phase;
		session.run();
		CHECK(session.crashed);
		CHECK(session.journal.phase == SyncCarryPhase::Done);
		CHECK(device.state.config.seq == 3 && device.state.config.confirmedSeq == 3);
		CHECK(device.state.chat == next && device.cache.chat == next);
		CHECK(device.stage.isEmpty());
	}
}

void SplitStagePersistence() {
	Section = "split carry stage persistence";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	for (auto part = 0; part != 3; ++part) {
		auto cloud = Cloud();
		cloud.mark(old);
		auto device = Device('a', old, cloud.cache(old));
		device.publish(cloud, Text('a'));
		auto session = Session(device, cloud, next, SyncCarryMode::Switch, SyncCarrySourceAccess::Protected);
		session.run(SyncCarryPhase::StageOwn);
		const auto reserved = PrepareSyncCarryStage(*session.journal.plan, device.state);
		REQUIRE(reserved);
		if (part != 1) device.stage = session.journal.plan->ownRecord;
		if (part != 0) device.state = *reserved;
		session.store(ResumeSyncCarryJournal(session.journal, device.state, false, true));
		CHECK(session.journal.phase == SyncCarryPhase::StageOwn);
		session.run();
		CHECK(session.journal.phase == SyncCarryPhase::Done);
		CHECK(device.state.config.seq == 2 && device.state.config.confirmedSeq == 2);
		CHECK(device.stage.isEmpty());
	}
	auto cloud = Cloud();
	cloud.mark(old);
	auto device = Device('a', old, cloud.cache(old));
	device.prepare(Text('a'));
	auto firstLost = Session(device, cloud, next, SyncCarryMode::Switch, SyncCarrySourceAccess::ChatGone);
	firstLost.run();
	CHECK(device.state.chat == next && device.state.config.abandonedSeq == 1);
	CHECK(device.state.config.confirmedSeq == 0 && device.state.config.seq == 1);
	CHECK(device.localText == Text('a'));
	CHECK(cloud.inventory(next).messages.empty());
	device.publish(cloud, device.localText);
	CHECK(device.state.config.seq == 2 && device.state.config.confirmedSeq == 2);
	CHECK(device.state.config.abandonedSeq == 0);
}

void FollowAndProtectedSources() {
	Section = "follow, late posts and inaccessible sources";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	for (const auto access : {
		SyncCarrySourceAccess::Readable, SyncCarrySourceAccess::Protected,
		SyncCarrySourceAccess::Unreadable, SyncCarrySourceAccess::ChatGone,
	}) {
		auto cloud = Cloud();
		cloud.mark(old);
		auto devices = std::vector<Device>();
		for (auto i = 0; i != 5; ++i) {
			devices.emplace_back(char('a' + i), old, cloud.cache(old));
			devices.back().publish(cloud, Text(char('a' + i)));
		}
		auto first = Session(devices[0], cloud, next, SyncCarryMode::Switch, access);
		first.run();
		REQUIRE(first.journal.phase == SyncCarryPhase::Done);
		if (access != SyncCarrySourceAccess::Readable) {
			CHECK(first.journal.plan->selected.empty());
			CHECK(ParseSyncEnvelope(first.journal.plan->ownRecord).header->writerInstall == devices[0].state.install);
			CHECK(cloud.inventory(next).messages.size() == 1);
		}
		for (auto i = 1; i != 5; ++i) {
			if (access == SyncCarrySourceAccess::Readable) {
				devices[i].publish(cloud, Text(char('a' + i), 2));
			}
			auto follow = Session(devices[i], cloud, next, SyncCarryMode::Follow, access, true);
			follow.run();
			CHECK(follow.journal.phase == SyncCarryPhase::Done);
			CHECK(devices[i].state.chat == next);
			if (access == SyncCarrySourceAccess::Readable) {
				const auto source = cloud.inventory(old);
				CHECK(std::none_of(source.messages.begin(), source.messages.end(), [&](const auto &message) {
					const auto header = ParseSyncEnvelope(message.bytes).header;
					return header && header->writerInstall == devices[i].state.install
						&& message.date > follow.journal.targetMark->markDate;
				}));
			}
		}
		for (const auto &device : devices) {
			const auto destination = cloud.inventory(next);
			CHECK(std::any_of(destination.messages.begin(), destination.messages.end(), [&](const auto &message) {
				const auto envelope = ParseSyncEnvelope(message.bytes);
				return envelope.header && envelope.header->writerInstall == device.state.install
					&& envelope.header->seq == device.state.config.confirmedSeq
					&& InspectConfigPayload(envelope).version.key == device.baseVersion.key;
			}));
		}
	}
	auto cloud = Cloud();
	cloud.mark(PeerToken());
	auto implicit = Device('a', PeerToken(), cloud.cache(PeerToken()));
	const auto original = implicit.publish(cloud, Text('a'));
	cloud.mark(next);
	implicit.publish(cloud, Text('a', 2));
	auto selfFollow = Session(implicit, cloud, next, SyncCarryMode::Follow,
		SyncCarrySourceAccess::Readable, true);
	selfFollow.run();
	CHECK(implicit.state.version == 2);
	CHECK(cloud.has(PeerToken(), original.reference.recordHash));
	CHECK(!cloud.inventory(PeerToken()).messages.empty());
	CHECK(SyncCarryCleanupCandidates(*selfFollow.journal.plan, cloud.inventory(next)).empty());
}

void FillContracts() {
	Section = "fill without local state";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	for (auto crash = int(SyncCarryPhase::ResolveAgain); crash <= int(SyncCarryPhase::Cleanup); ++crash) {
		auto cloud = Cloud();
		cloud.mark(old);
		auto device = Device('a', old, cloud.cache(old));
		const auto record = device.publish(cloud, Text('a'));
		cloud.mark(next);
		auto resolver = SyncChatResolveInput();
		for (const auto &peer : { old, next }) {
			auto mark = SyncChatMark();
			const auto cache = cloud.cache(peer);
			mark.location = cache;
			mark.status = SyncChatMarkStatus::Supported;
			mark.eligibility = Eligible(peer);
			mark.readable = mark.freshlyRead = true;
			mark.records = peer == old ? SyncChatRecordPresence::Present : SyncChatRecordPresence::Empty;
			resolver.marks.push_back(mark);
		}
		const auto resolution = ResolveSyncChat(resolver);
		REQUIRE(resolution.action == SyncChatAction::Fill);
		auto start = SyncCarryJournalStart();
		start.operationId = QString(32, u'a');
		start.account = Account;
		start.source = old;
		start.target = next;
		start.mode = SyncCarryMode::Fill;
		start.stateAbsentConfirmed = true;
		start.operationConfirmed = true;
		start.resolution = resolution;
		start.targetEligibility = Eligible(next);
		start.ownRequestId = u"42"_q;
		auto begun = BeginSyncCarryJournal(start);
		REQUIRE(begun);
		auto journal = begun.journal;
		auto crashed = false;
		auto done = false;
		for (auto step = 0; step != 40; ++step) {
			if (journal.phase == SyncCarryPhase::Done) {
				done = true;
				break;
			}
			if (!crashed && int(journal.phase) == crash) {
				crashed = true;
				const auto resumed = ResumeSyncCarryJournal(journal, std::nullopt, true, true);
				REQUIRE(!resumed.canonical.isEmpty());
				journal = ParseSyncCarryJournal(resumed.canonical).journal;
				continue;
			}
			auto event = SyncCarryEvent(journal, SyncCarryEventKind::Resolution);
			event.outcome = SyncCarryOutcome::Success;
			event.objectsDurable = true;
			event.stateAbsentConfirmed = true;
			switch (journal.phase) {
			case SyncCarryPhase::ResolveAgain:
				event.resolution = resolution;
				break;
			case SyncCarryPhase::Plan: {
				auto input = SyncCarryInput();
				input.account = Account;
				input.source = old;
				input.target = next;
				input.mode = SyncCarryMode::Fill;
				input.access = SyncCarrySourceAccess::Readable;
				input.targetEligibility = Eligible(next);
				input.oldInventory = cloud.inventory(old);
				input.newInventory = cloud.inventory(next);
				input.winner = resolution.winner;
				const auto plan = PlanCarry(input);
				REQUIRE(plan);
				CHECK(plan.plan.ownRecord.isEmpty() && plan.plan.stateHash.isEmpty());
				event.kind = SyncCarryEventKind::PlanPrepared;
				event.plan = plan.plan;
			} break;
			case SyncCarryPhase::Deliver: {
				const auto forwards = SyncCarryJournalForwards(journal);
				event.kind = SyncCarryEventKind::DeliveryPrepared;
				const auto intent = AdvanceSyncCarryJournal(journal, event);
				REQUIRE(!intent.canonical.isEmpty());
				journal = ParseSyncCarryJournal(intent.canonical).journal;
				for (const auto &reference : forwards) REQUIRE(cloud.forward(reference, next));
				continue;
			}
			case SyncCarryPhase::VerifyDestination:
				event.kind = SyncCarryEventKind::DestinationRead;
				event.destination = cloud.inventory(next);
				break;
			case SyncCarryPhase::Mark:
				CHECK(NextSyncCarryJournalAction(journal) == SyncCarryJournalAction::ReadMark);
				event.kind = SyncCarryEventKind::MarkRead;
				event.mark = cloud.marks.at(cloud.key(next));
				break;
			case SyncCarryPhase::PrepareCache:
				event.kind = SyncCarryEventKind::CachePrepared;
				event.cache = { journal.operationId, Account, *journal.targetMark, false };
				break;
			case SyncCarryPhase::CommitState: {
				event.kind = SyncCarryEventKind::StateObserved;
				event.objectsDurable = false;
				const auto blocked = AdvanceSyncCarryJournal(journal, event);
				CHECK(blocked.status == SyncCarryJournalStatus::Wait);
				CHECK(blocked.journal.phase == SyncCarryPhase::CommitState);
				CHECK(!blocked.journal.stateCommitted);
				event.objectsDurable = true;
			} break;
			case SyncCarryPhase::CompleteCache:
				event.kind = SyncCarryEventKind::CacheCompleted;
				event.cache = { journal.operationId, Account, *journal.targetMark, true };
				break;
			case SyncCarryPhase::Cleanup:
				event.kind = SyncCarryEventKind::CleanupCompleted;
				event.destination = cloud.inventory(next);
				CHECK(!CheckSyncCarryDeletion(journal, record, *event.destination));
				break;
			default: REQUIRE(false);
			}
			const auto advanced = AdvanceSyncCarryJournal(journal, event);
			REQUIRE(!advanced.canonical.isEmpty());
			journal = ParseSyncCarryJournal(advanced.canonical).journal;
		}
		CHECK(done);
		CHECK(journal.stateHash.isEmpty() && journal.nextStateHash.isEmpty());
		CHECK(cloud.has(next, record.reference.recordHash));
		CHECK(cloud.has(old, record.reference.recordHash));
		CHECK(device.state.chat == old && device.state.version == 2);
	}
}

void PlaylistHookAndReadback() {
	Section = "playlist hook and readback refusal";
	auto cloud = Cloud();
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	cloud.mark(old);
	auto device = Device('a', old, cloud.cache(old));
	device.publish(cloud, Text('a'));
	cloud.post(old, QByteArray("opaque future library"), SyncCarryFamily::Playlists);
	auto input = Input(device, cloud, next);
	const auto planned = PlanCarry(input);
	REQUIRE(planned);
	CHECK(planned.plan.playlistRequired);
	for (const auto &reference : planned.plan.forwards) REQUIRE(cloud.forward(reference, next));
	CHECK(VerifySyncCarryReadback(planned.plan, cloud.inventory(next)).status == SyncCarryStatus::PlaylistRequired);
	auto receipt = SyncPlaylistCarryReceipt();
	receipt.status = SyncPlaylistCarryStatus::Verified;
	receipt.account = Account;
	receipt.source = old;
	receipt.target = next;
	receipt.planStamp = SyncCarryPlanStamp(planned.plan);
	receipt.destinationStamp = SelectSyncCarryHeads(cloud.inventory(next)).stamp;
	CHECK(VerifySyncCarryReadback(planned.plan, cloud.inventory(next), receipt));
	CHECK(planned.plan.sourceStamp == SelectSyncCarryHeads(cloud.inventory(old)).stamp);
	auto otherSnapshot = planned.plan;
	otherSnapshot.sourceStamp = QString(64, u'a');
	CHECK(VerifySyncCarryReadback(otherSnapshot, cloud.inventory(next), receipt).status == SyncCarryStatus::PlaylistRequired);
	receipt.target = old;
	CHECK(VerifySyncCarryReadback(planned.plan, cloud.inventory(next), receipt).status == SyncCarryStatus::PlaylistRequired);
	receipt.target = next;
	receipt.destinationStamp = QString(64, u'a');
	CHECK(VerifySyncCarryReadback(planned.plan, cloud.inventory(next), receipt).status == SyncCarryStatus::PlaylistRequired);
	const auto raw = cloud.inventory(old).messages.front().bytes;
	auto document = QJsonDocument::fromJson(raw).object();
	document.insert(u"stream"_q, u"library"_q);
	const auto library = CanonicalizeSyncJson(QJsonDocument(document).toJson());
	REQUIRE(library);
	const auto another = Peer(u"chat:30"_q);
	cloud.post(another, library.canonical);
	const auto librarySelection = SelectSyncCarryHeads(cloud.inventory(another));
	CHECK(librarySelection.status == SyncCarryStatus::Ready);
	CHECK(librarySelection.playlists && librarySelection.heads.empty());
	input = Input(device, cloud, another);
	CHECK(PlanCarry(input).plan.playlistRequired);
	auto broken = cloud.inventory(next);
	broken.messages.front().ownership = SyncChatMessageOwnership::Other;
	CHECK(VerifySyncCarryReadback(planned.plan, broken).status == SyncCarryStatus::MissingRecords);
	broken = cloud.inventory(next);
	broken.messages.front().bytes += ' ';
	CHECK(VerifySyncCarryReadback(planned.plan, broken).status == SyncCarryStatus::WrongScope);
}

void SeededDevices() {
	Section = "seeded multi-device moves";
	for (auto seed = 0; seed != 12; ++seed) {
		auto random = std::mt19937_64(0xb5 + seed);
		auto cloud = Cloud();
		auto current = Peer(u"chat:10"_q);
		cloud.mark(current);
		auto devices = std::vector<Device>();
		const auto count = 3 + seed % 3;
		for (auto i = 0; i != count; ++i) {
			devices.emplace_back(char('a' + i), current, cloud.cache(current));
			devices.back().publish(cloud, Text(char('a' + i)));
		}
		const auto future = cloud.post(current, QByteArray("{\"purple_sync\":2,\"opaque\":true}"));
		for (auto move = 0; move != 8; ++move) {
			const auto author = int(random() % count);
			if (devices[author].state.chat != current) {
				auto follow = Session(devices[author], cloud, current, SyncCarryMode::Follow);
				follow.run();
			}
			const auto target = Peer(u"chat:%1"_q.arg(100 + move * 2));
			if (move == 3) {
				const auto other = (author + 1) % count;
				if (devices[other].state.chat != current) {
					auto follow = Session(devices[other], cloud, current, SyncCarryMode::Follow);
					follow.run();
				}
				const auto second = Peer(u"chat:%1"_q.arg(101 + move * 2));
				auto a = Session(devices[author], cloud, target);
				a.run(SyncCarryPhase::Mark);
				auto b = Session(devices[other], cloud, second);
				b.run(SyncCarryPhase::Mark);
				a.run();
				b.run();
				current = second;
			} else {
				auto session = Session(devices[author], cloud, target);
				if (move % 2) session.crashBefore = SyncCarryPhase::CommitState;
				session.run();
				current = target;
			}
			for (auto i = 0; i != count; ++i) {
				if (devices[i].state.chat == current || random() % 3 == 0) continue;
				if (random() % 2) devices[i].publish(cloud, Text(char('a' + i), move + 2));
				auto follow = Session(devices[i], cloud, current, SyncCarryMode::Follow,
					SyncCarrySourceAccess::Readable, true);
				follow.run();
			}
		}
		for (auto &device : devices) {
			if (device.state.chat != current) {
				auto follow = Session(device, cloud, current, SyncCarryMode::Follow);
				follow.run();
			}
			CHECK(device.state.chat == current && device.cache.chat == current);
			const auto forwardOrder = Resolution(device, cloud);
			const auto reverseOrder = Resolution(device, cloud, true);
			CHECK(forwardOrder.action == reverseOrder.action);
			CHECK(forwardOrder.winner == reverseOrder.winner);
			CHECK(forwardOrder.cache == reverseOrder.cache);
			const auto destination = cloud.inventory(current);
			CHECK(std::any_of(destination.messages.begin(), destination.messages.end(), [&](const auto &message) {
				const auto header = ParseSyncEnvelope(message.bytes).header;
				return header && header->writerInstall == device.state.install
					&& header->seq == device.state.config.confirmedSeq;
			}));
		}
		CHECK(cloud.has(current, future.reference.recordHash));
	}
}

}

int main() {
	PendingContracts();
	CarryContracts();
	JournalContracts();
	CloneAndPlanBinding();
	LegacyCarry();
	ReviewDurability();
	ReviewCleanup();
	ReviewMarkOrdering();
	ReviewDuplicateLedger();
	OldMarkCleanup();
	CrashMatrix();
	SplitStagePersistence();
	FollowAndProtectedSources();
	FillContracts();
	PlaylistHookAndReadback();
	SeededDevices();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
