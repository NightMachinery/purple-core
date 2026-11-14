/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists_diff.h"

#include "purple/purple_playlists_ops.h"

#include <set>
#include <utility>

namespace Purple {
namespace {

[[nodiscard]] PlaylistLibraryDiffResult FailedDiff(
		PlaylistLibraryStatus status,
		PlaylistLibraryError error,
		PlaylistDiffSide side) {
	return { status, error, side, {}, {}, {}, {} };
}

[[nodiscard]] PeerToken NormalizePeer(PeerToken peer, uint64_t selfUserId) {
	if (selfUserId > 0
		&& peer.kind == PeerTokenKind::User
		&& peer.id == selfUserId) {
		return PeerToken();
	}
	return peer;
}

[[nodiscard]] PlaylistSource NormalizeSource(
		PlaylistSource source,
		uint64_t selfUserId) {
	source.peer = NormalizePeer(source.peer, selfUserId);
	for (auto &alt : source.alt) {
		alt.peer = NormalizePeer(alt.peer, selfUserId);
	}
	return source;
}

[[nodiscard]] Playlist NormalizePlaylistPeers(
		Playlist playlist,
		uint64_t selfUserId) {
	if (playlist.songs) {
		for (auto &[docId, reg] : *playlist.songs) {
			if (reg.value.source) {
				reg.value.source = NormalizeSource(
					std::move(*reg.value.source),
					selfUserId);
			}
		}
	}
	return playlist;
}

[[nodiscard]] PlaylistDiffPresence ClassifyPresence(
		const PlaylistLibrary &library,
		const QString &playlistId,
		const Playlist *&outPlaylist) {
	outPlaylist = nullptr;
	if (library.purged.contains(playlistId)) {
		return PlaylistDiffPresence::Purged;
	}
	const auto it = library.playlists.find(playlistId);
	if (it == library.playlists.end()) {
		return PlaylistDiffPresence::Absent;
	}
	outPlaylist = &it->second;
	if (!outPlaylist->title || !outPlaylist->deleted) {
		return PlaylistDiffPresence::Incomplete;
	}
	return outPlaylist->deleted->value
		? PlaylistDiffPresence::Deleted
		: PlaylistDiffPresence::Visible;
}

[[nodiscard]] PlaylistSongRegisterState ClassifySongRegister(
		const PlaylistSongRegister *reg) {
	if (!reg) {
		return PlaylistSongRegisterState::Absent;
	}
	return reg->value.source
		? PlaylistSongRegisterState::Present
		: PlaylistSongRegisterState::Tombstone;
}

[[nodiscard]] PlaylistSongValueChange BuildSongValueChange(
		uint64_t documentId,
		const PlaylistSource &rawBefore,
		const PlaylistSource &rawAfter,
		uint64_t selfUserId) {
	auto beforeSource = NormalizeSource(rawBefore, selfUserId);
	auto afterSource = NormalizeSource(rawAfter, selfUserId);
	const auto primaryChanged = (beforeSource.peer != afterSource.peer)
		|| (beforeSource.msg != afterSource.msg);
	const auto usernameChanged = (beforeSource.username != afterSource.username);
	const auto fallbacksChanged = (beforeSource.alt != afterSource.alt);
	const auto metaChanged = (beforeSource.meta != afterSource.meta);
	const auto sinceChanged = (beforeSource.since != afterSource.since);
	return {
		documentId,
		std::move(beforeSource),
		std::move(afterSource),
		primaryChanged,
		usernameChanged,
		fallbacksChanged,
		metaChanged,
		sinceChanged,
	};
}

void AccumulateSummary(
		PlaylistDiffSummary &summary,
		const PlaylistDiffEntry &entry) {
	++summary.changedPlaylists;
	if (entry.beforePresence == PlaylistDiffPresence::Absent
		&& IsPresentPlaylistPresence(entry.afterPresence)) {
		++summary.newPlaylists;
	}
	if (entry.beforePresence != PlaylistDiffPresence::Deleted
		&& entry.afterPresence == PlaylistDiffPresence::Deleted) {
		++summary.deletedPlaylists;
	}
	if (entry.beforePresence == PlaylistDiffPresence::Deleted
		&& entry.afterPresence == PlaylistDiffPresence::Visible) {
		++summary.restoredPlaylists;
	}
	if (entry.beforePresence != PlaylistDiffPresence::Purged
		&& entry.afterPresence == PlaylistDiffPresence::Purged) {
		++summary.purgedPlaylists;
	}
	if (entry.beforeTitle && entry.afterTitle && entry.titleChanged) {
		++summary.renamedPlaylists;
	}
	if (entry.visibleOrderChanged) {
		++summary.reorderedPlaylists;
	}
	summary.addedSongs += entry.addedSongIds.size();
	summary.removedSongs += entry.removedSongIds.size();
	summary.changedSongs += entry.changedSongs.size();
	for (const auto &song : entry.changedSongs) {
		if (song.primarySourceChanged) {
			++summary.relinkedSongs;
		}
		if (song.fallbacksChanged) {
			++summary.fallbackChangedSongs;
		}
		if (song.metaChanged) {
			++summary.metaChangedSongs;
		}
		if (song.sinceChanged) {
			++summary.sinceChangedSongs;
		}
	}
}

void DiffSongs(
		const Playlist *beforePl,
		const Playlist *afterPl,
		uint64_t selfUserId,
		PlaylistDiffEntry &contentEntry,
		PlaylistRegisterDiffEntry &registerEntry) {
	const auto beforeHasSongs = (beforePl && beforePl->songs);
	const auto afterHasSongs = (afterPl && afterPl->songs);
	registerEntry.songsContainerChanged = (beforeHasSongs != afterHasSongs);
	if (!beforeHasSongs && !afterHasSongs) {
		return;
	}
	static const auto kEmptySongs = std::map<uint64_t, PlaylistSongRegister>{};
	const auto &beforeSongs = beforeHasSongs ? *beforePl->songs : kEmptySongs;
	const auto &afterSongs = afterHasSongs ? *afterPl->songs : kEmptySongs;

	auto itB = beforeSongs.begin();
	auto itA = afterSongs.begin();
	while (itB != beforeSongs.end() || itA != afterSongs.end()) {
		const auto hasB = (itB != beforeSongs.end());
		const auto hasA = (itA != afterSongs.end());
		const auto docId = (hasB && (!hasA || itB->first <= itA->first))
			? itB->first
			: itA->first;
		const PlaylistSongRegister *regB = (hasB && itB->first == docId)
			? &itB->second
			: nullptr;
		const PlaylistSongRegister *regA = (hasA && itA->first == docId)
			? &itA->second
			: nullptr;
		if (regB) {
			++itB;
		}
		if (regA) {
			++itA;
		}

		const auto stateB = ClassifySongRegister(regB);
		const auto stateA = ClassifySongRegister(regA);
		if (stateB != PlaylistSongRegisterState::Present
			&& stateA == PlaylistSongRegisterState::Present) {
			contentEntry.addedSongIds.push_back(docId);
		} else if (stateB == PlaylistSongRegisterState::Present
			&& stateA != PlaylistSongRegisterState::Present) {
			contentEntry.removedSongIds.push_back(docId);
		} else if (stateB == PlaylistSongRegisterState::Present
			&& stateA == PlaylistSongRegisterState::Present) {
			auto valueChange = BuildSongValueChange(
				docId,
				*regB->value.source,
				*regA->value.source,
				selfUserId);
			const auto valueChanged = valueChange.primarySourceChanged
				|| valueChange.primaryUsernameChanged
				|| valueChange.fallbacksChanged
				|| valueChange.metaChanged
				|| valueChange.sinceChanged;
			if (valueChanged) {
				contentEntry.changedSongs.push_back(std::move(valueChange));
			}
			if (valueChanged || regB->stamp != regA->stamp) {
				registerEntry.songRegisterChanges.push_back({
					docId,
					stateB,
					stateA,
					regB->stamp,
					regA->stamp,
				});
			}
			continue;
		}

		if (stateB != stateA || (regB && regA && regB->stamp != regA->stamp)) {
			registerEntry.songRegisterChanges.push_back({
				docId,
				stateB,
				stateA,
				regB ? regB->stamp : 0,
				regA ? regA->stamp : 0,
			});
		}
	}
}

[[nodiscard]] std::optional<PlaylistNotice> MaybeBuildNotice(
		const QString &playlistId,
		PlaylistDiffPresence beforePresence,
		const Playlist *beforePl,
		PlaylistDiffPresence afterPresence,
		const Playlist *afterPl,
		uint64_t selfUserId) {
	if (beforePresence != PlaylistDiffPresence::Visible || !beforePl) {
		return std::nullopt;
	}
	if (afterPresence == PlaylistDiffPresence::Purged) {
		return PlaylistNotice{
			playlistId,
			beforePl->title->value,
			PlaylistNoticeReason::PurgedElsewhere,
			PlaylistNoticeAction::KeepCopy,
			NormalizePlaylistPeers(*beforePl, selfUserId),
		};
	}
	// An incomplete post-merge entry with an explicit deleted=true register still hides the playlist.
	if (afterPresence == PlaylistDiffPresence::Deleted
		|| (afterPl && afterPl->deleted && afterPl->deleted->value)) {
		return PlaylistNotice{
			playlistId,
			beforePl->title->value,
			PlaylistNoticeReason::DeletedElsewhere,
			PlaylistNoticeAction::Restore,
			NormalizePlaylistPeers(*beforePl, selfUserId),
		};
	}
	return std::nullopt;
}

} // namespace

PlaylistLibraryDiffResult DiffPlaylistLibraries(
		const PlaylistLibrary &before,
		const PlaylistLibrary &after,
		uint64_t selfUserId) {
	const auto validBefore = ValidatePlaylistLibrary(before, selfUserId);
	if (!validBefore) {
		const auto side = (validBefore.error == PlaylistLibraryError::InvalidSelfUserId)
			? PlaylistDiffSide::SelfUserId
			: PlaylistDiffSide::Before;
		return FailedDiff(validBefore.status, validBefore.error, side);
	}
	const auto validAfter = ValidatePlaylistLibrary(after, selfUserId);
	if (!validAfter) {
		return FailedDiff(
			validAfter.status,
			validAfter.error,
			PlaylistDiffSide::After);
	}

	auto allIds = std::set<QString>();
	allIds.insert(before.purged.begin(), before.purged.end());
	allIds.insert(after.purged.begin(), after.purged.end());
	for (const auto &[plId, playlist] : before.playlists) {
		allIds.insert(plId);
	}
	for (const auto &[plId, playlist] : after.playlists) {
		allIds.insert(plId);
	}

	auto result = PlaylistLibraryDiffResult();
	result.status = PlaylistLibraryStatus::Valid;

	for (const auto &plId : allIds) {
		const Playlist *beforePl = nullptr;
		const Playlist *afterPl = nullptr;
		const auto beforePresence = ClassifyPresence(before, plId, beforePl);
		const auto afterPresence = ClassifyPresence(after, plId, afterPl);

		auto contentEntry = PlaylistDiffEntry();
		contentEntry.playlistId = plId;
		contentEntry.beforePresence = beforePresence;
		contentEntry.afterPresence = afterPresence;

		const auto beforeTitleReg = beforePl ? beforePl->title : std::nullopt;
		const auto afterTitleReg = afterPl ? afterPl->title : std::nullopt;
		if (beforeTitleReg) {
			contentEntry.beforeTitle = beforeTitleReg->value;
		}
		if (afterTitleReg) {
			contentEntry.afterTitle = afterTitleReg->value;
		}
		contentEntry.titleChanged = (contentEntry.beforeTitle != contentEntry.afterTitle);

		const auto beforeDeletedReg = beforePl ? beforePl->deleted : std::nullopt;
		const auto afterDeletedReg = afterPl ? afterPl->deleted : std::nullopt;
		if (beforeDeletedReg) {
			contentEntry.beforeDeleted = beforeDeletedReg->value;
		}
		if (afterDeletedReg) {
			contentEntry.afterDeleted = afterDeletedReg->value;
		}
		contentEntry.deletedChanged = (contentEntry.beforeDeleted != contentEntry.afterDeleted);

		contentEntry.beforeVisibleSongOrder = beforePl
			? PlaylistVisibleSongIds(*beforePl)
			: std::vector<uint64_t>{};
		contentEntry.afterVisibleSongOrder = afterPl
			? PlaylistVisibleSongIds(*afterPl)
			: std::vector<uint64_t>{};
		contentEntry.visibleOrderChanged =
			(contentEntry.beforeVisibleSongOrder != contentEntry.afterVisibleSongOrder);

		auto registerEntry = PlaylistRegisterDiffEntry();
		registerEntry.playlistId = plId;
		registerEntry.beforePresence = beforePresence;
		registerEntry.afterPresence = afterPresence;
		registerEntry.titleRegisterChanged = (beforeTitleReg != afterTitleReg);
		registerEntry.deletedRegisterChanged = (beforeDeletedReg != afterDeletedReg);
		registerEntry.orderRegisterChanged =
			((beforePl ? beforePl->order : std::nullopt)
				!= (afterPl ? afterPl->order : std::nullopt));

		DiffSongs(beforePl, afterPl, selfUserId, contentEntry, registerEntry);

		const auto hasContentChange =
			(beforePresence != afterPresence)
			|| contentEntry.titleChanged
			|| contentEntry.deletedChanged
			|| contentEntry.visibleOrderChanged
			|| !contentEntry.addedSongIds.empty()
			|| !contentEntry.removedSongIds.empty()
			|| !contentEntry.changedSongs.empty();
		if (hasContentChange) {
			AccumulateSummary(result.summary, contentEntry);
			result.changes.push_back(std::move(contentEntry));
		}

		const auto hasRegisterChange =
			(beforePresence != afterPresence)
			|| registerEntry.titleRegisterChanged
			|| registerEntry.deletedRegisterChanged
			|| registerEntry.orderRegisterChanged
			|| registerEntry.songsContainerChanged
			|| !registerEntry.songRegisterChanges.empty();
		if (hasRegisterChange) {
			result.registerChanges.push_back(std::move(registerEntry));
		}

		if (auto notice = MaybeBuildNotice(
				plId,
				beforePresence,
				beforePl,
				afterPresence,
				afterPl,
				selfUserId)) {
			++result.summary.notices;
			if (notice->reason == PlaylistNoticeReason::PurgedElsewhere) {
				++result.summary.purgedNotices;
			} else {
				++result.summary.deletedNotices;
			}
			result.notices.push_back(std::move(*notice));
		}
	}

	return result;
}

} // namespace Purple
