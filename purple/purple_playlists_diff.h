/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_playlists.h"

#include <QtCore/QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Purple {

enum class PlaylistDiffSide {
	None,
	Before,
	After,
	SelfUserId,
};

enum class PlaylistDiffPresence {
	Absent,
	Visible,
	Deleted,
	Incomplete,
	Purged,
};

[[nodiscard]] inline bool IsPresentPlaylistPresence(
		PlaylistDiffPresence presence) {
	return presence == PlaylistDiffPresence::Visible
		|| presence == PlaylistDiffPresence::Deleted
		|| presence == PlaylistDiffPresence::Incomplete;
}

enum class PlaylistSongRegisterState {
	Absent,
	Present,
	Tombstone,
};

struct PlaylistSongRegisterChange {
	uint64_t documentId = 0;
	PlaylistSongRegisterState beforeState = PlaylistSongRegisterState::Absent;
	PlaylistSongRegisterState afterState = PlaylistSongRegisterState::Absent;
	uint64_t beforeStamp = 0;
	uint64_t afterStamp = 0;

	[[nodiscard]] bool operator==(const PlaylistSongRegisterChange &) const = default;
};

struct PlaylistSongValueChange {
	uint64_t documentId = 0;
	PlaylistSource beforeSource;
	PlaylistSource afterSource;
	bool primarySourceChanged = false;
	bool primaryUsernameChanged = false;
	bool fallbacksChanged = false;
	bool metaChanged = false;
	bool sinceChanged = false;

	[[nodiscard]] bool relinked() const {
		return primarySourceChanged;
	}

	[[nodiscard]] bool operator==(const PlaylistSongValueChange &) const = default;
};

struct PlaylistDiffEntry {
	QString playlistId;
	PlaylistDiffPresence beforePresence = PlaylistDiffPresence::Absent;
	PlaylistDiffPresence afterPresence = PlaylistDiffPresence::Absent;
	std::optional<QString> beforeTitle;
	std::optional<QString> afterTitle;
	bool titleChanged = false;
	std::optional<bool> beforeDeleted;
	std::optional<bool> afterDeleted;
	bool deletedChanged = false;
	std::vector<uint64_t> beforeVisibleSongOrder;
	std::vector<uint64_t> afterVisibleSongOrder;
	bool visibleOrderChanged = false;
	std::vector<uint64_t> addedSongIds;
	std::vector<uint64_t> removedSongIds;
	std::vector<PlaylistSongValueChange> changedSongs;

	[[nodiscard]] bool isBeforeVisible() const {
		return beforePresence == PlaylistDiffPresence::Visible;
	}

	[[nodiscard]] bool isAfterVisible() const {
		return afterPresence == PlaylistDiffPresence::Visible;
	}

	[[nodiscard]] bool isBeforePurged() const {
		return beforePresence == PlaylistDiffPresence::Purged;
	}

	[[nodiscard]] bool isAfterPurged() const {
		return afterPresence == PlaylistDiffPresence::Purged;
	}

	[[nodiscard]] bool operator==(const PlaylistDiffEntry &) const = default;
};

struct PlaylistRegisterDiffEntry {
	QString playlistId;
	PlaylistDiffPresence beforePresence = PlaylistDiffPresence::Absent;
	PlaylistDiffPresence afterPresence = PlaylistDiffPresence::Absent;
	bool titleRegisterChanged = false;
	bool deletedRegisterChanged = false;
	bool orderRegisterChanged = false;
	bool songsContainerChanged = false;
	std::vector<PlaylistSongRegisterChange> songRegisterChanges;

	[[nodiscard]] bool operator==(const PlaylistRegisterDiffEntry &) const = default;
};

enum class PlaylistNoticeReason {
	DeletedElsewhere,
	PurgedElsewhere,
};

enum class PlaylistNoticeAction {
	Restore,
	KeepCopy,
};

struct PlaylistNotice {
	QString playlistId;
	QString title;
	PlaylistNoticeReason reason = PlaylistNoticeReason::DeletedElsewhere;
	PlaylistNoticeAction suggestedAction = PlaylistNoticeAction::Restore;
	Playlist preMergePlaylist;

	[[nodiscard]] bool operator==(const PlaylistNotice &) const = default;
};

struct PlaylistDiffSummary {
	std::size_t changedPlaylists = 0;
	std::size_t newPlaylists = 0;
	std::size_t deletedPlaylists = 0;
	std::size_t restoredPlaylists = 0;
	std::size_t purgedPlaylists = 0;
	std::size_t renamedPlaylists = 0;
	std::size_t reorderedPlaylists = 0;
	std::size_t addedSongs = 0;
	std::size_t removedSongs = 0;
	std::size_t changedSongs = 0;
	std::size_t relinkedSongs = 0;
	std::size_t fallbackChangedSongs = 0;
	std::size_t metaChangedSongs = 0;
	std::size_t sinceChangedSongs = 0;
	std::size_t notices = 0;
	std::size_t deletedNotices = 0;
	std::size_t purgedNotices = 0;

	[[nodiscard]] bool operator==(const PlaylistDiffSummary &) const = default;
};

struct PlaylistLibraryDiffResult {
	PlaylistLibraryStatus status = PlaylistLibraryStatus::Invalid;
	PlaylistLibraryError error = PlaylistLibraryError::None;
	PlaylistDiffSide side = PlaylistDiffSide::None;
	std::vector<PlaylistDiffEntry> changes;
	std::vector<PlaylistRegisterDiffEntry> registerChanges;
	std::vector<PlaylistNotice> notices;
	PlaylistDiffSummary summary;

	[[nodiscard]] bool valid() const {
		return status == PlaylistLibraryStatus::Valid;
	}

	[[nodiscard]] explicit operator bool() const {
		return valid();
	}

	[[nodiscard]] bool hasContentChanges() const {
		return !changes.empty();
	}

	[[nodiscard]] bool hasStoredRegisterChanges() const {
		return !registerChanges.empty();
	}

	[[nodiscard]] bool operator==(const PlaylistLibraryDiffResult &) const = default;
};

[[nodiscard]] PlaylistLibraryDiffResult DiffPlaylistLibraries(
	const PlaylistLibrary &before,
	const PlaylistLibrary &after,
	uint64_t selfUserId = 0);

} // namespace Purple
