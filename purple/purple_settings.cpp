/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_settings.h"

#include <QtCore/QStringList>

#include <algorithm>
#include <functional>

// Return parse errors instead of throwing, so a hand-edited file with a typo
// degrades to "keep the last good settings and show a banner" rather than to an
// exception crossing a Qt event handler.
#define TOML_EXCEPTIONS 0
#include <toml.hpp>

namespace Purple {
namespace {

constexpr auto kNormalPresetName = "normal";
constexpr auto kPreviousPreset = "previous";
// Not Ctrl+Shift+P, which it used to be: that is the spoiler shortcut every
// message field registers (kSpoilerSequence in lib_ui's input_field.h), so with
// the composer focused Qt saw two claims on the sequence, called it ambiguous
// and fired neither. See ReservedByInputField() below.
constexpr auto kDefaultHotkey = "Ctrl+Shift+E";
constexpr auto kDefaultPeekAutoOff = 120;

// How deep a "*name" spread may nest before we call it a loop. Sets referring
// to sets is the point of them; a set referring to itself is a typo.
constexpr auto kMaxSpreadDepth = 8;

[[nodiscard]] QString Text(std::string_view value) {
	return QString::fromUtf8(value.data(), int(value.size()));
}

[[nodiscard]] QString At(const toml::node &node) {
	return u"line %1"_q.arg(node.source().begin.line);
}

// toml++ keeps tables sorted by key, but the user wrote them in some order and
// both the preset list in the UI and every warning should follow the file
// rather than the alphabet.
[[nodiscard]] auto TablesInFileOrder(
		const toml::table &parent,
		const QString &context,
		std::vector<QString> &warnings) {
	auto result = std::vector<std::pair<QString, const toml::table*>>();
	for (auto &&[key, value] : parent) {
		const auto name = Text(key.str());
		if (const auto table = value.as_table()) {
			result.emplace_back(name, table);
		} else {
			warnings.push_back(u"%1: '%2' is not a table (%3), ignoring it."_q
				.arg(context, name, At(value)));
		}
	}
	std::stable_sort(result.begin(), result.end(), [](
			const auto &a,
			const auto &b) {
		return a.second->source().begin.line < b.second->source().begin.line;
	});
	return result;
}

[[nodiscard]] std::optional<bool> ReadBool(
		const toml::table &table,
		std::string_view key,
		const QString &context,
		std::vector<QString> &warnings) {
	const auto node = table.get(key);
	if (!node) {
		return std::nullopt;
	} else if (const auto value = node->as_boolean()) {
		// as_boolean() rather than value<bool>(), which converts: toml++ hands
		// back true for `1' and false for `0' while refusing `"true"'. Every
		// _p key in the file comes through here, so taking that conversion
		// would mean the whole schema quietly accepting a shape the docs say
		// it does not - and then the first switch flipped in the app rewrites
		// the number as a boolean, so the file the user wrote is not even the
		// file they end up with.
		return value->get();
	}
	warnings.push_back(u"%1: '%2' should be true or false (%3), ignoring it."_q
		.arg(context, Text(key), At(*node)));
	return std::nullopt;
}

[[nodiscard]] std::optional<QString> ReadString(
		const toml::table &table,
		std::string_view key,
		const QString &context,
		std::vector<QString> &warnings) {
	const auto node = table.get(key);
	if (!node) {
		return std::nullopt;
	} else if (const auto value = node->value<std::string_view>()) {
		return Text(*value);
	}
	warnings.push_back(u"%1: '%2' should be a string (%3), ignoring it."_q
		.arg(context, Text(key), At(*node)));
	return std::nullopt;
}

[[nodiscard]] std::optional<ShowMode> ReadShowMode(
		const toml::table &table,
		const QString &context,
		std::vector<QString> &warnings) {
	const auto node = table.get("show_mode");
	if (!node) {
		return std::nullopt;
	}
	const auto text = node->value<std::string_view>();
	const auto value = text ? ParseShowMode(Text(*text)) : std::nullopt;
	if (!value) {
		warnings.push_back(
			u"%1: 'show_mode' should be one of always, message, "
			"message_or_reaction, mention, never (%2), ignoring it."_q
				.arg(context, At(*node)));
	}
	return value;
}

[[nodiscard]] std::optional<StoryMode> ReadStoryMode(
		const toml::table &table,
		const QString &context,
		std::vector<QString> &warnings) {
	const auto node = table.get("stories");
	if (!node) {
		return std::nullopt;
	}
	const auto text = node->value<std::string_view>();
	const auto value = text ? ParseStoryMode(Text(*text)) : std::nullopt;
	if (!value) {
		// Deliberately not naming the preset-level spellings here. "all" on an
		// entry would be a category error - the entry is already a set of
		// people - and offering it would invite writing it.
		warnings.push_back(
			u"%1: 'stories' should be one of always, unseen, never (%2), "
			"ignoring it."_q.arg(context, At(*node)));
	}
	return value;
}

[[nodiscard]] std::optional<FolderInclude> ReadFolderInclude(
		const toml::table &table,
		std::string_view key,
		const QString &context,
		std::vector<QString> &warnings) {
	const auto node = table.get(key);
	if (!node) {
		return std::nullopt;
	}
	const auto text = node->value<std::string_view>();
	const auto value = text ? ParseFolderInclude(Text(*text)) : std::nullopt;
	if (!value) {
		warnings.push_back(
			u"%1: '%2' should be one of none, pinned, all (%3), "
			"ignoring it."_q.arg(context, Text(key), At(*node)));
	}
	return value;
}

// Warns when a key the file no longer understands is still sitting there. The
// config model was rebuilt and the old spellings are gone; silence would let a
// preset that is doing nothing look exactly like a preset that is working.
void WarnRetired(
		const toml::table &table,
		std::string_view key,
		const QString &context,
		const QString &instead,
		std::vector<QString> &warnings) {
	if (const auto node = table.get(key)) {
		warnings.push_back(u"%1: '%2' is no longer a setting (%3); %4."_q
			.arg(context, Text(key), At(*node), instead));
	}
}

[[nodiscard]] bool KnownPresetReference(
		const std::vector<Preset> &presets,
		const QString &name) {
	if (!name.compare(QLatin1String(kNormalPresetName), Qt::CaseInsensitive)) {
		return true;
	}
	return std::any_of(presets.begin(), presets.end(), [&](const Preset &p) {
		return p.name == name;
	});
}

// Peer ids out of an array, deduplicated without reordering: the file is the
// user's, and the order they wrote is the order everything shows back to them.
[[nodiscard]] std::vector<PeerIdValue> ReadIds(
		const toml::array &array,
		const QString &context,
		const QString &what,
		std::vector<QString> &warnings) {
	auto result = std::vector<PeerIdValue>();
	for (auto &&element : array) {
		const auto id = element.value<int64>();
		if (!id) {
			warnings.push_back(
				u"%1: %2 entries should be peer ids (%3), ignoring one."_q
					.arg(context, what, At(element)));
		} else if (std::find(result.begin(), result.end(), *id)
			== result.end()) {
			result.push_back(*id);
		}
	}
	return result;
}

[[nodiscard]] std::vector<ChatKind> ReadKinds(
		const toml::table &table,
		const QString &context,
		std::vector<QString> &warnings) {
	auto result = std::vector<ChatKind>();
	const auto node = table.get("kinds");
	if (!node) {
		return result;
	}
	const auto array = node->as_array();
	if (!array) {
		warnings.push_back(u"%1: 'kinds' should be an array (%2)."_q
			.arg(context, At(*node)));
		return result;
	}
	for (auto &&element : *array) {
		const auto name = element.value<std::string_view>();
		const auto kind = name ? ParseChatKind(Text(*name)) : std::nullopt;
		if (!kind) {
			warnings.push_back(
				u"%1: '%2' is not one of private, groups, channels, bots "
				"(%3)."_q.arg(
					context,
					name ? Text(*name) : u"?"_q,
					At(element)));
		} else if (std::find(result.begin(), result.end(), *kind)
			== result.end()) {
			result.push_back(*kind);
		}
	}
	return result;
}

[[nodiscard]] std::vector<List> ReadLists(
		const toml::table &root,
		std::vector<QString> &warnings) {
	auto result = std::vector<List>();
	const auto node = root.get("lists");
	if (!node) {
		return result;
	} else if (!node->as_table()) {
		warnings.push_back(u"'lists' should be a table (%1)."_q.arg(At(*node)));
		return result;
	}
	const auto &entries = TablesInFileOrder(
		*node->as_table(),
		u"lists"_q,
		warnings);
	for (const auto &[name, table] : entries) {
		const auto context = u"list '%1'"_q.arg(name);
		if (name.startsWith('*')) {
			warnings.push_back(
				u"%1: names starting with '*' are reserved for set references, "
				"ignoring this list."_q.arg(context));
			continue;
		}
		auto list = List();
		list.name = name;
		list.title = ReadString(*table, "title", context, warnings)
			.value_or(name);
		list.kinds = ReadKinds(*table, context, warnings);
		if (const auto members = table->get("members")) {
			if (const auto array = members->as_array()) {
				list.members = ReadIds(
					*array,
					context,
					u"'members'"_q,
					warnings);
			} else {
				warnings.push_back(u"%1: 'members' should be an array (%2)."_q
					.arg(context, At(*members)));
			}
		}
		WarnRetired(
			*table,
			"show",
			context,
			u"a preset decides that, per entry in its 'list_order'"_q,
			warnings);
		WarnRetired(
			*table,
			"notify",
			context,
			u"a preset decides that, per entry in its 'list_order'"_q,
			warnings);
		WarnRetired(
			*table,
			"locked",
			context,
			u"a preset can only reach a list it names"_q,
			warnings);
		result.push_back(std::move(list));
	}
	return result;
}

// The named sequences a "*name" reference splices in. Held as raw arrays and
// expanded on use, so a set that refers to another set costs nothing to declare
// in whichever order reads best.
using SetTables = std::vector<std::pair<QString, const toml::array*>>;

[[nodiscard]] SetTables ReadSets(
		const toml::table &root,
		std::string_view section,
		std::string_view key,
		std::vector<QString> &warnings) {
	auto result = SetTables();
	const auto node = root.get(section);
	if (!node) {
		return result;
	} else if (!node->as_table()) {
		warnings.push_back(u"'%1' should be a table (%2)."_q
			.arg(Text(section), At(*node)));
		return result;
	}
	const auto context = Text(section);
	for (const auto &[name, table] : TablesInFileOrder(
			*node->as_table(),
			context,
			warnings)) {
		const auto inner = u"%1 '%2'"_q.arg(context, name);
		const auto entries = table->get(key);
		if (SpreadReference(AllFoldersName()) == name) {
			warnings.push_back(
				u"%1: '%2' is the built-in \"every folder\" set and cannot be "
				"redefined, ignoring it."_q.arg(inner, AllFoldersName()));
		} else if (!entries) {
			warnings.push_back(u"%1: needs a '%2' array, ignoring it."_q
				.arg(inner, Text(key)));
		} else if (const auto array = entries->as_array()) {
			result.emplace_back(name, array);
		} else {
			warnings.push_back(u"%1: '%2' should be an array (%3)."_q
				.arg(inner, Text(key), At(*entries)));
		}
	}
	return result;
}

[[nodiscard]] const toml::array *FindSet(
		const SetTables &sets,
		const QString &name) {
	const auto i = std::find_if(sets.begin(), sets.end(), [&](const auto &s) {
		return s.first == name;
	});
	return (i == sets.end()) ? nullptr : i->second;
}

// Expands the two array shapes a preset writes: a list_order and a folders
// selection. Both accept inline tables and "*name" references to a set, and
// both resolve a name mentioned twice to its first mention - which for a
// list_order is forced, since order there is capture, and which folders follow
// so there is one rule to remember rather than two.
class Expander final {
public:
	Expander(
		SetTables listSets,
		SetTables folderSets,
		std::vector<QString> &warnings)
	: _listSets(std::move(listSets))
	, _folderSets(std::move(folderSets))
	, _warnings(warnings) {
	}

	[[nodiscard]] std::vector<ListEntry> listOrder(
			const toml::array &array,
			const QString &context) {
		auto result = std::vector<ListEntry>();
		auto visited = QStringList();
		expandLists(array, context, visited, result);
		return result;
	}

	[[nodiscard]] std::vector<PresetFolder> folders(
			const toml::array &array,
			const QString &context) {
		auto result = std::vector<PresetFolder>();
		auto visited = QStringList();
		expandFolders(array, context, visited, result);
		return result;
	}

private:
	// Shared by both shapes: an element is either a "*name" reference, an
	// inline table, or a mistake. Returns the set contents to recurse into,
	// nothing when the caller should read the element as a table.
	[[nodiscard]] const toml::array *reference(
		const toml::node &element,
		const QString &context,
		const SetTables &sets,
		const QString &what,
		QStringList &visited,
		bool *isAllFolders);

	void expandLists(
		const toml::array &array,
		const QString &context,
		QStringList &visited,
		std::vector<ListEntry> &out);
	void expandFolders(
		const toml::array &array,
		const QString &context,
		QStringList &visited,
		std::vector<PresetFolder> &out);

	SetTables _listSets;
	SetTables _folderSets;
	std::vector<QString> &_warnings;

};

const toml::array *Expander::reference(
		const toml::node &element,
		const QString &context,
		const SetTables &sets,
		const QString &what,
		QStringList &visited,
		bool *isAllFolders) {
	const auto text = element.value<std::string_view>();
	if (!text) {
		return nullptr;
	}
	const auto value = Text(*text);
	const auto name = SpreadReference(value);
	if (!name) {
		_warnings.push_back(
			u"%1: '%2' is neither a table nor a \"*set\" reference (%3), "
			"ignoring it."_q.arg(context, value, At(element)));
		return nullptr;
	}
	if (isAllFolders && value == AllFoldersName()) {
		*isAllFolders = true;
		return nullptr;
	}
	if (visited.contains(*name) || visited.size() >= kMaxSpreadDepth) {
		_warnings.push_back(
			u"%1: '%2' refers back into itself (%3), ignoring it."_q
				.arg(context, value, At(element)));
		return nullptr;
	}
	const auto found = FindSet(sets, *name);
	if (!found) {
		_warnings.push_back(u"%1: there is no %2 called '%3' (%4)."_q
			.arg(context, what, *name, At(element)));
		return nullptr;
	}
	visited.push_back(*name);
	return found;
}

void Expander::expandLists(
		const toml::array &array,
		const QString &context,
		QStringList &visited,
		std::vector<ListEntry> &out) {
	for (auto &&element : array) {
		if (const auto table = element.as_table()) {
			const auto name = ReadString(*table, "list", context, _warnings);
			if (!name || name->isEmpty()) {
				_warnings.push_back(
					u"%1: an entry needs 'list' (%2), ignoring it."_q
						.arg(context, At(element)));
				continue;
			}
			const auto inner = u"%1 entry '%2'"_q.arg(context, *name);
			const auto known = std::any_of(out.begin(), out.end(), [&](
					const ListEntry &entry) {
				return entry.list == *name;
			});
			if (known) {
				// Silent when the earlier mention came from a spread: naming
				// an entry and then splicing in a set that also holds it is
				// how you override one thing and take the defaults for the
				// rest, and warning would punish exactly the idiom the spread
				// exists for. An explicit duplicate is still a mistake.
				if (visited.isEmpty()) {
					_warnings.push_back(
						u"%1: '%2' is already claimed further up, so this "
						"entry never decides anything (%3)."_q
							.arg(context, *name, At(element)));
				}
				continue;
			}
			auto entry = ListEntry();
			entry.list = *name;
			entry.show = ReadShowMode(*table, inner, _warnings);
			entry.notify = ReadBool(*table, "notify_p", inner, _warnings);
			entry.stories = ReadStoryMode(*table, inner, _warnings);
			WarnRetired(
				*table,
				"show_p",
				inner,
				u"it is no longer a yes-or-no: write show_mode = \"always\" "
				"or \"never\", or leave it out for the default that suits "
				"the chat"_q,
				_warnings);
			WarnRetired(
				*table,
				"groups_require_mention_p",
				inner,
				u"write show_mode = \"mention\" instead - and note that it "
				"is what a group already defaults to"_q,
				_warnings);
			out.push_back(std::move(entry));
			continue;
		}
		const auto nested = reference(
			element,
			context,
			_listSets,
			u"list_set"_q,
			visited,
			nullptr);
		if (nested) {
			expandLists(*nested, context, visited, out);
			visited.removeLast();
		}
	}
}

void Expander::expandFolders(
		const toml::array &array,
		const QString &context,
		QStringList &visited,
		std::vector<PresetFolder> &out) {
	const auto push = [&](PresetFolder &&folder, const toml::node &at) {
		const auto known = std::any_of(out.begin(), out.end(), [&](
				const PresetFolder &entry) {
			return !entry.name.compare(folder.name, Qt::CaseInsensitive);
		});
		if (known) {
			_warnings.push_back(
				u"%1: '%2' is named more than once, keeping the first (%3)."_q
					.arg(context, folder.name, At(at)));
			return;
		}
		out.push_back(std::move(folder));
	};
	for (auto &&element : array) {
		if (const auto table = element.as_table()) {
			const auto name = ReadString(*table, "name", context, _warnings);
			if (!name || name->isEmpty()) {
				_warnings.push_back(
					u"%1: a folder needs 'name' (%2), ignoring it."_q
						.arg(context, At(element)));
				continue;
			}
			const auto inner = u"%1 folder '%2'"_q.arg(context, *name);
			auto folder = PresetFolder();
			folder.name = *name;
			folder.enabled = ReadBool(*table, "enabled_p", inner, _warnings);
			folder.show = ReadBool(*table, "show_p", inner, _warnings);
			folder.notify = ReadBool(*table, "notify_p", inner, _warnings);
			folder.badge = ReadBool(*table, "badge_p", inner, _warnings);
			folder.showMode = ReadShowMode(*table, inner, _warnings);
			folder.include = ReadFolderInclude(
				*table,
				"include_in_main_view",
				inner,
				_warnings);
			folder.stories = ReadStoryMode(*table, inner, _warnings);
			WarnRetired(
				*table,
				"filtered",
				inner,
				u"use include_in_main_view = \"all\", which says the same "
				"thing the right way round"_q,
				_warnings);
			WarnRetired(
				*table,
				"include_in_main_view_p",
				inner,
				u"it is no longer a yes-or-no: write "
				"include_in_main_view = \"all\" or \"pinned\""_q,
				_warnings);
			WarnRetired(
				*table,
				"pinned_only_p",
				inner,
				u"write include_in_main_view = \"pinned\" instead"_q,
				_warnings);
			push(std::move(folder), element);
			continue;
		}
		auto all = false;
		const auto nested = reference(
			element,
			context,
			_folderSets,
			u"folder_set"_q,
			visited,
			&all);
		if (all) {
			auto folder = PresetFolder();
			folder.name = AllFoldersName();
			push(std::move(folder), element);
		} else if (nested) {
			expandFolders(*nested, context, visited, out);
			visited.removeLast();
		}
	}
}

[[nodiscard]] std::vector<PresetView> ReadViews(
		const toml::table &table,
		Expander &expander,
		const QString &context,
		std::vector<QString> &warnings) {
	auto result = std::vector<PresetView>();
	const auto node = table.get("views");
	if (!node) {
		return result;
	}
	const auto array = node->as_array();
	if (!array) {
		warnings.push_back(u"%1: 'views' should be an array of tables (%2)."_q
			.arg(context, At(*node)));
		return result;
	}
	for (auto &&element : *array) {
		const auto fields = element.as_table();
		if (!fields) {
			warnings.push_back(
				u"%1: should be a [[presets.x.views]] table (%2)."_q
					.arg(context, At(element)));
			continue;
		}
		const auto name = ReadString(*fields, "name", context, warnings);
		if (!name || name->isEmpty()) {
			warnings.push_back(u"%1: a view needs 'name' (%2), ignoring it."_q
				.arg(context, At(element)));
			continue;
		}
		const auto inner = u"%1 view '%2'"_q.arg(context, *name);
		const auto known = std::any_of(result.begin(), result.end(), [&](
				const PresetView &view) {
			return !view.name.compare(*name, Qt::CaseInsensitive);
		});
		if (known) {
			warnings.push_back(
				u"%1: there is already a view called '%2', ignoring it."_q
					.arg(context, *name));
			continue;
		}
		auto view = PresetView();
		view.name = *name;
		if (const auto pinned = fields->get("pinned")) {
			if (const auto ids = pinned->as_array()) {
				view.pinned = ReadIds(*ids, inner, u"'pinned'"_q, warnings);
			} else {
				warnings.push_back(u"%1: 'pinned' should be an array (%2)."_q
					.arg(inner, At(*pinned)));
			}
		}
		if (const auto order = fields->get("list_order")) {
			if (const auto ids = order->as_array()) {
				// Before expanding, so this only ever sees what the view
				// itself wrote. A view picks which chats appear on one tab.
				// Silence is a property of the chat, not of the tab it is
				// being looked at on, so a notify here would be a setting
				// that cannot mean anything.
				//
				// A "*name" spread is deliberately not checked: the set was
				// written for a preset's own order, where notify_p is
				// exactly what it should say, and reusing it on a tab is the
				// whole point of having sets. Warning there would make the
				// idiom unusable without saying anything to act on.
				for (auto &&element : *ids) {
					const auto entry = element.as_table();
					if (!entry || !entry->get("notify_p")) {
						continue;
					}
					const auto named = entry->get_as<std::string>("list");
					warnings.push_back(
						u"%1: 'notify_p' means nothing inside a view - a "
						"chat has one mute state however many tabs show "
						"it - ignoring it on '%2'."_q.arg(
							inner,
							named
								? QString::fromStdString(named->get())
								: u"?"_q));
				}
				view.listOrder = expander.listOrder(*ids, inner);
			} else {
				warnings.push_back(u"%1: 'list_order' should be an array (%2)."_q
					.arg(inner, At(*order)));
			}
		}
		if (view.listOrder.empty()) {
			warnings.push_back(
				u"%1: names no list, so the tab would always be empty; "
				"ignoring it."_q.arg(inner));
			continue;
		}
		result.push_back(std::move(view));
	}
	return result;
}

void WarnUnknownLists(
		const std::vector<ListEntry> &entries,
		const std::vector<List> &lists,
		const QString &context,
		std::vector<QString> &warnings) {
	for (const auto &entry : entries) {
		const auto known = std::any_of(lists.begin(), lists.end(), [&](
				const List &list) {
			return list.name == entry.list;
		});
		if (!known) {
			warnings.push_back(
				u"%1: names list '%2', which has no [lists.%2] table, so it "
				"claims nothing."_q.arg(context, entry.list));
		}
	}
}

[[nodiscard]] std::vector<Preset> ReadPresets(
		const toml::table &root,
		const std::vector<List> &lists,
		Expander &expander,
		std::vector<QString> &warnings) {
	auto result = std::vector<Preset>();
	const auto node = root.get("presets");
	if (!node) {
		return result;
	} else if (!node->as_table()) {
		warnings.push_back(u"'presets' should be a table (%1)."_q
			.arg(At(*node)));
		return result;
	}
	const auto entries = TablesInFileOrder(
		*node->as_table(),
		u"presets"_q,
		warnings);

	for (const auto &[name, table] : entries) {
		if (IsReservedPresetName(name)) {
			warnings.push_back(
				u"preset '%1': that name is reserved, ignoring this preset."_q
					.arg(name));
			continue;
		}
		const auto context = u"preset '%1'"_q.arg(name);
		auto preset = Preset();
		preset.name = name;
		preset.viewName = ReadString(
			*table,
			"default_view_name",
			context,
			warnings
		).value_or(QString()).trimmed();
		preset.hideEverywhere = ReadBool(
			*table,
			"hide_everywhere_p",
			context,
			warnings);
		preset.hideArchive = ReadBool(
			*table,
			"hide_archive_p",
			context,
			warnings);
		preset.hotkey = ReadString(
			*table,
			"hotkey",
			context,
			warnings
		).value_or(QString()).trimmed();
		if (const auto node = table->get("stories")) {
			const auto text = node->value<std::string_view>();
			preset.stories = text
				? ParseStoryPolicy(Text(*text))
				: std::nullopt;
			if (!preset.stories) {
				warnings.push_back(
					u"%1: 'stories' should be one of all, all_unseen, follow, "
					"follow_unseen, none (%2), ignoring it."_q
						.arg(context, At(*node)));
			}
		}

		if (const auto pinned = table->get("pinned")) {
			if (const auto array = pinned->as_array()) {
				preset.pinned = ReadIds(
					*array,
					context,
					u"'pinned'"_q,
					warnings);
			} else {
				warnings.push_back(
					u"%1: 'pinned' should be an array (%2)."_q
						.arg(context, At(*pinned)));
			}
		}

		if (const auto order = table->get("list_order")) {
			if (const auto array = order->as_array()) {
				preset.listOrder = expander.listOrder(*array, context);
			} else {
				warnings.push_back(u"%1: 'list_order' should be an array (%2)."_q
					.arg(context, At(*order)));
			}
		}
		WarnUnknownLists(preset.listOrder, lists, context, warnings);

		if (const auto folders = table->get("folders")) {
			if (const auto array = folders->as_array()) {
				preset.folders = expander.folders(*array, context);
			} else {
				warnings.push_back(u"%1: 'folders' should be an array (%2)."_q
					.arg(context, At(*folders)));
			}
		}
		preset.views = ReadViews(*table, expander, context, warnings);
		for (const auto &view : preset.views) {
			WarnUnknownLists(
				view.listOrder,
				lists,
				u"%1 view '%2'"_q.arg(context, view.name),
				warnings);
		}

		// A view showing a chat the preset has taken out of the app entirely
		// is not a preference, it is a crash: tdesktop asserts that being in a
		// filter implies being in the main chat list.
		if (preset.hideEverywhere.value_or(false) && !preset.views.empty()) {
			warnings.push_back(
				u"%1: 'hide_everywhere_p' takes hidden chats out of the whole "
				"app, which leaves nothing for an extra view to show; dropping "
				"its %2 view(s)."_q.arg(context).arg(preset.views.size()));
			preset.views.clear();
		}

		WarnRetired(
			*table,
			"inherit",
			context,
			u"write what the preset does, or spread a \"*set\" into it"_q,
			warnings);
		WarnRetired(
			*table,
			"overrides",
			context,
			u"put the flags on the 'list_order' entry itself"_q,
			warnings);
		WarnRetired(
			*table,
			"groups_require_mention",
			context,
			u"set 'groups_require_mention_p' on the entries it applies to"_q,
			warnings);
		WarnRetired(
			*table,
			"hide_everywhere",
			context,
			u"it is spelled 'hide_everywhere_p' now"_q,
			warnings);

		if (preset.listOrder.empty()) {
			warnings.push_back(
				u"%1: names no list, so it hides and silences everything."_q
					.arg(context));
		}
		result.push_back(std::move(preset));
	}
	return result;
}

// The rules out of one [[...rules]] array, flat or inside a ruleset. `context'
// names a rule the way its warnings will: "schedule rule 2" for the flat array,
// "schedule ruleset 'phone' rule 2" for one inside a ruleset.
[[nodiscard]] std::vector<ScheduleRule> ReadScheduleRules(
		const toml::array &array,
		const std::vector<Preset> &presets,
		const std::function<QString(int)> &context,
		std::vector<QString> &warnings) {
	auto result = std::vector<ScheduleRule>();
	auto index = 0;
	for (auto &&element : array) {
		const auto where = context(++index);
		const auto fields = element.as_table();
		if (!fields) {
			warnings.push_back(u"%1: should be a table (%2)."_q
				.arg(where, At(element)));
			continue;
		}
		auto rule = ScheduleRule();

		// The counter above is not reset by a `continue', so it still counts
		// the rules that were skipped - which is the whole point: an index
		// that shifted when a rule broke would make every edit made from a
		// screen land on the wrong rule. Inside a ruleset it counts within that
		// ruleset, because that is what the splice ops address by.
		rule.sourceIndex = index - 1;
		rule.sourceLine = int(fields->source().begin.line);
		rule.enabled = ReadBool(*fields, "enabled_p", where, warnings)
			.value_or(true);
		const auto from = ReadString(*fields, "from", where, warnings);
		const auto till = ReadString(*fields, "to", where, warnings);
		const auto preset = ReadString(*fields, "preset", where, warnings);
		if (!from || !till || !preset) {
			warnings.push_back(
				u"%1: needs 'from', 'to' and 'preset', skipping it."_q
					.arg(where));
			continue;
		}
		const auto parsedFrom = ParseTimeOfDay(*from);
		const auto parsedTill = ParseTimeOfDay(*till);
		if (!parsedFrom || !parsedTill) {
			warnings.push_back(
				u"%1: 'from' and 'to' should look like \"09:00\", "
				"skipping it."_q.arg(where));
			continue;
		} else if (*parsedFrom == *parsedTill) {
			warnings.push_back(
				u"%1: 'from' and 'to' are the same time, skipping it."_q
					.arg(where));
			continue;
		} else if (rule.enabled && !KnownPresetReference(presets, *preset)) {
			// Only for a rule that would actually fire. A disabled rule aimed
			// at a preset you have not written yet is the normal state of the
			// example in the starter file, and warning about it would mean a
			// fresh install complains on every single start.
			warnings.push_back(
				u"%1: preset '%2' does not exist, skipping it."_q
					.arg(where, *preset));
			continue;
		}
		rule.from = *parsedFrom;
		rule.till = *parsedTill;
		rule.preset = *preset;

		if (const auto days = fields->get("days")) {
			if (const auto list = days->as_array()) {
				for (auto &&day : *list) {
					const auto name = day.value<std::string_view>();
					const auto parsed = name
						? ParseWeekday(Text(*name))
						: std::nullopt;
					if (parsed) {
						rule.days.push_back(*parsed);
					} else {
						warnings.push_back(
							u"%1: '%2' is not a weekday like \"mon\" (%3)."_q
								.arg(
									where,
									name ? Text(*name) : u"?"_q,
									At(day)));
					}
				}
			} else {
				warnings.push_back(u"%1: 'days' should be an array (%2)."_q
					.arg(where, At(*days)));
			}
		}
		if (rule.days.empty()) {
			warnings.push_back(
				u"%1: no weekdays given, applying it every day."_q.arg(where));
			rule.days = { 1, 2, 3, 4, 5, 6, 7 };
		}
		result.push_back(std::move(rule));
	}
	return result;
}

// A ruleset's own `outside'. Unset and "does not exist" are deliberately the
// same answer here: both leave the question to [schedule] outside, which is a
// value that has already been checked.
[[nodiscard]] std::optional<QString> ReadRulesetOutside(
		const toml::table &fields,
		const std::vector<Preset> &presets,
		const QString &context,
		std::vector<QString> &warnings) {
	const auto outside = ReadString(fields, "outside", context, warnings);
	if (!outside) {
		return std::nullopt;
	} else if (!KnownPresetReference(presets, *outside)) {
		warnings.push_back(
			u"%1: outside: preset '%2' does not exist, using the one the "
			"schedule sets."_q.arg(context, *outside));
		return std::nullopt;
	}
	return outside;
}

[[nodiscard]] std::vector<ScheduleRuleset> ReadRulesets(
		const toml::table &table,
		const std::vector<Preset> &presets,
		std::vector<QString> &warnings) {
	auto result = std::vector<ScheduleRuleset>();
	const auto node = table.get("rulesets");
	if (!node) {
		return result;
	}
	const auto array = node->as_array();
	if (!array) {
		warnings.push_back(u"'schedule.rulesets' should be an array (%1)."_q
			.arg(At(*node)));
		return result;
	}
	auto index = 0;
	for (auto &&element : *array) {
		// Counted the way rules are, the skipped ones included, so a warning
		// names the block a person can count to in the file.
		const auto raw = index++;
		const auto position = u"schedule ruleset %1"_q.arg(raw + 1);
		const auto fields = element.as_table();
		if (!fields) {
			warnings.push_back(u"%1: should be a [[schedule.rulesets]] table "
				"(%2)."_q.arg(position, At(element)));
			continue;
		}
		auto ruleset = ScheduleRuleset();
		ruleset.sourceIndex = raw;
		ruleset.sourceLine = int(fields->source().begin.line);
		ruleset.name = ReadString(*fields, "name", position, warnings)
			.value_or(QString())
			.trimmed();
		if (ruleset.name.isEmpty()) {
			// Skipped rather than given one, because the name is the address
			// every edit from a screen goes through: a ruleset the app cannot
			// name is a ruleset it cannot safely touch.
			warnings.push_back(
				u"%1: needs a 'name', skipping it."_q.arg(position));
			continue;
		}
		const auto taken = std::any_of(
			result.begin(),
			result.end(),
			[&](const ScheduleRuleset &other) {
				return !other.name.compare(ruleset.name, Qt::CaseInsensitive);
			});
		if (taken) {
			warnings.push_back(
				u"%1: another ruleset is already called '%2', skipping it."_q
					.arg(position, ruleset.name));
			continue;
		}
		const auto context = u"schedule ruleset '%1'"_q.arg(ruleset.name);

		if (const auto device = ReadString(
				*fields,
				"device",
				context,
				warnings)) {
			if (device->trimmed().isEmpty()) {
				warnings.push_back(u"%1: 'device' is empty, applying it to "
					"every device."_q.arg(context));
			} else {
				ruleset.device = device->trimmed();
			}
		}
		if (const auto mode = ReadString(*fields, "mode", context, warnings)) {
			if (const auto parsed = ParseRulesetMode(*mode)) {
				ruleset.mode = *parsed;
			} else {
				// Off rather than on. A mode nobody can read is a sentence the
				// file did not finish, and running rules whose reason cannot be
				// read back is the worse of the two ways to be wrong.
				warnings.push_back(u"%1: 'mode' should be \"disabled\", "
					"\"enabled\" or \"always\", switching it off."_q
						.arg(context));
				ruleset.mode = RulesetMode::Disabled;
			}
		}
		ruleset.outside = ReadRulesetOutside(
			*fields,
			presets,
			context,
			warnings);

		if (const auto rules = fields->get("rules")) {
			if (const auto inner = rules->as_array()) {
				ruleset.rules = ReadScheduleRules(
					*inner,
					presets,
					[&](int at) {
						return u"%1 rule %2"_q.arg(context).arg(at);
					},
					warnings);
			} else {
				warnings.push_back(u"%1: 'rules' should be an array (%2)."_q
					.arg(context, At(*rules)));
			}
		}
		result.push_back(std::move(ruleset));
	}
	return result;
}

[[nodiscard]] Schedule ReadSchedule(
		const toml::table &root,
		const std::vector<Preset> &presets,
		std::vector<QString> &warnings) {
	auto result = Schedule();
	const auto node = root.get("schedule");
	if (!node) {
		return result;
	} else if (!node->as_table()) {
		warnings.push_back(u"'schedule' should be a table (%1)."_q
			.arg(At(*node)));
		return result;
	}
	const auto &table = *node->as_table();
	result.enabled = ReadBool(table, "enabled_p", u"schedule"_q, warnings)
		.value_or(true);

	// Read before the rules, so a file that names an outside preset and no
	// windows at all still gets the name checked rather than silently ignored.
	if (const auto outside = ReadString(
			table,
			"outside",
			u"schedule"_q,
			warnings)) {
		if (KnownPresetReference(presets, *outside)) {
			result.outside = *outside;
		} else {
			// Falling back rather than skipping, because there is nothing to
			// skip: something has to be wanted outside every window, and normal
			// is the answer that behaves like the build before this key.
			warnings.push_back(
				u"[schedule] outside: preset '%1' does not exist, using "
				"normal."_q.arg(*outside));
		}
	}

	if (const auto rules = table.get("rules")) {
		if (const auto array = rules->as_array()) {
			result.rules = ReadScheduleRules(
				*array,
				presets,
				[](int at) { return u"schedule rule %1"_q.arg(at); },
				warnings);
		} else {
			warnings.push_back(u"'schedule.rules' should be an array (%1)."_q
				.arg(At(*rules)));
		}
	}
	result.rulesets = ReadRulesets(table, presets, warnings);

	// The flat array joins the rulesets as one of them, first, so everything
	// downstream has a single shape to work with. It is the file's base layer:
	// for every device, always written down, and never more specific than a
	// ruleset that named one - which is exactly what the files that have only
	// this array have always meant.
	if (!result.rules.empty()) {
		auto implicit = ScheduleRuleset();
		implicit.name = u"rules"_q;
		implicit.sourceLine = result.rules.front().sourceLine;
		implicit.rules = result.rules;
		result.rulesets.insert(result.rulesets.begin(), std::move(implicit));
	}
	return result;
}

[[nodiscard]] FocusSync ReadFocusSync(
		const toml::table &root,
		const std::vector<Preset> &presets,
		std::vector<QString> &warnings) {
	auto result = FocusSync();
	result.exitPreset = QString::fromLatin1(kPreviousPreset);
	const auto node = root.get("focus_sync");
	if (!node) {
		return result;
	} else if (!node->as_table()) {
		warnings.push_back(u"'focus_sync' should be a table (%1)."_q
			.arg(At(*node)));
		return result;
	}
	const auto &table = *node->as_table();
	const auto context = u"focus_sync"_q;
	result.enabled = ReadBool(table, "enabled_p", context, warnings)
		.value_or(false);
	result.enterPreset = ReadString(table, "enter_preset", context, warnings)
		.value_or(QString());
	result.exitPreset = ReadString(table, "exit_preset", context, warnings)
		.value_or(QString::fromLatin1(kPreviousPreset));

	if (result.enabled
		&& (result.enterPreset.isEmpty()
			|| !KnownPresetReference(presets, result.enterPreset))) {
		warnings.push_back(
			u"focus_sync: 'enter_preset' is missing or names a preset that "
			"does not exist, turning focus sync off."_q);
		result.enabled = false;
	}
	if (result.exitPreset.compare(
			QLatin1String(kPreviousPreset),
			Qt::CaseInsensitive)
		&& !KnownPresetReference(presets, result.exitPreset)) {
		warnings.push_back(
			u"focus_sync: 'exit_preset' names '%1', which does not exist; "
			"restoring the previous preset instead."_q
				.arg(result.exitPreset));
		result.exitPreset = QString::fromLatin1(kPreviousPreset);
	}
	return result;
}

[[nodiscard]] Peek ReadPeek(
		const toml::table &root,
		std::vector<QString> &warnings) {
	auto result = Peek();
	result.hotkey = QString::fromLatin1(kDefaultHotkey);
	result.autoOffSeconds = kDefaultPeekAutoOff;
	const auto node = root.get("peek");
	if (!node) {
		return result;
	} else if (!node->as_table()) {
		warnings.push_back(u"'peek' should be a table (%1)."_q.arg(At(*node)));
		return result;
	}
	const auto &table = *node->as_table();
	const auto context = u"peek"_q;
	result.hotkey = ReadString(table, "hotkey", context, warnings)
		.value_or(QString::fromLatin1(kDefaultHotkey));
	if (const auto autoOff = ReadString(table, "auto_off", context, warnings)) {
		if (const auto seconds = ParseDuration(*autoOff)) {
			result.autoOffSeconds = *seconds;
		} else {
			warnings.push_back(
				u"peek: 'auto_off' should look like \"2m\", \"90s\" or "
				"\"off\", keeping the default."_q);
		}
	}
	return result;
}

[[nodiscard]] Recent ReadRecent(
		const toml::table &root,
		std::vector<QString> &warnings) {
	auto result = Recent();
	const auto node = root.get("recent");
	if (!node) {
		return result;
	} else if (!node->as_table()) {
		warnings.push_back(
			u"'recent' should be a table (%1)."_q.arg(At(*node)));
		return result;
	}
	const auto &table = *node->as_table();
	const auto context = u"recent"_q;
	const auto key = "stay_visible_after_close";
	if (const auto stay = ReadString(table, key, context, warnings)) {
		if (const auto seconds = ParseDuration(*stay)) {
			result.staySecondsAfterClose = *seconds;
		} else {
			warnings.push_back(
				u"recent: 'stay_visible_after_close' should look like \"2m\", "
				"\"90s\", \"1h\" or \"off\", keeping it off."_q);
		}
	}
	const auto styleKey = "after_close_chat_style";
	if (const auto style = ReadString(table, styleKey, context, warnings)) {
		if (const auto parsed = ParseRecentStyle(*style)) {
			result.style = *parsed;
		} else {
			warnings.push_back(
				u"recent: 'after_close_chat_style' should be \"none\", "
				"\"stripe\" or \"timer\", drawing nothing."_q);
		}
	}
	if (const auto scope = ReadString(table, "applies_to", context, warnings)) {
		if (const auto parsed = ParseRecentScope(*scope)) {
			result.scope = *parsed;
		} else {
			warnings.push_back(
				u"recent: 'applies_to' should be \"already_in_view\", "
				"\"any_open_chat\" or \"any_open_chat_except_in_folder\", "
				"keeping \"%1\"."_q.arg(RecentScopeName(result.scope)));
		}
	}
	return result;
}

[[nodiscard]] Overrides ReadOverrides(
		const toml::table &root,
		std::vector<QString> &warnings) {
	auto result = Overrides();
	const auto node = root.get("overrides");
	if (!node) {
		return result;
	} else if (!node->as_table()) {
		warnings.push_back(
			u"'overrides' should be a table (%1)."_q.arg(At(*node)));
		return result;
	}
	const auto &table = *node->as_table();
	const auto context = u"overrides"_q;
	if (const auto scope = ReadString(table, "hide_scope", context, warnings)) {
		if (const auto parsed = ParseHideScope(*scope)) {
			result.hideScope = *parsed;
		} else {
			warnings.push_back(
				u"overrides: 'hide_scope' should be \"hide_everywhere\", "
				"\"keep_in_folder_but_exclude_from_badge_count\" or "
				"\"keep_in_folder\", keeping \"%1\"."_q.arg(
					HideScopeName(result.hideScope)));
		}
	}
	return result;
}

// [devices]: an id a client reports for itself, and what to call it on a
// screen. Every key here is a device somebody owns, so there is no such thing
// as an unknown key to warn about - only a value that is not a name at all.
[[nodiscard]] std::vector<DeviceLabel> ReadDevices(
		const toml::table &root,
		std::vector<QString> &warnings) {
	auto result = std::vector<DeviceLabel>();
	const auto node = root.get("devices");
	if (!node) {
		return result;
	}
	const auto table = node->as_table();
	if (!table) {
		warnings.push_back(u"'devices' should be a table (%1)."_q
			.arg(At(*node)));
		return result;
	}
	for (auto &&[key, value] : *table) {
		const auto id = Text(key.str()).trimmed();
		const auto label = value.value<std::string_view>();
		if (id.isEmpty()) {
			warnings.push_back(u"devices: an entry has no device id (%1), "
				"ignoring it."_q.arg(At(value)));
		} else if (!label) {
			warnings.push_back(u"devices: '%1' should be a name in quotes "
				"(%2), ignoring it."_q.arg(id, At(value)));
		} else {
			result.push_back({ id, Text(*label).trimmed() });
		}
	}
	return result;
}

[[nodiscard]] Suggestions ReadSuggestions(
		const toml::table &root,
		std::vector<QString> &warnings) {
	auto result = Suggestions();
	const auto node = root.get("suggestions");
	if (!node) {
		return result;
	} else if (!node->as_table()) {
		warnings.push_back(
			u"'suggestions' should be a table (%1)."_q.arg(At(*node)));
		return result;
	}
	const auto &table = *node->as_table();
	result.hideInvisible = ReadBool(
		table,
		"hide_invisible_p",
		u"suggestions"_q,
		warnings
	).value_or(true);
	result.recommendedChannels = ReadBool(
		table,
		"recommended_channels_p",
		u"suggestions"_q,
		warnings
	).value_or(false);
	return result;
}

// [sync]: what settings.toml does on its way to your other devices. Shaped
// exactly like ReadSuggestions, down to the warning for a value that is not a
// table at all, because there is nothing here that deserves to be read
// differently from the rest of the file.
[[nodiscard]] Sync ReadSync(
		const toml::table &root,
		std::vector<QString> &warnings) {
	auto result = Sync();
	const auto node = root.get("sync");
	if (!node) {
		return result;
	} else if (!node->as_table()) {
		warnings.push_back(
			u"'sync' should be a table (%1)."_q.arg(At(*node)));
		return result;
	}
	const auto &table = *node->as_table();
	result.sendAfterSave = ReadBool(
		table,
		"send_after_save_p",
		u"sync"_q,
		warnings
	).value_or(false);
	return result;
}

// One duration key, read the way [peek] auto_off and [recent]
// stay_visible_after_close already are - the same spellings, the same parser,
// and a warning that keeps the default rather than guessing at a number.
[[nodiscard]] int ReadSeconds(
		const toml::table &table,
		std::string_view key,
		const QString &context,
		int fallback,
		std::vector<QString> &warnings) {
	const auto text = ReadString(table, key, context, warnings);
	if (!text) {
		return fallback;
	}
	const auto seconds = ParseDuration(*text);
	if (!seconds) {
		warnings.push_back(
			u"%1: '%2' should look like \"10s\", \"5m\" or \"24h\", keeping "
			"%3 seconds."_q.arg(context, Text(key)).arg(fallback));
		return fallback;
	}
	return *seconds;
}

// [last_seen]: whether a coarse last seen explains itself, and whether the
// trade is offered. Shaped like ReadPeek - two flags and three durations, all
// defaulted, so a file that says nothing gets the documented behaviour.
[[nodiscard]] LastSeen ReadLastSeen(
		const toml::table &root,
		std::vector<QString> &warnings) {
	auto result = LastSeen();
	const auto node = root.get("last_seen");
	if (!node) {
		return result;
	} else if (!node->as_table()) {
		warnings.push_back(
			u"'last_seen' should be a table (%1)."_q.arg(At(*node)));
		return result;
	}
	const auto &table = *node->as_table();
	const auto context = u"last_seen"_q;
	result.reasons = ReadBool(table, "reasons_p", context, warnings)
		.value_or(true);
	result.trade = ReadBool(table, "trade_p", context, warnings)
		.value_or(true);
	result.tradeHoldSeconds = ReadSeconds(
		table,
		"trade_hold",
		context,
		result.tradeHoldSeconds,
		warnings);
	result.tradeRememberSeconds = ReadSeconds(
		table,
		"trade_remember",
		context,
		result.tradeRememberSeconds,
		warnings);
	result.tradeCooldownSeconds = ReadSeconds(
		table,
		"trade_cooldown",
		context,
		result.tradeCooldownSeconds,
		warnings);
	return result;
}

// One budget's `target' string. It is one string rather than four keys
// because a budget counts exactly one thing, and four mutually exclusive keys
// would let a file ask it to count two.
[[nodiscard]] bool ReadBudgetTarget(
		ScreenTimeBudget &budget,
		const QString &where,
		std::vector<QString> &warnings) {
	const auto text = budget.target.trimmed();
	if (!text.compare(u"all"_q, Qt::CaseInsensitive)) {
		budget.kind = BudgetTarget::All;
		return true;
	}
	const auto colon = text.indexOf(':');
	if (colon <= 0) {
		warnings.push_back(
			u"%1: 'target' should be \"all\", \"chat:<id>\", \"kind:<kind>\" "
			"or \"preset:<name>\", skipping it."_q.arg(where));
		return false;
	}
	const auto prefix = text.left(colon).trimmed().toLower();
	const auto rest = text.mid(colon + 1).trimmed();
	if (prefix == u"chat"_q) {
		auto ok = false;
		const auto id = rest.toLongLong(&ok);
		if (!ok || !id) {
			warnings.push_back(u"%1: 'target' names chat '%2', which is not a "
				"chat id, skipping it."_q.arg(where, rest));
			return false;
		}
		budget.kind = BudgetTarget::Chat;
		budget.chat = id;
		return true;
	} else if (prefix == u"kind"_q) {
		const auto parsed = ParseScreenTimeKind(rest);
		if (!parsed) {
			warnings.push_back(u"%1: 'target' names kind '%2', which is not "
				"\"private\", \"groups\", \"channels\", \"bots\" or "
				"\"elsewhere\", skipping it."_q.arg(where, rest));
			return false;
		}
		budget.kind = BudgetTarget::Kind;
		budget.chatKind = *parsed;
		return true;
	} else if (prefix == u"preset"_q) {
		if (rest.isEmpty()) {
			warnings.push_back(u"%1: 'target' names no preset, skipping it."_q
				.arg(where));
			return false;
		}
		budget.kind = BudgetTarget::Preset;
		budget.preset = rest;
		return true;
	}
	warnings.push_back(
		u"%1: 'target' should be \"all\", \"chat:<id>\", \"kind:<kind>\" or "
		"\"preset:<name>\", skipping it."_q.arg(where));
	return false;
}

[[nodiscard]] std::vector<ScreenTimeBudget> ReadBudgets(
		const toml::table &table,
		std::vector<QString> &warnings) {
	auto result = std::vector<ScreenTimeBudget>();
	const auto node = table.get("budgets");
	if (!node) {
		return result;
	}
	const auto array = node->as_array();
	if (!array) {
		warnings.push_back(u"screen_time: 'budgets' should be a list of "
			"[[screen_time.budgets]] blocks (%1)."_q.arg(At(*node)));
		return result;
	}
	auto index = 0;
	for (auto &&element : *array) {
		const auto where = u"screen_time budget %1"_q.arg(++index);
		const auto fields = element.as_table();
		if (!fields) {
			warnings.push_back(u"%1: should be a table (%2)."_q
				.arg(where, At(element)));
			continue;
		}
		auto budget = ScreenTimeBudget();

		// Counted before anything can reject the budget, so the ones the parser
		// throws away still take up their place in the array. An index that
		// shifted when a budget broke would make every edit made from a screen
		// land on the wrong budget.
		budget.sourceIndex = index - 1;
		const auto target = ReadString(*fields, "target", where, warnings);
		const auto perDay = ReadString(*fields, "per_day", where, warnings);
		if (!target || !perDay) {
			warnings.push_back(
				u"%1: needs 'target' and 'per_day', skipping it."_q.arg(where));
			continue;
		}
		budget.target = *target;
		if (!ReadBudgetTarget(budget, where, warnings)) {
			continue;
		}
		const auto seconds = ParseDuration(*perDay);
		if (!seconds) {
			warnings.push_back(u"%1: 'per_day' should look like \"45m\" or "
				"\"2h\", skipping it."_q.arg(where));
			continue;
		}
		budget.perDaySeconds = *seconds;
		if (const auto mode = ReadString(*fields, "mode", where, warnings)) {
			if (const auto parsed = ParseBudgetMode(*mode)) {
				budget.mode = *parsed;
			} else {
				warnings.push_back(u"%1: 'mode' should be \"soft\" or "
					"\"hard\", keeping \"soft\"."_q.arg(where));
			}
		}
		budget.snoozeSeconds = ReadSeconds(
			*fields,
			"snooze",
			where,
			budget.snoozeSeconds,
			warnings);
		if (const auto count = fields->get("snoozes_per_day")) {
			const auto value = count->value<int>();
			if (!value || *value < 0) {
				warnings.push_back(u"%1: 'snoozes_per_day' should be a whole "
					"number, keeping %2."_q.arg(where)
						.arg(budget.snoozesPerDay));
			} else {
				budget.snoozesPerDay = *value;
			}
		}
		result.push_back(std::move(budget));
	}
	return result;
}

// [screen_time]: the thresholds the log is read back through, and the budgets.
// Every threshold is applied at read time rather than while recording, which
// is what makes changing one re-derive the history it already has.
[[nodiscard]] ScreenTime ReadScreenTime(
		const toml::table &root,
		std::vector<QString> &warnings) {
	auto result = ScreenTime();
	const auto node = root.get("screen_time");
	if (!node) {
		return result;
	} else if (!node->as_table()) {
		warnings.push_back(
			u"'screen_time' should be a table (%1)."_q.arg(At(*node)));
		return result;
	}
	const auto &table = *node->as_table();
	const auto context = u"screen_time"_q;
	result.enabled = ReadBool(table, "enabled_p", context, warnings)
		.value_or(false);
	result.actionSpanSeconds = ReadSeconds(
		table,
		"action_span",
		context,
		result.actionSpanSeconds,
		warnings);
	result.activeGapSeconds = ReadSeconds(
		table,
		"active_gap",
		context,
		result.activeGapSeconds,
		warnings);
	result.idleAfterSeconds = ReadSeconds(
		table,
		"idle_after",
		context,
		result.idleAfterSeconds,
		warnings);
	if (const auto days = table.get("retention_days")) {
		const auto value = days->value<int>();
		if (!value || *value < 0) {
			warnings.push_back(u"screen_time: 'retention_days' should be a "
				"whole number of days (%1), keeping %2."_q.arg(At(*days))
					.arg(result.retentionDays));
		} else {
			result.retentionDays = *value;
		}
	}
	result.budgets = ReadBudgets(table, warnings);
	return result;
}

// The one key that is about the file rather than about anything in it. A
// number this build does not recognise is not an error: the keys it does know
// are still where they were, so it reads what it understands and says out loud
// that there may be more of the file than it can see.
[[nodiscard]] int ReadVersion(
		const toml::table &root,
		std::vector<QString> &warnings) {
	const auto result = kSettingsVersion;
	const auto node = root.get("version");
	if (!node) {
		return result;
	}
	const auto value = node->value<int>();
	if (!value) {
		warnings.push_back(
			u"'version' should be a whole number (%1), reading the file as "
			"version %2."_q.arg(At(*node)).arg(result));
		return result;
	} else if (*value < 1) {
		warnings.push_back(
			u"'version' should be 1 or more (%1), reading the file as version "
			"%2."_q.arg(At(*node)).arg(result));
		return result;
	} else if (*value > kSettingsVersion) {
		warnings.push_back(
			u"settings.toml is version %1, written by a newer Purple Telegram "
			"(%2); this build understands version %3 and may ignore keys it "
			"has never heard of."_q
				.arg(*value)
				.arg(At(*node))
				.arg(kSettingsVersion));
	}
	return *value;
}

} // namespace

std::optional<RecentScope> ParseRecentScope(const QString &value) {
	const auto trimmed = value.trimmed().toLower();
	if (trimmed == u"already_in_view"_q) {
		return RecentScope::AlreadyInView;
	} else if (trimmed == u"any_open_chat"_q) {
		return RecentScope::AnyOpenChat;
	} else if (trimmed == u"any_open_chat_except_in_folder"_q) {
		return RecentScope::AnyOpenChatExceptInFolder;
	}
	return std::nullopt;
}

QString RecentScopeName(RecentScope value) {
	switch (value) {
	case RecentScope::AlreadyInView: return u"already_in_view"_q;
	case RecentScope::AnyOpenChat: return u"any_open_chat"_q;
	case RecentScope::AnyOpenChatExceptInFolder:
		return u"any_open_chat_except_in_folder"_q;
	}
	return u"already_in_view"_q;
}

std::optional<ChatKind> ParseChatKind(const QString &value) {
	const auto trimmed = value.trimmed().toLower();
	if (trimmed == u"private"_q) {
		return ChatKind::Private;
	} else if (trimmed == u"groups"_q) {
		return ChatKind::Group;
	} else if (trimmed == u"channels"_q) {
		return ChatKind::Channel;
	} else if (trimmed == u"bots"_q) {
		return ChatKind::Bot;
	}
	return std::nullopt;
}

QString ChatKindName(ChatKind kind) {
	switch (kind) {
	case ChatKind::Private: return u"private"_q;
	case ChatKind::Group: return u"groups"_q;
	case ChatKind::Channel: return u"channels"_q;
	case ChatKind::Bot: return u"bots"_q;
	}
	return QString();
}

std::optional<ShowMode> ParseShowMode(const QString &value) {
	const auto trimmed = value.trimmed().toLower();
	if (trimmed == u"always"_q) {
		return ShowMode::Always;
	} else if (trimmed == u"message"_q) {
		return ShowMode::Message;
	} else if (trimmed == u"message_or_reaction"_q) {
		return ShowMode::MessageOrReaction;
	} else if (trimmed == u"mention"_q) {
		return ShowMode::Mention;
	} else if (trimmed == u"never"_q) {
		return ShowMode::Never;
	}
	return std::nullopt;
}

QString ShowModeName(ShowMode value) {
	switch (value) {
	case ShowMode::Always: return u"always"_q;
	case ShowMode::Message: return u"message"_q;
	case ShowMode::MessageOrReaction: return u"message_or_reaction"_q;
	case ShowMode::Mention: return u"mention"_q;
	case ShowMode::Never: return u"never"_q;
	}
	return QString();
}

std::optional<RecentStyle> ParseRecentStyle(const QString &value) {
	const auto trimmed = value.trimmed().toLower();
	if (trimmed == u"none"_q) {
		return RecentStyle::None;
	} else if (trimmed == u"stripe"_q) {
		return RecentStyle::Stripe;
	} else if (trimmed == u"timer"_q) {
		return RecentStyle::Timer;
	}
	return std::nullopt;
}

QString RecentStyleName(RecentStyle value) {
	switch (value) {
	case RecentStyle::None: return u"none"_q;
	case RecentStyle::Stripe: return u"stripe"_q;
	case RecentStyle::Timer: return u"timer"_q;
	}
	return QString();
}

std::optional<HideScope> ParseHideScope(const QString &value) {
	const auto trimmed = value.trimmed().toLower();
	if (trimmed == u"hide_everywhere"_q) {
		return HideScope::Everywhere;
	} else if (trimmed == u"keep_in_folder_but_exclude_from_badge_count"_q) {
		return HideScope::KeepInFolderUncounted;
	} else if (trimmed == u"keep_in_folder"_q) {
		return HideScope::KeepInFolder;
	}
	return std::nullopt;
}

QString HideScopeName(HideScope value) {
	switch (value) {
	case HideScope::Everywhere: return u"hide_everywhere"_q;
	case HideScope::KeepInFolderUncounted:
		return u"keep_in_folder_but_exclude_from_badge_count"_q;
	case HideScope::KeepInFolder: return u"keep_in_folder"_q;
	}
	return QString();
}

std::optional<ScreenTimeKind> ParseScreenTimeKind(const QString &value) {
	const auto trimmed = value.trimmed().toLower();
	if (trimmed == u"private"_q) {
		return ScreenTimeKind::Private;
	} else if (trimmed == u"groups"_q || trimmed == u"group"_q) {
		return ScreenTimeKind::Group;
	} else if (trimmed == u"channels"_q || trimmed == u"channel"_q) {
		return ScreenTimeKind::Channel;
	} else if (trimmed == u"bots"_q || trimmed == u"bot"_q) {
		return ScreenTimeKind::Bot;
	} else if (trimmed == u"elsewhere"_q) {
		return ScreenTimeKind::Elsewhere;
	}
	return std::nullopt;
}

QString ScreenTimeKindName(ScreenTimeKind value) {
	switch (value) {
	case ScreenTimeKind::Private: return u"private"_q;
	case ScreenTimeKind::Group: return u"groups"_q;
	case ScreenTimeKind::Channel: return u"channels"_q;
	case ScreenTimeKind::Bot: return u"bots"_q;
	case ScreenTimeKind::Elsewhere: return u"elsewhere"_q;
	}
	return QString();
}

ScreenTimeKind ScreenTimeKindFor(ChatKind kind) {
	switch (kind) {
	case ChatKind::Private: return ScreenTimeKind::Private;
	case ChatKind::Group: return ScreenTimeKind::Group;
	case ChatKind::Channel: return ScreenTimeKind::Channel;
	case ChatKind::Bot: return ScreenTimeKind::Bot;
	}
	return ScreenTimeKind::Elsewhere;
}

std::optional<BudgetMode> ParseBudgetMode(const QString &value) {
	const auto trimmed = value.trimmed().toLower();
	if (trimmed == u"soft"_q) {
		return BudgetMode::Soft;
	} else if (trimmed == u"hard"_q) {
		return BudgetMode::Hard;
	}
	return std::nullopt;
}

QString BudgetModeName(BudgetMode value) {
	switch (value) {
	case BudgetMode::Soft: return u"soft"_q;
	case BudgetMode::Hard: return u"hard"_q;
	}
	return QString();
}

LastSeenReason ReasonFor(bool exactKnown, bool coarse, bool byMe) {
	// Exact first, and unconditionally: a status carrying a real moment has
	// nothing to explain, and a `by_me' flag riding along on one would be the
	// server describing a coarsening that did not happen.
	if (exactKnown) {
		return LastSeenReason::None;
	} else if (!coarse) {
		// userStatusEmpty - "a long time ago" - or a status with nothing
		// usable in it. Inactivity and a block look identical here and the
		// server does not say which, so the fork says nothing rather than
		// guessing at the one answer it would be unforgivable to get wrong.
		return LastSeenReason::None;
	}
	return byMe ? LastSeenReason::ByMe : LastSeenReason::HiddenByThem;
}

std::optional<StoryPolicy> ParseStoryPolicy(const QString &value) {
	const auto trimmed = value.trimmed().toLower();
	if (trimmed == u"all"_q) {
		return StoryPolicy::All;
	} else if (trimmed == u"all_unseen"_q) {
		return StoryPolicy::AllUnseen;
	} else if (trimmed == u"follow"_q) {
		return StoryPolicy::Follow;
	} else if (trimmed == u"follow_unseen"_q) {
		return StoryPolicy::FollowUnseen;
	} else if (trimmed == u"none"_q) {
		return StoryPolicy::None;
	}
	return std::nullopt;
}

QString StoryPolicyName(StoryPolicy value) {
	switch (value) {
	case StoryPolicy::All: return u"all"_q;
	case StoryPolicy::AllUnseen: return u"all_unseen"_q;
	case StoryPolicy::Follow: return u"follow"_q;
	case StoryPolicy::FollowUnseen: return u"follow_unseen"_q;
	case StoryPolicy::None: return u"none"_q;
	}
	return QString();
}

std::optional<StoryMode> ParseStoryMode(const QString &value) {
	const auto trimmed = value.trimmed().toLower();
	if (trimmed == u"always"_q) {
		return StoryMode::Always;
	} else if (trimmed == u"unseen"_q) {
		return StoryMode::Unseen;
	} else if (trimmed == u"never"_q) {
		return StoryMode::Never;
	}
	return std::nullopt;
}

QString StoryModeName(StoryMode value) {
	switch (value) {
	case StoryMode::Always: return u"always"_q;
	case StoryMode::Unseen: return u"unseen"_q;
	case StoryMode::Never: return u"never"_q;
	}
	return QString();
}

ShowMode DefaultShowMode(ChatKind kind) {
	switch (kind) {
	case ChatKind::Channel:
	case ChatKind::Bot: return ShowMode::Always;
	case ChatKind::Group: return ShowMode::Mention;
	case ChatKind::Private: return ShowMode::Message;
	}
	return ShowMode::Message;
}

int ShowModeRank(ShowMode value) {
	switch (value) {
	case ShowMode::Never: return 0;
	case ShowMode::Mention: return 1;
	case ShowMode::Message: return 2;
	case ShowMode::MessageOrReaction: return 3;
	case ShowMode::Always: return 4;
	}
	return 0;
}

bool ShowModeWatchesUnread(ShowMode value) {
	switch (value) {
	case ShowMode::Always:
	case ShowMode::Never: return false;
	case ShowMode::Message:
	case ShowMode::MessageOrReaction:
	case ShowMode::Mention: return true;
	}
	return false;
}

std::optional<FolderInclude> ParseFolderInclude(const QString &value) {
	const auto trimmed = value.trimmed().toLower();
	if (trimmed == u"none"_q) {
		return FolderInclude::None;
	} else if (trimmed == u"pinned"_q) {
		return FolderInclude::Pinned;
	} else if (trimmed == u"all"_q) {
		return FolderInclude::All;
	}
	return std::nullopt;
}

QString FolderIncludeName(FolderInclude value) {
	switch (value) {
	case FolderInclude::None: return u"none"_q;
	case FolderInclude::Pinned: return u"pinned"_q;
	case FolderInclude::All: return u"all"_q;
	}
	return QString();
}

bool IsReservedPresetName(const QString &name) {
	return !name.compare(
		QLatin1String(kNormalPresetName),
		Qt::CaseInsensitive);
}

bool IsPreviousPresetName(const QString &name) {
	return !name.compare(
		QLatin1String(kPreviousPreset),
		Qt::CaseInsensitive);
}

QString DefaultViewName(const QString &preset) {
	auto result = preset.trimmed();
	if (result.isEmpty()) {
		return result;
	}
	// A capital anywhere means the casing was already decided. "iH" is a name,
	// not a lower-case word waiting to be tidied, and capitalising it hands
	// back something the user did not write. Only a name with no capital at all
	// is one nobody has expressed an opinion about.
	for (const auto ch : result) {
		if (ch.isUpper()) {
			return result;
		}
	}
	// In place, and only the first character: QString::toUpper() on the whole
	// name would shout a preset deliberately written in caps back at the user.
	result[0] = result[0].toUpper();
	return result;
}

QString PresetTitle(const QString &name, const QString &viewName) {
	return viewName.isEmpty() ? DefaultViewName(name) : viewName;
}

QString PresetTitle(const Preset &preset) {
	return PresetTitle(preset.name, preset.viewName);
}

std::optional<QString> SpreadReference(const QString &value) {
	const auto trimmed = value.trimmed();
	if (trimmed.size() < 2 || !trimmed.startsWith('*')) {
		return std::nullopt;
	}
	return trimmed.mid(1);
}

const QString &AllFoldersName() {
	static const auto result = u"*ALL"_q;
	return result;
}

bool IsAllFolders(const PresetFolder &folder) {
	return (folder.name == AllFoldersName());
}

std::optional<int> ParseDuration(const QString &value) {
	const auto trimmed = value.trimmed();
	if (trimmed.isEmpty()) {
		return std::nullopt;
	} else if (!trimmed.compare(u"off"_q, Qt::CaseInsensitive)
		|| !trimmed.compare(u"none"_q, Qt::CaseInsensitive)) {
		return 0;
	}
	auto multiplier = 1;
	auto digits = trimmed;
	const auto last = trimmed.back().toLower();
	if (last == 's' || last == 'm' || last == 'h') {
		multiplier = (last == 's') ? 1 : (last == 'm') ? 60 : 3600;
		digits = trimmed.left(trimmed.size() - 1).trimmed();
	}
	auto ok = false;
	const auto number = digits.toInt(&ok);
	if (!ok || number < 0) {
		return std::nullopt;
	}
	return number * multiplier;
}

std::optional<int> ParseTimeOfDay(const QString &value) {
	const auto parts = value.trimmed().split(':');
	if (parts.size() != 2) {
		return std::nullopt;
	}
	auto hoursOk = false;
	auto minutesOk = false;
	const auto hours = parts[0].toInt(&hoursOk);
	const auto minutes = parts[1].toInt(&minutesOk);
	if (!hoursOk || !minutesOk
		|| hours < 0 || hours > 23
		|| minutes < 0 || minutes > 59) {
		return std::nullopt;
	}
	return hours * 60 + minutes;
}

QString TimeOfDayText(int minutes) {
	if (minutes < 0 || minutes >= 24 * 60) {
		return QString();
	}
	return u"%1:%2"_q
		.arg(minutes / 60, 2, 10, QChar('0'))
		.arg(minutes % 60, 2, 10, QChar('0'));
}

// The one spelling of the weekdays, shared by the two directions so they can
// never drift apart.
[[nodiscard]] static const std::vector<QString> &WeekdayNames() {
	static const auto result = std::vector<QString>{
		u"mon"_q, u"tue"_q, u"wed"_q, u"thu"_q, u"fri"_q, u"sat"_q, u"sun"_q,
	};
	return result;
}

std::optional<RulesetMode> ParseRulesetMode(const QString &value) {
	const auto trimmed = value.trimmed().toLower();
	if (trimmed == u"disabled"_q) {
		return RulesetMode::Disabled;
	} else if (trimmed == u"enabled"_q) {
		return RulesetMode::Enabled;
	} else if (trimmed == u"always"_q) {
		return RulesetMode::Always;
	}
	return std::nullopt;
}

QString RulesetModeName(RulesetMode value) {
	switch (value) {
	case RulesetMode::Disabled: return u"disabled"_q;
	case RulesetMode::Enabled: return u"enabled"_q;
	case RulesetMode::Always: return u"always"_q;
	}
	return QString();
}

std::optional<int> ParseWeekday(const QString &value) {
	const auto &names = WeekdayNames();
	const auto trimmed = value.trimmed().toLower();
	for (auto i = 0; i != int(names.size()); ++i) {
		if (trimmed == names[i] || trimmed.startsWith(names[i])) {
			return i + 1;
		}
	}
	return std::nullopt;
}

QString WeekdayName(int day) {
	const auto &names = WeekdayNames();
	return (day < 1 || day > int(names.size()))
		? QString()
		: names[day - 1];
}

const List *Settings::list(const QString &name) const {
	const auto i = std::find_if(
		lists.begin(),
		lists.end(),
		[&](const List &list) { return list.name == name; });
	return (i == lists.end()) ? nullptr : &*i;
}

const Preset *Settings::preset(const QString &name) const {
	const auto i = std::find_if(
		presets.begin(),
		presets.end(),
		[&](const Preset &preset) { return preset.name == name; });
	return (i == presets.end()) ? nullptr : &*i;
}

const DeviceLabel *Settings::device(const QString &id) const {
	const auto i = std::find_if(
		devices.begin(),
		devices.end(),
		[&](const DeviceLabel &device) {
			return !device.id.compare(id, Qt::CaseInsensitive);
		});
	return (i == devices.end()) ? nullptr : &*i;
}

ParseResult ParseSettings(const QString &text, const QString &path) {
	auto result = ParseResult();
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		result.error = u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description()));
		return result;
	}
	const auto root = std::move(parsed).table();

	result.settings.version = ReadVersion(root, result.warnings);

	if (const auto premium = root.get("premium")) {
		if (const auto table = premium->as_table()) {
			result.settings.premium.enabled = ReadBool(
				*table,
				"enabled_p",
				u"premium"_q,
				result.warnings).value_or(true);
			WarnRetired(
				*table,
				"enabled",
				u"premium"_q,
				u"it is spelled 'enabled_p' now"_q,
				result.warnings);
		} else {
			result.warnings.push_back(u"'premium' should be a table (%1)."_q
				.arg(At(*premium)));
		}
	}
	if (const auto retired = root.get("list_order")) {
		result.warnings.push_back(
			u"'list_order' at the top of the file is no longer a setting (%1); "
			"each preset writes its own."_q.arg(At(*retired)));
	}

	result.settings.lists = ReadLists(root, result.warnings);

	auto expander = Expander(
		ReadSets(root, "list_sets", "list_order", result.warnings),
		ReadSets(root, "folder_sets", "folders", result.warnings),
		result.warnings);
	result.settings.presets = ReadPresets(
		root,
		result.settings.lists,
		expander,
		result.warnings);
	result.settings.schedule = ReadSchedule(
		root,
		result.settings.presets,
		result.warnings);
	result.settings.focusSync = ReadFocusSync(
		root,
		result.settings.presets,
		result.warnings);
	result.settings.peek = ReadPeek(root, result.warnings);
	result.settings.recent = ReadRecent(root, result.warnings);
	result.settings.overrides = ReadOverrides(root, result.warnings);
	result.settings.suggestions = ReadSuggestions(root, result.warnings);
	result.settings.sync = ReadSync(root, result.warnings);
	result.settings.lastSeen = ReadLastSeen(root, result.warnings);
	result.settings.screenTime = ReadScreenTime(root, result.warnings);
	result.settings.devices = ReadDevices(root, result.warnings);

	// Hotkeys last, because this is the one check that needs the presets and
	// [peek] at once. Two actions holding the same sequence make it ambiguous
	// and Qt then fires NEITHER, so a silent duplicate does not pick a winner -
	// it breaks both keys, which is worth a warning even though nothing here
	// can stop it.
	//
	// Compared after a crude normalisation rather than through QKeySequence:
	// this file is compiled standalone against Qt Core by purple/test_config.sh,
	// and QKeySequence is QtGui. So "Ctrl+Shift+W" twice is caught and
	// "Ctrl+Shift+W" against "Shift+Ctrl+W" is not - the common mistake, not
	// every mistake.
	const auto normalise = [](const QString &keys) {
		return keys.trimmed().toLower().remove(QChar(' '));
	};

	// A key a message field claims for itself. Those are registered as
	// Qt::WidgetShortcut on the field, so they only join the contest while one
	// has focus - which is why a clash here looks like "my key works until I
	// click the message box". Qt calls the sequence ambiguous and fires
	// neither, so the loss is silent and there is nothing to see in the log.
	//
	// Compared as normalised text rather than as QKeySequence because this file
	// is compiled standalone against Qt Core, and QKeySequence is QtGui.
	const auto reservedBy = [](const QString &key) -> QString {
		static const auto kReserved = std::vector<std::pair<QString, QString>>{
			{ u"ctrl+b"_q, u"bold"_q },
			{ u"ctrl+i"_q, u"italic"_q },
			{ u"ctrl+u"_q, u"underline"_q },
			{ u"ctrl+k"_q, u"insert link"_q },
			{ u"ctrl+shift+x"_q, u"strikethrough"_q },
			{ u"ctrl+shift+m"_q, u"monospace"_q },
			{ u"ctrl+shift+n"_q, u"clear formatting"_q },
			{ u"ctrl+shift+p"_q, u"spoiler"_q },
			{ u"ctrl+shift+d"_q, u"edit date"_q },
			{ u"ctrl+shift+."_q, u"blockquote"_q },
		};
		for (const auto &[taken, what] : kReserved) {
			if (taken == key) {
				return what;
			}
		}
		return QString();
	};
	auto claimed = std::vector<std::pair<QString, QString>>();
	const auto owner = [&](const QString &key) -> const QString* {
		for (const auto &[taken, by] : claimed) {
			if (taken == key) {
				return &by;
			}
		}
		return nullptr;
	};
	if (!result.settings.peek.hotkey.isEmpty()) {
		const auto key = normalise(result.settings.peek.hotkey);
		if (const auto what = reservedBy(key); !what.isEmpty()) {
			result.warnings.push_back(
				u"peek: hotkey '%1' is what a message field uses for %2, so it "
				"will do nothing while the composer has focus. Pick another."_q
					.arg(result.settings.peek.hotkey, what));
		}
		claimed.emplace_back(key, u"peek"_q);
	}
	for (auto &preset : result.settings.presets) {
		if (preset.hotkey.isEmpty()) {
			continue;
		}
		const auto key = normalise(preset.hotkey);
		if (const auto by = owner(key)) {
			result.warnings.push_back(
				u"preset '%1': hotkey '%2' is already %3's; dropping it, or Qt "
				"would fire neither."_q.arg(preset.name, preset.hotkey, *by));
			preset.hotkey = QString();
			continue;
		}
		if (const auto what = reservedBy(key); !what.isEmpty()) {
			result.warnings.push_back(
				u"preset '%1': hotkey '%2' is what a message field uses for "
				"%3, so it will do nothing while the composer has focus. Pick "
				"another."_q.arg(preset.name, preset.hotkey, what));
		}
		claimed.emplace_back(key, u"preset '%1'"_q.arg(preset.name));
	}
	return result;
}

} // namespace Purple
