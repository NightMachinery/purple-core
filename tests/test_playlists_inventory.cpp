/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists_inventory.h"

#include "purple/purple_peer_token.h"
#include "purple/purple_playlists.h"
#include "purple/purple_playlists_codec.h"
#include "purple/purple_playlists_local.h"
#include "purple/purple_playlists_sync.h"
#include "purple/purple_sync_chat.h"
#include "purple/purple_types.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include <zlib.h>

namespace {

using namespace Purple;

int Checks = 0;
int Failures = 0;

void Check(bool ok, const char *expr, int line) {
	++Checks;
	if (!ok) {
		++Failures;
		std::printf("FAIL line %d: %s\n", line, expr);
	}
}

#define CHECK(cond) Check(bool(cond), #cond, __LINE__)

constexpr uint64_t kSelfId = 7001ULL;

[[nodiscard]] PlaylistRecordMessageMeta ValidMeta(
		QString caption = PlaylistRecordCaption(),
		QString mimeType = PlaylistRecordMimeType()) {
	return PlaylistRecordMessageMeta{
		.isRegularMessage = true,
		.forwarded = false,
		.isDocument = true,
		.caption = std::move(caption),
		.mimeType = std::move(mimeType),
		.fileNames = { PlaylistRecordFileName() },
	};
}

[[nodiscard]] PlaylistRecordMessageMeta ForwardedMeta() {
	auto meta = ValidMeta();
	meta.forwarded = true;
	return meta;
}

[[nodiscard]] PlaylistRecordMessageMeta SettingsMeta() {
	return PlaylistRecordMessageMeta{
		.isRegularMessage = true,
		.forwarded = false,
		.isDocument = true,
		.caption = u"Settings #purplesync"_q,
		.mimeType = u"application/json"_q,
		.fileNames = { u"Purple settings sync.json"_q },
	};
}

[[nodiscard]] PlaylistRecordMessageMeta AmbiguousSettingsMeta() {
	return ValidMeta(u"Mixed #purplesync #purpleplaylists"_q);
}

[[nodiscard]] QString HexHash(QChar ch) {
	return QString(64, ch);
}

[[nodiscard]] PlaylistLocalMemoEntry MakeValidMemo(
		uint64_t docId,
		QChar hexDigit = u'a',
		int classifier = kPlaylistRecordClassifierVersion) {
	return PlaylistLocalMemoEntry{
		.documentId = docId,
		.classifier = classifier,
		.state = PlaylistLocalMemoState::Valid,
		.canonicalHash = HexHash(hexDigit),
	};
}

[[nodiscard]] PlaylistLocalMemoEntry MakeNonValidMemo(
		uint64_t docId,
		PlaylistLocalMemoState state,
		int classifier = kPlaylistRecordClassifierVersion) {
	return PlaylistLocalMemoEntry{
		.documentId = docId,
		.classifier = classifier,
		.state = state,
		.canonicalHash = std::nullopt,
	};
}

[[nodiscard]] QByteArray DeflateGzip(const QByteArray &plain) {
	z_stream strm{};
	if (deflateInit2(
			&strm,
			6,
			Z_DEFLATED,
			16 + MAX_WBITS,
			8,
			Z_DEFAULT_STRATEGY) != Z_OK) {
		return {};
	}
	static constexpr Bytef kEmpty = 0;
	QByteArray out;
	std::vector<Bytef> chunk(32768);
	qsizetype offset = 0;
	while (true) {
		if (strm.avail_in == 0 && offset < plain.size()) {
			const auto n = std::min<qsizetype>(plain.size() - offset, 1 << 20);
			strm.next_in = const_cast<Bytef *>(
				reinterpret_cast<const Bytef *>(plain.constData() + offset));
			strm.avail_in = static_cast<uInt>(n);
			offset += n;
		} else if (strm.avail_in == 0) {
			strm.next_in = const_cast<Bytef *>(&kEmpty);
		}
		const auto flush = (offset >= plain.size()) ? Z_FINISH : Z_NO_FLUSH;
		strm.next_out = chunk.data();
		strm.avail_out = static_cast<uInt>(chunk.size());
		const auto rc = deflate(&strm, flush);
		if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
			deflateEnd(&strm);
			return {};
		}
		const auto have = static_cast<qsizetype>(chunk.size() - strm.avail_out);
		if (have > 0) {
			out.append(reinterpret_cast<const char *>(chunk.data()), have);
		}
		if (rc == Z_STREAM_END) {
			deflateEnd(&strm);
			return out;
		}
	}
}

void CheckEmptyActions(const PlaylistInventoryResult &result) {
	CHECK(result.presentCandidates.empty());
	CHECK(result.readCandidates.empty());
	CHECK(result.reusableMemo.empty());
	CHECK(result.invalidatedMemo.empty());
	CHECK(result.confirmedGoneIds.empty());
	CHECK(result.vanishedMemoRecordCount == 0);
	CHECK(result.pendingByIdIds.empty());
	CHECK(result.unresolvedPriorMemo.empty());
	CHECK(result.unknownOwnershipIds.empty());
	CHECK(result.conflictingIds.empty());
}

void CheckInvalidResult(
		const PlaylistInventoryResult &result,
		PlaylistInventoryError expectedError) {
	CHECK(!bool(result));
	CHECK(result.status == PlaylistInventoryStatus::InvalidInput);
	CHECK(result.error == expectedError);
	CHECK(!result.bindingChanged);
	CHECK(!result.normalizedCurrentChat.has_value());
	CHECK(!result.normalizedPriorChat.has_value());
	CHECK(!result.metadataComplete);
	CheckEmptyActions(result);
}

[[nodiscard]] std::vector<int32_t> CollectPresentIds(
		const PlaylistInventoryResult &r) {
	std::vector<int32_t> ids;
	for (const auto &c : r.presentCandidates) {
		ids.push_back(c.messageId);
	}
	return ids;
}

[[nodiscard]] std::vector<int32_t> CollectReadIds(
		const PlaylistInventoryResult &r) {
	std::vector<int32_t> ids;
	for (const auto &c : r.readCandidates) {
		ids.push_back(c.messageId);
	}
	return ids;
}

[[nodiscard]] std::vector<int32_t> CollectReusableIds(
		const PlaylistInventoryResult &r) {
	std::vector<int32_t> ids;
	for (const auto &e : r.reusableMemo) {
		ids.push_back(e.messageId);
	}
	return ids;
}

[[nodiscard]] std::vector<int32_t> CollectInvalidatedIds(
		const PlaylistInventoryResult &r) {
	std::vector<int32_t> ids;
	for (const auto &i : r.invalidatedMemo) {
		ids.push_back(i.messageId);
	}
	return ids;
}

void TestFailClosedDefaultsAndValidation() {
	const PlaylistInventoryObservation defaultObs;
	CHECK(defaultObs.messageId == 0);
	CHECK(defaultObs.documentId == 0);
	CHECK(defaultObs.ownership == SyncChatMessageOwnership::Unknown);
	CHECK(!IsPlaylistRecordCandidate(defaultObs.meta, defaultObs.ownership));

	const PlaylistByIdObservation defaultById;
	CHECK(defaultById.messageId == 0);
	CHECK(defaultById.outcome == PlaylistByIdOutcome::Failed);
	CHECK(defaultById.documentId == 0);
	CHECK(defaultById.ownership == SyncChatMessageOwnership::Unknown);

	const PlaylistInventoryInput defaultInput;
	CHECK(defaultInput.selfUserId == 0);
	CHECK(!defaultInput.currentChat.has_value());
	CHECK(!defaultInput.priorChat.has_value());
	CHECK(defaultInput.priorMemo.empty());
	CHECK(!defaultInput.listingSucceeded);
	CHECK(!defaultInput.listingExhausted);
	CHECK(!defaultInput.byIdSucceeded);

	const PlaylistInventoryResult defaultResult;
	CheckInvalidResult(defaultResult, PlaylistInventoryError::None);
	CheckInvalidResult(
		ReconcilePlaylistInventory(defaultInput),
		PlaylistInventoryError::InvalidCurrentChat);

	// Out-of-range selfUserId with valid currentChat.
	{
		PlaylistInventoryInput in;
		in.selfUserId = std::numeric_limits<uint64_t>::max();
		in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.listingSucceeded = true;
		in.listingExhausted = true;
		CheckInvalidResult(
			ReconcilePlaylistInventory(in),
			PlaylistInventoryError::InvalidSelfUserId);
	}

	{
		PlaylistInventoryInput input;
		input.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
		input.listingSucceeded = true;
		input.listingExhausted = true;
		const auto result = ReconcilePlaylistInventory(input);
		CHECK(bool(result));
		CHECK(result.metadataComplete);
	}

	// Invalid currentChat tokens.
	for (const auto &badChat : {
			PeerToken{ PeerTokenKind::Chat, 12 },
			PeerToken{ PeerTokenKind::User, kSelfId + 1 },
			PeerToken{ PeerTokenKind::Channel, 0 },
			PeerToken{ PeerTokenKind::Self, 1 },
		}) {
		PlaylistInventoryInput in;
		in.selfUserId = kSelfId;
		in.currentChat = badChat;
		in.listingSucceeded = true;
		CheckInvalidResult(
			ReconcilePlaylistInventory(in),
			PlaylistInventoryError::InvalidCurrentChat);
	}

	// Invalid priorChat tokens.
	for (const auto &badPrior : {
			PeerToken{ PeerTokenKind::Chat, 99 },
			PeerToken{ PeerTokenKind::User, kSelfId + 5 },
			PeerToken{ PeerTokenKind::Channel, 0 },
			PeerToken{ PeerTokenKind::Self, 2 },
		}) {
		PlaylistInventoryInput in;
		in.selfUserId = kSelfId;
		in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorChat = badPrior;
		in.listingSucceeded = true;
		CheckInvalidResult(
			ReconcilePlaylistInventory(in),
			PlaylistInventoryError::InvalidPriorChat);
	}

	// Non-empty priorMemo without priorChat fails closed with MissingPriorChat.
	{
		PlaylistInventoryInput in;
		in.selfUserId = kSelfId;
		in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorMemo.emplace(10, MakeValidMemo(100ULL));
		in.listingSucceeded = true;
		CheckInvalidResult(
			ReconcilePlaylistInventory(in),
			PlaylistInventoryError::MissingPriorChat);
	}

	// Invalid priorMemo entries fail closed with InvalidMemo (including msgId/docId/classifier/hash).
	{
		const auto checkBadMemo = [&](int32_t msgId, PlaylistLocalMemoEntry entry) {
			PlaylistInventoryInput in;
			in.selfUserId = kSelfId;
			in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
			in.priorChat = PeerToken{ PeerTokenKind::Self, 0 };
			in.priorMemo.emplace(msgId, std::move(entry));
			in.listingSucceeded = true;
			CheckInvalidResult(
				ReconcilePlaylistInventory(in),
				PlaylistInventoryError::InvalidMemo);
		};
		checkBadMemo(0, MakeValidMemo(100ULL));
		checkBadMemo(-7, MakeValidMemo(100ULL));
		checkBadMemo(10, MakeValidMemo(0ULL));
		checkBadMemo(10, MakeValidMemo(100ULL, u'a', 0));
		checkBadMemo(10, MakeValidMemo(100ULL, u'Z', 1));
		{
			auto badValid = MakeValidMemo(100ULL);
			badValid.canonicalHash.reset();
			checkBadMemo(10, badValid);
		}
		{
			auto badNonValid = MakeNonValidMemo(100ULL, PlaylistLocalMemoState::Invalid);
			badNonValid.canonicalHash = HexHash(u'b');
			checkBadMemo(10, badNonValid);
		}
	}

	// Invalid messageId in listedObservations and byIdObservations.
	const auto overMaxId = int64_t(std::numeric_limits<int32_t>::max()) + 1;
	for (const int64_t badId : { int64_t(0), int64_t(-1), overMaxId }) {
		{
			PlaylistInventoryInput in;
			in.selfUserId = kSelfId;
			in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
			in.listingSucceeded = true;
			in.listedObservations.push_back(PlaylistInventoryObservation{
				.messageId = badId,
				.documentId = 0,
				.ownership = SyncChatMessageOwnership::Other,
				.meta = {},
			});
			CheckInvalidResult(
				ReconcilePlaylistInventory(in),
				PlaylistInventoryError::InvalidMessageId);
		}
		{
			PlaylistInventoryInput in;
			in.selfUserId = kSelfId;
			in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
			in.priorChat = PeerToken{ PeerTokenKind::Self, 0 };
			in.priorMemo.emplace(10, MakeValidMemo(100ULL));
			in.listingSucceeded = true;
			in.byIdSucceeded = true;
			in.byIdObservations.push_back(PlaylistByIdObservation{
				.messageId = badId,
				.outcome = PlaylistByIdOutcome::Gone,
			});
			CheckInvalidResult(
				ReconcilePlaylistInventory(in),
				PlaylistInventoryError::InvalidMessageId);
		}
	}

	// Invalid documentId == 0 for Candidate and UnknownOwnership, in both listed and requested by-id.
	for (const auto ownership : {
			SyncChatMessageOwnership::Own,
			SyncChatMessageOwnership::Unknown,
		}) {
		{
			PlaylistInventoryInput in;
			in.selfUserId = kSelfId;
			in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
			in.listingSucceeded = true;
			in.listedObservations.push_back(PlaylistInventoryObservation{
				.messageId = 20,
				.documentId = 0,
				.ownership = ownership,
				.meta = ValidMeta(),
			});
			CheckInvalidResult(
				ReconcilePlaylistInventory(in),
				PlaylistInventoryError::InvalidDocumentId);
		}
		{
			PlaylistInventoryInput in;
			in.selfUserId = kSelfId;
			in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
			in.priorChat = PeerToken{ PeerTokenKind::Self, 0 };
			in.priorMemo.emplace(20, MakeValidMemo(200ULL));
			in.listingSucceeded = true;
			in.byIdSucceeded = true;
			in.byIdObservations.push_back(PlaylistByIdObservation{
				.messageId = 20,
				.outcome = PlaylistByIdOutcome::Present,
				.documentId = 0,
				.ownership = ownership,
				.meta = ValidMeta(),
			});
			CheckInvalidResult(
				ReconcilePlaylistInventory(in),
				PlaylistInventoryError::InvalidDocumentId);
		}
	}

	// Ignored / AmbiguousSettings / non-Present byId allow documentId == 0.
	{
		PlaylistInventoryInput in;
		in.selfUserId = kSelfId;
		in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorMemo.emplace(30, MakeValidMemo(300ULL));
		in.listingSucceeded = true;
		in.listingExhausted = true;
		in.byIdSucceeded = true;
		in.listedObservations = {
			{ 1, 0, SyncChatMessageOwnership::Other, ValidMeta() },
			{ 2, 0, SyncChatMessageOwnership::Own, AmbiguousSettingsMeta() },
			{ 3, 0, SyncChatMessageOwnership::Own, SettingsMeta() },
			{ 4, 0, SyncChatMessageOwnership::Own, ForwardedMeta() },
		};
		in.byIdObservations = {
			{ 30, PlaylistByIdOutcome::Gone, 0, SyncChatMessageOwnership::Unknown, {} },
		};
		const auto res = ReconcilePlaylistInventory(in);
		CHECK(bool(res));
		CHECK(res.metadataComplete);
		CHECK(res.confirmedGoneIds == (std::vector<int32_t>{ 30 }));
	}

	// Invalid PlaylistByIdOutcome enum value.
	{
		PlaylistInventoryInput in;
		in.selfUserId = kSelfId;
		in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorMemo.emplace(10, MakeValidMemo(100ULL));
		in.listingSucceeded = true;
		in.byIdSucceeded = true;
		in.byIdObservations.push_back(PlaylistByIdObservation{
			.messageId = 10,
			.outcome = static_cast<PlaylistByIdOutcome>(99),
		});
		CheckInvalidResult(
			ReconcilePlaylistInventory(in),
			PlaylistInventoryError::InvalidByIdOutcome);
	}
}

void TestChatBindingNormalizationAndChange() {
	// Empty priorChat with empty priorMemo is first-time binding: valid, bindingChanged == false.
	{
		PlaylistInventoryInput in;
		in.selfUserId = kSelfId;
		in.currentChat = PeerToken{ PeerTokenKind::User, kSelfId };
		in.listingSucceeded = true;
		in.listingExhausted = true;
		in.listedObservations = {
			{ 5, 500ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		};
		const auto res = ReconcilePlaylistInventory(in);
		CHECK(bool(res));
		CHECK(!res.bindingChanged);
		CHECK(res.normalizedCurrentChat == (PeerToken{ PeerTokenKind::Self, 0 }));
		CHECK(!res.normalizedPriorChat.has_value());
		CHECK(res.metadataComplete);
		CHECK(CollectPresentIds(res) == (std::vector<int32_t>{ 5 }));
		CHECK(CollectReadIds(res) == (std::vector<int32_t>{ 5 }));
	}

	// user:<selfUserId> <-> Self in both directions keeps binding identical and reuses memo.
	for (const auto &[cur, pri] : {
			std::pair{ PeerToken{ PeerTokenKind::User, kSelfId }, PeerToken{ PeerTokenKind::Self, 0 } },
			std::pair{ PeerToken{ PeerTokenKind::Self, 0 }, PeerToken{ PeerTokenKind::User, kSelfId } },
		}) {
		PlaylistInventoryInput in;
		in.selfUserId = kSelfId;
		in.currentChat = cur;
		in.priorChat = pri;
		in.priorMemo.emplace(10, MakeValidMemo(100ULL));
		in.listingSucceeded = true;
		in.listingExhausted = true;
		in.listedObservations = {
			{ 10, 100ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		};
		const auto res = ReconcilePlaylistInventory(in);
		CHECK(bool(res));
		CHECK(!res.bindingChanged);
		CHECK(res.normalizedCurrentChat == (PeerToken{ PeerTokenKind::Self, 0 }));
		CHECK(res.normalizedPriorChat == (PeerToken{ PeerTokenKind::Self, 0 }));
		CHECK(res.metadataComplete);
		CHECK(CollectReusableIds(res) == (std::vector<int32_t>{ 10 }));
		CHECK(res.readCandidates.empty());
	}

	// Same Channel token keeps binding identical.
	{
		PlaylistInventoryInput in;
		in.selfUserId = kSelfId;
		in.currentChat = PeerToken{ PeerTokenKind::Channel, 888ULL };
		in.priorChat = PeerToken{ PeerTokenKind::Channel, 888ULL };
		in.priorMemo.emplace(10, MakeValidMemo(100ULL));
		in.listingSucceeded = true;
		in.listingExhausted = true;
		in.listedObservations = {
			{ 10, 100ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		};
		const auto res = ReconcilePlaylistInventory(in);
		CHECK(bool(res));
		CHECK(!res.bindingChanged);
		CHECK(res.normalizedCurrentChat == (PeerToken{ PeerTokenKind::Channel, 888ULL }));
		CHECK(res.normalizedPriorChat == (PeerToken{ PeerTokenKind::Channel, 888ULL }));
		CHECK(CollectReusableIds(res) == (std::vector<int32_t>{ 10 }));
	}

	// Changed binding: old memo is NEVER reused, invalidated, or queried by-id in the new chat.
	{
		PlaylistInventoryInput in;
		in.selfUserId = kSelfId;
		in.currentChat = PeerToken{ PeerTokenKind::Channel, 900ULL };
		in.priorChat = PeerToken{ PeerTokenKind::User, kSelfId };
		in.priorMemo.emplace(10, MakeValidMemo(100ULL));
		in.priorMemo.emplace(20, MakeValidMemo(200ULL));
		in.listingSucceeded = true;
		in.listingExhausted = true;
		in.listedObservations = {
			{ 10, 100ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		};
		// Even stale/invalid byId rows are ignored when bindingChanged is true.
		in.byIdSucceeded = true;
		in.byIdObservations = {
			{ .messageId = 20, .outcome = PlaylistByIdOutcome::Gone },
		};
		const auto res = ReconcilePlaylistInventory(in);
		CHECK(bool(res));
		CHECK(res.bindingChanged);
		CHECK(res.normalizedCurrentChat == (PeerToken{ PeerTokenKind::Channel, 900ULL }));
		CHECK(res.normalizedPriorChat == (PeerToken{ PeerTokenKind::Self, 0 }));
		CHECK(res.metadataComplete);
		CHECK(CollectPresentIds(res) == (std::vector<int32_t>{ 10 }));
		CHECK(CollectReadIds(res) == (std::vector<int32_t>{ 10 }));
		CHECK(res.reusableMemo.empty());
		CHECK(res.invalidatedMemo.empty());
		CHECK(res.confirmedGoneIds.empty());
		CHECK(res.vanishedMemoRecordCount == 0);
		CHECK(res.pendingByIdIds.empty());
		CHECK(res.unresolvedPriorMemo.empty());
	}
}

void TestReuseInvalidationAndEligibilityTransitions() {
	PlaylistInventoryInput in;
	in.selfUserId = kSelfId;
	in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
	in.priorChat = PeerToken{ PeerTokenKind::Self, 0 };
	in.listingSucceeded = true;
	in.listingExhausted = true;

	// 10: Valid, same doc, classifier == 1 -> Reusable
	in.priorMemo.emplace(10, MakeValidMemo(1010ULL, u'a', kPlaylistRecordClassifierVersion));
	// 11: Valid, same doc, classifier != 1 (e.g. 2) -> STILL Reusable (Valid hash never needs re-read)
	in.priorMemo.emplace(11, MakeValidMemo(1011ULL, u'b', kPlaylistRecordClassifierVersion + 1));
	// 12: Non-valid (Invalid/NewerFormat/Oversized), same doc, classifier == 1 -> Reusable
	in.priorMemo.emplace(12, MakeNonValidMemo(1012ULL, PlaylistLocalMemoState::Invalid, kPlaylistRecordClassifierVersion));
	in.priorMemo.emplace(13, MakeNonValidMemo(1013ULL, PlaylistLocalMemoState::NewerFormat, kPlaylistRecordClassifierVersion));
	in.priorMemo.emplace(14, MakeNonValidMemo(1014ULL, PlaylistLocalMemoState::Oversized, kPlaylistRecordClassifierVersion));
	// 15..17: Non-valid, same doc, classifier != 1 -> ClassifierUpgradeNeeded + read
	in.priorMemo.emplace(15, MakeNonValidMemo(1015ULL, PlaylistLocalMemoState::Invalid, kPlaylistRecordClassifierVersion + 1));
	in.priorMemo.emplace(16, MakeNonValidMemo(1016ULL, PlaylistLocalMemoState::NewerFormat, kPlaylistRecordClassifierVersion + 2));
	in.priorMemo.emplace(17, MakeNonValidMemo(1017ULL, PlaylistLocalMemoState::Oversized, kPlaylistRecordClassifierVersion + 3));
	// 18: Valid with replaced documentId -> DocumentReplaced + read
	in.priorMemo.emplace(18, MakeValidMemo(1018ULL, u'c', kPlaylistRecordClassifierVersion));
	// 19: Invalid with replaced documentId -> DocumentReplaced (not ClassifierUpgradeNeeded) + read
	in.priorMemo.emplace(19, MakeNonValidMemo(1019ULL, PlaylistLocalMemoState::Invalid, kPlaylistRecordClassifierVersion + 1));
	// 20: Was memoed, now Ignored (forwarded / other / pure settings) -> LostEligibility (not read/present)
	in.priorMemo.emplace(20, MakeValidMemo(1020ULL, u'd'));
	in.priorMemo.emplace(21, MakeValidMemo(1021ULL, u'e'));
	in.priorMemo.emplace(22, MakeValidMemo(1022ULL, u'f'));
	// 23: Was memoed, now AmbiguousSettings -> LostEligibility (not read/present)
	in.priorMemo.emplace(23, MakeValidMemo(1023ULL, u'0'));
	// 24: Was memoed, now Unknown ownership -> unknownOwnershipIds + unresolvedPriorMemo
	in.priorMemo.emplace(24, MakeValidMemo(1024ULL, u'1'));

	// Observed in scrambled order; plus fresh 25 (Candidate), 26 (Unknown), 27 (Ignored), 28 (Ambiguous).
	in.listedObservations = {
		{ 25, 2025ULL, SyncChatMessageOwnership::Own, ValidMeta(u"Custom caption"_q, u"application/octet-stream"_q) },
		{ 26, 2026ULL, SyncChatMessageOwnership::Unknown, ValidMeta() },
		{ 27, 2027ULL, SyncChatMessageOwnership::Other, ValidMeta() },
		{ 28, 2028ULL, SyncChatMessageOwnership::Own, AmbiguousSettingsMeta() },
		{ 24, 1024ULL, SyncChatMessageOwnership::Unknown, ValidMeta() },
		{ 23, 1023ULL, SyncChatMessageOwnership::Own, AmbiguousSettingsMeta() },
		{ 22, 1022ULL, SyncChatMessageOwnership::Own, SettingsMeta() },
		{ 21, 1021ULL, SyncChatMessageOwnership::Other, ValidMeta() },
		{ 20, 1020ULL, SyncChatMessageOwnership::Own, ForwardedMeta() },
		{ 19, 9019ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		{ 18, 9018ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		{ 17, 1017ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		{ 16, 1016ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		{ 15, 1015ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		{ 14, 1014ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		{ 13, 1013ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		{ 12, 1012ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		{ 11, 1011ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		{ 10, 1010ULL, SyncChatMessageOwnership::Own, ValidMeta() },
	};

	const auto inputCopy = in;
	const auto res = ReconcilePlaylistInventory(in);
	CHECK(in == inputCopy);
	CHECK(bool(res));
	// Incomplete metadata because 24 & 26 have UnknownOwnership.
	CHECK(!res.metadataComplete);

	CHECK(CollectPresentIds(res) == (std::vector<int32_t>{ 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 25 }));
	CHECK(CollectReadIds(res) == (std::vector<int32_t>{ 15, 16, 17, 18, 19, 25 }));
	CHECK(CollectReusableIds(res) == (std::vector<int32_t>{ 10, 11, 12, 13, 14 }));
	CHECK(CollectInvalidatedIds(res) == (std::vector<int32_t>{ 15, 16, 17, 18, 19, 20, 21, 22, 23 }));

	CHECK(res.invalidatedMemo[0].reason == PlaylistInventoryInvalidationReason::ClassifierUpgradeNeeded);
	CHECK(res.invalidatedMemo[1].reason == PlaylistInventoryInvalidationReason::ClassifierUpgradeNeeded);
	CHECK(res.invalidatedMemo[2].reason == PlaylistInventoryInvalidationReason::ClassifierUpgradeNeeded);
	CHECK(res.invalidatedMemo[3].reason == PlaylistInventoryInvalidationReason::DocumentReplaced);
	CHECK(res.invalidatedMemo[3].priorDocumentId == 1018ULL);
	CHECK(res.invalidatedMemo[4].reason == PlaylistInventoryInvalidationReason::DocumentReplaced);
	CHECK(res.invalidatedMemo[4].priorDocumentId == 1019ULL);
	for (std::size_t idx = 5; idx <= 8; ++idx) {
		CHECK(res.invalidatedMemo[idx].reason == PlaylistInventoryInvalidationReason::LostEligibility);
	}

	CHECK(res.unknownOwnershipIds == (std::vector<int32_t>{ 24, 26 }));
	CHECK(res.unresolvedPriorMemo.size() == 1);
	CHECK(res.unresolvedPriorMemo.at(24) == MakeValidMemo(1024ULL, u'1'));
	CHECK(res.confirmedGoneIds.empty());
	CHECK(res.vanishedMemoRecordCount == 0);
	CHECK(res.pendingByIdIds.empty());
	CHECK(res.conflictingIds.empty());
}

void TestByIdProbesGoneTransientAndListingFailures() {
	// 1) Failed listing ignores stale listed/byId rows and preserves prior memo unresolved.
	{
		PlaylistInventoryInput in;
		in.selfUserId = kSelfId;
		in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorMemo.emplace(10, MakeValidMemo(100ULL));
		in.priorMemo.emplace(20, MakeValidMemo(200ULL));
		in.listingSucceeded = false;
		in.listingExhausted = true;
		in.listedObservations = {
			{ 10, 100ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		};
		in.byIdSucceeded = true;
		in.byIdObservations = {
			{ .messageId = 20, .outcome = PlaylistByIdOutcome::Gone },
		};
		const auto res = ReconcilePlaylistInventory(in);
		CHECK(bool(res));
		CHECK(!res.metadataComplete);
		CHECK(res.presentCandidates.empty());
		CHECK(res.readCandidates.empty());
		CHECK(res.reusableMemo.empty());
		CHECK(res.invalidatedMemo.empty());
		CHECK(res.confirmedGoneIds.empty());
		CHECK(res.vanishedMemoRecordCount == 0);
		CHECK(res.pendingByIdIds.empty());
		CHECK(res.unresolvedPriorMemo == in.priorMemo);
	}

	// 2) Succeeded listing with failed byId puts all missed memo IDs into pendingByIdIds + unresolvedPriorMemo.
	{
		PlaylistInventoryInput in;
		in.selfUserId = kSelfId;
		in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorMemo.emplace(10, MakeValidMemo(100ULL));
		in.priorMemo.emplace(20, MakeValidMemo(200ULL));
		in.priorMemo.emplace(30, MakeValidMemo(300ULL));
		in.listingSucceeded = true;
		in.listingExhausted = true;
		in.listedObservations = {
			{ 10, 100ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		};
		in.byIdSucceeded = false;
		in.byIdObservations = {
			{ .messageId = 20, .outcome = PlaylistByIdOutcome::Gone },
			{ 30, PlaylistByIdOutcome::Present, 300ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		};
		const auto res = ReconcilePlaylistInventory(in);
		CHECK(bool(res));
		CHECK(!res.metadataComplete);
		CHECK(CollectReusableIds(res) == (std::vector<int32_t>{ 10 }));
		CHECK(res.pendingByIdIds == (std::vector<int32_t>{ 20, 30 }));
		CHECK(res.confirmedGoneIds.empty());
		CHECK(res.vanishedMemoRecordCount == 0);
		CHECK(res.unresolvedPriorMemo.size() == 2);
		CHECK(res.unresolvedPriorMemo.contains(20));
		CHECK(res.unresolvedPriorMemo.contains(30));
	}

	// 3) Succeeded listing + succeeded byId with Present, Gone, Failed, Inaccessible, Cancelled,
	// omitted missed ID, and out-of-scope rows.
	{
		PlaylistInventoryInput in;
		in.selfUserId = kSelfId;
		in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorMemo.emplace(10, MakeValidMemo(100ULL));
		in.priorMemo.emplace(20, MakeValidMemo(200ULL));
		in.priorMemo.emplace(21, MakeValidMemo(210ULL));
		in.priorMemo.emplace(22, MakeValidMemo(220ULL));
		in.priorMemo.emplace(23, MakeValidMemo(230ULL));
		in.priorMemo.emplace(24, MakeValidMemo(240ULL));
		in.priorMemo.emplace(25, MakeValidMemo(250ULL));
		in.priorMemo.emplace(26, MakeValidMemo(260ULL));
		in.listingSucceeded = true;
		in.listingExhausted = true;
		in.listedObservations = {
			{ 10, 100ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		};
		in.byIdSucceeded = true;
		in.byIdObservations = {
			// 20: Present same doc -> reusable with fromById = true
			{ 20, PlaylistByIdOutcome::Present, 200ULL, SyncChatMessageOwnership::Own, ValidMeta() },
			// 21: Present replaced doc -> invalidated DocumentReplaced + read with fromById = true
			{ 21, PlaylistByIdOutcome::Present, 211ULL, SyncChatMessageOwnership::Own, ValidMeta() },
			// 22: Explicitly Gone -> confirmedGoneIds
			{ .messageId = 22, .outcome = PlaylistByIdOutcome::Gone },
			// 23..25: Transient failure states -> pending + preserved unresolved
			{ .messageId = 23, .outcome = PlaylistByIdOutcome::Failed },
			{ .messageId = 24, .outcome = PlaylistByIdOutcome::Inaccessible },
			{ .messageId = 25, .outcome = PlaylistByIdOutcome::Cancelled },
			// 26: Intentionally omitted from byIdObservations -> pending + preserved unresolved
			// 10: Already listed (out of scope for byId) -> stale Gone ignored
			{ .messageId = 10, .outcome = PlaylistByIdOutcome::Gone },
			// 999: Not in priorMemo (out of scope for byId) -> ignored even if Present/Gone
			{ 999, PlaylistByIdOutcome::Present, 0, SyncChatMessageOwnership::Own, ValidMeta() },
			{ .messageId = 998, .outcome = PlaylistByIdOutcome::Gone },
		};
		const auto res = ReconcilePlaylistInventory(in);
		CHECK(bool(res));
		CHECK(!res.metadataComplete);
		CHECK(CollectPresentIds(res) == (std::vector<int32_t>{ 10, 20, 21 }));
		CHECK(!res.presentCandidates[0].fromById);
		CHECK(res.presentCandidates[1].fromById);
		CHECK(res.presentCandidates[2].fromById);
		CHECK(CollectReusableIds(res) == (std::vector<int32_t>{ 10, 20 }));
		CHECK(!res.reusableMemo[0].fromById);
		CHECK(res.reusableMemo[1].fromById);
		CHECK(CollectInvalidatedIds(res) == (std::vector<int32_t>{ 21 }));
		CHECK(res.invalidatedMemo[0].fromById);
		CHECK(CollectReadIds(res) == (std::vector<int32_t>{ 21 }));
		CHECK(res.readCandidates[0].fromById);
		CHECK(res.confirmedGoneIds == (std::vector<int32_t>{ 22 }));
		CHECK(res.vanishedMemoRecordCount == 1);
		CHECK(res.pendingByIdIds == (std::vector<int32_t>{ 23, 24, 25, 26 }));
		CHECK(res.unresolvedPriorMemo.size() == 4);
		CHECK(res.conflictingIds.empty());
	}

	// 4) Clean listing + explicit Gone -> metadataComplete == true; partial listing preserves
	// positive observations + Gone while keeping metadataComplete == false.
	{
		PlaylistInventoryInput full;
		full.selfUserId = kSelfId;
		full.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
		full.priorChat = PeerToken{ PeerTokenKind::Self, 0 };
		full.priorMemo.emplace(10, MakeValidMemo(100ULL));
		full.priorMemo.emplace(20, MakeValidMemo(200ULL));
		full.listingSucceeded = true;
		full.listingExhausted = true;
		full.listedObservations = {
			{ 10, 100ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		};
		full.byIdSucceeded = true;
		full.byIdObservations = {
			{ .messageId = 20, .outcome = PlaylistByIdOutcome::Gone },
		};
		const auto fullRes = ReconcilePlaylistInventory(full);
		CHECK(bool(fullRes));
		CHECK(fullRes.metadataComplete);
		CHECK(fullRes.confirmedGoneIds == (std::vector<int32_t>{ 20 }));
		CHECK(fullRes.vanishedMemoRecordCount == 1);

		auto partial = full;
		partial.listingExhausted = false;
		partial.listedObservations.push_back(
			{ 30, 300ULL, SyncChatMessageOwnership::Own, ValidMeta() });
		const auto partialRes = ReconcilePlaylistInventory(partial);
		CHECK(bool(partialRes));
		CHECK(!partialRes.metadataComplete);
		CHECK(CollectReusableIds(partialRes) == (std::vector<int32_t>{ 10 }));
		CHECK(CollectReadIds(partialRes) == (std::vector<int32_t>{ 30 }));
		CHECK(partialRes.confirmedGoneIds == (std::vector<int32_t>{ 20 }));
	}
}

void TestDuplicatesAndConflicts() {
	// Identical duplicates deduplicate cleanly in both listed and by-id.
	{
		PlaylistInventoryInput in;
		in.selfUserId = kSelfId;
		in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorMemo.emplace(10, MakeValidMemo(100ULL));
		in.priorMemo.emplace(20, MakeValidMemo(200ULL));
		in.listingSucceeded = true;
		in.listingExhausted = true;
		in.listedObservations = {
			{ 10, 100ULL, SyncChatMessageOwnership::Own, ValidMeta() },
			{ 10, 100ULL, SyncChatMessageOwnership::Own, ValidMeta() },
			{ 10, 100ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		};
		in.byIdSucceeded = true;
		in.byIdObservations = {
			{ 20, PlaylistByIdOutcome::Gone, 0, SyncChatMessageOwnership::Unknown, {} },
			{ 20, PlaylistByIdOutcome::Gone, 999ULL, SyncChatMessageOwnership::Other, ValidMeta() },
		};
		const auto res = ReconcilePlaylistInventory(in);
		CHECK(bool(res));
		CHECK(res.metadataComplete);
		CHECK(CollectReusableIds(res) == (std::vector<int32_t>{ 10 }));
		CHECK(res.confirmedGoneIds == (std::vector<int32_t>{ 20 }));
		CHECK(res.conflictingIds.empty());
	}

	// Conflicting listed observations (candidate vs non-candidate, and candidate vs candidate)
	// work identically in both orders and do not emit by-id queries or invalidations.
	const PlaylistInventoryObservation candA{
		10, 100ULL, SyncChatMessageOwnership::Own, ValidMeta()
	};
	const PlaylistInventoryObservation candB{
		10, 101ULL, SyncChatMessageOwnership::Own, ValidMeta()
	};
	const PlaylistInventoryObservation ignoredA{
		10, 0, SyncChatMessageOwnership::Other, ForwardedMeta()
	};

	for (const auto &[first, second] : {
			std::pair{ candA, candB },
			std::pair{ candB, candA },
			std::pair{ candA, ignoredA },
			std::pair{ ignoredA, candA },
		}) {
		PlaylistInventoryInput in;
		in.selfUserId = kSelfId;
		in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorMemo.emplace(10, MakeValidMemo(100ULL));
		in.listingSucceeded = true;
		in.listingExhausted = true;
		in.listedObservations = { first, second, first };
		// Stale byId for conflicted listed ID 10 must be ignored.
		in.byIdSucceeded = true;
		in.byIdObservations = { { .messageId = 10, .outcome = PlaylistByIdOutcome::Gone } };

		const auto res = ReconcilePlaylistInventory(in);
		CHECK(bool(res));
		CHECK(!res.metadataComplete);
		CHECK(res.conflictingIds == (std::vector<int32_t>{ 10 }));
		CHECK(res.presentCandidates.empty());
		CHECK(res.readCandidates.empty());
		CHECK(res.reusableMemo.empty());
		CHECK(res.invalidatedMemo.empty());
		CHECK(res.confirmedGoneIds.empty());
		CHECK(res.pendingByIdIds.empty());
		CHECK(res.unresolvedPriorMemo.size() == 1);
		CHECK(res.unresolvedPriorMemo.at(10) == MakeValidMemo(100ULL));
	}

	// Conflicting by-id observations (Gone vs Present, Failed vs Cancelled, Present vs Present).
	const PlaylistByIdObservation byGone{ .messageId = 20, .outcome = PlaylistByIdOutcome::Gone };
	const PlaylistByIdObservation byPresent{
		20, PlaylistByIdOutcome::Present, 200ULL, SyncChatMessageOwnership::Own, ValidMeta()
	};
	const PlaylistByIdObservation byPresentDiff{
		20, PlaylistByIdOutcome::Present, 201ULL, SyncChatMessageOwnership::Own, ValidMeta()
	};
	const PlaylistByIdObservation byFailed{ .messageId = 20, .outcome = PlaylistByIdOutcome::Failed };
	const PlaylistByIdObservation byCancelled{ .messageId = 20, .outcome = PlaylistByIdOutcome::Cancelled };

	for (const auto &[first, second] : {
			std::pair{ byGone, byPresent },
			std::pair{ byPresent, byGone },
			std::pair{ byPresent, byPresentDiff },
			std::pair{ byFailed, byCancelled },
		}) {
		PlaylistInventoryInput in;
		in.selfUserId = kSelfId;
		in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorChat = PeerToken{ PeerTokenKind::Self, 0 };
		in.priorMemo.emplace(20, MakeValidMemo(200ULL));
		in.listingSucceeded = true;
		in.listingExhausted = true;
		in.byIdSucceeded = true;
		in.byIdObservations = { first, second, first };

		const auto res = ReconcilePlaylistInventory(in);
		CHECK(bool(res));
		CHECK(!res.metadataComplete);
		CHECK(res.conflictingIds == (std::vector<int32_t>{ 20 }));
		CHECK(res.confirmedGoneIds.empty());
		CHECK(res.vanishedMemoRecordCount == 0);
		CHECK(res.presentCandidates.empty());
		CHECK(res.reusableMemo.empty());
		CHECK(res.invalidatedMemo.empty());
		CHECK(res.pendingByIdIds.empty());
		CHECK(res.unresolvedPriorMemo.size() == 1);
		CHECK(res.unresolvedPriorMemo.at(20) == MakeValidMemo(200ULL));
	}
}

void TestRecordsRoundTripAndLargeStateNoArtificialCaps() {
	const auto validGz = DeflateGzip(
		R"({"format":1,"purged":{},"playlists":{}})");
	CHECK(!validGz.isEmpty());
	const auto cValid = ClassifyPlaylistRecord(PlaylistRecordInput{
		.messageId = 101,
		.documentId = 700101ULL,
		.ownership = SyncChatMessageOwnership::Own,
		.meta = ValidMeta(),
		.readOutcome = PlaylistRecordReadOutcome::Succeeded,
		.compressedBytes = validGz,
		.selfUserId = kSelfId,
	});
	const auto cInvalid = ClassifyPlaylistRecord(PlaylistRecordInput{
		.messageId = 102,
		.documentId = 700102ULL,
		.ownership = SyncChatMessageOwnership::Own,
		.meta = ValidMeta(),
		.readOutcome = PlaylistRecordReadOutcome::Succeeded,
		.compressedBytes = QByteArray("not-gzip"),
		.selfUserId = kSelfId,
	});
	CHECK(cValid.memoEntry.has_value());
	CHECK(cInvalid.memoEntry.has_value());
	CHECK(cValid.status == PlaylistRecordStatus::Valid);
	CHECK(cInvalid.status == PlaylistRecordStatus::Invalid);
	if (!cValid.memoEntry || !cInvalid.memoEntry) {
		return;
	}

	PlaylistLocalState localState = DefaultPlaylistLocalState();
	localState.chat = PeerToken{ PeerTokenKind::User, kSelfId };
	localState.memo.emplace(101, *cValid.memoEntry);
	localState.memo.emplace(102, *cInvalid.memoEntry);
	// Exceed 4 MiB without triggering any 4 MiB legacy settings cap.
	localState.preserved.insert(u"large_blob"_q, QString(5 * 1024 * 1024, u'Z'));

	const auto validation = ValidatePlaylistLocalState(localState, kSelfId);
	CHECK(bool(validation));
	const auto serialized = SerializePlaylistLocalState(localState, kSelfId);
	CHECK(bool(serialized));
	CHECK(serialized.json.size() > 5 * 1024 * 1024);
	const auto reparsed = ParsePlaylistLocalState(serialized.json, kSelfId);
	CHECK(bool(reparsed));
	CHECK(reparsed.state.memo == localState.memo);

	PlaylistInventoryInput in;
	in.selfUserId = kSelfId;
	in.currentChat = PeerToken{ PeerTokenKind::Self, 0 };
	in.priorChat = reparsed.state.chat;
	in.priorMemo = reparsed.state.memo;
	in.listingSucceeded = true;
	in.listingExhausted = true;
	in.listedObservations = {
		{ 101, 700101ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		{ 102, 700102ULL, SyncChatMessageOwnership::Own, ValidMeta() },
		{ 103, 700103ULL, SyncChatMessageOwnership::Own, ValidMeta() },
	};
	const auto res = ReconcilePlaylistInventory(in);
	CHECK(bool(res));
	CHECK(!res.bindingChanged);
	CHECK(res.metadataComplete);
	CHECK(CollectReusableIds(res) == (std::vector<int32_t>{ 101, 102 }));
	CHECK(CollectReadIds(res) == (std::vector<int32_t>{ 103 }));
}

void CheckSeededResultInvariants(
		const PlaylistInventoryInput &in,
		const PlaylistInventoryResult &res) {
	CHECK(bool(res));
	const auto isSortedUnique = [](const std::vector<int32_t> &ids) {
		for (std::size_t i = 1; i < ids.size(); ++i) {
			if (ids[i - 1] >= ids[i]) {
				return false;
			}
		}
		return true;
	};
	const auto presentIds = CollectPresentIds(res);
	const auto readIds = CollectReadIds(res);
	const auto reusableIds = CollectReusableIds(res);
	const auto invalidatedIds = CollectInvalidatedIds(res);

	CHECK(isSortedUnique(presentIds));
	CHECK(isSortedUnique(readIds));
	CHECK(isSortedUnique(reusableIds));
	CHECK(isSortedUnique(invalidatedIds));
	CHECK(isSortedUnique(res.confirmedGoneIds));
	CHECK(isSortedUnique(res.pendingByIdIds));
	CHECK(isSortedUnique(res.unknownOwnershipIds));
	CHECK(isSortedUnique(res.conflictingIds));
	CHECK(res.vanishedMemoRecordCount == res.confirmedGoneIds.size());

	const std::set<int32_t> presentSet(presentIds.begin(), presentIds.end());
	const std::set<int32_t> readSet(readIds.begin(), readIds.end());
	const std::set<int32_t> reusableSet(reusableIds.begin(), reusableIds.end());
	const std::set<int32_t> unknownSet(
		res.unknownOwnershipIds.begin(),
		res.unknownOwnershipIds.end());
	const std::set<int32_t> conflictSet(
		res.conflictingIds.begin(),
		res.conflictingIds.end());
	const std::set<int32_t> goneSet(
		res.confirmedGoneIds.begin(),
		res.confirmedGoneIds.end());
	const std::set<int32_t> pendingSet(
		res.pendingByIdIds.begin(),
		res.pendingByIdIds.end());

	for (const auto id : readSet) {
		CHECK(presentSet.contains(id));
		CHECK(!reusableSet.contains(id));
	}
	for (const auto id : reusableSet) {
		CHECK(presentSet.contains(id));
		CHECK(!readSet.contains(id));
	}
	for (const auto &inv : res.invalidatedMemo) {
		if (inv.reason == PlaylistInventoryInvalidationReason::LostEligibility) {
			CHECK(!presentSet.contains(inv.messageId));
			CHECK(!readSet.contains(inv.messageId));
		} else {
			CHECK(presentSet.contains(inv.messageId));
			CHECK(readSet.contains(inv.messageId));
		}
	}

	const bool expectedComplete = in.listingSucceeded
		&& in.listingExhausted
		&& res.pendingByIdIds.empty()
		&& res.unknownOwnershipIds.empty()
		&& res.conflictingIds.empty();
	CHECK(res.metadataComplete == expectedComplete);

	if (!res.bindingChanged && in.listingSucceeded) {
		const std::set<int32_t> invSet(invalidatedIds.begin(), invalidatedIds.end());
		for (const auto &[id, entry] : in.priorMemo) {
			const int buckets = int(reusableSet.contains(id))
				+ int(invSet.contains(id))
				+ int(goneSet.contains(id))
				+ int(res.unresolvedPriorMemo.contains(id));
			CHECK(buckets == 1);
			if (res.unresolvedPriorMemo.contains(id)) {
				CHECK(res.unresolvedPriorMemo.at(id) == entry);
			}
		}
		for (const auto &[id, entry] : res.unresolvedPriorMemo) {
			CHECK(in.priorMemo.contains(id));
			CHECK(in.priorMemo.at(id) == entry);
			CHECK(!in.byIdSucceeded
				|| unknownSet.contains(id)
				|| conflictSet.contains(id)
				|| pendingSet.contains(id));
		}
		for (const auto id : goneSet) {
			CHECK(in.priorMemo.contains(id));
		}
		for (const auto id : pendingSet) {
			CHECK(res.unresolvedPriorMemo.contains(id));
		}
	}
}

void TestSeededPermutationAndPropertyFuzz() {
	std::mt19937_64 rng(0x5EED12349876ULL);
	constexpr int kSeedCount = 260;

	for (int seed = 0; seed < kSeedCount; ++seed) {
		PlaylistInventoryInput in;
		in.selfUserId = kSelfId;
		const bool sameChat = (rng() % 5) != 0;
		in.currentChat = (rng() % 2 == 0)
			? PeerToken{ PeerTokenKind::User, kSelfId }
			: PeerToken{ PeerTokenKind::Self, 0 };
		in.priorChat = sameChat
			? PeerToken{ PeerTokenKind::Self, 0 }
			: PeerToken{ PeerTokenKind::Channel, 4040ULL };
		in.listingSucceeded = (rng() % 10) != 0;
		in.listingExhausted = (rng() % 3) != 0;
		in.byIdSucceeded = (rng() % 4) != 0;

		const int maxId = 24;
		for (int32_t id = 1; id <= maxId; ++id) {
			if ((rng() % 2) == 0) {
				const auto statePick = rng() % 4;
				const int classifier = kPlaylistRecordClassifierVersion + int(rng() % 2);
				const uint64_t doc = 5000ULL + uint64_t(id * 10);
				if (statePick == 0) {
					in.priorMemo.emplace(id, MakeValidMemo(doc, u'e', classifier));
				} else {
					const auto st = (statePick == 1)
						? PlaylistLocalMemoState::Invalid
						: (statePick == 2)
							? PlaylistLocalMemoState::NewerFormat
							: PlaylistLocalMemoState::Oversized;
					in.priorMemo.emplace(id, MakeNonValidMemo(doc, st, classifier));
				}
			}

			const auto makeObs = [&](int32_t msgId) {
				const auto kind = rng() % 8;
				const uint64_t doc = (kind == 0 || kind == 1)
					? 5000ULL + uint64_t(msgId * 10)
					: (kind == 2)
						? 5000ULL + uint64_t(msgId * 10 + 1)
						: (kind == 3)
							? 9000ULL + uint64_t(msgId)
							: 0ULL;
				switch (kind) {
				case 0:
				case 1:
				case 2:
					return PlaylistInventoryObservation{
						msgId, doc, SyncChatMessageOwnership::Own, ValidMeta()
					};
				case 3:
					return PlaylistInventoryObservation{
						msgId, doc, SyncChatMessageOwnership::Unknown, ValidMeta()
					};
				case 4:
					return PlaylistInventoryObservation{
						msgId, 0, SyncChatMessageOwnership::Other, ValidMeta()
					};
				case 5:
					return PlaylistInventoryObservation{
						msgId, 0, SyncChatMessageOwnership::Own, AmbiguousSettingsMeta()
					};
				default:
					return PlaylistInventoryObservation{
						msgId, 0, SyncChatMessageOwnership::Own, SettingsMeta()
					};
				}
			};

			const auto copies = int(rng() % 3);
			if (copies >= 1) {
				const auto first = makeObs(id);
				in.listedObservations.push_back(first);
				if (copies == 2) {
					in.listedObservations.push_back((rng() % 3 == 0) ? makeObs(id) : first);
				}
			}

			if ((rng() % 3) != 0) {
				const auto makeById = [&](int32_t msgId) {
					switch (rng() % 6) {
					case 0:
						return PlaylistByIdObservation{
							msgId,
							PlaylistByIdOutcome::Present,
							5000ULL + uint64_t(msgId * 10 + (rng() % 2)),
							SyncChatMessageOwnership::Own,
							ValidMeta(),
						};
					case 1:
						return PlaylistByIdObservation{
							msgId,
							PlaylistByIdOutcome::Present,
							7777ULL + uint64_t(msgId),
							SyncChatMessageOwnership::Unknown,
							ValidMeta(),
						};
					case 2:
						return PlaylistByIdObservation{ .messageId = msgId, .outcome = PlaylistByIdOutcome::Gone };
					case 3:
						return PlaylistByIdObservation{ .messageId = msgId, .outcome = PlaylistByIdOutcome::Failed };
					case 4:
						return PlaylistByIdObservation{ .messageId = msgId, .outcome = PlaylistByIdOutcome::Inaccessible };
					default:
						return PlaylistByIdObservation{ .messageId = msgId, .outcome = PlaylistByIdOutcome::Cancelled };
					}
				};
				const auto bFirst = makeById(id);
				in.byIdObservations.push_back(bFirst);
				if ((rng() % 4) == 0) {
					in.byIdObservations.push_back((rng() % 2 == 0) ? makeById(id) : bFirst);
				}
			}
		}

		const auto base = ReconcilePlaylistInventory(in);
		CheckSeededResultInvariants(in, base);

		auto shuffled = in;
		std::shuffle(
			shuffled.listedObservations.begin(),
			shuffled.listedObservations.end(),
			rng);
		std::shuffle(
			shuffled.byIdObservations.begin(),
			shuffled.byIdObservations.end(),
			rng);
		const auto permuted = ReconcilePlaylistInventory(shuffled);
		CHECK(permuted == base);
	}
}

} // namespace

int main() {
	TestFailClosedDefaultsAndValidation();
	TestChatBindingNormalizationAndChange();
	TestReuseInvalidationAndEligibilityTransitions();
	TestByIdProbesGoneTransientAndListingFailures();
	TestDuplicatesAndConflicts();
	TestRecordsRoundTripAndLargeStateNoArtificialCaps();
	TestSeededPermutationAndPropertyFuzz();

	if (Failures > 0) {
		std::printf("FAILED %d/%d checks\n", Failures, Checks);
		return 1;
	}
	std::printf("PASSED %d checks\n", Checks);
	return 0;
}
