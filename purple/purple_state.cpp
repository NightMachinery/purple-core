/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_state.h"

#include <QtCore/QStringList>

#define TOML_EXCEPTIONS 0
#include <toml.hpp>

namespace Purple {
namespace {

[[nodiscard]] QString Text(std::string_view value) {
	return QString::fromUtf8(value.data(), int(value.size()));
}

// Preset and folder names come from settings.toml, which the user writes, so
// they can hold quotes and backslashes even though they rarely will.
[[nodiscard]] QString Quoted(const QString &value) {
	auto escaped = QString();
	escaped.reserve(value.size() + 2);
	for (const auto ch : value) {
		if (ch == '"' || ch == '\\') {
			escaped += '\\';
			escaped += ch;
		} else if (ch == '\n') {
			escaped += u"\\n"_q;
		} else if (ch == '\r') {
			escaped += u"\\r"_q;
		} else if (ch == '\t') {
			escaped += u"\\t"_q;
		} else {
			escaped += ch;
		}
	}
	return '"' + escaped + '"';
}

[[nodiscard]] QString Boolean(bool value) {
	return value ? u"true"_q : u"false"_q;
}

[[nodiscard]] QString ReadString(
		const toml::table &table,
		std::string_view key,
		const QString &fallback = QString()) {
	const auto node = table.get(key);
	if (!node) {
		return fallback;
	}
	const auto value = node->value<std::string_view>();
	return value ? Text(*value) : fallback;
}

[[nodiscard]] bool ReadBool(
		const toml::table &table,
		std::string_view key,
		bool fallback) {
	const auto node = table.get(key);
	return node ? node->value_or(fallback) : fallback;
}

[[nodiscard]] QString SerializeOverrides(
		const std::vector<Override> &overrides) {
	if (overrides.empty()) {
		return QString();
	}
	auto result = u"\noverrides = [\n"_q;
	for (const auto &entry : overrides) {
		result += u"  { peer = %1, kind = %2, "
			"started = %3, until = %4, preset = %5 },\n"_q
			.arg(QString::number(entry.peer))
			.arg(Quoted(OverrideKindName(entry.kind)))
			.arg(QString::number(entry.startedUnix))
			.arg(QString::number(entry.untilUnix))
			.arg(Quoted(entry.preset));
	}
	return result + u"]\n"_q;
}

[[nodiscard]] QString SerializeLists(const std::vector<ResolvedList> &lists) {
	auto result = u"lists = [\n"_q;
	for (const auto &list : lists) {
		// show is written only when the entry said something. Writing a
		// collapsed value would freeze the chat-kind default into the cache,
		// so a restored resolution would stop following it.
		auto fields = u"list = %1"_q.arg(Quoted(list.list));
		if (list.show) {
			fields += u", show = %1"_q.arg(Quoted(ShowModeName(*list.show)));
		}
		fields += u", notify = %1"_q.arg(Boolean(list.notify));
		if (list.stories) {
			fields += u", stories = %1"_q.arg(
				Quoted(StoryModeName(*list.stories)));
		}
		result += u"  { %1 },\n"_q.arg(fields);
	}
	return result + u"]\n"_q;
}

void ReadOptionalBool(
		const toml::table &table,
		std::string_view key,
		std::optional<bool> &into) {
	if (const auto node = table.get(key)) {
		if (const auto value = node->value<bool>()) {
			into = *value;
		}
	}
}

[[nodiscard]] std::vector<ResolvedList> ReadResolvedLists(
		const toml::table &table,
		std::string_view key) {
	auto result = std::vector<ResolvedList>();
	const auto node = table.get(key);
	const auto array = node ? node->as_array() : nullptr;
	if (!array) {
		return result;
	}
	for (auto &&element : *array) {
		const auto fields = element.as_table();
		if (!fields) {
			continue;
		}
		auto entry = ResolvedList();
		entry.list = ReadString(*fields, "list");
		if (entry.list.isEmpty()) {
			continue;
		}
		if (const auto show = fields->get("show")) {
			if (const auto text = show->value<std::string_view>()) {
				entry.show = ParseShowMode(Text(*text));
			}
		}
		entry.notify = ReadBool(*fields, "notify", true);
		if (const auto stories = fields->get("stories")) {
			if (const auto text = stories->value<std::string_view>()) {
				entry.stories = ParseStoryMode(Text(*text));
			}
		}
		result.push_back(std::move(entry));
	}
	return result;
}

[[nodiscard]] std::vector<Override> ReadOverrides(const toml::table &root) {
	auto result = std::vector<Override>();
	const auto node = root.get("overrides");
	const auto array = node ? node->as_array() : nullptr;
	if (!array) {
		return result;
	}
	for (auto &&element : *array) {
		const auto fields = element.as_table();
		if (!fields) {
			continue;
		}
		auto entry = Override();
		const auto peer = fields->get("peer");
		const auto until = fields->get("until");
		const auto kind = fields->get("kind");
		entry.peer = peer ? peer->value<int64>().value_or(0) : 0;
		entry.untilUnix = until ? until->value<int64>().value_or(0) : 0;
		const auto started = fields->get("started");
		entry.startedUnix = started ? started->value<int64>().value_or(0) : 0;
		const auto name = kind
			? kind->value<std::string_view>()
			: std::optional<std::string_view>();
		const auto parsed = name
			? ParseOverrideKind(Text(*name))
			: std::nullopt;
		if (!entry.peer || !entry.untilUnix || !parsed) {
			// Skipped rather than fought over, like every other reader here:
			// the file is ours and a line we cannot read is one lost decision.
			continue;
		}
		entry.kind = *parsed;
		entry.preset = ReadString(*fields, "preset");
		result.push_back(std::move(entry));
	}
	return result;
}

[[nodiscard]] ResolvedCache ReadResolvedCache(const toml::table &root) {
	auto result = ResolvedCache();
	const auto node = root.get("resolved_cache");
	const auto table = node ? node->as_table() : nullptr;
	if (!table) {
		return result;
	}
	result.preset = ReadString(*table, "preset");
	if (result.preset.isEmpty()) {
		return result;
	}
	result.viewName = ReadString(*table, "view_name");
	if (result.viewName.isEmpty()) {
		result.viewName = DefaultViewName(result.preset);
	}
	result.hideEverywhere = ReadBool(*table, "hide_everywhere", false);
	result.hideArchive = ReadBool(*table, "hide_archive", true);
	if (const auto stories = table->get("stories")) {
		if (const auto text = stories->value<std::string_view>()) {
			// Falls back to Follow, which is also the default, so a file
			// written by an older build restores unchanged.
			result.stories = ParseStoryPolicy(Text(*text))
				.value_or(StoryPolicy::Follow);
		}
	}
	if (const auto pinned = table->get("pinned")) {
		if (const auto array = pinned->as_array()) {
			for (auto &&element : *array) {
				if (const auto id = element.value<int64>()) {
					result.pinned.push_back(*id);
				}
			}
		}
	}
	result.lists = ReadResolvedLists(*table, "lists");
	if (const auto folders = table->get("folders")) {
		if (const auto array = folders->as_array()) {
			for (auto &&element : *array) {
				const auto fields = element.as_table();
				if (!fields) {
					continue;
				}
				auto folder = PresetFolder();
				folder.name = ReadString(*fields, "name");
				if (folder.name.isEmpty()) {
					continue;
				}
				ReadOptionalBool(*fields, "enabled", folder.enabled);
				ReadOptionalBool(*fields, "show", folder.show);
				ReadOptionalBool(*fields, "notify", folder.notify);
				ReadOptionalBool(*fields, "badge", folder.badge);
				if (const auto stories = fields->get("stories")) {
					if (const auto text = stories->value<std::string_view>()) {
						folder.stories = ParseStoryMode(Text(*text));
					}
				}
				if (const auto mode = fields->get("show_mode")) {
					if (const auto text = mode->value<std::string_view>()) {
						folder.showMode = ParseShowMode(Text(*text));
					}
				}
				if (const auto include = fields->get("include_in_main_view")) {
					if (const auto text
						= include->value<std::string_view>()) {
						folder.include = ParseFolderInclude(Text(*text));
					}
				}
				result.folders.push_back(std::move(folder));
			}
		}
	}
	if (const auto views = table->get("views")) {
		if (const auto array = views->as_array()) {
			for (auto &&element : *array) {
				const auto fields = element.as_table();
				if (!fields) {
					continue;
				}
				auto view = ResolvedCacheView();
				view.name = ReadString(*fields, "name");
				view.lists = ReadResolvedLists(*fields, "lists");
				if (view.name.isEmpty() || view.lists.empty()) {
					continue;
				}
				if (const auto pinned = fields->get("pinned")) {
					if (const auto ids = pinned->as_array()) {
						for (auto &&id : *ids) {
							if (const auto value = id.value<int64>()) {
								view.pinned.push_back(*value);
							}
						}
					}
				}
				result.views.push_back(std::move(view));
			}
		}
	}

	// A cache naming lists it cannot describe is worse than none: the engine
	// would resolve half the chats and silently default the rest.
	if (result.lists.empty()) {
		result = ResolvedCache();
	}
	return result;
}

} // namespace

const QString &NormalPreset() {
	static const auto result = u"normal"_q;
	return result;
}

QString PresetSourceName(PresetSource source) {
	switch (source) {
	case PresetSource::Schedule: return u"schedule"_q;
	case PresetSource::Focus: return u"focus"_q;
	case PresetSource::Manual: return u"manual"_q;
	}
	return u"manual"_q;
}

PresetSource PresetSourceFromName(const QString &name) {
	return (name == u"schedule"_q)
		? PresetSource::Schedule
		: (name == u"focus"_q)
		? PresetSource::Focus
		: PresetSource::Manual;
}

// Nothing here reports errors. The file is ours, it is rewritten whenever
// anything changes, and a state we cannot read is not worth interrupting the
// user over - starting from stock behaviour is a safe place to begin.
State ParseState(const QString &text, const QString &path) {
	auto result = State();
	result.activePreset = NormalPreset();
	if (text.trimmed().isEmpty()) {
		return result;
	}
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		return result;
	}
	const auto &root = parsed.table();
	result.activePreset = ReadString(root, "active_preset", NormalPreset());
	result.activeSource = PresetSourceFromName(
		ReadString(root, "active_preset_source"));
	result.previousPreset = ReadString(root, "previous_preset");
	result.previousSource = PresetSourceFromName(
		ReadString(root, "previous_source"));
	result.focusActive = ReadBool(root, "focus_active", false);
	result.focusSeen = ReadBool(root, "focus_seen", false);
	result.schedulePaused = ReadBool(root, "schedule_paused", false);
	result.scheduleTarget = ReadString(root, "schedule_target");
	result.peekActive = ReadBool(root, "peek_active", false);
	result.overrides = ReadOverrides(root);
	if (const auto deadline = root.get("peek_deadline_unix")) {
		result.peekDeadlineUnix = deadline->value_or(int64(0));
	}
	result.resolvedCache = ReadResolvedCache(root);
	if (result.activePreset.isEmpty()) {
		result.activePreset = NormalPreset();
	}
	return result;
}

bool PeekLive(const State &state, int64 nowUnix) {
	return state.peekActive
		&& (!state.peekDeadlineUnix || state.peekDeadlineUnix > nowUnix);
}

std::optional<OverrideKind> ParseOverrideKind(const QString &value) {
	const auto trimmed = value.trimmed().toLower();
	if (trimmed == u"show"_q) {
		return OverrideKind::Show;
	} else if (trimmed == u"hide"_q) {
		return OverrideKind::Hide;
	} else if (trimmed == u"notify"_q) {
		return OverrideKind::Notify;
	}
	return std::nullopt;
}

QString OverrideKindName(OverrideKind value) {
	switch (value) {
	case OverrideKind::Show: return u"show"_q;
	case OverrideKind::Hide: return u"hide"_q;
	case OverrideKind::Notify: return u"notify"_q;
	}
	return QString();
}

const Override *OverrideFor(
		const State &state,
		PeerIdValue peer,
		const QString &preset,
		int64 nowUnix) {
	if (!peer || preset.isEmpty()) {
		return nullptr;
	}
	for (const auto &entry : state.overrides) {
		if (entry.peer == peer
			&& entry.untilUnix > nowUnix
			&& !entry.preset.compare(preset, Qt::CaseInsensitive)) {
			return &entry;
		}
	}
	return nullptr;
}

bool PruneOverrides(State &state, int64 nowUnix) {
	// Decided before anything moves. Building the kept list first and then
	// discarding it when nothing expired would leave every entry that stayed
	// moved-from - which is to say with an empty preset name, which is to say
	// invisible to OverrideFor(). The common call is the one that finds
	// nothing, so that would have emptied the whole feature on the first tick.
	auto expired = false;
	for (const auto &entry : state.overrides) {
		if (entry.untilUnix <= nowUnix) {
			expired = true;
			break;
		}
	}
	if (!expired) {
		return false;
	}
	auto kept = std::vector<Override>();
	kept.reserve(state.overrides.size());
	for (auto &entry : state.overrides) {
		if (entry.untilUnix > nowUnix) {
			kept.push_back(std::move(entry));
		}
	}
	state.overrides = std::move(kept);
	return true;
}

int64 NextOverrideDeadline(const State &state, const QString &preset) {
	auto result = int64(0);
	if (preset.isEmpty()) {
		return result;
	}
	for (const auto &entry : state.overrides) {
		if (entry.preset.compare(preset, Qt::CaseInsensitive)) {
			continue;
		} else if (!result || entry.untilUnix < result) {
			result = entry.untilUnix;
		}
	}
	return result;
}

QString SerializeState(const State &state) {
	auto result = QString();
	result += u"# Purple Telegram runtime state. This file is written by the "
		"app and\n# rewritten whenever anything in it changes - edit "
		"settings.toml instead.\n\n"_q;
	result += u"active_preset        = %1\n"_q.arg(Quoted(state.activePreset));
	result += u"active_preset_source = %1\n"_q
		.arg(Quoted(PresetSourceName(state.activeSource)));
	result += u"previous_preset      = %1\n"_q
		.arg(Quoted(state.previousPreset));
	result += u"previous_source      = %1\n"_q
		.arg(Quoted(PresetSourceName(state.previousSource)));
	result += u"focus_active         = %1\n"_q.arg(Boolean(state.focusActive));
	result += u"focus_seen           = %1\n"_q.arg(Boolean(state.focusSeen));
	result += u"schedule_paused      = %1\n"_q
		.arg(Boolean(state.schedulePaused));
	result += u"schedule_target      = %1\n"_q
		.arg(Quoted(state.scheduleTarget));
	result += u"peek_active          = %1\n"_q.arg(Boolean(state.peekActive));
	result += u"peek_deadline_unix   = %1\n"_q.arg(state.peekDeadlineUnix);
	result += SerializeOverrides(state.overrides);

	const auto &cache = state.resolvedCache;
	if (!cache.valid()) {
		return result;
	}
	result += u"\n# The last resolution that worked, so a broken settings.toml "
		"leaves the\n# app running on known-good settings instead of on "
		"defaults.\n[resolved_cache]\n"_q;
	result += u"preset = %1\n"_q.arg(Quoted(cache.preset));
	result += u"view_name = %1\n"_q.arg(Quoted(cache.viewName));
	result += u"hide_everywhere = %1\n"_q
		.arg(Boolean(cache.hideEverywhere));
	result += u"hide_archive = %1\n"_q.arg(Boolean(cache.hideArchive));
	result += u"stories = %1\n"_q.arg(Quoted(StoryPolicyName(cache.stories)));
	if (!cache.pinned.empty()) {
		auto ids = QStringList();
		for (const auto id : cache.pinned) {
			ids.push_back(QString::number(id));
		}
		result += u"pinned = [%1]\n"_q.arg(ids.join(u", "_q));
	}
	result += SerializeLists(cache.lists);
	if (!cache.folders.empty()) {
		result += u"folders = [\n"_q;
		for (const auto &folder : cache.folders) {
			//: Each key is written only when the preset said something about
			//: it, because nothing and false mean different things all the way
			//: down - see PresetFolder.
			auto fields = u"name = %1"_q.arg(Quoted(folder.name));
			if (folder.enabled) {
				fields += u", enabled = %1"_q.arg(Boolean(*folder.enabled));
			}
			if (folder.show) {
				fields += u", show = %1"_q.arg(Boolean(*folder.show));
			}
			if (folder.notify) {
				fields += u", notify = %1"_q.arg(Boolean(*folder.notify));
			}
			if (folder.badge) {
				fields += u", badge = %1"_q.arg(Boolean(*folder.badge));
			}
			if (folder.showMode) {
				fields += u", show_mode = %1"_q
					.arg(Quoted(ShowModeName(*folder.showMode)));
			}
			if (folder.stories) {
				fields += u", stories = %1"_q
					.arg(Quoted(StoryModeName(*folder.stories)));
			}
			if (folder.include) {
				fields += u", include_in_main_view = %1"_q
					.arg(Quoted(FolderIncludeName(*folder.include)));
			}
			result += u"  { %1 },\n"_q.arg(fields);
		}
		result += u"]\n"_q;
	}

	// Their own tables rather than inline ones: a view carries a nested array,
	// and TOML inline tables have to fit on a single line.
	for (const auto &view : cache.views) {
		result += u"\n[[resolved_cache.views]]\n"_q;
		result += u"name = %1\n"_q.arg(Quoted(view.name));
		if (!view.pinned.empty()) {
			auto ids = QStringList();
			for (const auto id : view.pinned) {
				ids.push_back(QString::number(id));
			}
			result += u"pinned = [%1]\n"_q.arg(ids.join(u", "_q));
		}
		result += SerializeLists(view.lists);
	}
	return result;
}

} // namespace Purple
