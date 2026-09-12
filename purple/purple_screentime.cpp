/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_screentime.h"

#include "purple/purple_state.h"

#include <QtCore/QDateTime>
#include <QtCore/QStringList>

#include <algorithm>
#include <map>

namespace Purple {
namespace {

constexpr auto kMsInSecond = int64(1000);
constexpr auto kFieldCount = 7;

// The two free-text fields are whatever the user typed, so they are flattened
// on the way out rather than escaped: a preset name with a tab in it is not a
// thing anybody meant, and a line that cannot be split on tabs is a line that
// takes the rest of the history with it.
[[nodiscard]] QString Flattened(const QString &value) {
	auto result = value;
	result.replace(QChar('\t'), QChar(' '));
	result.replace(QChar('\n'), QChar(' '));
	result.replace(QChar('\r'), QChar(' '));
	return result;
}

[[nodiscard]] int64 Milliseconds(int seconds) {
	return (seconds > 0) ? (int64(seconds) * kMsInSecond) : int64(0);
}

// The active-time rule, over one session's actions. Kept apart from the walk
// below because it is the one piece of arithmetic here worth reading on its
// own: everything else is bookkeeping about which events belong together.
[[nodiscard]] int64 ActiveMilliseconds(
		const std::vector<int64> &actions,
		int64 spanMs,
		int64 gapMs,
		int64 startMs,
		int64 endMs) {
	auto total = int64(0);

	// The end of the last stretch already counted, so two actions whose spans
	// overlap are one stretch rather than two - the union, not the sum.
	auto coveredTo = startMs;
	for (auto i = std::size_t(0); i != actions.size(); ++i) {
		auto from = actions[i];
		auto to = from + spanMs;
		if (i + 1 != actions.size() && actions[i + 1] - from <= gapMs) {
			// The next action landed inside the gap, so the whole way to it
			// counts: this is what makes a conversation one long stretch of
			// active time instead of a row of spikes.
			to = std::max(to, actions[i + 1]);
		}
		from = std::clamp(from, startMs, endMs);
		to = std::clamp(to, startMs, endMs);
		from = std::max(from, coveredTo);
		if (to > from) {
			total += to - from;
			coveredTo = to;
		}
	}
	return total;
}

[[nodiscard]] QString EffectivePreset(const QString &preset) {
	// An empty preset in the log is Normal: the recorder writes what is
	// running, and nothing is running under Normal.
	return preset.isEmpty() ? NormalPreset() : preset;
}

[[nodiscard]] bool BudgetCovers(
		const ScreenTimeBudget &budget,
		const Session &session) {
	switch (budget.kind) {
	case BudgetTarget::All:
		return true;
	case BudgetTarget::Chat:
		return (session.dialogId == budget.chat);
	case BudgetTarget::Kind:
		return (session.chatKind == budget.chatKind);
	case BudgetTarget::Preset:
		return !EffectivePreset(session.preset).compare(
			EffectivePreset(budget.preset),
			Qt::CaseInsensitive);
	}
	return false;
}

[[nodiscard]] QDateTime LocalTime(int64 ms, const QTimeZone &zone) {
	return QDateTime::fromMSecsSinceEpoch(ms, zone);
}

[[nodiscard]] int64 MsAt(
		const QDate &date,
		const QTime &time,
		const QTimeZone &zone) {
	return QDateTime(date, time, zone).toMSecsSinceEpoch();
}

// One stretch of wall time and the bucket it belongs to. Buckets are built out
// of these rather than the other way round, because HourOfDay folds many
// windows onto one bucket and the calendar units map one to one.
struct Window {
	int64 startMs = 0;
	int64 endMs = 0;
	int index = 0;
};

[[nodiscard]] QString WeekLabel(const QDate &date) {
	auto year = 0;
	const auto week = date.weekNumber(&year);
	return u"%1-W%2"_q
		.arg(year, 4, 10, QChar('0'))
		.arg(week, 2, 10, QChar('0'));
}

// The calendar unit `at' falls in, as a local date the unit starts on.
[[nodiscard]] QDate UnitStart(const QDate &date, BucketUnit unit) {
	switch (unit) {
	case BucketUnit::Week:
		// Monday, matching the schedule's `days' - a week that started on
		// whatever day the range did would make two ranges incomparable.
		return date.addDays(1 - date.dayOfWeek());
	case BucketUnit::Month:
		return QDate(date.year(), date.month(), 1);
	default:
		return date;
	}
}

[[nodiscard]] QDate UnitNext(const QDate &start, BucketUnit unit) {
	switch (unit) {
	case BucketUnit::Week: return start.addDays(7);
	case BucketUnit::Month: return start.addMonths(1);
	default: return start.addDays(1);
	}
}

[[nodiscard]] QString UnitLabel(const QDate &start, BucketUnit unit) {
	switch (unit) {
	case BucketUnit::Week: return WeekLabel(start);
	case BucketUnit::Month: return start.toString(u"yyyy-MM"_q);
	default: return start.toString(u"yyyy-MM-dd"_q);
	}
}

// Every whole local hour touching [fromMs, toMs). Used by both the hour-of-day
// row and the heat map, so the two can never disagree about which hour a
// minute belongs to.
[[nodiscard]] std::vector<Window> HourWindows(
		int64 fromMs,
		int64 toMs,
		const QTimeZone &zone) {
	auto result = std::vector<Window>();
	if (toMs <= fromMs) {
		return result;
	}
	auto cursor = LocalTime(fromMs, zone);
	cursor.setTime(QTime(cursor.time().hour(), 0));
	auto startMs = cursor.toMSecsSinceEpoch();
	while (startMs < toMs) {
		const auto next = cursor.addSecs(60 * 60);
		const auto endMs = next.toMSecsSinceEpoch();
		if (endMs <= startMs) {
			break; // A zone doing something we did not expect. Stop rather
			       // than spin.
		}
		result.push_back({ startMs, endMs, cursor.time().hour() });
		cursor = next;
		startMs = endMs;
	}
	return result;
}

} // namespace

std::optional<EventKind> ParseEventKind(const QString &value) {
	const auto trimmed = value.trimmed().toLower();
	if (trimmed == u"open"_q) {
		return EventKind::Open;
	} else if (trimmed == u"close"_q) {
		return EventKind::Close;
	} else if (trimmed == u"action"_q) {
		return EventKind::Action;
	} else if (trimmed == u"idle"_q) {
		return EventKind::Idle;
	} else if (trimmed == u"resume"_q) {
		return EventKind::Resume;
	} else if (trimmed == u"preset"_q) {
		return EventKind::Preset;
	} else if (trimmed == u"foreground"_q) {
		return EventKind::Foreground;
	} else if (trimmed == u"background"_q) {
		return EventKind::Background;
	} else if (trimmed == u"peek"_q) {
		return EventKind::Peek;
	} else if (trimmed == u"peek_end"_q) {
		return EventKind::PeekEnd;
	}
	return std::nullopt;
}

QString EventKindName(EventKind value) {
	switch (value) {
	case EventKind::Open: return u"open"_q;
	case EventKind::Close: return u"close"_q;
	case EventKind::Action: return u"action"_q;
	case EventKind::Idle: return u"idle"_q;
	case EventKind::Resume: return u"resume"_q;
	case EventKind::Preset: return u"preset"_q;
	case EventKind::Foreground: return u"foreground"_q;
	case EventKind::Background: return u"background"_q;
	case EventKind::Peek: return u"peek"_q;
	case EventKind::PeekEnd: return u"peek_end"_q;
	}
	return QString();
}

QString FormatEvent(const Event &event) {
	return u"%1\t%2\t%3\t%4\t%5\t%6\t%7"_q
		.arg(QString::number(event.unixMs))
		.arg(EventKindName(event.kind))
		.arg(QString::number(event.dialogId))
		.arg(ScreenTimeKindName(event.chatKind))
		.arg(Flattened(event.preset))
		.arg(Flattened(event.action))
		.arg(event.hidden ? u"1"_q : u"0"_q);
}

std::optional<Event> ParseEventLine(const QString &line) {
	// Only the line ending is stripped, never surrounding whitespace: the last
	// two fields are free text and may legitimately end in a space - a tab in
	// a preset name is flattened to one on the way out - and trimming the line
	// would swallow a trailing empty field along with it.
	auto trimmed = line;
	while (!trimmed.isEmpty()
		&& (trimmed.back() == QChar('\n') || trimmed.back() == QChar('\r'))) {
		trimmed.chop(1);
	}
	if (trimmed.trimmed().isEmpty()) {
		return std::nullopt;
	}
	const auto fields = trimmed.split(QChar('\t'));

	// Six is the shape this had before `hidden' was added, and it still reads:
	// a log written by an older build is history, and history is the whole
	// point of keeping raw events.
	if (fields.size() < kFieldCount - 1) {
		return std::nullopt;
	}
	auto event = Event();
	auto ok = false;
	event.unixMs = fields[0].trimmed().toLongLong(&ok);
	if (!ok || event.unixMs <= 0) {
		return std::nullopt;
	}
	const auto kind = ParseEventKind(fields[1]);
	if (!kind) {
		return std::nullopt;
	}
	event.kind = *kind;
	event.dialogId = fields[2].trimmed().toLongLong();
	event.chatKind = ParseScreenTimeKind(fields[3])
		.value_or(ScreenTimeKind::Elsewhere);
	event.preset = fields[4];
	event.action = fields[5];
	if (fields.size() >= kFieldCount) {
		event.hidden = (fields[6].trimmed() == u"1"_q);
	}
	return event;
}

std::vector<Event> ParseEventLog(const QString &text) {
	auto result = std::vector<Event>();
	const auto lines = text.split(QChar('\n'));
	result.reserve(lines.size());
	for (const auto &line : lines) {
		if (const auto event = ParseEventLine(line)) {
			result.push_back(*event);
		}
	}
	return result;
}

std::vector<Session> DeriveSessions(
		const std::vector<Event> &events,
		const ScreenTime &settings) {
	auto result = std::vector<Session>();
	if (events.empty()) {
		return result;
	}
	auto ordered = events;

	// Stable, so events sharing a millisecond keep the order they were written
	// in - which is what puts a Preset event after the Action it followed.
	std::stable_sort(ordered.begin(), ordered.end(), [](
			const Event &a,
			const Event &b) {
		return a.unixMs < b.unixMs;
	});

	const auto spanMs = Milliseconds(settings.actionSpanSeconds);
	const auto gapMs = Milliseconds(settings.activeGapSeconds);
	const auto idleAfterMs = Milliseconds(settings.idleAfterSeconds);

	auto open = std::optional<Session>();
	auto actions = std::vector<int64>();
	auto idleFrom = int64(0); // Zero means "not idle".
	auto idleTotal = int64(0);

	const auto finish = [&](int64 at) {
		if (!open) {
			return;
		}
		auto session = *open;
		session.endMs = std::max(at, session.startMs);
		if (idleFrom) {
			// An idle nobody resumed from runs to the end of the session.
			idleTotal += std::max(int64(0), session.endMs - idleFrom);
		}
		const auto span = session.endMs - session.startMs;
		session.idleMs = std::clamp(idleTotal, int64(0), span);
		session.activeMs = ActiveMilliseconds(
			actions,
			spanMs,
			gapMs,
			session.startMs,
			session.endMs);
		result.push_back(std::move(session));
		open.reset();
		actions.clear();
		idleFrom = 0;
		idleTotal = 0;
	};

	const auto start = [&](const Event &event) {
		auto session = Session();
		session.startMs = event.unixMs;
		session.endMs = event.unixMs;
		session.dialogId = event.dialogId;
		session.chatKind = event.chatKind;
		session.preset = event.preset;
		session.hidden = event.hidden;
		open = std::move(session);
	};

	auto lastMs = ordered.front().unixMs;
	for (const auto &event : ordered) {
		lastMs = std::max(lastMs, event.unixMs);
		switch (event.kind) {
		case EventKind::Open:
			// An Open with nothing closed before it is the ordinary case on a
			// phone: tapping straight from one chat to the next.
			finish(event.unixMs);
			start(event);
			break;

		case EventKind::Close:
		case EventKind::Background:
			// Background covers the screen locking too: it reaches the app the
			// same way, and a chat you cannot see is not screen time.
			finish(event.unixMs);
			break;

		case EventKind::Foreground:
			// Nothing on its own. Coming back to the app does not say which
			// chat you came back to; the Open that follows does.
			break;

		case EventKind::Preset: {
			if (!open) {
				break;
			}
			const auto carried = *open;
			const auto wasIdle = (idleFrom != 0);
			finish(event.unixMs);
			auto next = Session();
			next.startMs = event.unixMs;
			next.endMs = event.unixMs;
			next.dialogId = carried.dialogId;
			next.chatKind = carried.chatKind;
			next.hidden = carried.hidden;

			// The event carries the preset that has just started running, so
			// the half after the cut is labelled with it and the half before
			// keeps the old one. Every second of the log has exactly one.
			next.preset = event.preset;
			open = std::move(next);
			if (wasIdle) {
				// The pause did not end because a preset changed underneath
				// it, so it carries over into the new half.
				idleFrom = event.unixMs;
			}
		} break;

		case EventKind::Action:
			if (open) {
				if (idleFrom) {
					// An action IS input, so it ends the pause whether or not
					// a Resume was written.
					idleTotal += std::max(int64(0), event.unixMs - idleFrom);
					idleFrom = 0;
				}
				actions.push_back(event.unixMs);
			}
			break;

		case EventKind::Idle:
			if (open && !idleFrom) {
				// Stamped back to where the input actually stopped, which is
				// idle_after before the recorder noticed. That is what keeps
				// the threshold the core's: change idle_after and the same log
				// pauses somewhere else.
				idleFrom = std::max(
					open->startMs,
					event.unixMs - idleAfterMs);
				if (!actions.empty()) {
					// Never before the last action's own span, or the session
					// would be paused over time it counts as active.
					idleFrom = std::max(idleFrom, actions.back() + spanMs);
				}
				idleFrom = std::min(idleFrom, event.unixMs);
			}
			break;

		case EventKind::Resume:
			if (open && idleFrom) {
				idleTotal += std::max(int64(0), event.unixMs - idleFrom);
				idleFrom = 0;
			}
			break;

		case EventKind::Peek:
		case EventKind::PeekEnd:
			// Nothing here. A peek does not cut a session and does not end
			// one: the same chat is in front of you, for as long as it was.
			// DerivePeeks() is where these two are read.
			break;
		}
	}

	// A session still open at the end of the log ran until the last thing we
	// know happened. Guessing at anything later would invent screen time.
	finish(lastMs);
	return result;
}

std::vector<PeekRun> DerivePeeks(const std::vector<Event> &events) {
	auto result = std::vector<PeekRun>();
	if (events.empty()) {
		return result;
	}
	auto ordered = events;
	std::stable_sort(ordered.begin(), ordered.end(), [](
			const Event &a,
			const Event &b) {
		return a.unixMs < b.unixMs;
	});

	auto open = std::optional<int64>();
	auto lastMs = ordered.front().unixMs;
	for (const auto &event : ordered) {
		lastMs = std::max(lastMs, event.unixMs);
		if (event.kind == EventKind::Peek) {
			if (!open) {
				open = event.unixMs;
			}
		} else if (event.kind == EventKind::PeekEnd && open) {
			result.push_back({ *open, std::max(*open, event.unixMs) });
			open.reset();
		}
	}
	if (open) {
		result.push_back({ *open, std::max(*open, lastMs) });
	}
	return result;
}

PeekUsage PeekUsageIn(
		const std::vector<PeekRun> &peeks,
		int64 fromMs,
		int64 toMs) {
	auto result = PeekUsage();
	if (toMs <= fromMs) {
		return result;
	}
	for (const auto &peek : peeks) {
		// Half-open, like the window: a peek that ended at the moment the
		// window opened belongs to the window before. A peek with no length at
		// all - started and ended inside the same millisecond, which a mistaken
		// tap is - is the one thing that has to be asked about separately,
		// because it has no span to overlap with.
		const auto empty = (peek.endMs <= peek.startMs);
		const auto touches = (peek.startMs < toMs)
			&& (empty ? (peek.startMs >= fromMs) : (peek.endMs > fromMs));
		if (!touches) {
			continue;
		}
		++result.count;
		result.totalMs += std::max(
			int64(0),
			std::min(peek.endMs, toMs) - std::max(peek.startMs, fromMs));
	}
	return result;
}

Slice SliceOf(const Session &session, int64 fromMs, int64 toMs) {
	auto result = Slice();
	if (toMs <= fromMs) {
		return result;
	}
	const auto span = session.endMs - session.startMs;
	if (span <= 0) {
		// A session with no length at all - opened and closed on the same
		// millisecond - still belongs to the window its moment fell in.
		if (session.startMs >= fromMs && session.startMs < toMs) {
			result.totalMs = session.totalMs();
			result.activeMs = session.activeMs;
		}
		return result;
	}
	const auto overlapFrom = std::max(session.startMs, fromMs);
	const auto overlapTo = std::min(session.endMs, toMs);
	if (overlapTo <= overlapFrom) {
		return result;
	}
	const auto overlap = overlapTo - overlapFrom;
	if (overlap == span) {
		return { session.totalMs(), session.activeMs };
	}
	result.totalMs = session.totalMs() * overlap / span;
	result.activeMs = session.activeMs * overlap / span;
	return result;
}

Totals RangeTotals(
		const std::vector<Session> &sessions,
		int64 fromMs,
		int64 toMs) {
	auto result = Totals();
	auto chats = std::map<PeerIdValue, ChatTotal>();
	auto kinds = std::map<int, KindTotal>();
	auto presets = std::map<QString, PresetTotal>();
	for (const auto &session : sessions) {
		const auto slice = SliceOf(session, fromMs, toMs);
		if (!slice.totalMs && !slice.activeMs) {
			continue;
		}
		result.totalMs += slice.totalMs;
		result.activeMs += slice.activeMs;
		if (session.hidden) {
			result.hiddenMs += slice.totalMs;
		}
		auto &chat = chats[session.dialogId];
		chat.dialogId = session.dialogId;
		chat.chatKind = session.chatKind;
		chat.totalMs += slice.totalMs;
		chat.activeMs += slice.activeMs;

		auto &kind = kinds[int(session.chatKind)];
		kind.chatKind = session.chatKind;
		kind.totalMs += slice.totalMs;
		kind.activeMs += slice.activeMs;

		auto &preset = presets[EffectivePreset(session.preset)];
		preset.preset = EffectivePreset(session.preset);
		preset.totalMs += slice.totalMs;
		preset.activeMs += slice.activeMs;
	}
	for (const auto &[id, entry] : chats) {
		result.chats.push_back(entry);
	}
	for (const auto &[index, entry] : kinds) {
		result.kinds.push_back(entry);
	}
	for (const auto &[name, entry] : presets) {
		result.presets.push_back(entry);
	}

	// Longest first. The map already put each list in a stable order - by id,
	// by kind, by name - and a stable sort keeps that as the tie-break, so two
	// runs over one log can never disagree about the order.
	const auto longest = [](const auto &a, const auto &b) {
		return a.totalMs > b.totalMs;
	};
	std::stable_sort(result.chats.begin(), result.chats.end(), longest);
	std::stable_sort(result.kinds.begin(), result.kinds.end(), longest);
	std::stable_sort(result.presets.begin(), result.presets.end(), longest);
	return result;
}

std::vector<Bucket> Buckets(
		const std::vector<Session> &sessions,
		int64 fromMs,
		int64 toMs,
		BucketUnit unit,
		const QTimeZone &zone) {
	auto result = std::vector<Bucket>();
	if (toMs <= fromMs || !zone.isValid()) {
		return result;
	}
	auto windows = std::vector<Window>();
	if (unit == BucketUnit::HourOfDay) {
		// Twenty-four buckets whatever the range is, empty ones included: a
		// gap in the row is a fact about the day, not a missing bar.
		for (auto hour = 0; hour != 24; ++hour) {
			auto bucket = Bucket();
			bucket.index = hour;
			bucket.label = u"%1"_q.arg(hour, 2, 10, QChar('0'));
			result.push_back(std::move(bucket));
		}
		windows = HourWindows(fromMs, toMs, zone);
	} else {
		auto date = UnitStart(LocalTime(fromMs, zone).date(), unit);
		auto index = 0;
		while (MsAt(date, QTime(0, 0), zone) < toMs) {
			const auto next = UnitNext(date, unit);
			auto bucket = Bucket();
			bucket.index = index;
			bucket.startMs = MsAt(date, QTime(0, 0), zone);
			bucket.endMs = MsAt(next, QTime(0, 0), zone);
			bucket.label = UnitLabel(date, unit);
			windows.push_back({ bucket.startMs, bucket.endMs, index });
			result.push_back(std::move(bucket));
			date = next;
			++index;
		}
	}
	for (const auto &window : windows) {
		// Clipped to the range as well as to the bucket, so a session that
		// started before the range does not leak into its first bucket.
		const auto start = std::max(window.startMs, fromMs);
		const auto end = std::min(window.endMs, toMs);
		if (end <= start) {
			continue;
		}
		auto &bucket = result[window.index];
		for (const auto &session : sessions) {
			const auto slice = SliceOf(session, start, end);
			bucket.totalMs += slice.totalMs;
			bucket.activeMs += slice.activeMs;
		}
	}
	return result;
}

int64 HeatMap::total(int weekday, int hour) const {
	if (weekday < 1 || weekday > 7 || hour < 0 || hour > 23) {
		return 0;
	}
	return totalMs[weekday - 1][hour];
}

int64 HeatMap::active(int weekday, int hour) const {
	if (weekday < 1 || weekday > 7 || hour < 0 || hour > 23) {
		return 0;
	}
	return activeMs[weekday - 1][hour];
}

HeatMap HeatMapFor(
		const std::vector<Session> &sessions,
		int64 fromMs,
		int64 toMs,
		const QTimeZone &zone) {
	auto result = HeatMap();
	if (toMs <= fromMs || !zone.isValid()) {
		return result;
	}
	for (const auto &window : HourWindows(fromMs, toMs, zone)) {
		const auto start = std::max(window.startMs, fromMs);
		const auto end = std::min(window.endMs, toMs);
		if (end <= start) {
			continue;
		}
		const auto weekday = LocalTime(window.startMs, zone).date().dayOfWeek();
		if (weekday < 1 || weekday > 7) {
			continue;
		}
		for (const auto &session : sessions) {
			const auto slice = SliceOf(session, start, end);
			result.totalMs[weekday - 1][window.index] += slice.totalMs;
			result.activeMs[weekday - 1][window.index] += slice.activeMs;
		}
	}
	return result;
}

Comparison Compare(
		const std::vector<Session> &sessions,
		int64 fromMs,
		int64 toMs) {
	auto result = Comparison();
	if (toMs <= fromMs) {
		return result;
	}
	const auto length = toMs - fromMs;
	result.current = RangeTotals(sessions, fromMs, toMs);
	result.previous = RangeTotals(sessions, fromMs - length, fromMs);
	result.deltaMs = result.current.totalMs - result.previous.totalMs;
	if (result.previous.totalMs > 0) {
		result.changePercent = int(
			result.deltaMs * 100 / result.previous.totalMs);
	}
	return result;
}

std::vector<BudgetSpent> BudgetLedger(
		const std::vector<Event> &events,
		const ScreenTime &settings,
		const QDate &day,
		const QTimeZone &zone) {
	auto result = std::vector<BudgetSpent>();
	if (!day.isValid() || !zone.isValid()) {
		return result;
	}
	const auto from = MsAt(day, QTime(0, 0), zone);
	const auto to = MsAt(day.addDays(1), QTime(0, 0), zone);
	const auto sessions = DeriveSessions(events, settings);
	auto index = 0;
	for (const auto &budget : settings.budgets) {
		auto entry = BudgetSpent();
		entry.index = index++;
		entry.perDayMs = Milliseconds(budget.perDaySeconds);
		for (const auto &session : sessions) {
			if (BudgetCovers(budget, session)) {
				entry.spentMs += SliceOf(session, from, to).totalMs;
			}
		}
		entry.reached = (entry.spentMs >= entry.perDayMs);
		result.push_back(std::move(entry));
	}
	return result;
}

bool CoverAllowed(int snoozesUsed, const ScreenTimeBudget &budget) {
	return (budget.mode == BudgetMode::Hard)
		&& (budget.snoozeSeconds > 0)
		&& (budget.snoozesPerDay > 0)
		&& (snoozesUsed < budget.snoozesPerDay);
}

std::vector<Event> Prune(
		const std::vector<Event> &events,
		int64 nowMs,
		int retentionDays) {
	if (retentionDays <= 0) {
		return events;
	}
	const auto oldest = nowMs
		- int64(retentionDays) * 24 * 60 * 60 * kMsInSecond;
	auto result = std::vector<Event>();
	result.reserve(events.size());
	for (const auto &event : events) {
		if (event.unixMs >= oldest) {
			result.push_back(event);
		}
	}
	return result;
}

QString FormatSpan(int64 ms) {
	const auto seconds = (ms > 0) ? (ms / kMsInSecond) : int64(0);
	if (seconds < 60) {
		// Under a minute the seconds are the only honest unit - and a span of
		// nothing says "0 s" rather than a bare "0", so an empty bar reads as
		// a length like every other bar beside it.
		return u"%1 s"_q.arg(seconds);
	}
	const auto minutes = seconds / 60;
	const auto days = minutes / (24 * 60);
	const auto hours = (minutes / 60) % 24;
	const auto rest = minutes % 60;

	// Assembled rather than branched: three units, each written only when it
	// is nonzero, is seven cases spelled out or one list joined.
	auto parts = QStringList();
	if (days) {
		parts.push_back(u"%1 d"_q.arg(days));
	}
	if (hours) {
		parts.push_back(u"%1 h"_q.arg(hours));
	}
	if (rest) {
		parts.push_back(u"%1 m"_q.arg(rest));
	}
	return parts.join(QChar(' '));
}

} // namespace Purple
