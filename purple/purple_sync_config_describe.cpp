/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_config_describe.h"

#include "purple/purple_config_payload.h"
#include "purple/purple_sync_envelope.h"

#include <algorithm>

namespace Purple {
namespace {

constexpr auto kLargestRecordNumber = uint64_t(9007199254740991ULL);

[[nodiscard]] bool HasConcurrent(const SyncConfigReview &review) {
	const auto &heads = review.plan.classification.heads;
	return std::any_of(heads.begin(), heads.end(), [](const auto &outcome) {
		return outcome.kind == ConfigHeadKind::Concurrent;
	});
}

[[nodiscard]] SyncConfigDescription Say(
		SyncConfigMessage message,
		SyncConfigAction action = SyncConfigAction::None) {
	auto result = SyncConfigDescription();
	result.message = message;
	result.action = action;
	return result;
}

[[nodiscard]] SyncConfigMessage NotPublishable(const SyncSettingsFile &local) {
	return (local.status == SyncSettingsFileStatus::Absent)
		? SyncConfigMessage::NotPublishableAbsent
		: SyncConfigMessage::NotPublishableInvalid;
}

} // namespace

SyncDeviceNameParts SyncDeviceNameOf(
		const QString &platform,
		const QString &install) {
	const auto dash = install.indexOf(u'-');
	return {
		.platform = platform.simplified().left(kSyncDevicePlatformLength),
		.shortId = install.mid(dash + 1).left(kSyncDeviceShortIdLength),
	};
}

SyncDeviceNameParts SyncDeviceNameOf(const SyncConfigHeadRecord &record) {
	return SyncDeviceNameOf(record.platform, record.head.install);
}

std::vector<SyncDeviceNameParts> SyncConfigDeviceNames(
		const SyncConfigReview &review,
		const std::vector<ConfigHead> &heads) {
	auto result = std::vector<SyncDeviceNameParts>();
	result.reserve(heads.size());
	for (const auto &head : heads) {
		const auto record = FindSyncConfigHeadRecord(review, head);
		result.push_back(record
			? SyncDeviceNameOf(*record)
			: SyncDeviceNameOf(QString(), head.install));
	}
	return result;
}

bool SyncSettingsPublishable(
		const SyncConfigReview &review,
		const QString &device,
		const SyncConfigWriter &writer) {
	const auto &file = review.local;
	if (file.status != SyncSettingsFileStatus::Present
		|| file.text.isEmpty()) {
		return false;
	}
	const auto space = FormatSyncSpaceId(QByteArray(16, '\0'));
	const auto install = FormatSyncInstallId(QByteArray(16, '\0'));
	if (!space || !install) {
		return false;
	}
	const auto keep = PlanConfigChoice(
		review.state,
		review.plan,
		std::nullopt);
	const auto built = BuildConfigRecord({
		.text = file.text,
		.parents = (keep && keep->publish)
			? keep->parents
			: std::vector<ConfigVersion>(),
		.space = *space,
		.install = *install,
		.device = device,
		.platform = writer.platform,
		.app = writer.app,
		.seq = kLargestRecordNumber,
		.at = kLargestRecordNumber,
	});
	return built && built.canonical.size() <= kSyncConfigRecordMaximumBytes;
}

bool SyncChoicePublishes(
		const SyncConfigReview &review,
		const std::optional<QString> &chosenRemoteKey) {
	const auto choice = PlanConfigChoice(
		review.state,
		review.plan,
		chosenRemoteKey);
	return choice && choice->publish;
}

SyncConfigMessage SyncConfigChooseMessage(const SyncConfigReview &review) {
	if (review.plan.verdict == ConfigSyncVerdict::Choose) {
		return review.bound
			? SyncConfigMessage::ChooseBound
			: SyncConfigMessage::ChooseUnbound;
	} else if (HasConcurrent(review)) {
		return SyncConfigMessage::ConflictConcurrent;
	}
	return review.bound
		? SyncConfigMessage::ConflictSplitBound
		: SyncConfigMessage::ConflictSplitUnbound;
}

SyncConfigDescription DescribeSyncConfigReview(
		const SyncConfigReview &review,
		bool localPublishable) {
	using Action = SyncConfigAction;
	using Message = SyncConfigMessage;
	switch (review.status) {
	case SyncConfigReviewStatus::Ready:
		break;
	case SyncConfigReviewStatus::NeedsReview:
		return Say((review.bound && !review.state.pending.isEmpty())
			? Message::NeedsReviewWithPending
			: Message::NeedsReview);
	case SyncConfigReviewStatus::Incomplete:
		return Say(Message::Incomplete);
	case SyncConfigReviewStatus::CloneDetected:
		return Say(Message::CloneDetected);
	case SyncConfigReviewStatus::AccountUnavailable:
		return Say(Message::AccountUnavailable);
	case SyncConfigReviewStatus::AccountUnbound:
		return Say(Message::AccountUnbound);
	case SyncConfigReviewStatus::StoreError:
		return Say(Message::StoreError);
	case SyncConfigReviewStatus::InvalidSettings:
		return Say(Message::InvalidSettings);
	}
	const auto &plan = review.plan;
	switch (plan.verdict) {
	case ConfigSyncVerdict::Invalid:
		break;
	case ConfigSyncVerdict::Pending:
		return Say(Message::Pending, Action::FinishSending);
	case ConfigSyncVerdict::Conflict:
	case ConfigSyncVerdict::Choose: {
		auto result = Say(SyncConfigChooseMessage(review), Action::Choose);
		result.devices = SyncConfigDeviceNames(review, plan.offered);
		return result;
	}
	case ConfigSyncVerdict::UpdateReady: {
		const auto record = plan.offered.empty()
			? nullptr
			: FindSyncConfigHeadRecord(review, plan.offered.front());
		if (!record) {
			return Say(Message::UpdateMissing);
		}
		auto result = Say(Message::UpdateReady, Action::ReviewUpdate);
		result.devices = { SyncDeviceNameOf(*record) };
		result.at = record->at;
		return result;
	}
	case ConfigSyncVerdict::Adopt: {
		auto result = review.bound
			? Say(Message::AdoptBound)
			: Say(Message::AdoptUnbound, Action::Join);
		result.devices = SyncConfigDeviceNames(review, plan.same);
		return result;
	}
	case ConfigSyncVerdict::Empty:
		if (!localPublishable) {
			return Say(NotPublishable(review.local));
		}
		return Say(
			review.bound ? Message::EmptyBound : Message::EmptyUnbound,
			Action::Publish);
	case ConfigSyncVerdict::LocalChanges:
		if (!localPublishable) {
			return Say(NotPublishable(review.local));
		}
		return Say(
			((SyncConfigKeyFingerprint(review.state.base)
				!= review.local.fingerprint)
				? Message::LocalChangesEdited
				: Message::LocalChangesOwnStale),
			Action::PublishChanges);
	case ConfigSyncVerdict::UpToDate: {
		auto result = Say(review.heads.empty()
			? Message::UpToDateAlone
			: Message::UpToDateWith);
		result.others = int(review.heads.size());
		return result;
	}
	}
	return Say(Message::InvalidRecords);
}

std::vector<SyncConfigChoice> SyncConfigChoices(
		const SyncConfigReview &review,
		bool localPublishable) {
	const auto update = (review.plan.verdict
		== ConfigSyncVerdict::UpdateReady);
	auto result = std::vector<SyncConfigChoice>();
	for (const auto &head : review.plan.offered) {
		if (const auto record = FindSyncConfigHeadRecord(review, head)) {
			result.push_back({
				.key = head.key,
				.head = int(record - review.heads.data()),
				.publishes = !update && SyncChoicePublishes(review, head.key),
			});
		}
		if (update) {
			break;
		}
	}
	if (!update
		&& localPublishable
		&& PlanConfigChoice(review.state, review.plan, std::nullopt)) {
		result.push_back({
			.publishes = SyncChoicePublishes(review, std::nullopt),
		});
	}
	return result;
}

SyncConfigApplyFailure DescribeSyncConfigApplyFailure(
		const SyncConfigApplyOutcome &outcome) {
	auto result = SyncConfigApplyFailure{
		.status = outcome.status,
		.joined = outcome.joined,
		.historyKept = outcome.historyKept,
		.undoAvailable = outcome.undoAvailable,
		.otherVersionsRemain = outcome.otherVersionsRemain,
	};
	if (outcome.status == SyncConfigApplyStatus::Applied) {
		result.kind = SyncConfigApplyFailureKind::None;
	} else if (outcome.joined && !outcome.wroteFile) {
		result.kind = SyncConfigApplyFailureKind::JoinedNotWritten;
	} else if (outcome.wroteFile) {
		result.kind = (outcome.status == SyncConfigApplyStatus::StoreError)
			? SyncConfigApplyFailureKind::WrittenStateNotSaved
			: SyncConfigApplyFailureKind::WrittenNotReadBack;
	} else {
		result.kind = SyncConfigApplyFailureKind::NothingDone;
	}
	return result;
}

bool SyncConfigUndoFinished(SyncConfigRestoreStatus status) {
	switch (status) {
	case SyncConfigRestoreStatus::Restored:
	case SyncConfigRestoreStatus::Unchanged:
	case SyncConfigRestoreStatus::NotFound:
	case SyncConfigRestoreStatus::FileDidNotExist:
	case SyncConfigRestoreStatus::NotText:
	case SyncConfigRestoreStatus::InvalidReason:
		return true;
	case SyncConfigRestoreStatus::InvalidSettings:
	case SyncConfigRestoreStatus::HistoryError:
	case SyncConfigRestoreStatus::WriteError:
		break;
	}
	return false;
}

} // namespace Purple
