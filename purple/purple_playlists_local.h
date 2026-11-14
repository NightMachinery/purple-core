/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_playlists.h"
#include "purple/purple_playlists_diff.h"
#include "purple/purple_playlists_policy.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <cstdint>
#include <map>
#include <optional>

namespace Purple {

enum class PlaylistLocalStatus {
	Valid,
	NewerFormat,
	Invalid,
};

enum class PlaylistLocalError {
	None,
	InvalidJson,
	SizeLimit,
	FieldType,
	MissingField,
	InvalidFormat,
	InvalidId,
	InvalidStamp,
	InvalidRecentUse,
	InvalidReason,
	InvalidSnapshot,
	InvalidPlaylist,
	InvalidPreservedJson,
	InvalidSelfUserId,
	InvalidChat,
	InvalidMemo,
	InvalidStatus,
	InvalidUnsyncedSince,
};

enum class PlaylistLocalMemoState {
	Valid,
	Invalid,
	NewerFormat,
	Oversized,
};

enum class PlaylistLocalSyncResult {
	Idle,
	Synced,
	Offline,
	Wait,
	Problem,
	Failed,
	Oversized,
	NewerFormat,
};

struct PlaylistLocalMemoEntry {
	uint64_t documentId = 0;
	PlaylistLocalMemoState state = PlaylistLocalMemoState::Invalid;
	int32_t classifier = 0;
	std::optional<QString> canonicalHash;
	QJsonObject preserved;

	[[nodiscard]] bool operator==(const PlaylistLocalMemoEntry &) const = default;
};

struct PlaylistLocalStepStatus {
	PlaylistLocalSyncResult result = PlaylistLocalSyncResult::Idle;
	std::optional<uint64_t> lastStepAtMs;
	QJsonObject preserved;

	[[nodiscard]] bool operator==(const PlaylistLocalStepStatus &) const = default;
};

struct PlaylistLocalDueSeenEntry {
	PlaylistAutoPurgeDueSeen value;
	QJsonObject preserved;

	[[nodiscard]] bool operator==(const PlaylistLocalDueSeenEntry &) const = default;
};

struct PlaylistLocalHiddenEntry {
	PlaylistNoticeReason reason = PlaylistNoticeReason::DeletedElsewhere;
	Playlist playlist;
	QJsonObject preserved;

	[[nodiscard]] bool operator==(const PlaylistLocalHiddenEntry &) const = default;
};

struct PlaylistLocalState {
	int format = kPlaylistFormatVersion;
	bool sync = true;
	std::optional<PeerToken> chat;
	std::map<int32_t, PlaylistLocalMemoEntry> memo;
	PlaylistLocalStepStatus status;
	// Non-empty memo requires chat; codec never infers or clears status/unsyncedSince.
	std::optional<uint64_t> unsyncedSince;
	std::map<QString, PlaylistLocalDueSeenEntry> dueSeen;
	std::map<QString, uint64_t> recentUse;
	std::map<QString, PlaylistLocalHiddenEntry> hidden;
	// Separate unknown keys so stale copies cannot override typed values.
	QJsonObject preserved;

	[[nodiscard]] bool operator==(const PlaylistLocalState &) const = default;
};

struct PlaylistLocalValidationResult {
	PlaylistLocalStatus status = PlaylistLocalStatus::Invalid;
	PlaylistLocalError error = PlaylistLocalError::InvalidJson;
	PlaylistLibraryError playlistError = PlaylistLibraryError::None;

	[[nodiscard]] explicit operator bool() const noexcept {
		return status == PlaylistLocalStatus::Valid;
	}
};

struct PlaylistLocalParseResult {
	PlaylistLocalStatus status = PlaylistLocalStatus::Invalid;
	PlaylistLocalError error = PlaylistLocalError::InvalidJson;
	PlaylistLibraryError playlistError = PlaylistLibraryError::None;
	PlaylistLocalState state;

	[[nodiscard]] explicit operator bool() const noexcept {
		return status == PlaylistLocalStatus::Valid;
	}
};

struct PlaylistLocalWriteResult {
	QByteArray json;
	PlaylistLocalStatus status = PlaylistLocalStatus::Invalid;
	PlaylistLocalError error = PlaylistLocalError::InvalidJson;
	PlaylistLibraryError playlistError = PlaylistLibraryError::None;

	[[nodiscard]] explicit operator bool() const noexcept {
		return status == PlaylistLocalStatus::Valid;
	}
};

[[nodiscard]] inline PlaylistLocalState DefaultPlaylistLocalState() {
	return {};
}

[[nodiscard]] std::map<QString, PlaylistAutoPurgeDueSeen> PlaylistLocalDueSeenValues(
	const PlaylistLocalState &state);

[[nodiscard]] PlaylistLocalValidationResult ValidatePlaylistLocalState(
	const PlaylistLocalState &state,
	uint64_t selfUserId = 0);

// Hidden snapshots need an uncapped local budget instead of the settings cap.
[[nodiscard]] PlaylistLocalParseResult ParsePlaylistLocalState(
	const QByteArray &json,
	uint64_t selfUserId = 0,
	std::optional<qsizetype> maximumBytes = std::nullopt);

[[nodiscard]] PlaylistLocalWriteResult SerializePlaylistLocalState(
	const PlaylistLocalState &state,
	uint64_t selfUserId = 0);

} // namespace Purple
