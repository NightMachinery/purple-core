/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_sync_chat_carry.h"

namespace Purple {

enum class SyncCarryPhase {
	ResolvePending,
	WritePendingState,
	RetirePendingStage,
	ResolveAgain,
	Plan,
	StageOwn,
	Deliver,
	VerifyDestination,
	Mark,
	PrepareCache,
	CommitState,
	RetireOwnStage,
	CompleteCache,
	Cleanup,
	Done,
	Aborted,
	CleanupSkipped,
	InterruptedAbandoned,
	FinishedForFollow,
};

enum class SyncCarryOutcome { Success, Failed, Unknown };

enum class SyncCarryJournalAction {
	None,
	ResolvePending,
	WritePendingState,
	RetirePendingStage,
	ResolveAgain,
	Plan,
	StageOwn,
	Deliver,
	VerifyDestination,
	PostMark,
	ReadMark,
	PrepareCache,
	CommitState,
	RetireOwnStage,
	CompleteCache,
	Cleanup,
};

struct SyncCarryJournal {
	int version = 1;
	QString operationId;
	AccountKey account;
	PeerToken source;
	PeerToken target;
	SyncCarryMode mode = SyncCarryMode::Switch;
	SyncCarryPhase phase = SyncCarryPhase::ResolveAgain;
	uint64_t revision = 0;
	QString stateHash;
	QString pendingStateHash;
	QString pendingStageHash;
	uint64_t pendingSeq = 0;
	SyncChatAction expectedAction = SyncChatAction::Invalid;
	std::optional<SyncChatCache> expectedWinner;
	std::optional<SyncChatCache> oldMark;
	std::optional<SyncChatCache> targetMark;
	QString markText;
	QString markRequestId;
	QString ownRequestId;
	bool markAttempted = false;
	bool removeOldMarkAuthorized = false;
	std::optional<SyncCarryPlan> plan;
	std::vector<QString> missingHashes;
	QString nextStateHash;
	bool commitPrepared = false;
	bool destinationVerified = false;
	bool markVerified = false;
	bool cachePrepared = false;
	bool stateCommitted = false;
	bool cacheCompleted = false;
	QString loginAuthority;
	std::optional<SyncChatReaderFacts> readerBaseline;
	bool deliveryAttempted = false;
	std::vector<SyncCarryReference> deliveryHistory;
	QByteArray interruptedState;
	QByteArray interruptedStage;
	QString abandoningLogin;
	QByteArray deferredRecord;
	QByteArray targetPendingRecord;
	std::vector<SyncCarryReference> abandonedCleanup;
	bool cleanupChatGone = false;
	bool finishThenFollow = false;
	std::optional<SyncChatCache> supersededBy;
	std::optional<SyncChatCache> attemptedMark;
	SyncCarryOutcome lastOutcome = SyncCarryOutcome::Success;
	QJsonObject preserved;
};

struct SyncCarryJournalStart {
	QString operationId;
	AccountKey account;
	PeerToken source;
	PeerToken target;
	SyncCarryMode mode = SyncCarryMode::Switch;
	std::optional<SyncLocalState> state;
	bool stateAbsentConfirmed = false;
	QByteArray preferenceToken;
	bool otherStagesPresent = false;
	SyncChatResolution resolution;
	SyncChatEligibilityResult targetEligibility;
	bool readerConsentAccepted = false;
	bool operationConfirmed = false;
	QString markText;
	QString markRequestId;
	QString ownRequestId;
	bool removeOldMarkAuthorized = false;
	QString loginAuthority;
	SyncChatDirectRead oldRead = SyncChatDirectRead::Unknown;
	SyncCarrySourceAccess sourceAccess = SyncCarrySourceAccess::Unknown;
};

struct SyncCarryCacheReceipt {
	QString operationId;
	AccountKey account;
	SyncChatCache mark;
	bool complete = false;
};

enum class SyncCarryMarkOutcome { Unknown, Absent, Valid, ChatGone };

struct SyncCarryMarkReadback {
	SyncCarryMarkOutcome outcome = SyncCarryMarkOutcome::Unknown;
	AccountKey account;
	PeerToken chat;
	int32_t messageId = 0;
	int64_t date = 0;
	QString text;
	bool hasMedia = false;
	SyncChatMessageOwnership ownership = SyncChatMessageOwnership::Unknown;
};

enum class SyncCarryEventKind {
	PendingPrepared,
	StateObserved,
	StageRetired,
	Resolution,
	PlanPrepared,
	OwnStagePersisted,
	DeliveryPrepared,
	DeliveryResult,
	DestinationRead,
	MarkPostPrepared,
	MarkPostResult,
	MarkRead,
	CachePrepared,
	CacheCompleted,
	CleanupCompleted,
	Cancel,
	SkipCleanup,
	CleanupChatGone,
	FinishThenFollow,
};

struct SyncCarryJournalEvent {
	QString operationId;
	uint64_t revision = 0;
	AccountKey account;
	PeerToken source;
	PeerToken target;
	SyncCarryEventKind kind = SyncCarryEventKind::Resolution;
	SyncCarryOutcome outcome = SyncCarryOutcome::Unknown;
	bool objectsDurable = false;
	std::optional<SyncLocalState> state;
	bool stateAbsentConfirmed = false;
	QByteArray stagedRecord;
	QByteArray retainedLocalText;
	SyncChatDirectRead oldRead = SyncChatDirectRead::Unknown;
	std::optional<SyncCarryMessage> pendingReadback;
	QString preparedImageHash;
	SyncChatResolution resolution;
	std::optional<SyncCarryPlan> plan;
	std::optional<SyncCarryInventory> destination;
	SyncPlaylistCarryReceipt playlist;
	SyncCarryMarkReadback mark;
	SyncCarryCacheReceipt cache;
	QString retiredStageHash;
	std::vector<SyncCarryReference> deleted;
	bool noRequestInFlight = false;
	bool noPendingFiles = false;
	SyncCarrySourceAccess pendingAccess = SyncCarrySourceAccess::Unknown;
	SyncCarrySendIntent pendingSendIntent = SyncCarrySendIntent::Unknown;
	SyncCarryInventory pendingInventory;
	QByteArray targetPendingRecord;
	bool targetPendingStageDurable = false;
	bool skipCleanupConfirmed = false;
	QString neverLandedRequestId;
	bool neverLandedConfirmed = false;
	bool fullResolutionComplete = false;
};

enum class SyncCarryJournalStatus {
	Invalid,
	Valid,
	NewerVersion,
	Wait,
	Retry,
	NeedsReview,
	Superseded,
};

struct SyncCarryJournalResult {
	SyncCarryJournalStatus status = SyncCarryJournalStatus::Invalid;
	SyncCarryJournal journal;
	QByteArray canonical;

	[[nodiscard]] explicit operator bool() const {
		return status == SyncCarryJournalStatus::Valid;
	}
};

[[nodiscard]] SyncCarryJournalResult BeginSyncCarryJournal(
	const SyncCarryJournalStart &input);
[[nodiscard]] SyncCarryJournalAction NextSyncCarryJournalAction(
	const SyncCarryJournal &journal);
[[nodiscard]] SyncCarryJournalEvent SyncCarryEvent(
	const SyncCarryJournal &journal,
	SyncCarryEventKind kind);
[[nodiscard]] SyncCarryJournalResult AdvanceSyncCarryJournal(
	const SyncCarryJournal &journal,
	const SyncCarryJournalEvent &event,
	const QString &currentLogin = {});
enum class SyncCarryFinishRefusal {
	None,
	Scope,
	Version,
	Mode,
	Phase,
	Outcome,
	RequestInFlight,
	IncompleteResolution,
	Action,
	ResolutionScope,
	InvalidWinner,
	WinnerInTarget,
	MarkMismatch,
	AbsentUnproven,
	TargetGone,
	WinnerNotNewer,
	Serialization,
};

[[nodiscard]] SyncCarryFinishRefusal CheckSyncCarryFinishThenFollow(
	const SyncCarryJournal &journal,
	const SyncCarryJournalEvent &event,
	const QString &currentLogin = {});
[[nodiscard]] SyncCarryJournalResult ResumeSyncCarryJournal(
	const SyncCarryJournal &journal,
	const std::optional<SyncLocalState> &state,
	bool stateAbsentConfirmed,
	bool stateDurableConfirmed = false,
	const QString &currentLogin = {});
[[nodiscard]] SyncCarryJournalResult ParseSyncCarryJournal(const QByteArray &json);
[[nodiscard]] SyncCarryJournalResult SerializeSyncCarryJournal(
	const SyncCarryJournal &journal);
[[nodiscard]] std::vector<SyncCarryReference> SyncCarryJournalForwards(
	const SyncCarryJournal &journal);
[[nodiscard]] bool SyncCarryJournalPostsOwnRecord(const SyncCarryJournal &journal);
[[nodiscard]] bool CheckSyncCarryDeletion(
	const SyncCarryJournal &journal,
	const SyncCarryMessage &freshSource,
	const SyncCarryInventory &destination,
	const SyncPlaylistCarryReceipt &playlist = {});
[[nodiscard]] bool CheckSyncCarryOldMarkDeletion(
	const SyncCarryJournal &journal,
	const SyncCarryMarkReadback &oldMark,
	const SyncCarryInventory &destination,
	const SyncPlaylistCarryReceipt &playlist = {});

struct SyncCarryJournalCommit {
	SyncCarryCommit commit;
	QByteArray targetPendingRecord;
};

[[nodiscard]] SyncCarryJournalCommit PrepareSyncCarryJournalCommit(
	const SyncCarryJournal &journal,
	const std::optional<SyncLocalState> &state,
	const SyncCarryInventory &destination,
	const SyncPlaylistCarryReceipt &playlist = {});
[[nodiscard]] bool SyncCarryJournalHoldsWrites(const SyncCarryJournal &journal);
[[nodiscard]] std::optional<SyncChatCache> SyncCarryJournalCacheMark(
	const SyncCarryJournal &journal);

enum class SyncCarryReaderVerdict { Hold, Proceed, WarnAndProceed };

[[nodiscard]] SyncCarryReaderVerdict CheckSyncCarryReaders(
	const SyncCarryJournal &journal,
	const SyncChatEligibilityResult &current);
[[nodiscard]] bool CheckStaleMarkRemoval(
	const AccountKey &account,
	const SyncChatCache &stale,
	const SyncCarryMarkReadback &exactReadback,
	const SyncChatResolution &freshResolution,
	bool fullResolutionComplete);
[[nodiscard]] SyncCarryJournalResult AbandonInterruptedOperation(
	const SyncCarryJournal &journal,
	const AccountKey &account,
	const QString &currentLogin,
	bool explicitlyConfirmed,
	const std::optional<SyncLocalState> &state,
	bool stateAbsentConfirmed,
	const QByteArray &pendingStage,
	bool objectsDurable);

}
