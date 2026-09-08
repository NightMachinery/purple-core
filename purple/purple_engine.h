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

// What this install is, as far as a ruleset is concerned. Every field comes
// from the client, never from the file: the whole point of rulesets is that one
// settings.toml is carried between devices unchanged, so the file describes
// devices and the device describes itself.
struct DeviceIdentity {
	// Stable and unique to this install - "mac-3f9a" - and the string a ruleset
	// names to mean this one device. Empty when the client has none, which
	// matches only rulesets that ask for no device in particular.
	QString id;

	// "android", "ios", "macos", "windows" or "linux".
	QString platform;

	// "mobile" or "desktop".
	QString cls;
};

// Whether a ruleset is for this device at all: "any", its class, its platform,
// or its id outright. Case is ignored, because a device id ends up typed by
// hand into a file at least once.
[[nodiscard]] bool RulesetAppliesTo(
	const ScheduleRuleset &ruleset,
	const DeviceIdentity &device);

// How narrowly a ruleset aimed at the device it applies to: 3 for one naming a
// device id, 2 for a platform, 1 for a class, 0 for "any". It is a property of
// what the ruleset asked for and nothing else, so two devices always agree on
// which of two rulesets is the more specific.
[[nodiscard]] int RulesetSpecificity(const ScheduleRuleset &ruleset);

// The schedule this device actually runs, once the rulesets have been sorted
// out. Everything downstream sees a single flat list of rules and one outside
// preset, which is what lets the rule-picking below stay exactly as it was
// before rulesets existed - the merge order it falls back on for a tie is the
// only thing here that rulesets changed.
//
// The pointers are into the Schedule this was built from and are only good for
// as long as it is.
struct ScheduleForDevice {
	// The chosen rulesets' enabled rules, concatenated: the most specific
	// ruleset's rules first, then file order among equals.
	std::vector<const ScheduleRule*> rules;

	// The preset wanted between the windows: the most specific chosen ruleset
	// that names one, or [schedule] outside when none does.
	QString outside;

	// Which rulesets were chosen, in the order their rules were merged. For a
	// screen that has to explain why a rule is or is not running today.
	std::vector<const ScheduleRuleset*> chosen;
};

// Choosing among the rulesets, in one place because the rule has three parts
// and every one of them is a decision somebody will want to read back:
//
// - a ruleset applies when its device matches and its mode is not `disabled';
// - among the applicable `enabled' ones only the most specific tier runs, so a
//   ruleset naming this phone REPLACES the one for mobiles rather than piling
//   on top of it - which is what makes "the same file everywhere, refined per
//   device" work at all;
// - every applicable `always' ruleset runs as well, whatever won above, for the
//   rules that are true on every device and should not be copied into each.
//
// The flat [[schedule.rules]] array takes part as the implicit ruleset the
// parser materialises for it, so a file that never heard of rulesets resolves
// through exactly the same path.
[[nodiscard]] ScheduleForDevice ActiveSchedule(
	const Schedule &schedule,
	const DeviceIdentity &device);

// What the schedule wants active at this local time on this device: the preset
// of the NARROWEST rule covering the moment, or the active `outside' when rules
// exist and none does. See ScheduleRuleNow for why narrowest and not first.
//
// Nothing at all when the schedule is off, or when no ruleset that applies to
// this device has a rule in it - which is a different answer from wanting the
// outside preset and has to be: otherwise a file describing only OTHER devices
// would quietly drive this one, and an empty [schedule] section would force
// Normal over every other way of choosing a preset.
[[nodiscard]] std::optional<QString> ScheduleTarget(
	const Schedule &schedule,
	const QDateTime &now,
	const DeviceIdentity &device);

// The same for a caller with no device identity to offer. It matches only the
// rulesets that asked for no device in particular, which is every rule in a
// file written before rulesets existed.
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
// "Ending" is a move to the ACTIVE outside - the one this device's chosen
// rulesets settled on - not a move to Normal. Once the preset between windows
// is a key, five o'clock aiming at Home is a window ending like any other and
// must not steamroll a manual choice, which is the one thing a client mirroring
// `target != normal' would get wrong.
//
// Focus is left alone in both directions: it is the more immediate signal, and
// a schedule fighting it would make both unreadable.
//
// It lives here rather than in each app's tick because there are two ticks, and
// a rule this easy to get subtly wrong is worth having one copy of.
[[nodiscard]] bool ScheduleApplies(
	const ScheduleForDevice &active,
	const QString &target,
	PresetSource activeSource);

[[nodiscard]] bool ScheduleApplies(
	const Schedule &schedule,
	const DeviceIdentity &device,
	const QString &target,
	PresetSource activeSource);

[[nodiscard]] bool ScheduleApplies(
	const Schedule &schedule,
	const QString &target,
	PresetSource activeSource);

// The rule the schedule is inside right now, or null when none covers the
// moment - which includes a schedule that is switched off and one with no rule
// for this device. Points into `schedule'.
//
// Windows nest, so it is the narrowest one covering the moment rather than the
// first: "12:00-14:00 lunch" inside "08:00-17:00 work" means lunch at one
// o'clock whichever of the two was typed first.
//
// This is what ScheduleTarget() answers with, before it collapses the answer to
// a preset name. A screen that wants to say "work until 17:00" needs the rule
// itself, and working out which one it was a second time would mean a second
// copy of the midnight-crossing rule to keep in step.
[[nodiscard]] const ScheduleRule *ScheduleRuleNow(
	const Schedule &schedule,
	const QDateTime &now,
	const DeviceIdentity &device);

[[nodiscard]] const ScheduleRule *ScheduleRuleNow(
	const Schedule &schedule,
	const QDateTime &now);

// The narrowest rule in an already-resolved schedule that covers this moment.
//
// Narrowest by the length of its window in minutes, a midnight crossing
// measured the long way round through midnight. Ties - two rules with the same
// window - keep the order the rules were merged in: ruleset specificity first,
// then file position.
//
// Because the far end of a nested window is a move to the WIDER rule's preset
// rather than to `outside', ScheduleApplies reads it as a window starting, and
// the wider rule resumes even after a preset was chosen by hand inside the
// narrow one.
[[nodiscard]] const ScheduleRule *ScheduleRuleNow(
	const ScheduleForDevice &active,
	const QDateTime &now);

} // namespace Purple
