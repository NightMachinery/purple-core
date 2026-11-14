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
#include <map>
#include <vector>

namespace Purple {

inline constexpr auto kAutoPurgeRetentionMs =
	uint64_t(30) * 24 * 60 * 60 * 1000;
inline constexpr auto kAutoPurgeMinimumObservationMs =
	uint64_t(15) * 60 * 1000;

struct PlaylistSongCandidate {
	uint64_t documentId = 0;
	int32_t messageId = 0;
	bool hasAudioAttribute = false;
	bool isVoiceMessage = false;
	bool isRegularServerMessage = false;
	bool isMainMessageMedia = false;
	bool isScheduled = false;
	bool isLocalUnsent = false;
	bool isSponsored = false;
	bool isStory = false;
	bool isProfileMusic = false;
	bool isSecretChat = false;
	bool isRichMediaSubtrack = false;
	bool hasSelfDestructTimer = false;
	bool isProtectedContent = false;
};

enum class SongQualificationReason {
	Qualified,
	InvalidDocumentId,
	InvalidMessageId,
	MissingAudioAttribute,
	VoiceMessage,
	ScheduledMessage,
	LocalUnsentMessage,
	SponsoredMessage,
	StoryMessage,
	ProfileMusicItem,
	SecretChatMessage,
	NotRegularServerMessage,
	RichMediaSubtrack,
	NotMainMessageMedia,
	SelfDestructTimer,
	ProtectedContentRestricted,
};

struct SongQualificationResult {
	SongQualificationReason reason = SongQualificationReason::MissingAudioAttribute;

	[[nodiscard]] bool qualified() const {
		return reason == SongQualificationReason::Qualified;
	}

	[[nodiscard]] explicit operator bool() const {
		return qualified();
	}

	[[nodiscard]] bool operator==(const SongQualificationResult &) const = default;
};

struct PlaylistRawSongMeta {
	QString title;
	QString performer;
	QString fileName;
	int64_t durationSeconds = 0;
};

enum class SongMetaPrepareError {
	None,
	InvalidTitleUnicode,
	InvalidPerformerUnicode,
	InvalidDuration,
};

struct SongMetaPrepareResult {
	SongMetaPrepareError error = SongMetaPrepareError::None;
	PlaylistSongMeta meta;

	[[nodiscard]] explicit operator bool() const {
		return error == SongMetaPrepareError::None;
	}

	[[nodiscard]] bool operator==(const SongMetaPrepareResult &) const = default;
};

struct PlaylistChatOrderItem {
	uint64_t documentId = 0;
	int32_t messageId = 0;
	bool fromMigratedBasicGroup = false;

	[[nodiscard]] bool operator==(const PlaylistChatOrderItem &) const = default;
};

enum class PlaylistChatOrderError {
	None,
	InvalidDocumentId,
	InvalidMessageId,
};

struct PlaylistChatOrderResult {
	PlaylistChatOrderError error = PlaylistChatOrderError::None;
	std::vector<PlaylistChatOrderItem> items;

	[[nodiscard]] explicit operator bool() const {
		return error == PlaylistChatOrderError::None;
	}

	[[nodiscard]] bool operator==(const PlaylistChatOrderResult &) const = default;
};

struct PlaylistAutoPurgeDueSeen {
	uint64_t deletionStamp = 0;
	uint64_t firstSeenAtMs = 0;

	[[nodiscard]] bool operator==(const PlaylistAutoPurgeDueSeen &) const = default;
};

struct PlaylistAutoPurgeContext {
	bool syncEnabled = true;
	uint64_t nowMs = 0;
	uint64_t selfUserId = 0;
	bool serverRequestSucceeded = false;
	bool listingPagedToEnd = false;
	bool missingMemoEntriesResolved = false;
	bool allCandidatesClassifiedOrRead = false;
	bool noTransientReadFailure = false;
	bool noUnknownOwnership = false;
	std::size_t vanishedMemoRecordCount = 0;
	std::size_t newValidRecordCount = 0;
	bool noPresentNewerFormatRecord = false;
};

enum class PlaylistAutoPurgeStatus {
	Completed,
	InvalidLibrary,
	NewerFormatLibrary,
	BlockedServerRequestRequired,
	BlockedIncompleteDiscovery,
	BlockedNewerFormatRecord,
};

struct PlaylistAutoPurgeResult {
	PlaylistAutoPurgeStatus status = PlaylistAutoPurgeStatus::BlockedServerRequestRequired;
	PlaylistLibrary library;
	std::map<QString, PlaylistAutoPurgeDueSeen> dueSeen;
	std::vector<QString> purgedIds;

	[[nodiscard]] std::size_t purgedCount() const {
		return purgedIds.size();
	}

	[[nodiscard]] explicit operator bool() const {
		return status == PlaylistAutoPurgeStatus::Completed;
	}

	[[nodiscard]] bool operator==(const PlaylistAutoPurgeResult &) const = default;
};

[[nodiscard]] SongQualificationResult QualifySong(
	const PlaylistSongCandidate &candidate,
	bool protectedContentAllowed);

[[nodiscard]] SongMetaPrepareResult PrepareSongMeta(
	const PlaylistRawSongMeta &raw);

[[nodiscard]] PlaylistChatOrderResult SortPlaylistChatOrder(
	const std::vector<PlaylistChatOrderItem> &items);

[[nodiscard]] PlaylistAutoPurgeResult AutoPurgePlaylists(
	const PlaylistLibrary &mergedLibrary,
	const std::map<QString, PlaylistAutoPurgeDueSeen> &dueSeen,
	const PlaylistAutoPurgeContext &context);

} // namespace Purple
