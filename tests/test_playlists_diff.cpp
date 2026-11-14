/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists.h"
#include "purple/purple_playlists_diff.h"
#include "purple/purple_playlists_ops.h"
#include "purple/purple_types.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <vector>

using namespace Purple;

namespace {

auto Checks = 0;
auto Failures = 0;

void Check(bool good, const char *condition, int line) {
	++Checks;
	if (!good) {
		++Failures;
		std::printf("FAIL line %d: %s\n", line, condition);
	}
}

#define CHECK(cond) Check(bool(cond), #cond, __LINE__)

QByteArray MakeEntropy(uint8_t seed) {
	return QByteArray(kPlaylistIdEntropyBytes, char(seed));
}

PlaylistAddSongInput MakeSongInput(
		uint64_t docId,
		PeerToken peer,
		int32_t msg,
		const QString &title = u"Track"_q,
		const QString &performer = u"Artist"_q,
		int32_t duration = 180,
		const QString &username = QString()) {
	return {
		docId,
		peer,
		msg,
		username,
		PlaylistSongMeta{ title, performer, duration },
	};
}

void TestIdentityNormalizedNoopAndStampOnly() {
	constexpr auto selfId = uint64_t(4242);
	const auto chatPeer = PeerToken{ PeerTokenKind::Chat, 11 };
	const auto userSelf = PeerToken{ PeerTokenKind::User, selfId };
	const auto fallbackPeer = PeerToken{ PeerTokenKind::Channel, 99 };

	const auto created = CreatePlaylist(
		PlaylistLibrary{},
		u"Driving"_q,
		MakeEntropy(0x01),
		100,
		selfId);
	CHECK(created);
	const auto added = AddPlaylistSongs(
		created.library,
		created.playlistId,
		{
			MakeSongInput(10, userSelf, 5, u"S1"_q, u"A1"_q, 210),
			MakeSongInput(20, chatPeer, 7, u"S2"_q, u"A2"_q, 180, u"ch_name"_q),
		},
		110,
		selfId);
	CHECK(added);

	auto withFallback = added.library;
	withFallback.playlists.at(created.playlistId).songs->at(10).value.source->alt = {
		PlaylistSourceRef{ userSelf, 6, QString() },
		PlaylistSourceRef{ fallbackPeer, 8, u"ch_alt"_q },
	};

	const auto sameDiff = DiffPlaylistLibraries(withFallback, withFallback, selfId);
	CHECK(sameDiff);
	CHECK(sameDiff.side == PlaylistDiffSide::None);
	CHECK(!sameDiff.hasContentChanges());
	CHECK(!sameDiff.hasStoredRegisterChanges());
	CHECK(sameDiff.changes.empty());
	CHECK(sameDiff.registerChanges.empty());
	CHECK(sameDiff.notices.empty());
	CHECK(sameDiff.summary == PlaylistDiffSummary{});

	auto rawSelfRoundtrip = withFallback;
	auto &rawSrc10 = *rawSelfRoundtrip.playlists.at(created.playlistId).songs->at(10).value.source;
	rawSrc10.peer = userSelf;
	rawSrc10.alt[0].peer = userSelf;

	const auto roundtripDiff = DiffPlaylistLibraries(
		withFallback,
		rawSelfRoundtrip,
		selfId);
	CHECK(roundtripDiff);
	CHECK(!roundtripDiff.hasContentChanges());
	CHECK(!roundtripDiff.hasStoredRegisterChanges());
	CHECK(roundtripDiff.notices.empty());

	const auto noopJoin = JoinPlaylistLibraries(withFallback, rawSelfRoundtrip, selfId);
	CHECK(noopJoin);
	const auto noopJoinDiff = DiffPlaylistLibraries(
		withFallback,
		noopJoin.library,
		selfId);
	CHECK(noopJoinDiff);
	CHECK(!noopJoinDiff.hasContentChanges());
	CHECK(!noopJoinDiff.hasStoredRegisterChanges());
	CHECK(noopJoinDiff.notices.empty());

	auto higherStampSameValue = rawSelfRoundtrip;
	auto &plStamp = higherStampSameValue.playlists.at(created.playlistId);
	plStamp.title->stamp = 500;
	plStamp.deleted->stamp = 501;
	plStamp.order = PlaylistOrderRegister{ { 10, 20 }, 502 };
	plStamp.songs->at(10).stamp = 503;

	const auto stampOnlyDiff = DiffPlaylistLibraries(
		withFallback,
		higherStampSameValue,
		selfId);
	CHECK(stampOnlyDiff);
	CHECK(!stampOnlyDiff.hasContentChanges());
	CHECK(stampOnlyDiff.changes.empty());
	CHECK(stampOnlyDiff.notices.empty());
	CHECK(stampOnlyDiff.summary == PlaylistDiffSummary{});
	CHECK(stampOnlyDiff.hasStoredRegisterChanges());
	CHECK(stampOnlyDiff.registerChanges.size() == 1);
	const auto &regEntry = stampOnlyDiff.registerChanges.front();
	CHECK(regEntry.playlistId == created.playlistId);
	CHECK(regEntry.titleRegisterChanged);
	CHECK(regEntry.deletedRegisterChanged);
	CHECK(regEntry.orderRegisterChanged);
	CHECK(regEntry.songRegisterChanges.size() == 1);
	CHECK(regEntry.songRegisterChanges.front().documentId == 10);
	CHECK(regEntry.songRegisterChanges.front().beforeStamp == 110);
	CHECK(regEntry.songRegisterChanges.front().afterStamp == 503);

	auto changedHint = withFallback;
	auto &hintRegister = changedHint.playlists.at(created.playlistId).songs->at(20);
	hintRegister.value.source->username = u"new_hint"_q;
	hintRegister.stamp = 600;
	const auto hintDiff = DiffPlaylistLibraries(withFallback, changedHint, selfId);
	CHECK(hintDiff);
	CHECK(hintDiff.summary.changedSongs == 1);
	CHECK(hintDiff.summary.relinkedSongs == 0);
	CHECK(hintDiff.notices.empty());
	CHECK(!hintDiff.changes.front().visibleOrderChanged);
	CHECK(hintDiff.changes.front().changedSongs.size() == 1);
	CHECK(hintDiff.changes.front().changedSongs.front().primaryUsernameChanged);
	CHECK(!hintDiff.changes.front().changedSongs.front().relinked());
	CHECK(!hintDiff.changes.front().changedSongs.front().metaChanged);

	const auto renamed = RenamePlaylist(
		higherStampSameValue,
		created.playlistId,
		u"Driving Fast"_q,
		600,
		selfId);
	CHECK(renamed);
	const auto renameDiff = DiffPlaylistLibraries(
		higherStampSameValue,
		renamed.library,
		selfId);
	CHECK(renameDiff);
	CHECK(renameDiff.hasContentChanges());
	CHECK(renameDiff.changes.size() == 1);
	CHECK(renameDiff.changes.front().beforeTitle == u"Driving"_q);
	CHECK(renameDiff.changes.front().afterTitle == u"Driving Fast"_q);
	CHECK(renameDiff.changes.front().titleChanged);
	CHECK(renameDiff.summary.renamedPlaylists == 1);
	CHECK(renameDiff.notices.empty());
}

void TestTransitionsAndNoticeClassification() {
	constexpr auto selfId = uint64_t(777);
	const auto plNew = *FormatPlaylistId(MakeEntropy(0x10));
	const auto plDel = *FormatPlaylistId(MakeEntropy(0x11));
	const auto plRestore = *FormatPlaylistId(MakeEntropy(0x12));
	const auto plPurge = *FormatPlaylistId(MakeEntropy(0x13));
	const auto plPriorDelPurge = *FormatPlaylistId(MakeEntropy(0x14));
	const auto plMissing = *FormatPlaylistId(MakeEntropy(0x15));
	const auto plIncompQuiet = *FormatPlaylistId(MakeEntropy(0x16));
	const auto plIncompDel = *FormatPlaylistId(MakeEntropy(0x17));
	const auto plEmptyActive = *FormatPlaylistId(MakeEntropy(0x18));
	const auto plAbsentPurge = *FormatPlaylistId(MakeEntropy(0x19));
	const auto plIncompReveal = *FormatPlaylistId(MakeEntropy(0x1a));

	PlaylistLibrary before;
	before.playlists.emplace(plDel, Playlist{
		PlaylistTitleRegister{ u"Active To Delete"_q, 10 },
		PlaylistDeletedRegister{ false, 10 },
		PlaylistOrderRegister{ { 1 }, 10 },
		std::map<uint64_t, PlaylistSongRegister>{
			{ 1, PlaylistSongRegister{ PlaylistSongValue{ PlaylistSource{
				PeerToken{ PeerTokenKind::User, selfId }, 50, QString(), {},
				PlaylistSongMeta{ u"Keep"_q, u"Art"_q, 100 }, 10,
			} }, 10 } },
		},
	});
	before.playlists.emplace(plRestore, Playlist{
		PlaylistTitleRegister{ u"Was Deleted"_q, 20 },
		PlaylistDeletedRegister{ true, 20 },
		std::nullopt,
		std::map<uint64_t, PlaylistSongRegister>{
			{ 2, PlaylistSongRegister{ PlaylistSongValue{ PlaylistSource{
				PeerToken{ PeerTokenKind::Chat, 2 }, 60, QString(), {},
				PlaylistSongMeta{ u"Back"_q, u"Art"_q, 120 }, 20,
			} }, 20 } },
		},
	});
	before.playlists.emplace(plPurge, Playlist{
		PlaylistTitleRegister{ u"Active To Purge"_q, 30 },
		PlaylistDeletedRegister{ false, 30 },
		std::nullopt,
		std::map<uint64_t, PlaylistSongRegister>{},
	});
	before.playlists.emplace(plPriorDelPurge, Playlist{
		PlaylistTitleRegister{ u"Already Trash"_q, 40 },
		PlaylistDeletedRegister{ true, 40 },
		std::nullopt,
		std::map<uint64_t, PlaylistSongRegister>{},
	});
	before.playlists.emplace(plMissing, Playlist{
		PlaylistTitleRegister{ u"Vanished"_q, 50 },
		PlaylistDeletedRegister{ false, 50 },
		std::nullopt,
		std::map<uint64_t, PlaylistSongRegister>{},
	});
	before.playlists.emplace(plIncompQuiet, Playlist{
		PlaylistTitleRegister{ u"Missing Deleted Later"_q, 60 },
		PlaylistDeletedRegister{ false, 60 },
		std::nullopt,
		std::map<uint64_t, PlaylistSongRegister>{},
	});
	before.playlists.emplace(plIncompDel, Playlist{
		PlaylistTitleRegister{ u"Incomp With Deleted"_q, 70 },
		PlaylistDeletedRegister{ false, 70 },
		std::nullopt,
		std::map<uint64_t, PlaylistSongRegister>{},
	});
	before.playlists.emplace(plEmptyActive, Playlist{
		PlaylistTitleRegister{ u"Empty Active"_q, 80 },
		PlaylistDeletedRegister{ false, 80 },
		std::nullopt,
		std::map<uint64_t, PlaylistSongRegister>{},
	});
	before.playlists.emplace(plIncompReveal, Playlist{
		PlaylistTitleRegister{ u"Only Title"_q, 90 },
		std::nullopt,
		std::nullopt,
		std::nullopt,
	});

	PlaylistLibrary after;
	after.playlists.emplace(plNew, Playlist{
		PlaylistTitleRegister{ u"Brand New"_q, 100 },
		PlaylistDeletedRegister{ false, 100 },
		std::nullopt,
		std::map<uint64_t, PlaylistSongRegister>{
			{ 9, PlaylistSongRegister{ PlaylistSongValue{ PlaylistSource{
				PeerToken{ PeerTokenKind::Chat, 9 }, 9, QString(), {},
				PlaylistSongMeta{ u"New"_q, u"Artist"_q, 90 }, 100,
			} }, 100 } },
		},
	});
	after.playlists.emplace(plDel, before.playlists.at(plDel));
	after.playlists.at(plDel).deleted = PlaylistDeletedRegister{ true, 110 };
	after.playlists.emplace(plRestore, before.playlists.at(plRestore));
	after.playlists.at(plRestore).deleted = PlaylistDeletedRegister{ false, 120 };
	after.purged.insert(plPurge);
	after.purged.insert(plPriorDelPurge);
	after.playlists.emplace(plIncompQuiet, Playlist{
		PlaylistTitleRegister{ u"Missing Deleted Later"_q, 60 },
		std::nullopt,
		std::nullopt,
		std::nullopt,
	});
	after.playlists.emplace(plIncompDel, Playlist{
		std::nullopt,
		PlaylistDeletedRegister{ true, 130 },
		std::nullopt,
		std::nullopt,
	});
	after.playlists.emplace(plEmptyActive, Playlist{
		PlaylistTitleRegister{ u"Empty Active"_q, 80 },
		PlaylistDeletedRegister{ true, 140 },
		std::nullopt,
		std::map<uint64_t, PlaylistSongRegister>{},
	});
	after.purged.insert(plAbsentPurge);
	after.playlists.emplace(plIncompReveal, Playlist{
		PlaylistTitleRegister{ u"Only Title"_q, 90 },
		PlaylistDeletedRegister{ false, 150 },
		std::nullopt,
		std::nullopt,
	});

	const auto diff = DiffPlaylistLibraries(before, after, selfId);
	CHECK(diff);
	CHECK(diff.changes.size() == 11);
	for (std::size_t i = 1; i < diff.changes.size(); ++i) {
		CHECK(diff.changes[i - 1].playlistId < diff.changes[i].playlistId);
	}

	const auto findChange = [&](const QString &id) -> const PlaylistDiffEntry * {
		for (const auto &entry : diff.changes) {
			if (entry.playlistId == id) {
				return &entry;
			}
		}
		return nullptr;
	};

	const auto *cDel = findChange(plDel);
	CHECK(cDel != nullptr);
	CHECK(cDel->beforePresence == PlaylistDiffPresence::Visible);
	CHECK(cDel->afterPresence == PlaylistDiffPresence::Deleted);
	CHECK(cDel->deletedChanged);
	CHECK(!cDel->visibleOrderChanged);
	CHECK(cDel->addedSongIds.empty());
	CHECK(cDel->removedSongIds.empty());
	CHECK(cDel->changedSongs.empty());

	const auto *cRestore = findChange(plRestore);
	CHECK(cRestore != nullptr);
	CHECK(cRestore->beforePresence == PlaylistDiffPresence::Deleted);
	CHECK(cRestore->afterPresence == PlaylistDiffPresence::Visible);
	CHECK(cRestore->deletedChanged);
	CHECK(cRestore->addedSongIds.empty());
	CHECK(cRestore->removedSongIds.empty());
	CHECK(!cRestore->visibleOrderChanged);

	const auto *cNew = findChange(plNew);
	CHECK(cNew != nullptr);
	CHECK(cNew->beforePresence == PlaylistDiffPresence::Absent);
	CHECK(cNew->afterPresence == PlaylistDiffPresence::Visible);
	CHECK(cNew->addedSongIds == (std::vector<uint64_t>{ 9 }));

	const auto *cPriorDelPurge = findChange(plPriorDelPurge);
	CHECK(cPriorDelPurge != nullptr);
	CHECK(cPriorDelPurge->beforePresence == PlaylistDiffPresence::Deleted);
	CHECK(cPriorDelPurge->afterPresence == PlaylistDiffPresence::Purged);

	const auto *cIncompReveal = findChange(plIncompReveal);
	CHECK(cIncompReveal != nullptr);
	CHECK(cIncompReveal->beforePresence == PlaylistDiffPresence::Incomplete);
	CHECK(cIncompReveal->afterPresence == PlaylistDiffPresence::Visible);

	CHECK(diff.notices.size() == 4);
	for (std::size_t i = 1; i < diff.notices.size(); ++i) {
		CHECK(diff.notices[i - 1].playlistId < diff.notices[i].playlistId);
	}
	const auto findNotice = [&](const QString &id) -> const PlaylistNotice * {
		for (const auto &notice : diff.notices) {
			if (notice.playlistId == id) {
				return &notice;
			}
		}
		return nullptr;
	};
	CHECK(findNotice(plNew) == nullptr);
	CHECK(findNotice(plRestore) == nullptr);
	CHECK(findNotice(plPriorDelPurge) == nullptr);
	CHECK(findNotice(plMissing) == nullptr);
	CHECK(findNotice(plIncompQuiet) == nullptr);
	CHECK(findNotice(plAbsentPurge) == nullptr);
	CHECK(findNotice(plIncompReveal) == nullptr);

	const auto *nDel = findNotice(plDel);
	CHECK(nDel != nullptr);
	CHECK(nDel->reason == PlaylistNoticeReason::DeletedElsewhere);
	CHECK(nDel->suggestedAction == PlaylistNoticeAction::Restore);
	CHECK(nDel->preMergePlaylist.songs->at(1).value.source->peer == PeerToken{});

	const auto *nPurge = findNotice(plPurge);
	CHECK(nPurge != nullptr);
	CHECK(nPurge->reason == PlaylistNoticeReason::PurgedElsewhere);
	CHECK(nPurge->suggestedAction == PlaylistNoticeAction::KeepCopy);

	const auto *nIncompDel = findNotice(plIncompDel);
	CHECK(nIncompDel != nullptr);
	CHECK(nIncompDel->reason == PlaylistNoticeReason::DeletedElsewhere);
	CHECK(nIncompDel->suggestedAction == PlaylistNoticeAction::Restore);

	const auto *nEmpty = findNotice(plEmptyActive);
	CHECK(nEmpty != nullptr);
	CHECK(nEmpty->reason == PlaylistNoticeReason::DeletedElsewhere);
	CHECK(nEmpty->suggestedAction == PlaylistNoticeAction::Restore);
	CHECK(nEmpty->preMergePlaylist.deleted && !nEmpty->preMergePlaylist.deleted->value);
}

void TestConcurrentNoticesAndKeepCopy() {
	constexpr auto selfId = uint64_t(9001);
	const auto created = CreatePlaylist(
		PlaylistLibrary{},
		u"Workout Mix"_q,
		MakeEntropy(0x21),
		100,
		selfId);
	CHECK(created);
	const auto baseAdded = AddPlaylistSongs(
		created.library,
		created.playlistId,
		{
			MakeSongInput(10, PeerToken{ PeerTokenKind::Chat, 5 }, 10, u"Warmup"_q, u"DJ"_q, 180),
		},
		110,
		selfId);
	CHECK(baseAdded);

	auto richLocal = AddPlaylistSongs(
		baseAdded.library,
		created.playlistId,
		{
			MakeSongInput(20, PeerToken{ PeerTokenKind::User, selfId }, 20, u"Peak"_q, u"Runner"_q, 240),
			MakeSongInput(30, PeerToken{ PeerTokenKind::Channel, 77 }, 30, u"Cooldown"_q, u"Band"_q, 200),
		},
		200,
		selfId);
	CHECK(richLocal);
	auto &s20 = *richLocal.library.playlists.at(created.playlistId).songs->at(20).value.source;
	s20.alt = {
		PlaylistSourceRef{ PeerToken{ PeerTokenKind::User, selfId }, 21, QString() },
		PlaylistSourceRef{ PeerToken{ PeerTokenKind::Chat, 88 }, 22, u"alt_chat"_q },
		PlaylistSourceRef{ PeerToken{ PeerTokenKind::Channel, 99 }, 23, u"alt_chan"_q },
	};
	richLocal.library.playlists.at(created.playlistId).songs->at(20).value.source->peer =
		PeerToken{ PeerTokenKind::User, selfId };
	const auto reorderedLocal = ReorderPlaylistSongs(
		richLocal.library,
		created.playlistId,
		{ 30, 10, 20 },
		250,
		selfId);
	CHECK(reorderedLocal);
	const auto localBeforeSnapshot = reorderedLocal.library;

	const auto remoteDeleted = DeletePlaylist(baseAdded.library, created.playlistId, 300, selfId);
	CHECK(remoteDeleted);
	const auto mergedDel = JoinPlaylistLibraries(
		localBeforeSnapshot,
		remoteDeleted.library,
		selfId);
	CHECK(mergedDel);

	const auto diffDel = DiffPlaylistLibraries(localBeforeSnapshot, mergedDel.library, selfId);
	CHECK(diffDel);
	CHECK(diffDel.notices.size() == 1);
	const auto &delNotice = diffDel.notices.front();
	CHECK(delNotice.reason == PlaylistNoticeReason::DeletedElsewhere);
	CHECK(delNotice.suggestedAction == PlaylistNoticeAction::Restore);
	CHECK(delNotice.title == u"Workout Mix"_q);
	CHECK(delNotice.preMergePlaylist.deleted->value == false);
	CHECK(delNotice.preMergePlaylist.deleted->stamp == 100);
	CHECK(PlaylistVisibleSongIds(delNotice.preMergePlaylist) == (std::vector<uint64_t>{ 30, 10, 20 }));
	CHECK(delNotice.preMergePlaylist.songs->at(20).value.source->peer == PeerToken{});
	CHECK(delNotice.preMergePlaylist.songs->at(20).value.source->alt.size() == 3);
	CHECK(delNotice.preMergePlaylist.songs->at(20).value.source->alt[0].peer == PeerToken{});
	CHECK(delNotice.preMergePlaylist.songs->at(20).value.source->meta.title == u"Peak"_q);
	CHECK(delNotice.preMergePlaylist.songs->at(20).value.source->meta.performer == u"Runner"_q);
	CHECK(delNotice.preMergePlaylist.songs->at(20).value.source->meta.duration == 240);
	CHECK(diffDel.changes.size() == 1);
	CHECK(diffDel.changes.front().addedSongIds.empty());
	CHECK(diffDel.changes.front().removedSongIds.empty());
	CHECK(!diffDel.changes.front().visibleOrderChanged);

	const auto remotePurged = DeleteForeverPlaylist(remoteDeleted.library, created.playlistId, selfId);
	CHECK(remotePurged);
	const auto mergedPurge = JoinPlaylistLibraries(
		localBeforeSnapshot,
		remotePurged.library,
		selfId);
	CHECK(mergedPurge);

	const auto diffPurge = DiffPlaylistLibraries(localBeforeSnapshot, mergedPurge.library, selfId);
	CHECK(diffPurge);
	CHECK(diffPurge.notices.size() == 1);
	if (diffPurge.notices.size() == 1) {
		const auto &purgeNotice = diffPurge.notices.front();
		CHECK(purgeNotice.reason == PlaylistNoticeReason::PurgedElsewhere);
		CHECK(purgeNotice.suggestedAction == PlaylistNoticeAction::KeepCopy);
		const auto snapshotBeforeCopy = purgeNotice.preMergePlaylist;

		const auto keptCopy = CopyPlaylist(
			mergedPurge.library,
			purgeNotice.preMergePlaylist,
			MakeEntropy(0x22),
			500,
			selfId);
		CHECK(keptCopy);
		CHECK(keptCopy.changed);
		CHECK(purgeNotice.preMergePlaylist == snapshotBeforeCopy);
		CHECK(keptCopy.library.purged.contains(created.playlistId));
		const auto &copiedPl = keptCopy.library.playlists.at(keptCopy.playlistId);
		CHECK(copiedPl.title->value == u"Workout Mix (copy)"_q);
		CHECK(PlaylistVisibleSongIds(copiedPl) == (std::vector<uint64_t>{ 30, 10, 20 }));
		CHECK(copiedPl.songs->at(20).value.source->alt.size() == 3);
		CHECK(copiedPl.songs->at(20).value.source->alt[0].peer == PeerToken{});
	}
}

void TestSongDiffsTombstonesAndOrderSemantics() {
	constexpr auto selfId = uint64_t(123);
	const auto plId = *FormatPlaylistId(MakeEntropy(0x31));

	auto makeBaseSong = [&](uint64_t docId, uint64_t since, uint64_t stamp) {
		return PlaylistSongRegister{
			PlaylistSongValue{ PlaylistSource{
				PeerToken{ PeerTokenKind::Chat, docId },
				int32_t(docId * 10),
				QString(),
				{ PlaylistSourceRef{ PeerToken{ PeerTokenKind::Channel, 5 }, int32_t(docId), QString() } },
				PlaylistSongMeta{ u"T"_q, u"P"_q, 150 },
				since,
			} },
			stamp,
		};
	};

	PlaylistLibrary before;
	before.playlists.emplace(plId, Playlist{
		PlaylistTitleRegister{ u"Songs"_q, 100 },
		PlaylistDeletedRegister{ false, 100 },
		PlaylistOrderRegister{ { 10, 20, 30, 40 }, 100 },
		std::map<uint64_t, PlaylistSongRegister>{
			{ 10, makeBaseSong(10, 80, 80) },
			{ 20, makeBaseSong(20, 85, 85) },
			{ 30, makeBaseSong(30, 90, 90) },
			{ 40, makeBaseSong(40, 95, 95) },
			{ 50, makeBaseSong(50, 96, 96) },
			{ 60, makeBaseSong(60, 97, 97) },
			{ 70, PlaylistSongRegister{ PlaylistSongValue{ std::nullopt }, 98 } },
		},
	});

	auto after = before;
	auto &afterPl = after.playlists.at(plId);
	auto &afterSongs = *afterPl.songs;
	// 10: relinked primary source (same order)
	afterSongs.at(10).value.source->msg = 999;
	afterSongs.at(10).stamp = 200;
	// 20: fallback-only change
	afterSongs.at(20).value.source->alt.push_back(
		PlaylistSourceRef{ PeerToken{ PeerTokenKind::User, 8 }, 200, u"usr8"_q });
	afterSongs.at(20).stamp = 201;
	// 30: metadata-only change
	afterSongs.at(30).value.source->meta.title = u"T Edited"_q;
	afterSongs.at(30).stamp = 202;
	// 40: since-only change (jumps past order stamp 100 -> moves to tail!)
	afterSongs.at(40).value.source->since = 210;
	afterSongs.at(40).stamp = 210;
	// 50: removed via tombstone; 60: removed via register absence; 70: tomb -> re-added; 80: brand new
	afterSongs.at(50) = PlaylistSongRegister{ PlaylistSongValue{ std::nullopt }, 215 };
	afterSongs.erase(60);
	afterSongs.at(70) = makeBaseSong(70, 220, 220);
	afterSongs.emplace(80, makeBaseSong(80, 225, 225));
	afterSongs.emplace(90, PlaylistSongRegister{ PlaylistSongValue{ std::nullopt }, 230 });

	const auto diff = DiffPlaylistLibraries(before, after, selfId);
	CHECK(diff);
	CHECK(diff.changes.size() == 1);
	CHECK(diff.notices.empty());
	const auto &c = diff.changes.front();
	CHECK(c.addedSongIds == (std::vector<uint64_t>{ 70, 80 }));
	CHECK(c.removedSongIds == (std::vector<uint64_t>{ 50, 60 }));
	CHECK(c.visibleOrderChanged);
	CHECK(c.beforeVisibleSongOrder == (std::vector<uint64_t>{ 10, 20, 30, 40, 50, 60 }));
	CHECK(c.afterVisibleSongOrder == (std::vector<uint64_t>{ 10, 20, 30, 40, 70, 80 }));
	CHECK(c.changedSongs.size() == 4);
	CHECK(c.changedSongs[0].documentId == 10);
	CHECK(c.changedSongs[0].relinked());
	CHECK(!c.changedSongs[0].fallbacksChanged);
	CHECK(!c.changedSongs[0].metaChanged);
	CHECK(c.changedSongs[1].documentId == 20);
	CHECK(!c.changedSongs[1].relinked());
	CHECK(c.changedSongs[1].fallbacksChanged);
	CHECK(c.changedSongs[2].documentId == 30);
	CHECK(!c.changedSongs[2].relinked());
	CHECK(c.changedSongs[2].metaChanged);
	CHECK(c.changedSongs[3].documentId == 40);
	CHECK(c.changedSongs[3].sinceChanged);

	// 90 (absent -> tombstone) did NOT appear in added/removed, yet registerChanges tracks Absent vs Tombstone,
	// and distinguishes 50 (Present -> Tombstone) vs 60 (Present -> Absent).
	CHECK(diff.registerChanges.size() == 1);
	const auto &srcChanges = diff.registerChanges.front().songRegisterChanges;
	const auto findReg = [&](uint64_t docId) -> const PlaylistSongRegisterChange * {
		for (const auto &rc : srcChanges) {
			if (rc.documentId == docId) {
				return &rc;
			}
		}
		return nullptr;
	};
	CHECK(findReg(50) && findReg(50)->afterState == PlaylistSongRegisterState::Tombstone);
	CHECK(findReg(60) && findReg(60)->afterState == PlaylistSongRegisterState::Absent);
	CHECK(findReg(90) && findReg(90)->beforeState == PlaylistSongRegisterState::Absent);
	CHECK(findReg(90) && findReg(90)->afterState == PlaylistSongRegisterState::Tombstone);

	// Order stamp crossing a song's `since` without changing order->value must count as visibleOrderChanged.
	PlaylistLibrary orderStampLib;
	orderStampLib.playlists.emplace(plId, Playlist{
		PlaylistTitleRegister{ u"StampOrder"_q, 100 },
		PlaylistDeletedRegister{ false, 100 },
		PlaylistOrderRegister{ { 1, 2 }, 100 },
		std::map<uint64_t, PlaylistSongRegister>{
			{ 1, makeBaseSong(1, 90, 90) },
			{ 2, makeBaseSong(2, 120, 120) },
			{ 3, makeBaseSong(3, 110, 110) },
		},
	});
	auto orderStampAfter = orderStampLib;
	orderStampAfter.playlists.at(plId).order->stamp = 130;
	const auto orderStampDiff = DiffPlaylistLibraries(orderStampLib, orderStampAfter, selfId);
	CHECK(orderStampDiff);
	CHECK(orderStampDiff.hasContentChanges());
	CHECK(orderStampDiff.changes.front().visibleOrderChanged);
	CHECK(orderStampDiff.changes.front().beforeVisibleSongOrder == (std::vector<uint64_t>{ 1, 3, 2 }));
	CHECK(orderStampDiff.changes.front().afterVisibleSongOrder == (std::vector<uint64_t>{ 1, 2, 3 }));
	CHECK(orderStampDiff.summary.reorderedPlaylists == 1);

	// Adding an absent->tombstone song alone produces NO content change.
	auto tombstoneOnlyAfter = orderStampLib;
	tombstoneOnlyAfter.playlists.at(plId).songs->emplace(
		999, PlaylistSongRegister{ PlaylistSongValue{ std::nullopt }, 200 });
	const auto tombOnlyDiff = DiffPlaylistLibraries(orderStampLib, tombstoneOnlyAfter, selfId);
	CHECK(tombOnlyDiff);
	CHECK(!tombOnlyDiff.hasContentChanges());
	CHECK(tombOnlyDiff.notices.empty());
	CHECK(tombOnlyDiff.summary == PlaylistDiffSummary{});
	CHECK(tombOnlyDiff.hasStoredRegisterChanges());
	CHECK(tombOnlyDiff.registerChanges.front().songRegisterChanges.size() == 1);
	CHECK(tombOnlyDiff.registerChanges.front().songRegisterChanges.front().beforeState
		== PlaylistSongRegisterState::Absent);
	CHECK(tombOnlyDiff.registerChanges.front().songRegisterChanges.front().afterState
		== PlaylistSongRegisterState::Tombstone);

	// Real operations: relink keeps display order, remove+readd moves to tail,
	// orphan order IDs stay quiet in content changes, and editing a deleted playlist emits no notice.
	const auto opsStart = CreatePlaylist(PlaylistLibrary{}, u"OpsOrder"_q, MakeEntropy(0x32), 100);
	const auto opsSongs = AddPlaylistSongs(
		opsStart.library,
		opsStart.playlistId,
		{
			MakeSongInput(1, PeerToken{ PeerTokenKind::Chat, 1 }, 10),
			MakeSongInput(2, PeerToken{ PeerTokenKind::Chat, 1 }, 20),
		},
		110);
	const auto opsOrdered = ReorderPlaylistSongs(opsSongs.library, opsStart.playlistId, { 2, 1 }, 120);
	const auto opsRelinked = AddPlaylistSongs(
		opsOrdered.library,
		opsStart.playlistId,
		{ MakeSongInput(2, PeerToken{ PeerTokenKind::Channel, 9 }, 200) },
		130);
	const auto relinkOrderDiff = DiffPlaylistLibraries(opsOrdered.library, opsRelinked.library);
	CHECK(relinkOrderDiff);
	CHECK(!relinkOrderDiff.changes.front().visibleOrderChanged);
	CHECK(relinkOrderDiff.changes.front().changedSongs.size() == 1);
	CHECK(relinkOrderDiff.changes.front().changedSongs.front().relinked());
	CHECK(relinkOrderDiff.changes.front().changedSongs.front().fallbacksChanged);

	const auto opsRemoved = RemovePlaylistSongs(opsOrdered.library, opsStart.playlistId, { 2 }, 140);
	const auto opsReadded = AddPlaylistSongs(
		opsRemoved.library,
		opsStart.playlistId,
		{ MakeSongInput(2, PeerToken{ PeerTokenKind::Chat, 1 }, 20) },
		150);
	const auto readdOrderDiff = DiffPlaylistLibraries(opsOrdered.library, opsReadded.library);
	CHECK(readdOrderDiff);
	CHECK(readdOrderDiff.changes.front().visibleOrderChanged);
	CHECK(readdOrderDiff.changes.front().beforeVisibleSongOrder == (std::vector<uint64_t>{ 2, 1 }));
	CHECK(readdOrderDiff.changes.front().afterVisibleSongOrder == (std::vector<uint64_t>{ 1, 2 }));
	CHECK(readdOrderDiff.changes.front().changedSongs.size() == 1);
	CHECK(readdOrderDiff.changes.front().changedSongs.front().sinceChanged);

	auto orphanOrder = opsOrdered.library;
	orphanOrder.playlists.at(opsStart.playlistId).order->value.insert(
		orphanOrder.playlists.at(opsStart.playlistId).order->value.begin(), 9999);
	const auto orphanDiff = DiffPlaylistLibraries(opsOrdered.library, orphanOrder);
	CHECK(orphanDiff);
	CHECK(!orphanDiff.hasContentChanges());
	CHECK(orphanDiff.hasStoredRegisterChanges());
	CHECK(orphanDiff.registerChanges.front().orderRegisterChanged);

	const auto delBefore = DeletePlaylist(opsOrdered.library, opsStart.playlistId, 200);
	auto delEdited = delBefore.library;
	delEdited.playlists.at(opsStart.playlistId).songs->emplace(
		3, PlaylistSongRegister{ PlaylistSongValue{ PlaylistSource{
			PeerToken{ PeerTokenKind::Chat, 1 }, 30, QString(), {},
			PlaylistSongMeta{ u"AddedWhileDeleted"_q, u"Art"_q, 180 }, 210,
		} }, 210 });
	const auto delEditDiff = DiffPlaylistLibraries(delBefore.library, delEdited);
	CHECK(delEditDiff);
	CHECK(delEditDiff.notices.empty());
	CHECK(delEditDiff.changes.size() == 1);
	CHECK(delEditDiff.changes.front().addedSongIds == (std::vector<uint64_t>{ 3 }));
}

void TestValidationRefusalsAndImmutability() {
	const auto validBefore = CreatePlaylist(
		PlaylistLibrary{},
		u"Valid"_q,
		MakeEntropy(0x41),
		100);
	CHECK(validBefore);
	const auto beforeCopy = validBefore.library;
	auto validAfter = RenamePlaylist(beforeCopy, validBefore.playlistId, u"Valid 2"_q, 120).library;
	const auto afterCopy = validAfter;

	const auto badSelf = DiffPlaylistLibraries(
		validBefore.library,
		validAfter,
		uint64_t(std::numeric_limits<int64_t>::max()) + 1);
	CHECK(!badSelf);
	CHECK(badSelf.status == PlaylistLibraryStatus::Invalid);
	CHECK(badSelf.error == PlaylistLibraryError::InvalidSelfUserId);
	CHECK(badSelf.side == PlaylistDiffSide::SelfUserId);
	CHECK(badSelf.changes.empty());
	CHECK(badSelf.registerChanges.empty());
	CHECK(badSelf.notices.empty());

	auto newerBefore = validBefore.library;
	newerBefore.format = 2;
	const auto newerBeforeRes = DiffPlaylistLibraries(newerBefore, validAfter);
	CHECK(!newerBeforeRes);
	CHECK(newerBeforeRes.status == PlaylistLibraryStatus::NewerFormat);
	CHECK(newerBeforeRes.side == PlaylistDiffSide::Before);
	CHECK(newerBeforeRes.changes.empty());
	CHECK(newerBeforeRes.notices.empty());

	auto newerAfter = validAfter;
	newerAfter.format = 3;
	const auto newerAfterRes = DiffPlaylistLibraries(validBefore.library, newerAfter);
	CHECK(!newerAfterRes);
	CHECK(newerAfterRes.status == PlaylistLibraryStatus::NewerFormat);
	CHECK(newerAfterRes.side == PlaylistDiffSide::After);
	CHECK(newerAfterRes.changes.empty());
	CHECK(newerAfterRes.notices.empty());

	auto invalidBefore = validBefore.library;
	invalidBefore.playlists.at(validBefore.playlistId).title->value = QString();
	const auto invalidBeforeRes = DiffPlaylistLibraries(invalidBefore, validAfter);
	CHECK(!invalidBeforeRes);
	CHECK(invalidBeforeRes.status == PlaylistLibraryStatus::Invalid);
	CHECK(invalidBeforeRes.error == PlaylistLibraryError::InvalidTitle);
	CHECK(invalidBeforeRes.side == PlaylistDiffSide::Before);
	CHECK(invalidBeforeRes.changes.empty());
	CHECK(invalidBeforeRes.notices.empty());

	auto invalidAfter = validAfter;
	invalidAfter.purged.insert(validBefore.playlistId);
	const auto invalidAfterRes = DiffPlaylistLibraries(validBefore.library, invalidAfter);
	CHECK(!invalidAfterRes);
	CHECK(invalidAfterRes.status == PlaylistLibraryStatus::Invalid);
	CHECK(invalidAfterRes.error == PlaylistLibraryError::PurgedPlaylistPresent);
	CHECK(invalidAfterRes.side == PlaylistDiffSide::After);
	CHECK(invalidAfterRes.changes.empty());
	CHECK(invalidAfterRes.notices.empty());

	const auto okRes = DiffPlaylistLibraries(validBefore.library, validAfter);
	CHECK(okRes);
	CHECK(validBefore.library == beforeCopy);
	CHECK(validAfter == afterCopy);
}

} // namespace

int main() {
	TestIdentityNormalizedNoopAndStampOnly();
	TestTransitionsAndNoticeClassification();
	TestConcurrentNoticesAndKeepCopy();
	TestSongDiffsTombstonesAndOrderSemantics();
	TestValidationRefusalsAndImmutability();

	if (Failures > 0) {
		std::printf("FAILED %d/%d checks\n", Failures, Checks);
		return 1;
	}
	std::printf("PASSED %d checks\n", Checks);
	return 0;
}
