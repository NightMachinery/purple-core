/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_settings.h"

#include <functional>

// The only writes the app ever makes to settings.toml. The file is hand-owned
// and its comments are the point of it being TOML at all, so nothing here
// re-serialises the document: we locate the members array through toml++'s
// source regions and edit the raw lines, leaving every byte we are not
// responsible for exactly where the user put it. See docs/purple/config.md.
namespace Purple {

// Resolves a peer id to the display name written as that line's trailing
// comment. Names go stale, so the comment is regenerated every time its line is
// rewritten rather than being read back from the file.
using MemberTitle = std::function<QString(PeerIdValue)>;

struct SpliceResult {
	QString text;
	bool changed = false;

	// Non-empty means nothing was written and `text` is the input unchanged.
	QString error;

	[[nodiscard]] bool ok() const {
		return error.isEmpty();
	}
};

[[nodiscard]] SpliceResult AddListMember(
	const QString &text,
	const QString &path,
	const QString &list,
	PeerIdValue id,
	const MemberTitle &title);

[[nodiscard]] SpliceResult RemoveListMember(
	const QString &text,
	const QString &path,
	const QString &list,
	PeerIdValue id,
	const MemberTitle &title);

// Rewrites one extra view's pinned order. Unlike a list's members this is an
// ordered array the app owns outright, so the whole bracketed span is replaced
// rather than edited a line at a time: the ids move relative to one another on
// every drag, and there is no stable line for a comment to belong to.
//
// The view is addressed by name rather than by position. The file may hold
// views the parser dropped - unnamed, duplicated, listing no list - so the nth
// [[presets.x.views]] block in the file is not the nth tab on the strip, and a
// pin written by index would land on a different view than the one dragged.
[[nodiscard]] SpliceResult SetViewPinned(
	const QString &text,
	const QString &path,
	const QString &preset,
	const QString &view,
	const std::vector<PeerIdValue> &ids,
	const MemberTitle &title);

// The same, for the preset's own main view. Separate from the view version
// above only because the table is found differently - a preset has no `name'
// key to sit a fresh array under, so it goes straight below the header.
//
// A preset that names `pinned' owns its main view's order outright, instead of
// mirroring the account's. That is the point of the key: a pin made inside a
// work preset stays inside it and never reaches the server.
[[nodiscard]] SpliceResult SetPresetPinned(
	const QString &text,
	const QString &path,
	const QString &preset,
	const std::vector<PeerIdValue> &ids,
	const MemberTitle &title);

// Appends a new empty [lists.x] table, just after the last one already there so
// the lists stay together. The list is created and nothing else: no preset
// names it yet, so it changes nothing until one does.
//
// Refuses a name the parser would refuse - empty, or starting with '*' - and a
// name already taken. `title' is written only when it says something the name
// does not.
[[nodiscard]] SpliceResult AddList(
	const QString &text,
	const QString &path,
	const QString &name,
	const QString &title);

// Sets one boolean under one table, keeping the key, the spacing and any
// trailing comment exactly as the user wrote them. Adds the key, and the table,
// if either is missing. Used for the Premium toggle in Settings, which is the
// only scalar the app owns in an otherwise hand-written file.
[[nodiscard]] SpliceResult SetTableBool(
	const QString &text,
	const QString &path,
	const QString &table,
	const QString &key,
	bool value);

// What a screen believes it is about to edit: the window and the preset it read
// off the rule. Every schedule op takes one and refuses when the rule at that
// index no longer says the same thing, so a dialog left open while the file
// changed underneath cannot rewrite or delete a rule other than the one on it.
//
// Times are minutes since midnight rather than the text the file holds, so
// "9:00" and "09:00" are the same rule.
struct ScheduleRuleExpected {
	int from = -1;
	int till = -1;
	QString preset;
};

// Rewrites one [[schedule.rules]] block in place, key by key: each of
// 'enabled_p', 'days', 'from', 'to' and 'preset' has its value replaced where
// it stands, or is added at the end of the block when the rule never had it.
// Everything else - a key the app knows nothing about, a comment, the spacing -
// is left exactly where the user put it.
//
// `index' is ScheduleRule::sourceIndex: the position in the RAW array, counting
// the rules the parser threw away, which is what stops a broken rule in the
// middle of the file from moving the ones after it.
[[nodiscard]] SpliceResult SetScheduleRule(
	const QString &text,
	const QString &path,
	int index,
	const ScheduleRuleExpected &expected,
	const ScheduleRule &rule);

// Writes a new rule after the last [[schedule.rules]] block, or under
// [schedule] when there are no rules yet, or as a fresh [schedule] section at
// the end of the file when there is no schedule at all.
[[nodiscard]] SpliceResult AppendScheduleRule(
	const QString &text,
	const QString &path,
	const ScheduleRule &rule);

// Takes a rule out: its header down to the line before the next one. The blank
// line and any comment block above that next header stay where they are - they
// belong to what follows, not to what is going - and so does anything written
// above the rule's own header, which may well be a note about the section.
[[nodiscard]] SpliceResult RemoveScheduleRule(
	const QString &text,
	const QString &path,
	int index,
	const ScheduleRuleExpected &expected);

// Exposed for the tests: the ids a list holds, in file order.
[[nodiscard]] std::vector<PeerIdValue> ListMembers(
	const QString &text,
	const QString &path,
	const QString &list);

// Exposed for the tests: the ids a view pins, in file order.
[[nodiscard]] std::vector<PeerIdValue> PresetPinned(
	const QString &text,
	const QString &path,
	const QString &preset);

[[nodiscard]] std::vector<PeerIdValue> ViewPinned(
	const QString &text,
	const QString &path,
	const QString &preset,
	const QString &view);

} // namespace Purple
