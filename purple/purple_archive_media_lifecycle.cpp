/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_archive_media_lifecycle.h"

#include <algorithm>

namespace Purple {
namespace {

ArchiveTransaction Refuse(ArchivePlanStatus status) {
	auto result = ArchiveTransaction();
	result.status = status;
	return result;
}

}

ArchiveMediaLifecyclePlan PlanArchiveMediaLifecycle(const ArchiveState &state,
		const ArchiveStoreEpoch &current, const ArchiveMediaLifecycleFact &fact) {
	auto result = ArchiveMediaLifecyclePlan();
	const auto &source = fact.source;
	if (!CanRunArchiveWork(source.epoch, current) || current.account != state.account
		|| source.key.account != state.account
		|| source.generation != ArchiveGenerationFor(state, source.key.chat)) {
		result.transaction = Refuse(ArchivePlanStatus::Stale);
		return result;
	}
	if (!IsArchiveState(state) || !IsArchiveMessageKey(source.key)
		|| !IsArchiveToken(source.version) || !IsArchiveToken(source.revision)
		|| !IsArchiveHash(source.contentHash) || !IsArchiveToken(source.mediaIdentity)
		|| int(fact.lifetime) < 0 || int(fact.lifetime) > int(ArchiveMediaLifetime::Secret)
		|| int(fact.transition) < 0 || int(fact.transition) > int(ArchiveMediaTransition::CacheEviction)
		|| int(fact.bytes) < 0 || int(fact.bytes) > int(ArchiveAvailableBytes::Complete)
		|| (fact.lifetime == ArchiveMediaLifetime::Secret && source.key.chat.kind != ArchiveChatKind::Secret)) {
		return result;
	}
	const auto message = FindArchiveMessage(state, source.key);
	const auto version = FindArchiveVersion(state, source.key, source.version);
	if (!message || !version || message->versions.empty() || message->payloadPurged
		|| message->versions.back().id != source.version || version->revision != source.revision
		|| ArchiveContentHash(version->content) != source.contentHash) {
		result.transaction = Refuse(ArchivePlanStatus::Stale);
		return result;
	}
	const auto media = std::find_if(version->media.begin(), version->media.end(),
		[&](const auto &entry) { return entry.identity == source.mediaIdentity; });
	if (media == version->media.end()) { return result; }
	result.retention = media->state == ArchiveMediaState::Complete ? ArchiveMediaRetention::Owned
		: fact.bytes == ArchiveAvailableBytes::Complete ? ArchiveMediaRetention::NeedsCopy
		: fact.bytes == ArchiveAvailableBytes::Partial ? ArchiveMediaRetention::Partial
		: ArchiveMediaRetention::Unavailable;
	if (fact.transition == ArchiveMediaTransition::CacheEviction
		|| (fact.transition == ArchiveMediaTransition::Consumption && !fact.destroysContent)) {
		result.transaction = Refuse(ArchivePlanStatus::NoChange);
		return result;
	}
	if (!fact.destroysContent || fact.lifetime == ArchiveMediaLifetime::Ordinary
		|| !IsArchiveToken(fact.eventId) || !fact.order || fact.observedAt < 0) {
		return result;
	}
	auto event = ArchiveEvent();
	event.key = source.key;
	event.id = fact.eventId;
	event.order = fact.order;
	event.origin = fact.transition == ArchiveMediaTransition::Consumption
		? ArchiveOrigin::MediaConsumption : ArchiveOrigin::MediaExpiry;
	event.generation = source.generation;
	event.targetVersion = source.version;
	event.targetRevision = source.revision;
	event.expectedContentHash = source.contentHash;
	event.lifecycleEpoch = source.epoch;
	event.lifecycleMediaIdentity = source.mediaIdentity;
	event.observation = version->observation;
	event.observation.observedAt = fact.observedAt;
	result.transaction = PlanArchiveEvent(state, event);
	if (result.transaction.status == ArchivePlanStatus::Ready
		|| result.transaction.status == ArchivePlanStatus::NoChange) {
		result.event = std::move(event);
		result.captureBeforeDestruction = true;
	}
	return result;
}

ArchiveTransaction PlanArchiveScopedMediaCompletion(const ArchiveState &state,
		const ArchiveStoreEpoch &captured, const ArchiveStoreEpoch &current,
		const QString &jobId, const ArchiveMediaResult &result) {
	if (!CanRunArchiveWork(captured, current) || current.account != state.account) {
		auto refused = Refuse(ArchivePlanStatus::Stale);
		refused.mediaRefusal = ArchiveMediaRefusal::StoreEpochMismatch;
		return refused;
	}
	return PlanArchiveMediaCompletion(state, jobId, result);
}

}
