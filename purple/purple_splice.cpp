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

// A TOML basic string. Control characters are dropped rather than escaped: a
// list title arrives from a text field, and a stray newline in one is a
// mis-paste rather than something worth preserving.
[[nodiscard]] QString QuotedValue(const QString &value) {
	auto result = QString('"');
	for (const auto ch : value.simplified()) {
		if (ch == '"' || ch == '\\') {
			result += '\\';
			result += ch;
		} else if (ch.unicode() >= 0x20) {
			result += ch;
		}
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

// One rule by its raw position. The array is walked as it is rather than
// filtered: an element the parser threw away is still an element, and a rule
// keeping its address while the one above it is broken is the whole point of
// addressing rules this way.
[[nodiscard]] const toml::table *ScheduleRuleAt(
		const toml::array &rules,
		int index,
		QString &error) {
	if (index < 0 || index >= int(rules.size())) {
		error = u"there is no schedule rule %1 any more."_q.arg(index + 1);
		return nullptr;
	}
	const auto element = rules.get(index);
	const auto fields = element ? element->as_table() : nullptr;
	if (!fields) {
		error = u"schedule rule %1 is not a table (line %2)."_q
			.arg(index + 1)
			.arg(element ? int(element->source().begin.line) : 0);
		return nullptr;
	} else if (fields->is_inline()) {
		error = u"schedule rule %1 is written inline (line %2); rewrite it as a "
			"[[schedule.rules]] table before editing it from the app."_q
			.arg(index + 1)
			.arg(int(fields->source().begin.line));
		return nullptr;
	}
	return fields;
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
	auto ignored = QString();
	const auto rules = FindScheduleRules(parsed.table(), ignored);
	const auto after = rules ? RuleSignatures(*rules) : QStringList();
	if (after != expected) {
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
	const auto rules = FindScheduleRules(parsed.table(), error);
	if (!rules) {
		return Refuse(text, error);
	}
	const auto fields = ScheduleRuleAt(*rules, index, error);
	if (!fields) {
		return Refuse(text, error);
	} else if (!RuleMatches(*fields, expected)) {
		return Refuse(text, u"schedule rule %1 is not the rule you were "
			"editing any more; the file changed underneath."_q.arg(index + 1));
	}
	const auto before = RuleSignatures(*rules);
	auto after = before;
	after[index] = WrittenSignature(rule);
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
		return Refuse(text, u"could not locate schedule rule %1."_q
			.arg(index + 1));
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
			return Refuse(text, u"could not rewrite schedule rule %1 (line "
				"%2)."_q.arg(index + 1).arg(at.line));
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

	auto before = QStringList();
	auto at = lines.size();
	auto section = false;
	if (parsed.table().get("schedule")) {
		auto error = QString();
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
		before = rules ? RuleSignatures(*rules) : QStringList();
		if (rules && !rules->empty()) {
			const auto last = ScheduleRuleAt(
				*rules,
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
				at = AfterBlock(lines, BlockLastLine(*table, lines));
			}
		}
	} else {
		// No schedule at all. The section header goes in with the rule, so the
		// file gains one readable block rather than a rules array under nothing.
		section = true;
	}

	auto block = QStringList();
	block.push_back(ending);
	if (section) {
		block.push_back(u"[schedule]"_q + ending);
	}
	block.push_back(u"[[schedule.rules]]"_q + ending);
	for (const auto &[key, value] : RuleValues(rule)) {
		block.push_back(key.leftJustified(9) + u" = "_q + value + ending);
	}
	if (at >= lines.size()) {
		// At the end of the file, where there may be no trailing newline.
		if (!lines.isEmpty() && lines.back().trimmed().isEmpty()) {
			lines.removeLast();
		}
		if (lines.isEmpty()) {
			// Nothing above to be separated from.
			block.removeFirst();
		}
		lines += block;
		lines.push_back(QString());
	} else {
		if (!lines[at].trimmed().isEmpty()) {
			// A blank line below too, unless the file already has one there.
			block.push_back(QString());
		}
		for (auto i = block.size(); i != 0;) {
			lines.insert(at, block[--i]);
		}
	}

	auto after = before;
	after.push_back(WrittenSignature(rule));

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
	const auto rules = FindScheduleRules(parsed.table(), error);
	if (!rules) {
		return Refuse(text, error);
	}
	const auto fields = ScheduleRuleAt(*rules, index, error);
	if (!fields) {
		return Refuse(text, error);
	} else if (!RuleMatches(*fields, expected)) {
		return Refuse(text, u"schedule rule %1 is not the rule you were "
			"editing any more; the file changed underneath."_q.arg(index + 1));
	}
	auto after = RuleSignatures(*rules);
	after.removeAt(index);

	auto lines = text.split('\n');
	const auto header = int(fields->source().begin.line);
	if (header < 1 || header > lines.size()) {
		return Refuse(text, u"could not locate schedule rule %1."_q
			.arg(index + 1));
	}
	const auto stop = AfterBlock(lines, BlockLastLine(*fields, lines));
	if (stop < header) {
		return Refuse(text, u"could not tell where schedule rule %1 ends."_q
			.arg(index + 1));
	}
	lines.erase(lines.begin() + (header - 1), lines.begin() + stop);

	// The blank line above the block that is going stays, and so does the one
	// below it that belongs to whatever follows - which would leave two of them
	// where there was one. Close the seam.
	const auto seam = header - 1;
	if (seam >= 1
		&& seam < lines.size()
		&& lines[seam - 1].trimmed().isEmpty()
		&& lines[seam].trimmed().isEmpty()) {
		lines.removeAt(seam);
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
