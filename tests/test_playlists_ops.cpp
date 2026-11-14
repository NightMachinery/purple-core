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
#include <set>
#include <vector>

using namespace Purple;
using namespace Qt::StringLiterals;

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

PeerToken SelfPeer() {
	return PeerToken{ PeerTokenKind::Self, 0 };
}

PeerToken UserPeer(uint64_t id) {
	return PeerToken{ PeerTokenKind::User, id };
}

PeerToken ChatPeer(uint64_t id) {
	return PeerToken{ PeerTokenKind::Chat, id };
}

PeerToken ChannelPeer(uint64_t id) {
	return PeerToken{ PeerTokenKind::Channel, id };
}

PlaylistAddSongInput MakeSongInput(
		uint64_t docId,
		PeerToken peer,
		int32_t msg,
		const QString &title = u"Track"_s,
		const QString &performer = u"Artist"_s,
		int32_t duration = 180,
		const QString &username = QString()) {
	return PlaylistAddSongInput{
		.documentId = docId,
		.peer = peer,
		.msg = msg,
		.username = username,
		.meta = PlaylistSongMeta{
			.title = title,
			.performer = performer,
			.duration = duration,
		},
	};
}

QByteArray MakeEntropy(uint8_t fill) {
	return QByteArray(int(kPlaylistIdEntropyBytes), char(fill));
}

QByteArray CanonicalBytes(
		const PlaylistLibrary &library,
		uint64_t selfUserId = 0) {
	const auto serialized = SerializePlaylistLibrary(library, selfUserId);
	return serialized ? serialized.canonical : QByteArray();
}

constexpr uint64_t kMaxStamp = uint64_t(1) << 52;

void TestFormatPlaylistId() {
	const auto zeroId = FormatPlaylistId(MakeEntropy(0x00));
	CHECK(zeroId.has_value());
	CHECK(*zeroId == u"pl-aaaaaaaaaaaaaaaa"_s);
	CHECK(IsPlaylistId(*zeroId));

	const auto maxId = FormatPlaylistId(MakeEntropy(0xFF));
	CHECK(maxId.has_value());
	CHECK(*maxId == u"pl-7777777777777777"_s);
	CHECK(IsPlaylistId(*maxId));

	const auto sample = QByteArray::fromHex("0123456789abcdef0123");
	const auto first = FormatPlaylistId(sample);
	const auto second = FormatPlaylistId(sample);
	CHECK(first.has_value() && second.has_value());
	CHECK(*first == *second);
	CHECK(IsPlaylistId(*first));

	CHECK(!FormatPlaylistId(QByteArray()).has_value());
	CHECK(!FormatPlaylistId(QByteArray(9, '\0')).has_value());
	CHECK(!FormatPlaylistId(QByteArray(11, '\0')).has_value());
}

void TestCreateAndUndoCreate() {
	PlaylistLibrary lib;
	const auto entropy = MakeEntropy(0x12);
	const auto created = CreatePlaylist(lib, u"Road Trip"_s, entropy, 100);
	CHECK(created);
	CHECK(created.changed);
	CHECK(IsPlaylistId(created.playlistId));
	CHECK(created.undo.kind == PlaylistUndoKind::Create);
	CHECK(created.undo.playlistId == created.playlistId);
	CHECK(created.undo.createdStamp == 100);
	CHECK(created.library.playlists.size() == 1);

	const auto &pl = created.library.playlists.at(created.playlistId);
	CHECK(pl.title.has_value() && pl.title->value == u"Road Trip"_s && pl.title->stamp == 100);
	CHECK(pl.deleted.has_value() && !pl.deleted->value && pl.deleted->stamp == 100);
	CHECK(pl.order.has_value() && pl.order->value.empty() && pl.order->stamp == 100);
	CHECK(pl.songs.has_value() && pl.songs->empty());
	CHECK(ValidatePlaylistLibrary(created.library).status == PlaylistLibraryStatus::Valid);

	const auto collisionActive = CreatePlaylist(
		created.library,
		u"Another"_s,
		entropy,
		200);
	CHECK(collisionActive.error == PlaylistEditError::IdCollision);
	CHECK(!collisionActive.changed);
	CHECK(collisionActive.library == created.library);

	const auto badEntropy = CreatePlaylist(lib, u"X"_s, QByteArray(9, 'a'), 100);
	CHECK(badEntropy.error == PlaylistEditError::InvalidEntropy);

	const auto badTitle = CreatePlaylist(lib, u"   "_s, entropy, 100);
	CHECK(badTitle.error == PlaylistEditError::InvalidTitle);

	const auto badStampZero = CreatePlaylist(lib, u"X"_s, entropy, 0);
	CHECK(badStampZero.error == PlaylistEditError::InvalidStamp);

	const auto badStampHigh = CreatePlaylist(lib, u"X"_s, entropy, kMaxStamp + 1);
	CHECK(badStampHigh.error == PlaylistEditError::InvalidStamp);

	const auto undone = UndoPlaylistOperation(created.library, created.undo, 105);
	CHECK(undone);
	CHECK(undone.changed);
	CHECK(!undone.undo.hasUndo());
	CHECK(!undone.library.playlists.contains(created.playlistId));
	CHECK(undone.library.purged.contains(created.playlistId));

	const auto collisionPurged = CreatePlaylist(
		undone.library,
		u"Reuse"_s,
		entropy,
		300);
	CHECK(collisionPurged.error == PlaylistEditError::IdCollision);

	const auto renamed = RenamePlaylist(created.library, created.playlistId, u"Edited"_s, 110);
	CHECK(renamed);
	const auto undoAfterRename = UndoPlaylistOperation(renamed.library, created.undo, 120);
	CHECK(undoAfterRename.error == PlaylistEditError::UndoConflict);
	CHECK(!undoAfterRename.changed);
	CHECK(undoAfterRename.library == renamed.library);

	const auto added = AddPlaylistSongs(
		created.library,
		created.playlistId,
		{ MakeSongInput(10, ChatPeer(77), 1) },
		110);
	CHECK(added);
	const auto undoAfterAdd = UndoPlaylistOperation(added.library, created.undo, 120);
	CHECK(undoAfterAdd.error == PlaylistEditError::UndoConflict);

	const auto deleted = DeletePlaylist(created.library, created.playlistId, 110);
	CHECK(deleted);
	const auto undoAfterDelete = UndoPlaylistOperation(deleted.library, created.undo, 120);
	CHECK(undoAfterDelete.error == PlaylistEditError::UndoConflict);

	PlaylistLibrary remoteLib;
	remoteLib.playlists.emplace(created.playlistId, Playlist{
		.title = PlaylistTitleRegister{ u"Remote"_s, 101 },
		.deleted = std::nullopt,
		.order = std::nullopt,
		.songs = std::nullopt,
	});
	const auto joined = JoinPlaylistLibraries(created.library, remoteLib);
	CHECK(joined);
	const auto undoAfterMerge = UndoPlaylistOperation(joined.library, created.undo, 120);
	CHECK(undoAfterMerge.error == PlaylistEditError::UndoConflict);
}

void TestLifecycleAndRefusals() {
	PlaylistLibrary lib;
	const auto c = CreatePlaylist(lib, u"Mix"_s, MakeEntropy(0x21), 100);
	CHECK(c);
	const auto id = c.playlistId;

	const auto noopRename = RenamePlaylist(c.library, id, u"Mix"_s, 110);
	CHECK(noopRename);
	CHECK(!noopRename.changed);
	CHECK(!noopRename.undo.hasUndo());
	CHECK(CanonicalBytes(noopRename.library) == CanonicalBytes(c.library));

	const auto untrimmedInvalid = RenamePlaylist(c.library, id, u"  Mix  "_s, 110);
	CHECK(untrimmedInvalid.error == PlaylistEditError::InvalidTitle);
	CHECK(!untrimmedInvalid.changed);

	const auto renamed = RenamePlaylist(c.library, id, u"Night Mix"_s, 120);
	CHECK(renamed);
	CHECK(renamed.changed);
	CHECK(renamed.library.playlists.at(id).title->value == u"Night Mix"_s);
	CHECK(renamed.library.playlists.at(id).title->stamp == 120);

	const auto noopRestore = RestorePlaylist(renamed.library, id, 130);
	CHECK(noopRestore);
	CHECK(!noopRestore.changed);
	CHECK(!noopRestore.undo.hasUndo());

	const auto foreverActive = DeleteForeverPlaylist(renamed.library, id);
	CHECK(foreverActive.error == PlaylistEditError::PlaylistNotDeleted);

	const auto deleted = DeletePlaylist(renamed.library, id, 140);
	CHECK(deleted);
	CHECK(deleted.changed);
	CHECK(deleted.library.playlists.at(id).deleted->value);
	CHECK(deleted.library.playlists.at(id).deleted->stamp == 140);

	const auto noopDelete = DeletePlaylist(deleted.library, id, 150);
	CHECK(noopDelete);
	CHECK(!noopDelete.changed);
	CHECK(!noopDelete.undo.hasUndo());

	CHECK(RenamePlaylist(deleted.library, id, u"X"_s, 160).error == PlaylistEditError::PlaylistDeleted);
	CHECK(AddPlaylistSongs(
		deleted.library,
		id,
		{ MakeSongInput(1, ChatPeer(1), 1) },
		160).error == PlaylistEditError::PlaylistDeleted);
	CHECK(RemovePlaylistSongs(deleted.library, id, { 1 }, 160).error == PlaylistEditError::PlaylistDeleted);
	CHECK(ReorderPlaylistSongs(deleted.library, id, {}, 160).error == PlaylistEditError::PlaylistDeleted);

	const auto restored = RestorePlaylist(deleted.library, id, 170);
	CHECK(restored);
	CHECK(restored.changed);
	CHECK(!restored.library.playlists.at(id).deleted->value);
	CHECK(restored.library.playlists.at(id).deleted->stamp == 170);

	const auto deletedAgain = DeletePlaylist(restored.library, id, 180);
	CHECK(deletedAgain);
	const auto purgedResult = DeleteForeverPlaylist(deletedAgain.library, id);
	CHECK(purgedResult);
	CHECK(purgedResult.changed);
	CHECK(!purgedResult.undo.hasUndo());
	CHECK(!purgedResult.library.playlists.contains(id));
	CHECK(purgedResult.library.purged.contains(id));

	const auto undoNone = UndoPlaylistOperation(purgedResult.library, purgedResult.undo, 190);
	CHECK(undoNone.error == PlaylistEditError::NothingToUndo);

	CHECK(RenamePlaylist(purgedResult.library, id, u"Y"_s, 200).error == PlaylistEditError::PlaylistPurged);
	CHECK(RenamePlaylist(purgedResult.library, u"pl-bbbbbbbbbbbbbbbb"_s, u"Y"_s, 200).error
		== PlaylistEditError::PlaylistNotFound);

	PlaylistLibrary partialLib;
	partialLib.playlists.emplace(id, Playlist{
		.title = PlaylistTitleRegister{ u"Partial"_s, 10 },
		.deleted = std::nullopt,
		.order = std::nullopt,
		.songs = std::nullopt,
	});
	CHECK(RenamePlaylist(partialLib, id, u"Z"_s, 20).error == PlaylistEditError::PlaylistIncomplete);
	CHECK(DeletePlaylist(partialLib, id, 20).error == PlaylistEditError::PlaylistIncomplete);
	CHECK(AddPlaylistSongs(partialLib, id, {}, 20).error == PlaylistEditError::PlaylistIncomplete);
}

void TestAddRelinkRemoveReorderAndUndo() {
	const uint64_t selfId = 42;
	PlaylistLibrary lib;
	const auto c = CreatePlaylist(lib, u"Album"_s, MakeEntropy(0x33), 100, selfId);
	CHECK(c);
	const auto id = c.playlistId;

	const auto dupReq = AddPlaylistSongs(
		c.library,
		id,
		{
			MakeSongInput(500, ChatPeer(10), 1),
			MakeSongInput(500, ChatPeer(10), 2),
		},
		110,
		selfId);
	CHECK(dupReq.error == PlaylistEditError::DuplicateDocumentId);
	CHECK(dupReq.library == c.library);

	const auto batchAdd = AddPlaylistSongs(
		c.library,
		id,
		{
			MakeSongInput(30, UserPeer(selfId), 11, u"First"_s, u"A"_s, 100),
			MakeSongInput(10, ChatPeer(200), 22, u"Second"_s, u"B"_s, 200),
			MakeSongInput(20, ChannelPeer(300), 33, u"Third"_s, u"C"_s, 300),
		},
		120,
		selfId);
	CHECK(batchAdd);
	CHECK(batchAdd.changed);
	CHECK(batchAdd.addedCount == 3);
	CHECK(batchAdd.relinkedCount == 0);
	CHECK(batchAdd.unchangedCount == 0);

	const auto &songsAfterAdd = *batchAdd.library.playlists.at(id).songs;
	CHECK(songsAfterAdd.at(30).stamp == 120);
	CHECK(songsAfterAdd.at(30).value.source->since == 120);
	CHECK(songsAfterAdd.at(30).value.source->peer == SelfPeer());
	CHECK(songsAfterAdd.at(10).stamp == 121);
	CHECK(songsAfterAdd.at(20).stamp == 122);
	CHECK(batchAdd.library.playlists.at(id).order->value.empty());
	CHECK(batchAdd.library.playlists.at(id).order->stamp == 100);
	CHECK((PlaylistVisibleSongIds(batchAdd.library.playlists.at(id)) == std::vector<uint64_t>{ 30, 10, 20 }));

	const auto sameAdd = AddPlaylistSongs(
		batchAdd.library,
		id,
		{
			MakeSongInput(30, SelfPeer(), 11, u"Changed Meta Ignored"_s, u"X"_s, 999),
		},
		130,
		selfId);
	CHECK(sameAdd);
	CHECK(!sameAdd.changed);
	CHECK(!sameAdd.undo.hasUndo());
	CHECK(sameAdd.unchangedCount == 1);
	CHECK(sameAdd.library.playlists.at(id).songs->at(30).value.source->meta.title == u"First"_s);
	CHECK(sameAdd.library.playlists.at(id).songs->at(30).value.source->since == 120);

	auto mixedAdd = AddPlaylistSongs(
		batchAdd.library,
		id,
		{
			MakeSongInput(30, SelfPeer(), 11, u"Ignored"_s, u"X"_s, 999),
			MakeSongInput(40, ChatPeer(400), 44, u"Fourth"_s, u"D"_s, 400),
		},
		130,
		selfId);
	CHECK(mixedAdd);
	CHECK(mixedAdd.changed);
	CHECK(mixedAdd.addedCount == 1);
	CHECK(mixedAdd.unchangedCount == 1);
	CHECK(mixedAdd.undo.priorSongs.size() == 1);
	CHECK(mixedAdd.undo.priorSongs.contains(40));
	CHECK(!mixedAdd.undo.priorSongs.at(40).has_value());
	CHECK(mixedAdd.library.playlists.at(id).songs->at(30).stamp == 120);
	CHECK(mixedAdd.library.playlists.at(id).songs->at(40).stamp == 130);

	const auto undoMixed = UndoPlaylistOperation(mixedAdd.library, mixedAdd.undo, 135, selfId);
	CHECK(undoMixed);
	CHECK(undoMixed.changed);
	CHECK(undoMixed.library.playlists.at(id).songs->at(30).value.source.has_value());
	CHECK(undoMixed.library.playlists.at(id).songs->at(30).stamp == 120);
	CHECK(undoMixed.library.playlists.at(id).songs->at(40).value.isRemoved());
	CHECK(undoMixed.library.playlists.at(id).songs->at(40).stamp == 135);

	auto relink1 = AddPlaylistSongs(
		batchAdd.library,
		id,
		{ MakeSongInput(10, ChatPeer(201), 201, u"Second New"_s, u"B2"_s, 210, u"ch_a"_s) },
		140,
		selfId);
	CHECK(relink1);
	CHECK(relink1.relinkedCount == 1);
	const auto &s10_1 = relink1.library.playlists.at(id).songs->at(10).value.source;
	CHECK(s10_1->since == 121);
	CHECK(s10_1->meta.title == u"Second New"_s);
	CHECK(s10_1->alt.size() == 1);
	CHECK(s10_1->alt.at(0).peer == ChatPeer(200));
	CHECK(s10_1->alt.at(0).msg == 22);

	const auto relink2 = AddPlaylistSongs(
		relink1.library,
		id,
		{ MakeSongInput(10, ChatPeer(202), 202, u"X"_s, u"Y"_s, 220) },
		141,
		selfId);
	const auto relink3 = AddPlaylistSongs(
		relink2.library,
		id,
		{ MakeSongInput(10, ChatPeer(203), 203, u"X"_s, u"Y"_s, 220) },
		142,
		selfId);
	const auto relink4 = AddPlaylistSongs(
		relink3.library,
		id,
		{ MakeSongInput(10, ChatPeer(200), 22, u"Promote Old Alt"_s, u"Z"_s, 230) },
		143,
		selfId);
	CHECK(relink4);
	const auto &s10_4 = relink4.library.playlists.at(id).songs->at(10).value.source;
	CHECK(s10_4->peer == ChatPeer(200));
	CHECK(s10_4->msg == 22);
	CHECK(s10_4->alt.size() == 3);
	CHECK(s10_4->alt.at(0).peer == ChatPeer(203) && s10_4->alt.at(0).msg == 203);
	CHECK(s10_4->alt.at(1).peer == ChatPeer(202) && s10_4->alt.at(1).msg == 202);
	CHECK(s10_4->alt.at(2).peer == ChatPeer(201) && s10_4->alt.at(2).msg == 201);
	for (const auto &alt : s10_4->alt) {
		CHECK(!(alt.peer == s10_4->peer && alt.msg == s10_4->msg));
	}

	const auto undoRelink1 = UndoPlaylistOperation(relink1.library, relink1.undo, 150, selfId);
	CHECK(undoRelink1);
	const auto &restored10 = undoRelink1.library.playlists.at(id).songs->at(10);
	CHECK(restored10.stamp == 150);
	CHECK(restored10.value.source->since == 121);
	CHECK(restored10.value.source->meta.title == u"Second"_s);
	CHECK(restored10.value.source->peer == ChatPeer(200));
	CHECK(restored10.value.source->alt.empty());

	const auto reordered = ReorderPlaylistSongs(
		batchAdd.library,
		id,
		{ 20, 30, 10 },
		160,
		selfId);
	CHECK(reordered);
	CHECK(reordered.changed);
	CHECK((PlaylistVisibleSongIds(reordered.library.playlists.at(id)) == std::vector<uint64_t>{ 20, 30, 10 }));

	const auto sameOrder = ReorderPlaylistSongs(
		reordered.library,
		id,
		{ 20, 30, 10 },
		161,
		selfId);
	CHECK(sameOrder);
	CHECK(!sameOrder.changed);
	CHECK(!sameOrder.undo.hasUndo());

	CHECK(ReorderPlaylistSongs(reordered.library, id, { 20, 30 }, 162, selfId).error
		== PlaylistEditError::InvalidOrder);
	CHECK(ReorderPlaylistSongs(reordered.library, id, { 20, 20, 10 }, 162, selfId).error
		== PlaylistEditError::InvalidOrder);
	CHECK(ReorderPlaylistSongs(reordered.library, id, { 20, 30, 999 }, 162, selfId).error
		== PlaylistEditError::InvalidOrder);

	const auto removed = RemovePlaylistSongs(
		reordered.library,
		id,
		{ 30, 999, 30 },
		170,
		selfId);
	CHECK(removed);
	CHECK(removed.changed);
	CHECK(removed.removedCount == 1);
	CHECK(removed.undo.priorSongs.size() == 1);
	CHECK(removed.undo.priorSongs.contains(30));
	CHECK((PlaylistVisibleSongIds(removed.library.playlists.at(id)) == std::vector<uint64_t>{ 20, 10 }));
	CHECK(ReorderPlaylistSongs(removed.library, id, { 20, 30, 10 }, 171, selfId).error
		== PlaylistEditError::InvalidOrder);

	const auto undoRemove = UndoPlaylistOperation(removed.library, removed.undo, 175, selfId);
	CHECK(undoRemove);
	CHECK((PlaylistVisibleSongIds(undoRemove.library.playlists.at(id)) == std::vector<uint64_t>{ 20, 30, 10 }));
}

void TestConcurrentUndoMerges() {
	PlaylistLibrary lib;
	const auto c = CreatePlaylist(lib, u"Sync"_s, MakeEntropy(0x44), 100);
	const auto id = c.playlistId;
	const auto added = AddPlaylistSongs(
		c.library,
		id,
		{
			MakeSongInput(1, ChatPeer(1), 1, u"One"_s),
			MakeSongInput(2, ChatPeer(2), 2, u"Two"_s),
		},
		110);
	CHECK(added);

	const auto localRelink = AddPlaylistSongs(
		added.library,
		id,
		{ MakeSongInput(1, ChatPeer(9), 9) },
		120);
	const auto remoteReorder = ReorderPlaylistSongs(
		added.library,
		id,
		{ 2, 1 },
		125);
	CHECK(localRelink && remoteReorder);

	const auto merged = JoinPlaylistLibraries(localRelink.library, remoteReorder.library);
	CHECK(merged);
	const auto undone = UndoPlaylistOperation(merged.library, localRelink.undo, 130);
	CHECK(undone);
	CHECK(undone.library.playlists.at(id).songs->at(1).value.source->peer == ChatPeer(1));
	CHECK((PlaylistVisibleSongIds(undone.library.playlists.at(id)) == std::vector<uint64_t>{ 2, 1 }));

	const auto deleted = DeletePlaylist(added.library, id, 140);
	const auto localRename = RenamePlaylist(added.library, id, u"NewName"_s, 135);
	const auto deletedMerged = JoinPlaylistLibraries(deleted.library, localRename.library);
	CHECK(deletedMerged);
	CHECK(UndoPlaylistOperation(deletedMerged.library, localRename.undo, 150).error == PlaylistEditError::PlaylistDeleted);
	const auto undoDelete = UndoPlaylistOperation(deletedMerged.library, deleted.undo, 150);
	CHECK(undoDelete);
	CHECK(!undoDelete.library.playlists.at(id).deleted->value);
}

void TestPartialOrderUndoAndStaleReorder() {
	const auto created = CreatePlaylist({}, u"Ordering"_q, MakeEntropy(0x46), 100);
	const auto id = created.playlistId;
	auto partial = AddPlaylistSongs(created.library, id, {
		MakeSongInput(1, SelfPeer(), 1),
		MakeSongInput(2, SelfPeer(), 2),
	}, 110).library;
	partial.playlists.at(id).order = std::nullopt;
	CHECK(ValidatePlaylistLibrary(partial));
	const auto reordered = ReorderPlaylistSongs(partial, id, { 2, 1 }, 120);
	CHECK(reordered && reordered.changed);
	CHECK(reordered.undo.hasOrderWrite && !reordered.undo.priorOrder);
	const auto undone = UndoPlaylistOperation(reordered.library, reordered.undo, 130);
	CHECK(undone && undone.changed);
	const auto &order = undone.library.playlists.at(id).order;
	CHECK(order.has_value() && order->value.empty() && order->stamp == 130);
	const auto remerged = JoinPlaylistLibraries(undone.library, reordered.library);
	CHECK(remerged);
	CHECK((PlaylistVisibleSongIds(remerged.library.playlists.at(id))
		== std::vector<uint64_t>{ 1, 2 }));
	const auto redone = UndoPlaylistOperation(undone.library, undone.undo, 131);
	CHECK(redone);
	CHECK((PlaylistVisibleSongIds(redone.library.playlists.at(id))
		== std::vector<uint64_t>{ 2, 1 }));

	const auto removed = RemovePlaylistSongs(reordered.library, id, { 2 }, 140);
	const auto readded = AddPlaylistSongs(removed.library, id,
		{ MakeSongInput(2, SelfPeer(), 2) }, 150);
	CHECK(readded);
	CHECK((PlaylistVisibleSongIds(readded.library.playlists.at(id))
		== std::vector<uint64_t>{ 1, 2 }));
	CHECK((readded.library.playlists.at(id).order->value
		== std::vector<uint64_t>{ 2, 1 }));
	const auto requested = ReorderPlaylistSongs(readded.library, id, { 2, 1 }, 160);
	CHECK(requested && requested.changed);
	CHECK(requested.library.playlists.at(id).order->stamp == 160);
	CHECK((PlaylistVisibleSongIds(requested.library.playlists.at(id))
		== std::vector<uint64_t>{ 2, 1 }));
	const auto unchanged = ReorderPlaylistSongs(requested.library, id, { 2, 1 }, 161);
	CHECK(unchanged && !unchanged.changed);
}

void TestStampMonotonicityAndSaturation() {
	PlaylistLibrary lib;
	const auto c = CreatePlaylist(lib, u"Stamps"_s, MakeEntropy(0x55), 500);
	const auto id = c.playlistId;

	const auto behind = RenamePlaylist(c.library, id, u"Stamps2"_s, 100);
	CHECK(behind);
	CHECK(behind.library.playlists.at(id).title->stamp == 501);

	const auto edgeThree = AddPlaylistSongs(
		behind.library,
		id,
		{
			MakeSongInput(1, ChatPeer(1), 1),
			MakeSongInput(2, ChatPeer(2), 2),
			MakeSongInput(3, ChatPeer(3), 3),
		},
		kMaxStamp - 1);
	CHECK(edgeThree.error == PlaylistEditError::StampOverflow);
	CHECK(edgeThree.library == behind.library);

	const auto edgeTwoExact = AddPlaylistSongs(
		behind.library,
		id,
		{
			MakeSongInput(1, ChatPeer(1), 1),
			MakeSongInput(2, ChatPeer(2), 2),
		},
		kMaxStamp - 1);
	CHECK(edgeTwoExact);
	CHECK(edgeTwoExact.library.playlists.at(id).songs->at(1).stamp == kMaxStamp - 1);
	CHECK(edgeTwoExact.library.playlists.at(id).songs->at(2).stamp == kMaxStamp);

	const auto atMax = AddPlaylistSongs(
		behind.library,
		id,
		{ MakeSongInput(1, ChatPeer(1), 1) },
		kMaxStamp);
	CHECK(atMax);
	CHECK(atMax.library.playlists.at(id).songs->at(1).stamp == kMaxStamp);

	CHECK(RenamePlaylist(atMax.library, id, u"Overflow"_s, kMaxStamp).error
		== PlaylistEditError::StampOverflow);

	PlaylistLibrary deletedAtMax = atMax.library;
	deletedAtMax.playlists.at(id).deleted = PlaylistDeletedRegister{ true, kMaxStamp };
	CHECK(RestorePlaylist(deletedAtMax, id, kMaxStamp).error == PlaylistEditError::StampOverflow);
	const auto purgedAtMax = DeleteForeverPlaylist(deletedAtMax, id);
	CHECK(purgedAtMax);
	CHECK(purgedAtMax.library.purged.contains(id));
}

void TestDisplayOrderingAndGroupTwo() {
	PlaylistLibrary lib;
	const auto plA = CreatePlaylist(lib, u"beta"_s, MakeEntropy(0x61), 100);
	const auto plB = CreatePlaylist(plA.library, u"Alpha"_s, MakeEntropy(0x62), 100);
	const auto plC = CreatePlaylist(plB.library, u"Deleted"_s, MakeEntropy(0x63), 100);
	lib = DeletePlaylist(plC.library, plC.playlistId, 110).library;
	lib.playlists.emplace(u"pl-dddddddddddddddd"_s, Playlist{
		.title = PlaylistTitleRegister{ u"Partial"_s, 100 },
		.deleted = std::nullopt,
		.order = std::nullopt,
		.songs = std::nullopt,
	});
	lib = AddPlaylistSongs(
		lib,
		plB.playlistId,
		{
			MakeSongInput(10, ChatPeer(1), 1),
			MakeSongInput(20, ChatPeer(1), 2),
			MakeSongInput(30, ChatPeer(1), 3),
		},
		200).library;
	lib = ReorderPlaylistSongs(lib, plB.playlistId, { 30, 10, 20 }, 210).library;
	lib = RemovePlaylistSongs(lib, plB.playlistId, { 10 }, 220).library;
	lib = AddPlaylistSongs(lib, plB.playlistId, { MakeSongInput(10, ChatPeer(1), 1) }, 230).library;
	lib = AddPlaylistSongs(lib, plB.playlistId, { MakeSongInput(15, ChatPeer(1), 4) }, 230).library;
	lib.playlists.at(plB.playlistId).songs->at(10).value.source->since = 230;
	lib.playlists.at(plB.playlistId).songs->at(15).value.source->since = 230;
	lib = AddPlaylistSongs(
		lib,
		plB.playlistId,
		{ MakeSongInput(30, ChatPeer(9), 99) },
		240).library;

	CHECK((PlaylistVisibleSongIds(lib.playlists.at(plB.playlistId)) == std::vector<uint64_t>{ 30, 20, 10, 15 }));

	auto tieBreakLib = lib;
	const auto plTie1 = CreatePlaylist(tieBreakLib, u"same"_s, MakeEntropy(0x72), 100);
	const auto plTie2 = CreatePlaylist(plTie1.library, u"SAME"_s, MakeEntropy(0x71), 100);
	const auto entries = ListVisiblePlaylists(
		plTie2.library,
		{
			{ plA.playlistId, 5000 },
		});
	CHECK(entries.size() == 4);
	CHECK(entries.at(0).id == plA.playlistId && entries.at(0).visibleSongCount == 0);
	CHECK(entries.at(1).id == plB.playlistId && entries.at(1).visibleSongCount == 4);
	CHECK(entries.at(2).id == plTie2.playlistId);
	CHECK(entries.at(3).id == plTie1.playlistId);
}

void TestLocalEditSizeCap() {
	PlaylistLibrary base;
	const auto created = CreatePlaylist(base, u"Huge"_s, MakeEntropy(0x81), 100);
	const auto hugeId = created.playlistId;
	const auto deletedCreated = CreatePlaylist(created.library, u"ShrinkMe"_s, MakeEntropy(0x82), 100);
	const auto shrinkId = deletedCreated.playlistId;
	base = DeletePlaylist(deletedCreated.library, shrinkId, 110).library;

	const QString longTitle(96, u'a');
	const QString longPerf(96, u'b');
	auto &shrinkSongs = *base.playlists.at(shrinkId).songs;
	for (uint64_t i = 1; i <= 20; ++i) {
		shrinkSongs.emplace(i, PlaylistSongRegister{
			PlaylistSongValue{ PlaylistSource{
				.peer = ChatPeer(1),
				.msg = int32_t(i),
				.username = QString(),
				.alt = {},
				.meta = PlaylistSongMeta{ longTitle, longPerf, 180 },
				.since = 100,
			} },
			100,
		});
	}

	auto &hugeSongs = *base.playlists.at(hugeId).songs;
	const uint64_t initialCount = 82000;
	for (uint64_t i = 1; i <= initialCount; ++i) {
		hugeSongs.emplace(1000000ULL + i, PlaylistSongRegister{
			PlaylistSongValue{ PlaylistSource{
				.peer = ChatPeer(1),
				.msg = int32_t(i),
				.username = QString(),
				.alt = {},
				.meta = PlaylistSongMeta{ longTitle, longPerf, 210 },
				.since = 100,
			} },
			100,
		});
	}

	auto measure = CanonicalBytes(base).size();
	CHECK(measure < kPlaylistLocalEditMaximumBytes);
	uint64_t nextDoc = 1000000ULL + initialCount + 1;
	while (kPlaylistLocalEditMaximumBytes - measure > 1000) {
		const auto addCount = qsizetype((kPlaylistLocalEditMaximumBytes - measure - 500) / 310) + 1;
		for (qsizetype k = 0; k < addCount; ++k) {
			hugeSongs.emplace(nextDoc, PlaylistSongRegister{
				PlaylistSongValue{ PlaylistSource{
					.peer = ChatPeer(1),
					.msg = int32_t(nextDoc - 1000000ULL),
					.username = QString(),
					.alt = {},
					.meta = PlaylistSongMeta{ longTitle, longPerf, 210 },
					.since = 100,
				} },
				100,
			});
			++nextDoc;
		}
		measure = CanonicalBytes(base).size();
	}
	CHECK(measure > kPlaylistLocalEditMaximumBytes - 1000);
	CHECK(measure <= kPlaylistLocalEditMaximumBytes);

	const auto overflowAdd = AddPlaylistSongs(
		base,
		hugeId,
		{
			MakeSongInput(nextDoc, ChatPeer(1), 1, longTitle, longPerf, 210),
			MakeSongInput(nextDoc + 1, ChatPeer(1), 2, longTitle, longPerf, 210),
			MakeSongInput(nextDoc + 2, ChatPeer(1), 3, longTitle, longPerf, 210),
			MakeSongInput(nextDoc + 3, ChatPeer(1), 4, longTitle, longPerf, 210),
		},
		200);
	CHECK(overflowAdd.error == PlaylistEditError::LibraryFull);
	CHECK(!overflowAdd.changed);

	auto oversized = base;
	auto &overSongs = *oversized.playlists.at(hugeId).songs;
	for (uint64_t k = 0; k < 50; ++k) {
		overSongs.emplace(nextDoc + k, PlaylistSongRegister{
			PlaylistSongValue{ PlaylistSource{
				.peer = ChatPeer(1),
				.msg = int32_t(k + 1),
				.username = QString(),
				.alt = {},
				.meta = PlaylistSongMeta{ longTitle, longPerf, 210 },
				.since = 100,
			} },
			100,
		});
	}
	const auto overBytes = CanonicalBytes(oversized).size();
	CHECK(overBytes > kPlaylistLocalEditMaximumBytes);

	const auto overNoop = RenamePlaylist(oversized, hugeId, u"Huge"_s, 250);
	CHECK(overNoop);
	CHECK(!overNoop.changed);

	const auto overBlockedAdd = AddPlaylistSongs(
		oversized,
		hugeId,
		{ MakeSongInput(nextDoc + 999, ChatPeer(1), 999) },
		250);
	CHECK(overBlockedAdd.error == PlaylistEditError::LibraryFull);

	const auto shrunkRemove = RemovePlaylistSongs(oversized, hugeId, { 1000001ULL }, 260);
	CHECK(shrunkRemove);
	CHECK(shrunkRemove.changed);
	const auto afterRemoveBytes = CanonicalBytes(shrunkRemove.library).size();
	CHECK(afterRemoveBytes > kPlaylistLocalEditMaximumBytes);
	CHECK(afterRemoveBytes < overBytes);

	const auto shrunkForever = DeleteForeverPlaylist(oversized, shrinkId);
	CHECK(shrunkForever);
	CHECK(shrunkForever.changed);
	const auto afterForeverBytes = CanonicalBytes(shrunkForever.library).size();
	CHECK(afterForeverBytes > kPlaylistLocalEditMaximumBytes);
	CHECK(afterForeverBytes < overBytes);
}

} // namespace

int main() {
	TestFormatPlaylistId();
	TestCreateAndUndoCreate();
	TestLifecycleAndRefusals();
	TestAddRelinkRemoveReorderAndUndo();
	TestConcurrentUndoMerges();
	TestPartialOrderUndoAndStaleReorder();
	TestStampMonotonicityAndSaturation();
	TestDisplayOrderingAndGroupTwo();
	TestLocalEditSizeCap();

	if (Failures > 0) {
		std::printf("FAILED %d/%d checks\n", Failures, Checks);
		return 1;
	}
	std::printf("PASSED %d checks\n", Checks);
	return 0;
}
