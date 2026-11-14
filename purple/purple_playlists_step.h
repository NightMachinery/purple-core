/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_playlists_cover.h"

namespace Purple {

enum class PlaylistStepLocalRead {
	Failed,
	Missing,
	Succeeded,
};

enum class PlaylistStepTrigger {
	Automatic,
	Manual,
	Logout,
	LocalPurge,
};

struct PlaylistStepClock {
	uint64_t selfUserId = 0;
	std::optional<PeerToken> chat;
	uint64_t nowMs = 0;
	bool requestSucceeded = false;

	[[nodiscard]] bool operator==(const PlaylistStepClock &) const = default;
};

struct PlaylistStepInput {
	uint64_t selfUserId = 0;
	bool accountAuthorized = false;
	PlaylistStepTrigger trigger = PlaylistStepTrigger::Automatic;
	PlaylistStepLocalRead libraryRead = PlaylistStepLocalRead::Failed;
	PlaylistLibrary library;
	PlaylistStepLocalRead stateRead = PlaylistStepLocalRead::Failed;
	PlaylistLocalState state;
	PlaylistInventoryInput discovery;
	std::vector<PlaylistRecordPayloadProof> payloads;
	PlaylistStepClock clock;

	[[nodiscard]] bool operator==(const PlaylistStepInput &) const = default;
};

enum class PlaylistStepStatus {
	Blocked,
	Skipped,
	Planned,
};

enum class PlaylistStepError {
	None,
	Unauthorized,
	InvalidSelfUserId,
	InvalidTrigger,
	LibraryReadFailed,
	InvalidLibrary,
	NewerLibrary,
	StateReadFailed,
	InvalidState,
	NewerState,
	InvalidInventory,
	AccountMismatch,
	MergeFailed,
	DiffFailed,
};

enum class PlaylistStepPublication {
	NotPlanned,
	SyncOff,
	DiscoveryUnavailable,
	InitialEmpty,
	EqualRecord,
	Required,
	ExpandedLimit,
	CompressedLimit,
	EncodingFailed,
};

struct PlaylistStepRecordCounts {
	std::size_t valid = 0;
	std::size_t invalid = 0;
	std::size_t newerFormat = 0;
	std::size_t oversized = 0;
	std::size_t unreadable = 0;

	[[nodiscard]] bool operator==(const PlaylistStepRecordCounts &) const = default;
};

struct PlaylistStepResult {
	PlaylistStepStatus status = PlaylistStepStatus::Blocked;
	PlaylistStepError error = PlaylistStepError::None;
	std::optional<PlaylistLibrary> library;
	std::optional<PlaylistLocalState> state;
	QByteArray canonicalBytes;
	QString canonicalHash;
	bool writeLibrary = false;
	bool writeState = false;
	bool steadyState = false;
	PlaylistInventoryResult inventory;
	std::map<int32_t, PlaylistLocalMemoEntry> retiredMemo;
	std::vector<PlaylistRecordIdentity> retryReads;
	PlaylistStepRecordCounts records;
	std::size_t newlyMergedRecords = 0;
	bool discoveryComplete = false;
	PlaylistAutoPurgeStatus purgeStatus =
		PlaylistAutoPurgeStatus::BlockedServerRequestRequired;
	std::vector<QString> purgedIds;
	PlaylistLibraryDiffResult diff;
	PlaylistStepPublication publication = PlaylistStepPublication::NotPlanned;
	QByteArray publicationBytes;
	PlaylistRecordPruneResult prune;

	[[nodiscard]] bool publish() const {
		return publication == PlaylistStepPublication::Required;
	}

	[[nodiscard]] bool operator==(const PlaylistStepResult &) const = default;
};

[[nodiscard]] PlaylistStepResult PlanPlaylistStep(
	const PlaylistStepInput &input);

} // namespace Purple
