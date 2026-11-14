/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_playlists_inventory.h"

namespace Purple {

enum class PlaylistPayloadOrigin {
	Unproven,
	DownloadedThisStep,
	Cached,
	PublishedThisStep,
};

struct PlaylistRecordPayloadProof {
	std::optional<PeerToken> chat;
	int64_t messageId = 0;
	uint64_t documentId = 0;
	PlaylistPayloadOrigin origin = PlaylistPayloadOrigin::Unproven;
	bool readSucceeded = false;
	bool publicationAcknowledged = false;
	std::optional<QString> expectedCanonicalHash;
	QByteArray compressedBytes;

	[[nodiscard]] bool operator==(const PlaylistRecordPayloadProof &) const = default;
};

struct PlaylistRecordPruneInput {
	uint64_t selfUserId = 0;
	std::optional<PeerToken> currentChat;
	PlaylistInventoryResult inventory;
	bool presentSetChanged = false;
	bool publicationSucceeded = false;
	std::vector<PlaylistRecordPayloadProof> payloads;

	[[nodiscard]] bool operator==(const PlaylistRecordPruneInput &) const = default;
};

enum class PlaylistRecordPruneStatus {
	InvalidInput,
	Skipped,
	Planned,
};

enum class PlaylistRecordPruneError {
	None,
	InvalidChat,
	InvalidInventory,
	BindingMismatch,
	InvalidIdentity,
	ConflictingPresence,
	ContainmentFailed,
};

enum class PlaylistRecordUnusableReason {
	ExcludedPresence,
	UnrecognizedRecord,
	MissingPayload,
	IdentityMismatch,
	ConflictingPayload,
	UnprovenPayload,
	ReadFailed,
	PublicationUnacknowledged,
	InvalidCacheHash,
	CacheHashMismatch,
	InvalidPayload,
	NewerFormat,
	Oversized,
};

struct PlaylistRecordIdentity {
	int32_t messageId = 0;
	uint64_t documentId = 0;

	[[nodiscard]] bool operator==(const PlaylistRecordIdentity &) const = default;
};

struct PlaylistRecordPrune {
	PlaylistRecordIdentity target;
	PlaylistRecordIdentity witness;
	QString witnessCanonicalHash;

	[[nodiscard]] bool operator==(const PlaylistRecordPrune &) const = default;
};

struct PlaylistRecordUnusable {
	int32_t messageId = 0;
	PlaylistRecordUnusableReason reason = PlaylistRecordUnusableReason::MissingPayload;

	[[nodiscard]] bool operator==(const PlaylistRecordUnusable &) const = default;
};

struct PlaylistRecordPruneResult {
	PlaylistRecordPruneStatus status = PlaylistRecordPruneStatus::InvalidInput;
	PlaylistRecordPruneError error = PlaylistRecordPruneError::None;
	std::vector<PlaylistRecordPrune> prune;
	std::vector<PlaylistRecordIdentity> survivors;
	std::vector<PlaylistRecordUnusable> unusable;

	[[nodiscard]] bool operator==(const PlaylistRecordPruneResult &) const = default;
};

[[nodiscard]] PlaylistRecordPruneResult PlanPlaylistRecordPrunes(
	const PlaylistRecordPruneInput &input);

} // namespace Purple
