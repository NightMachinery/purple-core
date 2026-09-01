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
		const auto header = (existing && !existing->is_inline())
			? int(existing->source().begin.line)
			: 0;
		if (header >= 1 && header <= lines.size()) {
			lines.insert(header, assignment);
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
