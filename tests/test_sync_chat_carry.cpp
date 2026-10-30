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
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <tuple>

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
	QString loginAuthority;

	Session(
			Device &d,
			Cloud &c,
			const PeerToken &n,
			SyncCarryMode m = SyncCarryMode::Switch,
			SyncCarrySourceAccess a = SyncCarrySourceAccess::Readable,
			bool cleanup = false,
			bool removeMark = false,
			QString login = {})
	: device(d), cloud(c), source(d.state.chat), target(n), mode(m), access(a)
	, initial(Resolution(d, c)), authorizedCleanup(cleanup), loginAuthority(std::move(login)) {
		if (mode == SyncCarryMode::Switch) {
			initial.action = SyncChatAction::Use;
			initial.cache = device.cache;
			initial.winner = device.cache;
			initial.source.reset();
		}
		auto start = SyncCarryJournalStart();
		start.loginAuthority = loginAuthority;
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
		store(ResumeSyncCarryJournal(parsed.journal, device.state, false, true, loginAuthority));
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
			store(AdvanceSyncCarryJournal(journal, event, loginAuthority));
			CHECK(journal.phase == SyncCarryPhase::VerifyDestination);
			for (const auto &reference : forwards) {
				REQUIRE(cloud.forward(reference, target));
			}
			if (own) cloud.post(target, ownBytes);
			if (restart(phase, true)) return;
			auto result = SyncCarryEvent(journal, SyncCarryEventKind::DeliveryResult);
			result.outcome = ambiguous ? SyncCarryOutcome::Unknown : SyncCarryOutcome::Success;
			store(AdvanceSyncCarryJournal(journal, result, loginAuthority));
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
				store(AdvanceSyncCarryJournal(journal, event, loginAuthority));
				CHECK(NextSyncCarryJournalAction(journal) == SyncCarryJournalAction::ReadMark);
				cloud.mark(target, text, request);
				if (restart(phase, true)) return;
				auto result = SyncCarryEvent(journal, SyncCarryEventKind::MarkPostResult);
				result.outcome = ambiguous ? SyncCarryOutcome::Unknown : SyncCarryOutcome::Success;
				store(AdvanceSyncCarryJournal(journal, result, loginAuthority));
				return;
			}
			event.kind = SyncCarryEventKind::MarkRead;
			event.mark = cloud.marks.at(cloud.key(target));
			break;
		case SyncCarryPhase::PrepareCache:
			event.kind = SyncCarryEventKind::CachePrepared;
			event.cache = { journal.operationId, Account, *SyncCarryJournalCacheMark(journal), false };
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
			event.cache = { journal.operationId, Account, *SyncCarryJournalCacheMark(journal), true };
			device.cache = *SyncCarryJournalCacheMark(journal);
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
		case SyncCarryPhase::CleanupSkipped:
		case SyncCarryPhase::InterruptedAbandoned:
		case SyncCarryPhase::FinishedForFollow:
		case SyncCarryPhase::Aborted:
			return;
		}
		store(AdvanceSyncCarryJournal(journal, event, loginAuthority));
	}

	void run(std::optional<SyncCarryPhase> stop = std::nullopt) {
		for (auto stepCount = 0; stepCount != 100; ++stepCount) {
			if (held || journal.phase == SyncCarryPhase::Done || journal.phase == SyncCarryPhase::Aborted
				|| journal.phase == SyncCarryPhase::FinishedForFollow || stop == journal.phase) {
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

SyncCarryJournalResult DeltaAdvance(
		const SyncCarryJournal &journal,
		const SyncCarryJournalEvent &event) {
	return AdvanceSyncCarryJournal(journal, event, journal.loginAuthority);
}

SyncCarryJournalResult DeltaResume(
		const SyncCarryJournal &journal,
		const std::optional<SyncLocalState> &state,
		bool absent,
		bool durable = false) {
	return ResumeSyncCarryJournal(journal, state, absent, durable, journal.loginAuthority);
}

SyncCarryJournalResult DeltaStore(const SyncCarryJournalResult &result) {
	REQUIRE(!result.canonical.isEmpty());
	const auto parsed = ParseSyncCarryJournal(result.canonical);
	REQUIRE(parsed);
	CHECK(SerializeSyncCarryJournal(parsed.journal).canonical == result.canonical);
	return parsed;
}

void ProtectedStep(Session &s, SyncCarrySendIntent intent) {
	const auto phase = s.journal.phase;
	if (s.restart(phase, false)) return;
	auto event = SyncCarryEvent(s.journal, SyncCarryEventKind::Resolution);
	event.outcome = SyncCarryOutcome::Success;
	event.objectsDurable = true;
	event.state = s.device.state;
	if (phase == SyncCarryPhase::ResolvePending) {
		event.kind = SyncCarryEventKind::PendingPrepared;
		event.stagedRecord = s.device.stage;
		event.retainedLocalText = s.device.localText;
		event.pendingAccess = SyncCarrySourceAccess::Protected;
		event.pendingSendIntent = intent;
		event.pendingInventory = s.cloud.inventory(s.source);
		const auto prepared = ReconcileProtectedStageForCarry(Account, s.device.state, s.source,
			event.pendingAccess, intent, s.device.stage, s.device.localText, event.pendingInventory);
		REQUIRE(prepared);
		event.preparedImageHash = SyncCarryRecordHash(StateBytes(prepared.state));
		s.device.images[event.preparedImageHash] = StateBytes(prepared.state);
		if (!prepared.restageInTarget) {
			s.device.baseText = s.device.localText;
			s.device.baseVersion = InspectConfigPayload(ParseSyncEnvelope(s.device.stage)).version;
		}
	} else if (phase == SyncCarryPhase::VerifyDestination) {
		event.kind = SyncCarryEventKind::DestinationRead;
		event.destination = s.cloud.inventory(s.target);
		if (VerifySyncCarryReadback(*s.journal.plan, *event.destination)) {
			const auto prepared = PrepareSyncCarryJournalCommit(s.journal, s.device.state, *event.destination);
			REQUIRE(prepared.commit);
			event.preparedImageHash = prepared.commit.stateHash;
			event.targetPendingRecord = prepared.targetPendingRecord;
			s.device.images[event.preparedImageHash] = prepared.commit.canonical;
		}
		if (s.restart(phase, true)) return;
	} else if (phase == SyncCarryPhase::RetireOwnStage && !s.journal.targetPendingRecord.isEmpty()) {
		event.kind = SyncCarryEventKind::StageRetired;
		event.retiredStageHash = s.journal.plan->ownRecordHash;
		s.device.stage = s.journal.targetPendingRecord;
		event.targetPendingStageDurable = true;
		event.stagedRecord = s.device.stage;
		if (s.restart(phase, true)) return;
	} else if (phase == SyncCarryPhase::CompleteCache && !s.journal.targetPendingRecord.isEmpty()) {
		event.kind = SyncCarryEventKind::CacheCompleted;
		event.cache = { s.journal.operationId, Account, *SyncCarryJournalCacheMark(s.journal), true };
		s.device.stage = s.journal.targetPendingRecord;
		event.targetPendingStageDurable = true;
		event.stagedRecord = s.device.stage;
		s.device.cache = *SyncCarryJournalCacheMark(s.journal);
		if (s.restart(phase, true)) return;
	} else {
		s.step();
		return;
	}
	if ((phase == SyncCarryPhase::RetireOwnStage || phase == SyncCarryPhase::CompleteCache)
		&& !s.journal.targetPendingRecord.isEmpty()) {
		for (auto invalid = 0; invalid != 3; ++invalid) {
			auto bad = event;
			if (invalid == 0) bad.targetPendingStageDurable = false;
			if (invalid == 1) bad.stagedRecord = "wrong stage";
			if (invalid == 2) bad.objectsDurable = false;
			CHECK(DeltaAdvance(s.journal, bad).canonical.isEmpty());
		}
	}
	s.store(DeltaAdvance(s.journal, event));
}

void ProtectedPendingDelta() {
	Section = "protected pending delta";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	for (const auto intent : { SyncCarrySendIntent::NotIssued, SyncCarrySendIntent::Issued }) {
		for (auto found = 0; found != 2; ++found) {
			if (found && intent == SyncCarrySendIntent::NotIssued) continue;
			for (auto crash = -1; crash <= int(SyncCarryPhase::Cleanup); ++crash) {
				for (auto after = 0; after != 2; ++after) {
					auto cloud = Cloud();
					cloud.mark(old);
					auto d = Device('a', old, cloud.cache(old));
					d.publish(cloud, Text('a'));
					d.prepare(Text('a', 2));
					const auto pending = d.stage;
					const auto payload = InspectConfigPayload(ParseSyncEnvelope(pending));
					if (found) cloud.post(old, pending);
					const auto sourceCount = cloud.inventory(old).messages.size();
					auto session = Session(d, cloud, next, SyncCarryMode::Switch, SyncCarrySourceAccess::Protected, false, false, QString(32, u'a'));
					if (crash >= 0) {
						if (after) session.crashBeforeAck = SyncCarryPhase(crash);
						else session.crashBefore = SyncCarryPhase(crash);
					}
					for (auto n = 0; n != 100 && session.journal.phase != SyncCarryPhase::Done; ++n) {
						CHECK(SyncCarryJournalHoldsWrites(session.journal));
						CHECK(!cloud.has(next, SyncCarryRecordHash(pending)));
						ProtectedStep(session, intent);
					}
					REQUIRE(session.journal.phase == SyncCarryPhase::Done);
					CHECK(!SyncCarryJournalHoldsWrites(session.journal));
					CHECK(d.state.chat == next);
					CHECK(cloud.inventory(old).messages.size() == sourceCount);
					if (found) {
						CHECK(d.stage.isEmpty());
						CHECK(d.state.configData.base == payload.version.key);
						CHECK(d.state.config.pendingSeq == 0);
					} else {
						REQUIRE(!d.stage.isEmpty());
						const auto restaged = InspectConfigPayload(ParseSyncEnvelope(d.stage));
						CHECK(restaged.text == payload.text);
						CHECK(restaged.version.key == payload.version.key);
						CHECK(restaged.version.parents == payload.version.parents);
						CHECK(restaged.version.lineage == payload.version.lineage);
						CHECK(d.state.configData.pending == payload.version.key);
						CHECK(d.state.config.pendingSeq == d.state.config.seq);
						CHECK(d.state.config.seq > 2);
						CHECK(!cloud.has(next, SyncCarryRecordHash(d.stage)));
						const auto posted = cloud.post(next, d.stage);
						CHECK(FinishPendingStageForCarry(Account, d.state, d.stage, posted));
					}
				}
			}
		}
	}
	auto cloud = Cloud();
	cloud.mark(old);
	auto d = Device('a', old, cloud.cache(old));
	d.publish(cloud, Text('a'));
	d.prepare(Text('a', 2));
	for (auto variant = 0; variant != 12; ++variant) {
		auto account = Account;
		auto state = d.state;
		auto access = SyncCarrySourceAccess::Protected;
		auto source = old;
		auto intent = SyncCarrySendIntent::Issued;
		auto inventory = cloud.inventory(old);
		auto stage = d.stage;
		auto text = d.localText;
		if (variant == 0) intent = SyncCarrySendIntent::Unknown;
		if (variant == 1) inventory.complete = false;
		if (variant == 2) inventory.chat = next;
		if (variant == 3) inventory.account.environment = 1;
		if (variant == 4) inventory.messages.front().ownership = SyncChatMessageOwnership::Unknown;
		if (variant == 5) stage = "bad";
		if (variant == 6) text = Text('z');
		if (variant == 7) source = next;
		if (variant == 8) access = SyncCarrySourceAccess::ChatGone;
		if (variant == 9) access = SyncCarrySourceAccess::Readable;
		if (variant == 10) state = ReserveSyncSeq(state, SyncLocalStream::Library, QString(64, u'f')).state;
		if (variant == 11) account.environment = 1;
		CHECK(!ReconcileProtectedStageForCarry(account, state, source, access, intent, stage, text, inventory));
	}
}

SyncCarryJournalStart RepairStart(const Device &d) {
	auto start = SyncCarryJournalStart();
	start.operationId = QString(32, u'a');
	start.account = Account;
	start.source = start.target = d.state.chat;
	start.mode = SyncCarryMode::RepairMark;
	start.loginAuthority = QString(32, u'a');
	start.state = d.state;
	start.preferenceToken = d.state.bindingToken.toLatin1();
	start.resolution.action = SyncChatAction::Searching;
	start.resolution.cache = d.cache;
	start.targetEligibility = Eligible(d.state.chat);
	start.operationConfirmed = true;
	start.markText = u"#purplechatsync v1"_q;
	start.markRequestId = u"1234"_q;
	start.ownRequestId = u"1235"_q;
	start.oldRead = SyncChatDirectRead::MarkGone;
	start.sourceAccess = SyncCarrySourceAccess::Readable;
	return start;
}

void RepairMarkDelta() {
	Section = "repair mark delta";
	const auto chat = Peer(u"chat:10"_q);
	for (auto crash = -1; crash != 6; ++crash) {
		auto cloud = Cloud();
		cloud.mark(chat);
		auto d = Device('a', chat, cloud.cache(chat));
		d.publish(cloud, Text('a'));
		const auto state = StateBytes(d.state);
		const auto start = RepairStart(d);
		auto result = DeltaStore(BeginSyncCarryJournal(start));
		for (auto step = 0; step != 20 && result.journal.phase != SyncCarryPhase::Done; ++step) {
			auto j = result.journal;
			auto event = SyncCarryEvent(j, SyncCarryEventKind::Resolution);
			event.outcome = SyncCarryOutcome::Success;
			event.objectsDurable = true;
			event.state = d.state;
			CHECK(SyncCarryJournalForwards(j).empty());
			CHECK(!SyncCarryJournalPostsOwnRecord(j));
			if (j.phase == SyncCarryPhase::Mark) {
				if (NextSyncCarryJournalAction(j) == SyncCarryJournalAction::PostMark) {
					event.kind = SyncCarryEventKind::MarkPostPrepared;
					result = DeltaStore(DeltaAdvance(j, event));
					cloud.mark(chat, j.markText, j.markRequestId);
					if (crash == 0) result = DeltaStore(DeltaResume(result.journal, d.state, false, true));
					continue;
				}
				event.kind = SyncCarryEventKind::MarkRead;
				event.mark = cloud.marks.at(cloud.key(chat));
			} else if (j.phase == SyncCarryPhase::PrepareCache) {
				event.kind = SyncCarryEventKind::CachePrepared;
				event.cache = { j.operationId, Account, *j.targetMark, false };
			} else if (j.phase == SyncCarryPhase::CommitState) {
				event.kind = SyncCarryEventKind::StateObserved;
			} else if (j.phase == SyncCarryPhase::CompleteCache) {
				event.kind = SyncCarryEventKind::CacheCompleted;
				event.cache = { j.operationId, Account, *j.targetMark, true };
			} else REQUIRE(false);
			if (crash == step + 1) result = DeltaStore(DeltaResume(j, d.state, false, true));
			else result = DeltaStore(DeltaAdvance(j, event));
			CHECK(StateBytes(d.state) == state);
		}
		CHECK(result.journal.phase == SyncCarryPhase::Done);
		CHECK(cloud.requests.size() == 1);
		CHECK(StateBytes(d.state) == state);
	}
	auto cloud = Cloud();
	cloud.mark(chat);
	auto d = Device('a', chat, cloud.cache(chat));
	for (auto variant = 0; variant != 12; ++variant) {
		auto start = RepairStart(d);
		if (variant == 0) start.oldRead = SyncChatDirectRead::Unknown;
		if (variant == 1) start.oldRead = SyncChatDirectRead::ChatGone;
		if (variant == 2) start.sourceAccess = SyncCarrySourceAccess::Protected;
		if (variant == 3) start.targetEligibility.refusal = SyncChatRefusal::CannotSendFiles;
		if (variant == 4) start.targetEligibility.readers.populationUnknown = true;
		if (variant == 5) start.targetEligibility.readers.warningRequired = true;
		if (variant == 6) start.target = Peer(u"chat:20"_q);
		if (variant == 7) start.state.reset();
		if (variant == 8) start.otherStagesPresent = true;
		if (variant == 9) start.operationConfirmed = false;
		if (variant == 10) start.preferenceToken = "wrong";
		if (variant == 11) start.resolution.winner = d.cache;
		CHECK(!BeginSyncCarryJournal(start));
	}
	auto start = RepairStart(d);
	start.targetEligibility.readers.warningRequired = true;
	start.targetEligibility.readers.otherMembers = 1;
	start.readerConsentAccepted = true;
	CHECK(BeginSyncCarryJournal(start));
}

std::set<std::tuple<QString, int32_t, QString>> CloudSnapshot(const Cloud &cloud) {
	auto result = std::set<std::tuple<QString, int32_t, QString>>();
	for (const auto &[chat, messages] : cloud.messages) {
		for (const auto &message : messages) {
			result.emplace(chat, message.reference.messageId, message.reference.recordHash);
		}
	}
	for (const auto &[chat, mark] : cloud.marks) {
		result.emplace(chat, mark.messageId, mark.text);
	}
	return result;
}

SyncChatResolution FreshResolution(
		const SyncChatCache &cache,
		SyncChatDirectRead read,
		const Cloud &cloud,
		int retries = 0) {
	auto input = SyncChatResolveInput();
	input.cache = cache;
	input.cacheRead = read;
	input.cachedEligibility = Eligible(cache.chat);
	input.searchRetriesCompleted = retries;
	for (const auto &[key, mark] : cloud.marks) {
		auto found = SyncChatMark();
		found.location = { mark.chat, mark.messageId, mark.date };
		found.status = SyncChatMarkStatus::Supported;
		found.eligibility = Eligible(mark.chat);
		found.records = cloud.inventory(mark.chat).messages.empty()
			? SyncChatRecordPresence::Empty : SyncChatRecordPresence::Present;
		found.readable = found.freshlyRead = true;
		input.marks.push_back(found);
	}
	return ResolveSyncChat(input);
}

SyncCarryJournalEvent FtfEvent(
		const SyncCarryJournal &journal,
		const Device &device,
		const SyncChatResolution &resolution,
		const SyncCarryMarkReadback &mark = {}) {
	auto event = SyncCarryEvent(journal, SyncCarryEventKind::FinishThenFollow);
	event.outcome = SyncCarryOutcome::Success;
	event.objectsDurable = true;
	event.state = device.state;
	event.resolution = resolution;
	event.fullResolutionComplete = true;
	event.noRequestInFlight = true;
	event.mark = mark;
	return event;
}

// Persists MarkPostPrepared with an unknown result; the mark lands only if asked.
void HoldMark(Session &s, bool lands) {
	s.run(SyncCarryPhase::Mark);
	REQUIRE(s.journal.phase == SyncCarryPhase::Mark);
	REQUIRE(NextSyncCarryJournalAction(s.journal) == SyncCarryJournalAction::PostMark);
	const auto text = s.journal.markText;
	const auto request = s.journal.operationId + s.journal.markRequestId;
	auto event = SyncCarryEvent(s.journal, SyncCarryEventKind::MarkPostPrepared);
	event.outcome = SyncCarryOutcome::Unknown;
	s.store(DeltaAdvance(s.journal, event));
	REQUIRE(s.journal.markAttempted);
	if (lands) s.cloud.mark(s.target, text, request);
}

SyncCarryJournalResult FtfNow(Session &s) {
	const auto i = s.cloud.marks.find(s.cloud.key(s.target));
	const auto mark = i == s.cloud.marks.end() ? SyncCarryMarkReadback() : i->second;
	return DeltaAdvance(s.journal, FtfEvent(s.journal, s.device, Resolution(s.device, s.cloud), mark));
}

// A refusal persists nothing and hands back the unchanged journal, with a typed reason.
bool Refuses(
		const SyncCarryJournal &journal,
		const SyncCarryJournalEvent &event,
		const QString &login) {
	const auto result = AdvanceSyncCarryJournal(journal, event, login);
	return result.status == SyncCarryJournalStatus::Invalid && result.canonical.isEmpty()
		&& SerializeSyncCarryJournal(result.journal).canonical == SerializeSyncCarryJournal(journal).canonical
		&& result.journal.revision == journal.revision && result.journal.phase == journal.phase
		&& CheckSyncCarryFinishThenFollow(journal, event, login) != SyncCarryFinishRefusal::None;
}

bool RefusesFor(
		const SyncCarryJournal &journal,
		const SyncCarryJournalEvent &event,
		const QString &login,
		SyncCarryFinishRefusal reason) {
	const auto actual = CheckSyncCarryFinishThenFollow(journal, event, login);
	if (actual != reason) std::printf("refusal %d, expected %d\n", int(actual), int(reason));
	return Refuses(journal, event, login) && actual == reason;
}

bool HoldsConfirmedHead(const Cloud &cloud, const Device &device, const PeerToken &chat) {
	const auto destination = cloud.inventory(chat);
	return std::any_of(destination.messages.begin(), destination.messages.end(), [&](const auto &message) {
		const auto header = ParseSyncEnvelope(message.bytes).header;
		return header && header->writerInstall == device.state.install
			&& header->seq == device.state.config.confirmedSeq;
	});
}

void FinishThenFollowDelta() {
	Section = "finish then follow delta";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	const auto other = Peer(u"chat:30"_q);
	const auto login = QString(32, u'a');
	CHECK(int(SyncCarryPhase::Cleanup) == 13 && int(SyncCarryPhase::Done) == 14
		&& int(SyncCarryPhase::Aborted) == 15 && int(SyncCarryPhase::CleanupSkipped) == 16
		&& int(SyncCarryPhase::InterruptedAbandoned) == 17
		&& int(SyncCarryPhase::FinishedForFollow) == 18);

	// Case (c): our acknowledged mark M in N is outranked by W elsewhere.
	auto cloud = Cloud();
	cloud.mark(old);
	auto d = Device('a', old, cloud.cache(old));
	d.publish(cloud, Text('a'));
	auto s = Session(d, cloud, next, SyncCarryMode::Switch, SyncCarrySourceAccess::Readable, true, true, login);
	HoldMark(s, true);
	const auto m = cloud.cache(next);
	const auto ours = cloud.marks.at(cloud.key(next));
	cloud.mark(other);
	const auto w = cloud.cache(other);
	const auto j = s.journal;
	const auto resolution = Resolution(d, cloud);
	REQUIRE(resolution.action == SyncChatAction::Follow && resolution.winner == w);
	const auto event = FtfEvent(j, d, resolution, ours);
	CHECK(CheckSyncCarryFinishThenFollow(j, event, login) == SyncCarryFinishRefusal::None);
	using Refusal = SyncCarryFinishRefusal;
	const auto reasons = std::array{
		Refusal::WinnerInTarget, Refusal::WinnerNotNewer, Refusal::IncompleteResolution,
		Refusal::RequestInFlight, Refusal::Outcome, Refusal::Outcome, Refusal::Scope, Refusal::Scope,
		Refusal::Scope, Refusal::Scope, Refusal::Scope, Refusal::Scope, Refusal::Scope,
		Refusal::ResolutionScope, Refusal::ResolutionScope, Refusal::TargetGone, Refusal::MarkMismatch,
		Refusal::MarkMismatch, Refusal::MarkMismatch, Refusal::MarkMismatch, Refusal::MarkMismatch,
		Refusal::AbsentUnproven, Refusal::AbsentUnproven, Refusal::AbsentUnproven, Refusal::Action,
		Refusal::InvalidWinner,
	};
	for (auto variant = 0; variant != int(reasons.size()); ++variant) {
		auto bad = event;
		auto current = login;
		switch (variant) {
		case 0: bad.resolution.winner = m; break;
		case 1: bad.resolution.winner = SyncChatCache{ other, w.markId, m.markDate - 1 }; break;
		case 2: bad.fullResolutionComplete = false; break;
		case 3: bad.noRequestInFlight = false; break;
		case 4: bad.outcome = SyncCarryOutcome::Unknown; break;
		case 5: bad.outcome = SyncCarryOutcome::Failed; break;
		case 6: bad.account.environment = 1; break;
		case 7: bad.source = other; break;
		case 8: bad.target = other; break;
		case 9: ++bad.revision; break;
		case 10: bad.operationId = QString(32, u'z'); break;
		case 11: current = QString(32, u'b'); break;
		case 12: current.clear(); break;
		case 13: bad.resolution.cache.reset(); break;
		case 14: bad.resolution.cache->chat = other; break;
		case 15: bad.mark.outcome = SyncCarryMarkOutcome::ChatGone; break;
		case 16: bad.mark.text = u"#purplechatsync v1"_q; break;
		case 17: bad.mark.ownership = SyncChatMessageOwnership::Other; break;
		case 18: bad.mark.chat = other; break;
		case 19: bad.mark.account.environment = 1; break;
		case 20: bad.mark.hasMedia = true; break;
		case 21: bad.mark = { SyncCarryMarkOutcome::Absent, Account, next }; break;
		case 22:
			bad.mark = { SyncCarryMarkOutcome::Absent, Account, next };
			bad.neverLandedConfirmed = true;
			bad.neverLandedRequestId = u"99999"_q;
			break;
		case 23:
			bad.mark = { SyncCarryMarkOutcome::Absent, Account, other };
			bad.neverLandedConfirmed = true;
			bad.neverLandedRequestId = j.markRequestId;
			break;
		case 24: bad.resolution.winner.reset(); break;
		case 25: bad.resolution.winner = SyncChatCache{ other, -1, w.markDate }; break;
		}
		if (!CHECK(RefusesFor(j, bad, current, reasons[variant]))) std::printf("refused variant %d\n", variant);
	}
	for (const auto action : {
		SyncChatAction::Invalid, SyncChatAction::Unset, SyncChatAction::VerifyMark,
		SyncChatAction::NewerMark, SyncChatAction::ReadWinnerInventory, SyncChatAction::Fill,
	}) {
		auto bad = event;
		bad.resolution.action = action;
		CHECK(RefusesFor(j, bad, login, Refusal::Action));
	}
	auto unnamed = event;
	unnamed.resolution.action = SyncChatAction::Use;
	unnamed.resolution.winner.reset();
	CHECK(RefusesFor(j, unnamed, login, Refusal::Action));
	unnamed.resolution.action = SyncChatAction::Searching;
	unnamed.resolution.winner = SyncChatCache{ next, m.markId, 1 };
	CHECK(RefusesFor(j, unnamed, login, Refusal::WinnerInTarget));
	auto unattempted = j;
	unattempted.markAttempted = false;
	REQUIRE(SerializeSyncCarryJournal(unattempted));
	CHECK(RefusesFor(unattempted, FtfEvent(unattempted, d, resolution, ours), login, Refusal::MarkMismatch));
	const auto beforeAttempt = DeltaAdvance(unattempted, FtfEvent(unattempted, d, resolution));
	CHECK(beforeAttempt && !beforeAttempt.journal.attemptedMark && !beforeAttempt.journal.markAttempted);

	// A verified targetMark must match; Unknown keeps it; Absent cannot erase it.
	auto verified = j;
	verified.targetMark = m;
	REQUIRE(SerializeSyncCarryJournal(verified));
	auto different = event;
	different.mark.messageId = m.markId + 7;
	CHECK(RefusesFor(verified, different, login, Refusal::MarkMismatch));
	auto proven = event;
	proven.mark = { SyncCarryMarkOutcome::Absent, Account, next };
	proven.neverLandedConfirmed = true;
	proven.neverLandedRequestId = j.markRequestId;
	CHECK(RefusesFor(verified, proven, login, Refusal::AbsentUnproven));
	const auto provenUnmarked = DeltaAdvance(j, proven);
	REQUIRE(provenUnmarked);
	CHECK(!provenUnmarked.journal.targetMark && provenUnmarked.journal.markAttempted);
	CHECK(!provenUnmarked.journal.attemptedMark);
	auto unknown = event;
	unknown.mark = {};
	const auto kept = DeltaAdvance(verified, unknown);
	REQUIRE(kept);
	CHECK(kept.journal.targetMark == m && kept.journal.supersededBy == w && !kept.journal.attemptedMark);
	unknown.resolution.winner = SyncChatCache{ other, w.markId, m.markDate - 1 };
	CHECK(RefusesFor(verified, unknown, login, Refusal::WinnerNotNewer));
	const auto unmarkedOlder = DeltaAdvance(j, unknown);
	CHECK(unmarkedOlder && !unmarkedOlder.journal.targetMark);

	const auto before = CloudSnapshot(cloud);
	const auto image = d.images.at(j.nextStateHash);
	const auto accepted = DeltaStore(DeltaAdvance(j, event));
	CHECK(accepted.status == SyncCarryJournalStatus::Valid);
	CHECK(accepted.journal.phase == SyncCarryPhase::PrepareCache);
	CHECK(accepted.journal.finishThenFollow && !accepted.journal.markVerified);
	CHECK(accepted.journal.supersededBy == w && accepted.journal.targetMark == m);
	CHECK(accepted.journal.attemptedMark == m);
	CHECK(accepted.journal.markAttempted && accepted.journal.markRequestId == j.markRequestId);
	CHECK(SyncCarryJournalHoldsWrites(accepted.journal));
	CHECK(SyncCarryJournalCacheMark(accepted.journal) == m);
	CHECK(RefusesFor(accepted.journal, FtfEvent(accepted.journal, d, resolution, ours), login, Refusal::Phase));
	auto zero = SyncCarryEvent(accepted.journal, SyncCarryEventKind::CachePrepared);
	zero.outcome = SyncCarryOutcome::Success;
	zero.objectsDurable = true;
	zero.state = d.state;
	zero.cache = { accepted.journal.operationId, Account, SyncChatCache{ next, 0, 0 }, false };
	CHECK(!DeltaAdvance(accepted.journal, zero));
	const auto abandoned = DeltaStore(AbandonInterruptedOperation(accepted.journal, Account,
		QString(32, u'b'), true, d.state, false, d.stage, true));
	CHECK(abandoned.journal.phase == SyncCarryPhase::InterruptedAbandoned);
	CHECK(abandoned.journal.finishThenFollow && abandoned.journal.supersededBy == w);
	CHECK(abandoned.journal.markAttempted && !SyncCarryJournalHoldsWrites(abandoned.journal));
	s.store(accepted);
	s.run();
	const auto f = s.journal;
	REQUIRE(f.phase == SyncCarryPhase::FinishedForFollow);
	CHECK(f.finishThenFollow && f.supersededBy == w && f.targetMark == m && !f.markVerified);
	CHECK(f.attemptedMark == m);
	CHECK(f.stateCommitted && f.cacheCompleted);
	CHECK(f.markAttempted && f.markRequestId == j.markRequestId && f.ownRequestId == j.ownRequestId);
	CHECK(f.deliveryAttempted && f.deliveryHistory == j.deliveryHistory && !f.deliveryHistory.empty());
	CHECK(SyncCarryPlanJson(*f.plan) == SyncCarryPlanJson(*j.plan));
	CHECK(StateBytes(d.state) == image);
	CHECK(d.state.chat == next && d.cache == m);
	const auto destination = cloud.inventory(next);
	CHECK(!d.state.ownMessages.empty());
	for (const auto &own : d.state.ownMessages) {
		CHECK(std::any_of(destination.messages.begin(), destination.messages.end(), [&](const auto &message) {
			return message.reference.messageId == own.messageId
				&& message.reference.recordHash == own.recordHash;
		}));
	}
	CHECK(CloudSnapshot(cloud) == before);
	CHECK(cloud.requests.size() == 1);
	REQUIRE(f.abandonedCleanup.size() == 2);
	CHECK(std::find(f.abandonedCleanup.begin(), f.abandonedCleanup.end(),
		SyncCarryReference{ Account, old, j.oldMark->markId, {} }) != f.abandonedCleanup.end());
	for (const auto &message : cloud.inventory(old).messages) {
		CHECK(!CheckSyncCarryDeletion(f, message, destination));
	}
	CHECK(!CheckSyncCarryOldMarkDeletion(f, cloud.marks.at(cloud.key(old)), destination));
	CHECK(!SyncCarryJournalHoldsWrites(f));
	CHECK(NextSyncCarryJournalAction(f) == SyncCarryJournalAction::None);
	CHECK(SyncCarryJournalForwards(f).empty() && !SyncCarryJournalPostsOwnRecord(f));
	CHECK(CheckSyncCarryReaders(f, Eligible(next)) == SyncCarryReaderVerdict::Hold);
	CHECK(!PrepareSyncCarryJournalCommit(f, d.state, destination).commit);

	// Terminal replay.
	const auto canonical = SerializeSyncCarryJournal(f).canonical;
	for (const auto durable : { false, true }) {
		CHECK(DeltaResume(f, d.state, false, durable).canonical == canonical);
	}
	CHECK(ResumeSyncCarryJournal(f, d.state, false, true, QString(32, u'b')).canonical == canonical);
	for (auto kind = 0; kind <= int(SyncCarryEventKind::FinishThenFollow); ++kind) {
		auto replay = SyncCarryEvent(f, SyncCarryEventKind(kind));
		replay.outcome = SyncCarryOutcome::Success;
		replay.objectsDurable = replay.noRequestInFlight = replay.skipCleanupConfirmed = true;
		replay.state = d.state;
		replay.fullResolutionComplete = true;
		replay.mark = { SyncCarryMarkOutcome::ChatGone, Account, old };
		CHECK(Refuses(f, replay, login));
		CHECK(CheckSyncCarryFinishThenFollow(f, replay, login) == Refusal::Scope);
	}
	CHECK(!AbandonInterruptedOperation(f, Account, QString(32, u'b'), true, d.state, false, d.stage, true));

	// Codec: phase 18, superseded_by, unknown members and malformed shapes.
	auto json = QJsonDocument::fromJson(canonical).object();
	CHECK(json.value(u"phase"_q).toInt() == 18);
	CHECK(json.value(u"superseded_by"_q).toObject().value(u"id"_q).toInt() == w.markId);
	CHECK(json.value(u"attempted_mark"_q).toObject().value(u"id"_q).toInt() == m.markId);
	json.insert(u"future_ftf"_q, 7);
	auto superseded = json.value(u"superseded_by"_q).toObject();
	superseded.insert(u"future"_q, true);
	json.insert(u"superseded_by"_q, superseded);
	const auto future = DeltaStore(ParseSyncCarryJournal(QJsonDocument(json).toJson()));
	const auto roundtrip = QJsonDocument::fromJson(future.canonical).object();
	CHECK(roundtrip.value(u"future_ftf"_q).toInt() == 7);
	CHECK(roundtrip.value(u"superseded_by"_q).toObject().value(u"future"_q).toBool());
	for (auto invalid = 0; invalid != 15; ++invalid) {
		auto broken = QJsonDocument::fromJson(canonical).object();
		auto cache = broken.value(u"superseded_by"_q).toObject();
		auto cleanup = broken.value(u"abandoned_cleanup"_q).toArray();
		switch (invalid) {
		case 0: broken.insert(u"phase"_q, 19); break;
		case 1: broken.insert(u"superseded_by"_q, u"chat:30"_q); break;
		case 2: broken.remove(u"superseded_by"_q); break;
		case 3: cache.insert(u"chat"_q, *PeerTokenText(next)); break;
		case 4: cache.insert(u"date"_q, QJsonValue(qint64(m.markDate - 1))); break;
		case 5: cleanup.removeLast(); broken.insert(u"abandoned_cleanup"_q, cleanup); break;
		case 6: broken.insert(u"phase"_q, int(SyncCarryPhase::Cleanup)); break;
		case 7: broken.insert(u"phase"_q, int(SyncCarryPhase::Done)); break;
		case 8: broken.insert(u"superseded_by"_q, QJsonObject()); break;
		case 9: broken.insert(u"mark_verified"_q, true); break;
		case 10: broken.insert(u"cache_completed"_q, false); break;
		case 11: broken.insert(u"attempted_mark"_q, u"chat:20"_q); break;
		case 12: broken.insert(u"attempted_mark"_q, QJsonValue(QJsonValue::Null)); break;
		case 13: broken.insert(u"mark_attempted"_q, false); break;
		case 14: {
			auto attempted = broken.value(u"attempted_mark"_q).toObject();
			attempted.insert(u"chat"_q, *PeerTokenText(old));
			broken.insert(u"attempted_mark"_q, attempted);
		} break;
		}
		if (invalid == 3 || invalid == 4) broken.insert(u"superseded_by"_q, cache);
		if (!CHECK(!ParseSyncCarryJournal(QJsonDocument(broken).toJson()))) {
			std::printf("codec variant %d\n", invalid);
		}
	}
	auto atMark = QJsonDocument::fromJson(SerializeSyncCarryJournal(j).canonical).object();
	atMark.insert(u"superseded_by"_q, QJsonValue(QJsonValue::Null));
	CHECK(!ParseSyncCarryJournal(QJsonDocument(atMark).toJson()));
	auto oversize = j;
	oversize.preserved.insert(u"future"_q, QString(4 * 1024 * 1024, u'x'));
	CHECK(RefusesFor(oversize, event, login, Refusal::Serialization));
	oversize = f;
	oversize.preserved.insert(u"future"_q, QString(4 * 1024 * 1024, u'x'));
	CHECK(!SerializeSyncCarryJournal(oversize));

	// The later ordinary Follow carries N into W and leaves O untouched.
	const auto oldMessages = cloud.inventory(old).messages.size();
	const auto follow = Resolution(d, cloud);
	CHECK(follow.action == SyncChatAction::Follow && follow.winner == w && follow.source == next);
	auto onward = Session(d, cloud, other, SyncCarryMode::Follow, SyncCarrySourceAccess::Readable, true, false, login);
	onward.run();
	CHECK(onward.journal.phase == SyncCarryPhase::Done);
	CHECK(d.state.chat == other && d.cache == w);
	CHECK(HoldsConfirmedHead(cloud, d, other));
	CHECK(cloud.inventory(old).messages.size() == oldMessages);
}

void FinishThenFollowUnmarked() {
	Section = "finish then follow unmarked";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	const auto other = Peer(u"chat:30"_q);
	const auto login = QString(32, u'a');
	const auto zero = SyncChatCache{ next, 0, 0 };

	// Case (d) after the cadence: nothing landed; W is the old mark in O.
	auto cloud = Cloud();
	cloud.mark(old);
	auto d = Device('a', old, cloud.cache(old));
	d.publish(cloud, Text('a'));
	auto s = Session(d, cloud, next, SyncCarryMode::Switch, SyncCarrySourceAccess::Readable, true, true, login);
	HoldMark(s, false);
	const auto resolution = Resolution(d, cloud);
	REQUIRE(resolution.action == SyncChatAction::Use && resolution.winner == d.cache);
	const auto before = CloudSnapshot(cloud);
	const auto accepted = DeltaStore(DeltaAdvance(s.journal, FtfEvent(s.journal, d, resolution)));
	CHECK(!accepted.journal.targetMark && accepted.journal.supersededBy == d.cache);
	CHECK(SyncCarryJournalCacheMark(accepted.journal) == zero);
	auto receipt = SyncCarryEvent(accepted.journal, SyncCarryEventKind::CachePrepared);
	receipt.outcome = SyncCarryOutcome::Success;
	receipt.objectsDurable = true;
	receipt.state = d.state;
	for (const auto &mark : { SyncChatCache{ next, 1, cloud.clock }, SyncChatCache{ old, 0, 0 }, d.cache }) {
		receipt.cache = { accepted.journal.operationId, Account, mark, false };
		CHECK(!DeltaAdvance(accepted.journal, receipt));
	}
	receipt.cache = { accepted.journal.operationId, Account, zero, true };
	CHECK(!DeltaAdvance(accepted.journal, receipt));
	receipt.cache = { accepted.journal.operationId, Account, zero, false };
	CHECK(DeltaAdvance(accepted.journal, receipt));
	s.store(accepted);
	s.run();
	CHECK(s.journal.phase == SyncCarryPhase::FinishedForFollow);
	CHECK(!s.journal.targetMark && s.journal.markAttempted && s.journal.supersededBy == cloud.cache(old));
	CHECK(!s.journal.attemptedMark);
	CHECK(!QJsonDocument::fromJson(SerializeSyncCarryJournal(s.journal).canonical).object()
		.contains(u"attempted_mark"_q));
	CHECK(s.journal.abandonedCleanup.size() == 2);
	CHECK(d.state.chat == next && d.cache == zero);
	CHECK(CloudSnapshot(cloud) == before);
	CHECK(cloud.requests.empty());

	// The unmarked binding afterwards: Follow from N, else Searching then Lost.
	const auto gone = FreshResolution(zero, SyncChatDirectRead::MarkGone, cloud);
	CHECK(gone.action == SyncChatAction::Follow && gone.winner == cloud.cache(old) && gone.source == next);
	const auto empty = Cloud();
	CHECK(FreshResolution(zero, SyncChatDirectRead::MarkGone, empty).action == SyncChatAction::Searching);
	CHECK(FreshResolution(zero, SyncChatDirectRead::MarkGone, empty, 3).action == SyncChatAction::Lost);
	const auto again = Session(d, cloud, other, SyncCarryMode::Switch, SyncCarrySourceAccess::Readable, false, false, login);
	CHECK(again.journal.oldMark == zero);
	auto back = Session(d, cloud, old, SyncCarryMode::Follow, SyncCarrySourceAccess::Readable, true, false, login);
	CHECK(back.journal.oldMark == zero);
	back.run();
	CHECK(back.journal.phase == SyncCarryPhase::Done);
	CHECK(d.state.chat == old && d.cache == cloud.cache(old));
	CHECK(HoldsConfirmedHead(cloud, d, old));

	// Searching and Lost name no W; an older alternative in N refuses.
	for (const auto action : { SyncChatAction::Searching, SyncChatAction::Lost }) {
		auto c = Cloud();
		c.mark(old);
		auto device = Device('a', old, c.cache(old));
		device.publish(c, Text('a'));
		auto held = Session(device, c, next, SyncCarryMode::Switch, SyncCarrySourceAccess::Readable, false, false, login);
		HoldMark(held, false);
		auto searching = SyncChatResolution();
		searching.action = action;
		searching.cache = device.cache;
		auto inN = FtfEvent(held.journal, device, searching);
		inN.resolution.winner = SyncChatCache{ next, 9, 1 };
		CHECK(RefusesFor(held.journal, inN, login, SyncCarryFinishRefusal::WinnerInTarget));
		auto elsewhere = inN;
		elsewhere.resolution.winner = SyncChatCache{ other, 9, 1 };
		const auto bare = DeltaStore(DeltaAdvance(held.journal, elsewhere));
		CHECK(bare.journal.finishThenFollow && !bare.journal.supersededBy && !bare.journal.targetMark);
		const auto json = QJsonDocument::fromJson(bare.canonical).object();
		CHECK(json.contains(u"superseded_by"_q) && json.value(u"superseded_by"_q).isNull());
		CHECK(DeltaStore(DeltaAdvance(held.journal, FtfEvent(held.journal, device, searching))).canonical
			.size() > 0);
	}
}

void FinishThenFollowTie() {
	Section = "finish then follow same-second loser";
	const auto high = Peer(u"chat:20"_q);
	const auto low = Peer(u"chat:10"_q);
	const auto login = QString(32, u'a');
	using Refusal = SyncCarryFinishRefusal;
	auto cloud = Cloud();
	cloud.mark(high);
	auto d = Device('a', high, cloud.cache(high));
	d.publish(cloud, Text('a'));
	auto s = Session(d, cloud, low, SyncCarryMode::Switch, SyncCarrySourceAccess::Readable, true, true, login);
	s.run(SyncCarryPhase::Mark);
	cloud.clock = s.journal.oldMark->markDate - 1;
	HoldMark(s, true);
	const auto m = cloud.cache(low);
	const auto ours = cloud.marks.at(cloud.key(low));
	REQUIRE(m.markDate == s.journal.oldMark->markDate);
	REQUIRE(!SyncChatMarkOutranks(m, *s.journal.oldMark));
	const auto resolution = Resolution(d, cloud);
	REQUIRE(resolution.action == SyncChatAction::Use && resolution.winner == d.cache);
	const auto state = StateBytes(d.state);
	const auto before = CloudSnapshot(cloud);
	const auto request = s.journal.markRequestId;
	const auto held = [&](const char *where) {
		if (!CHECK(!s.journal.stateCommitted && !s.journal.markVerified && s.journal.markAttempted
			&& s.journal.markRequestId == request
			&& StateBytes(d.state) == state && d.state.chat == high && d.cache.chat == high
			&& CloudSnapshot(cloud) == before && cloud.requests.size() == 1)) {
			std::printf("held check at %s\n", where);
		}
	};

	// Restart after the loss is seen and before FtF: the mark read holds, nothing posts.
	auto read = SyncCarryEvent(s.journal, SyncCarryEventKind::MarkRead);
	read.outcome = SyncCarryOutcome::Success;
	read.noRequestInFlight = true;
	read.mark = ours;
	s.store(DeltaAdvance(s.journal, read));
	CHECK(s.held && s.journal.phase == SyncCarryPhase::Mark);
	s.store(DeltaResume(s.journal, d.state, false, true));
	CHECK(s.journal.phase == SyncCarryPhase::VerifyDestination);
	s.run(SyncCarryPhase::Mark);
	CHECK(NextSyncCarryJournalAction(s.journal) == SyncCarryJournalAction::ReadMark);
	held("before FtF");

	// FtF records our mark, then a restart before the commit drops it as the target only.
	s.store(FtfNow(s));
	CHECK(s.journal.targetMark == m && s.journal.attemptedMark == m && s.journal.supersededBy == d.cache);
	s.store(DeltaResume(s.journal, d.state, false, true));
	CHECK(s.journal.phase == SyncCarryPhase::VerifyDestination);
	CHECK(!s.journal.finishThenFollow && !s.journal.supersededBy && !s.journal.targetMark);
	CHECK(s.journal.attemptedMark == m);
	CHECK(NextSyncCarryJournalAction(s.journal) == SyncCarryJournalAction::VerifyDestination);
	held("after precommit restart");
	s.run(SyncCarryPhase::Mark);
	CHECK(NextSyncCarryJournalAction(s.journal) == SyncCarryJournalAction::ReadMark);

	// Neither a losing read nor a never-landed claim can commit or reopen PostMark.
	read = SyncCarryEvent(s.journal, SyncCarryEventKind::MarkRead);
	read.outcome = SyncCarryOutcome::Success;
	read.noRequestInFlight = true;
	read.mark = ours;
	const auto losing = DeltaAdvance(s.journal, read);
	CHECK(losing.status == SyncCarryJournalStatus::NeedsReview);
	CHECK(losing.journal.phase == SyncCarryPhase::Mark && !losing.journal.markVerified);
	read.mark = { SyncCarryMarkOutcome::Absent, Account, low };
	read.neverLandedConfirmed = true;
	read.neverLandedRequestId = s.journal.markRequestId;
	const auto absent = DeltaAdvance(s.journal, read);
	CHECK(absent.status == SyncCarryJournalStatus::NeedsReview);
	CHECK(absent.journal.markAttempted && absent.journal.attemptedMark == m);
	CHECK(NextSyncCarryJournalAction(DeltaStore(absent).journal) == SyncCarryJournalAction::ReadMark);
	auto claim = FtfEvent(s.journal, d, resolution, read.mark);
	claim.neverLandedConfirmed = true;
	claim.neverLandedRequestId = s.journal.markRequestId;
	CHECK(RefusesFor(s.journal, claim, login, Refusal::AbsentUnproven));
	auto other = ours;
	other.messageId += 5;
	CHECK(RefusesFor(s.journal, FtfEvent(s.journal, d, resolution, other), login, Refusal::MarkMismatch));
	held("after refused claims");

	// FtF again, then the commit and the ordinary Follow back to O.
	s.store(FtfNow(s));
	CHECK(s.journal.targetMark == m && s.journal.attemptedMark == m);
	s.run();
	CHECK(s.journal.phase == SyncCarryPhase::FinishedForFollow);
	CHECK(s.journal.attemptedMark == m && s.journal.markAttempted);
	CHECK(d.state.chat == low && d.cache == m);
	CHECK(CloudSnapshot(cloud) == before && cloud.requests.size() == 1);
	const auto follow = Resolution(d, cloud);
	CHECK(follow.action == SyncChatAction::Follow && follow.source == low);
	auto back = Session(d, cloud, high, SyncCarryMode::Follow, SyncCarrySourceAccess::Readable, false, false, login);
	back.run();
	CHECK(back.journal.phase == SyncCarryPhase::Done);
	CHECK(d.state.chat == high && HoldsConfirmedHead(cloud, d, high));
	CHECK(cloud.requests.size() == 1);
}

void FinishThenFollowScope() {
	Section = "finish then follow scope";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	const auto other = Peer(u"chat:30"_q);
	const auto login = QString(32, u'a');

	// Every phase but Mark refuses, before and after the commit.
	{
		auto cloud = Cloud();
		cloud.mark(old);
		auto d = Device('a', old, cloud.cache(old));
		d.publish(cloud, Text('a'));
		d.prepare(Text('a', 2));
		auto s = Session(d, cloud, next, SyncCarryMode::Switch, SyncCarrySourceAccess::Readable, true, true, login);
		auto phases = std::set<SyncCarryPhase>();
		for (auto n = 0; n != 60 && s.journal.phase != SyncCarryPhase::Done; ++n) {
			if (s.journal.phase != SyncCarryPhase::Mark) {
				phases.insert(s.journal.phase);
				CHECK(RefusesFor(s.journal, FtfEvent(s.journal, d, Resolution(d, cloud)), login,
					SyncCarryFinishRefusal::Phase));
			}
			s.step();
		}
		REQUIRE(s.journal.phase == SyncCarryPhase::Done);
		CHECK(RefusesFor(s.journal, FtfEvent(s.journal, d, Resolution(d, cloud)), login,
			SyncCarryFinishRefusal::Scope));
		CHECK(phases.size() >= 11);
		CHECK(phases.contains(SyncCarryPhase::CommitState) && phases.contains(SyncCarryPhase::Cleanup));
	}

	// Version 1 refuses the event and cannot carry its fields.
	{
		auto cloud = Cloud();
		cloud.mark(old);
		auto d = Device('a', old, cloud.cache(old));
		d.publish(cloud, Text('a'));
		auto s = Session(d, cloud, next);
		HoldMark(s, true);
		cloud.mark(other);
		REQUIRE(s.journal.version == 1);
		CHECK(RefusesFor(s.journal, FtfEvent(s.journal, d, Resolution(d, cloud),
			cloud.marks.at(cloud.key(next))), {}, SyncCarryFinishRefusal::Version));
		auto forced = s.journal;
		forced.finishThenFollow = true;
		CHECK(!SerializeSyncCarryJournal(forced));
		forced = s.journal;
		forced.attemptedMark = cloud.cache(next);
		CHECK(!SerializeSyncCarryJournal(forced));
		auto json = QJsonDocument::fromJson(SerializeSyncCarryJournal(s.journal).canonical).object();
		json.insert(u"superseded_by"_q, QJsonValue(QJsonValue::Null));
		json.insert(u"attempted_mark"_q, u"inert"_q);
		const auto kept = DeltaStore(ParseSyncCarryJournal(QJsonDocument(json).toJson()));
		CHECK(!kept.journal.finishThenFollow && !kept.journal.attemptedMark);
		CHECK(QJsonDocument::fromJson(kept.canonical).object().contains(u"superseded_by"_q));
		json.insert(u"phase"_q, int(SyncCarryPhase::FinishedForFollow));
		CHECK(!ParseSyncCarryJournal(QJsonDocument(json).toJson()));
	}

	// Follow and Fill journals publish no mark and refuse.
	{
		auto cloud = Cloud();
		cloud.mark(old);
		auto d = Device('a', old, cloud.cache(old));
		d.publish(cloud, Text('a'));
		cloud.mark(next);
		auto follow = Session(d, cloud, next, SyncCarryMode::Follow, SyncCarrySourceAccess::Readable, false, false, login);
		follow.run(SyncCarryPhase::Mark);
		REQUIRE(follow.journal.phase == SyncCarryPhase::Mark);
		cloud.mark(other);
		CHECK(RefusesFor(follow.journal, FtfEvent(follow.journal, d, Resolution(d, cloud),
			cloud.marks.at(cloud.key(next))), login, SyncCarryFinishRefusal::Mode));
		auto resolver = SyncChatResolveInput();
		for (const auto &peer : { old, next }) {
			auto mark = SyncChatMark();
			mark.location = cloud.cache(peer);
			mark.status = SyncChatMarkStatus::Supported;
			mark.eligibility = Eligible(peer);
			mark.readable = mark.freshlyRead = true;
			mark.records = peer == old ? SyncChatRecordPresence::Present : SyncChatRecordPresence::Empty;
			resolver.marks.push_back(mark);
		}
		auto start = SyncCarryJournalStart();
		start.operationId = QString(32, u'a');
		start.account = Account;
		start.source = old;
		start.target = next;
		start.mode = SyncCarryMode::Fill;
		start.stateAbsentConfirmed = true;
		start.operationConfirmed = true;
		start.resolution = ResolveSyncChat(resolver);
		start.targetEligibility = Eligible(next);
		start.ownRequestId = u"42"_q;
		start.loginAuthority = login;
		REQUIRE(start.resolution.action == SyncChatAction::Fill);
		const auto fill = DeltaStore(BeginSyncCarryJournal(start)).journal;
		CHECK(fill.version == 2);
		auto event = FtfEvent(fill, d, start.resolution);
		event.state.reset();
		event.stateAbsentConfirmed = true;
		CHECK(RefusesFor(fill, event, login, SyncCarryFinishRefusal::Mode));
	}

	// Existing paths, pinned: case (b) and the never-landed proof.
	{
		auto cloud = Cloud();
		cloud.mark(old);
		auto d = Device('a', old, cloud.cache(old));
		d.publish(cloud, Text('a'));
		auto s = Session(d, cloud, next, SyncCarryMode::Switch, SyncCarrySourceAccess::Readable, true, false, login);
		HoldMark(s, false);
		const auto j = s.journal;
		auto read = SyncCarryEvent(j, SyncCarryEventKind::MarkRead);
		read.outcome = SyncCarryOutcome::Success;
		read.noRequestInFlight = true;
		read.mark = { SyncCarryMarkOutcome::Absent, Account, next };
		const auto wait = DeltaAdvance(j, read);
		CHECK(wait.status == SyncCarryJournalStatus::Wait && wait.journal.markAttempted);
		read.neverLandedConfirmed = true;
		read.neverLandedRequestId = j.markRequestId;
		const auto retry = DeltaAdvance(j, read);
		CHECK(retry.status == SyncCarryJournalStatus::Retry && !retry.journal.markAttempted);
		CHECK(NextSyncCarryJournalAction(DeltaStore(retry).journal) == SyncCarryJournalAction::PostMark);
		cloud.mark(next, j.markText);
		CHECK(Resolution(d, cloud).winner == cloud.cache(next));
		s.run();
		CHECK(s.journal.phase == SyncCarryPhase::Done);
		CHECK(cloud.requests.empty());
		CHECK(d.state.chat == next && d.cache == cloud.cache(next));
	}
}

void FinishThenFollowCrashes() {
	Section = "finish then follow crashes";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	const auto other = Peer(u"chat:30"_q);
	const auto login = QString(32, u'a');
	auto resumedAt = std::set<std::pair<SyncCarryPhase, bool>>();
	for (const auto access : { SyncCarrySourceAccess::Readable, SyncCarrySourceAccess::ChatGone }) {
		for (const auto after : { false, true }) {
			for (const auto phase : {
				SyncCarryPhase::PrepareCache, SyncCarryPhase::CommitState,
				SyncCarryPhase::RetireOwnStage, SyncCarryPhase::CompleteCache,
			}) {
				const auto readable = access == SyncCarrySourceAccess::Readable;
				auto cloud = Cloud();
				cloud.mark(old);
				auto d = Device('a', old, cloud.cache(old));
				d.publish(cloud, Text('a'));
				if (!readable) d.prepare(Text('a', 2));
				auto s = Session(d, cloud, next, SyncCarryMode::Switch, access, readable, readable, login);
				HoldMark(s, true);
				const auto m = cloud.cache(next);
				cloud.mark(other);
				const auto w = cloud.cache(other);
				s.store(FtfNow(s));
				REQUIRE(s.journal.phase == SyncCarryPhase::PrepareCache);
				const auto oldMessages = cloud.inventory(old).messages.size();
				if (after) s.crashBeforeAck = phase;
				else s.crashBefore = phase;
				for (auto n = 0; n != 40 && s.journal.phase != SyncCarryPhase::FinishedForFollow; ++n) {
					const auto crashed = s.crashed;
					s.step();
					if (crashed || !s.crashed) continue;
					resumedAt.emplace(phase, after);
					if (s.journal.stateCommitted) {
						CHECK(s.journal.finishThenFollow && s.journal.supersededBy == w);
						CHECK(s.journal.targetMark == m);
					} else {
						CHECK(s.journal.phase == SyncCarryPhase::VerifyDestination);
						CHECK(!s.journal.finishThenFollow && !s.journal.supersededBy);
						CHECK(s.journal.markAttempted && s.journal.targetMark == m);
						s.run(SyncCarryPhase::Mark);
						s.store(FtfNow(s));
						REQUIRE(s.journal.phase == SyncCarryPhase::PrepareCache);
					}
				}
				CHECK(s.journal.phase == SyncCarryPhase::FinishedForFollow);
				CHECK(s.crashed == s.visited.contains(phase));
				CHECK(d.state.chat == next && d.cache == m);
				CHECK(d.state.config.pendingSeq == 0 && d.stage.isEmpty());
				CHECK(cloud.requests.size() == 1);
				CHECK(cloud.inventory(old).messages.size() == oldMessages);
				CHECK(HoldsConfirmedHead(cloud, d, next));
			}
		}
	}
	for (const auto phase : {
		SyncCarryPhase::PrepareCache, SyncCarryPhase::CommitState,
		SyncCarryPhase::RetireOwnStage, SyncCarryPhase::CompleteCache,
	}) {
		CHECK(resumedAt.contains({ phase, false }) && resumedAt.contains({ phase, true }));
	}

	// The written state waits without a durable confirmation.
	auto cloud = Cloud();
	cloud.mark(old);
	auto d = Device('a', old, cloud.cache(old));
	d.publish(cloud, Text('a'));
	auto s = Session(d, cloud, next, SyncCarryMode::Switch, SyncCarrySourceAccess::Readable, false, false, login);
	HoldMark(s, true);
	cloud.mark(other);
	const auto w = cloud.cache(other);
	s.store(FtfNow(s));
	s.step();
	REQUIRE(s.journal.phase == SyncCarryPhase::CommitState);
	const auto before = s.journal;
	const auto precommit = DeltaStore(DeltaResume(before, d.state, false, true)).journal;
	CHECK(precommit.phase == SyncCarryPhase::VerifyDestination && !precommit.finishThenFollow);
	d.state = ParseSyncLocalState(d.images.at(before.nextStateHash)).state;
	const auto waiting = DeltaResume(before, d.state, false, false);
	CHECK(waiting.status == SyncCarryJournalStatus::Wait && waiting.canonical.isEmpty());
	const auto committed = DeltaStore(DeltaResume(before, d.state, false, true)).journal;
	CHECK(committed.stateCommitted && committed.phase == SyncCarryPhase::CompleteCache);
	CHECK(committed.finishThenFollow && committed.supersededBy == w);
}

void FinishThenFollowProtected() {
	Section = "finish then follow deferred record";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	const auto other = Peer(u"chat:30"_q);
	const auto login = QString(32, u'a');
	auto cloud = Cloud();
	cloud.mark(old);
	auto d = Device('a', old, cloud.cache(old));
	d.publish(cloud, Text('a'));
	d.prepare(Text('a', 2));
	const auto pending = d.stage;
	auto s = Session(d, cloud, next, SyncCarryMode::Switch, SyncCarrySourceAccess::Protected, false, false, login);
	for (auto n = 0; n != 60 && (s.journal.phase != SyncCarryPhase::Mark || !s.journal.markAttempted); ++n) {
		ProtectedStep(s, SyncCarrySendIntent::NotIssued);
	}
	REQUIRE(s.journal.phase == SyncCarryPhase::Mark && s.journal.markAttempted);
	REQUIRE(!s.journal.targetPendingRecord.isEmpty());
	cloud.mark(other);
	s.store(FtfNow(s));
	REQUIRE(s.journal.phase == SyncCarryPhase::PrepareCache);
	const auto target = s.journal.targetPendingRecord;
	for (auto n = 0; n != 20 && s.journal.phase != SyncCarryPhase::FinishedForFollow; ++n) {
		if (!s.journal.stateCommitted) {
			CHECK(d.stage != target && d.state.chat == old);
		}
		ProtectedStep(s, SyncCarrySendIntent::NotIssued);
	}
	REQUIRE(s.journal.phase == SyncCarryPhase::FinishedForFollow);
	CHECK(d.stage == target && d.state.chat == next);
	CHECK(d.state.config.pendingSeq == d.state.config.seq);
	CHECK(!cloud.has(next, SyncCarryRecordHash(target)));
	CHECK(!cloud.has(old, SyncCarryRecordHash(pending)));
	CHECK(InspectConfigPayload(ParseSyncEnvelope(target)).text
		== InspectConfigPayload(ParseSyncEnvelope(pending)).text);
}

void FinishThenFollowRepair() {
	Section = "finish then follow repair mark";
	const auto chat = Peer(u"chat:10"_q);
	const auto other = Peer(u"chat:30"_q);
	for (const auto marked : { false, true }) {
		auto cloud = Cloud();
		cloud.mark(chat);
		auto d = Device('a', chat, cloud.cache(chat));
		d.publish(cloud, Text('a'));
		const auto state = StateBytes(d.state);
		const auto ledger = d.state.ownMessages.size();
		auto j = DeltaStore(BeginSyncCarryJournal(RepairStart(d))).journal;
		auto event = SyncCarryEvent(j, SyncCarryEventKind::MarkPostPrepared);
		event.outcome = SyncCarryOutcome::Unknown;
		j = DeltaStore(DeltaAdvance(j, event)).journal;
		if (marked) cloud.mark(chat, j.markText, j.markRequestId);
		else cloud.marks.erase(cloud.key(chat));
		cloud.mark(other);
		const auto w = cloud.cache(other);
		const auto resolution = FreshResolution(d.cache, SyncChatDirectRead::MarkGone, cloud);
		REQUIRE(resolution.action == SyncChatAction::Follow && resolution.winner == w);
		const auto readback = marked ? cloud.marks.at(cloud.key(chat)) : SyncCarryMarkReadback();
		auto inN = FtfEvent(j, d, resolution, readback);
		inN.resolution.winner = SyncChatCache{ chat, 99, w.markDate };
		CHECK(RefusesFor(j, inN, j.loginAuthority, SyncCarryFinishRefusal::WinnerInTarget));
		j = DeltaStore(DeltaAdvance(j, FtfEvent(j, d, resolution, readback))).journal;
		CHECK(j.phase == SyncCarryPhase::PrepareCache && j.finishThenFollow && j.supersededBy == w);
		const auto cache = marked ? cloud.cache(chat) : SyncChatCache{ chat, 0, 0 };
		CHECK(SyncCarryJournalCacheMark(j) == cache);
		const auto resumed = DeltaStore(DeltaResume(j, d.state, false, true)).journal;
		CHECK(resumed.phase == SyncCarryPhase::Mark && !resumed.finishThenFollow && !resumed.supersededBy);
		CHECK(resumed.markAttempted);
		CHECK(NextSyncCarryJournalAction(resumed) == SyncCarryJournalAction::ReadMark);
		event = SyncCarryEvent(j, SyncCarryEventKind::CachePrepared);
		event.outcome = SyncCarryOutcome::Success;
		event.objectsDurable = true;
		event.state = d.state;
		event.cache = { j.operationId, Account, cache, false };
		j = DeltaStore(DeltaAdvance(j, event)).journal;
		CHECK(j.phase == SyncCarryPhase::CommitState);
		j = DeltaStore(DeltaResume(j, d.state, false, true)).journal;
		CHECK(j.phase == SyncCarryPhase::CompleteCache && j.stateCommitted);
		CHECK(j.finishThenFollow && j.supersededBy == w);
		event = SyncCarryEvent(j, SyncCarryEventKind::CacheCompleted);
		event.outcome = SyncCarryOutcome::Success;
		event.objectsDurable = true;
		event.state = d.state;
		event.cache = { j.operationId, Account, cache, true };
		j = DeltaStore(DeltaAdvance(j, event)).journal;
		CHECK(j.phase == SyncCarryPhase::FinishedForFollow && j.abandonedCleanup.empty());
		CHECK(!SyncCarryJournalHoldsWrites(j));
		CHECK(StateBytes(d.state) == state && d.state.ownMessages.size() == ledger);
		CHECK(cloud.requests.size() == (marked ? 1u : 0u));
	}
}

void FinishThenFollowSeeded() {
	Section = "finish then follow seeded switches";
	const auto login = QString(32, u'a');
	const auto old = Peer(u"chat:10"_q);
	const auto targets = std::array{ Peer(u"chat:20"_q), Peer(u"chat:30"_q) };
	auto ties = 0, loserFirst = 0, winnerFirst = 0;
	for (auto seed = 0; seed != 12; ++seed) {
		auto random = std::mt19937_64(0xf7f0 + seed);
		auto cloud = Cloud();
		cloud.mark(old);
		auto devices = std::vector<Device>();
		const auto count = 3 + seed % 3;
		devices.reserve(count);
		for (auto i = 0; i != count; ++i) {
			devices.emplace_back(char('a' + i), old, cloud.cache(old));
			devices.back().publish(cloud, Text(char('a' + i)));
		}
		const auto future = cloud.post(old, QByteArray("{\"purple_sync\":2,\"opaque\":true}"));
		const auto heads = cloud.inventory(old).messages;
		const auto first = int(random() % count);
		const auto second = (first + 1 + int(random() % (count - 1))) % count;
		const auto cleanA = random() % 2 == 0;
		const auto markA = random() % 2 == 0;
		const auto cleanB = random() % 2 == 0;
		const auto markB = random() % 2 == 0;
		auto a = Session(devices[first], cloud, targets[0], SyncCarryMode::Switch,
			SyncCarrySourceAccess::Readable, cleanA, markA, login);
		++cloud.clock;
		auto b = Session(devices[second], cloud, targets[1], SyncCarryMode::Switch,
			SyncCarrySourceAccess::Readable, cleanB, markB, login);
		a.run(SyncCarryPhase::Mark);
		b.run(SyncCarryPhase::Mark);
		const auto aFirst = random() % 2 == 0;
		HoldMark(aFirst ? a : b, true);
		if (random() % 2) cloud.clock = cloud.marks.at(cloud.key((aFirst ? a : b).target)).date - 1;
		HoldMark(aFirst ? b : a, true);
		ties += cloud.cache(targets[0]).markDate == cloud.cache(targets[1]).markDate;
		const auto aWins = SyncChatMarkOutranks(cloud.cache(targets[0]), cloud.cache(targets[1]));
		auto &winner = aWins ? a : b;
		auto &loser = aWins ? b : a;
		const auto w = cloud.cache(winner.target);
		const auto recoverLoser = [&] {
			const auto resolution = Resolution(loser.device, cloud);
			REQUIRE(resolution.action == SyncChatAction::Follow && resolution.winner == w);
			loser.store(FtfNow(loser));
			REQUIRE(loser.journal.phase == SyncCarryPhase::PrepareCache);
			const auto before = CloudSnapshot(cloud);
			loser.run();
			CHECK(loser.journal.phase == SyncCarryPhase::FinishedForFollow);
			CHECK(CloudSnapshot(cloud) == before);
		};
		const auto recoverWinner = [&] {
			CHECK(Resolution(winner.device, cloud).winner == w);
			winner.run();
			CHECK(winner.journal.phase == SyncCarryPhase::Done);
		};
		if (random() % 2) {
			++loserFirst;
			recoverLoser();
			recoverWinner();
		} else {
			++winnerFirst;
			recoverWinner();
			recoverLoser();
		}
		CHECK(loser.journal.supersededBy == w && loser.journal.markAttempted);
		CHECK(loser.device.state.chat == loser.target);
		auto onward = Session(loser.device, cloud, winner.target, SyncCarryMode::Follow,
			SyncCarrySourceAccess::Readable, true, false, login);
		onward.run();
		CHECK(onward.journal.phase == SyncCarryPhase::Done);
		for (auto &device : devices) {
			if (device.state.chat != winner.target) {
				auto follow = Session(device, cloud, winner.target, SyncCarryMode::Follow,
					SyncCarrySourceAccess::Readable, random() % 2 == 0, false, login);
				follow.run();
			}
			CHECK(device.state.chat == winner.target && device.cache.chat == winner.target);
			CHECK(HoldsConfirmedHead(cloud, device, winner.target));
			const auto forwardOrder = Resolution(device, cloud);
			const auto reverseOrder = Resolution(device, cloud, true);
			CHECK(forwardOrder.action == reverseOrder.action && forwardOrder.winner == reverseOrder.winner);
		}
		for (const auto &message : heads) {
			CHECK(cloud.has(winner.target, message.reference.recordHash)
				|| cloud.has(old, message.reference.recordHash));
		}
		CHECK(cloud.has(winner.target, future.reference.recordHash));
	}
	CHECK(ties > 0 && loserFirst > 0 && winnerFirst > 0);
}

void CleanupAndInterruptedDelta() {
	Section = "cleanup and interrupted delta";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	auto cloud = Cloud();
	cloud.mark(old);
	auto d = Device('a', old, cloud.cache(old));
	d.publish(cloud, Text('a'));
	auto session = Session(d, cloud, next, SyncCarryMode::Switch, SyncCarrySourceAccess::Readable, true, true, QString(32, u'a'));
	session.run(SyncCarryPhase::Cleanup);
	const auto j = session.journal;
	const auto state = StateBytes(d.state);
	for (auto variant = 0; variant != 9; ++variant) {
		auto event = SyncCarryEvent(j, SyncCarryEventKind::SkipCleanup);
		event.outcome = SyncCarryOutcome::Success;
		event.state = d.state;
		event.noRequestInFlight = true;
		event.skipCleanupConfirmed = true;
		if (variant == 0) event.skipCleanupConfirmed = false;
		if (variant == 1) event.noRequestInFlight = false;
		if (variant == 2) ++event.revision;
		if (variant == 3) event.account.environment = 1;
		if (variant == 4) event.state->createdDevice = u"changed"_q;
		if (variant == 5) event.deleted.push_back({ Account, next, 123, QString(64, u'a') });
		if (variant == 6) event.outcome = SyncCarryOutcome::Unknown;
		if (variant == 7) event.source = next;
		if (variant == 8) event.operationId = QString(32, u'z');
		CHECK(!DeltaAdvance(j, event));
	}
	auto event = SyncCarryEvent(j, SyncCarryEventKind::SkipCleanup);
	event.outcome = SyncCarryOutcome::Success;
	event.state = d.state;
	event.noRequestInFlight = event.skipCleanupConfirmed = true;
	const auto skipped = DeltaStore(DeltaAdvance(j, event));
	CHECK(skipped.journal.phase == SyncCarryPhase::CleanupSkipped);
	CHECK(skipped.journal.abandonedCleanup.size() == 2);
	CHECK(!skipped.journal.cleanupChatGone);
	CHECK(!SyncCarryJournalHoldsWrites(skipped.journal));
	CHECK(NextSyncCarryJournalAction(skipped.journal) == SyncCarryJournalAction::None);
	CHECK(!CheckSyncCarryDeletion(skipped.journal, cloud.inventory(old).messages.front(), cloud.inventory(next)));
	CHECK(DeltaResume(skipped.journal, d.state, false).journal.phase == SyncCarryPhase::CleanupSkipped);
	for (auto variant = 0; variant != 5; ++variant) {
		event = SyncCarryEvent(j, SyncCarryEventKind::CleanupChatGone);
		event.outcome = SyncCarryOutcome::Success;
		event.state = d.state;
		event.noRequestInFlight = true;
		event.mark = { SyncCarryMarkOutcome::ChatGone, Account, old };
		if (variant == 0) event.mark.outcome = SyncCarryMarkOutcome::Unknown;
		if (variant == 1) event.mark.outcome = SyncCarryMarkOutcome::Absent;
		if (variant == 2) event.mark.chat = next;
		if (variant == 3) event.mark.account.environment = 1;
		const auto gone = DeltaAdvance(j, event);
		if (variant != 4) CHECK(!gone);
		else {
			REQUIRE(gone);
			CHECK(gone.journal.phase == SyncCarryPhase::Done);
			CHECK(gone.journal.cleanupChatGone);
			CHECK(gone.journal.abandonedCleanup.empty());
		}
	}
	CHECK(StateBytes(d.state) == state);
	for (auto at = 0; at != 5; ++at) {
		auto c = Cloud();
		c.mark(old);
		auto device = Device('a', old, c.cache(old));
		device.publish(c, Text('a'));
		const auto access = at == 1 ? SyncCarrySourceAccess::Protected : SyncCarrySourceAccess::Readable;
		auto s = Session(device, c, next, SyncCarryMode::Switch, access, true, false, QString(32, u'a'));
		const auto stop = at == 0 ? SyncCarryPhase::ResolveAgain : at == 1 ? SyncCarryPhase::StageOwn
			: at == 2 ? SyncCarryPhase::Mark : at == 3 ? SyncCarryPhase::CommitState : SyncCarryPhase::Cleanup;
		s.run(stop);
		if (at == 1) s.step();
		if (at == 2) s.step();
		auto snapshot = s.journal;
		snapshot.loginAuthority = QString(32, u'a');
		const auto before = SerializeSyncCarryJournal(snapshot).canonical;
		const auto current = StateBytes(device.state);
		CHECK(!ResumeSyncCarryJournal(snapshot, device.state, false, true, QString(32, u'b')));
		for (auto invalid = 0; invalid != 5; ++invalid) {
			CHECK(!AbandonInterruptedOperation(snapshot,
				invalid == 0 ? AccountKey{ 1, 777 } : Account,
				invalid == 1 ? QString(32, u'a') : QString(32, u'b'), invalid != 2,
				device.state, false, invalid == 3 ? QByteArray("bad") : device.stage, invalid != 4));
		}
		const auto abandoned = DeltaStore(AbandonInterruptedOperation(snapshot, Account,
			QString(32, u'b'), true, device.state, false, device.stage, true));
		CHECK(abandoned.journal.phase == SyncCarryPhase::InterruptedAbandoned);
		CHECK(abandoned.journal.interruptedState == current);
		CHECK(abandoned.journal.interruptedStage == device.stage);
		CHECK(abandoned.journal.markAttempted == snapshot.markAttempted);
		CHECK(abandoned.journal.deliveryHistory == snapshot.deliveryHistory);
		CHECK(abandoned.journal.ownRequestId == snapshot.ownRequestId);
		CHECK(abandoned.journal.markRequestId == snapshot.markRequestId);
		CHECK(abandoned.journal.plan.has_value() == snapshot.plan.has_value());
		CHECK(!SyncCarryJournalHoldsWrites(abandoned.journal));
		const auto terminal = ResumeSyncCarryJournal(abandoned.journal, device.state,
			false, true, QString(32, u'b'));
		REQUIRE(terminal);
		CHECK(terminal.canonical == abandoned.canonical);
		CHECK(!AbandonInterruptedOperation(snapshot, Account, QString(32, u'b'),
			true, std::nullopt, true, {}, true));
		CHECK(SyncCarryJournalForwards(abandoned.journal).empty());
		CHECK(!SyncCarryJournalPostsOwnRecord(abandoned.journal));
		CHECK(StateBytes(device.state) == current);
		CHECK(SerializeSyncCarryJournal(snapshot).canonical == before);
	}
}

void JournalDeltaGuards() {
	Section = "versioned journal guards";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	auto cloud = Cloud();
	cloud.mark(old);
	auto device = Device('a', old, cloud.cache(old));
	device.publish(cloud, Text('a'));
	auto repair = DeltaStore(BeginSyncCarryJournal(RepairStart(device))).journal;
	auto event = SyncCarryEvent(repair, SyncCarryEventKind::MarkPostPrepared);
	event.outcome = SyncCarryOutcome::Unknown;
	repair = DeltaStore(DeltaAdvance(repair, event)).journal;
	REQUIRE(repair.markAttempted);
	event = SyncCarryEvent(repair, SyncCarryEventKind::MarkRead);
	event.outcome = SyncCarryOutcome::Success;
	event.mark = { SyncCarryMarkOutcome::Absent, Account, old };
	event.noRequestInFlight = event.neverLandedConfirmed = true;
	event.neverLandedRequestId = repair.markRequestId;
	for (auto invalid = 0; invalid != 3; ++invalid) {
		auto bad = event;
		if (invalid == 0) bad.noRequestInFlight = false;
		if (invalid == 1) bad.neverLandedConfirmed = false;
		if (invalid == 2) bad.neverLandedRequestId = u"99999"_q;
		const auto held = DeltaAdvance(repair, bad);
		CHECK(held.status == SyncCarryJournalStatus::Wait);
		CHECK(held.journal.markAttempted);
		CHECK(NextSyncCarryJournalAction(DeltaStore(held).journal) == SyncCarryJournalAction::ReadMark);
	}
	const auto retry = DeltaAdvance(repair, event);
	CHECK(retry.status == SyncCarryJournalStatus::Retry);
	CHECK(!retry.journal.markAttempted);
	CHECK(NextSyncCarryJournalAction(DeltaStore(retry).journal) == SyncCarryJournalAction::PostMark);
	CHECK(!AdvanceSyncCarryJournal(repair, event));
	CHECK(!AdvanceSyncCarryJournal(repair, event, QString(32, u'b')));
	CHECK(!ResumeSyncCarryJournal(repair, device.state, false, true));
	CHECK(!ResumeSyncCarryJournal(repair, device.state, false, true, QString(32, u'b')));
	for (auto invalid = 0; invalid != 7; ++invalid) {
		auto json = QJsonDocument::fromJson(SerializeSyncCarryJournal(repair).canonical).object();
		if (invalid == 0) json.remove(u"reader_baseline"_q);
		if (invalid == 1) json.insert(u"login_authority"_q, QString());
		if (invalid == 2) json.insert(u"login_authority"_q, u"bad"_q);
		auto readers = json.value(u"reader_baseline"_q).toObject();
		if (invalid == 3) readers.insert(u"unknown"_q, true);
		if (invalid == 4) readers.insert(u"members"_q, -1);
		if (invalid == 5) readers.insert(u"admins"_q, u"unknown"_q);
		if (invalid == 6) readers.insert(u"members"_q, double(9007199254740992ULL));
		if (invalid >= 3) json.insert(u"reader_baseline"_q, readers);
		CHECK(!ParseSyncCarryJournal(QJsonDocument(json).toJson()));
	}
	auto oversize = repair;
	oversize.preserved.insert(u"future"_q, QString(4 * 1024 * 1024, u'x'));
	CHECK(!SerializeSyncCarryJournal(oversize));
	CHECK(!DeltaAdvance(oversize, event));
	auto session = Session(device, cloud, next, SyncCarryMode::Switch,
		SyncCarrySourceAccess::Readable, false, false, QString(32, u'a'));
	session.run(SyncCarryPhase::Deliver);
	event = SyncCarryEvent(session.journal, SyncCarryEventKind::DeliveryPrepared);
	event.outcome = SyncCarryOutcome::Unknown;
	const auto attempted = DeltaStore(DeltaAdvance(session.journal, event));
	REQUIRE(!attempted.journal.deliveryHistory.empty());
	auto json = QJsonDocument::fromJson(attempted.canonical).object();
	auto refs = json.value(u"delivery_history"_q).toArray();
	refs.push_back(refs.first());
	json.insert(u"delivery_history"_q, refs);
	CHECK(!ParseSyncCarryJournal(QJsonDocument(json).toJson()));
	refs.removeLast();
	auto reference = refs.first().toObject();
	reference.insert(u"future_reference"_q, 42);
	refs[0] = reference;
	json.insert(u"delivery_history"_q, refs);
	const auto future = DeltaStore(ParseSyncCarryJournal(QJsonDocument(json).toJson()));
	const auto roundtrip = QJsonDocument::fromJson(future.canonical).object();
	CHECK(roundtrip.value(u"delivery_history"_q).toArray().first().toObject()
		.value(u"future_reference"_q).toInt() == 42);
	const auto discarded = DeltaStore(AbandonInterruptedOperation(future.journal, Account,
		QString(32, u'b'), true, device.state, false, device.stage, true));
	CHECK(discarded.journal.deliveryAttempted);
	CHECK(discarded.journal.deliveryHistory == attempted.journal.deliveryHistory);
	CHECK(discarded.journal.plan->forwards == attempted.journal.plan->forwards);
	CHECK(!SyncCarryJournalHoldsWrites(discarded.journal));
	for (auto invalid = 0; invalid != 3; ++invalid) {
		auto broken = QJsonDocument::fromJson(discarded.canonical).object();
		if (invalid == 0) broken.insert(u"interrupted_state"_q, QString());
		if (invalid == 1) broken.insert(u"interrupted_stage"_q, u"unrelated stage"_q);
		if (invalid == 2) broken.insert(u"abandoning_login"_q, discarded.journal.loginAuthority);
		CHECK(!ParseSyncCarryJournal(QJsonDocument(broken).toJson()));
	}
}

void ReaderStaleAndFillDelta() {
	Section = "readers stale marks and fill delta";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	auto cloud = Cloud();
	const auto mark = cloud.mark(old);
	auto d = Device('a', old, cloud.cache(old));
	d.publish(cloud, Text('a'));
	auto session = Session(d, cloud, next, SyncCarryMode::Switch,
		SyncCarrySourceAccess::Readable, false, false, QString(32, u'a'));
	const auto j = DeltaStore(SerializeSyncCarryJournal(session.journal)).journal;
	REQUIRE(j.readerBaseline);
	CHECK(*j.readerBaseline == Eligible(next).readers);
	CHECK(CheckSyncCarryReaders(j, Eligible(next)) == SyncCarryReaderVerdict::Proceed);
	auto added = Eligible(next);
	added.readers.otherMembers = 1;
	CHECK(CheckSyncCarryReaders(j, added) == SyncCarryReaderVerdict::WarnAndProceed);
	added.readers.populationUnknown = true;
	CHECK(CheckSyncCarryReaders(j, added) == SyncCarryReaderVerdict::Hold);
	added = Eligible(next);
	added.refusal = SyncChatRefusal::CannotSendFiles;
	CHECK(CheckSyncCarryReaders(j, added) == SyncCarryReaderVerdict::Hold);
	session.run(SyncCarryPhase::Deliver);
	auto intent = SyncCarryEvent(session.journal, SyncCarryEventKind::DeliveryPrepared);
	intent.outcome = SyncCarryOutcome::Unknown;
	const auto prepared = DeltaStore(DeltaAdvance(session.journal, intent));
	CHECK(prepared.journal.deliveryAttempted);
	CHECK(prepared.journal.readerBaseline == j.readerBaseline);
	CHECK(prepared.journal.deliveryHistory == session.journal.plan->forwards);
	cloud.mark(next);
	auto resolution = SyncChatResolution();
	resolution.action = SyncChatAction::Use;
	resolution.cache = resolution.winner = cloud.cache(next);
	resolution.postingHeld = false;
	CHECK(CheckStaleMarkRemoval(Account, d.cache, mark, resolution, true));
	for (auto variant = 0; variant != 10; ++variant) {
		auto r = resolution;
		auto exact = mark;
		auto account = Account;
		if (variant == 0) exact.ownership = SyncChatMessageOwnership::Other;
		if (variant == 1) exact.ownership = SyncChatMessageOwnership::Unknown;
		if (variant == 2) ++exact.messageId;
		if (variant == 3) exact.chat = next;
		if (variant == 4) exact.text = u"ordinary text"_q;
		if (variant == 5) r.winner = r.cache = d.cache;
		if (variant == 6) r.action = SyncChatAction::Searching;
		if (variant == 7) account.environment = 1;
		if (variant == 8) r.postingHeld = true;
		CHECK(!CheckStaleMarkRemoval(account, d.cache, exact, r, variant != 9));
	}
	for (const auto access : { SyncCarrySourceAccess::Unknown, SyncCarrySourceAccess::Protected,
		SyncCarrySourceAccess::Unreadable, SyncCarrySourceAccess::ChatGone }) {
		auto input = Input(d, cloud, next, access, SyncCarryMode::Fill);
		input.state.reset();
		CHECK(!PlanCarry(input));
	}
	auto fill = Input(d, cloud, next, SyncCarrySourceAccess::Readable, SyncCarryMode::Fill);
	fill.state.reset();
	fill.oldInventory.messages.clear();
	const auto empty = PlanCarry(fill);
	REQUIRE(empty);
	CHECK(empty.plan.requiredHashes.empty());
	fill.oldInventory.complete = false;
	CHECK(!PlanCarry(fill));
	auto forged = empty.plan;
	forged.access = SyncCarrySourceAccess::ChatGone;
	forged.sourceStamp.clear();
	CHECK(!ParseSyncCarryPlan(SyncCarryPlanJson(forged)));
	CHECK(!PrepareSyncCarryCommit(forged, std::nullopt, cloud.inventory(next)));
	auto document = QJsonDocument::fromJson(SerializeSyncCarryJournal(j).canonical).object();
	document.insert(u"future"_q, 42);
	auto baseline = document.value(u"reader_baseline"_q).toObject();
	baseline.insert(u"future_reader"_q, u"preserved"_q);
	document.insert(u"reader_baseline"_q, baseline);
	const auto future = DeltaStore(ParseSyncCarryJournal(QJsonDocument(document).toJson()));
	const auto saved = QJsonDocument::fromJson(future.canonical).object();
	CHECK(saved.value(u"future"_q).toInt() == 42);
	CHECK(saved.value(u"reader_baseline"_q).toObject().value(u"future_reader"_q).toString() == u"preserved"_q);
	CHECK(ParseSyncCarryJournal("{\"version\":3}").status == SyncCarryJournalStatus::NewerVersion);
}

void DeltaPreconditionAudit() {
	Section = "delta precondition audit";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	auto cloud = Cloud();
	cloud.mark(old);
	auto device = Device('a', old, cloud.cache(old));
	device.publish(cloud, Text('a'));
	const auto beforeMove = device.state;
	device.prepare(Text('a', 2));
	const auto pending = device.state;
	const auto stage = device.stage;
	for (auto invalid = 0; invalid != 7; ++invalid) {
		auto state = pending;
		auto inventory = cloud.inventory(old);
		auto bytes = stage;
		if (invalid == 0) state.install.clear();
		if (invalid == 1) state.config.issuedRecords.clear();
		if (invalid == 2) state.configData.pending = QString(64, u'f');
		if (invalid == 3) bytes = cloud.inventory(old).messages.front().bytes;
		if (invalid == 4) inventory.messages.front().reference.recordHash = QString(64, u'f');
		if (invalid == 5) inventory.messages.front().reference.messageId = 0;
		if (invalid == 6) inventory.messages.front().bytes = "bad";
		CHECK(!ReconcileProtectedStageForCarry(Account, state, old,
			SyncCarrySourceAccess::Protected, SyncCarrySendIntent::Issued,
			bytes, device.localText, inventory));
	}
	const auto absent = ReconcileProtectedStageForCarry(Account, pending, old,
		SyncCarrySourceAccess::Protected, SyncCarrySendIntent::NotIssued,
		stage, device.localText);
	REQUIRE(absent);
	CHECK(absent.restageInTarget);
	CHECK(ReconcileProtectedStageForCarry(Account, beforeMove, old,
		SyncCarrySourceAccess::Protected, SyncCarrySendIntent::Unknown,
		{}, {}).status == SyncCarryPendingStatus::NoPending);
	const auto record = cloud.post(old, stage);
	const auto found = ReconcileProtectedStageForCarry(Account, pending, old,
		SyncCarrySourceAccess::Protected, SyncCarrySendIntent::Issued,
		stage, device.localText, cloud.inventory(old));
	const auto normal = FinishPendingStageForCarry(Account, pending, stage, record);
	REQUIRE(found && normal);
	CHECK(StateBytes(found.state) == StateBytes(normal.state));
	CHECK(!found.restageInTarget);
	device.state = normal.state;
	device.stage.clear();
	for (auto invalid = 0; invalid != 9; ++invalid) {
		auto start = RepairStart(device);
		if (invalid == 0) start.loginAuthority.clear();
		if (invalid == 1) start.loginAuthority = u"bad"_q;
		if (invalid == 2) start.state = pending;
		if (invalid == 3) start.resolution.cache.reset();
		if (invalid == 4) start.resolution.cache->markId = 0;
		if (invalid == 5) start.resolution.action = SyncChatAction::Use;
		if (invalid == 6) start.sourceAccess = SyncCarrySourceAccess::Unreadable;
		if (invalid == 7) start.markText = u"not a mark"_q;
		if (invalid == 8) start.markRequestId = u"0"_q;
		CHECK(!BeginSyncCarryJournal(start));
	}
	auto session = Session(device, cloud, next, SyncCarryMode::Switch,
		SyncCarrySourceAccess::Readable, true, true, QString(32, u'a'));
	for (const auto phase : { SyncCarryPhase::ResolveAgain, SyncCarryPhase::Deliver,
		SyncCarryPhase::Mark, SyncCarryPhase::CommitState }) {
		session.run(phase);
		for (const auto kind : { SyncCarryEventKind::SkipCleanup, SyncCarryEventKind::CleanupChatGone }) {
			auto event = SyncCarryEvent(session.journal, kind);
			event.outcome = SyncCarryOutcome::Success;
			event.state = device.state;
			event.noRequestInFlight = event.skipCleanupConfirmed = true;
			event.mark = { SyncCarryMarkOutcome::ChatGone, Account, old };
			CHECK(!DeltaAdvance(session.journal, event));
		}
	}
	const auto precommitState = device.state;
	session.run(SyncCarryPhase::Cleanup);
	const auto journal = session.journal;
	CHECK(!AbandonInterruptedOperation(journal, Account, QString(32, u'b'),
		true, precommitState, false, {}, true));
	for (const auto &login : { QString(), u"bad"_q, journal.loginAuthority }) {
		CHECK(!AbandonInterruptedOperation(journal, Account, login,
			true, device.state, false, {}, true));
	}
	const auto remaining = SyncCarryCleanupCandidates(*journal.plan, cloud.inventory(next));
	REQUIRE(!remaining.empty());
	auto event = SyncCarryEvent(journal, SyncCarryEventKind::SkipCleanup);
	event.outcome = SyncCarryOutcome::Success;
	event.state = device.state;
	event.noRequestInFlight = event.skipCleanupConfirmed = true;
	event.deleted.push_back(remaining.front());
	const auto partial = DeltaStore(DeltaAdvance(journal, event));
	CHECK(std::find(partial.journal.abandonedCleanup.begin(),
		partial.journal.abandonedCleanup.end(), remaining.front())
		== partial.journal.abandonedCleanup.end());
	CHECK(!partial.journal.abandonedCleanup.empty());
	CHECK(DeltaResume(partial.journal, device.state, false).canonical == partial.canonical);
	event.deleted.push_back(remaining.front());
	CHECK(!DeltaAdvance(journal, event));
	event = SyncCarryEvent(journal, SyncCarryEventKind::CleanupChatGone);
	event.outcome = SyncCarryOutcome::Success;
	event.state = device.state;
	event.noRequestInFlight = true;
	event.mark = { SyncCarryMarkOutcome::ChatGone, Account, old };
	const auto gone = DeltaStore(DeltaAdvance(journal, event));
	CHECK(!SyncCarryJournalHoldsWrites(gone.journal));
	CHECK(DeltaResume(gone.journal, device.state, false).canonical == gone.canonical);
	const auto abandoned = DeltaStore(AbandonInterruptedOperation(journal, Account,
		QString(32, u'b'), true, device.state, false, {}, true));
	auto forged = abandoned.journal;
	forged.interruptedState = StateBytes(precommitState);
	CHECK(!SerializeSyncCarryJournal(forged));
	CHECK(!AbandonInterruptedOperation(abandoned.journal, Account,
		QString(32, u'c'), true, device.state, false, {}, true));
	auto stale = device.cache;
	const auto exact = cloud.marks.at(cloud.key(stale.chat));
	cloud.mark(old);
	auto resolution = SyncChatResolution();
	resolution.action = SyncChatAction::Use;
	resolution.cache = resolution.winner = cloud.cache(old);
	resolution.postingHeld = false;
	CHECK(CheckStaleMarkRemoval(Account, stale, exact, resolution, true));
	for (auto invalid = 0; invalid != 7; ++invalid) {
		auto readback = exact;
		auto fresh = resolution;
		if (invalid == 0) readback.hasMedia = true;
		if (invalid == 1) ++readback.date;
		if (invalid == 2) readback.outcome = SyncCarryMarkOutcome::Unknown;
		if (invalid == 3) fresh.winner.reset();
		if (invalid == 4) fresh.cache = stale;
		if (invalid == 5) fresh.winner->markDate = 0;
		if (invalid == 6) readback.account.userId = 778;
		CHECK(!CheckStaleMarkRemoval(Account, stale, readback, fresh, true));
	}
	auto baseline = journal;
	baseline.phase = SyncCarryPhase::Cleanup;
	for (auto invalid = 0; invalid != 3; ++invalid) {
		auto eligibility = Eligible(next);
		if (invalid == 0) eligibility.readers.otherMembers.reset();
		if (invalid == 1) eligibility.readers.otherAdmins.reset();
		if (invalid == 2) baseline.readerBaseline.reset();
		CHECK(CheckSyncCarryReaders(baseline, eligibility) == SyncCarryReaderVerdict::Hold);
	}
	CHECK(CheckSyncCarryReaders(gone.journal, Eligible(next)) == SyncCarryReaderVerdict::Hold);
}

void DeferredVersionAudit() {
	Section = "deferred exact version audit";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	auto cloud = Cloud();
	cloud.mark(old);
	auto device = Device('a', old, cloud.cache(old));
	device.publish(cloud, Text('a'));
	device.publish(cloud, Text('a', 2));
	device.prepare(Text('a', 3));
	const auto original = ParseSyncEnvelope(device.stage);
	const auto payload = InspectConfigPayload(original);
	auto session = Session(device, cloud, next, SyncCarryMode::Switch,
		SyncCarrySourceAccess::Protected, false, false, QString(32, u'a'));
	for (auto step = 0; step != 100 && session.journal.phase != SyncCarryPhase::Mark; ++step) {
		ProtectedStep(session, SyncCarrySendIntent::NotIssued);
	}
	REQUIRE(session.journal.phase == SyncCarryPhase::Mark);
	const auto target = ParseSyncEnvelope(session.journal.targetPendingRecord);
	const auto restaged = InspectConfigPayload(target);
	REQUIRE(target.header && original.header);
	CHECK(restaged.version.parents == payload.version.parents);
	CHECK(restaged.version.lineage == payload.version.lineage);
	CHECK(restaged.version.key == payload.version.key);
	CHECK(restaged.text == payload.text);
	CHECK(target.header->at == original.header->at);
	CHECK(target.header->writerPlatform == original.header->writerPlatform);
	CHECK(target.header->writerApp == original.header->writerApp);
	auto forgedVersion = restaged.version;
	REQUIRE(forgedVersion.lineage.size() > 1);
	forgedVersion.parents.push_back(forgedVersion.lineage.back());
	const auto forged = BuildConfigRecord({
		.text = restaged.text,
		.version = forgedVersion,
		.space = target.header->space,
		.install = target.header->writerInstall,
		.device = target.header->writerDevice,
		.platform = target.header->writerPlatform,
		.app = target.header->writerApp,
		.seq = target.header->seq,
		.at = target.header->at,
	});
	REQUIRE(forged);
	auto broken = session.journal;
	broken.targetPendingRecord = forged.canonical;
	CHECK(!SerializeSyncCarryJournal(broken));
	auto json = QJsonDocument::fromJson(SerializeSyncCarryJournal(session.journal).canonical).object();
	json.insert(u"target_pending_record"_q, QString::fromUtf8(forged.canonical));
	CHECK(!ParseSyncCarryJournal(QJsonDocument(json).toJson()));
}

QByteArray ChangedCarryHeader(const QByteArray &bytes, int variant) {
	const auto parsed = ParseSyncEnvelope(bytes);
	const auto payload = InspectConfigPayload(parsed);
	REQUIRE(parsed.header && payload);
	const auto changed = BuildConfigRecord({
		.text = payload.text,
		.version = payload.version,
		.space = parsed.header->space,
		.install = parsed.header->writerInstall,
		.device = parsed.header->writerDevice,
		.platform = variant == 1 ? u"changed platform"_q : parsed.header->writerPlatform,
		.app = variant == 2 ? u"changed app"_q : parsed.header->writerApp,
		.seq = parsed.header->seq,
		.at = parsed.header->at + (variant == 0 ? 1 : 0),
	});
	REQUIRE(changed);
	return changed.canonical;
}

SyncCarryJournalEvent PendingBarrierEvent(
		const SyncCarryJournal &journal,
		const SyncLocalState &state,
		const QByteArray &stage) {
	const auto kind = journal.phase == SyncCarryPhase::CommitState
		? SyncCarryEventKind::StateObserved : journal.phase == SyncCarryPhase::RetireOwnStage
		? SyncCarryEventKind::StageRetired : SyncCarryEventKind::CacheCompleted;
	auto event = SyncCarryEvent(journal, kind);
	event.outcome = SyncCarryOutcome::Success;
	event.objectsDurable = true;
	event.state = state;
	event.stagedRecord = stage;
	event.targetPendingStageDurable = !stage.isEmpty();
	event.noPendingFiles = stage.isEmpty();
	event.retiredStageHash = journal.plan->ownRecordHash;
	event.cache = { journal.operationId, Account, *journal.targetMark, true };
	return event;
}

void RecoveredPendingRecordBindings() {
	Section = "recovered pending record bindings";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	auto cloud = Cloud();
	cloud.mark(old);
	auto device = Device('a', old, cloud.cache(old));
	device.publish(cloud, Text('a'));
	device.prepare(Text('a', 2));
	auto session = Session(device, cloud, next, SyncCarryMode::Switch,
		SyncCarrySourceAccess::Protected, false, false, QString(32, u'a'));
	auto snapshots = std::map<SyncCarryPhase, SyncCarryJournal>();
	for (auto step = 0; step != 100; ++step) {
		const auto phase = session.journal.phase;
		if (session.journal.commitPrepared) snapshots[phase] = session.journal;
		if (phase == SyncCarryPhase::CompleteCache) break;
		ProtectedStep(session, SyncCarrySendIntent::NotIssued);
	}
	REQUIRE(session.journal.phase == SyncCarryPhase::CompleteCache);
	const auto committed = device.state;
	const auto record = session.journal.targetPendingRecord;
	REQUIRE(!record.isEmpty() && committed.config.pendingSeq);
	for (const auto &[phase, journal] : snapshots) {
		const auto canonical = SerializeSyncCarryJournal(journal).canonical;
		REQUIRE(!canonical.isEmpty());
		for (auto variant = -1; variant != 3; ++variant) {
			const auto bytes = variant < 0 ? QByteArray() : ChangedCarryHeader(record, variant);
			auto broken = journal;
			broken.targetPendingRecord = bytes;
			CHECK(!SerializeSyncCarryJournal(broken));
			auto json = QJsonDocument::fromJson(canonical).object();
			json.insert(u"target_pending_record"_q, QString::fromUtf8(bytes));
			CHECK(!ParseSyncCarryJournal(QJsonDocument(json).toJson()));
			if (phase == SyncCarryPhase::RetireOwnStage || phase == SyncCarryPhase::CompleteCache) {
				const auto event = PendingBarrierEvent(broken, committed, bytes);
				CHECK(!DeltaAdvance(broken, event));
			}
		}
	}
	for (const auto phase : { SyncCarryPhase::CommitState,
		SyncCarryPhase::RetireOwnStage, SyncCarryPhase::CompleteCache }) {
		const auto journal = snapshots.at(phase);
		for (auto variant = 0; variant != 2; ++variant) {
			auto wrongState = committed;
			if (variant == 0) wrongState.config.issuedRecords.back().recordHash = QString(64, u'f');
			if (variant == 1) wrongState.configData.pending = wrongState.configData.base;
			auto broken = journal;
			broken.nextStateHash = SyncCarryRecordHash(StateBytes(wrongState));
			const auto event = PendingBarrierEvent(broken, wrongState, record);
			CHECK(!DeltaAdvance(broken, event));
			CHECK(!DeltaResume(broken, wrongState, false, true));
		}
		auto missing = journal;
		missing.deferredRecord.clear();
		missing.targetPendingRecord.clear();
		const auto event = PendingBarrierEvent(missing, committed, {});
		CHECK(!DeltaAdvance(missing, event));
		CHECK(!DeltaResume(missing, committed, false, true));
	}
	CHECK(DeltaAdvance(session.journal,
		PendingBarrierEvent(session.journal, committed, record)));
	for (auto variant = -1; variant != 3; ++variant) {
		auto broken = session.journal;
		const auto bytes = variant < 0 ? QByteArray() : ChangedCarryHeader(record, variant);
		broken.targetPendingRecord = bytes;
		CHECK(!AbandonInterruptedOperation(broken, Account, QString(32, u'b'),
			true, committed, false, record, true));
	}
	const auto abandoned = AbandonInterruptedOperation(session.journal, Account,
		QString(32, u'b'), true, committed, false, record, true);
	REQUIRE(abandoned);
	for (auto variant = -1; variant != 3; ++variant) {
		const auto bytes = variant < 0 ? QByteArray() : ChangedCarryHeader(record, variant);
		auto broken = abandoned.journal;
		broken.interruptedStage = bytes;
		CHECK(!SerializeSyncCarryJournal(broken));
		auto json = QJsonDocument::fromJson(abandoned.canonical).object();
		json.insert(u"interrupted_stage"_q, QString::fromUtf8(bytes));
		CHECK(!ParseSyncCarryJournal(QJsonDocument(json).toJson()));
	}
	auto broken = session.journal;
	broken.deferredRecord = ChangedCarryHeader(broken.deferredRecord, 0);
	CHECK(!SerializeSyncCarryJournal(broken));
	broken = session.journal;
	broken.plan->ownRecord = ChangedCarryHeader(broken.plan->ownRecord, 0);
	CHECK(!SerializeSyncCarryJournal(broken));
}

void OtherRecoveredRecordBindings() {
	Section = "other recovered record bindings";
	const auto old = Peer(u"chat:10"_q);
	const auto next = Peer(u"chat:20"_q);
	auto cloud = Cloud();
	cloud.mark(old);
	auto device = Device('a', old, cloud.cache(old));
	device.publish(cloud, Text('a'));
	device.prepare(Text('a', 2));
	const auto pending = device.state;
	const auto pendingBytes = device.stage;
	auto session = Session(device, cloud, next, SyncCarryMode::Switch,
		SyncCarrySourceAccess::Protected, false, false, QString(32, u'a'));
	ProtectedStep(session, SyncCarrySendIntent::NotIssued);
	REQUIRE(session.journal.phase == SyncCarryPhase::WritePendingState);
	const auto prepared = ParseSyncLocalState(device.images.at(session.journal.pendingStateHash));
	REQUIRE(prepared);
	for (auto variant = 0; variant != 3; ++variant) {
		auto broken = session.journal;
		broken.deferredRecord = ChangedCarryHeader(broken.deferredRecord, variant);
		broken.pendingStageHash = SyncCarryRecordHash(broken.deferredRecord);
		REQUIRE(SerializeSyncCarryJournal(broken));
		auto event = SyncCarryEvent(broken, SyncCarryEventKind::StateObserved);
		event.outcome = SyncCarryOutcome::Success;
		event.objectsDurable = true;
		event.state = prepared.state;
		CHECK(!DeltaAdvance(broken, event));
		CHECK(!DeltaResume(broken, prepared.state, false, true));
		CHECK(!AbandonInterruptedOperation(broken, Account, QString(32, u'b'),
			true, pending, false, pendingBytes, true));
	}
	auto broken = session.journal;
	broken.deferredRecord = QJsonDocument::fromJson(broken.deferredRecord).toJson();
	broken.pendingStageHash = SyncCarryRecordHash(broken.deferredRecord);
	CHECK(!SerializeSyncCarryJournal(broken));
	auto json = QJsonDocument::fromJson(SerializeSyncCarryJournal(session.journal).canonical).object();
	json.insert(u"deferred_record"_q, QString::fromUtf8(broken.deferredRecord));
	json.insert(u"pending_stage_hash"_q, broken.pendingStageHash);
	CHECK(!ParseSyncCarryJournal(QJsonDocument(json).toJson()));
	CHECK(!ReconcileProtectedStageForCarry(Account, pending, old,
		SyncCarrySourceAccess::Protected, SyncCarrySendIntent::NotIssued,
		broken.deferredRecord, device.localText));
	auto ownCloud = Cloud();
	ownCloud.mark(old);
	auto ownDevice = Device('b', old, ownCloud.cache(old));
	ownDevice.publish(ownCloud, Text('b'));
	auto own = Session(ownDevice, ownCloud, next, SyncCarryMode::Switch,
		SyncCarrySourceAccess::Protected, false, false, QString(32, u'a'));
	own.run(SyncCarryPhase::Deliver);
	auto changed = own.journal;
	const auto oldHash = changed.plan->ownRecordHash;
	changed.plan->ownRecord = ChangedCarryHeader(changed.plan->ownRecord, 0);
	const auto newHash = SyncCarryRecordHash(changed.plan->ownRecord);
	changed.plan->ownRecordHash = newHash;
	changed.plan->issued.back().recordHash = newHash;
	std::replace(changed.plan->requiredHashes.begin(), changed.plan->requiredHashes.end(), oldHash, newHash);
	std::sort(changed.plan->requiredHashes.begin(), changed.plan->requiredHashes.end());
	std::replace(changed.missingHashes.begin(), changed.missingHashes.end(), oldHash, newHash);
	std::sort(changed.missingHashes.begin(), changed.missingHashes.end());
	REQUIRE(SerializeSyncCarryJournal(changed));
	CHECK(!SyncCarryPlanUsesState(*changed.plan, ownDevice.state));
	CHECK(!DeltaResume(changed, ownDevice.state, false, true));
	own.run(SyncCarryPhase::RetireOwnStage);
	auto retired = SyncCarryEvent(own.journal, SyncCarryEventKind::StageRetired);
	retired.outcome = SyncCarryOutcome::Success;
	retired.retiredStageHash = own.journal.plan->ownRecordHash;
	retired.noPendingFiles = true;
	CHECK(DeltaAdvance(own.journal, retired));
	auto uncertain = SyncCarryEvent(own.journal, SyncCarryEventKind::StageRetired);
	uncertain.outcome = SyncCarryOutcome::Unknown;
	const auto held = DeltaAdvance(own.journal, uncertain);
	CHECK(held.status == SyncCarryJournalStatus::Wait && !held.canonical.isEmpty());
	auto readCloud = Cloud();
	readCloud.mark(old);
	auto readDevice = Device('c', old, readCloud.cache(old));
	readDevice.publish(readCloud, Text('c'));
	auto read = Session(readDevice, readCloud, next, SyncCarryMode::Switch,
		SyncCarrySourceAccess::Readable, true, false, QString(32, u'a'));
	read.run(SyncCarryPhase::Cleanup);
	auto metadata = read.journal;
	REQUIRE(!metadata.plan->cleanupCandidates.empty());
	metadata.plan->cleanupCandidates.front().header->writerApp = u"changed metadata"_q;
	REQUIRE(SerializeSyncCarryJournal(metadata));
	CHECK(!CheckSyncCarryDeletion(metadata, readCloud.inventory(old).messages.front(), readCloud.inventory(next)));
}

}

int main() {
	ProtectedPendingDelta();
	RepairMarkDelta();
	FinishThenFollowDelta();
	FinishThenFollowUnmarked();
	FinishThenFollowTie();
	FinishThenFollowScope();
	FinishThenFollowCrashes();
	FinishThenFollowProtected();
	FinishThenFollowRepair();
	FinishThenFollowSeeded();
	CleanupAndInterruptedDelta();
	ReaderStaleAndFillDelta();
	JournalDeltaGuards();
	DeltaPreconditionAudit();
	DeferredVersionAudit();
	RecoveredPendingRecordBindings();
	OtherRecoveredRecordBindings();
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
