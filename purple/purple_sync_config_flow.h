/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_config_sync.h"
#include "purple/purple_sync_inventory.h"
#include "purple/purple_sync_local_state.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace Purple {

inline constexpr auto kSyncSettingsMaximumBytes = 256 * 1024;
inline constexpr auto kSyncConfigRecordMaximumBytes = 256 * 1024;

enum class SyncSettingsFileStatus {
	Present,
	Absent,
	Invalid,
};

struct SyncSettingsFile {
	SyncSettingsFileStatus status = SyncSettingsFileStatus::Invalid;
	QByteArray text;
	QString fingerprint;
};

[[nodiscard]] SyncSettingsFile MakeSyncSettingsFile(
	SyncSettingsFileStatus status,
	const QByteArray &text = QByteArray());
[[nodiscard]] bool SameSyncSettingsFile(
	const SyncSettingsFile &a,
	const SyncSettingsFile &b);
[[nodiscard]] bool SyncSettingsTextWritable(const QByteArray &bytes);
[[nodiscard]] QString SyncConfigKeyFingerprint(const QString &key);

struct SyncConfigHeadRecord {
	ConfigHead head;
	int32_t messageId = 0;
	QByteArray text;
	QString device;
	QString platform;
	QString app;
	uint64_t at = 0;
	bool newerSchema = false;
};

enum class SyncConfigHeadsStatus {
	Complete,
	NeedsReview,
};

struct SyncConfigHeads {
	SyncConfigHeadsStatus status = SyncConfigHeadsStatus::NeedsReview;
	std::vector<SyncConfigHeadRecord> heads;
};

struct SyncConfigOwnHead {
	bool valid = false;
	std::optional<SyncConfigHeadRecord> head;
};

enum class SyncConfigReviewStatus {
	Ready,
	NeedsReview,
	Incomplete,
	CloneDetected,
	AccountUnavailable,
	AccountUnbound,
	StoreError,
	InvalidSettings,
};

struct SyncConfigReview {
	SyncConfigReviewStatus status = SyncConfigReviewStatus::Incomplete;
	uint64_t accountUserId = 0;
	bool bound = false;
	QString space;
	ConfigSyncState state;
	SyncSettingsFile local;
	std::vector<SyncConfigHeadRecord> heads;
	std::optional<SyncConfigHeadRecord> ownHead;
	ConfigSyncPlan plan;
};

[[nodiscard]] ConfigSyncState SyncConfigStateOf(const SyncLocalState &state);
[[nodiscard]] SyncLocalConfigState SyncLocalConfigOf(
	const ConfigSyncState &state);
[[nodiscard]] std::vector<ConfigHead> SyncConfigHeadsOf(
	const std::vector<SyncConfigHeadRecord> &records);
[[nodiscard]] std::vector<QString> SyncConfigVersionKeys(
	const std::vector<ConfigVersion> &versions);
[[nodiscard]] bool SameSyncConfigKeySet(
	std::vector<QString> a,
	std::vector<QString> b);
void SelectSyncSpaceIfEmpty(
	SyncAccountInventoryResult &inventory,
	const QString &space);

[[nodiscard]] std::optional<SyncConfigHeadRecord> ParseSyncConfigHead(
	const SyncCandidateRecord &record);
[[nodiscard]] SyncConfigHeads ExtractSyncConfigHeads(
	const SyncAccountInventoryResult &inventory,
	const QString &space,
	const QString &ownInstall);
[[nodiscard]] SyncConfigOwnHead FindSyncConfigOwnHead(
	const SyncAccountInventoryResult &inventory,
	const SyncOwnInventoryResult &own);
[[nodiscard]] const SyncConfigHeadRecord *FindSyncConfigHeadRecord(
	const SyncConfigReview &review,
	const ConfigHead &head);

[[nodiscard]] SyncConfigReview ReviewSyncConfigInventory(
	const SyncAccountInventoryResult &inventory,
	const SyncLocalState *state,
	const QByteArray &stagedRecord,
	const SyncSettingsFile &local);

[[nodiscard]] QString SyncConfigReviewStamp(const SyncConfigReview &review);
[[nodiscard]] SyncConfigReview SyncConfigJoinedReview(
	const SyncConfigReview &shown,
	const SyncLocalState &joined);

enum class SyncConfigApplyStatus {
	Applied,
	NeedsRecheck,
	NeedsReview,
	InvalidChoice,
	AccountUnavailable,
	AccountUnbound,
	SetupFailed,
	StoreError,
	InvalidSettings,
	HistoryError,
	WriteError,
};

enum class SyncConfigApplyPlanStatus {
	Ready,
	NeedsRecheck,
	NeedsReview,
	InvalidChoice,
};

struct SyncConfigApplyPlan {
	SyncConfigApplyPlanStatus status = SyncConfigApplyPlanStatus::NeedsReview;
	bool join = false;
	ConfigSyncState state;
	std::vector<ConfigHead> heads;
	std::optional<ConfigHead> ownHead;
	ConfigSyncVerdict verdict = ConfigSyncVerdict::Invalid;
	ConfigChoicePlan choice;
	std::optional<SyncConfigHeadRecord> source;
	QString localFingerprint;
	QString writeFingerprint;
	QString versionKey;
	bool update = false;
	bool otherVersionsRemain = false;
};

[[nodiscard]] SyncConfigApplyPlanStatus CheckSyncConfigApplyChoice(
	const SyncConfigReview &shown,
	const std::optional<QString> &chosenRemoteKey);
[[nodiscard]] SyncConfigApplyPlan PlanSyncConfigApply(
	const SyncConfigReview &fresh,
	const QString &expectedStamp,
	const std::optional<QString> &chosenRemoteKey);

enum class SyncConfigApplyCompletionStatus {
	Ready,
	InvalidPlan,
	ReadBackMismatch,
	AdoptRefused,
};

struct SyncConfigApplyCompletion {
	SyncConfigApplyCompletionStatus status
		= SyncConfigApplyCompletionStatus::InvalidPlan;
	QString fingerprint;
	std::optional<ConfigSyncState> adopted;
	ConfigSyncVerdict nextVerdict = ConfigSyncVerdict::Invalid;
	bool publishNeeded = false;
	std::vector<QString> expectedParents;
	bool promiseKept = true;
};

[[nodiscard]] SyncConfigApplyCompletion CompleteSyncConfigApply(
	const SyncConfigApplyPlan &plan,
	const SyncSettingsFile &current);

enum class SyncConfigCommitStatus {
	Ready,
	Unchanged,
	InvalidTransition,
	InvalidState,
};

struct SyncConfigCommitCheck {
	SyncConfigCommitStatus status = SyncConfigCommitStatus::InvalidTransition;
	SyncLocalError error = SyncLocalError::None;
	SyncLocalState state;
	QByteArray canonical;
};

[[nodiscard]] SyncConfigCommitCheck CheckSyncConfigDataCommit(
	const SyncLocalState &current,
	const SyncLocalConfigState &next);

struct SyncConfigPublishRequest {
	bool pendingOnly = false;
	std::optional<QString> expectedFingerprint;
	std::optional<std::vector<QString>> expectedParents;
};

enum class SyncConfigPublishEntry {
	Refuse,
	FinishStaged,
	NewContent,
};

enum class SyncConfigPublishGateStatus {
	Proceed,
	AlreadySynced,
	NeedsReview,
};

struct SyncConfigPublishGate {
	SyncConfigPublishGateStatus status
		= SyncConfigPublishGateStatus::NeedsReview;
	ConfigSyncPlan plan;
	std::vector<ConfigVersion> parents;
};

[[nodiscard]] SyncConfigPublishEntry PlanSyncConfigPublishEntry(
	const SyncConfigPublishRequest &request,
	bool staged);
[[nodiscard]] SyncConfigPublishGate PlanSyncConfigPublishGate(
	const SyncLocalState &state,
	const SyncAccountInventoryResult &inventory,
	const SyncOwnInventoryResult &own,
	const QString &localFingerprint,
	const SyncConfigPublishRequest &request);

enum class SyncConfigPublishStatus {
	Confirmed,
	AlreadySynced,
	NeedsReview,
	CloneDetected,
	Incomplete,
	AccountUnavailable,
	AccountUnbound,
	StoreError,
	InvalidSettings,
	OutcomeUnknown,
	Cancelled,
	StillSending,
};

enum class SyncConfigSendQueue {
	Empty,
	HoldsSyncRecord,
};

enum class SyncConfigPostStep {
	Finish,
	ConfirmFound,
	Stage,
	Post,
};

struct SyncConfigPostPlan {
	SyncConfigPostStep step = SyncConfigPostStep::Finish;
	SyncConfigPublishStatus status = SyncConfigPublishStatus::NeedsReview;
	int32_t messageId = 0;
	QByteArray record;
	SyncLocalConfigState nextConfigData;
	SyncOwnInventoryResult own;
};

struct SyncConfigWriter {
	QString platform;
	QString app;
};

[[nodiscard]] SyncConfigPostPlan PlanSyncConfigPost(
	const SyncLocalState &state,
	const QByteArray &bindingToken,
	const QByteArray &stagedRecord,
	const SyncAccountInventoryResult &inventory,
	const SyncSettingsFile &local,
	const SyncConfigPublishRequest &request,
	int64_t now,
	const SyncConfigWriter &writer,
	SyncConfigSendQueue queue);
[[nodiscard]] SyncConfigPostPlan PlanSyncConfigStagedPost(
	const SyncLocalState &state,
	const QByteArray &bindingToken,
	const SyncOwnInventoryResult &own,
	const QByteArray &stagedRecord);

enum class SyncConfigRestoreStatus {
	Restored,
	Unchanged,
	NotFound,
	FileDidNotExist,
	NotText,
	InvalidReason,
	InvalidSettings,
	HistoryError,
	WriteError,
};

} // namespace Purple
