/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <cstdint>
#include <optional>
#include <vector>

#include "purple/purple_playlists.h"
#include "purple/purple_playlists_codec.h"
#include "purple/purple_playlists_local.h"
#include "purple/purple_sync_chat.h"

namespace Purple {

inline constexpr int32_t kPlaylistRecordClassifierVersion = 1;

[[nodiscard]] QString PlaylistRecordFileName();
[[nodiscard]] QString PlaylistRecordMimeType();
[[nodiscard]] QString PlaylistRecordCaption();
[[nodiscard]] QString PlaylistRecordCaptionTag();

struct PlaylistRecordMessageMeta {
	bool isRegularMessage = false;
	bool forwarded = false;
	bool isDocument = false;
	QString caption;
	QString mimeType;
	std::vector<QString> fileNames;

	[[nodiscard]] bool operator==(const PlaylistRecordMessageMeta &) const = default;
};

enum class PlaylistRecordCandidateVerdict {
	Candidate,
	UnknownOwnership,
	AmbiguousSettings,
	Ignored,
};

[[nodiscard]] PlaylistRecordCandidateVerdict InspectPlaylistRecordMetadata(
	const PlaylistRecordMessageMeta &meta,
	SyncChatMessageOwnership ownership);

[[nodiscard]] bool IsPlaylistRecordCandidate(
	const PlaylistRecordMessageMeta &meta,
	SyncChatMessageOwnership ownership);

enum class PlaylistRecordReadOutcome {
	Succeeded,
	Failed,
	Vanished,
	Inaccessible,
	Cancelled,
};

struct PlaylistRecordInput {
	int64_t messageId = 0;
	uint64_t documentId = 0;
	SyncChatMessageOwnership ownership = SyncChatMessageOwnership::Unknown;
	PlaylistRecordMessageMeta meta;
	PlaylistRecordReadOutcome readOutcome = PlaylistRecordReadOutcome::Failed;
	QByteArray compressedBytes;
	uint64_t selfUserId = 0;

	[[nodiscard]] bool operator==(const PlaylistRecordInput &) const = default;
};

enum class PlaylistRecordStatus {
	Valid,
	Invalid,
	NewerFormat,
	Oversized,
	Incomplete,
	Ignored,
	InvalidInput,
};

enum class PlaylistRecordError {
	None,
	InvalidMessageId,
	InvalidDocumentId,
	InvalidSelfUserId,
	UnknownOwnership,
	AmbiguousSettingsConflict,
	NotOwnRecord,
	UnrecognizedRecord,
	ReadFailed,
	ReadVanished,
	ReadInaccessible,
	ReadCancelled,
	CodecError,
	SchemaError,
	SerializeFailed,
};

struct PlaylistRecordClassification {
	int64_t messageId = 0;
	uint64_t documentId = 0;
	PlaylistRecordStatus status = PlaylistRecordStatus::InvalidInput;
	PlaylistRecordError error = PlaylistRecordError::None;
	PlaylistCodecError codecError = PlaylistCodecError::None;
	PlaylistLibraryError libraryError = PlaylistLibraryError::None;
	int32_t classifierVersion = kPlaylistRecordClassifierVersion;
	std::optional<PlaylistLibrary> library;
	std::optional<QByteArray> canonicalBytes;
	std::optional<QByteArray> compressedBytes;
	std::optional<QString> canonicalHash;
	std::optional<PlaylistLocalMemoEntry> memoEntry;

	[[nodiscard]] explicit operator bool() const {
		return status == PlaylistRecordStatus::Valid;
	}

	[[nodiscard]] bool operator==(const PlaylistRecordClassification &) const = default;
};

[[nodiscard]] PlaylistRecordClassification ClassifyPlaylistRecord(
	const PlaylistRecordInput &input);

[[nodiscard]] std::optional<PlaylistLocalMemoEntry> PlaylistRecordMemoEntry(
	const PlaylistRecordClassification &classification);

} // namespace Purple
