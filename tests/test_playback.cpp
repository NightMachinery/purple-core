/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playback.h"

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

[[nodiscard]] const char *ChatName(PlaybackChat chat) {
	switch (chat) {
	case PlaybackChat::OneToOne: return "one-to-one";
	case PlaybackChat::Self: return "self";
	case PlaybackChat::Other: return "other";
	}
	return "?";
}

// The full matrix: setting on or off, by chat kind, by the direction of the
// message the manual play started on, by the direction of the candidate. Only
// one row confines (on, one-to-one, started on theirs), and in it only your
// own candidates are dropped.
void TestMatrix() {
	Section = "matrix";
	const PlaybackChat chats[] = {
		PlaybackChat::OneToOne,
		PlaybackChat::Self,
		PlaybackChat::Other,
	};
	for (const auto enabled : { true, false }) {
		auto settings = Playback();
		settings.privateChatQueue = enabled;
		for (const auto chat : chats) {
			for (const auto startOutgoing : { false, true }) {
				const auto anchor = AnchorForManualPlay(
					settings,
					chat,
					startOutgoing);
				const auto confined = enabled
					&& chat == PlaybackChat::OneToOne
					&& !startOutgoing;
				if (anchor.confined != confined) {
					std::printf("anchor: enabled=%d chat=%s startOut=%d\n",
						int(enabled), ChatName(chat), int(startOutgoing));
				}
				CHECK(anchor.confined == confined);
				for (const auto candidateOutgoing : { false, true }) {
					const auto keeps = QueueKeeps(anchor, candidateOutgoing);
					const auto expected = !confined || !candidateOutgoing;
					if (keeps != expected) {
						std::printf("keeps: enabled=%d chat=%s startOut=%d "
							"candidateOut=%d\n",
							int(enabled), ChatName(chat), int(startOutgoing),
							int(candidateOutgoing));
					}
					CHECK(keeps == expected);
				}
			}
		}
	}
}

// The cases the design names, spelled out so a reader need not run the matrix
// in their head.
void TestNamedCases() {
	Section = "named cases";
	const auto on = Playback();
	CHECK(on.privateChatQueue);

	// Started on theirs in a private chat: theirs play, yours are skipped.
	const auto theirs = AnchorForManualPlay(on, PlaybackChat::OneToOne, false);
	CHECK(theirs.confined);
	CHECK(QueueKeeps(theirs, false));
	CHECK(!QueueKeeps(theirs, true));

	// Started on yours: today's behaviour.
	const auto yours = AnchorForManualPlay(on, PlaybackChat::OneToOne, true);
	CHECK(!yours.confined);
	CHECK(QueueKeeps(yours, false));
	CHECK(QueueKeeps(yours, true));

	// Saved Messages, groups and channels never confine.
	CHECK(!AnchorForManualPlay(on, PlaybackChat::Self, false).confined);
	CHECK(!AnchorForManualPlay(on, PlaybackChat::Other, false).confined);

	// Off: nothing confines.
	auto off = Playback();
	off.privateChatQueue = false;
	CHECK(!AnchorForManualPlay(off, PlaybackChat::OneToOne, false).confined);

	// A default anchor, which a client holds before any manual play, keeps
	// everything.
	CHECK(QueueKeeps(VoiceQueueAnchor(), false));
	CHECK(QueueKeeps(VoiceQueueAnchor(), true));
}

} // namespace

int main() {
	TestMatrix();
	TestNamedCases();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
