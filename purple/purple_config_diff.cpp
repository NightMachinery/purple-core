/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_config_diff.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

namespace Purple {
namespace {

constexpr auto kWorkLimit = int64(5'000'000);
constexpr auto kUnreachedForward = -1;
constexpr auto kUnreachedBackward = std::numeric_limits<int>::max();

struct Line {
	const char *data = nullptr;
	qsizetype size = 0;
};

struct Block {
	int oldBegin = 0;
	int oldEnd = 0;
	int newBegin = 0;
	int newEnd = 0;
};

[[nodiscard]] std::vector<Line> SplitLines(const QByteArray &bytes) {
	const auto data = bytes.constData();
	const auto till = data + bytes.size();
	auto result = std::vector<Line>(bytes.count('\n') + 1);
	auto line = result.data();
	const auto add = [&](const char *start, const char *end) {
		line->data = start;
		line->size = (end > start && end[-1] == '\r')
			? (end - start - 1)
			: (end - start);
		++line;
	};
	auto start = data;
	for (auto i = data; i != till; ++i) {
		if (*i == '\n') {
			add(start, i);
			start = i + 1;
		}
	}
	if (start != till) {
		add(start, till);
	}
	result.resize(line - result.data());
	return result;
}

[[nodiscard]] uint64_t Hash(const Line &line) {
	auto result = uint64_t(14695981039346656037ULL);
	for (auto i = qsizetype(0); i != line.size; ++i) {
		result = (result ^ uchar(line.data[i])) * 1099511628211ULL;
	}
	result ^= result >> 29;
	result *= 0xbf58476d1ce4e5b9ULL;
	return result ^ (result >> 32);
}

[[nodiscard]] bool Equal(const Line &a, const Line &b) {
	return (a.size == b.size)
		&& !std::memcmp(a.data, b.data, size_t(a.size));
}

[[nodiscard]] QString Text(const Line &line) {
	return QString::fromUtf8(line.data, line.size);
}

// Myers' O(ND) difference algorithm in its linear-space form ("An O(ND)
// Difference Algorithm and Its Variations", 1986, section 4b). Lines are
// points on an edit graph: moving right deletes a line of the old text,
// moving down inserts a line of the new one, and a diagonal step is free where
// the two lines are equal. A search from the top-left corner and one from the
// bottom-right corner advance one edit at a time, each keeping only the
// furthest point it has reached on every diagonal, until the two frontiers
// overlap on a diagonal. That overlap lies on a shortest edit script, so the
// problem splits there into two halves that are solved the same way, each
// with a smaller edit distance, and memory stays linear in the input.
//
// The frontiers are kept inside the graph: a move that would leave it is not
// taken, and a diagonal no move can reach holds an "unreached" marker instead
// of a point, so every stored point is one that some path of that cost
// really reaches. Lines are interned to integers first (hash, then bytes), so
// the searches compare integers.
//
// Two limits keep the cost bounded on any input. The top-level search stops
// once the edit distance is known to exceed kConfigDiffEditLimit lines, and
// the whole run stops after kWorkLimit steps (diagonals visited plus lines
// compared), which only very repetitive texts approach. Either way the caller
// reports the texts as one replaced block.
class LineDiffer final {
public:
	LineDiffer(
		const std::vector<Line> &before,
		const std::vector<Line> &after);

	[[nodiscard]] bool run(int xLow, int xHigh, int yLow, int yHigh);
	[[nodiscard]] const std::vector<char> &removed() const;
	[[nodiscard]] const std::vector<char> &added() const;

private:
	struct Point {
		int x = 0;
		int y = 0;
	};

	void intern(int xLow, int xHigh, int yLow, int yHigh);
	[[nodiscard]] bool spend(int64 steps);
	[[nodiscard]] bool compare(
		int xLow,
		int xHigh,
		int yLow,
		int yHigh,
		bool top);
	[[nodiscard]] std::optional<Point> middle(
		int xLow,
		int xHigh,
		int yLow,
		int yHigh,
		bool top);

	const std::vector<Line> &_before;
	const std::vector<Line> &_after;
	std::vector<int> _beforeIds;
	std::vector<int> _afterIds;
	std::vector<int> _forwardStorage;
	std::vector<int> _backwardStorage;
	std::vector<char> _removed;
	std::vector<char> _added;
	const int *_a = nullptr;
	const int *_b = nullptr;
	int *_forward = nullptr;
	int *_backward = nullptr;
	int64 _budget = kWorkLimit;

};

LineDiffer::LineDiffer(
	const std::vector<Line> &before,
	const std::vector<Line> &after)
: _before(before)
, _after(after)
, _removed(before.size(), 0)
, _added(after.size(), 0) {
}

const std::vector<char> &LineDiffer::removed() const {
	return _removed;
}

const std::vector<char> &LineDiffer::added() const {
	return _added;
}

bool LineDiffer::spend(int64 steps) {
	_budget -= steps;
	return _budget >= 0;
}

void LineDiffer::intern(int xLow, int xHigh, int yLow, int yHigh) {
	const auto total = (xHigh - xLow) + (yHigh - yLow);
	auto capacity = size_t(16);
	while (capacity < size_t(total) * 2) {
		capacity *= 2;
	}
	const auto mask = capacity - 1;
	auto slotHashes = std::vector<uint64_t>(capacity);
	auto slotLines = std::vector<const Line*>(capacity, nullptr);
	auto slotIds = std::vector<int>(capacity);
	const auto hashes = slotHashes.data();
	const auto lines = slotLines.data();
	const auto ids = slotIds.data();
	auto count = 0;
	const auto assign = [&](const Line *line) {
		const auto hash = Hash(*line);
		auto slot = hash & mask;
		while (lines[slot]
			&& (hashes[slot] != hash || !Equal(*lines[slot], *line))) {
			slot = (slot + 1) & mask;
		}
		if (!lines[slot]) {
			lines[slot] = line;
			hashes[slot] = hash;
			ids[slot] = count++;
		}
		return ids[slot];
	};
	_beforeIds.assign(_before.size(), -1);
	_afterIds.assign(_after.size(), -1);
	const auto before = _before.data();
	const auto after = _after.data();
	const auto beforeIds = _beforeIds.data();
	const auto afterIds = _afterIds.data();
	for (auto x = xLow; x != xHigh; ++x) {
		beforeIds[x] = assign(before + x);
	}
	for (auto y = yLow; y != yHigh; ++y) {
		afterIds[y] = assign(after + y);
	}
	_a = beforeIds;
	_b = afterIds;
}

bool LineDiffer::run(int xLow, int xHigh, int yLow, int yHigh) {
	intern(xLow, xHigh, yLow, yHigh);
	const auto size = (xHigh - xLow) + (yHigh - yLow) + 3;
	const auto offset = yHigh - xLow + 1;
	_forwardStorage.assign(size, kUnreachedForward);
	_backwardStorage.assign(size, kUnreachedBackward);
	_forward = _forwardStorage.data() + offset;
	_backward = _backwardStorage.data() + offset;
	return compare(xLow, xHigh, yLow, yHigh, true);
}

bool LineDiffer::compare(
		int xLow,
		int xHigh,
		int yLow,
		int yHigh,
		bool top) {
	const auto a = _a;
	const auto b = _b;
	const auto width = (xHigh - xLow) + (yHigh - yLow);
	while (xLow < xHigh && yLow < yHigh && a[xLow] == b[yLow]) {
		++xLow;
		++yLow;
	}
	while (xLow < xHigh && yLow < yHigh && a[xHigh - 1] == b[yHigh - 1]) {
		--xHigh;
		--yHigh;
	}
	if (!spend(width - (xHigh - xLow) - (yHigh - yLow))) {
		return false;
	} else if (xLow == xHigh) {
		std::fill(_added.begin() + yLow, _added.begin() + yHigh, char(1));
		return true;
	} else if (yLow == yHigh) {
		std::fill(
			_removed.begin() + xLow,
			_removed.begin() + xHigh,
			char(1));
		return true;
	}
	const auto point = middle(xLow, xHigh, yLow, yHigh, top);
	return point
		&& compare(xLow, point->x, yLow, point->y, false)
		&& compare(point->x, xHigh, point->y, yHigh, false);
}

auto LineDiffer::middle(
		int xLow,
		int xHigh,
		int yLow,
		int yHigh,
		bool top) -> std::optional<Point> {
	const auto a = _a;
	const auto b = _b;
	const auto forward = _forward;
	const auto backward = _backward;
	const auto width = (xHigh - xLow) + (yHigh - yLow);
	const auto dMin = xLow - yHigh;
	const auto dMax = xHigh - yLow;
	const auto fMid = xLow - yLow;
	const auto bMid = xHigh - yHigh;
	const auto odd = ((fMid - bMid) & 1) != 0;
	auto fMin = fMid;
	auto fMax = fMid;
	auto bMin = bMid;
	auto bMax = bMid;
	forward[fMid] = xLow;
	backward[bMid] = xHigh;
	for (auto cost = 1;; ++cost) {
		if ((top && 2 * cost - 1 > kConfigDiffEditLimit)
			|| 2 * cost - 1 > width) {
			return std::nullopt;
		}
		if (fMin > dMin) {
			forward[--fMin - 1] = kUnreachedForward;
		} else {
			++fMin;
		}
		if (fMax < dMax) {
			forward[++fMax + 1] = kUnreachedForward;
		} else {
			--fMax;
		}
		for (auto d = fMax; d >= fMin; d -= 2) {
			const auto low = forward[d - 1];
			const auto high = forward[d + 1];
			auto x = kUnreachedForward;
			if (low != kUnreachedForward && low < xHigh) {
				x = low + 1;
			}
			if (high != kUnreachedForward && high - d <= yHigh && high > x) {
				x = high;
			}
			if (x == kUnreachedForward) {
				forward[d] = x;
				continue;
			}
			const auto start = x;
			auto y = x - d;
			while (x < xHigh && y < yHigh && a[x] == b[y]) {
				++x;
				++y;
			}
			_budget -= 1 + x - start;
			if (_budget < 0) {
				return std::nullopt;
			}
			forward[d] = x;
			if (odd
				&& bMin <= d
				&& d <= bMax
				&& backward[d] != kUnreachedBackward
				&& backward[d] <= x) {
				return Point{ x, y };
			}
		}
		if (top && 2 * cost > kConfigDiffEditLimit) {
			return std::nullopt;
		}
		if (bMin > dMin) {
			backward[--bMin - 1] = kUnreachedBackward;
		} else {
			++bMin;
		}
		if (bMax < dMax) {
			backward[++bMax + 1] = kUnreachedBackward;
		} else {
			--bMax;
		}
		for (auto d = bMax; d >= bMin; d -= 2) {
			const auto low = backward[d - 1];
			const auto high = backward[d + 1];
			auto x = kUnreachedBackward;
			if (high != kUnreachedBackward && high > xLow) {
				x = high - 1;
			}
			if (low != kUnreachedBackward && low - d >= yLow && low < x) {
				x = low;
			}
			if (x == kUnreachedBackward) {
				backward[d] = x;
				continue;
			}
			const auto start = x;
			auto y = x - d;
			while (x > xLow && y > yLow && a[x - 1] == b[y - 1]) {
				--x;
				--y;
			}
			_budget -= 1 + start - x;
			if (_budget < 0) {
				return std::nullopt;
			}
			backward[d] = x;
			if (!odd
				&& fMin <= d
				&& d <= fMax
				&& forward[d] != kUnreachedForward
				&& x <= forward[d]) {
				return Point{ x, y };
			}
		}
	}
}

[[nodiscard]] std::optional<std::vector<Block>> Blocks(
		const std::vector<char> &removed,
		const std::vector<char> &added) {
	auto result = std::vector<Block>();
	const auto n = int(removed.size());
	const auto m = int(added.size());
	auto x = 0;
	auto y = 0;
	while (x < n || y < m) {
		if (x < n && y < m && !removed[x] && !added[y]) {
			++x;
			++y;
			continue;
		}
		auto block = Block{ x, x, y, y };
		while ((x < n && removed[x]) || (y < m && added[y])) {
			while (x < n && removed[x]) {
				++x;
			}
			while (y < m && added[y]) {
				++y;
			}
		}
		if (x == block.oldBegin && y == block.newBegin) {
			return std::nullopt;
		}
		block.oldEnd = x;
		block.newEnd = y;
		result.push_back(block);
	}
	return result;
}

[[nodiscard]] ConfigTextDiff Replacement(
		const std::vector<Line> &before,
		const std::vector<Line> &after) {
	auto result = ConfigTextDiff();
	result.truncated = true;
	result.removed = int(before.size());
	result.added = int(after.size());
	auto hunk = ConfigDiffHunk();
	hunk.oldCount = result.removed;
	hunk.newCount = result.added;
	hunk.oldStart = hunk.oldCount ? 1 : 0;
	hunk.newStart = hunk.newCount ? 1 : 0;
	hunk.lines.reserve(before.size() + after.size());
	for (auto x = 0; x != hunk.oldCount; ++x) {
		hunk.lines.push_back({
			ConfigDiffLineKind::Removed,
			x + 1,
			0,
			Text(before[x]),
		});
	}
	for (auto y = 0; y != hunk.newCount; ++y) {
		hunk.lines.push_back({
			ConfigDiffLineKind::Added,
			0,
			y + 1,
			Text(after[y]),
		});
	}
	result.hunks.push_back(std::move(hunk));
	return result;
}

[[nodiscard]] ConfigTextDiff Hunks(
		const std::vector<Line> &before,
		const std::vector<Block> &blocks,
		const std::vector<Line> &after,
		int context) {
	auto result = ConfigTextDiff();
	for (const auto &block : blocks) {
		result.removed += block.oldEnd - block.oldBegin;
		result.added += block.newEnd - block.newBegin;
	}
	using Kind = ConfigDiffLineKind;
	const auto push = [](
			ConfigDiffHunk &hunk,
			Kind kind,
			int oldLine,
			int newLine,
			const Line &text) {
		hunk.lines.push_back({ kind, oldLine, newLine, Text(text) });
	};
	auto index = size_t(0);
	while (index < blocks.size()) {
		auto last = index;
		while (last + 1 < blocks.size()
			&& (blocks[last + 1].oldBegin - blocks[last].oldEnd
				<= 2 * context)) {
			++last;
		}
		const auto &first = blocks[index];
		const auto &closing = blocks[last];
		const auto oldFrom = std::max(0, first.oldBegin - context);
		const auto newFrom = first.newBegin - (first.oldBegin - oldFrom);
		const auto oldTo = std::min(
			int(before.size()),
			closing.oldEnd + context);
		const auto newTo = closing.newEnd + (oldTo - closing.oldEnd);
		auto hunk = ConfigDiffHunk();
		hunk.oldCount = oldTo - oldFrom;
		hunk.newCount = newTo - newFrom;
		hunk.oldStart = hunk.oldCount ? (oldFrom + 1) : oldFrom;
		hunk.newStart = hunk.newCount ? (newFrom + 1) : newFrom;
		auto x = oldFrom;
		auto y = newFrom;
		for (auto i = index; i <= last; ++i) {
			const auto &block = blocks[i];
			for (; x != block.oldBegin; ++x, ++y) {
				push(hunk, Kind::Context, x + 1, y + 1, before[x]);
			}
			for (; x != block.oldEnd; ++x) {
				push(hunk, Kind::Removed, x + 1, 0, before[x]);
			}
			for (; y != block.newEnd; ++y) {
				push(hunk, Kind::Added, 0, y + 1, after[y]);
			}
		}
		for (; x != oldTo; ++x, ++y) {
			push(hunk, Kind::Context, x + 1, y + 1, before[x]);
		}
		result.hunks.push_back(std::move(hunk));
		index = last + 1;
	}
	return result;
}

} // namespace

ConfigTextDiff DiffConfigText(
		const QByteArray &before,
		const QByteArray &after,
		int context) {
	const auto oldLines = SplitLines(before);
	const auto newLines = SplitLines(after);
	const auto n = int(oldLines.size());
	const auto m = int(newLines.size());
	const auto oldData = oldLines.data();
	const auto newData = newLines.data();
	auto prefix = 0;
	while (prefix < n
		&& prefix < m
		&& Equal(oldData[prefix], newData[prefix])) {
		++prefix;
	}
	if (prefix == n && prefix == m) {
		auto result = ConfigTextDiff();
		result.identical = true;
		return result;
	}
	auto suffix = 0;
	while (suffix < n - prefix
		&& suffix < m - prefix
		&& Equal(oldData[n - 1 - suffix], newData[m - 1 - suffix])) {
		++suffix;
	}
	auto differ = LineDiffer(oldLines, newLines);
	if (!differ.run(prefix, n - suffix, prefix, m - suffix)) {
		return Replacement(oldLines, newLines);
	}
	const auto blocks = Blocks(differ.removed(), differ.added());
	if (!blocks) {
		return Replacement(oldLines, newLines);
	}
	return Hunks(oldLines, *blocks, newLines, std::max(context, 0));
}

} // namespace Purple
