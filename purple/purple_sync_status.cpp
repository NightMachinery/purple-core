/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_status.h"

namespace Purple {

SyncStatusResult ResolveSyncStatus(const SyncStatusFacts &facts) {
	using Status = SyncStatus;
	using Tier = SyncAttentionTier;
	using Action = SyncPrimaryAction;

	if (!facts.enabled) {
		return { Status::Off, Tier::StatusOnly, Action::TurnOn };
	}
	if (facts.manuallyPaused) {
		return { Status::ManuallyPaused, Tier::StatusOnly, Action::Resume };
	}
	if (facts.needsChoice) {
		return { Status::NeedsChoice, Tier::Card, Action::ChooseSettings };
	}
	if (facts.problem != SyncProblem::None) {
		auto action = Action::OpenDetails;
		switch (facts.problem) {
		case SyncProblem::None: break;
		case SyncProblem::AccountSignedOut:
			action = Action::ChooseAccount;
			break;
		case SyncProblem::OwnMessageDeleted:
			action = Action::KeepSyncing;
			break;
		case SyncProblem::NewerClientNeeded:
			action = Action::UpdateClient;
			break;
		case SyncProblem::Other: break;
		}
		return { Status::PausedForProblem, Tier::Card, action };
	}
	if (facts.updateReady) {
		return { Status::UpdateReady, Tier::Notice, Action::ReviewUpdate };
	}
	if (facts.fileError) {
		return { Status::NotSyncingFileError,
			facts.attentionDue ? Tier::Notice : Tier::StatusOnly,
			Action::OpenDetails };
	}
	if (facts.cantSync) {
		return { Status::CantSync,
			facts.attentionDue ? Tier::Notice : Tier::StatusOnly,
			Action::OpenDetails };
	}
	if (facts.waitingForConnection) {
		return { Status::WaitingForConnection,
			facts.attentionDue ? Tier::Notice : Tier::StatusOnly,
			facts.attentionDue ? Action::OpenDetails : Action::None };
	}
	if (facts.syncing) {
		return { Status::Syncing, Tier::StatusOnly, Action::None };
	}
	return { Status::UpToDate, Tier::StatusOnly, Action::None };
}

} // namespace Purple
