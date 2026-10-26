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
#include <vector>

namespace Purple {

enum class SyncChatMessageOwnership {
	Unknown,
	Own,
	Other,
};

enum class SyncChatMarkStatus {
	NotMark,
	Supported,
	NewerVersion,
};

struct SyncChatMarkParseResult {
	SyncChatMarkStatus status = SyncChatMarkStatus::NotMark;
	QString version;
};

[[nodiscard]] SyncChatMarkParseResult ParseSyncChatMark(
	const QString &text,
	bool hasMedia,
	SyncChatMessageOwnership ownership);

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

struct SyncChatReaderFacts {
	bool warningRequired = false;
	bool privateRecipient = false;
	bool botOwner = false;
	bool populationUnknown = false;
	std::optional<uint64_t> otherMembers;
	std::optional<uint64_t> otherAdmins;

	[[nodiscard]] bool operator==(const SyncChatReaderFacts &) const = default;
};

struct SyncChatEligibilityResult {
	SyncChatRefusal refusal = SyncChatRefusal::UnknownKind;
	SyncChatReaderFacts readers;

	[[nodiscard]] explicit operator bool() const {
		return refusal == SyncChatRefusal::None;
	}
};

[[nodiscard]] SyncChatEligibilityResult SyncChatEligibility(
	const SyncChatFacts &facts,
	SyncChatBroadcastOwnership broadcastOwnership);

struct SyncChatCache {
	PeerToken chat;
	int32_t markId = 0;
	int64_t markDate = 0;

	[[nodiscard]] bool operator==(const SyncChatCache &) const = default;
};

[[nodiscard]] bool SyncChatMarkOutranks(
	const SyncChatCache &a,
	const SyncChatCache &b);

enum class SyncChatRecordPresence {
	Unknown,
	Empty,
	Present,
};

struct SyncChatMark {
	SyncChatCache location;
	SyncChatMarkStatus status = SyncChatMarkStatus::NotMark;
	SyncChatEligibilityResult eligibility;
	SyncChatRecordPresence records = SyncChatRecordPresence::Unknown;
	bool readable = false;
	bool freshlyRead = false;
};

enum class SyncChatDirectRead {
	Unknown,
	Valid,
	MarkGone,
	ChatGone,
	Migrated,
};

enum class SyncChatSettingsHint {
	NotRun,
	Unknown,
	Absent,
	Found,
};

struct SyncChatResolveInput {
	std::vector<SyncChatMark> marks;
	std::optional<SyncChatCache> cache;
	bool settingsBound = false;
	PeerToken settingsChat;
	SyncChatDirectRead cacheRead = SyncChatDirectRead::Unknown;
	SyncChatSettingsHint settingsHint = SyncChatSettingsHint::NotRun;
	SyncChatEligibilityResult cachedEligibility;
	bool readerWarningAccepted = false;
	int searchRetriesCompleted = 0;
	uint64_t selfUserId = 0;
};

enum class SyncChatAction {
	Invalid,
	Unset,
	Use,
	VerifyMark,
	Follow,
	Fill,
	Searching,
	Lost,
	NewerMark,
	ReadWinnerInventory,
};

struct SyncChatResolution {
	SyncChatAction action = SyncChatAction::Invalid;
	std::optional<SyncChatCache> cache;
	std::optional<PeerToken> readChat;
	std::optional<SyncChatCache> winner;
	std::optional<PeerToken> source;
	std::optional<PeerToken> olderAlternative;
	SyncChatReaderFacts readers;
	bool needsReaderWarning = false;
	bool postingHeld = true;
	int retryAfterSeconds = 0;
};

[[nodiscard]] SyncChatResolution ResolveSyncChat(
	const SyncChatResolveInput &input);

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
