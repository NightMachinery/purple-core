/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_playlists.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace Purple {

inline constexpr auto kPlaylistIdEntropyBytes = qsizetype(10);
inline constexpr auto kPlaylistLocalEditMaximumBytes = qsizetype(24) * 1024 * 1024;

enum class PlaylistEditError {
	None,
	InvalidLibrary,
	NewerFormat,
	InvalidSelfUserId,
	InvalidId,
	InvalidEntropy,
	IdCollision,
	PlaylistNotFound,
	PlaylistPurged,
	PlaylistIncomplete,
	PlaylistDeleted,
	PlaylistNotDeleted,
	InvalidTitle,
	InvalidDocumentId,
	DuplicateDocumentId,
	InvalidPeerToken,
	InvalidUsername,
	InvalidMeta,
	InvalidValue,
	InvalidOrder,
	InvalidStamp,
	StampOverflow,
	LibraryFull,
	NothingToUndo,
	UndoConflict,
};

enum class PlaylistUndoKind {
	None,
	Create,
	RegisterWrites,
	Copy,
};

struct PlaylistUndoRecord {
	PlaylistUndoKind kind = PlaylistUndoKind::None;
	QString playlistId;
	QString createdTitle;
	uint64_t createdStamp = 0;
	bool hasTitleWrite = false;
	std::optional<PlaylistTitleRegister> priorTitle;
	bool hasDeletedWrite = false;
	std::optional<PlaylistDeletedRegister> priorDeleted;
	bool hasOrderWrite = false;
	std::optional<PlaylistOrderRegister> priorOrder;
	std::map<uint64_t, std::optional<PlaylistSongRegister>> priorSongs;
	std::optional<Playlist> expectedCopiedPlaylist;

	[[nodiscard]] bool hasUndo() const {
		return kind != PlaylistUndoKind::None;
	}

	[[nodiscard]] bool operator==(const PlaylistUndoRecord &) const = default;
};

struct PlaylistEditResult {
	PlaylistEditError error = PlaylistEditError::None;
	bool changed = false;
	PlaylistLibrary library;
	QString playlistId;
	PlaylistUndoRecord undo;
	std::size_t addedCount = 0;
	std::size_t relinkedCount = 0;
	std::size_t removedCount = 0;
	std::size_t unchangedCount = 0;

	[[nodiscard]] explicit operator bool() const {
		return error == PlaylistEditError::None;
	}
};

struct PlaylistAddSongInput {
	uint64_t documentId = 0;
	PeerToken peer;
	int32_t msg = 0;
	QString username;
	PlaylistSongMeta meta;

	[[nodiscard]] bool operator==(const PlaylistAddSongInput &) const = default;
};

struct PlaylistListEntry {
	QString id;
	QString title;
	uint64_t recentUseMs = 0;
	std::size_t visibleSongCount = 0;

	[[nodiscard]] bool operator==(const PlaylistListEntry &) const = default;
};

[[nodiscard]] std::optional<QString> FormatPlaylistId(const QByteArray &entropy);

[[nodiscard]] PlaylistEditResult CreatePlaylist(
	const PlaylistLibrary &library,
	const QString &title,
	const QByteArray &entropy,
	uint64_t nowMs,
	uint64_t selfUserId = 0);

[[nodiscard]] PlaylistEditResult CopyPlaylist(
	const PlaylistLibrary &library,
	const Playlist &source,
	const QByteArray &entropy,
	uint64_t nowMs,
	uint64_t selfUserId = 0);

[[nodiscard]] PlaylistEditResult RenamePlaylist(
	const PlaylistLibrary &library,
	const QString &playlistId,
	const QString &title,
	uint64_t nowMs,
	uint64_t selfUserId = 0);

[[nodiscard]] PlaylistEditResult DeletePlaylist(
	const PlaylistLibrary &library,
	const QString &playlistId,
	uint64_t nowMs,
	uint64_t selfUserId = 0);

[[nodiscard]] PlaylistEditResult RestorePlaylist(
	const PlaylistLibrary &library,
	const QString &playlistId,
	uint64_t nowMs,
	uint64_t selfUserId = 0);

[[nodiscard]] PlaylistEditResult DeleteForeverPlaylist(
	const PlaylistLibrary &library,
	const QString &playlistId,
	uint64_t selfUserId = 0);

[[nodiscard]] PlaylistEditResult AddPlaylistSongs(
	const PlaylistLibrary &library,
	const QString &playlistId,
	const std::vector<PlaylistAddSongInput> &songs,
	uint64_t nowMs,
	uint64_t selfUserId = 0);

[[nodiscard]] PlaylistEditResult RemovePlaylistSongs(
	const PlaylistLibrary &library,
	const QString &playlistId,
	const std::vector<uint64_t> &documentIds,
	uint64_t nowMs,
	uint64_t selfUserId = 0);

[[nodiscard]] PlaylistEditResult ReorderPlaylistSongs(
	const PlaylistLibrary &library,
	const QString &playlistId,
	const std::vector<uint64_t> &documentIds,
	uint64_t nowMs,
	uint64_t selfUserId = 0);

[[nodiscard]] PlaylistEditResult UndoPlaylistOperation(
	const PlaylistLibrary &library,
	const PlaylistUndoRecord &undo,
	uint64_t nowMs,
	uint64_t selfUserId = 0);

[[nodiscard]] std::vector<uint64_t> PlaylistVisibleSongIds(
	const Playlist &playlist);

[[nodiscard]] std::vector<PlaylistListEntry> ListVisiblePlaylists(
	const PlaylistLibrary &library,
	const std::map<QString, uint64_t> &recentUseMs = {});

} // namespace Purple
