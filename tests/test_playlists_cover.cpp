#include "purple/purple_playlists_cover.h"
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
const auto IdA = u"pl-aaaaaaaaaaaaaaab"_q;
const auto IdB = u"pl-aaaaaaaaaaaaaaac"_q;
const auto IdC = u"pl-aaaaaaaaaaaaaaad"_q;

PlaylistLibrary Title(QString id, QString title, uint64_t stamp) {
	PlaylistLibrary library;
	library.playlists[id].title = PlaylistTitleRegister{ std::move(title), stamp };
	return library;
}
QByteArray Canonical(const PlaylistLibrary &library) {
	const auto encoded = SerializePlaylistLibrary(library, SelfId);
	CHECK(encoded);
	return encoded.canonical;
}
QString Hash(const PlaylistLibrary &library) {
	return QString::fromLatin1(QCryptographicHash::hash(Canonical(library),
		QCryptographicHash::Sha256).toHex());
}
QByteArray Zip(const QByteArray &bytes) {
	const auto zipped = EncodePlaylistBytes(bytes, kPlaylistRecordCodecCaps);
	CHECK(zipped.status == PlaylistCodecStatus::Valid);
	return zipped.bytes;
}
PlaylistLibrary Join(const PlaylistLibrary &a, const PlaylistLibrary &b) {
	const auto joined = JoinPlaylistLibraries(a, b, SelfId);
	CHECK(joined);
	return joined.library;
}
PlaylistLibrary JoinAll(const std::map<int32_t, PlaylistLibrary> &libraries) {
	PlaylistLibrary result;
	for (const auto &[id, library] : libraries) {
		CHECK(id > 0);
		result = Join(result, library);
	}
	return result;
}
PlaylistRecordMessageMeta Meta() {
	PlaylistRecordMessageMeta result;
	result.isRegularMessage = true;
	result.isDocument = true;
	result.fileNames = { PlaylistRecordFileName() };
	result.caption = PlaylistRecordCaption();
	return result;
}
PlaylistRecordPruneInput Input(const std::map<int32_t, PlaylistLibrary> &libraries) {
	PlaylistRecordPruneInput result;
	result.selfUserId = SelfId;
	result.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
	result.presentSetChanged = true;
	PlaylistInventoryInput inventory;
	inventory.selfUserId = SelfId;
	inventory.currentChat = result.currentChat;
	inventory.listingSucceeded = true;
	inventory.listingExhausted = true;
	for (const auto &[id, library] : libraries) {
		const auto document = uint64_t(id) + 1000;
		inventory.listedObservations.push_back(PlaylistInventoryObservation{
			.messageId = id,
			.documentId = document,
			.ownership = SyncChatMessageOwnership::Own,
			.meta = Meta(),
		});
		PlaylistRecordPayloadProof proof;
		proof.chat = result.currentChat;
		proof.messageId = id;
		proof.documentId = document;
		proof.origin = PlaylistPayloadOrigin::DownloadedThisStep;
		proof.readSucceeded = true;
		proof.compressedBytes = Zip(Canonical(library));
		result.payloads.push_back(std::move(proof));
	}
	result.inventory = ReconcilePlaylistInventory(inventory);
	CHECK(result.inventory.status == PlaylistInventoryStatus::Valid);
	return result;
}
void CheckProofs(const PlaylistRecordPruneResult &plan,
		const std::map<int32_t, PlaylistLibrary> &all) {
	CHECK(plan.status == PlaylistRecordPruneStatus::Planned);
	std::map<int32_t, PlaylistLibrary> survivors;
	std::set<int32_t> targets;
	for (const auto &entry : plan.prune) {
		CHECK(targets.insert(entry.target.messageId).second);
	}
	for (const auto &entry : plan.survivors) {
		CHECK(all.contains(entry.messageId));
		CHECK(!targets.contains(entry.messageId));
		CHECK(entry.documentId == uint64_t(entry.messageId) + 1000);
		if (all.contains(entry.messageId)) {
			survivors.emplace(entry.messageId, all.at(entry.messageId));
		}
	}
	for (const auto &entry : plan.prune) {
		CHECK(all.contains(entry.target.messageId));
		CHECK(survivors.contains(entry.witness.messageId));
		CHECK(!targets.contains(entry.witness.messageId));
		CHECK(entry.target.documentId == uint64_t(entry.target.messageId) + 1000);
		CHECK(entry.witness.documentId == uint64_t(entry.witness.messageId) + 1000);
		if (all.contains(entry.target.messageId) && survivors.contains(entry.witness.messageId)) {
			const auto &outer = all.at(entry.witness.messageId);
			const auto &inner = all.at(entry.target.messageId);
			const auto contains = PlaylistLibraryContains(outer, inner, SelfId);
			CHECK(contains.valid() && contains.contains);
			CHECK(entry.witnessCanonicalHash == Hash(outer));
			CHECK(Canonical(outer) != Canonical(inner)
				|| entry.witness.messageId > entry.target.messageId);
		}
	}
	if (!all.empty()) {
		CHECK(!survivors.empty());
	}
	CHECK(plan.prune.size() + plan.survivors.size() == all.size());
	CHECK(Canonical(JoinAll(all)) == Canonical(JoinAll(survivors)));
}
void ExpectUnusable(PlaylistRecordPruneInput input, PlaylistRecordUnusableReason reason) {
	const auto copy = input;
	const auto plan = PlanPlaylistRecordPrunes(input);
	CHECK(input == copy);
	CHECK(plan.status == PlaylistRecordPruneStatus::Planned);
	CHECK(plan.prune.empty());
	CHECK(plan.survivors.empty());
	CHECK(plan.unusable.size() == 1);
	if (plan.unusable.size() == 1) {
		CHECK(plan.unusable.front().reason == reason);
	}
}
void BasicOrder() {
	const auto a = Title(IdA, u"A"_q, 1);
	const auto b = Title(IdB, u"B"_q, 2);
	const auto ab = Join(a, b);
	for (const auto &libraries : {
			std::map<int32_t, PlaylistLibrary>{},
			std::map<int32_t, PlaylistLibrary>{{1, a}},
			std::map<int32_t, PlaylistLibrary>{{1, a}, {2, a}, {3, a}},
			std::map<int32_t, PlaylistLibrary>{{1, a}, {2, ab}},
			std::map<int32_t, PlaylistLibrary>{{1, ab}, {2, a}},
			std::map<int32_t, PlaylistLibrary>{{1, a}, {2, b}},
			std::map<int32_t, PlaylistLibrary>{{1, a}, {2, b}, {3, ab}}}) {
		auto input = Input(libraries);
		const auto frozen = input;
		const auto plan = PlanPlaylistRecordPrunes(input);
		CheckProofs(plan, libraries);
		CHECK(input == frozen);
		std::reverse(input.inventory.presentCandidates.begin(), input.inventory.presentCandidates.end());
		std::reverse(input.payloads.begin(), input.payloads.end());
		CHECK(plan == PlanPlaylistRecordPrunes(input));
	}
	const auto equal = PlanPlaylistRecordPrunes(Input({{1, a}, {2, a}, {3, a}}));
	CHECK(equal.survivors == std::vector<PlaylistRecordIdentity>{{3, 1003}});
	const auto direct = PlanPlaylistRecordPrunes(Input({{1, a}, {2, ab}, {3, ab}}));
	CHECK(direct.prune.size() == 2);
	CHECK(direct.prune[0].witness.messageId == 3);
	CHECK(direct.prune[1].witness.messageId == 3);
	PlaylistLibrary purged;
	purged.purged.insert(IdA);
	CheckProofs(PlanPlaylistRecordPrunes(Input({{1, purged}, {2, a}})), {{1, purged}, {2, a}});
	PlaylistLibrary partial;
	partial.playlists[IdA].deleted = PlaylistDeletedRegister{ false, 8 };
	const auto full = Join(partial, a);
	CheckProofs(PlanPlaylistRecordPrunes(Input({{1, partial}, {2, a}, {3, full}})),
		{{1, partial}, {2, a}, {3, full}});
	const auto maxima = PlanPlaylistRecordPrunes(Input({{1, PlaylistLibrary{}}, {2, a}, {3, b}}));
	CHECK(maxima.prune.size() == 1 && maxima.prune[0].witness.messageId == 3);
}
void IdentityAndProvenance() {
	const auto a = Title(IdA, u"A"_q, 1);
	const auto one = Input({{1, a}});
	auto input = one;
	input.payloads.clear();
	ExpectUnusable(input, PlaylistRecordUnusableReason::MissingPayload);
	input = one;
	input.payloads[0].origin = PlaylistPayloadOrigin::Unproven;
	ExpectUnusable(input, PlaylistRecordUnusableReason::UnprovenPayload);
	input.payloads[0].origin = static_cast<PlaylistPayloadOrigin>(999);
	ExpectUnusable(input, PlaylistRecordUnusableReason::UnprovenPayload);
	input = one;
	input.payloads[0].readSucceeded = false;
	ExpectUnusable(input, PlaylistRecordUnusableReason::ReadFailed);
	for (auto variant = 0; variant != 4; ++variant) {
		input = one;
		if (variant == 0) input.payloads[0].chat.reset();
		if (variant == 1) input.payloads[0].chat = PeerToken{ PeerTokenKind::Channel, 9 };
		if (variant == 2) input.payloads[0].documentId += 1;
		if (variant == 3) input.payloads[0].chat = PeerToken{ PeerTokenKind::Self, 9 };
		ExpectUnusable(input, PlaylistRecordUnusableReason::IdentityMismatch);
	}
	input = one;
	input.payloads[0].messageId = 2;
	ExpectUnusable(input, PlaylistRecordUnusableReason::MissingPayload);
	input = one;
	input.payloads[0].origin = PlaylistPayloadOrigin::Cached;
	ExpectUnusable(input, PlaylistRecordUnusableReason::InvalidCacheHash);
	for (const auto &bad : {QString(63, u'a'), QString(64, u'A'), QString(64, u'g')}) {
		input.payloads[0].expectedCanonicalHash = bad;
		ExpectUnusable(input, PlaylistRecordUnusableReason::InvalidCacheHash);
	}
	input.payloads[0].expectedCanonicalHash = QString(64, u'a');
	ExpectUnusable(input, PlaylistRecordUnusableReason::CacheHashMismatch);
	input.payloads[0].expectedCanonicalHash = Hash(a);
	CheckProofs(PlanPlaylistRecordPrunes(input), {{1, a}});
	input.payloads[0].compressedBytes = Zip(Canonical(Title(IdB, u"B"_q, 2)));
	ExpectUnusable(input, PlaylistRecordUnusableReason::CacheHashMismatch);
	input = one;
	input.payloads[0].origin = PlaylistPayloadOrigin::PublishedThisStep;
	ExpectUnusable(input, PlaylistRecordUnusableReason::PublicationUnacknowledged);
	input = Input({{1, a}, {2, a}});
	input.presentSetChanged = false;
	input.publicationSucceeded = true;
	input.payloads[1].origin = PlaylistPayloadOrigin::PublishedThisStep;
	input.payloads[1].publicationAcknowledged = true;
	CheckProofs(PlanPlaylistRecordPrunes(input), {{1, a}, {2, a}});
	input.payloads[1].publicationAcknowledged = false;
	const auto refused = PlanPlaylistRecordPrunes(input);
	CHECK(refused.prune.empty());
	CHECK(refused.survivors.size() == 1 && refused.survivors.front().messageId == 1);
	input = one;
	input.currentChat = PeerToken{ PeerTokenKind::User, SelfId };
	input.payloads[0].chat = input.currentChat;
	input.payloads.push_back(one.payloads.front());
	CheckProofs(PlanPlaylistRecordPrunes(input), {{1, a}});
	input = one;
	input.currentChat = PeerToken{ PeerTokenKind::Channel, 17 };
	input.inventory.normalizedCurrentChat = input.currentChat;
	input.payloads[0].chat = input.currentChat;
	CheckProofs(PlanPlaylistRecordPrunes(input), {{1, a}});
}
void RejectionsAndScheduling() {
	const auto a = Title(IdA, u"A"_q, 1);
	const auto one = Input({{1, a}});
	auto input = one;
	input.presentSetChanged = false;
	input.inventory = {};
	input.currentChat.reset();
	input.payloads[0].compressedBytes = QByteArray("not gzip");
	const auto skipped = PlanPlaylistRecordPrunes(input);
	CHECK(skipped.status == PlaylistRecordPruneStatus::Skipped);
	CHECK(skipped.prune.empty() && skipped.survivors.empty() && skipped.unusable.empty());
	for (const auto &chat : {PeerToken{PeerTokenKind::Chat, 1}, PeerToken{PeerTokenKind::User, 99},
			PeerToken{PeerTokenKind::Self, 3}, PeerToken{PeerTokenKind::Channel, 0}}) {
		input = one;
		input.currentChat = chat;
		const auto result = PlanPlaylistRecordPrunes(input);
		CHECK(result.error == PlaylistRecordPruneError::InvalidChat && result.prune.empty());
	}
	input = one;
	input.selfUserId = std::numeric_limits<uint64_t>::max();
	CHECK(PlanPlaylistRecordPrunes(input).status == PlaylistRecordPruneStatus::InvalidInput);
	input = one;
	input.selfUserId = 0;
	CheckProofs(PlanPlaylistRecordPrunes(input), {{1, a}});
	input = one;
	input.inventory.status = PlaylistInventoryStatus::InvalidInput;
	CHECK(PlanPlaylistRecordPrunes(input).error == PlaylistRecordPruneError::InvalidInventory);
	input = one;
	input.inventory.normalizedCurrentChat = PeerToken{PeerTokenKind::Channel, 1};
	CHECK(PlanPlaylistRecordPrunes(input).error == PlaylistRecordPruneError::BindingMismatch);
	for (auto doc : {uint64_t(0), uint64_t(1001)}) {
		input = one;
		input.inventory.presentCandidates[0].documentId = doc;
		if (doc) input.inventory.presentCandidates[0].messageId = 0;
		CHECK(PlanPlaylistRecordPrunes(input).error == PlaylistRecordPruneError::InvalidIdentity);
	}
	input = one;
	input.inventory.presentCandidates.push_back(input.inventory.presentCandidates[0]);
	input.payloads.push_back(input.payloads[0]);
	CheckProofs(PlanPlaylistRecordPrunes(input), {{1, a}});
	input.inventory.presentCandidates[1].documentId++;
	CHECK(PlanPlaylistRecordPrunes(input).error == PlaylistRecordPruneError::ConflictingPresence);
	std::reverse(input.inventory.presentCandidates.begin(), input.inventory.presentCandidates.end());
	CHECK(PlanPlaylistRecordPrunes(input).error == PlaylistRecordPruneError::ConflictingPresence);
	input = one;
	input.payloads.push_back(input.payloads[0]);
	input.payloads[1].compressedBytes = Zip(Canonical(PlaylistLibrary{}));
	ExpectUnusable(input, PlaylistRecordUnusableReason::ConflictingPayload);
	std::reverse(input.payloads.begin(), input.payloads.end());
	ExpectUnusable(input, PlaylistRecordUnusableReason::ConflictingPayload);
	input = one;
	input.inventory.conflictingIds.push_back(1);
	ExpectUnusable(input, PlaylistRecordUnusableReason::ExcludedPresence);
	input = one;
	input.inventory.unknownOwnershipIds.push_back(1);
	ExpectUnusable(input, PlaylistRecordUnusableReason::ExcludedPresence);
	input = one;
	input.inventory.presentCandidates[0].meta.caption += u" #purplesync"_q;
	ExpectUnusable(input, PlaylistRecordUnusableReason::UnrecognizedRecord);
	input = one;
	input.inventory.presentCandidates[0].meta.forwarded = true;
	ExpectUnusable(input, PlaylistRecordUnusableReason::UnrecognizedRecord);
	input = one;
	input.payloads[0].compressedBytes = QByteArray("bad gzip");
	ExpectUnusable(input, PlaylistRecordUnusableReason::InvalidPayload);
	for (const auto &json : {QByteArray("{\"format\":1,\"playlists\":{},\"purged\":{},\"extra\":0}"),
			QByteArray("null"), QByteArray("[]")}) {
		input = one;
		input.payloads[0].compressedBytes = Zip(json);
		ExpectUnusable(input, PlaylistRecordUnusableReason::InvalidPayload);
	}
	input = one;
	input.payloads[0].compressedBytes = Zip(QByteArray("{\"format\":2}"));
	ExpectUnusable(input, PlaylistRecordUnusableReason::NewerFormat);
	input = one;
	input.payloads[0].compressedBytes = QByteArray(10 * 1024 * 1024 + 1, 'x');
	ExpectUnusable(input, PlaylistRecordUnusableReason::Oversized);
	input = one;
	input.inventory.presentCandidates.clear();
	const auto absent = PlanPlaylistRecordPrunes(input);
	CHECK(absent.prune.empty() && absent.survivors.empty());
	input = Input({{1, a}, {2, a}});
	input.inventory.metadataComplete = false;
	input.inventory.pendingByIdIds.push_back(3);
	input.payloads.push_back(input.payloads[0]);
	input.payloads.back().messageId = 3;
	CheckProofs(PlanPlaylistRecordPrunes(input), {{1, a}, {2, a}});
	for (const auto ownership : {SyncChatMessageOwnership::Unknown, SyncChatMessageOwnership::Other}) {
		PlaylistInventoryInput discovery;
		discovery.currentChat = PeerToken{PeerTokenKind::Self, 0};
		discovery.selfUserId = SelfId;
		discovery.listingSucceeded = true;
		discovery.listingExhausted = true;
		discovery.listedObservations.push_back(PlaylistInventoryObservation{
			.messageId = 1, .documentId = 1001, .ownership = ownership, .meta = Meta(),
		});
		input = one;
		input.inventory = ReconcilePlaylistInventory(discovery);
		const auto result = PlanPlaylistRecordPrunes(input);
		CHECK(result.prune.empty() && result.survivors.empty());
	}
}
void ThreeDeviceInterleavings() {
	for (auto seed = uint32_t(1); seed <= 220; ++seed) {
		std::mt19937 random(seed);
		std::array<PlaylistLibrary, 3> devices;
		std::map<int32_t, PlaylistLibrary> cloud;
		PlaylistLibrary everPublished;
		auto nextId = int32_t(1);
		const std::array<QString, 3> ids{IdA, IdB, IdC};
		for (auto turn = 0; turn != 12; ++turn) {
			const auto device = random() % devices.size();
			if (!cloud.empty() && random() % 2) {
				devices[device] = Join(devices[device], cloud.rbegin()->second);
			}
			const auto id = ids[random() % ids.size()];
			PlaylistLibrary edit;
			auto &playlist = edit.playlists[id];
			if (random() % 2) {
				playlist.title = PlaylistTitleRegister{
					QString::number(random() % 100), uint64_t(turn + 1)};
			} else {
				playlist.deleted = PlaylistDeletedRegister{
					bool(random() % 2), uint64_t(turn + 1)};
			}
			if (turn > 6 && random() % 7 == 0) {
				edit.playlists.clear();
				edit.purged.insert(id);
			}
			devices[device] = Join(devices[device], edit);
			cloud[nextId++] = devices[device];
			everPublished = Join(everPublished, devices[device]);
			std::map<int32_t, PlaylistLibrary> listed;
			for (const auto &[message, library] : cloud) {
				if (random() % 4 != 0) listed.emplace(message, library);
			}
			auto input = Input(listed);
			input.inventory.metadataComplete = false;
			for (auto &proof : input.payloads) {
				if (random() % 2) {
					proof.origin = PlaylistPayloadOrigin::Cached;
					proof.expectedCanonicalHash = Hash(listed.at(int32_t(proof.messageId)));
				}
			}
			const auto plan = PlanPlaylistRecordPrunes(input);
			CheckProofs(plan, listed);
			std::shuffle(input.payloads.begin(), input.payloads.end(), random);
			std::shuffle(input.inventory.presentCandidates.begin(), input.inventory.presentCandidates.end(), random);
			CHECK(plan == PlanPlaylistRecordPrunes(input));
			for (const auto &entry : plan.prune) cloud.erase(entry.target.messageId);
			CHECK(Canonical(JoinAll(cloud)) == Canonical(everPublished));
		}
		for (auto quiet = 0; quiet != 2; ++quiet) {
			for (auto &device : devices) {
				device = Join(device, JoinAll(cloud));
				cloud[nextId++] = device;
				everPublished = Join(everPublished, device);
				const auto plan = PlanPlaylistRecordPrunes(Input(cloud));
				CheckProofs(plan, cloud);
				for (const auto &entry : plan.prune) cloud.erase(entry.target.messageId);
				CHECK(Canonical(JoinAll(cloud)) == Canonical(everPublished));
			}
		}
		CHECK(cloud.size() == 1);
	}
}
} // namespace

int main() {
	BasicOrder();
	IdentityAndProvenance();
	RejectionsAndScheduling();
	ThreeDeviceInterleavings();
	std::printf("PLAYLIST_COVER_CHECKS=%d FAILURES=%d\n", Checks, Failures);
	return Failures ? 1 : 0;
}
