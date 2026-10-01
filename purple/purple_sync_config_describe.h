/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_sync_config_flow.h"

#include <QtCore/QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace Purple {

inline constexpr auto kSyncDevicePlatformLength = 32;
inline constexpr auto kSyncDeviceShortIdLength = 4;

struct SyncDeviceNameParts {
	QString platform;
	QString shortId;

	friend bool operator==(
		const SyncDeviceNameParts &,
		const SyncDeviceNameParts &) = default;
};

[[nodiscard]] SyncDeviceNameParts SyncDeviceNameOf(
	const QString &platform,
	const QString &install);
[[nodiscard]] SyncDeviceNameParts SyncDeviceNameOf(
	const SyncConfigHeadRecord &record);
[[nodiscard]] std::vector<SyncDeviceNameParts> SyncConfigDeviceNames(
	const SyncConfigReview &review,
	const std::vector<ConfigHead> &heads);

[[nodiscard]] bool SyncSettingsPublishable(
	const SyncConfigReview &review,
	const QString &device,
	const SyncConfigWriter &writer);
[[nodiscard]] bool SyncChoicePublishes(
	const SyncConfigReview &review,
	const std::optional<QString> &chosenRemoteKey);

enum class SyncConfigAction {
	None,
	Publish,
	Join,
	ReviewUpdate,
	Choose,
	PublishChanges,
	FinishSending,
};

enum class SyncConfigMessage {
	NeedsReviewWithPending,
	NeedsReview,
	Incomplete,
	CloneDetected,
	AccountUnavailable,
	AccountUnbound,
	StoreError,
	InvalidSettings,
	InvalidRecords,
	Pending,
	ChooseBound,
	ChooseUnbound,
	ConflictConcurrent,
	ConflictSplitBound,
	ConflictSplitUnbound,
	UpdateReady,
	UpdateMissing,
	AdoptBound,
	AdoptUnbound,
	NotPublishableAbsent,
	NotPublishableInvalid,
	EmptyBound,
	EmptyUnbound,
	LocalChangesEdited,
	LocalChangesOwnStale,
	UpToDateAlone,
	UpToDateWith,
	UsingLastGood,
	UsingLastGoodWithPending,
};

struct SyncConfigDescription {
	SyncConfigMessage message = SyncConfigMessage::InvalidRecords;
	SyncConfigAction action = SyncConfigAction::None;
	std::vector<SyncDeviceNameParts> devices;
	uint64_t at = 0;
	int others = 0;
};

[[nodiscard]] SyncConfigMessage SyncConfigChooseMessage(
	const SyncConfigReview &review);
[[nodiscard]] SyncConfigDescription DescribeSyncConfigReview(
	const SyncConfigReview &review,
	bool localPublishable);

struct SyncConfigChoice {
	std::optional<QString> key;
	int head = -1;
	bool publishes = false;
};

[[nodiscard]] std::vector<SyncConfigChoice> SyncConfigChoices(
	const SyncConfigReview &review,
	bool localPublishable);

struct SyncConfigApplyOutcome {
	SyncConfigApplyStatus status = SyncConfigApplyStatus::NeedsReview;
	bool joined = false;
	bool wroteFile = false;
	bool historyKept = false;
	bool undoAvailable = false;
	bool otherVersionsRemain = false;
};

enum class SyncConfigApplyFailureKind {
	None,
	JoinedNotWritten,
	WrittenStateNotSaved,
	WrittenNotReadBack,
	NothingDone,
};

struct SyncConfigApplyFailure {
	SyncConfigApplyFailureKind kind = SyncConfigApplyFailureKind::None;
	SyncConfigApplyStatus status = SyncConfigApplyStatus::Applied;
	bool joined = false;
	bool historyKept = false;
	bool undoAvailable = false;
	bool otherVersionsRemain = false;
};

[[nodiscard]] SyncConfigApplyFailure DescribeSyncConfigApplyFailure(
	const SyncConfigApplyOutcome &outcome);
[[nodiscard]] bool SyncConfigUndoFinished(SyncConfigRestoreStatus status);

} // namespace Purple
