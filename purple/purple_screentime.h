/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_settings.h"
#include "purple/purple_types.h"

#include <QtCore/QDate>
#include <QtCore/QString>
#include <QtCore/QTimeZone>

#include <array>
#include <optional>
#include <vector>

// screentime.log and everything derived from it.
//
// The log is raw events and nothing else - no totals, no sessions, no days.
// That is the whole design: every threshold in [screen_time] is applied here,
// at read time, so changing action_span or idle_after re-derives the history
// you already have instead of only affecting what happens next. A log of
// totals would have baked yesterday's settings into yesterday forever.
//
// Local only. Nothing in this file sends anything anywhere, and the plan says
// it never will: two devices would double-count nothing useful, and this is a
// record of what you looked at.
namespace Purple {

// What happened. One line per event, and the kinds are deliberately few:
// anything the core can work out for itself - which chat a session belongs to,
// how long it ran, whether it was active - is not an event.
enum class EventKind : uchar {
	// A chat came to the front. Carries the dialog, its kind, the running
	// preset, and whether the preset hides it (which is what makes "time in
	// hidden chats while peeking" answerable).
	Open,

	// That chat went away, with the app still in front.
	Close,

	// A send action: a composer edit (one per burst), a send, voice recording
	// start or stop, an attachment pick, a reply or an edit. `action' names
	// which, for a screen that wants to break it down.
	Action,

	// No input for [screen_time] idle_after. The recorder emits it because it
	// is the only thing that sees touches and scrolls, which are not worth a
	// line each; the threshold is still the core's, and the pause is stamped
	// back to where it really began. See DeriveSessions.
	Idle,

	// Input again after an Idle.
	Resume,

	// The running preset changed. Cuts the session so every second of it has
	// exactly one preset.
	Preset,

	// The app came to the front, and left it. Background ends whatever session
	// was running - a chat you cannot see is not screen time - and covers the
	// screen locking, which reaches the app the same way.
	Foreground,
	Background,

	// A peek started, and the one that was running ended. The pair is what
	// makes "peeked 4 times, 23 min today" answerable, and it is not the same
	// question as the `hidden' flag below: that one is time spent IN chats the
	// preset hides, while this is the peek itself - started, very often, to
	// look at the list rather than to open anything on it.
	//
	// A Peek is written when one starts AND whenever the running one's deadline
	// moves - an extension, a chip tapped again - each carrying the deadline in
	// `action'. That is not a second peek and does not count as one; it is the
	// log keeping an answer to "when was this due to end" that survives the app
	// being killed.
	//
	// Neither cuts a session or touches a total. A peek does not change which
	// chat is in front of you; it changes what the list beside it is willing
	// to show.
	Peek,
	PeekEnd,
};

// "open", "close", "action", "idle", "resume", "preset", "foreground",
// "background", "peek", "peek_end".
[[nodiscard]] std::optional<EventKind> ParseEventKind(const QString &value);
[[nodiscard]] QString EventKindName(EventKind value);

// One line of screentime.log.
struct Event {
	// Milliseconds, unlike everything else in the core, which counts unix
	// seconds. A send and the composer edit before it can land in the same
	// second and their order is what active time is derived from.
	int64 unixMs = 0;

	EventKind kind = EventKind::Open;

	// Zero for an event that is not about a chat - a preset change, going to
	// the background - and for time spent elsewhere.
	PeerIdValue dialogId = 0;

	ScreenTimeKind chatKind = ScreenTimeKind::Elsewhere;

	// The preset running when this happened. Empty is Normal, spelled as the
	// file spells it, so a total can be split by preset without a lookup.
	QString preset;

	// Which action, for an Action, and for a Peek the moment that peek is due
	// to end - unix milliseconds, or empty for one with no clock on it. Empty
	// for every other kind.
	//
	// The deadline rides along here rather than in a field of its own because
	// a line has seven fields and a reader that has to count them is a reader
	// two apps can disagree with. What it is for is DerivePeeks: an app killed
	// while a peek ran leaves a peek with no end in the log, and a peek that
	// cannot outlive its own deadline is one that cannot swallow the hours the
	// app spent dead.
	QString action;

	// Whether the chat an Open names is one the running preset hides - which
	// is to say, whether you reached it by peeking. Meaningless on any other
	// kind, and written as 0 there.
	bool hidden = false;

	friend bool operator==(const Event &, const Event &) = default;
};

// One line, tab-separated:
//
//     unix_ms  kind  dialog_id  chat_kind  preset  action  hidden
//
// Tabs rather than commas because a preset name is whatever the user typed and
// a comma in one is likelier than a tab; tabs in the two free-text fields are
// turned into spaces on the way out, so a line always has exactly seven
// fields. No trailing newline - the caller appends one.
[[nodiscard]] QString FormatEvent(const Event &event);

// Nothing for a line that cannot be read. The caller skips it and carries on:
// the log is append-only and written from several places, so a truncated last
// line after a crash is expected rather than exceptional, and one lost event
// is worth far less than the rest of the history.
[[nodiscard]] std::optional<Event> ParseEventLine(const QString &line);

// The whole file, bad lines dropped in silence. Blank lines too.
[[nodiscard]] std::vector<Event> ParseEventLog(const QString &text);

// One chat in front of you, for as long as it was.
struct Session {
	int64 startMs = 0;
	int64 endMs = 0;

	PeerIdValue dialogId = 0;
	ScreenTimeKind chatKind = ScreenTimeKind::Elsewhere;
	QString preset;

	// Time inside the session that counts as active rather than reading. See
	// DeriveSessions for the rule.
	int64 activeMs = 0;

	// Time inside it the session was paused for: idle, and nothing else.
	int64 idleMs = 0;

	// Whether the chat was hidden by the running preset when it opened - the
	// "time in hidden chats while peeking" number.
	bool hidden = false;

	// What the session is worth. The wall span less the idle in it, which is
	// what makes the totals add up to time actually spent looking.
	[[nodiscard]] int64 totalMs() const {
		const auto span = (endMs > startMs) ? (endMs - startMs) : int64(0);
		return (span > idleMs) ? (span - idleMs) : int64(0);
	}

	friend bool operator==(const Session &, const Session &) = default;
};

// Sessions out of raw events.
//
// A session runs from an Open to whichever comes first of Close, Background,
// the next Open, or the end of the log; a Preset event cuts it and starts an
// identical one carrying the new preset, so every second has exactly one.
//
// `activeMs' is the union of what the actions claim:
//
// - each Action counts for `action_span' from where it lands;
// - when the next Action lands within `active_gap' of it, the whole gap counts
//   as well, so a conversation reads as active time rather than as a row of
//   three-second spikes;
// - a lone Action counts only its span.
//
// It is clipped to the session, because active time longer than the session it
// is inside would be a number nobody could explain: send and close instantly
// and you were active for the moment you were there, not for three seconds.
//
// Idle pauses the session. The pause starts `idle_after' BEFORE the Idle event
// rather than at it, because that is when the input actually stopped - which
// is what keeps the threshold the core's own, and what makes a changed
// idle_after re-derive the same history differently.
//
// Events are read in time order; ties keep the order they were written in,
// which is what puts a Preset after the Action it follows.
[[nodiscard]] std::vector<Session> DeriveSessions(
	const std::vector<Event> &events,
	const ScreenTime &settings);

// A session's share of a window, both clocks scaled by how much of it fell
// inside. Splitting rather than assigning whole to the day it began on: a chat
// left open across midnight belongs to both days, and the hour-of-day view
// would be useless if a two-hour session landed entirely in one hour.
struct Slice {
	int64 totalMs = 0;
	int64 activeMs = 0;
};

[[nodiscard]] Slice SliceOf(const Session &session, int64 fromMs, int64 toMs);

// One chat's share of a range, for the ranked list.
struct ChatTotal {
	PeerIdValue dialogId = 0;
	ScreenTimeKind chatKind = ScreenTimeKind::Elsewhere;
	int64 totalMs = 0;
	int64 activeMs = 0;
};

struct KindTotal {
	ScreenTimeKind chatKind = ScreenTimeKind::Elsewhere;
	int64 totalMs = 0;
	int64 activeMs = 0;
};

struct PresetTotal {
	QString preset;
	int64 totalMs = 0;
	int64 activeMs = 0;
};

struct Totals {
	int64 totalMs = 0;
	int64 activeMs = 0;

	// Time in chats the running preset was hiding - which is to say, time
	// spent peeking. Part of totalMs, not on top of it.
	int64 hiddenMs = 0;

	// Ranked, longest first; ties by dialog id so the order never wobbles
	// between two runs over the same log.
	std::vector<ChatTotal> chats;

	// Ranked the same way. Only the kinds that appear.
	std::vector<KindTotal> kinds;
	std::vector<PresetTotal> presets;
};

// [fromMs, toMs) - half-open, like every window in this core, so two
// neighbouring ranges cannot both claim the moment they meet.
[[nodiscard]] Totals RangeTotals(
	const std::vector<Session> &sessions,
	int64 fromMs,
	int64 toMs);

// One peek, from the moment it started to the moment it ended. The unit behind
// "peeked 4 times, 23 min today", which is the number worth seeing on a screen
// time view: peek is the way out of the preset, so how much it is used is how
// much the preset was not being kept to.
struct PeekRun {
	int64 startMs = 0;
	int64 endMs = 0;

	friend bool operator==(const PeekRun &, const PeekRun &) = default;
};

// Peeks out of raw events, the way sessions are.
//
// A run starts at a Peek and ends at the PeekEnd that follows it. Three rules
// cover everything else, and each of them is a real case rather than a defence:
//
// - a Peek while one is already open is IGNORED, and the run carries on. The
//   recorders write these at a change and once at startup, so a second Peek
//   with no end between them is the app having been restarted while the same
//   peek ran - not a new one. Nothing else can produce it: restarting a peek at
//   a new length from a chip is not a change the recorder can see;
// - a PeekEnd with nothing open is ignored, which is what the tail of a log
//   whose start was pruned away looks like;
// - a run still open at the end of the log ends at the last thing we know
//   happened, or at the deadline its last Peek carried if that came first. An
//   app killed while a peek ran is the ordinary way to get a run with no end,
//   and without the deadline it would swallow every hour between the kill and
//   the next thing the log has to say. A peek with no clock on it has no
//   deadline to be bounded by, which is right: that one really does run until
//   somebody stops it.
[[nodiscard]] std::vector<PeekRun> DerivePeeks(const std::vector<Event> &events);

// How much peek there was in a window: how many and how long.
struct PeekUsage {
	int count = 0;
	int64 totalMs = 0;
};

// [fromMs, toMs), like every window here.
//
// A peek that crosses the edge of the window counts in BOTH windows it touches,
// and contributes to each the part that fell inside - the same way a chat left
// open across midnight belongs to both days. Counting it only where it started
// would have a peek that began at 23:58 read as "0 times, 2 min", which is a
// line nobody would believe.
[[nodiscard]] PeekUsage PeekUsageIn(
	const std::vector<PeekRun> &peeks,
	int64 fromMs,
	int64 toMs);

// What a row of bars is a row of.
enum class BucketUnit : uchar {
	// Twenty-four buckets, every day in the range folded onto one clock. The
	// "reading load" view: where the time goes in a day, which is what a
	// schedule window is placed by.
	HourOfDay,

	// One bucket per calendar day, week (Monday to Monday) or month in the
	// range. Calendar rather than arithmetic, so a week is a week wherever the
	// range happens to start.
	Day,
	Week,
	Month,
};

struct Bucket {
	// Where it sits in the row: the hour for HourOfDay, otherwise the count
	// from the first bucket.
	int index = 0;

	// The bucket's own window. Zero for HourOfDay, which is not a window in
	// time at all - it is every day at once.
	int64 startMs = 0;
	int64 endMs = 0;

	// "13", "2026-09-08", "2026-W37", "2026-09". Short enough for an axis and
	// unambiguous enough for a CSV export.
	QString label;

	int64 totalMs = 0;
	int64 activeMs = 0;
};

// Every bucket in [fromMs, toMs), including the empty ones - a day with no
// screen time is a bar of height zero and not a missing bar.
//
// `zone' rather than the system's, because a day boundary is a local-time
// question and a core that reached for the machine's clock could not be tested
// on two machines with the same answer.
[[nodiscard]] std::vector<Bucket> Buckets(
	const std::vector<Session> &sessions,
	int64 fromMs,
	int64 toMs,
	BucketUnit unit,
	const QTimeZone &zone);

// Hour by weekday, for the month view. Rows are Qt::Monday .. Qt::Sunday, 1..7,
// the same numbering the schedule's `days' uses.
struct HeatMap {
	std::array<std::array<int64, 24>, 7> totalMs = {};
	std::array<std::array<int64, 24>, 7> activeMs = {};

	[[nodiscard]] int64 total(int weekday, int hour) const;
	[[nodiscard]] int64 active(int weekday, int hour) const;
};

[[nodiscard]] HeatMap HeatMapFor(
	const std::vector<Session> &sessions,
	int64 fromMs,
	int64 toMs,
	const QTimeZone &zone);

// This range against the one of the same length immediately before it, which
// is what "up 12% on last week" means and the only comparison that does not
// need a calendar to explain.
struct Comparison {
	Totals current;
	Totals previous;

	// current.totalMs - previous.totalMs, and the same as a percentage of the
	// previous. Nothing for a previous range with no time in it: there is no
	// percentage change from zero, and any number shown there would be made
	// up.
	int64 deltaMs = 0;
	std::optional<int> changePercent;
};

[[nodiscard]] Comparison Compare(
	const std::vector<Session> &sessions,
	int64 fromMs,
	int64 toMs);

// What one budget has spent today.
struct BudgetSpent {
	// Which budget, by position in [screen_time] budgets, so a screen can name
	// it back exactly as the file wrote it.
	int index = 0;

	int64 spentMs = 0;
	int64 perDayMs = 0;

	// Whether the allowance is gone. A soft budget shows a bulletin at this
	// point and a hard one puts the cover up; neither touches messages or
	// notifications.
	bool reached = false;
};

// The day's ledger, derived from the raw events like everything else - so a
// changed threshold applies to today's total, not only to tomorrow's.
//
// `settings' carries both the thresholds the sessions are derived through and
// the budgets themselves, which is why it is one argument rather than two:
// they have to be the same [screen_time] or the ledger would count today with
// one set of rules and judge it by another.
[[nodiscard]] std::vector<BudgetSpent> BudgetLedger(
	const std::vector<Event> &events,
	const ScreenTime &settings,
	const QDate &day,
	const QTimeZone &zone);

// Whether the cover over a spent hard budget may be snoozed away once more.
// False for a soft budget, which never puts one up; false once the day's
// snoozes are used, and false for a budget that offers none.
[[nodiscard]] bool CoverAllowed(
	int snoozesUsed,
	const ScreenTimeBudget &budget);

// The log with everything older than `retentionDays' dropped, in the order it
// was in. Pure rather than in place because the caller's next move is to
// rewrite the file from what comes back.
//
// A `retentionDays' of zero keeps everything, matching every other zero in
// [screen_time].
[[nodiscard]] std::vector<Event> Prune(
	const std::vector<Event> &events,
	int64 nowMs,
	int retentionDays);

// A span of screen time in the shortest words that stay exact: "1 d 3 h 2 m",
// "1 d 2 m", "1 h", "59 m", "48 s", "0 s".
//
// Here rather than in either app because both had written it themselves and
// the two had already drifted - one said "0" for an empty span, the other
// "0 s" - and because neither had a day unit, so a phone left alone over a
// weekend reported "51 h 2 m" and left the reader to divide.
//
// The rules, and why:
//
// - Days, hours, minutes. Every nonzero unit is written and every zero one is
//   left out, so "1 d 2 m" is a whole answer rather than something that has to
//   be padded to "1 d 0 h 2 m". Dropping a unit is never ambiguous because the
//   unit is always spelled beside its number.
// - Seconds only below a minute. Once there is a minute to report the seconds
//   are noise on a number nobody compares that closely: "59 m", not
//   "59 m 59 s".
// - Zero, a negative span and anything under a second are all "0 s". A
//   negative span is a clock that moved rather than a negative amount of time,
//   and "-3 m" would only invite somebody to explain it.
// - Never a bare number of milliseconds, which is the failure this replaces.
//
// The words are English and unlocalised, a deliberate exception to the rule
// that wording stays out of the core: these are unit letters beside a number
// in a chart, both apps already shipped exactly these letters, and handing
// back a struct of numbers instead would only have moved the drift somewhere
// the tests cannot see it.
[[nodiscard]] QString FormatSpan(int64 ms);

} // namespace Purple
