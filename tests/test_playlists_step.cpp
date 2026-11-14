#include "purple/purple_playlists_step.h"
#include "purple/purple_types.h"

#include <QtCore/QCryptographicHash>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <map>
#include <random>
#include <set>

namespace {
using namespace Purple;
auto Checks = 0;
auto Failures = 0;
void Check(bool ok, const char *expression, int line) {
	++Checks;
	if (!ok) {
		++Failures;
		std::printf("FAIL %d: %s\n", line, expression);
	}
}
#define CHECK(...) Check(bool((__VA_ARGS__)), #__VA_ARGS__, __LINE__)
constexpr auto SelfId = uint64_t(7001);
const auto Chat = PeerToken{ PeerTokenKind::Self, 0 };
const auto IdA = u"pl-aaaaaaaaaaaaaaab"_q;
const auto IdB = u"pl-aaaaaaaaaaaaaaac"_q;
const auto IdC = u"pl-aaaaaaaaaaaaaaad"_q;

QByteArray Canonical(const PlaylistLibrary &library) {
	const auto result = SerializePlaylistLibrary(library, SelfId);
	CHECK(result);
	return result.canonical;
}
QString Hash(const PlaylistLibrary &library) {
	return QString::fromLatin1(QCryptographicHash::hash(Canonical(library),
		QCryptographicHash::Sha256).toHex());
}
QByteArray Zip(const QByteArray &bytes) {
	const auto result = EncodePlaylistBytes(bytes);
	CHECK(result);
	return result.bytes;
}
PlaylistLibrary Join(const PlaylistLibrary &a, const PlaylistLibrary &b) {
	const auto result = JoinPlaylistLibraries(a, b, SelfId);
	CHECK(result);
	return result.library;
}
PlaylistLibrary Title(QString id, QString title, uint64_t stamp) {
	PlaylistLibrary result;
	result.playlists[id].title = PlaylistTitleRegister{ std::move(title), stamp };
	return result;
}
PlaylistLibrary Visible(QString id = IdA, uint64_t stamp = 10) {
	auto result = Title(id, u"Playlist"_q, stamp);
	result.playlists[id].deleted = PlaylistDeletedRegister{ false, stamp };
	return result;
}
PlaylistRecordMessageMeta Meta() {
	PlaylistRecordMessageMeta result;
	result.isRegularMessage = true;
	result.isDocument = true;
	result.caption = PlaylistRecordCaption();
	result.fileNames = { PlaylistRecordFileName() };
	return result;
}
PlaylistRecordPayloadProof Proof(int32_t id, const PlaylistLibrary &library) {
	PlaylistRecordPayloadProof result;
	result.chat = Chat;
	result.messageId = id;
	result.documentId = uint64_t(id) + 1000;
	result.origin = PlaylistPayloadOrigin::DownloadedThisStep;
	result.readSucceeded = true;
	result.compressedBytes = Zip(Canonical(library));
	return result;
}
void Add(PlaylistStepInput &input, int32_t id, const PlaylistLibrary &library) {
	input.discovery.listedObservations.push_back(PlaylistInventoryObservation{
		.messageId = id,
		.documentId = uint64_t(id) + 1000,
		.ownership = SyncChatMessageOwnership::Own,
		.meta = Meta(),
	});
	input.payloads.push_back(Proof(id, library));
}
PlaylistStepInput Input(const PlaylistLibrary &library = {}) {
	PlaylistStepInput result;
	result.selfUserId = SelfId;
	result.accountAuthorized = true;
	result.libraryRead = PlaylistStepLocalRead::Succeeded;
	result.library = library;
	result.stateRead = PlaylistStepLocalRead::Succeeded;
	result.state.chat = Chat;
	result.discovery.selfUserId = SelfId;
	result.discovery.currentChat = Chat;
	result.discovery.listingSucceeded = true;
	result.discovery.listingExhausted = true;
	result.clock.selfUserId = SelfId;
	result.clock.chat = Chat;
	result.clock.nowMs = 10000;
	result.clock.requestSucceeded = true;
	return result;
}
PlaylistLocalMemoEntry Memo(int32_t id, const PlaylistLibrary &library) {
	PlaylistLocalMemoEntry result;
	result.documentId = uint64_t(id) + 1000;
	result.classifier = kPlaylistRecordClassifierVersion;
	result.state = PlaylistLocalMemoState::Valid;
	result.canonicalHash = Hash(library);
	return result;
}
void CheckBlocked(const PlaylistStepResult &result) {
	CHECK(result.status == PlaylistStepStatus::Blocked);
	CHECK(!result.library && !result.state);
	CHECK(!result.writeLibrary && !result.writeState && !result.publish());
	CHECK(result.prune.prune.empty() && result.purgedIds.empty());
	CHECK(result.canonicalBytes.isEmpty() && result.publicationBytes.isEmpty());
}
void CheckState(const PlaylistStepResult &result) {
	CHECK(result.status == PlaylistStepStatus::Planned);
	CHECK(result.library && result.state);
	if (!result.library || !result.state) return;
	CHECK(ValidatePlaylistLibrary(*result.library, SelfId));
	CHECK(ValidatePlaylistLocalState(*result.state, SelfId));
	CHECK(Canonical(*result.library) == result.canonicalBytes);
	CHECK(Hash(*result.library) == result.canonicalHash);
	const auto stateBytes = SerializePlaylistLocalState(*result.state, SelfId);
	CHECK(stateBytes);
	CHECK(ParsePlaylistLocalState(stateBytes.json, SelfId));
	if (result.publish()) {
		const auto decoded = DecodePlaylistBytes(result.publicationBytes);
		CHECK(decoded && decoded.bytes == result.canonicalBytes);
	}
}
void BasicPublishAndMemo() {
	auto input = Input();
	input.libraryRead = PlaylistStepLocalRead::Missing;
	input.stateRead = PlaylistStepLocalRead::Missing;
	auto plan = PlanPlaylistStep(input);
	CheckState(plan);
	CHECK(!plan.writeLibrary && !plan.publish());
	CHECK(plan.publication == PlaylistStepPublication::InitialEmpty);
	input = Input(Visible());
	const auto original = input;
	plan = PlanPlaylistStep(input);
	CHECK(input == original);
	CHECK(plan.publish() && !plan.writeLibrary && plan.prune.prune.empty());
	CheckState(plan);
	Add(input, 1, input.library);
	plan = PlanPlaylistStep(input);
	CheckState(plan);
	CHECK(!plan.publish() && !plan.writeLibrary && plan.writeState);
	CHECK(plan.records.valid == 1 && plan.newlyMergedRecords == 1);
	CHECK(plan.state->memo.at(1) == Memo(1, input.library));
	input.state = *plan.state;
	input.payloads.clear();
	plan = PlanPlaylistStep(input);
	CheckState(plan);
	CHECK(plan.steadyState && plan.newlyMergedRecords == 0);
	CHECK(!plan.publish() && !plan.writeLibrary && !plan.writeState);
	CHECK(plan.inventory.readCandidates.empty());
	CHECK(plan.prune.status == PlaylistRecordPruneStatus::Skipped);
	// Bad cached bytes are irrelevant when no cover check or read is required.
	input.payloads.push_back(Proof(1, input.library));
	input.payloads[0].compressedBytes = "not gzip";
	CHECK(PlanPlaylistStep(input) == plan);
	input.library.playlists[IdA].title = PlaylistTitleRegister{ u"Edited"_q, 30 };
	plan = PlanPlaylistStep(input);
	CHECK(plan.publish() && !plan.writeLibrary && !plan.steadyState);
	CHECK(plan.newlyMergedRecords == 0 && plan.prune.prune.empty());
	input = Input();
	Add(input, 1, Visible());
	plan = PlanPlaylistStep(input);
	CHECK(plan.writeLibrary && !plan.publish());
	CHECK(plan.diff.hasContentChanges() && plan.state->memo.size() == 1);
	CheckState(plan);
	input.libraryRead = PlaylistStepLocalRead::Missing;
	input.state = *plan.state;
	input.payloads.clear();
	plan = PlanPlaylistStep(input);
	CHECK(plan.inventory.readCandidates.size() == 1 && plan.records.unreadable == 1);
	CHECK(plan.state->memo.empty());
}
void MissingLibraryCheckpoint() {
	auto input = Input();
	input.libraryRead = PlaylistStepLocalRead::Missing;
	Add(input, 1, {});
	const auto plan = PlanPlaylistStep(input);
	CheckState(plan);
	CHECK(plan.writeLibrary);
	CHECK(plan.state->memo.size() == 1 && !plan.publish());
}
void MergeAndAcknowledgment() {
	const auto a = Title(IdA, u"A"_q, 20);
	const auto b = Title(IdB, u"B"_q, 30);
	const auto joined = Join(a, b);
	auto input = Input();
	Add(input, 1, a);
	Add(input, 2, b);
	const auto before = PlanPlaylistStep(input);
	CheckState(before);
	CHECK(before.publish() && before.writeLibrary);
	CHECK(before.library == joined);
	CHECK(before.prune.prune.empty());
	input.payloads.push_back(Proof(3, joined));
	input.payloads.back().origin = PlaylistPayloadOrigin::PublishedThisStep;
	input.payloads.back().publicationAcknowledged = false;
	auto plan = PlanPlaylistStep(input);
	CHECK(plan == before);
	input.discovery.listedObservations.push_back(PlaylistInventoryObservation{
		.messageId = 3, .documentId = 1003,
		.ownership = SyncChatMessageOwnership::Own, .meta = Meta(),
	});
	plan = PlanPlaylistStep(input);
	CHECK(plan.records.unreadable == 1 && plan.prune.prune.empty());
	CHECK(plan.state->memo.size() == 2);
	input.payloads.back().publicationAcknowledged = true;
	plan = PlanPlaylistStep(input);
	CheckState(plan);
	CHECK(!plan.publish() && plan.prune.prune.size() == 2);
	for (const auto &entry : plan.prune.prune) {
		CHECK(entry.witness.messageId == 3);
		CHECK(entry.witnessCanonicalHash == Hash(joined));
	}
	// A caller may already have persisted the ACK's memo before asking for prune.
	input.state = *plan.state;
	input.library = *plan.library;
	plan = PlanPlaylistStep(input);
	CHECK(plan.prune.prune.size() == 2 && !plan.steadyState);
	input.payloads.back().publicationAcknowledged = false;
	plan = PlanPlaylistStep(input);
	CHECK(plan.prune.status == PlaylistRecordPruneStatus::Skipped);
	input = Input();
	Add(input, 1, a);
	Add(input, 2, joined);
	input.discovery.listingExhausted = false;
	plan = PlanPlaylistStep(input);
	CHECK(!plan.discoveryComplete && plan.prune.prune.size() == 1);
	CHECK(plan.prune.prune[0].witness.messageId == 2);
	CHECK(!plan.publish());
}
void CacheRequiresBoundMemo() {
	auto input = Input();
	Add(input, 1, Visible());
	input.payloads[0].origin = PlaylistPayloadOrigin::Cached;
	input.payloads[0].expectedCanonicalHash = Hash(Visible());
	const auto plan = PlanPlaylistStep(input);
	CHECK(plan.records.unreadable == 1);
	CHECK(!plan.writeLibrary && plan.state->memo.empty());
	CHECK(plan.prune.prune.empty());
	input = Input(Visible());
	Add(input, 1, input.library);
	Add(input, 2, input.library);
	input.state.memo[1] = Memo(1, Title(IdB, u"different"_q, 10));
	input.state.memo[2] = Memo(2, input.library);
	for (auto &proof : input.payloads) {
		proof.origin = PlaylistPayloadOrigin::Cached;
		proof.expectedCanonicalHash = Hash(input.library);
	}
	input.library.playlists[IdA].title->stamp++;
	const auto forged = PlanPlaylistStep(input);
	CHECK(forged.publish());
	CHECK(forged.prune.prune.empty());
}
void LocalGuardsAndModes() {
	CheckBlocked(PlanPlaylistStep({}));
	for (const auto missing : {false, true}) {
		auto input = Input(Visible());
		if (missing) input.libraryRead = PlaylistStepLocalRead::Failed;
		else input.stateRead = PlaylistStepLocalRead::Failed;
		CheckBlocked(PlanPlaylistStep(input));
	}
	for (const auto state : {false, true}) {
		for (const auto newer : {false, true}) {
			auto input = Input(Visible());
			if (state) input.state.format = newer ? 2 : 0;
			else input.library.format = newer ? 2 : 0;
			CheckBlocked(PlanPlaylistStep(input));
		}
	}
	auto input = Input(Visible());
	input.library.playlists[IdA].title->stamp = 0;
	CheckBlocked(PlanPlaylistStep(input));
	input = Input(Visible());
	input.state.chat = PeerToken{ PeerTokenKind::Chat, 1 };
	CheckBlocked(PlanPlaylistStep(input));
	input = Input();
	input.discovery.currentChat.reset();
	CheckBlocked(PlanPlaylistStep(input));
	input = Input();
	input.state.memo[1] = Memo(1, Visible());
	input.state.memo[1].canonicalHash = u"arbitrary"_q;
	CheckBlocked(PlanPlaylistStep(input));
	input = Input();
	input.discovery.selfUserId = SelfId + 1;
	CheckBlocked(PlanPlaylistStep(input));
	input = Input();
	input.selfUserId = 0;
	CheckBlocked(PlanPlaylistStep(input));
	input = Input();
	input.trigger = PlaylistStepTrigger(99);
	CheckBlocked(PlanPlaylistStep(input));
	input = Input(Visible());
	input.accountAuthorized = false;
	CheckBlocked(PlanPlaylistStep(input));
	input = Input(Visible());
	input.discovery.priorChat = Chat;
	input.discovery.priorMemo[1] = Memo(1, input.library);
	Add(input, 1, input.library);
	input.payloads.clear();
	const auto injected = PlanPlaylistStep(input);
	CHECK(injected.inventory.readCandidates.size() == 1);
	CHECK(injected.records.unreadable == 1 && injected.state->memo.empty());
	input = Input(Visible());
	input.state.sync = false;
	CHECK(PlanPlaylistStep(input).status == PlaylistStepStatus::Skipped);
	for (const auto trigger : {PlaylistStepTrigger::Manual, PlaylistStepTrigger::Logout}) {
		input.trigger = trigger;
		const auto plan = PlanPlaylistStep(input);
		CheckState(plan);
		CHECK(plan.publish() && !plan.state->sync);
	}
	input.trigger = PlaylistStepTrigger::LocalPurge;
	CHECK(!PlanPlaylistStep(input).publish());
	input.state.sync = true;
	CHECK(PlanPlaylistStep(input).status == PlaylistStepStatus::Skipped);
}
void RemoteFailuresAndBindings() {
	const auto a = Visible();
	for (const auto origin : {PlaylistPayloadOrigin::Unproven, PlaylistPayloadOrigin::Cached,
			PlaylistPayloadOrigin::PublishedThisStep, PlaylistPayloadOrigin::DownloadedThisStep}) {
		auto input = Input();
		Add(input, 1, a);
		input.payloads[0].origin = origin;
		input.payloads[0].readSucceeded = false;
		const auto plan = PlanPlaylistStep(input);
		CHECK(plan.records.unreadable == 1 && plan.state->memo.empty());
		CHECK(!plan.writeLibrary && plan.prune.prune.empty() && !plan.discoveryComplete);
	}
	for (auto failure = 0; failure != 5; ++failure) {
		auto input = Input();
		Add(input, 1, a);
		if (failure == 0) input.payloads[0].chat = PeerToken{ PeerTokenKind::Channel, 8 };
		if (failure == 1) ++input.payloads[0].documentId;
		if (failure == 2) input.payloads.clear();
		if (failure == 3) {
			input.payloads.push_back(input.payloads[0]);
			input.payloads.back().readSucceeded = false;
		}
		if (failure == 4) {
			input.payloads[0].origin = PlaylistPayloadOrigin::Cached;
			input.payloads[0].expectedCanonicalHash = QString(64, u'a');
		}
		const auto plan = PlanPlaylistStep(input);
		CHECK(plan.records.unreadable == 1 && plan.state->memo.empty());
		CHECK(plan.prune.prune.empty() && !plan.writeLibrary);
	}
	for (const auto ownership : {SyncChatMessageOwnership::Unknown, SyncChatMessageOwnership::Other}) {
		auto input = Input();
		Add(input, 1, a);
		input.discovery.listedObservations[0].ownership = ownership;
		const auto plan = PlanPlaylistStep(input);
		CHECK(!plan.writeLibrary && plan.state->memo.empty() && plan.prune.prune.empty());
		CHECK(plan.records.unreadable == (ownership == SyncChatMessageOwnership::Unknown ? 1 : 0));
	}
	for (const auto newer : {false, true}) {
		auto input = Input(a);
		Add(input, 1, {});
		input.payloads[0].compressedBytes = newer ? Zip("{\"format\":2}") : QByteArray("bad gzip");
		const auto plan = PlanPlaylistStep(input);
		CHECK(!plan.writeLibrary && plan.publish());
		CHECK(plan.state->memo.at(1).state == (newer
			? PlaylistLocalMemoState::NewerFormat : PlaylistLocalMemoState::Invalid));
		CHECK(plan.prune.prune.empty() && plan.records.unreadable == 0);
		CheckState(plan);
	}
	auto input = Input(a);
	input.state.memo[1] = Memo(1, a);
	input.state.unsyncedSince = 9;
	input.state.preserved.insert(u"old_chats"_q, QJsonObject{{u"opaque"_q, true}});
	Add(input, 1, a);
	input.discovery.listedObservations[0].documentId = 9001;
	input.payloads.clear();
	auto plan = PlanPlaylistStep(input);
	CHECK(plan.records.unreadable == 1 && plan.state->memo == input.state.memo);
	CHECK(plan.state->unsyncedSince == 9 && plan.state->preserved == input.state.preserved);
	input.discovery.listedObservations.clear();
	plan = PlanPlaylistStep(input);
	CHECK(plan.inventory.pendingByIdIds == std::vector<int32_t>{1});
	CHECK(plan.state->memo == input.state.memo && !plan.discoveryComplete);
	input.discovery.byIdSucceeded = true;
	input.discovery.byIdObservations.push_back(PlaylistByIdObservation{
		.messageId = 1, .outcome = PlaylistByIdOutcome::Present, .documentId = 1001,
		.ownership = SyncChatMessageOwnership::Own, .meta = Meta(),
	});
	plan = PlanPlaylistStep(input);
	CHECK(plan.steadyState && !plan.publish() && plan.prune.prune.empty());
	input.discovery.byIdObservations[0].outcome = PlaylistByIdOutcome::Gone;
	plan = PlanPlaylistStep(input);
	CHECK(plan.state->memo.empty() && plan.publish());
	CHECK(plan.inventory.vanishedMemoRecordCount == 1);
	input.discovery.currentChat = PeerToken{ PeerTokenKind::Channel, 44 };
	plan = PlanPlaylistStep(input);
	CHECK(plan.inventory.bindingChanged && plan.state->memo.empty());
	CHECK(plan.retiredMemo == input.state.memo && plan.publish());
	CHECK(plan.state->preserved == input.state.preserved);
	input.discovery.currentChat = PeerToken{ PeerTokenKind::User, SelfId };
	input.discovery.byIdObservations[0].outcome = PlaylistByIdOutcome::Present;
	plan = PlanPlaylistStep(input);
	CHECK(!plan.inventory.bindingChanged && plan.steadyState);
	input.discovery.listingSucceeded = false;
	plan = PlanPlaylistStep(input);
	CHECK(!plan.publish() && !plan.discoveryComplete && !plan.writeLibrary);
	CHECK(plan.state->memo == input.state.memo);
}
void NoticesAndPurges() {
	const auto stamp = uint64_t(1000);
	const auto due = stamp + kAutoPurgeRetentionMs;
	auto deleted = Visible();
	deleted.playlists[IdA].deleted = PlaylistDeletedRegister{ true, stamp };
	auto input = Input(deleted);
	Add(input, 1, deleted);
	input.clock.nowMs = due;
	auto plan = PlanPlaylistStep(input);
	CHECK(plan.purgedIds.empty());
	CHECK(plan.state->dueSeen.at(IdA).value == PlaylistAutoPurgeDueSeen{stamp, due});
	input.state = *plan.state;
	input.payloads.clear();
	input.clock.nowMs = due + kAutoPurgeMinimumObservationMs - 1;
	plan = PlanPlaylistStep(input);
	CHECK(plan.purgedIds.empty() && plan.state->dueSeen == input.state.dueSeen);
	input.clock.nowMs++;
	plan = PlanPlaylistStep(input);
	CheckState(plan);
	CHECK(plan.purgedIds == std::vector<QString>{IdA} && plan.publish());
	CHECK(plan.library->purged.contains(IdA) && plan.writeLibrary);
	CHECK(plan.diff.notices.empty() && plan.prune.prune.empty());
	for (auto failure = 0; failure != 7; ++failure) {
		auto blocked = input;
		if (failure == 0) blocked.discovery.listingExhausted = false;
		if (failure == 1) blocked.clock.requestSucceeded = false;
		if (failure == 2) blocked.clock.chat = PeerToken{PeerTokenKind::Channel, 77};
		if (failure == 3) blocked.clock.selfUserId++;
		if (failure == 4) blocked.clock.nowMs = due - 1;
		if (failure == 5) {
			blocked.discovery.listedObservations.clear();
			blocked.discovery.byIdSucceeded = true;
			blocked.discovery.byIdObservations.push_back(PlaylistByIdObservation{
				.messageId = 1, .outcome = PlaylistByIdOutcome::Gone,
				.documentId = 0, .ownership = SyncChatMessageOwnership::Unknown, .meta = {},
			});
		}
		if (failure == 6) {
			Add(blocked, 2, Visible(IdB));
			blocked.payloads.clear();
		}
		plan = PlanPlaylistStep(blocked);
		CHECK(plan.purgedIds.empty());
		if (failure != 4) CHECK(plan.state->dueSeen == input.state.dueSeen);
	}
	auto newer = input;
	Add(newer, 2, {});
	newer.payloads[0].compressedBytes = Zip("{\"format\":2}");
	plan = PlanPlaylistStep(newer);
	CHECK(plan.purgedIds.empty() && plan.state->dueSeen.empty());
	CHECK(plan.purgeStatus == PlaylistAutoPurgeStatus::BlockedNewerFormatRecord);
	auto restored = deleted;
	restored.playlists[IdA].deleted = PlaylistDeletedRegister{ false, stamp + 1 };
	Add(input, 2, restored);
	plan = PlanPlaylistStep(input);
	CHECK(plan.purgedIds.empty() && plan.state->dueSeen.empty());
	CHECK(!plan.library->playlists.at(IdA).deleted->value && plan.diff.notices.empty());
	input = Input(Visible());
	input.state.hidden[IdA].playlist = input.library.playlists.at(IdA);
	input.state.hidden[IdA].preserved.insert(u"future"_q, true);
	Add(input, 1, deleted);
	plan = PlanPlaylistStep(input);
	CHECK(plan.diff.notices.size() == 1);
	CHECK(plan.state->hidden.at(IdA).reason == PlaylistNoticeReason::DeletedElsewhere);
	CHECK(plan.state->hidden.at(IdA).playlist == input.library.playlists.at(IdA));
	CHECK(plan.state->hidden.at(IdA).preserved == input.state.hidden.at(IdA).preserved);
	PlaylistLibrary purged;
	purged.purged.insert(IdA);
	input = Input(restored);
	Add(input, 1, purged);
	plan = PlanPlaylistStep(input);
	CHECK(plan.diff.notices.size() == 1 && plan.library->purged.contains(IdA));
	CHECK(plan.state->hidden.at(IdA).reason == PlaylistNoticeReason::PurgedElsewhere);
	CHECK(plan.state->hidden.at(IdA).playlist == restored.playlists.at(IdA));
	CheckState(plan);
	input = Input(deleted);
	input.state.sync = false;
	input.trigger = PlaylistStepTrigger::LocalPurge;
	input.clock.nowMs = due;
	input.discovery = {};
	plan = PlanPlaylistStep(input);
	CHECK(plan.purgedIds.size() == 1 && !plan.publish() && plan.writeLibrary);
	input.clock.requestSucceeded = false;
	CHECK(PlanPlaylistStep(input).purgedIds.empty());
	input = Input(Visible());
	auto sameContent = input.library;
	sameContent.playlists[IdA].title->stamp++;
	Add(input, 1, sameContent);
	plan = PlanPlaylistStep(input);
	CHECK(plan.writeLibrary && !plan.diff.hasContentChanges());
	CHECK(plan.diff.hasStoredRegisterChanges() && plan.diff.notices.empty());
}
void LargeLocalPreserved() {
	auto library = Visible();
	auto &songs = library.playlists[IdA].songs.emplace();
	PlaylistSource source;
	source.peer = Chat;
	source.msg = 1;
	source.since = 10;
	source.meta.title = QString(96, u'a');
	source.meta.performer = QString(96, u'b');
	for (auto id = uint64_t(1); id <= 160000; ++id) {
		songs.emplace(id, PlaylistSongRegister{PlaylistSongValue{source}, 10});
	}
	CHECK(Canonical(library).size() > kPlaylistRecordMaximumUncompressedBytes);
	auto input = Input(library);
	Add(input, 1, Title(IdB, u"remote"_q, 20));
	const auto plan = PlanPlaylistStep(input);
	CHECK(plan.publication == PlaylistStepPublication::ExpandedLimit);
	CHECK(!plan.publish() && plan.publicationBytes.isEmpty());
	CHECK(plan.writeLibrary && plan.library->playlists.at(IdA).songs->size() == 160000);
	CHECK(plan.library->playlists.contains(IdB));
	CHECK(plan.canonicalBytes.size() > kPlaylistRecordMaximumUncompressedBytes);
}
void CompressedPublicationLimit() {
	auto library = Visible();
	auto &songs = library.playlists[IdA].songs.emplace();
	std::mt19937 random(7041);
	const auto alphabet = u"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"_q;
	const auto randomText = [&] {
		auto result = QString(96, u'a');
		for (auto &ch : result) ch = alphabet[random() % 64];
		return result;
	};
	PlaylistSource source;
	source.peer = Chat;
	source.msg = 1;
	source.since = 10;
	for (auto id = uint64_t(1); id <= 85000; ++id) {
		source.meta.title = randomText();
		source.meta.performer = randomText();
		songs.emplace(id, PlaylistSongRegister{PlaylistSongValue{source}, 10});
	}
	CHECK(Canonical(library).size() <= kPlaylistRecordMaximumUncompressedBytes);
	const auto plan = PlanPlaylistStep(Input(library));
	CHECK(plan.publication == PlaylistStepPublication::CompressedLimit);
	CHECK(!plan.publish() && plan.publicationBytes.isEmpty());
	CHECK(plan.library == library && !plan.writeLibrary);
}
PlaylistLibrary JoinCloud(const std::map<int32_t, PlaylistLibrary> &cloud) {
	PlaylistLibrary result;
	for (const auto &[id, library] : cloud) {
		CHECK(id > 0);
		result = Join(result, library);
	}
	return result;
}
void Interleavings() {
	for (auto seed = uint32_t(1); seed <= 70; ++seed) {
		std::mt19937 random(seed);
		std::array<PlaylistLibrary, 3> devices;
		std::array<PlaylistLocalState, 3> states;
		std::map<int32_t, PlaylistLibrary> cloud;
		auto allPublished = PlaylistLibrary();
		auto nextId = int32_t(1);
		const std::array<QString, 3> ids{IdA, IdB, IdC};
		const auto run = [&](std::size_t device, bool miss) {
			auto input = Input(devices[device]);
			input.state = states[device];
			for (const auto &[id, library] : cloud) {
				if (miss && random() % 4 == 0 && !input.state.memo.contains(id)) continue;
				Add(input, id, library);
				if (input.state.memo.contains(id)) {
					input.payloads.back().origin = PlaylistPayloadOrigin::Cached;
					input.payloads.back().expectedCanonicalHash = Hash(library);
				}
			}
			input.discovery.byIdSucceeded = true;
			for (const auto &[id, entry] : input.state.memo) {
				if (!cloud.contains(id)) input.discovery.byIdObservations.push_back(PlaylistByIdObservation{
					.messageId = id, .outcome = PlaylistByIdOutcome::Gone,
					.documentId = 0, .ownership = SyncChatMessageOwnership::Unknown, .meta = {},
				});
				CHECK(entry.documentId != 0);
			}
			const auto original = input;
			auto plan = PlanPlaylistStep(input);
			CheckState(plan);
			CHECK(input == original);
			std::shuffle(input.discovery.listedObservations.begin(), input.discovery.listedObservations.end(), random);
			std::shuffle(input.payloads.begin(), input.payloads.end(), random);
			CHECK(plan == PlanPlaylistStep(input));
			devices[device] = *plan.library;
			states[device] = *plan.state;
			if (plan.publish()) {
				const auto publishedId = nextId++;
				cloud[publishedId] = devices[device];
				allPublished = Join(allPublished, devices[device]);
				input.library = devices[device];
				input.state = states[device];
				Add(input, publishedId, devices[device]);
				input.payloads.back().origin = PlaylistPayloadOrigin::PublishedThisStep;
				input.payloads.back().publicationAcknowledged = true;
				plan = PlanPlaylistStep(input);
				CHECK(!plan.publish());
				states[device] = *plan.state;
			}
			std::set<int32_t> removed;
			for (const auto &entry : plan.prune.prune) removed.insert(entry.target.messageId);
			for (const auto &entry : plan.prune.prune) {
				CHECK(!removed.contains(entry.witness.messageId));
				CHECK(cloud.contains(entry.witness.messageId));
				CHECK(PlaylistLibraryContains(cloud.at(entry.witness.messageId), cloud.at(entry.target.messageId), SelfId).contains);
			}
			for (const auto id : removed) cloud.erase(id);
			CHECK(Canonical(JoinCloud(cloud)) == Canonical(allPublished));
		};
		for (auto turn = 1; turn <= 10; ++turn) {
			const auto device = random() % 3;
			const auto edit = Title(ids[random() % 3], QString::number(random() % 100), uint64_t(turn));
			devices[device] = Join(devices[device], edit);
			run(device, true);
		}
		for (auto quiet = 0; quiet != 2; ++quiet) {
			for (auto device = std::size_t(0); device != 3; ++device) run(device, false);
		}
		CHECK(cloud.size() == 1);
		for (const auto &library : devices) CHECK(Canonical(library) == Canonical(JoinCloud(cloud)));
	}
}
} // namespace

int main() {
	BasicPublishAndMemo();
	MissingLibraryCheckpoint();
	MergeAndAcknowledgment();
	CacheRequiresBoundMemo();
	LocalGuardsAndModes();
	RemoteFailuresAndBindings();
	NoticesAndPurges();
	LargeLocalPreserved();
	CompressedPublicationLimit();
	Interleavings();
	std::printf("PLAYLIST_STEP_CHECKS=%d FAILURES=%d\n", Checks, Failures);
	return Failures ? 1 : 0;
}
