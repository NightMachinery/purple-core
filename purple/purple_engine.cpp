/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_engine.h"

#include <QtCore/QDateTime>

#include <algorithm>

namespace Purple {
namespace {

[[nodiscard]] EffectiveList Effective(const ListEntry &entry) {
	auto result = EffectiveList();
	result.list = entry.list;
	result.show = entry.show;
	result.notify = entry.notify.value_or(true);
	result.stories = entry.stories;
	return result;
}

[[nodiscard]] std::vector<EffectiveList> Effective(
		const std::vector<ListEntry> &entries) {
	auto result = std::vector<EffectiveList>();
	result.reserve(entries.size());
	for (const auto &entry : entries) {
		result.push_back(Effective(entry));
	}
	return result;
}

} // namespace

bool FolderEnabled(const PresetFolder &folder) {
	return folder.enabled.value_or(true);
}

const EffectiveList *Resolved::list(const QString &name) const {
	const auto i = std::find_if(
		lists.begin(),
		lists.end(),
		[&](const EffectiveList &entry) { return entry.list == name; });
	return (i == lists.end()) ? nullptr : &*i;
}

std::optional<Resolved> Resolve(
		const Settings &settings,
		const QString &preset) {
	auto result = Resolved();
	if (!preset.compare(NormalPreset(), Qt::CaseInsensitive)) {
		result.preset = NormalPreset();
		result.viewName = DefaultViewName(result.preset);
		result.normal = true;
		return result;
	}
	const auto found = settings.preset(preset);
	if (!found) {
		return std::nullopt;
	}

	// No chain to walk any more. A preset says what it does, in one place, and
	// reuse is a "*set" spread the parser has already expanded - so everything
	// below is a copy rather than a search.
	result.preset = preset;
	result.viewName = found->viewName.isEmpty()
		? DefaultViewName(preset)
		: found->viewName;
	result.hideEverywhere = found->hideEverywhere.value_or(false);
	result.hideArchive = found->hideArchive.value_or(true);
	result.pinned = found->pinned;
	result.lists = Effective(found->listOrder);
	result.folders = found->folders;
	result.exemptFolders = ExemptFolderList(result.folders);
	result.silencedFolders = SilencedFolderNames(result.folders);
	result.quietFolders = QuietFolderNames(result.folders);
	result.stories = found->stories.value_or(StoryPolicy::Follow);
	result.storyFolders = StoryFolderList(result.folders);

	result.views.reserve(found->views.size());
	for (const auto &view : found->views) {
		auto resolved = ResolvedView();
		resolved.name = view.name;
		resolved.pinned = view.pinned;
		resolved.lists = Effective(view.listOrder);
		result.views.push_back(std::move(resolved));
	}
	return result;
}

std::vector<StoryFolder> StoryFolderList(
		const std::vector<PresetFolder> &folders) {
	auto result = std::vector<StoryFolder>();
	for (const auto &folder : folders) {
		if (FolderEnabled(folder) && folder.stories.has_value()) {
			result.push_back({ folder.name, *folder.stories });
		}
	}
	return result;
}

std::vector<ExemptFolder> ExemptFolderList(
		const std::vector<PresetFolder> &folders) {
	auto result = std::vector<ExemptFolder>();
	for (const auto &folder : folders) {
		if (!FolderEnabled(folder)) {
			continue;
		}
		// Only an explicit include pulls a folder's chats in. Saying nothing
		// leaves them to whatever their list decided, which is what every
		// folder the preset does not name is left to.
		const auto include = folder.include.value_or(FolderInclude::None);
		if (include != FolderInclude::None) {
			result.push_back({ folder.name, include, folder.showMode });
		}
	}
	return result;
}

std::vector<QString> SilencedFolderNames(
		const std::vector<PresetFolder> &folders) {
	auto result = std::vector<QString>();
	for (const auto &folder : folders) {
		if (FolderEnabled(folder)
			&& folder.notify.has_value()
			&& !*folder.notify) {
			result.push_back(folder.name);
		}
	}
	return result;
}

std::vector<QString> QuietFolderNames(
		const std::vector<PresetFolder> &folders) {
	auto result = std::vector<QString>();
	for (const auto &folder : folders) {
		if (FolderEnabled(folder)
			&& folder.badge.has_value()
			&& !*folder.badge) {
			result.push_back(folder.name);
		}
	}
	return result;
}

bool ListHolds(const List &list, PeerIdValue id, ChatKind kind) {
	if (std::find(list.members.begin(), list.members.end(), id)
		!= list.members.end()) {
		return true;
	}
	return std::find(list.kinds.begin(), list.kinds.end(), kind)
		!= list.kinds.end();
}

const EffectiveList *MatchList(
		const Settings &settings,
		const Resolved &resolved,
		PeerIdValue id,
		ChatKind kind) {
	if (resolved.normal) {
		return nullptr;
	}

	// Priority order, first match wins. Order is capture as well as priority:
	// once an entry claims a chat, nothing further down ever sees it, which is
	// what makes a separate override table unnecessary.
	for (const auto &effective : resolved.lists) {
		const auto list = settings.list(effective.list);
		if (list && ListHolds(*list, id, kind)) {
			return &effective;
		}
	}
	return nullptr;
}

Visibility Visible(
		const Settings &settings,
		const Resolved &resolved,
		PeerIdValue id,
		ChatKind kind) {
	auto result = Visibility();
	if (resolved.normal) {
		return result;
	}
	if (const auto effective = MatchList(settings, resolved, id, kind)) {
		// The last collapse: an entry that said nothing takes the default for
		// what this chat actually is, which is the one thing resolution could
		// not decide because one entry can claim several kinds.
		result.show = effective->show.value_or(DefaultShowMode(kind));
		result.notify = effective->notify;
	} else {
		// Nothing claimed it. A preset names what gets through, so saying
		// nothing about a chat is saying no - to both halves, because a chat
		// you are not looking at has no business interrupting you either.
		result.show = ShowMode::Never;
		result.notify = false;
	}

	// Peek reveals; it does not un-silence. Both halves of a preset could be
	// suspended together, but they answer different questions: hiding is about
	// what you can find, silencing is about what may interrupt you, and a peek
	// is a deliberate look at the chat list. Unmuting for it would deliver a
	// burst of notifications for chats you are already looking at, and then
	// take the mute back before you had dealt with them.
	//
	// A peek reveals what a mode was holding back too - a group nobody has
	// mentioned you in is exactly the kind of thing you peek to check.
	if (resolved.peeking) {
		result.show = ShowMode::Always;
	}
	return result;
}

bool ViewHolds(
		const Settings &settings,
		const ResolvedView &view,
		PeerIdValue id,
		ChatKind kind) {
	for (const auto &effective : view.lists) {
		const auto list = settings.list(effective.list);
		if (list && ListHolds(*list, id, kind)) {
			// Only "never" drops a chat from a tab. A view is a selection you
			// asked for by name, so the unread-watching modes are deliberately
			// not honoured here - a "Focus" tab that emptied itself whenever
			// its chats went quiet would be the opposite of the point - and an
			// entry that said nothing means "on this tab", not the per-kind
			// default that governs the main view.
			return (effective.show.value_or(ShowMode::Always)
				!= ShowMode::Never);
		}
	}
	// Same rule as the main view: a tab holds what it names, and nothing else.
	return false;
}

bool NamedExplicitly(
		const Settings &settings,
		const Resolved &resolved,
		PeerIdValue id) {
	if (resolved.normal || !id) {
		return false;
	}
	const auto named = [&](const std::vector<EffectiveList> &order) {
		for (const auto &effective : order) {
			if (effective.show == ShowMode::Never) {
				continue;
			}
			const auto list = settings.list(effective.list);
			if (list
				&& (std::find(list->members.begin(), list->members.end(), id)
					!= list->members.end())) {
				return true;
			}
		}
		return false;
	};
	// The main order and every view, without regard to which entry would win
	// the main order's first-match-wins race. Over-approximating costs nothing
	// here - the only consequence is keeping a chat in the chat list that the
	// user wrote down by hand - and reasoning about capture would make the
	// answer depend on the order of two things that are not in competition.
	if (named(resolved.lists)) {
		return true;
	}
	for (const auto &view : resolved.views) {
		if (named(view.lists)) {
			return true;
		}
	}
	return false;
}

ResolvedCache ToCache(const Resolved &resolved) {
	auto result = ResolvedCache();
	if (resolved.normal) {
		return result;
	}
	const auto cached = [](const std::vector<EffectiveList> &lists) {
		auto result = std::vector<ResolvedList>();
		result.reserve(lists.size());
		for (const auto &entry : lists) {
			result.push_back({
				entry.list,
				entry.show,
				entry.notify,
				entry.stories,
			});
		}
		return result;
	};
	result.preset = resolved.preset;
	result.viewName = resolved.viewName;
	result.hideEverywhere = resolved.hideEverywhere;
	result.hideArchive = resolved.hideArchive;
	result.pinned = resolved.pinned;
	result.folders = resolved.folders;
	result.stories = resolved.stories;
	result.lists = cached(resolved.lists);
	result.views.reserve(resolved.views.size());
	for (const auto &view : resolved.views) {
		result.views.push_back({ view.name, view.pinned, cached(view.lists) });
	}
	return result;
}

std::optional<Resolved> FromCache(const ResolvedCache &cache) {
	if (!cache.valid()) {
		return std::nullopt;
	}
	const auto restored = [](const std::vector<ResolvedList> &lists) {
		auto result = std::vector<EffectiveList>();
		result.reserve(lists.size());
		for (const auto &entry : lists) {
			result.push_back({
				entry.list,
				entry.show,
				entry.notify,
				entry.stories,
			});
		}
		return result;
	};
	auto result = Resolved();
	result.preset = cache.preset;
	result.viewName = cache.viewName.isEmpty()
		? DefaultViewName(cache.preset)
		: cache.viewName;
	result.hideEverywhere = cache.hideEverywhere;
	result.hideArchive = cache.hideArchive;
	result.pinned = cache.pinned;
	result.folders = cache.folders;
	result.exemptFolders = ExemptFolderList(result.folders);
	result.silencedFolders = SilencedFolderNames(result.folders);
	result.quietFolders = QuietFolderNames(result.folders);
	result.stories = cache.stories;
	result.storyFolders = StoryFolderList(result.folders);
	result.lists = restored(cache.lists);
	result.views.reserve(cache.views.size());
	for (const auto &view : cache.views) {
		result.views.push_back({
			view.name,
			view.pinned,
			restored(view.lists),
		});
	}
	return result;
}

bool RulesetAppliesTo(
		const ScheduleRuleset &ruleset,
		const DeviceIdentity &device) {
	const auto wanted = ruleset.device.trimmed();
	if (wanted.isEmpty() || !wanted.compare(u"any"_q, Qt::CaseInsensitive)) {
		return true;
	}
	const auto names = [&](const QString &what) {
		return !what.isEmpty() && !wanted.compare(what, Qt::CaseInsensitive);
	};
	return names(device.cls) || names(device.platform) || names(device.id);
}

int RulesetSpecificity(const ScheduleRuleset &ruleset) {
	const auto wanted = ruleset.device.trimmed();
	if (wanted.isEmpty() || !wanted.compare(u"any"_q, Qt::CaseInsensitive)) {
		return 0;
	}
	for (const auto &cls : { u"mobile"_q, u"desktop"_q }) {
		if (!wanted.compare(cls, Qt::CaseInsensitive)) {
			return 1;
		}
	}
	for (const auto &platform : {
			u"android"_q,
			u"ios"_q,
			u"macos"_q,
			u"windows"_q,
			u"linux"_q }) {
		if (!wanted.compare(platform, Qt::CaseInsensitive)) {
			return 2;
		}
	}

	// The list of platforms is closed and the list of devices is not, so
	// anything left is a device id. That is also what makes a typo the most
	// specific thing in the file rather than a warning: "andriod" is a device
	// nobody owns, and a ruleset for a device nobody owns simply never applies.
	return 3;
}

ScheduleForDevice ActiveSchedule(
		const Schedule &schedule,
		const DeviceIdentity &device) {
	auto result = ScheduleForDevice();
	result.outside = schedule.outside;

	// A Schedule the parser did not build - one assembled in a test or by a
	// caller filling in the flat list - has no ruleset to stand for its rules,
	// so they are taken as they are. Reading a file always goes the other way:
	// ReadSchedule() materialises the implicit ruleset, so the flat rules take
	// their place in the merge order rather than being bolted on ahead of it.
	if (schedule.rulesets.empty()) {
		for (const auto &rule : schedule.rules) {
			if (rule.enabled) {
				result.rules.push_back(&rule);
			}
		}
		return result;
	}

	auto applicable = std::vector<const ScheduleRuleset*>();
	for (const auto &ruleset : schedule.rulesets) {
		if (ruleset.mode != RulesetMode::Disabled
			&& RulesetAppliesTo(ruleset, device)) {
			applicable.push_back(&ruleset);
		}
	}

	// Only the narrowest tier of the ordinary rulesets runs. Everything less
	// specific has been replaced rather than added to, which is the difference
	// between "the same file everywhere, refined per device" and a file whose
	// every layer keeps firing at once.
	auto tier = -1;
	for (const auto ruleset : applicable) {
		if (ruleset->mode == RulesetMode::Enabled) {
			tier = std::max(tier, RulesetSpecificity(*ruleset));
		}
	}
	for (const auto ruleset : applicable) {
		if (ruleset->mode == RulesetMode::Always
			|| RulesetSpecificity(*ruleset) == tier) {
			result.chosen.push_back(ruleset);
		}
	}

	// Most specific first, then file order. stable_sort rather than a
	// comparison that reaches for sourceIndex, because file order is the order
	// they are already in and the implicit ruleset has no index to compare.
	std::stable_sort(
		result.chosen.begin(),
		result.chosen.end(),
		[](const ScheduleRuleset *a, const ScheduleRuleset *b) {
			return RulesetSpecificity(*a) > RulesetSpecificity(*b);
		});

	for (const auto ruleset : result.chosen) {
		if (ruleset->outside) {
			// The first one to name a preset, in the order above: the most
			// specific chosen ruleset that has an opinion, ties by file order.
			result.outside = *ruleset->outside;
			break;
		}
	}
	for (const auto ruleset : result.chosen) {
		for (const auto &rule : ruleset->rules) {
			if (rule.enabled) {
				result.rules.push_back(&rule);
			}
		}
	}
	return result;
}

std::optional<QString> ScheduleTarget(
		const Schedule &schedule,
		const QDateTime &now,
		const DeviceIdentity &device) {
	if (!schedule.enabled) {
		return std::nullopt;
	}
	const auto active = ActiveSchedule(schedule, device);
	if (active.rules.empty()) {
		return std::nullopt;
	}
	const auto rule = ScheduleRuleNow(active, now);
	return rule ? rule->preset : active.outside;
}

std::optional<QString> ScheduleTarget(
		const Schedule &schedule,
		const QDateTime &now) {
	return ScheduleTarget(schedule, now, DeviceIdentity());
}

bool ScheduleApplies(
		const ScheduleForDevice &active,
		const QString &target,
		PresetSource activeSource) {
	if (activeSource == PresetSource::Focus) {
		return false;
	}
	return (target != active.outside)
		|| (activeSource == PresetSource::Schedule);
}

bool ScheduleApplies(
		const Schedule &schedule,
		const DeviceIdentity &device,
		const QString &target,
		PresetSource activeSource) {
	return ScheduleApplies(
		ActiveSchedule(schedule, device),
		target,
		activeSource);
}

bool ScheduleApplies(
		const Schedule &schedule,
		const QString &target,
		PresetSource activeSource) {
	return ScheduleApplies(schedule, DeviceIdentity(), target, activeSource);
}

// How long a rule's window is, in minutes. Internal: nothing outside this
// file has a reason to rank two windows against each other.
[[nodiscard]] static int ScheduleRuleSpan(const ScheduleRule &rule) {
	// A window crossing midnight is measured through it rather than as a
	// negative number: "22:00 to 06:00" is eight hours, not minus sixteen.
	// Getting this wrong would make every night rule the narrowest thing in
	// the file and let it win against anything nested inside it.
	return (rule.from < rule.till)
		? (rule.till - rule.from)
		: (24 * 60 - rule.from + rule.till);
}

const ScheduleRule *ScheduleRuleNow(
		const ScheduleForDevice &active,
		const QDateTime &now) {
	const auto covers = [](const ScheduleRule &rule, int day) {
		return std::find(rule.days.begin(), rule.days.end(), day)
			!= rule.days.end();
	};
	const auto time = now.time();
	const auto minutes = time.hour() * 60 + time.minute();
	const auto today = now.date().dayOfWeek();
	const auto yesterday = (today == 1) ? 7 : (today - 1);

	// The NARROWEST window covering the moment wins, not the first one.
	//
	// Windows nest: "08:00-17:00 work" with "12:00-14:00 lunch" inside it is
	// how anybody would write a lunch break, and under first-match-wins the
	// answer depended on which of the two happened to be typed first - which
	// is exactly the kind of thing nobody remembers about their own file. The
	// narrower rule is the more specific statement, the same way a ruleset
	// naming this phone beats one naming mobiles.
	//
	// Ties keep the order the rules were merged in: ruleset specificity first,
	// then file position. Two rules with the same window really are the same
	// statement twice, and position is the only answer left that can be
	// predicted by reading.
	//
	// It also makes the far end of the nested window a window STARTING rather
	// than one ending: at 14:00 the target moves from lunch back to work, and
	// ScheduleApplies sees a move to something that is not the outside preset,
	// so work resumes even if a preset was chosen by hand during lunch.
	const ScheduleRule *result = nullptr;
	auto narrowest = 0;
	for (const auto pointer : active.rules) {
		const auto &rule = *pointer;
		auto covering = false;
		if (!rule.enabled) {
			continue;
		} else if (rule.from < rule.till) {
			// Half-open, so 09:00-12:00 and 12:00-17:00 hand over cleanly
			// rather than both claiming noon. The parser has already refused
			// a rule whose ends are equal, so there is no empty window here.
			covering = covers(rule, today)
				&& minutes >= rule.from
				&& minutes < rule.till;
		} else {
			// A window crossing midnight belongs to the day it starts on, so
			// "mon, 22:00 to 06:00" runs into Tuesday morning instead of
			// stopping at midnight or needing Tuesday listed as well - which
			// would also have claimed Tuesday 00:00 to 06:00 twice over.
			covering = (covers(rule, today) && minutes >= rule.from)
				|| (covers(rule, yesterday) && minutes < rule.till);
		}
		if (!covering) {
			continue;
		}
		const auto span = ScheduleRuleSpan(rule);
		if (!result || span < narrowest) {
			result = &rule;
			narrowest = span;
		}
	}
	return result;
}

const ScheduleRule *ScheduleRuleNow(
		const Schedule &schedule,
		const QDateTime &now,
		const DeviceIdentity &device) {
	if (!schedule.enabled) {
		return nullptr;
	}
	return ScheduleRuleNow(ActiveSchedule(schedule, device), now);
}

const ScheduleRule *ScheduleRuleNow(
		const Schedule &schedule,
		const QDateTime &now) {
	return ScheduleRuleNow(schedule, now, DeviceIdentity());
}

std::optional<ScheduleTick> ScheduleStep(
		const Settings &settings,
		const State &state,
		const QDateTime &now,
		const DeviceIdentity &device) {
	auto result = ScheduleTick();
	result.state = state;
	auto &fresh = result.state;

	// A pause with a deadline lifts itself, both fields at once, and the
	// ordinary boundary rule then runs below in this same step. That is what
	// catches up, once and immediately, on the windows that opened and closed
	// while it was held off - waiting for the next window edge instead would
	// leave the preset wherever the pause found it, which for a pause lifting
	// on a Sunday evening could be a whole day.
	if (fresh.schedulePaused) {
		if (!ScheduleUnpauseDue(fresh, now.toSecsSinceEpoch())) {
			return std::nullopt;
		}
		fresh.schedulePaused = false;
		fresh.schedulePausedUntil = 0;
		result.unpaused = true;
	}
	const auto target = ScheduleTarget(settings.schedule, now, device);
	const auto boundary = target && (*target != fresh.scheduleTarget);
	if (!boundary && !result.unpaused) {
		// Acting on the change rather than on the value is the whole design. It
		// is what lets a preset chosen by hand stand until the next boundary
		// instead of being overwritten on the next tick, and what makes a
		// boundary missed while the app was closed still happen, once, at the
		// next launch.
		return std::nullopt;
	}
	result.target = boundary ? *target : fresh.scheduleTarget;
	result.kept = fresh.activePreset;
	result.keptSource = fresh.activeSource;

	// Two rules, and the asymmetry between them is deliberate. A window
	// starting is a positive instruction - "at nine, work mode" - and it
	// overrides a preset chosen by hand. A window ending only means the reason
	// for that preset has passed, which is no reason at all to undo something
	// asked for. Focus is left alone in both directions: it is the more
	// immediate signal, and a schedule fighting it would make both unreadable.
	//
	// All three of those live in ScheduleApplies rather than here, including
	// the focus one: one of the two ticks this replaces also tested
	// `activeSource != Focus' itself before asking, which read as a fourth rule
	// and was in fact the same one written twice - ScheduleApplies answers
	// false for Focus outright. Dropping the clause changes no answer and
	// leaves one place to read the rule.
	result.applied = boundary
		&& ScheduleApplies(
			settings.schedule,
			device,
			result.target,
			result.keptSource);
	if (boundary) {
		fresh.scheduleTarget = result.target;
	}
	if (result.applied) {
		fresh.activePreset = result.target;
		fresh.activeSource = PresetSource::Schedule;
	}
	return result;
}

QString FocusChangeName(FocusChange value) {
	switch (value) {
	case FocusChange::None: return u"none"_q;
	case FocusChange::Entered: return u"entered"_q;
	case FocusChange::Kept: return u"kept"_q;
	case FocusChange::Restored: return u"restored"_q;
	case FocusChange::Exited: return u"exited"_q;
	case FocusChange::Schedule: return u"schedule"_q;
	}
	return u"none"_q;
}

// Entering a focus session: remember what was running and why, so leaving can
// put both back, and let the preset [focus_sync] names take over.
//
// Internal, like ScheduleRuleSpan above: the two halves are only ever reached
// through FocusStep(), which is what decides that there IS an edge - and half
// the policy applied on a tick with no edge would be a preset moving for no
// reason anybody could name.
static void FocusEnter(State &state, const Settings &settings) {
	const auto from = state.activePreset;

	// Focus cannot be what we remember returning to, or a hand-edited state
	// file could leave the two pointing at each other.
	const auto fromSource = (state.activeSource == PresetSource::Focus)
		? PresetSource::Manual
		: state.activeSource;
	state.focusSeen = true;
	state.previousPreset = from;
	state.previousSource = fromSource;
	state.activePreset = settings.focusSync.enterPreset;
	state.activeSource = PresetSource::Focus;
}

// Leaving one. Answers which of the four things it did, for the caller's line.
//
// `enterTarget' is what the schedule wanted at the moment focus took over, or
// null when nothing remembers - see the missed-window case below.
[[nodiscard]] static FocusChange FocusLeave(
		State &state,
		const Settings &settings,
		const DeviceIdentity &device,
		const std::optional<QString> &enterTarget,
		const QDateTime &now) {
	if (state.activeSource != PresetSource::Focus) {
		// The preset in force is not the one focus imposed: it was chosen while
		// focus was on, and that choice outlives the focus session.
		state.focusSeen = false;
		return FocusChange::Kept;
	}
	const auto &sync = settings.focusSync;
	const auto restore = IsPreviousPresetName(sync.exitPreset);
	const auto previous = state.previousPreset;
	const auto previousSource = state.previousSource;
	state.focusSeen = false;
	state.previousPreset = QString();
	state.previousSource = PresetSource::Manual;
	if (restore) {
		// A schedule window that opened - or closed - while focus held the
		// preset was recorded by the tick and never applied, because focus is
		// the more immediate signal. Putting the pre-focus preset back now would
		// leave the tick nothing to do, since the target it compares against has
		// already moved, and the window would be missed until the next boundary.
		// So the boundary rule runs here instead, on the same asymmetry the tick
		// uses: a window that has opened overrides what was there, and one that
		// has closed only undoes a preset the schedule itself set - which is why
		// it is the pre-focus source, not the focus one, that decides.
		//
		// Only when something remembers what the schedule wanted when focus took
		// over. That is not a key state.toml has, so a caller keeps it beside
		// the file and it is simply absent after a restart, in which case this
		// restores exactly as it would have without the rule.
		const auto target = state.schedulePaused
			? std::optional<QString>()
			: ScheduleTarget(settings.schedule, now, device);
		const auto moved = enterTarget
			&& target
			&& (*target != *enterTarget);

		// The same boundary rule the tick runs, asked of ScheduleApplies rather
		// than spelled out again here: with rulesets and an `outside' key, "a
		// window ending" is no longer "the target is Normal", and two copies of
		// that sentence would have drifted the moment one of them was fixed.
		if (moved
			&& ScheduleApplies(
				settings.schedule,
				device,
				*target,
				previousSource)) {
			state.activePreset = *target;
			state.activeSource = PresetSource::Schedule;
			state.scheduleTarget = *target;
			return FocusChange::Schedule;
		}
	}
	const auto wanted = restore ? previous : sync.exitPreset;

	// Restoring puts back the reason as well as the preset, so a window the
	// schedule had opened still closes at its own boundary afterwards. A preset
	// named outright was not put there by either, so it is the user's until
	// something moves it.
	state.activePreset = wanted.isEmpty() ? NormalPreset() : wanted;
	state.activeSource = restore ? previousSource : PresetSource::Manual;
	return restore ? FocusChange::Restored : FocusChange::Exited;
}

std::optional<FocusTick> FocusStep(
		const Settings &settings,
		const State &state,
		bool focusActive,
		const std::optional<QString> &enterTarget,
		const QDateTime &now,
		const DeviceIdentity &device) {
	auto result = FocusTick();
	result.state = state;
	auto &fresh = result.state;
	fresh.focusActive = focusActive;

	const auto &sync = settings.focusSync;
	if (!sync.enabled) {
		// Switching focus sync off while it is holding a preset has to hand
		// that preset back. Leaving it in force would be a preset nothing on
		// screen explains and nothing left running would ever lift.
		if (fresh.activeSource == PresetSource::Focus) {
			result.change = FocusLeave(
				fresh,
				settings,
				device,
				enterTarget,
				now);
		} else if (fresh.focusSeen) {
			fresh.focusSeen = false;
		}
	} else if (fresh.focusActive == fresh.focusSeen) {
		// No edge, so nothing happens - which is exactly what makes a preset
		// chosen by hand mid-session stand until focus itself changes. The flag
		// above may still have moved on its own, and on a client where this
		// call is what writes it, that write is the point of the call.
	} else if (fresh.focusActive) {
		FocusEnter(fresh, settings);
		result.change = FocusChange::Entered;

		// Handed back for the caller to keep until the session ends. Empty
		// means the schedule wanted nothing, which is a different answer from
		// wanting Normal and stays distinguishable from it.
		result.enterTarget = ScheduleTarget(settings.schedule, now, device)
			.value_or(QString());
	} else {
		result.change = FocusLeave(fresh, settings, device, enterTarget, now);
	}

	// Nothing to write, which is every pass but the ones at an edge. Comparing
	// the serialisations rather than the fields is deliberate: "would this
	// change the file" is the question the caller is actually asking, and a
	// hand-written list of the fields the policy touches would have to be
	// revisited every time the policy grew one - silently, since a forgotten
	// field there fails as a write that never happens.
	return (SerializeState(fresh) == SerializeState(state))
		? std::nullopt
		: std::optional<FocusTick>(std::move(result));
}

} // namespace Purple
