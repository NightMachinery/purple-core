/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_settings.h"
#include "purple/purple_types.h"

// state.toml, the machine-owned half of the configuration. Everything here is
// rewritten whenever it changes and carries no comments, which is exactly why
// it is a separate file: state churns constantly, and it must never touch the
// mtime of the settings.toml the user is editing by hand.
namespace Purple {

// What put the current preset in place. The distinction is load-bearing rather
// than informational: a schedule boundary aiming at Normal is skipped when the
// user chose the current preset themselves. See spec 6.2.
enum class PresetSource : uchar {
	Manual,
	Schedule,
	Focus,
};

[[nodiscard]] QString PresetSourceName(PresetSource source);
[[nodiscard]] PresetSource PresetSourceFromName(const QString &name);

struct ResolvedList {
	QString list;

	// Unset is meaningful and has to survive the round trip: it is what makes
	// the entry take the default for whatever kind the chat turns out to be.
	std::optional<ShowMode> show;

	bool notify = true;

	// Unset here too, and for the same reason: it is what leaves this entry's
	// people to the preset's own stories policy.
	std::optional<StoryMode> stories;
};

struct ResolvedCacheView {
	QString name;
	std::vector<PeerIdValue> pinned;
	std::vector<ResolvedList> lists;
};

// The last resolution that worked. When a reload leaves the active preset
// unresolvable - deleted or renamed mid-edit - the engine keeps running on this
// rather than falling back to showing everything. See spec 8.5.
struct ResolvedCache {
	QString preset;

	// Cached alongside the rest so a broken reload does not also rename the
	// tab. Absent in a file written by an older build, which is why reading it
	// falls back to the preset name rather than to an empty label.
	QString viewName;

	std::vector<ResolvedList> lists;
	bool hideEverywhere = false;

	// True in a file written by an older build, which is also what a preset
	// saying nothing asks for - so an upgrade changes nothing here.
	bool hideArchive = true;

	// The main view's own pinned order. Empty means the preset mirrors the
	// account's, which is the default and what an older file restores as.
	std::vector<PeerIdValue> pinned;

	// What the stories strip does. Follow in a file written by an older build,
	// which is also the default, so an upgrade changes nothing here.
	StoryPolicy stories = StoryPolicy::Follow;

	// Empty means no folder tabs, which is what a preset naming no folders
	// asks for - there is no "said nothing" case left to distinguish.
	std::vector<PresetFolder> folders;

	// Cached too, so a settings.toml broken mid-edit does not take the extra
	// tabs away along with everything else it cannot read.
	std::vector<ResolvedCacheView> views;

	[[nodiscard]] bool valid() const {
		return !preset.isEmpty();
	}
};

// A decision about one chat that outranks the preset, until a deadline.
//
// Scoped to the preset it was made under rather than global: "show this until
// six" is a statement about the work you are doing now, and carrying it into
// every other preset would make it a different, larger promise than the one
// the menu offered.
enum class OverrideKind : uchar {
	Show,   // In the view past the preset's rules. Does not un-silence.
	Hide,   // Out of the view, and silenced with it.
	Notify, // May interrupt, lifting only the preset's mute - never your own.
};

// "show", "hide", "notify".
[[nodiscard]] std::optional<OverrideKind> ParseOverrideKind(
	const QString &value);
[[nodiscard]] QString OverrideKindName(OverrideKind value);

struct Override {
	PeerIdValue peer = 0;
	OverrideKind kind = OverrideKind::Show;

	// Unix seconds, like peekDeadlineUnix and for the same reason: crl::now()
	// is monotonic and would mean nothing after a restart, and an override is
	// measured in hours.
	int64 untilUnix = 0;

	// When it was made. Only the ring in the chat list needs this - it is what
	// makes the arc proportional rather than guessed - but the file reads
	// better for having both ends of the span written down.
	int64 startedUnix = 0;

	QString preset;

	friend bool operator==(const Override &, const Override &) = default;
};

struct State {
	QString activePreset;
	PresetSource activeSource = PresetSource::Manual;

	// Remembered when OS focus takes over, so exiting focus can put back both
	// the preset and the reason it was active.
	QString previousPreset;
	PresetSource previousSource = PresetSource::Manual;

	// Whether the OS says a focus mode is on. Written by whatever is watching
	// for that, which is deliberately not the same thing as what acts on it:
	// the policy below is worth proving, and the detection is a moving target
	// that Apple owns. See docs/purple/work_mode.md.
	bool focusActive = false;

	// The last value of focusActive that was acted on, so focus sync moves on
	// a change rather than on a value - the same shape as scheduleTarget, and
	// for the same reason. A preset chosen by hand in the middle of a focus
	// session stands, because nothing fires again until focus itself changes.
	bool focusSeen = false;

	bool schedulePaused = false;

	// The last preset the schedule computed, so it can act on a change rather
	// than on every tick. That is what lets a preset chosen by hand survive
	// until the next boundary instead of being overwritten a second later, and
	// what lets a boundary missed while the app was closed still be caught up
	// on the next launch. Empty means it has never run.
	QString scheduleTarget;

	bool peekActive = false;
	int64 peekDeadlineUnix = 0;

	// Live "until" decisions, in the order they were made. Small by nature -
	// each one is a thing you did on purpose and it expires by itself.
	std::vector<Override> overrides;

	ResolvedCache resolvedCache;
};

// Whether the peek recorded in state is still running. A deadline of zero means
// auto_off is turned off, so the peek runs until it is turned off by hand -
// which is the only reason the flag is persisted rather than kept in memory.
// The comparison is what makes a peek that outlived the app expire on its own:
// the deadline is in the past by the time anything reads it again.
[[nodiscard]] bool PeekLive(const State &state, int64 nowUnix);

// The name that means "behave exactly like stock Telegram Desktop".
[[nodiscard]] const QString &NormalPreset();

[[nodiscard]] State ParseState(const QString &text, const QString &path);
[[nodiscard]] QString SerializeState(const State &state);

// The override in force for this chat right now, or null. Expired entries and
// entries made under a different preset are both invisible here, which is what
// makes every caller a single test rather than three.
[[nodiscard]] const Override *OverrideFor(
	const State &state,
	PeerIdValue peer,
	const QString &preset,
	int64 nowUnix);

// Drops whatever has run out. True if anything went, so the caller knows
// whether a rebuild is owed.
bool PruneOverrides(State &state, int64 nowUnix);

// The earliest deadline still outstanding under this preset, or 0. What the
// timer is armed for.
[[nodiscard]] int64 NextOverrideDeadline(
	const State &state,
	const QString &preset);

} // namespace Purple
