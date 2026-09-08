/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_settings.h"
#include "purple/purple_state.h"

class QDateTime;

// Turns a preset into a flat table of "for this list, show and notify are
// these", and answers what that means for one chat. Resolution happens once per
// config or preset change; nothing here may be called per repaint.
//
// Kept free of tdesktop dependencies for the same reason as the parser: every
// policy in the spec is a rule about data, and rules about data are far easier
// to prove outside a running app. See docs/purple/config.md.
namespace Purple {

// A folder a preset pulls into its main view, with everything the decision
// needs: how much of it comes (Pinned or All - never None, those are not here)
// and what mode its chats take once in.
struct ExemptFolder {
	QString name;
	FolderInclude include = FolderInclude::All;

	// Unset leaves the chats to DefaultShowMode() for whatever they are: the
	// folder chose which chats come in, not when they show.
	std::optional<ShowMode> showMode;

	friend bool operator==(const ExemptFolder &, const ExemptFolder &)
		= default;
};

// Whether a folder entry is switched on - `enabled_p', defaulting to true.
//
// A disabled entry stays in the resolution rather than being filtered out of
// it, because "*ALL" is expanded later, against the account's real folders, and
// skips whatever the selection already names. Dropping the entry here would
// therefore hand a disabled folder straight back to any preset that also asks
// for every folder. So it stays, claimed and inert, and every place that acts
// on a folder asks this first.
[[nodiscard]] bool FolderEnabled(const PresetFolder &folder);

// A folder that said something about its people's stories, paired with what it
// said. Same shape as ExemptFolder, and for the same reason.
struct StoryFolder {
	QString name;
	StoryMode mode = StoryMode::Always;

	friend bool operator==(const StoryFolder &, const StoryFolder &) = default;
};

// The folders that named a `stories' mode of their own.
[[nodiscard]] std::vector<StoryFolder> StoryFolderList(
	const std::vector<PresetFolder> &folders);

// The folders a preset selection pulls into its main view. Saying nothing
// leaves a folder's chats to whatever the lists decided, like every folder the
// preset does not name.
[[nodiscard]] std::vector<ExemptFolder> ExemptFolderList(
	const std::vector<PresetFolder> &folders);

// The folders a preset silences - the ones that said `notify_p = false'.
[[nodiscard]] std::vector<QString> SilencedFolderNames(
	const std::vector<PresetFolder> &folders);

// The folders that asked to be left out of every count - `badge_p = false'.
[[nodiscard]] std::vector<QString> QuietFolderNames(
	const std::vector<PresetFolder> &folders);

// One step of a resolved order. `notify' is collapsed here; `show' cannot be,
// because its default depends on what the chat turns out to be and one entry
// can claim several kinds at once. Visible() does that last step.
struct EffectiveList {
	QString list;
	std::optional<ShowMode> show;
	bool notify = true;

	// What this entry's people's stories do. Unset leaves them to the preset's
	// own policy, which is the usual case.
	std::optional<StoryMode> stories;

	friend bool operator==(
		const EffectiveList &,
		const EffectiveList &) = default;
};

// An extra tab the preset invents. Its order picks membership only - a chat an
// entry claims with `show' false is off this tab - because silence belongs to
// the chat rather than to the tab it is being looked at on.
struct ResolvedView {
	QString name;
	std::vector<PeerIdValue> pinned;
	std::vector<EffectiveList> lists;

	friend bool operator==(const ResolvedView &, const ResolvedView &) = default;
};

struct Resolved {
	QString preset;

	// What to call the preset's main tab, from its own `default_view_name' or
	// from its name. See DefaultViewName().
	QString viewName;

	// Normal is not "a preset with everything on" but a bypass: the engine is
	// skipped entirely, so it cannot drift as preset features are added.
	bool normal = false;

	// The main view's own pinned order, or empty for a preset that mirrors the
	// account's. See Preset::pinned.
	std::vector<PeerIdValue> pinned;

	// Priority order, first match wins. A chat no entry claims is hidden and
	// silenced - a preset names what gets through, and saying nothing about a
	// chat is saying no.
	std::vector<EffectiveList> lists;

	// The real folders the preset shows, in strip order, possibly including the
	// "*ALL" marker. Empty means no folder tabs at all.
	std::vector<PresetFolder> folders;

	// The folders that asked to be pulled into the main view, lifted out so the
	// common case - nobody asked - is an empty vector to test rather than a
	// walk of the folder list per hidden chat.
	std::vector<ExemptFolder> exemptFolders;

	// Names from `folders' that said `notify_p = false'. Same reason as above:
	// the common case is nobody asked, and that has to be one empty vector to
	// test rather than a folder walk per mute query.
	std::vector<QString> silencedFolders;

	// Names from `folders' that said `badge_p = false'. Same shape again.
	std::vector<QString> quietFolders;

	// What the stories strip does while this preset runs.
	StoryPolicy stories = StoryPolicy::Follow;

	// The folders that said something about stories, lifted out for the same
	// reason as the three lists above - the common case is nobody asked.
	std::vector<StoryFolder> storyFolders;

	// The extra tabs, in the order the file gave them, after the main view.
	std::vector<ResolvedView> views;

	// Whether hiding means "gone from the app" rather than "absent from this
	// preset's view of the chat list". Off by default: a hidden chat stays
	// reachable through the forward picker, search and recent chats, and only
	// the view leaves it out. See Preset::hideEverywhere.
	bool hideEverywhere = false;

	// Whether the archive is out of the way while this preset runs. True
	// unless the preset said otherwise; see Preset::hideArchive. Consumers ask
	// this only while a preset is filtering, which is why Normal carrying the
	// default here costs nothing.
	bool hideArchive = true;

	// A peek is running, so the preset's hiding is suspended - but not its
	// silencing. Set by the gate from state.toml rather than by Resolve(): a
	// peek is transient and expires on a clock, which is also why ToCache()
	// does not carry it. A cached resolution restored with a peek in it would
	// leave the chat list revealed with nothing left running to put it back.
	bool peeking = false;

	[[nodiscard]] const EffectiveList *list(const QString &name) const;

	// Used to decide whether a reload actually changed anything, so a state
	// write that only moved a peek deadline does not rebuild every chat list.
	friend bool operator==(const Resolved &, const Resolved &) = default;
};

// Nothing if the preset does not exist. The caller is expected to fall back to
// the last good resolution rather than to defaults - defaulting would quietly
// unhide every chat the user hid.
[[nodiscard]] std::optional<Resolved> Resolve(
	const Settings &settings,
	const QString &preset);

// Whether a list claims this chat: its id is a member, or its kind is one the
// list matches.
[[nodiscard]] bool ListHolds(
	const List &list,
	PeerIdValue id,
	ChatKind kind);

// The entry that decides a chat under this resolution: the first in the
// preset's order whose list claims it. Null means no entry claimed it, which is
// the fall-through - hidden and silenced.
[[nodiscard]] const EffectiveList *MatchList(
	const Settings &settings,
	const Resolved &resolved,
	PeerIdValue id,
	ChatKind kind);

// What the chat list and the notification gate actually ask.
//
// `show' is a mode rather than an answer, because everything except Always and
// Never depends on the chat's unread state - and the engine deliberately knows
// nothing about unread counts. The caller with the chat in hand finishes the
// job; see History::purpleHiddenFromView().
struct Visibility {
	ShowMode show = ShowMode::Always;
	bool notify = true;
};

[[nodiscard]] Visibility Visible(
	const Settings &settings,
	const Resolved &resolved,
	PeerIdValue id,
	ChatKind kind);

// Whether one of the preset's extra views shows this chat. Views select
// membership; they never change what a chat is allowed to do.
[[nodiscard]] bool ViewHolds(
	const Settings &settings,
	const ResolvedView &view,
	PeerIdValue id,
	ChatKind kind);

// Whether the running resolution names this id OUTRIGHT - written into the
// `members' of a list the preset or one of its views orders - as opposed to
// merely matching it through a `kinds' rule.
//
// The distinction carries weight the other predicates here do not: it is the
// difference between the user asking for one particular chat and the user
// describing a category that happens to contain it. Only the first is grounds
// for keeping a chat in the chat list that tdesktop would otherwise drop, and
// reading a `kinds' match as an explicit request would sweep in every contact
// with no conversation. See History::purpleKeptForView().
//
// Entries whose show is Never are skipped: an entry naming a chat in order to
// hide it has not asked for it to be anywhere.
[[nodiscard]] bool NamedExplicitly(
	const Settings &settings,
	const Resolved &resolved,
	PeerIdValue id);

[[nodiscard]] ResolvedCache ToCache(const Resolved &resolved);
[[nodiscard]] std::optional<Resolved> FromCache(const ResolvedCache &cache);

// What the schedule wants active at this local time: the preset of the first
// rule covering the moment, or `schedule.outside' when rules exist and none
// does.
//
// Nothing at all when the schedule is off or has no rules, which is a different
// answer from wanting the outside preset and has to be: otherwise an empty
// [schedule] section would quietly force Normal over every other way of
// choosing a preset.
[[nodiscard]] std::optional<QString> ScheduleTarget(
	const Schedule &schedule,
	const QDateTime &now);

// Whether a target the schedule has just moved to should take the running
// preset with it, given what put that preset in place.
//
// The asymmetry between the two answers is the whole boundary rule. A window
// STARTING is a positive instruction - "at nine, work mode" - and it overrides
// a preset chosen by hand. A window ENDING only means the reason for that
// preset has passed, which is no reason at all to undo something asked for, so
// it lands only when the schedule is what put the running preset there.
//
// "Ending" is a move to `schedule.outside', not a move to Normal. Once the
// preset outside every window is a key, five o'clock aiming at Home is a window
// ending like any other and must not steamroll a manual choice - which is the
// one thing a client mirroring `target != normal' would get wrong.
//
// Focus is left alone in both directions: it is the more immediate signal, and
// a schedule fighting it would make both unreadable.
//
// It lives here rather than in each app's tick because there are two ticks, and
// a rule this easy to get subtly wrong is worth having one copy of.
[[nodiscard]] bool ScheduleApplies(
	const Schedule &schedule,
	const QString &target,
	PresetSource activeSource);

// The rule the schedule is inside right now, or null when none covers the
// moment - which includes a schedule that is switched off or has no rules.
// Points into `schedule.rules'.
//
// This is what ScheduleTarget() answers with, before it collapses the answer to
// a preset name. A screen that wants to say "work until 17:00" needs the rule
// itself, and working out which one it was a second time would mean a second
// copy of the midnight-crossing rule to keep in step.
[[nodiscard]] const ScheduleRule *ScheduleRuleNow(
	const Schedule &schedule,
	const QDateTime &now);

} // namespace Purple
