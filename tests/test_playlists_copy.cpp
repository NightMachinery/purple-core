/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists.h"
#include "purple/purple_playlists_ops.h"
#include "purple/purple_sync_json.h"
#include "purple/purple_types.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <cstdio>
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

QByteArray MakeEntropy(uint8_t fill) {
	return QByteArray(int(kPlaylistIdEntropyBytes), char(fill));
}

PlaylistSource MakeComplexSource(uint64_t selfId, uint64_t since) {
	return PlaylistSource{
		.peer = PeerToken{ PeerTokenKind::User, selfId },
		.msg = 700,
		.username = u"main_channel"_q,
		.alt = {
			PlaylistSourceRef{ PeerToken{ PeerTokenKind::Chat, 11 }, 101, u"alt_a"_q },
			PlaylistSourceRef{ PeerToken{ PeerTokenKind::Channel, 22 }, 202, QString() },
			PlaylistSourceRef{ PeerToken{ PeerTokenKind::User, selfId }, 303, u"alt_c"_q },
		},
		.meta = PlaylistSongMeta{ u"Main Track"_q, u"Composer"_q, 245 },
		.since = since,
	};
}

void TestEmptyAndRichDisplayOrderCopy() {
	const uint64_t selfId = 42;
	PlaylistLibrary dest;
	const auto emptyCreate = CreatePlaylist(dest, u"Empty Src"_q, MakeEntropy(0x01), 100, selfId);
	CHECK(emptyCreate);
	const auto &emptySrc = emptyCreate.library.playlists.at(emptyCreate.playlistId);

	const auto emptyCopy = CopyPlaylist(emptyCreate.library, emptySrc, MakeEntropy(0x02), 500, selfId);
	CHECK(emptyCopy);
	CHECK(emptyCopy.changed);
	CHECK(emptyCopy.addedCount == 0);
	CHECK(emptyCopy.playlistId != emptyCreate.playlistId);
	CHECK(emptyCopy.undo.kind == PlaylistUndoKind::Copy);
	CHECK(emptyCopy.library.playlists.at(emptyCreate.playlistId) == emptySrc);
	const auto &emptyCopiedPl = emptyCopy.library.playlists.at(emptyCopy.playlistId);
	CHECK(emptyCopiedPl.title == (PlaylistTitleRegister{ u"Empty Src (copy)"_q, 500 }));
	CHECK(emptyCopiedPl.deleted == (PlaylistDeletedRegister{ false, 500 }));
	CHECK(emptyCopiedPl.order == (PlaylistOrderRegister{ {}, 500 }));
	CHECK(emptyCopiedPl.songs.has_value() && emptyCopiedPl.songs->empty());

	const auto undoEmpty = UndoPlaylistOperation(emptyCopy.library, emptyCopy.undo, 0, selfId);
	CHECK(undoEmpty);
	CHECK(undoEmpty.changed);
	CHECK(!undoEmpty.library.playlists.contains(emptyCopy.playlistId));
	CHECK(undoEmpty.library.purged.contains(emptyCopy.playlistId));
	CHECK(undoEmpty.library.playlists.at(emptyCreate.playlistId) == emptySrc);

	Playlist src;
	src.title = PlaylistTitleRegister{ u"Roadmix"_q, 9000 };
	src.deleted = PlaylistDeletedRegister{ true, 9500 };
	src.order = PlaylistOrderRegister{ { 300, 999, 100, 200 }, 200 };
	src.songs = std::map<uint64_t, PlaylistSongRegister>{
		{ 100, PlaylistSongRegister{ PlaylistSongValue{ std::nullopt }, 250 } },
		{ 200, PlaylistSongRegister{ PlaylistSongValue{ MakeComplexSource(selfId, 150) }, 8000 } },
		{ 300, PlaylistSongRegister{ PlaylistSongValue{ PlaylistSource{
			.peer = PeerToken{ PeerTokenKind::Channel, 9 },
			.msg = 12,
			.username = QString(),
			.alt = {},
			.meta = PlaylistSongMeta{ u"A"_q, u"P"_q, 60 },
			.since = 120,
		} }, 120 } },
		{ 400, PlaylistSongRegister{ PlaylistSongValue{ PlaylistSource{
			.peer = PeerToken{ PeerTokenKind::Chat, 8 },
			.msg = 15,
			.username = QString(),
			.alt = {},
			.meta = PlaylistSongMeta{ u"Readded"_q, u"Q"_q, 90 },
			.since = 300,
		} }, 300 } },
	};

	const auto expectedOrder = PlaylistVisibleSongIds(src);
	CHECK((expectedOrder == std::vector<uint64_t>{ 300, 200, 400 }));

	auto destWithPurgedOld = emptyCreate.library;
	const auto oldPurgedId = *FormatPlaylistId(MakeEntropy(0x03));
	destWithPurgedOld.purged.insert(oldPurgedId);
	const auto copied = CopyPlaylist(
		destWithPurgedOld,
		src,
		MakeEntropy(0x04),
		2000,
		selfId);
	CHECK(copied);
	CHECK(copied.changed);
	CHECK(copied.addedCount == 3);
	CHECK(ValidatePlaylistLibrary(copied.library, selfId).status == PlaylistLibraryStatus::Valid);

	const auto &copiedPl = copied.library.playlists.at(copied.playlistId);
	CHECK(copiedPl.title == (PlaylistTitleRegister{ u"Roadmix (copy)"_q, 2000 }));
	CHECK(copiedPl.deleted == (PlaylistDeletedRegister{ false, 2000 }));
	CHECK(copiedPl.order == (PlaylistOrderRegister{ {}, 2000 }));
	CHECK((PlaylistVisibleSongIds(copiedPl) == std::vector<uint64_t>{ 300, 200, 400 }));
	CHECK(!copiedPl.songs->contains(100));
	CHECK(!copiedPl.songs->contains(999));

	const auto &song300 = copiedPl.songs->at(300);
	const auto &song200 = copiedPl.songs->at(200);
	const auto &song400 = copiedPl.songs->at(400);
	CHECK(song300.stamp == 2001 && song300.value.source->since == 2001);
	CHECK(song200.stamp == 2002 && song200.value.source->since == 2002);
	CHECK(song400.stamp == 2003 && song400.value.source->since == 2003);
	CHECK(song200.value.source->peer == (PeerToken{ PeerTokenKind::Self, 0 }));
	CHECK(song200.value.source->msg == 700);
	CHECK(song200.value.source->username == u"main_channel"_q);
	CHECK(song200.value.source->meta == (PlaylistSongMeta{ u"Main Track"_q, u"Composer"_q, 245 }));
	CHECK(song200.value.source->alt.size() == 3);
	CHECK(song200.value.source->alt.at(0) == (PlaylistSourceRef{ PeerToken{ PeerTokenKind::Chat, 11 }, 101, u"alt_a"_q }));
	CHECK(song200.value.source->alt.at(1) == (PlaylistSourceRef{ PeerToken{ PeerTokenKind::Channel, 22 }, 202, QString() }));
	CHECK(song200.value.source->alt.at(2) == (PlaylistSourceRef{ PeerToken{ PeerTokenKind::Self, 0 }, 303, u"alt_c"_q }));
	CHECK(copied.library.purged.contains(oldPurgedId));
	CHECK(copied.library.playlists.at(emptyCreate.playlistId) == emptySrc);
}

void TestRefusalsTitleTruncationAndStamps() {
	PlaylistLibrary lib;
	const auto active = CreatePlaylist(lib, u"One"_q, MakeEntropy(0x11), 100);
	const auto validSrc = active.library.playlists.at(active.playlistId);

	CHECK(CopyPlaylist(active.library, validSrc, MakeEntropy(0x11), 200).error
		== PlaylistEditError::IdCollision);

	auto withPurged = active.library;
	const auto purgedId = *FormatPlaylistId(MakeEntropy(0x12));
	withPurged.purged.insert(purgedId);
	CHECK(CopyPlaylist(withPurged, validSrc, MakeEntropy(0x12), 200).error
		== PlaylistEditError::IdCollision);
	CHECK(CopyPlaylist(withPurged, validSrc, QByteArray(9, 'a'), 200).error
		== PlaylistEditError::InvalidEntropy);
	CHECK(CopyPlaylist(withPurged, validSrc, MakeEntropy(0x13), 0).error
		== PlaylistEditError::InvalidStamp);
	CHECK(CopyPlaylist(withPurged, validSrc, MakeEntropy(0x13), kMaximumPlaylistStamp + 1).error
		== PlaylistEditError::InvalidStamp);

	Playlist partialNoDeleted;
	partialNoDeleted.title = PlaylistTitleRegister{ u"Partial"_q, 10 };
	CHECK(CopyPlaylist(withPurged, partialNoDeleted, MakeEntropy(0x13), 200).error
		== PlaylistEditError::PlaylistIncomplete);

	Playlist partialNoTitle;
	partialNoTitle.deleted = PlaylistDeletedRegister{ false, 10 };
	CHECK(CopyPlaylist(withPurged, partialNoTitle, MakeEntropy(0x13), 200).error
		== PlaylistEditError::PlaylistIncomplete);

	Playlist invalidSrc = validSrc;
	invalidSrc.title->value = u"   bad   "_q;
	CHECK(CopyPlaylist(withPurged, invalidSrc, MakeEntropy(0x13), 200).error
		== PlaylistEditError::InvalidTitle);

	Playlist badPeerSrc = validSrc;
	badPeerSrc.songs->emplace(1, PlaylistSongRegister{
		PlaylistSongValue{ PlaylistSource{
			.peer = PeerToken{ PeerTokenKind::User, 0 },
			.msg = 1,
			.since = 10,
		} },
		10,
	});
	const auto badPeerRes = CopyPlaylist(withPurged, badPeerSrc, MakeEntropy(0x13), 200);
	CHECK(badPeerRes.error == PlaylistEditError::InvalidPeerToken);
	CHECK(badPeerRes.library == withPurged);

	Playlist partialMissingOrderAndSongs;
	partialMissingOrderAndSongs.title = PlaylistTitleRegister{ u"Detached"_q, 10 };
	partialMissingOrderAndSongs.deleted = PlaylistDeletedRegister{ false, 10 };
	const auto partialOk = CopyPlaylist(withPurged, partialMissingOrderAndSongs, MakeEntropy(0x14), 200);
	CHECK(partialOk);
	CHECK(partialOk.addedCount == 0);

	QString noteEmoji;
	noteEmoji.append(QChar(0xD83C));
	noteEmoji.append(QChar(0xDFB5));
	Playlist longTitleExactFit = validSrc;
	longTitleExactFit.title->value = QString(56, u'a') + noteEmoji + QString(5, u'z');
	const auto fitCopy = CopyPlaylist(withPurged, longTitleExactFit, MakeEntropy(0x15), 200);
	CHECK(fitCopy);
	CHECK(fitCopy.library.playlists.at(fitCopy.playlistId).title->value
		== (QString(56, u'a') + noteEmoji + u" (copy)"_q));

	Playlist longTitleDropSplit = validSrc;
	longTitleDropSplit.title->value = QString(56, u'a') + QChar(u'b') + noteEmoji + u"z"_q;
	const auto dropCopy = CopyPlaylist(withPurged, longTitleDropSplit, MakeEntropy(0x16), 200);
	CHECK(dropCopy);
	CHECK(dropCopy.library.playlists.at(dropCopy.playlistId).title->value
		== (QString(56, u'a') + QChar(u'b') + u" (copy)"_q));

	Playlist twoSongsHighStamp = validSrc;
	twoSongsHighStamp.title->stamp = kMaximumPlaylistStamp;
	twoSongsHighStamp.deleted->stamp = kMaximumPlaylistStamp;
	twoSongsHighStamp.order->stamp = kMaximumPlaylistStamp;
	twoSongsHighStamp.songs = std::map<uint64_t, PlaylistSongRegister>{
		{ 10, PlaylistSongRegister{ PlaylistSongValue{ PlaylistSource{
			.peer = PeerToken{ PeerTokenKind::Chat, 1 },
			.msg = 1,
			.since = kMaximumPlaylistStamp,
		} }, kMaximumPlaylistStamp } },
		{ 20, PlaylistSongRegister{ PlaylistSongValue{ PlaylistSource{
			.peer = PeerToken{ PeerTokenKind::Chat, 2 },
			.msg = 2,
			.since = kMaximumPlaylistStamp,
		} }, kMaximumPlaylistStamp } },
	};

	const auto independentClock = CopyPlaylist(withPurged, twoSongsHighStamp, MakeEntropy(0x17), 500);
	CHECK(independentClock);
	CHECK(independentClock.library.playlists.at(independentClock.playlistId).title->stamp == 500);
	CHECK(independentClock.library.playlists.at(independentClock.playlistId).songs->at(10).stamp == 501);
	CHECK(independentClock.library.playlists.at(independentClock.playlistId).songs->at(20).stamp == 502);

	const auto exactMax = CopyPlaylist(
		withPurged,
		twoSongsHighStamp,
		MakeEntropy(0x18),
		kMaximumPlaylistStamp - 2);
	CHECK(exactMax);
	CHECK(exactMax.library.playlists.at(exactMax.playlistId).songs->at(10).stamp == kMaximumPlaylistStamp - 1);
	CHECK(exactMax.library.playlists.at(exactMax.playlistId).songs->at(20).stamp == kMaximumPlaylistStamp);

	const auto overflow = CopyPlaylist(
		withPurged,
		twoSongsHighStamp,
		MakeEntropy(0x19),
		kMaximumPlaylistStamp - 1);
	CHECK(overflow.error == PlaylistEditError::StampOverflow);
	CHECK(!overflow.changed);
	CHECK(overflow.library == withPurged);
}

void TestCopyUndoConflictAndNoopMerge() {
	const uint64_t selfId = 77;
	PlaylistLibrary base;
	const auto s = CreatePlaylist(base, u"Source"_q, MakeEntropy(0x21), 100, selfId);
	const auto added = AddPlaylistSongs(
		s.library,
		s.playlistId,
		{
			{ 1, PeerToken{ PeerTokenKind::User, selfId }, 10, QString(), { u"One"_q, u"Art"_q, 100 } },
			{ 2, PeerToken{ PeerTokenKind::Chat, 5 }, 20, QString(), { u"Two"_q, u"Art"_q, 120 } },
		},
		110,
		selfId);
	const auto src = added.library.playlists.at(s.playlistId);

	const auto copied = CopyPlaylist(added.library, src, MakeEntropy(0x22), 300, selfId);
	CHECK(copied);
	const auto copyId = copied.playlistId;

	auto unnormalizedRemote = copied.library;
	unnormalizedRemote.playlists.at(copyId).songs->at(1).value.source->peer = PeerToken{ PeerTokenKind::User, selfId };
	const auto merged = JoinPlaylistLibraries(copied.library, unnormalizedRemote, selfId);
	CHECK(merged);
	const auto undoAfterNoopMerge = UndoPlaylistOperation(merged.library, copied.undo, 400, selfId);
	CHECK(undoAfterNoopMerge);
	CHECK(!undoAfterNoopMerge.library.playlists.contains(copyId));
	CHECK(undoAfterNoopMerge.library.purged.contains(copyId));
	CHECK(undoAfterNoopMerge.library.playlists.at(s.playlistId) == src);

	const auto repeatUndo = UndoPlaylistOperation(undoAfterNoopMerge.library, copied.undo, 405, selfId);
	CHECK(repeatUndo.error == PlaylistEditError::PlaylistPurged);

	const auto renamed = RenamePlaylist(copied.library, copyId, u"Edited Copy"_q, 310, selfId);
	CHECK(UndoPlaylistOperation(renamed.library, copied.undo, 320, selfId).error
		== PlaylistEditError::UndoConflict);

	const auto addedMore = AddPlaylistSongs(
		copied.library,
		copyId,
		{ { 3, PeerToken{ PeerTokenKind::Chat, 6 }, 30, QString(), { u"Three"_q, u"Art"_q, 90 } } },
		310,
		selfId);
	CHECK(UndoPlaylistOperation(addedMore.library, copied.undo, 320, selfId).error
		== PlaylistEditError::UndoConflict);

	const auto removedSong = RemovePlaylistSongs(copied.library, copyId, { 1 }, 310, selfId);
	CHECK(UndoPlaylistOperation(removedSong.library, copied.undo, 320, selfId).error
		== PlaylistEditError::UndoConflict);

	const auto reordered = ReorderPlaylistSongs(copied.library, copyId, { 2, 1 }, 310, selfId);
	CHECK(UndoPlaylistOperation(reordered.library, copied.undo, 320, selfId).error
		== PlaylistEditError::UndoConflict);

	const auto deletedCopy = DeletePlaylist(copied.library, copyId, 310, selfId);
	CHECK(UndoPlaylistOperation(deletedCopy.library, copied.undo, 320, selfId).error
		== PlaylistEditError::UndoConflict);

	PlaylistLibrary stampBump;
	auto bumpedCopy = copied.library.playlists.at(copyId);
	bumpedCopy.title->stamp = 301;
	stampBump.playlists.emplace(copyId, std::move(bumpedCopy));
	const auto mergedBump = JoinPlaylistLibraries(copied.library, stampBump, selfId);
	CHECK(UndoPlaylistOperation(mergedBump.library, copied.undo, 320, selfId).error
		== PlaylistEditError::UndoConflict);
}

void TestCopySizeCapAtomicity() {
	PlaylistLibrary lib;
	const auto huge = CreatePlaylist(lib, u"Huge"_q, MakeEntropy(0x31), 100);
	const auto srcCreated = CreatePlaylist(huge.library, u"Medium Src"_q, MakeEntropy(0x32), 100);
	lib = srcCreated.library;

	const QString longText(96, u'k');
	auto &srcSongs = *lib.playlists.at(srcCreated.playlistId).songs;
	for (uint64_t i = 1; i <= 500; ++i) {
		srcSongs.emplace(i, PlaylistSongRegister{
			PlaylistSongValue{ PlaylistSource{
				.peer = PeerToken{ PeerTokenKind::Chat, 1 },
				.msg = int32_t(i),
				.username = QString(),
				.alt = {},
				.meta = PlaylistSongMeta{ longText, longText, 180 },
				.since = 100,
			} },
			100,
		});
	}

	auto &hugeSongs = *lib.playlists.at(huge.playlistId).songs;
	for (uint64_t i = 1; i <= 75000; ++i) {
		hugeSongs.emplace(1000000ULL + i, PlaylistSongRegister{
			PlaylistSongValue{ PlaylistSource{
				.peer = PeerToken{ PeerTokenKind::Chat, 2 },
				.msg = int32_t(i),
				.username = QString(),
				.alt = {},
				.meta = PlaylistSongMeta{ longText, longText, 200 },
				.since = 100,
			} },
			100,
		});
	}

	auto before = SerializePlaylistLibrary(lib);
	CHECK(before);
	uint64_t nextDoc = 1075001ULL;
	while (before
		&& before.canonical.size() < kPlaylistLocalEditMaximumBytes
		&& (kPlaylistLocalEditMaximumBytes - before.canonical.size()) > 50000) {
		const auto addCount = qsizetype((kPlaylistLocalEditMaximumBytes - before.canonical.size() - 20000) / 310) + 1;
		for (qsizetype k = 0; k < addCount; ++k) {
			hugeSongs.emplace(nextDoc, PlaylistSongRegister{
				PlaylistSongValue{ PlaylistSource{
					.peer = PeerToken{ PeerTokenKind::Chat, 2 },
					.msg = int32_t(nextDoc - 1000000ULL),
					.username = QString(),
					.alt = {},
					.meta = PlaylistSongMeta{ longText, longText, 200 },
					.since = 100,
				} },
				100,
			});
			++nextDoc;
		}
		before = SerializePlaylistLibrary(lib);
		CHECK(before);
	}
	CHECK(before.canonical.size() < kPlaylistLocalEditMaximumBytes);
	CHECK(kPlaylistLocalEditMaximumBytes - before.canonical.size() <= 50000);

	const auto full = CopyPlaylist(
		lib,
		lib.playlists.at(srcCreated.playlistId),
		MakeEntropy(0x33),
		500);
	CHECK(full.error == PlaylistEditError::LibraryFull);
	CHECK(!full.changed);
	CHECK(full.library.playlists.size() == 2);
	CHECK(!full.library.playlists.contains(*FormatPlaylistId(MakeEntropy(0x33))));
}

} // namespace

int main() {
	TestEmptyAndRichDisplayOrderCopy();
	TestRefusalsTitleTruncationAndStamps();
	TestCopyUndoConflictAndNoopMerge();
	TestCopySizeCapAtomicity();

	if (Failures > 0) {
		std::printf("FAILED %d/%d checks\n", Failures, Checks);
		return 1;
	}
	std::printf("PASSED %d checks\n", Checks);
	return 0;
}
