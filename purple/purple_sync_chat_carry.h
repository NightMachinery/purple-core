/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_config_payload.h"
#include "purple/purple_local_choice.h"
#include "purple/purple_sync_chat.h"
#include "purple/purple_sync_inventory.h"

#include <QtCore/QJsonObject>

#include <cstdint>
#include <optional>
#include <vector>

namespace Purple {

enum class SyncCarryMode { Switch, Follow, Fill, RepairMark };
enum class SyncCarrySourceAccess { Unknown, Readable, Protected, Unreadable, ChatGone };
enum class SyncCarryFamily { Settings, Playlists };
enum class SyncCarryCleanup { None, OwnLatePosts, AllSettings };

enum class SyncCarryStatus {
	Invalid,
	Ready,
	ReadSource,
	ReadDestination,
	FinishPendingOld,
	AbandonPending,
	UnknownSource,
	MissingLocalVersion,
	NeedsReview,
	ConsentRequired,
	WrongScope,
	MissingRecords,
	PlaylistRequired,
};

struct SyncCarryReference {
	AccountKey account;
	PeerToken chat;
	int32_t messageId = 0;
	QString recordHash;

	[[nodiscard]] bool operator==(const SyncCarryReference &) const = default;
};

struct SyncCarryMessage {
	SyncCarryReference reference;
	int64_t date = 0;
	QByteArray bytes;
	SyncCarryFamily family = SyncCarryFamily::Settings;
	SyncChatMessageOwnership ownership = SyncChatMessageOwnership::Unknown;
};

struct SyncCarryInventory {
	AccountKey account;
	PeerToken chat;
	bool complete = false;
	std::vector<SyncCarryMessage> messages;
};

struct SyncCarrySelected {
	SyncCarryReference reference;
	int64_t date = 0;
	SyncCandidateStatus status = SyncCandidateStatus::Invalid;
	std::optional<SyncEnvelopeHeader> header;
};

struct SyncCarrySelection {
	SyncCarryStatus status = SyncCarryStatus::Invalid;
	std::vector<SyncCarrySelected> records;
	std::vector<SyncCarrySelected> heads;
	std::vector<QString> hashes;
	bool playlists = false;
	QString stamp;
};

[[nodiscard]] QString SyncCarryRecordHash(const QByteArray &bytes);
[[nodiscard]] bool IsSyncCarryHash(const QString &hash);
[[nodiscard]] std::optional<SyncCarryMessage> MakeSyncCarryMessage(
	const AccountKey &account,
	const PeerToken &chat,
	int32_t messageId,
	int64_t date,
	QByteArray bytes,
	SyncChatMessageOwnership ownership,
	SyncCarryFamily family = SyncCarryFamily::Settings);
[[nodiscard]] SyncCarrySelection SelectSyncCarryHeads(
	const SyncCarryInventory &inventory);

struct SyncCarryOwnVersion {
	QByteArray text;
	ConfigVersion version;
	QString platform;
	QString app;
	uint64_t at = 0;
};

struct SyncCarryInput {
	AccountKey account;
	PeerToken source;
	PeerToken target;
	SyncCarryMode mode = SyncCarryMode::Switch;
	SyncCarrySourceAccess access = SyncCarrySourceAccess::Unknown;
	SyncChatEligibilityResult targetEligibility;
	bool readerConsentAccepted = false;
	std::optional<SyncLocalState> state;
	QByteArray preferenceToken;
	bool otherStagesPresent = false;
	SyncCarryInventory oldInventory;
	SyncCarryInventory newInventory;
	std::optional<SyncCarryOwnVersion> ownVersion;
	std::optional<SyncChatCache> winner;
	SyncCarryCleanup cleanup = SyncCarryCleanup::None;
	bool cleanupAuthorized = false;
	bool playlistsInUse = false;
};

struct SyncCarryPlan {
	AccountKey account;
	PeerToken source;
	PeerToken target;
	SyncCarryMode mode = SyncCarryMode::Switch;
	SyncCarrySourceAccess access = SyncCarrySourceAccess::Unknown;
	QString sourceStamp;
	QString stateHash;
	QString reservedStateHash;
	QString ownerInstall;
	QString ownerDevice;
	uint64_t confirmedAtCommit = 0;
	std::vector<SyncIssuedRecord> issued;
	std::vector<SyncCarrySelected> selected;
	std::vector<SyncCarryReference> forwards;
	std::vector<QString> requiredHashes;
	std::vector<SyncCarrySelected> cleanupCandidates;
	QByteArray ownRecord;
	QString ownRecordHash;
	std::optional<SyncChatCache> winner;
	SyncCarryCleanup cleanup = SyncCarryCleanup::None;
	bool cleanupAuthorized = false;
	bool playlistRequired = false;
	QJsonObject preserved;
};

struct SyncCarryPlanResult {
	SyncCarryStatus status = SyncCarryStatus::Invalid;
	SyncCarryPlan plan;
	std::optional<SyncLocalState> reservedState;

	[[nodiscard]] explicit operator bool() const {
		return status == SyncCarryStatus::Ready;
	}
};

[[nodiscard]] SyncCarryPlanResult PlanCarry(const SyncCarryInput &input);
[[nodiscard]] QJsonObject SyncCarryPlanJson(const SyncCarryPlan &plan);
[[nodiscard]] std::optional<SyncCarryPlan> ParseSyncCarryPlan(
	const QJsonObject &document);
[[nodiscard]] QString SyncCarryPlanStamp(const SyncCarryPlan &plan);

enum class SyncPlaylistCarryStatus { NotInUse, Incomplete, Verified, Failed };

struct SyncPlaylistCarryReceipt {
	SyncPlaylistCarryStatus status = SyncPlaylistCarryStatus::NotInUse;
	AccountKey account;
	PeerToken source;
	PeerToken target;
	QString planStamp;
	QString destinationStamp;
};

struct SyncCarryReadback {
	SyncCarryStatus status = SyncCarryStatus::Invalid;
	std::vector<QString> missingHashes;
	QString destinationStamp;

	[[nodiscard]] explicit operator bool() const {
		return status == SyncCarryStatus::Ready;
	}
};

[[nodiscard]] SyncCarryReadback VerifySyncCarryReadback(
	const SyncCarryPlan &plan,
	const SyncCarryInventory &destination,
	const SyncPlaylistCarryReceipt &playlist = {});

struct SyncCarryCommit {
	SyncCarryStatus status = SyncCarryStatus::Invalid;
	std::optional<SyncLocalState> state;
	QByteArray canonical;
	QString stateHash;

	[[nodiscard]] explicit operator bool() const {
		return status == SyncCarryStatus::Ready;
	}
};

[[nodiscard]] std::optional<SyncLocalState> PrepareSyncCarryStage(
	const SyncCarryPlan &plan,
	const SyncLocalState &state);
[[nodiscard]] bool SyncCarryPlanUsesState(
	const SyncCarryPlan &plan,
	const std::optional<SyncLocalState> &state);
[[nodiscard]] SyncCarryCommit PrepareSyncCarryCommit(
	const SyncCarryPlan &plan,
	const std::optional<SyncLocalState> &state,
	const SyncCarryInventory &destination,
	const SyncPlaylistCarryReceipt &playlist = {});
[[nodiscard]] std::vector<SyncCarryReference> SyncCarryCleanupCandidates(
	const SyncCarryPlan &plan,
	const SyncCarryInventory &destination,
	const SyncPlaylistCarryReceipt &playlist = {});
[[nodiscard]] bool SyncCarrySourceMatchesCleanup(
	const SyncCarryPlan &plan,
	const SyncCarryMessage &freshSource,
	const SyncCarryInventory &destination,
	const SyncPlaylistCarryReceipt &playlist = {});

enum class SyncCarryPendingStatus {
	Invalid,
	Ready,
	NoPending,
	WrongChat,
	NotChatGone,
	LocalCopyRequired,
	InvalidStage,
	LibraryPending,
	ReconciliationRequired,
	NotProtected,
};

struct SyncCarryPendingResult {
	SyncCarryPendingStatus status = SyncCarryPendingStatus::Invalid;
	AccountKey account;
	PeerToken chat;
	QString beforeHash;
	QString stageHash;
	uint64_t stageSeq = 0;
	SyncLocalState state;
	bool restageInTarget = false;

	[[nodiscard]] explicit operator bool() const {
		return status == SyncCarryPendingStatus::Ready
			|| status == SyncCarryPendingStatus::NoPending;
	}
};

[[nodiscard]] SyncCarryPendingResult AbandonStageForLostChat(
	const AccountKey &account,
	const SyncLocalState &state,
	const PeerToken &source,
	SyncChatDirectRead outcome,
	const QByteArray &stagedRecord,
	const QByteArray &retainedLocalText);
[[nodiscard]] SyncCarryPendingResult FinishPendingStageForCarry(
	const AccountKey &account,
	const SyncLocalState &state,
	const QByteArray &stagedRecord,
	const SyncCarryMessage &freshReadback);

enum class SyncCarrySendIntent { Unknown, NotIssued, Issued };

[[nodiscard]] SyncCarryPendingResult ReconcileProtectedStageForCarry(
	const AccountKey &account,
	const SyncLocalState &state,
	const PeerToken &source,
	SyncCarrySourceAccess access,
	SyncCarrySendIntent sendIntent,
	const QByteArray &stagedRecord,
	const QByteArray &retainedLocalText,
	const SyncCarryInventory &oldInventory = {});

}
