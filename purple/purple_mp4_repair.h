/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_round_journal.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Recovering a round video whose recorder crashed before it wrote `moov'
// (F10): the exact samples from the journal, then a scan of the media data
// for the rest, the untrunc approach. The result is a sample list; the client
// rebuilds the file from it with its own muxer (Android MP4Builder's
// writeSampleData with writeLength false, Desktop FFmpeg), so the recovered
// file has the layout upstream sends.
//
// The scanner reads the layout MP4Builder writes: `ftyp', then 16-byte
// `mdat' headers (one, or a new one after every 32 KB with split mdat)
// between samples, and samples in arrival order with no chunk grouping. Video
// samples delimit themselves with a 4-byte big-endian length (AVCC NAL units
// for Desktop's muxer); AAC frames do not, so their length comes from the
// client's AacFrameMeasure, a decoder.
//
// Pure C++17 without Qt. See docs/recording-formats.md.
namespace Purple {

// The bytes one raw AAC frame (1024 samples) at the start of `data' takes,
// decoded with the AudioSpecificConfig in `audio.csd[0]'; 0 or less when no
// whole frame decodes there. Called with at most `available' bytes; it must
// never read past them. The clients bind FFmpeg's AAC decoder.
using AacFrameMeasure = std::function<int(
	const RoundTrack &audio,
	const std::uint8_t *data,
	std::size_t available)>;

// Limits of the scan.
inline constexpr std::uint32_t kRoundMaxVideoSample = 1024 * 1024;
inline constexpr std::uint32_t kRoundMaxAacFrame = 8192;
// A repaired video sample is at least this much after the previous one.
inline constexpr std::int64_t kRoundMinVideoStepUs = 16667;
// Journaled times at or above this (2^40 us, about 12.7 days) are damage, so
// the timing arithmetic stays far from overflow.
inline constexpr std::int64_t kRoundMaxPtsUs = std::int64_t(1) << 40;

enum class RoundRecoveryStatus {
	// Every sample came from the journal; at most a torn last sample was
	// left.
	Exact,
	// Some or all samples came from the scan; the scan reached the end of
	// the data (at most a torn last sample was left).
	Repaired,
	// The scan stopped before the end of the data: the bin shows "Partly
	// recovered". The samples before that point are good.
	Partial,
	// Nothing to rebuild: see RoundRecovery::reason.
	Unrecoverable,
};

enum class RoundUnrecoverable {
	None,
	// No journal or no config header, so audio cannot be decoded.
	NoConfig,
	// A journal of a version this build does not read.
	UnknownVersion,
	// The media file does not start with an `ftyp' box and the journal gives
	// no samples.
	NotMp4,
	// The config header has no video track.
	NoVideoTrack,
	// No sample was found.
	NoSamples,
};

struct RoundRecovery {
	RoundRecoveryStatus status = RoundRecoveryStatus::Unrecoverable;
	RoundUnrecoverable reason = RoundUnrecoverable::None;

	RoundConfig config;

	// Every recovered sample, in media file order: first the exact ones from
	// the journal, then the repaired ones.
	std::vector<RoundSample> samples;
	std::size_t exactSamples = 0;
	std::size_t repairedSamples = 0;

	// Journal samples not used: past the end of the media file (not on disk
	// yet), not matching the bytes there (the file was rewritten), or with
	// values no recorder writes (see RepairRound). A nonzero count, or a
	// nonzero repairedSamples, makes the journal stale.
	std::size_t journalDropped = 0;

	// Where the scan started and stopped, and the bytes after that it could
	// not read.
	std::uint64_t scanStart = 0;
	std::uint64_t scanEnd = 0;
	std::uint64_t unparsedBytes = 0;

	// The constant encoder offset added to repaired video times: learned from
	// the exact part (video time minus the audio time written before it, the
	// median), otherwise 0.
	std::int64_t encoderOffsetUs = 0;
};

// Keeps the journaled samples from the first while they are usable, then
// scans `media' from the last kept one (or after `ftyp') and returns the kept
// samples followed by the repaired ones. A journaled sample is not usable,
// and it and every later one are counted in journalDropped, when it is
// empty, names an unknown track, does not lie inside the media file, starts
// inside ftyp or overlaps the previous sample, has a time outside
// [0, kRoundMaxPtsUs), goes back more than one frame within its track, or
// (video) no longer matches the bytes at its offset.
[[nodiscard]] RoundRecovery RepairRound(
	const std::uint8_t *media,
	std::size_t mediaSize,
	const RoundConfig &config,
	const std::vector<RoundSample> &journaled,
	const AacFrameMeasure &measure);

// The whole recovery: reads the journal and hands its config and samples to
// RepairRound. `journal' may be empty (no journal: unrecoverable, there is no config
// header).
[[nodiscard]] RoundRecovery RecoverRound(
	const std::uint8_t *media,
	std::size_t mediaSize,
	const std::uint8_t *journal,
	std::size_t journalSize,
	const AacFrameMeasure &measure);

} // namespace Purple
