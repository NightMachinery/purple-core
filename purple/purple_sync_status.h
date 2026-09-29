/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

namespace Purple {

enum class SyncStatus {
	Off,
	ManuallyPaused,
	NeedsChoice,
	PausedForProblem,
	UpdateReady,
	NotSyncingFileError,
	CantSync,
	WaitingForConnection,
	Syncing,
	UpToDate,
};

enum class SyncAttentionTier {
	StatusOnly,
	Notice,
	Card,
};

enum class SyncPrimaryAction {
	None,
	TurnOn,
	Resume,
	ChooseSettings,
	ChooseAccount,
	KeepSyncing,
	UpdateClient,
	ReviewUpdate,
	OpenDetails,
};

enum class SyncProblem {
	None,
	AccountSignedOut,
	OwnMessageDeleted,
	NewerClientNeeded,
	Other,
};

struct SyncStatusFacts {
	bool enabled = false;
	bool manuallyPaused = false;
	bool needsChoice = false;
	SyncProblem problem = SyncProblem::None;
	bool updateReady = false;
	bool fileError = false;
	bool cantSync = false;
	bool waitingForConnection = false;
	bool syncing = false;
	bool attentionDue = false;
};

struct SyncStatusResult {
	SyncStatus status = SyncStatus::Off;
	SyncAttentionTier tier = SyncAttentionTier::StatusOnly;
	SyncPrimaryAction primaryAction = SyncPrimaryAction::None;
};

[[nodiscard]] SyncStatusResult ResolveSyncStatus(
	const SyncStatusFacts &facts);

} // namespace Purple
