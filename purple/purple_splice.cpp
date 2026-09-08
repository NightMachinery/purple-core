/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_splice.h"

#include <QtCore/QStringList>

#include <algorithm>

#define TOML_EXCEPTIONS 0
#include <toml.hpp>

namespace Purple {
namespace {

constexpr auto kDefaultIndent = "  ";

struct Position {
	int line = 0; // 1-based, matching toml++ source positions.
	int column = 0; // 1-based.
};

[[nodiscard]] QString Text(std::string_view value) {
	return QString::fromUtf8(value.data(), int(value.size()));
}

// A display name is arbitrary user-controlled text and this ends up on a line
// of TOML, so anything that could break out of the comment has to go. Only line
// breaks can: everything after '#' is comment until end of line.
[[nodiscard]] QString CommentText(const MemberTitle &title, PeerIdValue id) {
	if (!title) {
		return QString();
	}
	auto result = title(id);
	if (result.isEmpty()) {
		return QString();
	}
	for (auto &ch : result) {
		if (ch == '\n' || ch == '\r' || ch == '\t') {
			ch = ' ';
		}
	}
	result = result.simplified();
	return result.isEmpty() ? QString() : (u" # "_q + result);
}

[[nodiscard]] QString Indentation(const QString &line) {
	auto i = 0;
	while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
		++i;
	}
	return line.left(i);
}

// True for a line inside the array that carries no id: blank, or the user's own
// comment. Those have to survive a member being removed next to them.
[[nodiscard]] bool BlankOrComment(const QString &line) {
	const auto trimmed = line.trimmed();
	return trimmed.isEmpty() || trimmed.startsWith('#');
}

// The id on a canonical member line, if the line is one. Anything else - two
// ids on a line, a trailing expression, a nested array - returns nothing, and
// the caller falls back to rewriting the whole array.
[[nodiscard]] std::optional<PeerIdValue> MemberOnLine(const QString &line) {
	auto rest = line.trimmed();
	const auto hash = rest.indexOf('#');
	if (hash >= 0) {
		rest = rest.left(hash).trimmed();
	}
	if (rest.endsWith(',')) {
		rest = rest.left(rest.size() - 1).trimmed();
	}
	if (rest.isEmpty()) {
		return std::nullopt;
	}
	auto ok = false;
	const auto id = rest.toLongLong(&ok);
	return ok ? std::make_optional(PeerIdValue(id)) : std::nullopt;
}

// Scans for the ']' that closes the '[' at `from`, skipping comments and
// counting nesting. Done by hand rather than read off toml++'s source region so
// the arithmetic here does not depend on whether that region's end is
// inclusive; the only thing between the brackets is integers and comments.
[[nodiscard]] std::optional<Position> FindClosing(
		const QStringList &lines,
		Position from) {
	auto depth = 0;
	for (auto line = from.line; line <= lines.size(); ++line) {
		const auto &text = lines[line - 1];
		const auto start = (line == from.line) ? (from.column - 1) : 0;
		for (auto i = start; i < text.size(); ++i) {
			const auto ch = text[i];
			if (ch == '#') {
				break;
			} else if (ch == '[') {
				++depth;
			} else if (ch == ']') {
				if (--depth == 0) {
					return Position{ line, i + 1 };
				}
			}
		}
	}
	return std::nullopt;
}

// Canonical form is one id per line with a trailing comma, the '[' alone at the
// end of its line and the ']' alone on its own. Only then can we edit single
// lines and leave the user's comments between them untouched; anything else
// gets rewritten wholesale, which is the largest blast radius we allow.
[[nodiscard]] bool IsCanonical(
		const QStringList &lines,
		Position open,
		Position close) {
	if (close.line <= open.line) {
		return false;
	}
	const auto afterOpen = lines[open.line - 1].mid(open.column);
	if (!BlankOrComment(afterOpen)) {
		return false;
	}
	const auto beforeClose = lines[close.line - 1].left(close.column - 1);
	if (!beforeClose.trimmed().isEmpty()) {
		return false;
	}
	for (auto line = open.line + 1; line < close.line; ++line) {
		const auto &text = lines[line - 1];
		// Commas need no checking: we only get here from a document that
		// already parsed, so they are wherever TOML requires them to be.
		if (!BlankOrComment(text) && !MemberOnLine(text)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] QString MemberLine(
		const QString &indent,
		PeerIdValue id,
		const MemberTitle &title) {
	return indent + QString::number(id) + u","_q + CommentText(title, id);
}

// A whole canonical array, written out: the '[' left on whatever came before
// it, one id per line, the ']' alone at the key's own indentation. `head' is
// the text preceding the bracket and `tail' whatever followed the closing one,
// so the caller decides how much of the surrounding line it is replacing.
[[nodiscard]] QStringList IdArrayLines(
		const QString &keyIndent,
		const QString &head,
		const QString &tail,
		const std::vector<PeerIdValue> &ids,
		const MemberTitle &title,
		const QString &ending) {
	const auto indent = keyIndent + QString::fromLatin1(kDefaultIndent);
	auto result = QStringList{ head + u"["_q + ending };
	for (const auto id : ids) {
		result.push_back(MemberLine(indent, id, title) + ending);
	}
	result.push_back(keyIndent + u"]"_q + tail);
	return result;
}

[[nodiscard]] const toml::table *FindListTable(
		const toml::table &root,
		const QString &list,
		QString &error) {
	const auto lists = root.get("lists");
	if (!lists || !lists->as_table()) {
		error = u"settings.toml has no [lists] tables."_q;
		return nullptr;
	}
	const auto utf8 = list.toUtf8();
	const auto node = lists->as_table()->get(
		std::string_view(utf8.constData(), utf8.size()));
	if (!node || !node->as_table()) {
		error = u"settings.toml has no [lists.%1] table."_q.arg(list);
		return nullptr;
	} else if (node->as_table()->is_inline()) {
		error = u"[lists.%1] is written inline; rewrite it as a table "
			"before editing its members from the app."_q.arg(list);
		return nullptr;
	}
	return node->as_table();
}

// The [[presets.<preset>.views]] block that calls itself <view>. Case-folded,
// matching the parser's own rule for deciding two views are the same one.
[[nodiscard]] const toml::table *FindPresetTable(
		const toml::table &root,
		const QString &preset,
		QString &error) {
	const auto presets = root.get("presets");
	if (!presets || !presets->as_table()) {
		error = u"settings.toml has no [presets] tables."_q;
		return nullptr;
	}
	const auto utf8 = preset.toUtf8();
	const auto node = presets->as_table()->get(
		std::string_view(utf8.constData(), utf8.size()));
	if (!node || !node->as_table()) {
		error = u"settings.toml has no [presets.%1] table."_q.arg(preset);
		return nullptr;
	} else if (node->as_table()->is_inline()) {
		error = u"[presets.%1] is written inline; rewrite it as a table "
			"before pinning inside it."_q.arg(preset);
		return nullptr;
	}
	return node->as_table();
}

[[nodiscard]] const toml::table *FindViewTable(
		const toml::table &root,
		const QString &preset,
		const QString &view,
		QString &error) {
	const auto found = FindPresetTable(root, preset, error);
	if (!found) {
		return nullptr;
	}
	const auto views = found->get("views");
	const auto array = views ? views->as_array() : nullptr;
	if (!array) {
		error = u"[presets.%1] has no views."_q.arg(preset);
		return nullptr;
	}
	for (auto &&element : *array) {
		const auto fields = element.as_table();
		if (!fields) {
			continue;
		}
		const auto name = fields->get_as<std::string>("name");
		if (!name || Text(name->get()).compare(view, Qt::CaseInsensitive)) {
			continue;
		} else if (fields->is_inline()) {
			error = u"the view '%1' of [presets.%2] is written inline; rewrite "
				"it as a [[presets.%2.views]] table before pinning inside "
				"it."_q.arg(view, preset);
			return nullptr;
		}
		return fields;
	}
	error = u"[presets.%1] has no view called '%2'."_q.arg(preset, view);
	return nullptr;
}

[[nodiscard]] std::vector<PeerIdValue> IdsOf(const toml::array &array) {
	auto result = std::vector<PeerIdValue>();
	for (auto &&element : array) {
		if (const auto id = element.value<int64>()) {
			result.push_back(*id);
		}
	}
	return result;
}

// Re-reads what we just wrote and refuses to hand it back unless it parses and
// the list holds exactly the ids we intended. Should be unreachable; it exists
// because the alternative failure mode is a corrupted file the user has to
// repair by hand.
[[nodiscard]] QString VerifySplice(
		const QString &text,
		const QString &path,
		const QString &list,
		const std::vector<PeerIdValue> &expected) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return u"the edit would not parse back (%1:%2: %3)"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description()));
	}
	auto ignored = QString();
	const auto table = FindListTable(parsed.table(), list, ignored);
	if (!table) {
		return u"the edit lost [lists.%1]"_q.arg(list);
	}
	const auto members = table->get("members");
	const auto array = members ? members->as_array() : nullptr;
	const auto ids = array ? IdsOf(*array) : std::vector<PeerIdValue>();
	if (ids != expected) {
		return u"the edit left [lists.%1] holding the wrong members"_q
			.arg(list);
	}
	return QString();
}

// The same refusal as VerifySplice, for the other array we own.
[[nodiscard]] QString VerifyViewPinned(
		const QString &text,
		const QString &path,
		const QString &preset,
		const QString &view,
		const std::vector<PeerIdValue> &expected) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return u"the edit would not parse back (%1:%2: %3)"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description()));
	}
	auto ignored = QString();
	const auto table = FindViewTable(parsed.table(), preset, view, ignored);
	if (!table) {
		return u"the edit lost the view '%1'"_q.arg(view);
	}
	const auto pinned = table->get("pinned");
	const auto array = pinned ? pinned->as_array() : nullptr;
	const auto ids = array ? IdsOf(*array) : std::vector<PeerIdValue>();
	if (ids != expected) {
		return u"the edit left the view '%1' pinning the wrong chats"_q
			.arg(view);
	}
	return QString();
}

[[nodiscard]] QString VerifyPresetPinned(
		const QString &text,
		const QString &path,
		const QString &preset,
		const std::vector<PeerIdValue> &expected) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return u"the edit would not parse back (%1:%2: %3)"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description()));
	}
	auto ignored = QString();
	const auto table = FindPresetTable(parsed.table(), preset, ignored);
	if (!table) {
		return u"the edit lost [presets.%1]"_q.arg(preset);
	}
	const auto pinned = table->get("pinned");
	const auto array = pinned ? pinned->as_array() : nullptr;
	const auto ids = array ? IdsOf(*array) : std::vector<PeerIdValue>();
	if (ids != expected) {
		return u"the edit left [presets.%1] pinning the wrong chats"_q
			.arg(preset);
	}
	return QString();
}

// A bare TOML key where the name allows one, a quoted key otherwise. The parser
// accepts either; a bare key is what a person would have typed.
[[nodiscard]] QString TableKey(const QString &name) {
	auto bare = !name.isEmpty();
	for (const auto ch : name) {
		if (!ch.isLetterOrNumber() || ch.unicode() > 127) {
			if (ch != '_' && ch != '-') {
				bare = false;
				break;
			}
		}
	}
	if (bare) {
		return name;
	}
	auto escaped = QString();
	escaped.reserve(name.size() + 2);
	for (const auto ch : name) {
		if (ch == '"' || ch == '\\') {
			escaped += '\\';
		}
		escaped += ch;
	}
	return '"' + escaped + '"';
}

// The text a basic string will actually carry: whitespace collapsed, control
// characters dropped. Kept apart from the quoting because a caller that wants
// to know whether the file already says this has to compare against what will
// land, not against what it asked for - otherwise a value the quoting tidies
// would read as changed on every single write.
[[nodiscard]] QString BasicStringText(const QString &value) {
	auto result = QString();
	result.reserve(value.size());
	for (const auto ch : value.simplified()) {
		if (ch.unicode() >= 0x20) {
			result += ch;
		}
	}
	return result;
}

// A TOML basic string. Control characters are dropped rather than escaped: a
// list title arrives from a text field, and a stray newline in one is a
// mis-paste rather than something worth preserving.
[[nodiscard]] QString QuotedValue(const QString &value) {
	auto result = QString('"');
	for (const auto ch : BasicStringText(value)) {
		if (ch == '"' || ch == '\\') {
			result += '\\';
		}
		result += ch;
	}
	return result + '"';
}

// Where a new [lists.x] table should go: just after the last one already
// there, so the lists stay together and a reader finds the new one where they
// were already looking. Returns a line index to insert before, or the line
// count for "at the end of the file".
[[nodiscard]] int AfterLastList(
		const toml::table &root,
		const QStringList &lines) {
	const auto lists = root.get("lists");
	const auto table = lists ? lists->as_table() : nullptr;
	if (!table || table->empty()) {
		return lines.size();
	}
	auto last = 0;
	for (auto &&[key, value] : *table) {
		if (const auto inner = value.as_table()) {
			last = std::max(last, int(inner->source().begin.line));
		}
	}
	if (last < 1 || last > lines.size()) {
		return lines.size();
	}
	// Scan to the next table header rather than trusting the region end -
	// nothing here does, which is why FindClosing() exists. Blank lines before
	// that header belong to whatever follows, so back over them.
	auto i = last;
	while (i < lines.size() && !lines[i].trimmed().startsWith('[')) {
		++i;
	}
	while (i > last && lines[i - 1].trimmed().isEmpty()) {
		--i;
	}
	return i;
}

[[nodiscard]] SpliceResult Refuse(const QString &text, const QString &error) {
	auto result = SpliceResult();
	result.text = text;
	result.error = error;
	return result;
}

[[nodiscard]] SpliceResult Unchanged(const QString &text) {
	auto result = SpliceResult();
	result.text = text;
	return result;
}

// The [schedule] table. Inline is refused by name and by line: editing lines
// inside `schedule = { ... }' would mean re-serialising the table, which is the
// one thing this file exists not to do.
[[nodiscard]] const toml::table *FindScheduleTable(
		const toml::table &root,
		QString &error) {
	const auto node = root.get("schedule");
	if (!node) {
		error = u"settings.toml has no [schedule] table."_q;
		return nullptr;
	}
	const auto table = node->as_table();
	if (!table) {
		error = u"'schedule' is not a table (line %1)."_q
			.arg(int(node->source().begin.line));
		return nullptr;
	} else if (table->is_inline()) {
		error = u"[schedule] is written inline (line %1); rewrite it as a table "
			"before editing the schedule from the app."_q
			.arg(int(table->source().begin.line));
		return nullptr;
	}
	return table;
}

[[nodiscard]] const toml::array *FindScheduleRules(
		const toml::table &root,
		QString &error) {
	const auto table = FindScheduleTable(root, error);
	if (!table) {
		return nullptr;
	}
	const auto rules = table->get("rules");
	if (!rules) {
		error = u"settings.toml has no [[schedule.rules]] blocks."_q;
		return nullptr;
	}
	const auto array = rules->as_array();
	if (!array) {
		error = u"'schedule.rules' is not an array (line %1)."_q
			.arg(int(rules->source().begin.line));
		return nullptr;
	}
	return array;
}

[[nodiscard]] const toml::array *FindRulesets(
		const toml::table &root,
		QString &error) {
	const auto table = FindScheduleTable(root, error);
	if (!table) {
		return nullptr;
	}
	const auto node = table->get("rulesets");
	if (!node) {
		error = u"settings.toml has no [[schedule.rulesets]] blocks."_q;
		return nullptr;
	}
	const auto array = node->as_array();
	if (!array) {
		error = u"'schedule.rulesets' is not an array (line %1)."_q
			.arg(int(node->source().begin.line));
		return nullptr;
	}
	return array;
}

// A ruleset and where it sits in the raw array. Found by name because that is
// how a screen addresses one: a ruleset's position moves whenever another is
// added above it, and an index a dialog read a minute ago would then edit the
// wrong ruleset. The raw position comes back anyway, because the fingerprint
// the edit is checked against is indexed by it.
struct FoundRuleset {
	const toml::table *fields = nullptr;
	int index = -1;
};

[[nodiscard]] FoundRuleset FindRulesetNamed(
		const toml::table &root,
		const QString &name,
		QString &error) {
	const auto array = FindRulesets(root, error);
	if (!array) {
		return FoundRuleset();
	}
	const auto wanted = name.trimmed();
	auto index = 0;
	for (auto &&element : *array) {
		const auto raw = index++;
		const auto fields = element.as_table();
		if (!fields) {
			continue;
		}
		const auto node = fields->get("name");
		if (!node) {
			continue;
		}
		const auto text = node->value<std::string_view>();
		if (!text
			|| Text(*text).trimmed().compare(wanted, Qt::CaseInsensitive)) {
			continue;
		} else if (fields->is_inline()) {
			error = u"schedule ruleset '%1' is written inline (line %2); "
				"rewrite it as a [[schedule.rulesets]] table before editing it "
				"from the app."_q.arg(wanted)
					.arg(int(fields->source().begin.line));
			return FoundRuleset();
		}

		// The first one wins, which is the one the parser kept: a name used
		// twice makes the second ruleset a warning and nothing else.
		return FoundRuleset{ fields, raw };
	}
	error = u"settings.toml has no schedule ruleset called '%1'."_q.arg(wanted);
	return FoundRuleset();
}

[[nodiscard]] bool RulesetNameTaken(
		const toml::table &root,
		const QString &name) {
	auto ignored = QString();
	const auto array = FindRulesets(root, ignored);
	if (!array) {
		return false;
	}
	const auto wanted = name.trimmed();
	for (auto &&element : *array) {
		const auto fields = element.as_table();
		if (!fields) {
			continue;
		}
		const auto node = fields->get("name");
		if (!node) {
			continue;
		}
		const auto text = node->value<std::string_view>();
		if (text && !Text(*text).trimmed().compare(wanted, Qt::CaseInsensitive)) {
			return true;
		}
	}
	return false;
}

// How a rule is named in a message, which is also how the parser names it in a
// warning, so the two can be read side by side.
[[nodiscard]] QString RuleWhere(const QString &ruleset, int index) {
	const auto trimmed = ruleset.trimmed();
	return trimmed.isEmpty()
		? u"schedule rule %1"_q.arg(index + 1)
		: u"schedule ruleset '%1' rule %2"_q.arg(trimmed).arg(index + 1);
}

// One rule by its raw position within whichever array holds it. The array is
// walked as it is rather than filtered: an element the parser threw away is
// still an element, and a rule keeping its address while the one above it is
// broken is the whole point of addressing rules this way.
[[nodiscard]] const toml::table *ScheduleRuleAt(
		const toml::array &rules,
		const QString &ruleset,
		int index,
		QString &error) {
	const auto where = RuleWhere(ruleset, index);
	const auto header = ruleset.trimmed().isEmpty()
		? u"[[schedule.rules]]"_q
		: u"[[schedule.rulesets.rules]]"_q;
	if (index < 0 || index >= int(rules.size())) {
		error = u"there is no %1 any more."_q.arg(where);
		return nullptr;
	}
	const auto element = rules.get(index);
	const auto fields = element ? element->as_table() : nullptr;
	if (!fields) {
		error = u"%1 is not a table (line %2)."_q
			.arg(where)
			.arg(element ? int(element->source().begin.line) : 0);
		return nullptr;
	} else if (fields->is_inline()) {
		error = u"%1 is written inline (line %2); rewrite it as a %3 table "
			"before editing it from the app."_q
			.arg(where)
			.arg(int(fields->source().begin.line))
			.arg(header);
		return nullptr;
	}
	return fields;
}

// The array a rule address points into: the flat [[schedule.rules]] for an
// empty ruleset name, otherwise the named ruleset's own rules.
[[nodiscard]] const toml::array *FindRuleArray(
		const toml::table &root,
		const QString &ruleset,
		int &rulesetIndex,
		QString &error) {
	if (ruleset.trimmed().isEmpty()) {
		rulesetIndex = -1;
		return FindScheduleRules(root, error);
	}
	const auto found = FindRulesetNamed(root, ruleset, error);
	if (!found.fields) {
		return nullptr;
	}
	rulesetIndex = found.index;
	const auto node = found.fields->get("rules");
	if (!node) {
		error = u"schedule ruleset '%1' has no rules yet."_q
			.arg(ruleset.trimmed());
		return nullptr;
	}
	const auto array = node->as_array();
	if (!array) {
		error = u"'rules' in schedule ruleset '%1' is not an array (line %2)."_q
			.arg(ruleset.trimmed())
			.arg(int(node->source().begin.line));
		return nullptr;
	}
	return array;
}

// Everything the app owns in one rule, as one string, so an edit can be checked
// against what it meant to write. Read straight off the file rather than
// through the parser: a rule the parser drops still has to compare equal to
// itself, or removing rule three could not prove it left rules one and two
// alone.
[[nodiscard]] QString RuleSignature(const toml::node &element) {
	const auto fields = element.as_table();
	if (!fields) {
		return u"?"_q;
	}
	const auto value = [&](std::string_view key) -> QString {
		const auto node = fields->get(key);
		if (!node) {
			return u"-"_q;
		} else if (const auto text = node->value<std::string_view>()) {
			return Text(*text);
		} else if (const auto flag = node->value<bool>()) {
			return *flag ? u"true"_q : u"false"_q;
		} else if (const auto array = node->as_array()) {
			auto parts = QStringList();
			for (auto &&item : *array) {
				const auto text = item.value<std::string_view>();
				parts.push_back(text ? Text(*text) : u"?"_q);
			}
			return parts.join(u","_q);
		}
		return u"?"_q;
	};
	return QStringList{
		value("enabled_p"),
		value("days"),
		value("from"),
		value("to"),
		value("preset"),
	}.join(u"|"_q);
}

[[nodiscard]] QStringList RuleSignatures(const toml::array &rules) {
	auto result = QStringList();
	for (auto &&element : rules) {
		result.push_back(RuleSignature(element));
	}
	return result;
}

// A ruleset's own keys, the same way and for the same reason. `override' lets a
// caller ask what the signature WOULD be with one key set to something else, or
// to nothing at all, which is how an edit says what it meant to do without
// having to spell the whole thing out again.
[[nodiscard]] QString RulesetSignature(
		const toml::table &fields,
		const QString &override = QString(),
		const std::optional<QString> &value = std::nullopt) {
	const auto read = [&](const char *key) -> QString {
		if (!override.isEmpty() && override == QLatin1String(key)) {
			return value ? *value : u"-"_q;
		}
		const auto node = fields.get(std::string_view(key));
		if (!node) {
			return u"-"_q;
		}
		const auto text = node->value<std::string_view>();
		return text ? Text(*text) : u"?"_q;
	};
	return QStringList{
		read("name"),
		read("device"),
		read("mode"),
		read("outside"),
	}.join(u"|"_q);
}

// Everything under [schedule] that an edit could disturb: the flat rules, and
// every ruleset with its own keys and its own rules. An op builds this from the
// file, applies to it exactly the change it means to make, and refuses unless
// the file it wrote reads back as that - so a splice that lands in the wrong
// block is caught by the block it should not have touched, not only by the one
// it should have.
struct ScheduleFingerprint {
	QStringList flat;
	std::vector<std::pair<QString, QStringList>> rulesets;

	friend bool operator==(
		const ScheduleFingerprint &,
		const ScheduleFingerprint &) = default;
};

[[nodiscard]] ScheduleFingerprint Fingerprint(const toml::table &root) {
	auto result = ScheduleFingerprint();
	const auto node = root.get("schedule");
	const auto schedule = node ? node->as_table() : nullptr;
	if (!schedule) {
		return result;
	}
	if (const auto rules = schedule->get("rules")) {
		if (const auto array = rules->as_array()) {
			result.flat = RuleSignatures(*array);
		}
	}
	const auto rulesets = schedule->get("rulesets");
	const auto array = rulesets ? rulesets->as_array() : nullptr;
	if (!array) {
		return result;
	}
	for (auto &&element : *array) {
		const auto fields = element.as_table();
		if (!fields) {
			result.rulesets.push_back({ u"?"_q, QStringList() });
			continue;
		}
		auto rules = QStringList();
		if (const auto inner = fields->get("rules")) {
			if (const auto list = inner->as_array()) {
				rules = RuleSignatures(*list);
			}
		}
		result.rulesets.push_back({ RulesetSignature(*fields), rules });
	}
	return result;
}

// The same signature for a rule we are about to write, spelt the way we write
// it - which is why the check below can be an equality rather than a parse.
[[nodiscard]] QString WrittenSignature(const ScheduleRule &rule) {
	auto days = QStringList();
	for (const auto day : rule.days) {
		days.push_back(WeekdayName(day));
	}
	return QStringList{
		rule.enabled ? u"true"_q : u"false"_q,
		days.join(u","_q),
		TimeOfDayText(rule.from),
		TimeOfDayText(rule.till),
		rule.preset,
	}.join(u"|"_q);
}

// Whether the rule in the file is still the one the screen read. Times are
// compared as minutes, so re-typing "9:00" as "09:00" by hand is not treated as
// somebody else's edit.
[[nodiscard]] bool RuleMatches(
		const toml::table &fields,
		const ScheduleRuleExpected &expected) {
	const auto text = [&](std::string_view key) -> QString {
		const auto node = fields.get(key);
		if (!node) {
			return QString();
		}
		const auto value = node->value<std::string_view>();
		return value ? Text(*value) : QString();
	};
	return ParseTimeOfDay(text("from")).value_or(-1) == expected.from
		&& ParseTimeOfDay(text("to")).value_or(-1) == expected.till
		&& text("preset") == expected.preset;
}

// The signature of a ruleset header the app is about to write, spelt the way it
// writes it. Unset keys are the ones it leaves out of the file.
[[nodiscard]] QString WrittenRulesetSignature(
		const QString &name,
		const std::optional<QString> &device,
		const std::optional<QString> &mode,
		const std::optional<QString> &outside) {
	const auto part = [](const std::optional<QString> &value) {
		return value ? BasicStringText(*value) : u"-"_q;
	};
	return QStringList{
		BasicStringText(name),
		part(device),
		part(mode),
		part(outside),
	}.join(u"|"_q);
}

// Where the value starting at `from' ends: the line it ends on, and the index
// one past its last character there. Worked out by hand rather than read off
// toml++'s source region, for the same reason FindClosing() is - nothing here
// depends on whether that region's end is inclusive.
struct ValueEnd {
	int line = 0; // 1-based.
	int index = 0; // 0-based, one past the end.
};

[[nodiscard]] std::optional<ValueEnd> FindValueEnd(
		const QStringList &lines,
		Position from) {
	if (from.line < 1 || from.line > lines.size()) {
		return std::nullopt;
	}
	const auto &line = lines[from.line - 1];
	const auto start = from.column - 1;
	if (start < 0 || start >= line.size()) {
		return std::nullopt;
	}
	const auto first = line[start];
	if (first == '[') {
		const auto close = FindClosing(lines, from);
		return close
			? std::make_optional(ValueEnd{ close->line, close->column })
			: std::nullopt;
	} else if (first == '"' || first == '\'') {
		if (line.mid(start, 3) == QString(3, first)) {
			// A multi-line string. Nothing the app writes is one, and finding
			// where it ends means a second copy of the parser's own scanner, so
			// this leaves it to the person who typed it.
			return std::nullopt;
		}
		const auto escapes = (first == '"');
		for (auto i = start + 1; i < line.size(); ++i) {
			if (escapes && line[i] == '\\') {
				++i;
			} else if (line[i] == first) {
				return ValueEnd{ from.line, int(i) + 1 };
			}
		}
		return std::nullopt;
	}
	auto i = start;
	while (i < line.size()
		&& !line[i].isSpace() // A carriage return among them, on a CRLF file.
		&& line[i] != '#'
		&& line[i] != ','
		&& line[i] != ']'
		&& line[i] != '}') {
		++i;
	}
	return (i > start)
		? std::make_optional(ValueEnd{ from.line, int(i) })
		: std::nullopt;
}

// Replaces the value that starts at `at', keeping whatever the line holds
// before it and after it - the key, the spacing, a trailing comment. A value
// spread over several lines comes back as one, because the span is all we are
// allowed to touch and all of it is ours.
[[nodiscard]] bool ReplaceValue(
		QStringList &lines,
		Position at,
		const QString &value) {
	const auto end = FindValueEnd(lines, at);
	if (!end) {
		return false;
	}
	const auto head = lines[at.line - 1].left(at.column - 1);
	const auto tail = lines[end->line - 1].mid(end->index);
	auto rebuilt = lines.mid(0, at.line - 1);
	rebuilt.push_back(head + value + tail);
	rebuilt += lines.mid(end->line);
	lines = std::move(rebuilt);
	return true;
}

// Takes out the whole line a key sits on, comment and all. A key and its value
// have a line to themselves in a table that is not inline, and a comment beside
// one is about the key that is going - so the line goes with it.
[[nodiscard]] bool RemoveValueLines(QStringList &lines, Position at) {
	const auto end = FindValueEnd(lines, at);
	if (!end || at.line < 1 || end->line > lines.size()) {
		return false;
	}
	auto rebuilt = lines.mid(0, at.line - 1);
	rebuilt += lines.mid(end->line);
	lines = std::move(rebuilt);
	return true;
}

// The last line a block occupies: the line its own last value ends on. Asking
// the values rather than scanning for the next header, because a `days' array
// written one weekday per line has lines of its own that a scanner would have
// to know to skip.
[[nodiscard]] int BlockLastLine(
		const toml::table &fields,
		const QStringList &lines) {
	auto last = int(fields.source().begin.line);
	for (auto &&[key, value] : fields) {
		const auto at = Position{
			int(value.source().begin.line),
			int(value.source().begin.column),
		};
		const auto end = FindValueEnd(lines, at);
		last = std::max(last, end ? end->line : at.line);
	}
	return last;
}

// The same, counting only the keys the block writes on its own lines. An
// array-of-tables key - `rules' inside a ruleset, `rules' and `rulesets' under
// [schedule] - is not one of them: its blocks are tables of their own further
// down the file, and counting them would file a key added to the ruleset inside
// the ruleset's first rule.
[[nodiscard]] int ScalarBlockLastLine(
		const toml::table &fields,
		const QStringList &lines) {
	auto last = int(fields.source().begin.line);
	for (auto &&[key, value] : fields) {
		const auto array = value.as_array();
		if (array && array->is_array_of_tables()) {
			continue;
		}
		const auto at = Position{
			int(value.source().begin.line),
			int(value.source().begin.column),
		};
		const auto end = FindValueEnd(lines, at);
		last = std::max(last, end ? end->line : at.line);
	}
	return last;
}

// The last line anything under [schedule] occupies: the section's own keys,
// every flat rule block, every ruleset and every rule inside one. Where a new
// ruleset goes, so the file keeps its schedule in one piece instead of growing
// a second one at the bottom.
[[nodiscard]] int ScheduleLastLine(
		const toml::table &schedule,
		const QStringList &lines) {
	auto last = ScalarBlockLastLine(schedule, lines);
	const auto blocks = [&](const toml::table &table, const char *key) {
		const auto node = table.get(std::string_view(key));
		const auto array = node ? node->as_array() : nullptr;
		return array ? array : nullptr;
	};
	const auto consider = [&](const toml::node &element) {
		const auto fields = element.as_table();
		if (fields && !fields->is_inline()) {
			last = std::max(last, ScalarBlockLastLine(*fields, lines));
		}
	};
	if (const auto rules = blocks(schedule, "rules")) {
		for (auto &&element : *rules) {
			consider(element);
		}
	}
	if (const auto rulesets = blocks(schedule, "rulesets")) {
		for (auto &&element : *rulesets) {
			consider(element);
			const auto fields = element.as_table();
			if (const auto inner = fields ? blocks(*fields, "rules") : nullptr) {
				for (auto &&rule : *inner) {
					consider(rule);
				}
			}
		}
	}
	return last;
}

// Where a line added to a block goes, and where a block being taken out stops:
// the next table header, backed over the blank lines and the comment block
// above it, which belong to whatever follows rather than to what is here.
// Returns a line index to insert before, or the line count for end of file.
[[nodiscard]] int AfterBlock(const QStringList &lines, int lastLine) {
	auto i = lastLine;
	while (i < lines.size() && !lines[i].trimmed().startsWith('[')) {
		++i;
	}
	while (i > lastLine
		&& (lines[i - 1].trimmed().isEmpty()
			|| lines[i - 1].trimmed().startsWith('#'))) {
		--i;
	}
	return i;
}

// The blank line above a block that has gone stays, and so does the one below
// it that belongs to whatever follows - which would leave two of them where
// there was one. Closes that seam.
void CloseSeam(QStringList &lines, int seam) {
	if (seam >= 1
		&& seam < lines.size()
		&& lines[seam - 1].trimmed().isEmpty()
		&& lines[seam].trimmed().isEmpty()) {
		lines.removeAt(seam);
	}
}

// Puts a block into the file at `at' - a line index to insert before, or the
// line count for the end of it - with the blank lines around it that make it
// read as a block rather than as more of whatever is above. `block' arrives
// with its own leading blank line, which is dropped when there is nothing above
// to be separated from.
void InsertBlock(QStringList &lines, int at, QStringList block) {
	if (at >= lines.size()) {
		// At the end of the file, where there may be no trailing newline.
		if (!lines.isEmpty() && lines.back().trimmed().isEmpty()) {
			lines.removeLast();
		}
		if (lines.isEmpty()) {
			lines += block.mid(1);
		} else {
			lines += block;
		}
		lines.push_back(QString());
		return;
	}
	if (!lines[at].trimmed().isEmpty()) {
		// A blank line below too, unless the file already has one there.
		block.push_back(QString());
	}
	for (auto i = block.size(); i != 0;) {
		lines.insert(at, block[--i]);
	}
}

[[nodiscard]] QString DaysValue(const std::vector<int> &days) {
	auto parts = QStringList();
	for (const auto day : days) {
		parts.push_back(QuotedValue(WeekdayName(day)));
	}
	return u"["_q + parts.join(u", "_q) + u"]"_q;
}

// The value every key the app owns takes, in the order a hand-written rule puts
// them in. One place, so a rule the app creates and a rule the app edits end up
// saying the same thing the same way.
[[nodiscard]] std::vector<std::pair<QString, QString>> RuleValues(
		const ScheduleRule &rule) {
	return {
		{ u"enabled_p"_q, rule.enabled ? u"true"_q : u"false"_q },
		{ u"days"_q, DaysValue(rule.days) },
		{ u"from"_q, QuotedValue(TimeOfDayText(rule.from)) },
		{ u"to"_q, QuotedValue(TimeOfDayText(rule.till)) },
		{ u"preset"_q, QuotedValue(rule.preset) },
	};
}

// What the parser would throw away on the next read. A rule written from a
// screen that got one of these wrong would look saved and then not be there.
// The days are not checked for being empty: that is a warning for a rule typed
// by hand too, and it means "every day" rather than "nothing".
[[nodiscard]] QString RuleProblem(const ScheduleRule &rule) {
	if (rule.preset.trimmed().isEmpty()) {
		return u"a schedule rule needs a preset."_q;
	} else if (TimeOfDayText(rule.from).isEmpty()
		|| TimeOfDayText(rule.till).isEmpty()) {
		return u"a schedule rule needs a 'from' and a 'to' time of day."_q;
	} else if (rule.from == rule.till) {
		return u"a schedule rule starting and ending at the same time covers "
			"nothing."_q;
	}
	for (const auto day : rule.days) {
		if (WeekdayName(day).isEmpty()) {
			return u"'%1' is not a weekday."_q.arg(day);
		}
	}
	return QString();
}

// Re-reads what we wrote and refuses unless every rule is what it should be:
// the one we touched saying exactly what we asked for, and every other rule
// still where it was, saying what it said. Signatures rather than text, so the
// check does not depend on the spacing we happened to write.
[[nodiscard]] QString VerifySchedule(
		const QString &text,
		const QString &path,
		const ScheduleFingerprint &expected) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return u"the edit would not parse back (%1:%2: %3)"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description()));
	}
	if (!(Fingerprint(parsed.table()) == expected)) {
		return u"the edit left the schedule holding the wrong rules"_q;
	}
	return QString();
}

// Whether this line is the header that declares [table], as opposed to the line
// toml++ points at for a table it never saw a header for.
[[nodiscard]] bool DeclaresTable(const QString &line, const QString &table) {
	const auto trimmed = line.trimmed();
	if (!trimmed.startsWith('[') || trimmed.startsWith(u"[["_q)) {
		return false;
	}
	const auto close = trimmed.indexOf(']');
	if (close < 0) {
		return false;
	}
	const auto inner = trimmed.mid(1, close - 1).trimmed();
	return (inner == table) || (inner == TableKey(table));
}

// A duration the way a person writes one, and the counterpart of
// ParseDuration(): the largest unit that divides the seconds exactly, because
// "2h" is what somebody typing a two-hour budget writes and "7200s" is what a
// machine would leave in their file for them to read.
[[nodiscard]] QString DurationText(int seconds) {
	if (seconds <= 0) {
		return u"0"_q;
	} else if (!(seconds % 3600)) {
		return u"%1h"_q.arg(seconds / 3600);
	} else if (!(seconds % 60)) {
		return u"%1m"_q.arg(seconds / 60);
	}
	return u"%1s"_q.arg(seconds);
}

// How a budget is named in a message, which is also how the parser names it in
// a warning, so the two can be read side by side.
[[nodiscard]] QString BudgetWhere(int index) {
	return u"screen_time budget %1"_q.arg(index + 1);
}

// The [screen_time] table. Inline is refused by name and by line, for the same
// reason [schedule] is: editing lines inside `screen_time = { ... }' would mean
// re-serialising the table.
[[nodiscard]] const toml::table *FindScreenTimeTable(
		const toml::table &root,
		QString &error) {
	const auto node = root.get("screen_time");
	if (!node) {
		error = u"settings.toml has no [screen_time] table."_q;
		return nullptr;
	}
	const auto table = node->as_table();
	if (!table) {
		error = u"'screen_time' is not a table (line %1)."_q
			.arg(int(node->source().begin.line));
		return nullptr;
	} else if (table->is_inline()) {
		error = u"[screen_time] is written inline (line %1); rewrite it as a "
			"table before editing the budgets from the app."_q
			.arg(int(table->source().begin.line));
		return nullptr;
	}
	return table;
}

[[nodiscard]] const toml::array *FindBudgets(
		const toml::table &root,
		QString &error) {
	const auto table = FindScreenTimeTable(root, error);
	if (!table) {
		return nullptr;
	}
	const auto node = table->get("budgets");
	if (!node) {
		error = u"settings.toml has no [[screen_time.budgets]] blocks."_q;
		return nullptr;
	}
	const auto array = node->as_array();
	if (!array) {
		error = u"'screen_time.budgets' is not an array (line %1)."_q
			.arg(int(node->source().begin.line));
		return nullptr;
	}
	return array;
}

// One budget by its raw position in the array. Walked as it is rather than
// filtered: an element the parser threw away is still an element, and a budget
// keeping its address while the one above it is broken is the whole point of
// addressing budgets this way.
[[nodiscard]] const toml::table *BudgetAt(
		const toml::array &budgets,
		int index,
		QString &error) {
	const auto where = BudgetWhere(index);
	if (index < 0 || index >= int(budgets.size())) {
		error = u"there is no %1 any more."_q.arg(where);
		return nullptr;
	}
	const auto element = budgets.get(index);
	const auto fields = element ? element->as_table() : nullptr;
	if (!fields) {
		error = u"%1 is not a table (line %2)."_q
			.arg(where)
			.arg(element ? int(element->source().begin.line) : 0);
		return nullptr;
	} else if (fields->is_inline()) {
		error = u"%1 is written inline (line %2); rewrite it as a "
			"[[screen_time.budgets]] table before editing it from the app."_q
			.arg(where)
			.arg(int(fields->source().begin.line));
		return nullptr;
	}
	return fields;
}

// Everything the app owns in one budget, as one string, so an edit can be
// checked against what it meant to write. Read straight off the file rather
// than through the parser: a budget the parser drops still has to compare equal
// to itself, or removing budget three could not prove it left budgets one and
// two alone.
[[nodiscard]] QString BudgetSignature(const toml::node &element) {
	const auto fields = element.as_table();
	if (!fields) {
		return u"?"_q;
	}
	const auto value = [&](std::string_view key) -> QString {
		const auto node = fields->get(key);
		if (!node) {
			return u"-"_q;
		} else if (const auto text = node->value<std::string_view>()) {
			return Text(*text);
		} else if (const auto number = node->value<int64_t>()) {
			return QString::number(qlonglong(*number));
		} else if (const auto flag = node->value<bool>()) {
			return *flag ? u"true"_q : u"false"_q;
		}
		return u"?"_q;
	};
	return QStringList{
		value("target"),
		value("per_day"),
		value("mode"),
		value("snooze"),
		value("snoozes_per_day"),
	}.join(u"|"_q);
}

// Every budget under [screen_time], in file order. The whole of what an edit
// here could disturb: an op builds this from the file, applies to it exactly
// the change it means to make, and refuses unless the file it wrote reads back
// as that.
[[nodiscard]] QStringList BudgetFingerprint(const toml::table &root) {
	auto result = QStringList();
	const auto node = root.get("screen_time");
	const auto screen = node ? node->as_table() : nullptr;
	const auto budgets = screen ? screen->get("budgets") : nullptr;
	const auto array = budgets ? budgets->as_array() : nullptr;
	if (!array) {
		return result;
	}
	for (auto &&element : *array) {
		result.push_back(BudgetSignature(element));
	}
	return result;
}

// One key of a budget block, as the file will hold it.
struct BudgetValue {
	QString key;

	// The value as it will parse back - unquoted, and already tidied the way
	// the quoting tidies. Nothing at all when the key belongs out of the file:
	// the three keys that have defaults are written only when the budget means
	// something other than the default.
	std::optional<QString> text;

	bool quoted = false;

	// What the line says. QuotedValue() tidies, and `text' is already tidy, so
	// asking twice cannot change the answer.
	[[nodiscard]] QString written() const {
		return quoted ? QuotedValue(*text) : *text;
	}
};

// Every key the app owns in a budget, in the order a hand-written one puts them
// in. One place, so a budget the app creates, a budget the app edits and the
// signature an edit is checked against can never drift apart.
[[nodiscard]] std::vector<BudgetValue> BudgetValues(
		const ScreenTimeBudget &budget) {
	const auto defaults = ScreenTimeBudget();
	const auto string = [](const QString &key, QString value) {
		return BudgetValue{ key, BasicStringText(value), true };
	};
	const auto only = [](BudgetValue value, bool differs) {
		if (!differs) {
			value.text = std::nullopt;
		}
		return value;
	};
	return {
		string(u"target"_q, budget.target),
		string(u"per_day"_q, DurationText(budget.perDaySeconds)),
		only(
			string(u"mode"_q, BudgetModeName(budget.mode)),
			budget.mode != defaults.mode),
		only(
			string(u"snooze"_q, DurationText(budget.snoozeSeconds)),
			budget.snoozeSeconds != defaults.snoozeSeconds),
		only(
			BudgetValue{
				u"snoozes_per_day"_q,
				QString::number(budget.snoozesPerDay),
			},
			budget.snoozesPerDay != defaults.snoozesPerDay),
	};
}

// The same signature for a budget we are about to write, spelt the way we write
// it - which is why the check below can be an equality rather than a parse. A
// key we leave out reads back as missing, which is exactly what the default
// means in the file.
[[nodiscard]] QString WrittenBudgetSignature(const ScreenTimeBudget &budget) {
	auto parts = QStringList();
	for (const auto &value : BudgetValues(budget)) {
		parts.push_back(value.text ? *value.text : u"-"_q);
	}
	return parts.join(u"|"_q);
}

// Whether the budget in the file is still the one the screen read. The target
// is all there is to ask about: everything else in the block is what the screen
// is open to change.
[[nodiscard]] bool BudgetTargetMatches(
		const toml::table &fields,
		const QString &expected) {
	const auto node = fields.get("target");
	if (!node) {
		return expected.trimmed().isEmpty();
	}
	const auto value = node->value<std::string_view>();
	return value && (Text(*value).trimmed() == expected.trimmed());
}

// What can be told about a budget before writing it. The target's grammar is
// not checked here - that is the parser's, and a second copy of it would be a
// second thing to keep in step - so it is asked of the parser afterwards
// instead, by BudgetReadsBack().
[[nodiscard]] QString BudgetProblem(const ScreenTimeBudget &budget) {
	if (BasicStringText(budget.target).trimmed().isEmpty()) {
		return u"a screen time budget needs a target."_q;
	} else if (budget.perDaySeconds < 0
		|| budget.snoozeSeconds < 0
		|| budget.snoozesPerDay < 0) {
		return u"a screen time budget cannot ask for less than no time."_q;
	}
	return QString();
}

// Whether the budget we just wrote at `index' survives a real read of the file.
// A budget written from a screen that got its target wrong would look saved and
// then not be there, and the parser is the only thing that can say so without
// this file holding its own copy of what a target may look like.
[[nodiscard]] bool BudgetReadsBack(
		const QString &text,
		const QString &path,
		int index) {
	const auto parsed = ParseSettings(text, path);
	for (const auto &budget : parsed.settings.screenTime.budgets) {
		if (budget.sourceIndex == index) {
			return true;
		}
	}
	return false;
}

// Re-reads what we wrote and refuses unless every budget is what it should be:
// the one we touched saying exactly what we asked for, and every other budget
// still where it was, saying what it said. Signatures rather than text, so the
// check does not depend on the spacing we happened to write.
[[nodiscard]] QString VerifyBudgets(
		const QString &text,
		const QString &path,
		const QStringList &expected) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return u"the edit would not parse back (%1:%2: %3)"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description()));
	}
	if (BudgetFingerprint(parsed.table()) != expected) {
		return u"the edit left [screen_time] holding the wrong budgets"_q;
	}
	return QString();
}

// The lines half of writing a `pinned' array. Shared, because a preset's main
// view and an extra view differ only in which table was located, what it is
// called when something goes wrong, and whether there is a key to sit the fresh
// array under.
[[nodiscard]] SpliceResult SetPinnedIn(
		const QString &text,
		const toml::table &table,
		const QString &what,
		std::string_view anchor,
		const std::vector<PeerIdValue> &ids,
		const MemberTitle &title,
		const std::function<QString(const QString&)> &verify) {
	const auto pinned = table.get("pinned");
	const auto array = pinned ? pinned->as_array() : nullptr;
	if (pinned && !array) {
		return Refuse(text, u"%1 has a 'pinned' that is not an array."_q
			.arg(what));
	} else if ((array ? IdsOf(*array) : std::vector<PeerIdValue>()) == ids) {
		return Unchanged(text);
	}

	auto lines = text.split('\n');
	const auto crlf = std::any_of(lines.begin(), lines.end(), [](
			const QString &line) {
		return line.endsWith('\r');
	});
	const auto ending = crlf ? u"\r"_q : QString();

	if (!array) {
		// No key yet, which is the ordinary case: pins only exist once somebody
		// has dragged one. Put the array where a reader looking for what this
		// tab does will already be looking.
		const auto header = int(table.source().begin.line);
		if (header < 1 || header > lines.size()) {
			return Refuse(text, u"could not locate %1."_q.arg(what));
		}
		auto after = header;
		if (!anchor.empty()) {
			if (const auto name = table.get(anchor)) {
				const auto line = int(name->source().begin.line);
				if (line >= header && line <= lines.size()) {
					after = line;
				}
			}
		}
		const auto indent = Indentation(lines[header - 1]);
		const auto written = IdArrayLines(
			indent,
			indent + u"pinned = "_q,
			ending,
			ids,
			title,
			ending);
		for (auto i = written.size(); i != 0;) {
			lines.insert(after, written[--i]);
		}
	} else {
		const auto open = Position{
			int(array->source().begin.line),
			int(array->source().begin.column),
		};
		if (open.line < 1 || open.line > lines.size()) {
			return Refuse(text, u"could not locate the pins of %1."_q
				.arg(what));
		}
		const auto close = FindClosing(lines, open);
		if (!close) {
			return Refuse(text, u"the pinned array of %1 is not closed."_q
				.arg(what));
		}
		const auto keyIndent = Indentation(lines[open.line - 1]);
		const auto replacement = IdArrayLines(
			keyIndent,
			lines[open.line - 1].left(open.column - 1),
			lines[close->line - 1].mid(close->column),
			ids,
			title,
			ending);

		auto rebuilt = lines.mid(0, open.line - 1);
		rebuilt += replacement;
		rebuilt += lines.mid(close->line);
		lines = std::move(rebuilt);
	}

	auto result = SpliceResult();
	result.text = lines.join('\n');
	if (auto failed = verify(result.text); !failed.isEmpty()) {
		return Refuse(text, failed);
	}
	result.changed = true;
	return result;
}

[[nodiscard]] SpliceResult Splice(
		const QString &text,
		const QString &path,
		const QString &list,
		PeerIdValue id,
		const MemberTitle &title,
		bool add) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return Refuse(text, u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	}
	auto error = QString();
	const auto table = FindListTable(parsed.table(), list, error);
	if (!table) {
		return Refuse(text, error);
	}

	const auto members = table->get("members");
	const auto array = members ? members->as_array() : nullptr;
	if (members && !array) {
		return Refuse(text, u"[lists.%1] has a 'members' that is not an "
			"array."_q.arg(list));
	}
	const auto before = array ? IdsOf(*array) : std::vector<PeerIdValue>();
	const auto known = std::find(before.begin(), before.end(), id)
		!= before.end();
	if (add == known) {
		return Unchanged(text);
	}
	auto expected = before;
	if (add) {
		expected.push_back(id);
	} else {
		expected.erase(
			std::remove(expected.begin(), expected.end(), id),
			expected.end());
	}

	auto lines = text.split('\n');
	const auto crlf = std::any_of(lines.begin(), lines.end(), [](
			const QString &line) {
		return line.endsWith('\r');
	});
	const auto ending = crlf ? u"\r"_q : QString();

	if (!array) {
		// The key is gone - the user deleted it, or the list was written
		// without one. Put a canonical array right under the table header.
		const auto header = int(table->source().begin.line);
		if (header < 1 || header > lines.size()) {
			return Refuse(text, u"could not locate [lists.%1]."_q.arg(list));
		}
		const auto indent = Indentation(lines[header - 1]);
		const auto written = IdArrayLines(
			indent,
			indent + u"members = "_q,
			ending,
			expected,
			title,
			ending);
		for (auto i = written.size(); i != 0;) {
			lines.insert(header, written[--i]);
		}
	} else {
		const auto open = Position{
			int(array->source().begin.line),
			int(array->source().begin.column),
		};
		if (open.line < 1 || open.line > lines.size()) {
			return Refuse(text, u"could not locate the members of "
				"[lists.%1]."_q.arg(list));
		}
		const auto close = FindClosing(lines, open);
		if (!close) {
			return Refuse(text, u"the members array of [lists.%1] is not "
				"closed."_q.arg(list));
		}
		const auto keyIndent = Indentation(lines[open.line - 1]);
		if (IsCanonical(lines, open, *close)) {
			auto indent = keyIndent + QString::fromLatin1(kDefaultIndent);
			for (auto line = open.line + 1; line < close->line; ++line) {
				if (MemberOnLine(lines[line - 1])) {
					indent = Indentation(lines[line - 1]);
					break;
				}
			}
			if (add) {
				lines.insert(
					close->line - 1,
					MemberLine(indent, id, title) + ending);
			} else {
				for (auto line = close->line - 1; line > open.line; --line) {
					if (MemberOnLine(lines[line - 1]) == id) {
						lines.removeAt(line - 1);
					}
				}
			}
		} else {
			// Squashed onto one line, or otherwise not something we can edit a
			// line at a time. Rewrite exactly the bracketed span - that is the
			// whole permissible blast radius - and leave the rest alone.
			const auto replacement = IdArrayLines(
				keyIndent,
				lines[open.line - 1].left(open.column - 1),
				lines[close->line - 1].mid(close->column),
				expected,
				title,
				ending);

			auto rebuilt = lines.mid(0, open.line - 1);
			rebuilt += replacement;
			rebuilt += lines.mid(close->line);
			lines = std::move(rebuilt);
		}
	}

	auto result = SpliceResult();
	result.text = lines.join('\n');
	if (auto failed = VerifySplice(result.text, path, list, expected);
		!failed.isEmpty()) {
		return Refuse(text, failed);
	}
	result.changed = true;
	return result;
}

} // namespace

SpliceResult AddListMember(
		const QString &text,
		const QString &path,
		const QString &list,
		PeerIdValue id,
		const MemberTitle &title) {
	return Splice(text, path, list, id, title, true);
}

SpliceResult RemoveListMember(
		const QString &text,
		const QString &path,
		const QString &list,
		PeerIdValue id,
		const MemberTitle &title) {
	return Splice(text, path, list, id, title, false);
}

SpliceResult SetViewPinned(
		const QString &text,
		const QString &path,
		const QString &preset,
		const QString &view,
		const std::vector<PeerIdValue> &ids,
		const MemberTitle &title) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return Refuse(text, u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	}
	auto error = QString();
	const auto table = FindViewTable(parsed.table(), preset, view, error);
	if (!table) {
		return Refuse(text, error);
	}
	return SetPinnedIn(
		text,
		*table,
		u"the view '%1'"_q.arg(view),
		"name",
		ids,
		title,
		[&](const QString &edited) {
			return VerifyViewPinned(edited, path, preset, view, ids);
		});
}

SpliceResult SetPresetPinned(
		const QString &text,
		const QString &path,
		const QString &preset,
		const std::vector<PeerIdValue> &ids,
		const MemberTitle &title) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return Refuse(text, u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	}
	auto error = QString();
	const auto table = FindPresetTable(parsed.table(), preset, error);
	if (!table) {
		return Refuse(text, error);
	}
	// No anchor key: a preset has no `name' to sit under, so a fresh array goes
	// straight below the [presets.x] header - which is also where a reader
	// looking for what the preset does starts.
	return SetPinnedIn(
		text,
		*table,
		u"[presets.%1]"_q.arg(preset),
		std::string_view(),
		ids,
		title,
		[&](const QString &edited) {
			return VerifyPresetPinned(edited, path, preset, ids);
		});
}

SpliceResult AddList(
		const QString &text,
		const QString &path,
		const QString &name,
		const QString &title) {
	const auto trimmed = name.trimmed();
	if (trimmed.isEmpty()) {
		return Refuse(text, u"a list needs a name."_q);
	} else if (trimmed.startsWith('*')) {
		// The parser refuses these for the same reason: nobody reading the file
		// could tell such a list from a "*set" spread.
		return Refuse(text, u"a list name cannot start with '*'."_q);
	}
	const auto utf8text = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8text.constData(), utf8text.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return Refuse(text, u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	}
	auto ignored = QString();
	if (FindListTable(parsed.table(), trimmed, ignored)) {
		return Refuse(text, u"there is already a list called '%1'."_q
			.arg(trimmed));
	}

	auto lines = text.split('\n');
	const auto crlf = std::any_of(lines.begin(), lines.end(), [](
			const QString &line) {
		return line.endsWith('\r');
	});
	const auto ending = crlf ? u"\r"_q : QString();
	const auto at = AfterLastList(parsed.table(), lines);

	// An empty members array in canonical shape, so the very next tick from the
	// chat menu splices one line into it rather than rewriting the block.
	auto block = QStringList();
	block.push_back(ending);
	block.push_back(u"[lists.%1]"_q.arg(TableKey(trimmed)) + ending);
	if (!title.trimmed().isEmpty() && title.trimmed() != trimmed) {
		block.push_back(u"title = %1"_q.arg(QuotedValue(title)) + ending);
	}
	block.push_back(u"members = ["_q + ending);
	block.push_back(u"]"_q + ending);
	if (at >= lines.size()) {
		// At the end of the file, where there may be no trailing newline.
		if (!lines.isEmpty() && lines.back().trimmed().isEmpty()) {
			lines.removeLast();
		}
		lines += block;
		lines.push_back(QString());
	} else {
		block.push_back(QString());
		for (auto i = block.size(); i != 0;) {
			lines.insert(at, block[--i]);
		}
	}

	auto result = SpliceResult();
	result.text = lines.join('\n');

	// Verified like every other splice: re-parse and confirm the list is there
	// and empty. A name the app allowed but TOML did not would otherwise leave
	// the user a file that no longer loads.
	const auto utf8 = result.text.toUtf8();
	auto back = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!back) {
		const auto &error = back.error();
		return Refuse(text, u"the edit would not parse back (%1:%2: %3)"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	}
	auto why = QString();
	const auto table = FindListTable(back.table(), trimmed, why);
	if (!table) {
		return Refuse(text, u"the edit did not create '%1'"_q.arg(trimmed));
	}
	const auto members = table->get("members");
	const auto array = members ? members->as_array() : nullptr;
	if (!array || !array->empty()) {
		return Refuse(text, u"the edit left '%1' with members it should not "
			"have"_q.arg(trimmed));
	}
	result.changed = true;
	return result;
}

SpliceResult SetTableString(
		const QString &text,
		const QString &path,
		const QString &table,
		const QString &key,
		const QString &value) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		// Never write to a file that does not parse, for the same reason as
		// SetTableBool: the user may be halfway through an edit, and a blind
		// append would hand them a duplicate table to untangle on top of
		// whatever they were already fixing.
		const auto &error = parsed.error();
		return Refuse(text, u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	}
	const auto tableUtf8 = table.toUtf8();
	const auto keyUtf8 = key.toUtf8();
	const auto tableView = std::string_view(
		tableUtf8.constData(),
		tableUtf8.size());
	const auto keyView = std::string_view(keyUtf8.constData(), keyUtf8.size());
	const auto tableNode = parsed.table().get(tableView);
	const auto existing = tableNode ? tableNode->as_table() : nullptr;
	const auto node = existing ? existing->get(keyView) : nullptr;
	const auto wanted = BasicStringText(value);
	if (node) {
		const auto had = node->value<std::string_view>();
		if (had && Text(*had) == wanted) {
			return Unchanged(text);
		}
	}

	auto lines = text.split('\n');
	const auto quoted = QuotedValue(value);
	auto done = false;
	if (node) {
		// Replace exactly the span toml++ read the value from, rather than
		// scanning from the '=' the way the boolean does. A string can hold a
		// '#' or a quoted space, and it can be a """ block over several lines;
		// the recorded span is the only description of it that all three cases
		// agree with.
		const auto &at = node->source();
		const auto first = int(at.begin.line);
		const auto last = int(at.end.line);
		const auto from = int(at.begin.column) - 1;
		const auto till = int(at.end.column) - 1;
		if (first >= 1
			&& last >= first
			&& last <= lines.size()
			&& from >= 0
			&& from <= lines[first - 1].size()
			&& till >= 0
			&& till <= lines[last - 1].size()) {
			const auto head = lines[first - 1].left(from);
			const auto tail = lines[last - 1].mid(till);
			lines[first - 1] = head + quoted + tail;
			if (last > first) {
				lines.erase(
					lines.begin() + first,
					lines.begin() + last);
			}
			done = true;
		}
	}
	auto result = QString();
	if (done) {
		result = lines.join('\n');
	} else {
		const auto assignment = u"%1 = %2"_q.arg(key, quoted);
		if (existing && existing->is_inline()) {
			return Refuse(text, u"[%1] is written inline; rewrite it as a table "
				"before setting '%2' from the app."_q.arg(table, key));
		}
		const auto header = existing ? int(existing->source().begin.line) : 0;
		if (header >= 1 && header <= lines.size()) {
			if (DeclaresTable(lines[header - 1], table)) {
				lines.insert(header, assignment);
			} else if (lines[header - 1].trimmed().startsWith('[')) {
				// toml++ hands back a position for a table it never saw a
				// header for - one it invented because something deeper was
				// written. [schedule] is the live case: a file holding nothing
				// but [[schedule.rules]] blocks points here at the first of
				// them, and putting the key under that line would file it
				// inside the rule instead of the table. So write the header the
				// file is missing, above the block that implied it.
				lines.insert(header - 1, u"[%1]"_q.arg(TableKey(table)));
				lines.insert(header, assignment);
				lines.insert(header + 1, QString());
			} else {
				// A dotted key wrote the table - `schedule.outside = "home"'
				// at the top level. A header inserted above that line would
				// swallow the dotted key into it and mean something else.
				return Refuse(text, u"[%1] is written as dotted keys (line %2); "
					"give it a [%1] header before setting '%3' from the app."_q
					.arg(table)
					.arg(header)
					.arg(key));
			}
			result = lines.join('\n');
		} else {
			result = text;
			if (!result.isEmpty()) {
				if (!result.endsWith('\n')) {
					result += '\n';
				}
				result += '\n';
			}
			result += u"[%1]\n%2\n"_q.arg(table, assignment);
		}
	}

	const auto verifyUtf8 = result.toUtf8();
	auto verify = toml::parse(
		std::string_view(verifyUtf8.constData(), verifyUtf8.size()),
		path.toStdString());
	if (!verify) {
		return Refuse(text, u"the edit would not parse back"_q);
	}
	const auto back = verify.table()[tableView][keyView]
		.value<std::string_view>();
	if (!back || Text(*back) != wanted) {
		return Refuse(text, u"the edit did not set %1.%2"_q.arg(table, key));
	}
	auto splice = SpliceResult();
	splice.text = result;
	splice.changed = true;
	return splice;
}

SpliceResult SetTableBool(
		const QString &text,
		const QString &path,
		const QString &table,
		const QString &key,
		bool value) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		// Never write to a file that does not parse: the user may be halfway
		// through an edit, and a blind append would leave them a duplicate
		// table to untangle on top of whatever they were already fixing.
		const auto &error = parsed.error();
		return Refuse(text, u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	}
	const auto tableUtf8 = table.toUtf8();
	const auto keyUtf8 = key.toUtf8();
	const auto tableView = std::string_view(
		tableUtf8.constData(),
		tableUtf8.size());
	const auto keyView = std::string_view(keyUtf8.constData(), keyUtf8.size());
	const auto tableNode = parsed.table().get(tableView);
	const auto existing = tableNode ? tableNode->as_table() : nullptr;
	const auto node = existing ? existing->get(keyView) : nullptr;
	if (node && node->value<bool>() == value) {
		return Unchanged(text);
	}

	auto lines = text.split('\n');
	const auto line = node ? int(node->source().begin.line) : 0;
	auto done = false;
	if (line >= 1 && line <= lines.size()) {
		// Replace just the value token, so "enabled   =   true   # keep ads
		// away" keeps its spacing and its comment.
		auto &target = lines[line - 1];
		const auto assign = target.indexOf('=');
		if (assign >= 0) {
			auto from = assign + 1;
			while (from < target.size() && target[from].isSpace()) {
				++from;
			}
			auto till = from;
			while (till < target.size()
				&& !target[till].isSpace()
				&& target[till] != '#') {
				++till;
			}
			if (till > from) {
				target.replace(
					from,
					till - from,
					value ? u"true"_q : u"false"_q);
				done = true;
			}
		}
	}
	auto result = QString();
	if (done) {
		result = lines.join('\n');
	} else {
		const auto assignment = u"%1 = %2"_q
			.arg(key, value ? u"true"_q : u"false"_q);
		if (existing && existing->is_inline()) {
			return Refuse(text, u"[%1] is written inline; rewrite it as a table "
				"before setting '%2' from the app."_q.arg(table, key));
		}
		const auto header = existing ? int(existing->source().begin.line) : 0;
		if (header >= 1 && header <= lines.size()) {
			if (DeclaresTable(lines[header - 1], table)) {
				lines.insert(header, assignment);
			} else if (lines[header - 1].trimmed().startsWith('[')) {
				// toml++ hands back a position for a table it never saw a
				// header for - one it invented because something deeper was
				// written. [schedule] is the live case: a file holding nothing
				// but [[schedule.rules]] blocks points here at the first of
				// them, and putting the key under that line would file it
				// inside the rule instead of the table. So write the header the
				// file is missing, above the block that implied it.
				lines.insert(header - 1, u"[%1]"_q.arg(TableKey(table)));
				lines.insert(header, assignment);
				lines.insert(header + 1, QString());
			} else {
				// A dotted key wrote the table - `schedule.enabled_p = true'
				// at the top level. A header inserted above that line would
				// swallow the dotted key into it and mean something else.
				return Refuse(text, u"[%1] is written as dotted keys (line %2); "
					"give it a [%1] header before setting '%3' from the app."_q
					.arg(table)
					.arg(header)
					.arg(key));
			}
			result = lines.join('\n');
		} else {
			result = text;
			if (!result.isEmpty()) {
				if (!result.endsWith('\n')) {
					result += '\n';
				}
				result += '\n';
			}
			result += u"[%1]\n%2\n"_q.arg(table, assignment);
		}
	}

	const auto verifyUtf8 = result.toUtf8();
	auto verify = toml::parse(
		std::string_view(verifyUtf8.constData(), verifyUtf8.size()),
		path.toStdString());
	if (!verify) {
		return Refuse(text, u"the edit would not parse back"_q);
	}
	const auto written = verify.table()[tableView][keyView].value<bool>();
	if (written != value) {
		return Refuse(text, u"the edit did not set %1.%2"_q.arg(table, key));
	}
	auto splice = SpliceResult();
	splice.text = result;
	splice.changed = true;
	return splice;
}

SpliceResult SetScheduleRule(
		const QString &text,
		const QString &path,
		const QString &ruleset,
		int index,
		const ScheduleRuleExpected &expected,
		const ScheduleRule &rule) {
	if (const auto problem = RuleProblem(rule); !problem.isEmpty()) {
		return Refuse(text, problem);
	}
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return Refuse(text, u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	}
	auto error = QString();
	auto rulesetIndex = -1;
	const auto rules = FindRuleArray(
		parsed.table(),
		ruleset,
		rulesetIndex,
		error);
	if (!rules) {
		return Refuse(text, error);
	}
	const auto fields = ScheduleRuleAt(*rules, ruleset, index, error);
	if (!fields) {
		return Refuse(text, error);
	} else if (!RuleMatches(*fields, expected)) {
		return Refuse(text, u"%1 is not the rule you were editing any more; "
			"the file changed underneath."_q.arg(RuleWhere(ruleset, index)));
	}
	const auto before = Fingerprint(parsed.table());
	auto after = before;
	auto &signatures = (rulesetIndex < 0)
		? after.flat
		: after.rulesets[rulesetIndex].second;
	signatures[index] = WrittenSignature(rule);
	if (after == before) {
		return Unchanged(text);
	}

	auto lines = text.split('\n');
	const auto crlf = std::any_of(lines.begin(), lines.end(), [](
			const QString &line) {
		return line.endsWith('\r');
	});
	const auto ending = crlf ? u"\r"_q : QString();
	const auto header = int(fields->source().begin.line);
	if (header < 1 || header > lines.size()) {
		return Refuse(text, u"could not locate %1."_q
			.arg(RuleWhere(ruleset, index)));
	}

	// A key the block already has is rewritten where it stands; one it never
	// had joins the end of the block. The rewrites are applied from the bottom
	// of the file upwards, so a `days' array collapsing from four lines to one
	// cannot move a line another rewrite is still pointing at.
	auto indent = Indentation(lines[header - 1]);
	auto topmost = 0;
	for (auto &&[key, value] : *fields) {
		const auto line = int(value.source().begin.line);
		if (line > header && line <= lines.size()
			&& (!topmost || line < topmost)) {
			topmost = line;
		}
	}
	if (topmost) {
		indent = Indentation(lines[topmost - 1]);
	}
	auto rewrites = std::vector<std::pair<Position, QString>>();
	auto additions = QStringList();
	for (const auto &[key, value] : RuleValues(rule)) {
		const auto keyUtf8 = key.toUtf8();
		const auto node = fields->get(
			std::string_view(keyUtf8.constData(), keyUtf8.size()));
		if (node) {
			rewrites.push_back({
				Position{
					int(node->source().begin.line),
					int(node->source().begin.column),
				},
				value,
			});
		} else {
			additions.push_back(indent + key + u" = "_q + value + ending);
		}
	}
	if (!additions.isEmpty()) {
		const auto at = AfterBlock(lines, BlockLastLine(*fields, lines));
		for (auto i = additions.size(); i != 0;) {
			lines.insert(at, additions[--i]);
		}
	}
	std::sort(rewrites.begin(), rewrites.end(), [](
			const std::pair<Position, QString> &a,
			const std::pair<Position, QString> &b) {
		return (a.first.line != b.first.line)
			? (a.first.line > b.first.line)
			: (a.first.column > b.first.column);
	});
	for (const auto &[at, value] : rewrites) {
		if (!ReplaceValue(lines, at, value)) {
			return Refuse(text, u"could not rewrite %1 (line %2)."_q
				.arg(RuleWhere(ruleset, index))
				.arg(at.line));
		}
	}

	auto result = SpliceResult();
	result.text = lines.join('\n');
	if (auto failed = VerifySchedule(result.text, path, after);
		!failed.isEmpty()) {
		return Refuse(text, failed);
	}
	result.changed = true;
	return result;
}

SpliceResult AppendScheduleRule(
		const QString &text,
		const QString &path,
		const QString &ruleset,
		const ScheduleRule &rule) {
	if (const auto problem = RuleProblem(rule); !problem.isEmpty()) {
		return Refuse(text, problem);
	}
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return Refuse(text, u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	}

	auto lines = text.split('\n');
	const auto crlf = std::any_of(lines.begin(), lines.end(), [](
			const QString &line) {
		return line.endsWith('\r');
	});
	const auto ending = crlf ? u"\r"_q : QString();

	auto after = Fingerprint(parsed.table());
	auto at = lines.size();
	auto section = false;
	auto header = u"[[schedule.rules]]"_q;
	auto error = QString();
	if (!ruleset.trimmed().isEmpty()) {
		const auto found = FindRulesetNamed(parsed.table(), ruleset, error);
		if (!found.fields) {
			return Refuse(text, error);
		}
		header = u"[[schedule.rulesets.rules]]"_q;
		const auto node = found.fields->get("rules");
		const auto rules = node ? node->as_array() : nullptr;
		if (node && !rules) {
			return Refuse(text, u"'rules' in schedule ruleset '%1' is not an "
				"array (line %2)."_q.arg(ruleset.trimmed())
					.arg(int(node->source().begin.line)));
		} else if (rules && !rules->empty()) {
			// After the ruleset's last rule, which is before whatever header
			// comes next - the next ruleset, or another section entirely.
			const auto last = ScheduleRuleAt(
				*rules,
				ruleset,
				int(rules->size()) - 1,
				error);
			if (!last) {
				return Refuse(text, error);
			}
			at = AfterBlock(lines, BlockLastLine(*last, lines));
		} else {
			// A ruleset with no rules yet: the first one goes under the
			// ruleset's own keys, which is where somebody reading it looks.
			at = AfterBlock(lines, ScalarBlockLastLine(*found.fields, lines));
		}
		after.rulesets[found.index].second.push_back(WrittenSignature(rule));
	} else if (parsed.table().get("schedule")) {
		const auto table = FindScheduleTable(parsed.table(), error);
		if (!table) {
			return Refuse(text, error);
		}
		const auto node = table->get("rules");
		const auto rules = node ? node->as_array() : nullptr;
		if (node && !rules) {
			return Refuse(text, u"'schedule.rules' is not an array (line %1)."_q
				.arg(int(node->source().begin.line)));
		}
		if (rules && !rules->empty()) {
			const auto last = ScheduleRuleAt(
				*rules,
				ruleset,
				int(rules->size()) - 1,
				error);
			if (!last) {
				return Refuse(text, error);
			}
			at = AfterBlock(lines, BlockLastLine(*last, lines));
		} else {
			// A [schedule] with no rules yet: the first one goes under the
			// section's own keys, which is where somebody reading it looks.
			// An implicit [schedule] - dotted keys, no header - has no such
			// place, so that one falls through to the end of the file.
			const auto line = int(table->source().begin.line);
			if (line >= 1 && line <= lines.size()
				&& DeclaresTable(lines[line - 1], u"schedule"_q)) {
				at = AfterBlock(lines, ScalarBlockLastLine(*table, lines));
			}
		}
		after.flat.push_back(WrittenSignature(rule));
	} else {
		// No schedule at all. The section header goes in with the rule, so the
		// file gains one readable block rather than a rules array under nothing.
		section = true;
		after.flat.push_back(WrittenSignature(rule));
	}

	auto block = QStringList();
	block.push_back(ending);
	if (section) {
		block.push_back(u"[schedule]"_q + ending);
	}
	block.push_back(header + ending);
	for (const auto &[key, value] : RuleValues(rule)) {
		block.push_back(key.leftJustified(9) + u" = "_q + value + ending);
	}
	InsertBlock(lines, at, block);

	auto result = SpliceResult();
	result.text = lines.join('\n');
	if (auto failed = VerifySchedule(result.text, path, after);
		!failed.isEmpty()) {
		return Refuse(text, failed);
	}
	result.changed = true;
	return result;
}

SpliceResult RemoveScheduleRule(
		const QString &text,
		const QString &path,
		const QString &ruleset,
		int index,
		const ScheduleRuleExpected &expected) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return Refuse(text, u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	}
	auto error = QString();
	auto rulesetIndex = -1;
	const auto rules = FindRuleArray(
		parsed.table(),
		ruleset,
		rulesetIndex,
		error);
	if (!rules) {
		return Refuse(text, error);
	}
	const auto fields = ScheduleRuleAt(*rules, ruleset, index, error);
	if (!fields) {
		return Refuse(text, error);
	} else if (!RuleMatches(*fields, expected)) {
		return Refuse(text, u"%1 is not the rule you were editing any more; "
			"the file changed underneath."_q.arg(RuleWhere(ruleset, index)));
	}
	auto after = Fingerprint(parsed.table());
	auto &signatures = (rulesetIndex < 0)
		? after.flat
		: after.rulesets[rulesetIndex].second;
	signatures.removeAt(index);

	auto lines = text.split('\n');
	const auto header = int(fields->source().begin.line);
	if (header < 1 || header > lines.size()) {
		return Refuse(text, u"could not locate %1."_q
			.arg(RuleWhere(ruleset, index)));
	}
	const auto stop = AfterBlock(lines, BlockLastLine(*fields, lines));
	if (stop < header) {
		return Refuse(text, u"could not tell where %1 ends."_q
			.arg(RuleWhere(ruleset, index)));
	}
	lines.erase(lines.begin() + (header - 1), lines.begin() + stop);
	CloseSeam(lines, header - 1);

	auto result = SpliceResult();
	result.text = lines.join('\n');
	if (auto failed = VerifySchedule(result.text, path, after);
		!failed.isEmpty()) {
		return Refuse(text, failed);
	}
	result.changed = true;
	return result;
}

SpliceResult AddRuleset(
		const QString &text,
		const QString &path,
		const QString &name,
		const QString &device,
		RulesetMode mode) {
	const auto trimmed = BasicStringText(name);
	if (trimmed.isEmpty()) {
		return Refuse(text, u"a schedule ruleset needs a name."_q);
	}
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return Refuse(text, u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	} else if (RulesetNameTaken(parsed.table(), trimmed)) {
		// The name is the address every later edit goes through, so two
		// rulesets sharing one is not something to warn about afterwards.
		return Refuse(text, u"there is already a schedule ruleset called "
			"'%1'."_q.arg(trimmed));
	}

	auto lines = text.split('\n');
	const auto crlf = std::any_of(lines.begin(), lines.end(), [](
			const QString &line) {
		return line.endsWith('\r');
	});
	const auto ending = crlf ? u"\r"_q : QString();

	auto at = lines.size();
	auto section = false;
	auto error = QString();
	if (parsed.table().get("schedule")) {
		const auto table = FindScheduleTable(parsed.table(), error);
		if (!table) {
			return Refuse(text, error);
		}

		// After everything the schedule already holds, rules and rulesets
		// alike, so the file keeps its schedule in one piece.
		at = AfterBlock(lines, ScheduleLastLine(*table, lines));
	} else {
		section = true;
	}

	// Only what differs from the default is written down. A file where every
	// ruleset spells out `mode = "enabled"' says no more than one that does not
	// and is a good deal harder to read.
	const auto wantsDevice = !BasicStringText(device).isEmpty()
		&& BasicStringText(device).compare(u"any"_q, Qt::CaseInsensitive);
	const auto wantsMode = (mode != RulesetMode::Enabled);
	auto values = std::vector<std::pair<QString, QString>>();
	values.push_back({ u"name"_q, QuotedValue(trimmed) });
	if (wantsDevice) {
		values.push_back({ u"device"_q, QuotedValue(device) });
	}
	if (wantsMode) {
		values.push_back({ u"mode"_q, QuotedValue(RulesetModeName(mode)) });
	}

	auto after = Fingerprint(parsed.table());
	after.rulesets.push_back({
		WrittenRulesetSignature(
			trimmed,
			wantsDevice
				? std::make_optional(BasicStringText(device))
				: std::nullopt,
			wantsMode
				? std::make_optional(RulesetModeName(mode))
				: std::nullopt,
			std::nullopt),
		QStringList(),
	});

	auto block = QStringList();
	block.push_back(ending);
	if (section) {
		block.push_back(u"[schedule]"_q + ending);
	}
	block.push_back(u"[[schedule.rulesets]]"_q + ending);
	for (const auto &[key, value] : values) {
		block.push_back(key.leftJustified(7) + u" = "_q + value + ending);
	}
	InsertBlock(lines, at, block);

	auto result = SpliceResult();
	result.text = lines.join('\n');
	if (auto failed = VerifySchedule(result.text, path, after);
		!failed.isEmpty()) {
		return Refuse(text, failed);
	}
	result.changed = true;
	return result;
}

SpliceResult RemoveRuleset(
		const QString &text,
		const QString &path,
		const QString &name) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return Refuse(text, u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	}
	auto error = QString();
	const auto found = FindRulesetNamed(parsed.table(), name, error);
	if (!found.fields) {
		return Refuse(text, error);
	}
	auto after = Fingerprint(parsed.table());
	after.rulesets.erase(after.rulesets.begin() + found.index);

	auto lines = text.split('\n');
	const auto header = int(found.fields->source().begin.line);
	if (header < 1 || header > lines.size()) {
		return Refuse(text, u"could not locate schedule ruleset '%1'."_q
			.arg(name.trimmed()));
	}

	// The ruleset ends where its LAST rule ends, not where its own keys do:
	// every [[schedule.rulesets.rules]] block under it belongs to it and goes
	// with it.
	auto lastLine = ScalarBlockLastLine(*found.fields, lines);
	if (const auto node = found.fields->get("rules")) {
		if (const auto rules = node->as_array()) {
			for (auto &&element : *rules) {
				const auto block = element.as_table();
				if (block && !block->is_inline()) {
					lastLine = std::max(
						lastLine,
						ScalarBlockLastLine(*block, lines));
				}
			}
		}
	}
	const auto stop = AfterBlock(lines, lastLine);
	if (stop < header) {
		return Refuse(text, u"could not tell where schedule ruleset '%1' "
			"ends."_q.arg(name.trimmed()));
	}
	lines.erase(lines.begin() + (header - 1), lines.begin() + stop);
	CloseSeam(lines, header - 1);

	auto result = SpliceResult();
	result.text = lines.join('\n');
	if (auto failed = VerifySchedule(result.text, path, after);
		!failed.isEmpty()) {
		return Refuse(text, failed);
	}
	result.changed = true;
	return result;
}

SpliceResult SetRulesetString(
		const QString &text,
		const QString &path,
		const QString &name,
		const QString &key,
		const QString &value) {
	const auto trimmedKey = key.trimmed();
	if (trimmedKey.isEmpty()) {
		return Refuse(text, u"a schedule ruleset key needs a name."_q);
	}
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return Refuse(text, u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	}
	auto error = QString();
	const auto found = FindRulesetNamed(parsed.table(), name, error);
	if (!found.fields) {
		return Refuse(text, error);
	}
	const auto wanted = BasicStringText(value);
	const auto clearing = wanted.isEmpty();
	if (trimmedKey == u"name"_q) {
		// A rename is allowed, but not into nothing and not onto a name that is
		// already an address: both would leave a ruleset nothing can edit.
		if (clearing) {
			return Refuse(text, u"a schedule ruleset needs a name."_q);
		} else if (wanted.compare(name.trimmed(), Qt::CaseInsensitive)
			&& RulesetNameTaken(parsed.table(), wanted)) {
			return Refuse(text, u"there is already a schedule ruleset called "
				"'%1'."_q.arg(wanted));
		}
	}
	const auto before = Fingerprint(parsed.table());
	auto after = before;
	after.rulesets[found.index].first = RulesetSignature(
		*found.fields,
		trimmedKey,
		clearing ? std::nullopt : std::make_optional(wanted));
	if (after == before) {
		return Unchanged(text);
	}

	auto lines = text.split('\n');
	const auto keyUtf8 = trimmedKey.toUtf8();
	const auto node = found.fields->get(
		std::string_view(keyUtf8.constData(), keyUtf8.size()));
	if (node && node->as_array() && node->as_array()->is_array_of_tables()) {
		// 'rules' is the one key here that is blocks rather than a value. The
		// verify would catch the attempt anyway, but not with a sentence saying
		// which op the caller wanted.
		return Refuse(text, u"'%1' in schedule ruleset '%2' is a set of blocks, "
			"not a value; use the rule ops to change it."_q
				.arg(trimmedKey, name.trimmed()));
	} else if (node) {
		const auto at = Position{
			int(node->source().begin.line),
			int(node->source().begin.column),
		};
		const auto done = clearing
			? RemoveValueLines(lines, at)
			: ReplaceValue(lines, at, QuotedValue(value));
		if (!done) {
			return Refuse(text, u"could not rewrite '%1' in schedule ruleset "
				"'%2' (line %3)."_q.arg(trimmedKey, name.trimmed())
					.arg(at.line));
		}
	} else if (!clearing) {
		// A key the ruleset never had joins the end of its own block, above the
		// first of its rules rather than inside it.
		const auto lastLine = ScalarBlockLastLine(*found.fields, lines);
		const auto at = AfterBlock(lines, lastLine);
		const auto header = int(found.fields->source().begin.line);
		auto indent = QString();
		if (lastLine > header && lastLine <= lines.size()) {
			indent = Indentation(lines[lastLine - 1]);
		}
		const auto crlf = std::any_of(lines.begin(), lines.end(), [](
				const QString &line) {
			return line.endsWith('\r');
		});
		lines.insert(
			at,
			indent + trimmedKey + u" = "_q + QuotedValue(value)
				+ (crlf ? u"\r"_q : QString()));
	}

	auto result = SpliceResult();
	result.text = lines.join('\n');
	if (auto failed = VerifySchedule(result.text, path, after);
		!failed.isEmpty()) {
		return Refuse(text, failed);
	}
	result.changed = true;
	return result;
}

SpliceResult AppendBudget(
		const QString &text,
		const QString &path,
		const ScreenTimeBudget &budget) {
	if (const auto problem = BudgetProblem(budget); !problem.isEmpty()) {
		return Refuse(text, problem);
	}
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return Refuse(text, u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	}

	auto lines = text.split('\n');
	const auto crlf = std::any_of(lines.begin(), lines.end(), [](
			const QString &line) {
		return line.endsWith('\r');
	});
	const auto ending = crlf ? u"\r"_q : QString();

	auto after = BudgetFingerprint(parsed.table());
	const auto index = int(after.size());
	auto at = lines.size();
	auto section = false;
	auto error = QString();
	if (parsed.table().get("screen_time")) {
		const auto table = FindScreenTimeTable(parsed.table(), error);
		if (!table) {
			return Refuse(text, error);
		}
		const auto node = table->get("budgets");
		const auto budgets = node ? node->as_array() : nullptr;
		if (node && !budgets) {
			return Refuse(text, u"'screen_time.budgets' is not an array "
				"(line %1)."_q.arg(int(node->source().begin.line)));
		} else if (budgets && budgets->empty()) {
			// `budgets = []' is an array the file wrote out in full, and TOML
			// does not let a [[screen_time.budgets]] header extend one of
			// those. Say so, rather than writing a file that will not parse.
			return Refuse(text, u"'screen_time.budgets' is written as an empty "
				"array (line %1); take that line out before adding a budget "
				"from the app."_q.arg(int(node->source().begin.line)));
		} else if (budgets) {
			// After the last budget, which is before whatever header comes
			// next - so the budgets stay together and a reader finds the new
			// one where they were already looking.
			const auto last = BudgetAt(*budgets, int(budgets->size()) - 1, error);
			if (!last) {
				return Refuse(text, error);
			}
			at = AfterBlock(lines, BlockLastLine(*last, lines));
		} else {
			// A [screen_time] with no budgets yet: the first one goes under
			// the section's own keys, which is where somebody reading it looks.
			// A [screen_time] the file never spelled out - dotted keys, or a
			// deeper header that implied it - has no such place, so that one
			// falls through to the end of the file, where a
			// [[screen_time.budgets]] header still says where it belongs.
			const auto line = int(table->source().begin.line);
			if (line >= 1 && line <= lines.size()
				&& DeclaresTable(lines[line - 1], u"screen_time"_q)) {
				at = AfterBlock(lines, ScalarBlockLastLine(*table, lines));
			}
		}
	} else {
		// No screen time at all. The section header goes in with the budget, so
		// the file gains one readable block rather than a budgets array under
		// nothing.
		section = true;
	}
	after.push_back(WrittenBudgetSignature(budget));

	const auto values = BudgetValues(budget);
	auto width = 0;
	for (const auto &value : values) {
		if (value.text) {
			width = std::max(width, int(value.key.size()));
		}
	}
	auto block = QStringList();
	block.push_back(ending);
	if (section) {
		block.push_back(u"[screen_time]"_q + ending);
	}
	block.push_back(u"[[screen_time.budgets]]"_q + ending);
	for (const auto &value : values) {
		if (value.text) {
			block.push_back(value.key.leftJustified(width)
				+ u" = "_q
				+ value.written()
				+ ending);
		}
	}
	InsertBlock(lines, at, block);

	auto result = SpliceResult();
	result.text = lines.join('\n');
	if (auto failed = VerifyBudgets(result.text, path, after);
		!failed.isEmpty()) {
		return Refuse(text, failed);
	} else if (!BudgetReadsBack(result.text, path, index)) {
		return Refuse(text, u"a budget for '%1' would not read back; check "
			"its target."_q.arg(budget.target.trimmed()));
	}
	result.changed = true;
	return result;
}

SpliceResult SetBudget(
		const QString &text,
		const QString &path,
		int index,
		const QString &expectedTarget,
		const ScreenTimeBudget &budget) {
	if (const auto problem = BudgetProblem(budget); !problem.isEmpty()) {
		return Refuse(text, problem);
	}
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return Refuse(text, u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	}
	auto error = QString();
	const auto budgets = FindBudgets(parsed.table(), error);
	if (!budgets) {
		return Refuse(text, error);
	}
	const auto fields = BudgetAt(*budgets, index, error);
	if (!fields) {
		return Refuse(text, error);
	} else if (!BudgetTargetMatches(*fields, expectedTarget)) {
		return Refuse(text, u"%1 is not the budget you were editing any more; "
			"the file changed underneath."_q.arg(BudgetWhere(index)));
	}
	const auto before = BudgetFingerprint(parsed.table());
	auto after = before;
	after[index] = WrittenBudgetSignature(budget);
	if (after == before) {
		return Unchanged(text);
	}

	auto lines = text.split('\n');
	const auto crlf = std::any_of(lines.begin(), lines.end(), [](
			const QString &line) {
		return line.endsWith('\r');
	});
	const auto ending = crlf ? u"\r"_q : QString();
	const auto header = int(fields->source().begin.line);
	if (header < 1 || header > lines.size()) {
		return Refuse(text, u"could not locate %1."_q.arg(BudgetWhere(index)));
	}

	// A key the block already has is rewritten where it stands, or taken out
	// when the budget is back to the default for it; one it never had joins the
	// end of the block. The edits are applied from the bottom of the file
	// upwards, so a line going away cannot move a line another edit is still
	// pointing at - and the additions go in below all of them, at a line no
	// edit points at.
	auto indent = Indentation(lines[header - 1]);
	auto topmost = 0;
	for (auto &&[key, value] : *fields) {
		const auto line = int(value.source().begin.line);
		if (line > header && line <= lines.size()
			&& (!topmost || line < topmost)) {
			topmost = line;
		}
	}
	if (topmost) {
		indent = Indentation(lines[topmost - 1]);
	}
	auto edits = std::vector<std::pair<Position, std::optional<QString>>>();
	auto additions = QStringList();
	for (const auto &value : BudgetValues(budget)) {
		const auto keyUtf8 = value.key.toUtf8();
		const auto node = fields->get(
			std::string_view(keyUtf8.constData(), keyUtf8.size()));
		if (node) {
			edits.push_back({
				Position{
					int(node->source().begin.line),
					int(node->source().begin.column),
				},
				value.text
					? std::make_optional(value.written())
					: std::nullopt,
			});
		} else if (value.text) {
			additions.push_back(
				indent + value.key + u" = "_q + value.written() + ending);
		}
	}
	if (!additions.isEmpty()) {
		const auto at = AfterBlock(lines, BlockLastLine(*fields, lines));
		for (auto i = additions.size(); i != 0;) {
			lines.insert(at, additions[--i]);
		}
	}
	std::sort(edits.begin(), edits.end(), [](
			const std::pair<Position, std::optional<QString>> &a,
			const std::pair<Position, std::optional<QString>> &b) {
		return (a.first.line != b.first.line)
			? (a.first.line > b.first.line)
			: (a.first.column > b.first.column);
	});
	for (const auto &[at, value] : edits) {
		const auto done = value
			? ReplaceValue(lines, at, *value)
			: RemoveValueLines(lines, at);
		if (!done) {
			return Refuse(text, u"could not rewrite %1 (line %2)."_q
				.arg(BudgetWhere(index))
				.arg(at.line));
		}
	}

	auto result = SpliceResult();
	result.text = lines.join('\n');
	if (auto failed = VerifyBudgets(result.text, path, after);
		!failed.isEmpty()) {
		return Refuse(text, failed);
	} else if (!BudgetReadsBack(result.text, path, index)) {
		return Refuse(text, u"a budget for '%1' would not read back; check "
			"its target."_q.arg(budget.target.trimmed()));
	}
	result.changed = true;
	return result;
}

SpliceResult RemoveBudget(
		const QString &text,
		const QString &path,
		int index,
		const QString &expectedTarget) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		const auto &error = parsed.error();
		return Refuse(text, u"%1:%2: %3"_q
			.arg(error.source().begin.line)
			.arg(error.source().begin.column)
			.arg(Text(error.description())));
	}
	auto error = QString();
	const auto budgets = FindBudgets(parsed.table(), error);
	if (!budgets) {
		return Refuse(text, error);
	}
	const auto fields = BudgetAt(*budgets, index, error);
	if (!fields) {
		return Refuse(text, error);
	} else if (!BudgetTargetMatches(*fields, expectedTarget)) {
		return Refuse(text, u"%1 is not the budget you were editing any more; "
			"the file changed underneath."_q.arg(BudgetWhere(index)));
	}
	auto after = BudgetFingerprint(parsed.table());
	after.removeAt(index);

	auto lines = text.split('\n');
	const auto header = int(fields->source().begin.line);
	if (header < 1 || header > lines.size()) {
		return Refuse(text, u"could not locate %1."_q.arg(BudgetWhere(index)));
	}
	const auto stop = AfterBlock(lines, BlockLastLine(*fields, lines));
	if (stop < header) {
		return Refuse(text, u"could not tell where %1 ends."_q
			.arg(BudgetWhere(index)));
	}
	lines.erase(lines.begin() + (header - 1), lines.begin() + stop);
	CloseSeam(lines, header - 1);

	auto result = SpliceResult();
	result.text = lines.join('\n');
	if (auto failed = VerifyBudgets(result.text, path, after);
		!failed.isEmpty()) {
		return Refuse(text, failed);
	}
	result.changed = true;
	return result;
}

std::vector<PeerIdValue> ListMembers(
		const QString &text,
		const QString &path,
		const QString &list) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		return {};
	}
	auto ignored = QString();
	const auto table = FindListTable(parsed.table(), list, ignored);
	if (!table) {
		return {};
	}
	const auto members = table->get("members");
	const auto array = members ? members->as_array() : nullptr;
	return array ? IdsOf(*array) : std::vector<PeerIdValue>();
}

std::vector<PeerIdValue> PresetPinned(
		const QString &text,
		const QString &path,
		const QString &preset) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		return {};
	}
	auto ignored = QString();
	const auto table = FindPresetTable(parsed.table(), preset, ignored);
	if (!table) {
		return {};
	}
	const auto pinned = table->get("pinned");
	const auto array = pinned ? pinned->as_array() : nullptr;
	return array ? IdsOf(*array) : std::vector<PeerIdValue>();
}

std::vector<PeerIdValue> ViewPinned(
		const QString &text,
		const QString &path,
		const QString &preset,
		const QString &view) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		return {};
	}
	auto ignored = QString();
	const auto table = FindViewTable(parsed.table(), preset, view, ignored);
	if (!table) {
		return {};
	}
	const auto pinned = table->get("pinned");
	const auto array = pinned ? pinned->as_array() : nullptr;
	return array ? IdsOf(*array) : std::vector<PeerIdValue>();
}

} // namespace Purple
