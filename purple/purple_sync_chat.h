/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_peer_token.h"
#include "purple/purple_sync_local_state.h"

#include <cstdint>
#include <optional>

namespace Purple {

enum class SyncChatMessageOwnership {
	Unknown,
	Own,
	Other,
};

enum class SyncChatKind {
	Unknown,
	Self,
	Private,
	Bot,
	BasicGroup,
	Supergroup,
	Broadcast,
	Secret,
	ChannelDirectMessages,
};

enum class SyncChatBroadcastOwnership {
	Unselected,
	OutgoingVerified,
	CreatorAndSoleAdmin,
};

struct SyncChatMessageFacts {
	SyncChatKind kind = SyncChatKind::Unknown;
	std::optional<bool> regular;
	std::optional<bool> forwarded;
	std::optional<bool> viaBot;
	std::optional<bool> scheduled;
	std::optional<bool> hasTtl;
	std::optional<bool> sentAsOther;
	std::optional<bool> out;
	std::optional<PeerToken> sender;
	uint64_t selfUserId = 0;
	SyncChatBroadcastOwnership broadcastOwnership
		= SyncChatBroadcastOwnership::Unselected;
	std::optional<bool> creator;
	std::optional<bool> soleAdmin;
};

[[nodiscard]] SyncChatMessageOwnership GetSyncChatMessageOwnership(
	const SyncChatMessageFacts &facts);

[[nodiscard]] bool IsOwnSyncChatMessage(const SyncChatMessageFacts &facts);

struct SyncChatFacts {
	SyncChatKind kind = SyncChatKind::Unknown;
	bool publicUsername = false;
	bool protectedContent = false;
	bool forum = false;
	bool linkedDiscussion = false;
	bool autoDelete = false;
	bool paidMessages = false;
	bool canSendFiles = false;
	bool anonymousAdmin = false;
	bool postsAsOther = false;
	bool migrated = false;
	bool slowMode = false;
	bool admin = false;
	bool creator = false;
	bool soleAdmin = false;
	std::optional<uint64_t> otherMembers;
	std::optional<uint64_t> otherAdmins;
};

enum class SyncChatRefusal {
	None,
	UnknownKind,
	Secret,
	TopicRouting,
	Public,
	Protected,
	Discussion,
	AutoDelete,
	Paid,
	CannotSendFiles,
	SendAsOther,
	Migrated,
	SlowMode,
	BroadcastOwnershipUnselected,
	BroadcastNeedsCreatorAndSoleAdmin,
};

struct SyncChatEligibilityResult {
	SyncChatRefusal refusal = SyncChatRefusal::UnknownKind;

	[[nodiscard]] explicit operator bool() const {
		return refusal == SyncChatRefusal::None;
	}
};

[[nodiscard]] SyncChatEligibilityResult SyncChatEligibility(
	const SyncChatFacts &facts,
	SyncChatBroadcastOwnership broadcastOwnership);

enum class SyncChatMoveError {
	None,
	InvalidState,
	InvalidChat,
	PendingStage,
};

struct SyncChatMoveResult {
	SyncLocalState state;
	SyncChatMoveError error = SyncChatMoveError::None;
	bool changed = false;

	[[nodiscard]] explicit operator bool() const {
		return error == SyncChatMoveError::None;
	}
};

[[nodiscard]] SyncChatMoveResult MoveSyncChat(
	const SyncLocalState &state,
	const PeerToken &newChat,
	bool stagePresent,
	uint64_t selfUserId = 0);

}
