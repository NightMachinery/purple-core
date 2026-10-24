/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_archive_lifecycle.h"

namespace Purple {

enum class ArchiveMediaLifetime { Ordinary, Ttl, ViewOnce, Secret };
enum class ArchiveMediaTransition { Expiry, Consumption, ExpiredReplacement, CacheEviction };
enum class ArchiveAvailableBytes { Unavailable, Partial, Complete };
enum class ArchiveMediaRetention { Unavailable, Partial, NeedsCopy, Owned };

struct ArchiveMediaSource {
	ArchiveStoreEpoch epoch;
	ArchiveMessageKey key;
	QString version;
	QString revision;
	QString contentHash;
	QString mediaIdentity;
	ArchiveGeneration generation;
};

struct ArchiveMediaLifecycleFact {
	ArchiveMediaSource source;
	ArchiveMediaLifetime lifetime = ArchiveMediaLifetime::Ordinary;
	ArchiveMediaTransition transition = ArchiveMediaTransition::Consumption;
	ArchiveAvailableBytes bytes = ArchiveAvailableBytes::Unavailable;
	bool destroysContent = false;
	QString eventId;
	uint64_t order = 0;
	int64 observedAt = 0;
};

struct ArchiveMediaLifecyclePlan {
	ArchiveTransaction transaction;
	std::optional<ArchiveEvent> event;
	ArchiveMediaRetention retention = ArchiveMediaRetention::Unavailable;
	bool captureBeforeDestruction = false;
};

[[nodiscard]] ArchiveMediaLifecyclePlan PlanArchiveMediaLifecycle(
	const ArchiveState &state, const ArchiveStoreEpoch &current,
	const ArchiveMediaLifecycleFact &fact);
[[nodiscard]] ArchiveTransaction PlanArchiveScopedMediaCompletion(
	const ArchiveState &state, const ArchiveStoreEpoch &captured,
	const ArchiveStoreEpoch &current, const QString &jobId,
	const ArchiveMediaResult &result);

}
