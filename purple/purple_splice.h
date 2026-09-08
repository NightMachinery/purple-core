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

// The same op for a string, written as a TOML basic string with '"' and '\'
// escaped. Used for [schedule] outside, the one string the app sets on the
// user's behalf.
//
// The value is tidied the way every other string this file writes is: runs of
// whitespace collapse and control characters are dropped, because a newline
// arriving from a text field is a mis-paste rather than something worth keeping
// in a TOML line. "Already set" is judged against that tidied form, so a value
// the tidying changes still settles after one write instead of rewriting the
// file on every call.
[[nodiscard]] SpliceResult SetTableString(
	const QString &text,
	const QString &path,
	const QString &table,
	const QString &key,
	const QString &value);

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

// WHERE a rule lives, in every op below. An empty `ruleset' is the flat
// [[schedule.rules]] array, which is the only place rules could be before
// rulesets existed and is still where a file that never grew one keeps them.
// A name is a [[schedule.rulesets]] block, matched ignoring case, and the index
// then counts within THAT ruleset's own [[schedule.rulesets.rules]] blocks.
//
// A ruleset is addressed by name and never by position, because its position
// moves whenever one above it is added or taken away, and an index a dialog
// read a minute ago would then edit the wrong ruleset.

// Rewrites one rule block in place, key by key: each of 'enabled_p', 'days',
// 'from', 'to' and 'preset' has its value replaced where it stands, or is added
// at the end of the block when the rule never had it. Everything else - a key
// the app knows nothing about, a comment, the spacing - is left exactly where
// the user put it.
//
// `index' is ScheduleRule::sourceIndex: the position in the RAW array, counting
// the rules the parser threw away, which is what stops a broken rule in the
// middle of the file from moving the ones after it.
[[nodiscard]] SpliceResult SetScheduleRule(
	const QString &text,
	const QString &path,
	const QString &ruleset,
	int index,
	const ScheduleRuleExpected &expected,
	const ScheduleRule &rule);

// Adds a rule at the end of wherever it belongs: after the last rule of the
// named ruleset and before whatever header comes next, or after the last flat
// rule. A file with no [schedule] at all gains the section along with the rule.
[[nodiscard]] SpliceResult AppendScheduleRule(
	const QString &text,
	const QString &path,
	const QString &ruleset,
	const ScheduleRule &rule);

// Takes one rule out, header through the line before the next block, leaving
// the blank line and any comment above the block that follows.
[[nodiscard]] SpliceResult RemoveScheduleRule(
	const QString &text,
	const QString &path,
	const QString &ruleset,
	int index,
	const ScheduleRuleExpected &expected);

// Adds an empty [[schedule.rulesets]] block after everything the schedule
// already holds. `device' and `mode' are written only when they are not the
// defaults ("any" and enabled), because a file where every ruleset spells out
// what it would have meant anyway is harder to read for no gain.
//
// Refuses an empty name and one already taken, ignoring case: the name is the
// address every later edit goes through.
[[nodiscard]] SpliceResult AddRuleset(
	const QString &text,
	const QString &path,
	const QString &name,
	const QString &device,
	RulesetMode mode);

// Takes a whole ruleset out, its rules with it - they are its rules and mean
// nothing without it.
[[nodiscard]] SpliceResult RemoveRuleset(
	const QString &text,
	const QString &path,
	const QString &name);

// Sets one of a ruleset's own string keys - 'device', 'mode', 'outside', or
// 'name' for a rename - keeping the spacing and any trailing comment. An empty
// `value' takes the key out of the file, which is how a screen says "back to
// the default" without writing the default down.
[[nodiscard]] SpliceResult SetRulesetString(
	const QString &text,
	const QString &path,
	const QString &name,
	const QString &key,
	const QString &value);

// Adds a budget at the end of wherever it belongs: after the last
// [[screen_time.budgets]] block, or under [screen_time]'s own keys when there
// are no budgets yet, or in a [screen_time] section written at the end of a
// file that had none.
//
// 'mode', 'snooze' and 'snoozes_per_day' are written only when they are not
// the defaults, because a file where every budget spells out what it would
// have meant anyway is harder to read for no gain.
[[nodiscard]] SpliceResult AppendBudget(
	const QString &text,
	const QString &path,
	const ScreenTimeBudget &budget);

// Rewrites one budget block in place, key by key, the way SetScheduleRule()
// does: each key has its value replaced where it stands, is added at the end
// of the block when the budget never had it, or - for the three that have
// defaults - has its line taken out when the value is back to the default.
// Everything else is left exactly where the user put it.
//
// `index' is ScreenTimeBudget::sourceIndex: the position in the RAW array,
// counting the budgets the parser threw away, which is what stops a broken
// budget in the middle of the file from moving the ones after it.
//
// `expectedTarget' is the target the screen read off the budget. The edit is
// refused when the budget at that index says something else, so a dialog left
// open while the file changed underneath cannot rewrite a budget other than
// the one on it. A budget has no other identity: the target is what it is
// about, and everything else about it is what the dialog is there to change.
[[nodiscard]] SpliceResult SetBudget(
	const QString &text,
	const QString &path,
	int index,
	const QString &expectedTarget,
	const ScreenTimeBudget &budget);

// Takes one budget out, header through the line before the next block, leaving
// the blank line and any comment above the block that follows.
[[nodiscard]] SpliceResult RemoveBudget(
	const QString &text,
	const QString &path,
	int index,
	const QString &expectedTarget);

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
