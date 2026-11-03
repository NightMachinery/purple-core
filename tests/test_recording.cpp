/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_recording.h"

#include <cstdio>

using namespace Purple;

namespace {
auto Checks = 0;
auto Failures = 0;
const char *Section = "";
void Report(bool condition, const char *expression, int line) {
	++Checks;
	if (!condition) { ++Failures; std::printf("FAIL %s:%d %s\n", Section, line, expression); std::fflush(stdout); }
}
#define CHECK(condition) Report(bool(condition), #condition, __LINE__)

constexpr auto kMinute = std::int64_t(60 * 1000);

void TestNudge() {
	Section = "nudge";
	const auto interval = 30 * 60;
	CHECK(NextNudgeAt(0, interval) == 30 * kMinute);
	CHECK(NextNudgeAt(29 * kMinute, interval) == 30 * kMinute);
	CHECK(NextNudgeAt(30 * kMinute - 1, interval) == 30 * kMinute);
	// At the nudge itself the next one is the following multiple.
	CHECK(NextNudgeAt(30 * kMinute, interval) == 60 * kMinute);
	CHECK(NextNudgeAt(61 * kMinute, interval) == 90 * kMinute);
	CHECK(NextNudgeAt(-5, interval) == 30 * kMinute);
	CHECK(!NextNudgeAt(10 * kMinute, 0).has_value());
	CHECK(!NextNudgeAt(10 * kMinute, -1).has_value());
	CHECK(NextNudgeAt(0, 60) == kMinute);

	// Polling once a second fires exactly once per multiple.
	auto fired = 0;
	auto firstAt = std::int64_t(-1);
	for (auto t = std::int64_t(1000); t <= 95 * kMinute; t += 1000) {
		if (NudgeDue(t - 1000, t, interval)) {
			if (firstAt < 0) {
				firstAt = t;
			}
			++fired;
		}
	}
	CHECK(fired == 3);
	CHECK(firstAt == 30 * kMinute);

	CHECK(NudgeDue(30 * kMinute - 1, 30 * kMinute, interval));
	CHECK(!NudgeDue(30 * kMinute, 30 * kMinute + 1, interval));
	// A late tick that crosses a multiple still fires once.
	CHECK(NudgeDue(25 * kMinute, 40 * kMinute, interval));
	// Backwards or standing still never fires.
	CHECK(!NudgeDue(31 * kMinute, 29 * kMinute, interval));
	CHECK(!NudgeDue(30 * kMinute, 30 * kMinute, interval));
	CHECK(!NudgeDue(29 * kMinute, 31 * kMinute, 0));

	auto settings = Recording();
	CHECK(NudgeDue(settings, 30 * kMinute - 1, 30 * kMinute));
	settings.longNudgeSeconds = 0;
	CHECK(!NudgeDue(settings, 30 * kMinute - 1, 30 * kMinute));
	settings.longNudgeSeconds = 10 * 60;
	CHECK(NudgeDue(settings, 10 * kMinute - 1, 10 * kMinute));
}

void TestBinExpiry() {
	Section = "bin expiry";
	const auto binned = std::int64_t(1800000000);
	const auto keep = 48 * 3600;
	CHECK(BinItemExpiresAt(binned, keep) == binned + keep);
	CHECK(!BinItemExpired(binned, binned, keep));
	CHECK(!BinItemExpired(binned, binned + keep - 1, keep));
	// Exactly bin_keep old: expired.
	CHECK(BinItemExpired(binned, binned + keep, keep));
	CHECK(BinItemExpired(binned, binned + keep + 1, keep));
	// "off": never.
	CHECK(!BinItemExpiresAt(binned, 0).has_value());
	CHECK(!BinItemExpired(binned, binned + 10LL * 365 * 86400, 0));
	// The clock moved back: kept.
	CHECK(!BinItemExpired(binned, binned - 1, keep));
	CHECK(!BinItemExpired(binned, binned - 100LL * 86400, 3600));

	auto settings = Recording();
	CHECK(!BinItemExpired(settings, binned, binned + keep - 1));
	CHECK(BinItemExpired(settings, binned, binned + keep));
	settings.binKeepSeconds = 7 * 86400;
	CHECK(!BinItemExpired(settings, binned, binned + keep));
	CHECK(BinItemExpired(settings, binned, binned + 7 * 86400));
	settings.binKeepSeconds = 0;
	CHECK(!BinItemExpired(settings, binned, binned + 7 * 86400));
	CHECK(kBinSweepIntervalSeconds == 3600);
}

void TestSeek() {
	Section = "seek";
	const auto duration = std::int64_t(60000);
	const auto back = SeekDirection::Back;
	const auto forward = SeekDirection::Forward;
	CHECK(SeekTarget(30000, duration, 10, back) == 20000);
	CHECK(SeekTarget(30000, duration, 10, forward) == 40000);
	// Clamped at 0.
	CHECK(SeekTarget(4000, duration, 10, back) == 0);
	CHECK(SeekTarget(10000, duration, 10, back) == 0);
	CHECK(SeekTarget(0, duration, 10, back) == 0);
	CHECK(SeekTarget(-50, duration, 10, back) == 0);
	// Clamped one second before the end.
	CHECK(SeekTarget(55000, duration, 10, forward) == 59000);
	CHECK(SeekTarget(49000, duration, 10, forward) == 59000);
	CHECK(SeekTarget(48999, duration, 10, forward) == 58999);
	// Already inside the last second: forward does not move backwards.
	CHECK(SeekTarget(59500, duration, 10, forward) == 59500);
	CHECK(SeekTarget(61000, duration, 10, forward) == 61000);
	// A message shorter than the margin.
	CHECK(SeekTarget(200, 800, 10, forward) == 200);
	CHECK(SeekTarget(0, 800, 10, forward) == 0);
	// Unknown duration: forward adds the step.
	CHECK(SeekTarget(5000, 0, 10, forward) == 15000);
	CHECK(SeekTarget(5000, -1, 30, forward) == 35000);

	// The step comes from [playback] seek_step.
	auto settings = Playback();
	CHECK(SeekTarget(settings, 30000, duration, back) == 20000);
	for (const auto step : { 5, 10, 15, 30 }) {
		settings.seekStepSeconds = step;
		CHECK(SeekTarget(settings, 30000, 120000, forward) == 30000 + step * 1000);
		CHECK(SeekTarget(settings, 30000, 120000, back) == 30000 - step * 1000);
	}
	CHECK(SeekTarget(30000, duration, 0, forward) == 30000);
}

} // namespace

int main() {
	TestNudge();
	TestBinExpiry();
	TestSeek();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
