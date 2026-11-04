/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_settings.h"

#include <cstdint>
#include <optional>

// The small recording rules both clients ask, so they are stated once: when
// the long-recording nudge fires (extra 5), when a Recording bin item expires
// (F10), and where a seek step lands (F9). Pure: the clients own the clocks,
// the timers and the players. See docs/media-recording-design.md.
namespace Purple {

[[nodiscard]] bool KeepRecordingOnPause(
	const Recording &settings,
	bool round,
	bool background,
	bool microphoneServiceReady);

[[nodiscard]] bool CanDockVoice(
	const Recording &settings,
	bool round,
	bool listening,
	bool trimmedResume);

// --- The long-recording nudge ([recording] long_nudge) ---

// The recorded time, in milliseconds, of the next nudge after `elapsedMs':
// the first whole multiple of the interval strictly after it, so a nudge
// fires at 30:00, 60:00, 90:00 and so on with the default. Recorded time
// excludes pauses. Nothing when the interval is 0 ("off") or negative.
[[nodiscard]] std::optional<std::int64_t> NextNudgeAt(
	std::int64_t elapsedMs,
	int intervalSeconds);

// Whether a nudge falls in (previousMs, elapsedMs]: what a client that polls
// its recorded time asks on each tick. It is true once per crossed multiple,
// even when a tick is late, and never for a tick that goes backwards.
[[nodiscard]] bool NudgeDue(
	std::int64_t previousMs,
	std::int64_t elapsedMs,
	int intervalSeconds);

// The convenience form that reads Settings::recording.
[[nodiscard]] bool NudgeDue(
	const Recording &settings,
	std::int64_t previousMs,
	std::int64_t elapsedMs);

// --- Recording bin expiry ([recording] bin_keep) ---

// How often a running app sweeps the bin, besides once at start.
inline constexpr int kBinSweepIntervalSeconds = 3600;

// When an item that entered the bin at `binnedAt' (Unix seconds: the end of
// the recording, or its recovery after a crash, so a crash found days later
// still gets the whole keep time) expires. Nothing when the keep time is 0
// ("off", never clear) or negative.
[[nodiscard]] std::optional<std::int64_t> BinItemExpiresAt(
	std::int64_t binnedAt,
	int keepSeconds);

// Whether that item is expired at `now': at or after the expiry, so an item
// exactly bin_keep old is deleted. An item from the future (the clock moved
// back) is kept.
[[nodiscard]] bool BinItemExpired(
	std::int64_t binnedAt,
	std::int64_t now,
	int keepSeconds);

[[nodiscard]] bool BinItemExpired(
	const Recording &settings,
	std::int64_t binnedAt,
	std::int64_t now);

// --- Seek back and forward ([playback] seek_step) ---

enum class SeekDirection {
	Back,
	Forward,
};

// How close to the end a forward seek may land, so the queue does not
// advance by surprise.
inline constexpr std::int64_t kSeekEndMarginMs = 1000;

// Where a seek by `stepSeconds' from `positionMs' lands, in milliseconds.
// Back stops at 0. Forward stops one second before the end
// (`durationMs - kSeekEndMarginMs', at least 0) and never moves backwards:
// from inside that last second it stays where it is. A duration of 0 or less
// is unknown, and forward then only adds the step.
[[nodiscard]] std::int64_t SeekTarget(
	std::int64_t positionMs,
	std::int64_t durationMs,
	int stepSeconds,
	SeekDirection direction);

[[nodiscard]] std::int64_t SeekTarget(
	const Playback &settings,
	std::int64_t positionMs,
	std::int64_t durationMs,
	SeekDirection direction);

} // namespace Purple
