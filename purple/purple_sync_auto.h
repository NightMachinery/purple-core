/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_config_diff.h"
#include "purple/purple_sync_config_flow.h"

namespace Purple {

inline constexpr auto kSyncAutoVersion = 1;
inline constexpr auto kSyncAutoCheckSeconds = int64_t(15 * 60);
inline constexpr auto kSyncAutoRetireLimit = 4;
inline constexpr auto kSyncAutoRecordLimit = 2 * 1024 * 1024;

enum class SyncAutoAction {
	NoOp, Check, AutoApply, Publish, EditInPlace, Post, Reconcile, Retire, Hold, RetryAt,
};

enum class SyncAutoReason {
	None, InvalidRecord, AccountUnavailable, OwnershipUnanswered, WrongFileAccount,
	AccountUnbound, SyncChatNotResolved, ChatNotWritable, InvalidSettings, FileAbsent,
	LastGood, NoSavedBytes, NeedsReview, Conflict, UnsafeImpact, WorkModeScopeChanged,
	CloneDetected, StoreMismatch, OwnershipChanged, DeletionRefused, RetryStopped,
	InterruptedApply, StateChanged,
};

enum class SyncAutoEvent {
	Enable, Disable, SessionReady, Foreground, Unlock, LocalSaved, Retry, Completed,
};

enum class SyncAutoOutcome {
	Inventory, Applied, Staged, Sent, Committed, Deleted,
	NetworkFailure, OutcomeUnknown, EditRefused, DeletionRefused,
};

struct SyncAutoRecord {
	int version = kSyncAutoVersion;
	AccountKey account;
	bool enabled = false;
	uint64_t event = 0;
	uint64_t ticket = 0;
	uint64_t run = 0;
	bool runStopped = false;
	QString session;
	bool active = false;
	SyncAutoAction pending = SyncAutoAction::NoOp;
	bool commitOnly = false;
	int64_t lastCheck = 0;
	bool checked = false;
	bool queuedCheck = false;
	bool queuedSave = false;
	QString savedFingerprint;
	int retries = 0;
	int64_t retryAt = 0;
	int retired = 0;
	bool editRefused = false;
	bool uncertain = false;
	bool retiring = false;
	SyncAutoReason held = SyncAutoReason::None;
	QByteArray expectedState;
	QByteArray expectedFile;
	QByteArray record;
	int32_t messageId = 0;
	std::vector<SyncAutoReason> noticed;
	QJsonObject preserved;
	friend bool operator==(const SyncAutoRecord &, const SyncAutoRecord &) = default;
};

struct SyncAutoFacts {
	AccountKey account;
	AccountKey fileAccount;
	bool signedIn = false;
	bool ownerAnswered = false;
	bool accountLock = false;
	QString session;
	QString inFlightSession;
	bool chatResolved = false;
	bool chatWritable = false;
	PeerToken chat;
	std::optional<SyncLocalState> state;
	QByteArray bindingToken;
	QString currentDevice;
	SyncSettingsFile local;
	QString savedFingerprint;
	bool saveDebounceFinished = false;
	QByteArray stagedRecord;
	SyncConfigSendQueue queue = SyncConfigSendQueue::Empty;
	bool editSupported = false;
	bool impactContextComplete = false;
	ConfigImpactInput impactContext;
	AccountWorkMode proposedWorkMode = AccountWorkMode::Own;
	SyncConfigWriter writer;
};

enum class SyncAutoMessagePresence { Unknown, Present, Absent };

struct SyncAutoServerRead {
	int32_t messageId = 0;
	QByteArray record;
	bool fresh = false;
	bool owned = false;
	SyncAutoMessagePresence presence = SyncAutoMessagePresence::Unknown;
};

struct SyncAutoInventory {
	AccountKey account;
	PeerToken chat;
	QString session;
	uint64_t ticket = 0;
	int64_t readAt = 0;
	bool fresh = false;
	SyncAccountInventoryResult inventory;
	std::vector<SyncAutoServerRead> reads;
};

struct SyncAutoInput {
	SyncAutoRecord record;
	SyncAutoFacts facts;
	SyncAutoEvent event = SyncAutoEvent::SessionReady;
	uint64_t sequence = 0;
	int64_t now = 0;
	uint64_t completedTicket = 0;
	SyncAutoOutcome outcome = SyncAutoOutcome::Inventory;
	std::optional<SyncAutoInventory> inventory;
};

struct SyncAutoPlan {
	SyncAutoAction action = SyncAutoAction::NoOp;
	SyncAutoReason reason = SyncAutoReason::None;
	bool notice = false;
	SyncAutoRecord record;
	uint64_t ticket = 0;
	int64_t retryAt = 0;
	int32_t messageId = 0;
	QByteArray bytes;
	std::optional<SyncLocalState> nextState;
	std::optional<SyncConfigApplyPlan> apply;
	bool commitOnly = false;
};

enum class SyncAutoCodecStatus { Valid, NewerVersion, Invalid };

struct SyncAutoDecodeResult {
	SyncAutoCodecStatus status = SyncAutoCodecStatus::Invalid;
	std::optional<SyncAutoRecord> record;
	QByteArray opaque;
};

[[nodiscard]] bool IsSyncAutoRecord(const SyncAutoRecord &record);
[[nodiscard]] std::optional<QByteArray> SerializeSyncAutoRecord(const SyncAutoRecord &record);
[[nodiscard]] SyncAutoDecodeResult ParseSyncAutoRecord(const QByteArray &bytes);
[[nodiscard]] SyncAutoPlan PlanSyncAuto(const SyncAutoInput &input);

}
