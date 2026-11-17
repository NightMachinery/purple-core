#include "purple/purple_playlists_download_job.h"

#include <algorithm>

namespace Purple {

PlaylistDownloadJobPolicy::PlaylistDownloadJobPolicy(std::size_t count)
: _rows(count, PlaylistDownloadPhase::Queued) {
}

std::vector<PlaylistDownloadTicket> PlaylistDownloadJobPolicy::admit() {
	auto result = std::vector<PlaylistDownloadTicket>();
	if (_cancelled) return result;
	auto active = progress().active;
	for (auto i = std::size_t(0); i != _rows.size() && active != 2; ++i) {
		if (_rows[i] != PlaylistDownloadPhase::Queued) continue;
		_rows[i] = PlaylistDownloadPhase::Active;
		result.push_back({ _generation, i });
		++active;
	}
	return result;
}

bool PlaylistDownloadJobPolicy::current(PlaylistDownloadTicket ticket) const {
	return !_cancelled && ticket.generation == _generation && ticket.index < _rows.size()
		&& _rows[ticket.index] == PlaylistDownloadPhase::Active;
}

bool PlaylistDownloadJobPolicy::finish(
		PlaylistDownloadTicket ticket,
		PlaylistDownloadPhase result) {
	if (!current(ticket)) return false;
	switch (result) {
	case PlaylistDownloadPhase::Downloaded:
	case PlaylistDownloadPhase::Unavailable:
	case PlaylistDownloadPhase::Failed:
	case PlaylistDownloadPhase::Busy: break;
	default: return false;
	}
	_rows[ticket.index] = result;
	return true;
}

bool PlaylistDownloadJobPolicy::retry() {
	if (_cancelled || progress().active) return false;
	++_generation;
	auto changed = false;
	for (auto &row : _rows) {
		if (row != PlaylistDownloadPhase::Failed && row != PlaylistDownloadPhase::Busy) continue;
		row = PlaylistDownloadPhase::Queued;
		changed = true;
	}
	return changed;
}

void PlaylistDownloadJobPolicy::cancel() {
	_cancelled = true;
	++_generation;
}

PlaylistDownloadProgress PlaylistDownloadJobPolicy::progress() const {
	auto result = PlaylistDownloadProgress{ .total = _rows.size() };
	for (const auto row : _rows) {
		switch (row) {
		case PlaylistDownloadPhase::Downloaded: ++result.downloaded; break;
		case PlaylistDownloadPhase::Unavailable: ++result.unavailable; break;
		case PlaylistDownloadPhase::Failed: ++result.failed; break;
		case PlaylistDownloadPhase::Busy: ++result.busy; break;
		case PlaylistDownloadPhase::Active: ++result.active; break;
		case PlaylistDownloadPhase::Queued: break;
		}
	}
	result.terminal = _cancelled || std::none_of(begin(_rows), end(_rows), [](auto row) {
		return row == PlaylistDownloadPhase::Queued || row == PlaylistDownloadPhase::Active;
	});
	result.cancelled = _cancelled;
	if (_cancelled) {
		result.active = 0;
		result.problem = PlaylistDownloadProblem::Cancelled;
	}
	return result;
}

} // namespace Purple
