/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_settings.h"
#include "purple/purple_types.h"

#include <QtCore/QByteArray>

#include <optional>
#include <vector>

// state.toml, the machine-owned half of the configuration. Everything here is
// rewritten whenever it changes and carries no comments, which is exactly why
// it is a separate file: state churns constantly, and it must never touch the
// mtime of the settings.toml the user is editing by hand.
namespace Purple {

// What put the current preset in place. The distinction is load-bearing rather
// than informational: a schedule boundary aiming at Normal is skipped when the
// user chose the current preset themselves. See spec 6.2.
enum class PresetSource : uchar {
	Manual,
	Schedule,
	Focus,
};

[[nodiscard]] QString PresetSourceName(PresetSource source);
[[nodiscard]] PresetSource PresetSourceFromName(const QString &name);

// What ended the last peek. Kept in state because the thing that ends a peek
// and the thing that can say so are not always the same moment: a peek ended by
// the screen locking is ended by a machine nobody is looking at, and the
// sentence belongs on the screen that comes back.
//
// A peek that simply ran out is not here. Nothing ends it - the deadline is in
// the past by the time anything reads it again - so there is no moment at which
// a reason could be written, and inventing one would mean a timer whose only
// job was to write a word into a file.
enum class PeekEnd : uchar {
	// Nothing has ended a peek since the last one started, including while one
	// is running. Starting a peek clears whatever the last one left here.
	None,

	// The user turned it off: the control, the checkbox, the hotkey - the
	// hotkey pressed at the cap included, which is a press that means "enough".
	Manual,

	// The OS session or screen lock, and the app's own passcode lock. Only ever
	// written where `[peek] end_on_screen_lock_p' and its two siblings say the
	// lock ends a peek at all; see PeekEndsOnLock() in purple_engine.h.
	ScreenLock,
	AppLock,
};

// "manual", "screen_lock", "app_lock", and empty for None - which is how it is
// left out of the file rather than written as a word meaning nothing.
[[nodiscard]] QString PeekEndName(PeekEnd reason);
[[nodiscard]] PeekEnd PeekEndFromName(const QString &name);

struct ResolvedList {
	QString list;

	// Unset is meaningful and has to survive the round trip: it is what makes
	// the entry take the default for whatever kind the chat turns out to be.
	std::optional<ShowMode> show;

	bool notify = true;

	// Unset here too, and for the same reason: it is what leaves this entry's
	// people to the preset's own stories policy.
	std::optional<StoryMode> stories;
};

struct ResolvedCacheView {
	QString name;
	std::vector<PeerIdValue> pinned;
	std::vector<ResolvedList> lists;
};

// The last resolution that worked. When a reload leaves the active preset
// unresolvable - deleted or renamed mid-edit - the engine keeps running on this
// rather than falling back to showing everything. See spec 8.5.
struct ResolvedCache {
	QString preset;

	// Cached alongside the rest so a broken reload does not also rename the
	// tab. Absent in a file written by an older build, which is why reading it
	// falls back to the preset name rather than to an empty label.
	QString viewName;

	std::vector<ResolvedList> lists;
	bool hideEverywhere = false;

	// True in a file written by an older build, which is also what a preset
	// saying nothing asks for - so an upgrade changes nothing here.
	bool hideArchive = true;

	// Same shape and the same reason: true in a file written by an older build,
	// which is what a preset saying nothing asks for anyway.
	bool hideAddStory = true;

	// The main view's own pinned order. Empty means the preset mirrors the
	// account's, which is the default and what an older file restores as.
	std::vector<PeerIdValue> pinned;

	// What the stories strip does. Follow in a file written by an older build,
	// which is also the default, so an upgrade changes nothing here.
	StoryPolicy stories = StoryPolicy::Follow;

	// Empty means no folder tabs, which is what a preset naming no folders
	// asks for - there is no "said nothing" case left to distinguish.
	std::vector<PresetFolder> folders;

	// Cached too, so a settings.toml broken mid-edit does not take the extra
	// tabs away along with everything else it cannot read.
	std::vector<ResolvedCacheView> views;

	[[nodiscard]] bool valid() const {
		return !preset.isEmpty();
	}
};

// A decision about one chat that outranks the preset, until a deadline.
//
// Scoped to the preset it was made under rather than global: "show this until
// six" is a statement about the work you are doing now, and carrying it into
// every other preset would make it a different, larger promise than the one
// the menu offered.
enum class OverrideKind : uchar {
	Show,   // In the view past the preset's rules. Does not un-silence.
	Hide,   // Out of the view, and silenced with it.
	Notify, // May interrupt, lifting only the preset's mute - never your own.
};

// "show", "hide", "notify".
[[nodiscard]] std::optional<OverrideKind> ParseOverrideKind(
	const QString &value);
[[nodiscard]] QString OverrideKindName(OverrideKind value);

struct Override {
	PeerIdValue peer = 0;
	OverrideKind kind = OverrideKind::Show;

	// Unix seconds, like peekDeadlineUnix and for the same reason: crl::now()
	// is monotonic and would mean nothing after a restart, and an override is
	// measured in hours.
	int64 untilUnix = 0;

	// When it was made. Only the ring in the chat list needs this - it is what
	// makes the arc proportional rather than guessed - but the file reads
	// better for having both ends of the span written down.
	int64 startedUnix = 0;

	QString preset;

	friend bool operator==(const Override &, const Override &) = default;
};

// One completed last-seen trade: we showed this person our last seen for a few
// seconds, read theirs, and put our privacy rules back.
//
// Remembered so the status line can say "last seen 14:32, as of 3 min ago"
// instead of falling straight back to "last seen recently" the moment the
// trade ends - and so a second trade with the same person is not offered
// again a minute later.
struct LastSeenTrade {
	PeerIdValue peer = 0;

	// When we read it, in unix seconds. This is the "as of" - the age of the
	// news, which is what decides whether it is still worth showing.
	int64 readAtUnix = 0;

	// The moment we read, in unix seconds: their real `was_online'. Zero for a
	// trade that ran its hold out without an exact status ever arriving, which
	// is still worth remembering - it is what the cooldown counts.
	int64 wasOnlineUnix = 0;

	friend bool operator==(const LastSeenTrade &, const LastSeenTrade &)
		= default;
};

struct State {
	QString activePreset;
	PresetSource activeSource = PresetSource::Manual;

	// Remembered when OS focus takes over, so exiting focus can put back both
	// the preset and the reason it was active.
	QString previousPreset;
	PresetSource previousSource = PresetSource::Manual;

	// Whether the OS says a focus mode is on. Written by whatever is watching
	// for that, which is deliberately not the same thing as what acts on it:
	// the policy below is worth proving, and the detection is a moving target
	// that Apple owns. See docs/purple/work_mode.md.
	bool focusActive = false;

	// The last value of focusActive that was acted on, so focus sync moves on
	// a change rather than on a value - the same shape as scheduleTarget, and
	// for the same reason. A preset chosen by hand in the middle of a focus
	// session stands, because nothing fires again until focus itself changes.
	bool focusSeen = false;

	bool schedulePaused = false;

	// When the pause runs out, in unix seconds, or 0 for a pause that lasts
	// until it is lifted by hand - which is what a pause has always been, and
	// what an older state.toml with no such key still says.
	//
	// Unix seconds rather than a monotonic deadline, for the same reason as
	// peekDeadlineUnix: a pause is measured in hours or days, so it has to mean
	// the same thing after the app has been closed and reopened.
	int64 schedulePausedUntil = 0;

	// The last preset the schedule computed, so it can act on a change rather
	// than on every tick. That is what lets a preset chosen by hand survive
	// until the next boundary instead of being overwritten a second later, and
	// what lets a boundary missed while the app was closed still be caught up
	// on the next launch. Empty means it has never run.
	QString scheduleTarget;

	bool peekActive = false;
	int64 peekDeadlineUnix = 0;

	// What ended the last peek, so the app can say so when it is next looked
	// at. Cleared by the next peek starting.
	PeekEnd peekEnded = PeekEnd::None;

	// Live "until" decisions, in the order they were made. Small by nature -
	// each one is a thing you did on purpose and it expires by itself.
	std::vector<Override> overrides;

	// The settings.toml this device last sent to Saved Messages, and the one
	// it last wrote after an import, as SettingsFingerprint() spells them.
	// Empty means it has never done that - which is what an older state.toml
	// says too, so the first auto-send after an upgrade happens normally.
	//
	// Two fingerprints rather than one because the two events are different
	// claims. "I sent these bytes" stops this device from sending the same
	// file twice; "I wrote these bytes because the other device sent them"
	// stops it from sending them straight back, which is the ping-pong: A
	// saves and sends, B imports and saves, B sends what it just received, A
	// imports it, and the file bounces between two machines that agree.
	QString lastSentFingerprint;
	QString lastImportedFingerprint;

	// The last seens read by trading, newest write per person. At most one
	// record per peer - a second trade with the same person replaces the first
	// rather than piling up - so this is bounded by the number of people you
	// have ever traded with rather than by how often.
	std::vector<LastSeenTrade> lastSeenTrades;

	ResolvedCache resolvedCache;
};

// What a settings.toml is, as far as the auto-send rule cares: "<length>:<hex>"
// where hex is the SHA-256 of the bytes. The same shape the Android watcher
// writes, so the two halves of a sync can compare notes about the same file.
//
// The length is in there because it is free and it makes the string readable by
// a person staring at a state.toml wondering why nothing sent - a bare hash
// tells you two files differ, and this at least tells you how.
[[nodiscard]] QString SettingsFingerprint(const QByteArray &bytes);

// Whether saving these bytes should also send them to Saved Messages.
//
// Pure, and pure on purpose: this is the whole of the ping-pong rule, and the
// app around it - which has a network, a Saved Messages history and a file
// watcher - is exactly the place that rule is hardest to prove anything about.
//
// `wroteFromImport' is the caller saying the save it is asking about is the
// import itself writing the file it just received. That never sends, even the
// first time, before any fingerprint has been recorded: the bytes came from the
// other device, and returning them is the thing this is here to stop.
[[nodiscard]] bool ShouldAutoSend(
	const Settings &settings,
	const State &state,
	const QByteArray &bytes,
	bool wroteFromImport);

// Whether the peek recorded in state is still running. A deadline of zero means
// auto_off is turned off, so the peek runs until it is turned off by hand -
// which is the only reason the flag is persisted rather than kept in memory.
// The comparison is what makes a peek that outlived the app expire on its own:
// the deadline is in the past by the time anything reads it again.
[[nodiscard]] bool PeekLive(const State &state, int64 nowUnix);

// The longest a peek can be stretched to by extending it, measured from the
// moment of the extension. An hour, which is the last detent: past that it is
// not a peek any more, it is the preset off, and the app has a plainer way to
// say that than tapping "+5 min" twelve times.
inline constexpr auto kPeekExtendCapSeconds = 60 * 60;

// How near the cap counts as being at it. A peek started AT the cap never reads
// as exactly the cap again: the deadline stands still while `now' walks towards
// it, so a peek of a full hour has an hour less a second or two left for as
// long as it runs, and a rule waiting for the deadline to sit exactly at
// `now + cap' would never once fire. "Already as long as a peek gets" has to
// mean "within reach of the cap" instead, and half a minute is wide enough for
// the hand that moved from the chip to the keyboard while staying narrower than
// the shortest length the control offers - so no press that still had a whole
// minute to buy is ever turned into an ending.
inline constexpr auto kPeekAtCapSlackSeconds = 30;

// Starts a peek of `seconds', or of no fixed length when `seconds' is zero.
// True when something actually moved, so a caller knows whether it owes a
// write and a rebuild.
//
// The two apps each carried these two lines themselves, next to their own
// "refused under Normal" check. That check stays with them - it needs the
// resolution, which is the engine's and not the state's - but the mutation is
// the same on both sides and belongs where it can be proved.
[[nodiscard]] bool StartPeek(State &state, int64 nowUnix, int seconds);

// Adds `addSeconds' to a running peek, measured from the deadline it already
// has rather than from now: the button says "+5 min" and a user pressing it
// twice quickly means ten minutes, not five and a bit.
//
// The remaining time is capped at `capSeconds' from now - pass
// kPeekExtendCapSeconds unless there is a reason not to; a cap of zero or less
// is no cap at all. False, and nothing touched, when there is nothing to
// extend: no peek, an expired one, a peek with no deadline (it is already
// longer than any extension could make it), a non-positive `addSeconds', or a
// peek already within kPeekAtCapSlackSeconds of the cap.
//
// That last one is the caller's cue to END the peek rather than to do nothing -
// it is the documented "Peek over - it was already as long as a peek gets" -
// which is why it is a refusal and not a clamp that quietly buys two seconds.
[[nodiscard]] bool ExtendPeek(
	State &state,
	int64 nowUnix,
	int addSeconds,
	int capSeconds);

// Ends one. Clears the deadline with the flag, so a peek started again later
// cannot inherit a stale one, and notes what ended it.
//
// The reason defaults to Manual because that is what every gesture that reaches
// here is: a control, a checkbox, a hotkey. A lock goes through
// EndPeekForLock() in purple_engine.h, which is the one place that knows
// whether this device's file lets a lock end a peek at all.
void StopPeek(State &state, PeekEnd reason = PeekEnd::Manual);

// How long a running peek has left, in seconds. Zero when nothing is peeking
// AND when a peek is running with no deadline on it, which is why
// PeekUntilStopped() exists beside this rather than a -1 in here: this number
// is fed straight to a countdown, and a sentinel that formats as "-0:01" the
// one time a caller forgets to check is a worse failure than a caller having
// to ask a second question.
[[nodiscard]] int PeekLeftSeconds(const State &state, int64 nowUnix);

// Whether the running peek is one with no clock on it - `auto_off = "off"', or
// a length of zero from any of the other keys. The other half of the answer
// above.
[[nodiscard]] bool PeekUntilStopped(const State &state);

// Whether a pause that was given a deadline has reached it. False for a pause
// with no deadline - 0 means "until I say otherwise" - and false when nothing
// is paused at all.
//
// The comparison is what makes a pause that ran out while the app was closed
// expire on its own: by the time anything reads it again the deadline is in the
// past. A caller that sees true clears BOTH fields and then runs the ordinary
// boundary rule (ScheduleTarget + ScheduleApplies) in the same tick, so the
// windows missed while paused are caught up on once, immediately, rather than
// waiting for the next window edge.
[[nodiscard]] bool ScheduleUnpauseDue(const State &state, int64 nowUnix);

// The name that means "behave exactly like stock Telegram Desktop".
[[nodiscard]] const QString &NormalPreset();

[[nodiscard]] State ParseState(const QString &text, const QString &path);
[[nodiscard]] QString SerializeState(const State &state);

// The override in force for this chat right now, or null. Expired entries and
// entries made under a different preset are both invisible here, which is what
// makes every caller a single test rather than three.
[[nodiscard]] const Override *OverrideFor(
	const State &state,
	PeerIdValue peer,
	const QString &preset,
	int64 nowUnix);

// Writes down a trade, replacing whatever was remembered for that person. One
// record per peer, because the only questions asked of this are "what did I
// read last" and "how long ago" - a history of every trade would answer
// neither and grow without end.
void RememberTrade(
	State &state,
	PeerIdValue peer,
	int64 readAtUnix,
	int64 wasOnlineUnix);

// What was read for this person, if it is still worth showing: nothing once
// the read is older than `rememberSeconds'. Staleness is decided here rather
// than when the record is written, so changing `trade_remember' changes what
// the status lines say without touching the file.
//
// A `rememberSeconds' of zero remembers nothing, matching every other duration
// key in the file where zero is off.
[[nodiscard]] std::optional<LastSeenTrade> RememberedTrade(
	const State &state,
	PeerIdValue peer,
	int64 nowUnix,
	int rememberSeconds);

// How long until a trade with this person would be allowed again, in seconds,
// or 0 when one is allowed right now. What the sheet shows as "you can refresh
// in 3:12", which is the whole reason it exists: TradeAllowed answers the
// yes/no, a countdown needs the number behind it, and computing that number
// twice in two apps is how the two would come to disagree about the boundary.
//
// It counts from the last trade's `readAtUnix' whatever that trade read, so a
// trade whose hold ran out with no exact status still holds the cooldown open -
// the cooldown counts attempts, not answers. A read stamped in the future is a
// clock that moved backwards rather than a cooldown that lasts forever, so it
// reads as zero.
[[nodiscard]] int TradeCooldownLeft(
	const State &state,
	PeerIdValue peer,
	int64 nowUnix,
	int cooldownSeconds);

// Whether a trade with this person may be offered now. False for the
// `cooldownSeconds' after the last one: a trade is a moment of exposure chosen
// on purpose, and one offered again every time their chat opens would turn it
// into a standing subscription nobody agreed to.
//
// Exactly "TradeCooldownLeft says zero", and written in terms of it rather than
// beside it, so the yes/no and the countdown cannot come to disagree about the
// instant the cooldown ends.
[[nodiscard]] bool TradeAllowed(
	const State &state,
	PeerIdValue peer,
	int64 nowUnix,
	int cooldownSeconds);

// What the fork does to one person's status line. Three answers and no fourth,
// in this order of precedence.
//
// Not a persisted type, unlike everything above it in this file: it is a
// question asked OF the state rather than a part of it. It lives here anyway,
// with the memory it reads, because the answer needs both the settings and the
// remembered trades - and the trades are here, while purple_settings.h cannot
// see them without the include arrow between the two files turning into a
// cycle.
enum class LastSeenLine : uchar {
	// Left exactly as the app wrote it. Hidden by them, an exact time, "a long
	// time ago" with no read to put over it, or the explanations turned off:
	// nothing true to add.
	Plain,

	// The coarse text plus the offer, after the usual middle dot. Only ever
	// this when the server said the coarsening is OUR privacy rules' doing,
	// which is the one case with something to do about it.
	ByMeTail,

	// Replaced outright by what a trade actually read: "last seen 14:32 · as of
	// 3 min ago". A real moment, read on purpose, and saying "last seen
	// recently" over the top of it would throw away the thing the trade was
	// for.
	Remembered,
};

// The decision, with the numbers a client needs to write the sentence. The
// WORDS are deliberately not here: each app has its own string catalogue - the
// desktop's literals, Android's strings.xml - and a core that shipped English
// would leave one of them translating around it.
struct LastSeenNote {
	LastSeenLine line = LastSeenLine::Plain;

	// Whether tapping the line opens the trade sheet. See LastSeenNoteNow for
	// which line a tap belongs to, and why.
	bool tappable = false;

	// Remembered only: their real `was_online', and when we read it. Both
	// halves are needed - the time is what the trade bought, the age is what
	// stops it reading as live.
	int64 wasOnlineUnix = 0;
	int64 readAtUnix = 0;

	// Seconds until a fresh trade with this person would be allowed, 0 when one
	// is allowed now. Filled in whatever the line is, because it is the sheet's
	// countdown rather than the line's, and the sheet is reachable from a line
	// that says nothing about it.
	int cooldownLeftSeconds = 0;
};

// The whole three-way decision, in the core, for both apps.
//
// `shape' is the caller saying which of the three kinds of status this is, and
// `reason' is what ReasonFor() made of it. Both come from the client because a
// status is a client type; everything after that is a rule about data, and
// rules about data live here. Until now the two apps each carried their own
// copy of the ordering below, and they had already drifted on the last line of
// it.
//
// The order is precedence, and each step is its own decision:
//
// - Plain immediately for an Exact status. The app has the real moment; a read
//   from some hours ago put over the top of it would be older news dressed as
//   newer, and this is the one case where the server has already answered the
//   question the trade was asked to answer.
// - Remembered otherwise, whenever a trade is still remembered and read a real
//   `was_online'. NOT gated on `reasons_p': this is not the fork explaining a
//   status, it is the fork showing what a trade the user asked for came back
//   with, and turning the explanations off should not hide the answer to a
//   question they asked out loud.
// - ByMeTail otherwise, when the status is Coarse because of our own rules and
//   both `trade_p' and `reasons_p' are on. That IS the fork's Peek affordance,
//   so both switches govern it.
// - Plain otherwise.
//
// What gates the remembered line is the AGE OF THE MEMORY - is it still within
// `trade_remember' - and not the shape of the status sitting under it. A read
// does not stop being a real moment that was really read because their status
// has since gone to "a long time ago"; if anything that is when it is worth
// the most, because it is now the only moment anyone has. Only an Exact status
// takes the memory's place, and it takes it by being newer rather than by
// being a different shape.
//
// "A long time ago" is still never EXPLAINED, though: it gets the remembered
// line or nothing, never a tail. The server is not withholding a moment there,
// so there is no coarsening for the fork to attribute to anybody.
//
// `tappable' needs `trade_p' and a line worth tapping:
//
// - the ByMe tail, which is drawn only when `reasons_p' is on, so its tap is
//   gated by `reasons_p' through the line rather than by a second test;
// - the remembered line, which is drawn REGARDLESS of `reasons_p', so its tap
//   works regardless too. A dead tap on a line the user can plainly see would
//   be that switch reaching somewhere it was never about.
//
// The remembered line being tappable at all is the fix for a real trap: the
// tail used to be the only way into the sheet, so the first trade replaced the
// only door and a second one was unreachable for a whole `trade_remember' - a
// day, by default. It now opens the same sheet, which shows
// `cooldownLeftSeconds' and offers the re-trade once that is spent. A
// remembered line whose reason is no longer ByMe is not tappable: they have
// changed their own privacy since, and there is nothing left to trade for.
// That covers a remembered line over "a long time ago" without needing a rule
// of its own - such a status has no reason at all - and the answer is the
// right one for it: they have gone quiet or shut us out, and neither is
// something the other half of a trade can buy back.
[[nodiscard]] LastSeenNote LastSeenNoteNow(
	const Settings &settings,
	const State &state,
	PeerIdValue peer,
	LastSeenReason reason,
	LastSeenShape shape,
	int64 nowUnix);

// Drops the trades that have gone stale. The serialiser cannot do this on its
// own - it has neither a clock nor the settings that say how long a read stays
// worth showing - so a caller with both runs it before writing. True if
// anything went, on the same terms as PruneOverrides.
bool PruneLastSeenTrades(State &state, int64 nowUnix, int rememberSeconds);

// Drops whatever has run out. True if anything went, so the caller knows
// whether a rebuild is owed.
bool PruneOverrides(State &state, int64 nowUnix);

// The earliest deadline still outstanding under this preset, or 0. What the
// timer is armed for.
[[nodiscard]] int64 NextOverrideDeadline(
	const State &state,
	const QString &preset);

} // namespace Purple
