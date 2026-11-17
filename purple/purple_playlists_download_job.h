#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Purple {

enum class PlaylistDownloadPhase { Queued, Active, Downloaded, Unavailable, Failed, Busy };
struct PlaylistDownloadTicket {
	uint64_t generation = 0;
	std::size_t index = 0;
};
enum class PlaylistDownloadProblem {
	None, Changed, Storage, MissingPlaylist, Busy, Unavailable, Cancelled, Unsupported,
};
struct PlaylistDownloadProgress {
	std::size_t total = 0;
	std::size_t downloaded = 0;
	std::size_t unavailable = 0;
	std::size_t failed = 0;
	std::size_t busy = 0;
	std::size_t active = 0;
	bool terminal = false;
	bool cancelled = false;
	PlaylistDownloadProblem problem = PlaylistDownloadProblem::None;
};

class PlaylistDownloadJobPolicy final {
public:
	explicit PlaylistDownloadJobPolicy(std::size_t count = 0);
	[[nodiscard]] std::vector<PlaylistDownloadTicket> admit();
	[[nodiscard]] bool current(PlaylistDownloadTicket ticket) const;
	[[nodiscard]] bool finish(PlaylistDownloadTicket ticket, PlaylistDownloadPhase result);
	[[nodiscard]] bool retry();
	void cancel();
	[[nodiscard]] PlaylistDownloadProgress progress() const;

private:
	uint64_t _generation = 1;
	bool _cancelled = false;
	std::vector<PlaylistDownloadPhase> _rows;

};

} // namespace Purple
