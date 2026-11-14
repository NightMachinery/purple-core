/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "purple/purple_peer_token.h"
#include "purple/purple_playlists_local.h"
#include "purple/purple_playlists_sync.h"
#include "purple/purple_sync_chat.h"

namespace Purple {

enum class PlaylistInventoryStatus {
	Valid,
	InvalidInput,
};

enum class PlaylistInventoryError {
	None,
	InvalidSelfUserId,
	InvalidCurrentChat,
	InvalidPriorChat,
	MissingPriorChat,
	InvalidMemo,
	InvalidMessageId,
	InvalidDocumentId,
	InvalidByIdOutcome,
};

enum class PlaylistByIdOutcome {
	Present,
	Gone,
	Failed,
	Inaccessible,
	Cancelled,
};

struct PlaylistInventoryObservation {
	int64_t messageId = 0;
	uint64_t documentId = 0;
	SyncChatMessageOwnership ownership = SyncChatMessageOwnership::Unknown;
	PlaylistRecordMessageMeta meta;

	[[nodiscard]] bool operator==(const PlaylistInventoryObservation &) const = default;
};

struct PlaylistByIdObservation {
	int64_t messageId = 0;
	PlaylistByIdOutcome outcome = PlaylistByIdOutcome::Failed;
	uint64_t documentId = 0;
	SyncChatMessageOwnership ownership = SyncChatMessageOwnership::Unknown;
	PlaylistRecordMessageMeta meta;

	[[nodiscard]] bool operator==(const PlaylistByIdObservation &) const = default;
};

struct PlaylistInventoryInput {
	uint64_t selfUserId = 0;
	std::optional<PeerToken> currentChat;
	std::optional<PeerToken> priorChat;
	std::map<int32_t, PlaylistLocalMemoEntry> priorMemo;
	bool listingSucceeded = false;
	bool listingExhausted = false;
	std::vector<PlaylistInventoryObservation> listedObservations;
	bool byIdSucceeded = false;
	std::vector<PlaylistByIdObservation> byIdObservations;

	[[nodiscard]] bool operator==(const PlaylistInventoryInput &) const = default;
};

struct PlaylistInventoryPresentCandidate {
	int32_t messageId = 0;
	uint64_t documentId = 0;
	PlaylistRecordMessageMeta meta;
	bool fromById = false;

	[[nodiscard]] bool operator==(const PlaylistInventoryPresentCandidate &) const = default;
};

struct PlaylistInventoryReadCandidate {
	int32_t messageId = 0;
	uint64_t documentId = 0;
	PlaylistRecordMessageMeta meta;
	bool fromById = false;

	[[nodiscard]] bool operator==(const PlaylistInventoryReadCandidate &) const = default;
};

struct PlaylistInventoryReusableEntry {
	int32_t messageId = 0;
	PlaylistLocalMemoEntry entry;
	bool fromById = false;

	[[nodiscard]] bool operator==(const PlaylistInventoryReusableEntry &) const = default;
};

enum class PlaylistInventoryInvalidationReason {
	DocumentReplaced,
	ClassifierUpgradeNeeded,
	LostEligibility,
};

struct PlaylistInventoryInvalidatedMemo {
	int32_t messageId = 0;
	uint64_t priorDocumentId = 0;
	PlaylistInventoryInvalidationReason reason =
		PlaylistInventoryInvalidationReason::LostEligibility;
	bool fromById = false;

	[[nodiscard]] bool operator==(const PlaylistInventoryInvalidatedMemo &) const = default;
};

struct PlaylistInventoryResult {
	PlaylistInventoryStatus status = PlaylistInventoryStatus::InvalidInput;
	PlaylistInventoryError error = PlaylistInventoryError::None;
	bool bindingChanged = false;
	std::optional<PeerToken> normalizedCurrentChat;
	std::optional<PeerToken> normalizedPriorChat;
	bool metadataComplete = false;
	std::vector<PlaylistInventoryPresentCandidate> presentCandidates;
	std::vector<PlaylistInventoryReadCandidate> readCandidates;
	std::vector<PlaylistInventoryReusableEntry> reusableMemo;
	std::vector<PlaylistInventoryInvalidatedMemo> invalidatedMemo;
	std::vector<int32_t> confirmedGoneIds;
	std::size_t vanishedMemoRecordCount = 0;
	std::vector<int32_t> pendingByIdIds;
	std::map<int32_t, PlaylistLocalMemoEntry> unresolvedPriorMemo;
	std::vector<int32_t> unknownOwnershipIds;
	std::vector<int32_t> conflictingIds;

	[[nodiscard]] explicit operator bool() const {
		return status == PlaylistInventoryStatus::Valid;
	}

	[[nodiscard]] bool operator==(const PlaylistInventoryResult &) const = default;
};

[[nodiscard]] PlaylistInventoryResult ReconcilePlaylistInventory(
	const PlaylistInventoryInput &input);

} // namespace Purple
