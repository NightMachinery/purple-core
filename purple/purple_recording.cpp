/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_recording.h"

#include <algorithm>

namespace Purple {

std::optional<std::int64_t> NextNudgeAt(
		std::int64_t elapsedMs,
		int intervalSeconds) {
	if (intervalSeconds <= 0) {
		return std::nullopt;
	}
	const auto interval = std::int64_t(intervalSeconds) * 1000;
	if (elapsedMs < 0) {
		return interval;
	}
	return (elapsedMs / interval + 1) * interval;
}

bool NudgeDue(
		std::int64_t previousMs,
		std::int64_t elapsedMs,
		int intervalSeconds) {
	if (elapsedMs <= previousMs) {
		return false;
	}
	const auto next = NextNudgeAt(previousMs, intervalSeconds);
	return next && (*next <= elapsedMs);
}

bool NudgeDue(
		const Recording &settings,
		std::int64_t previousMs,
		std::int64_t elapsedMs) {
	return NudgeDue(previousMs, elapsedMs, settings.longNudgeSeconds);
}

std::optional<std::int64_t> BinItemExpiresAt(
		std::int64_t binnedAt,
		int keepSeconds) {
	if (keepSeconds <= 0) {
		return std::nullopt;
	}
	return binnedAt + keepSeconds;
}

bool BinItemExpired(
		std::int64_t binnedAt,
		std::int64_t now,
		int keepSeconds) {
	const auto expires = BinItemExpiresAt(binnedAt, keepSeconds);
	return expires && (now >= binnedAt) && (now >= *expires);
}

bool BinItemExpired(
		const Recording &settings,
		std::int64_t binnedAt,
		std::int64_t now) {
	return BinItemExpired(binnedAt, now, settings.binKeepSeconds);
}

std::int64_t SeekTarget(
		std::int64_t positionMs,
		std::int64_t durationMs,
		int stepSeconds,
		SeekDirection direction) {
	const auto position = std::max<std::int64_t>(positionMs, 0);
	const auto step = std::int64_t(std::max(stepSeconds, 0)) * 1000;
	if (direction == SeekDirection::Back) {
		return std::max<std::int64_t>(position - step, 0);
	}
	const auto target = position + step;
	if (durationMs <= 0) {
		return target;
	}
	const auto last = std::max<std::int64_t>(durationMs - kSeekEndMarginMs, 0);
	return std::max(position, std::min(target, last));
}

std::int64_t SeekTarget(
		const Playback &settings,
		std::int64_t positionMs,
		std::int64_t durationMs,
		SeekDirection direction) {
	return SeekTarget(
		positionMs,
		durationMs,
		settings.seekStepSeconds,
		direction);
}

} // namespace Purple
