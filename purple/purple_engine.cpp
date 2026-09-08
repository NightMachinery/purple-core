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

std::optional<QString> ScheduleTarget(
		const Schedule &schedule,
		const QDateTime &now) {
	if (!schedule.enabled || schedule.rules.empty()) {
		return std::nullopt;
	}
	const auto rule = ScheduleRuleNow(schedule, now);
	return rule ? rule->preset : NormalPreset();
}

const ScheduleRule *ScheduleRuleNow(
		const Schedule &schedule,
		const QDateTime &now) {
	if (!schedule.enabled) {
		return nullptr;
	}
	const auto covers = [](const ScheduleRule &rule, int day) {
		return std::find(rule.days.begin(), rule.days.end(), day)
			!= rule.days.end();
	};
	const auto time = now.time();
	const auto minutes = time.hour() * 60 + time.minute();
	const auto today = now.date().dayOfWeek();
	const auto yesterday = (today == 1) ? 7 : (today - 1);

	// First match wins, in file order, the same rule the lists follow. Two
	// rules covering one moment is a thing a hand-written file will do, and
	// picking by position is the only answer that can be predicted by reading.
	for (const auto &rule : schedule.rules) {
		if (!rule.enabled) {
			continue;
		} else if (rule.from < rule.till) {
			// Half-open, so 09:00-12:00 and 12:00-17:00 hand over cleanly
			// rather than both claiming noon. The parser has already refused
			// a rule whose ends are equal, so there is no empty window here.
			if (covers(rule, today)
				&& minutes >= rule.from
				&& minutes < rule.till) {
				return &rule;
			}
		} else if ((covers(rule, today) && minutes >= rule.from)
			|| (covers(rule, yesterday) && minutes < rule.till)) {
			// A window crossing midnight belongs to the day it starts on, so
			// "mon, 22:00 to 06:00" runs into Tuesday morning instead of
			// stopping at midnight or needing Tuesday listed as well - which
			// would also have claimed Tuesday 00:00 to 06:00 twice over.
			return &rule;
		}
	}
	return nullptr;
}

} // namespace Purple
