/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The round video journal (F10 option (a)): the sidecar `<file>.pjr' a client
// writes next to a recording round video, so a crash can be recovered
// exactly. The byte layout is in docs/recording-formats.md; Android writes it
// from Java (PurpleRoundJournal) and Desktop with the encoders below, and both
// recover through RecoverRound() in purple_mp4_repair.h.
//
// The file is "PJR1", a version, then records, each with its own CRC-32:
//
//   - one header record (flags, video size and rotation),
//   - one track record per track, in the muxer's track order,
//   - batch records: samples, in the order they were written to the media
//     file.
//
// The header and the track records together are the config header. They are
// written once at the start even when the journal setting is Off ("off" then
// writes no batch), because repair needs the codec configuration.
//
// Pure C++17 without Qt.
namespace Purple {

inline constexpr std::uint16_t kRoundJournalVersion = 1;

enum class RoundTrackKind : std::uint8_t {
	Video = 1,
	Audio = 2,
};

// One track, as the muxer was given it.
struct RoundTrack {
	RoundTrackKind kind = RoundTrackKind::Video;

	// "video/avc", "audio/mp4a-latm", as MediaFormat names them.
	std::string mime;

	// The track's media timescale (MP4Builder: 90000 for video, the sample
	// rate for audio).
	std::uint32_t timescale = 0;

	// Video: the frame size. Audio: the sample rate and the channel count.
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t sampleRate = 0;
	std::uint32_t channels = 0;

	// The codec configuration buffers, in order: for H.264 csd-0 (SPS) and
	// csd-1 (PPS), for AAC csd-0 (the AudioSpecificConfig). At most 4, each at
	// most 64 KB.
	std::vector<std::vector<std::uint8_t>> csd;
};

// The config header.
struct RoundConfig {
	// MP4Builder's split-mdat mode: a new 16-byte mdat header after every
	// 32 KB of sample data.
	bool splitMdat = false;

	// How video samples are framed in the media data. false (Android's
	// MP4Builder): one 4-byte big-endian length (size - 4) over the whole
	// sample. true (Desktop's FFmpeg muxer): AVCC, a sequence of NAL units
	// each with its own 4-byte length.
	bool avccVideo = false;

	std::uint32_t width = 0;
	std::uint32_t height = 0;

	// Degrees clockwise: 0, 90, 180 or 270.
	std::uint16_t rotation = 0;

	std::vector<RoundTrack> tracks;

	// Index of the first video or audio track, or -1.
	[[nodiscard]] int videoTrack() const;
	[[nodiscard]] int audioTrack() const;
};

// One sample, as Mp4Movie.addSample sees it: where it starts in the media
// file (MP4Builder's data offset, including a video sample's 4-byte length)
// and its whole size.
struct RoundSample {
	std::uint8_t track = 0;
	bool sync = false;
	std::uint32_t size = 0;
	std::uint64_t offset = 0;
	std::int64_t ptsUs = 0;

	[[nodiscard]] std::uint64_t end() const {
		return offset + size;
	}

	friend bool operator==(const RoundSample &a, const RoundSample &b) {
		return a.track == b.track
			&& a.sync == b.sync
			&& a.size == b.size
			&& a.offset == b.offset
			&& a.ptsUs == b.ptsUs;
	}
	friend bool operator!=(const RoundSample &a, const RoundSample &b) {
		return !(a == b);
	}
};

// The standard CRC-32 (zlib, java.util.zip.CRC32): reflected polynomial
// 0xEDB88320, initial value and final xor 0xFFFFFFFF.
[[nodiscard]] std::uint32_t RoundJournalCrc(
	const std::uint8_t *data,
	std::size_t size);

// The start of a journal: the magic, the version, the header record and a
// track record for each track of `config'. A client that learns its tracks
// one by one writes EncodeRoundJournalStart() with no tracks and then
// EncodeRoundJournalTrack() as each one is added, before its first sample.
[[nodiscard]] std::vector<std::uint8_t> EncodeRoundJournalStart(
	const RoundConfig &config);

// A track record. `index' must be the number of tracks declared before it.
[[nodiscard]] std::vector<std::uint8_t> EncodeRoundJournalTrack(
	std::uint8_t index,
	const RoundTrack &track);

// A batch record of 1 to 65536 samples.
[[nodiscard]] std::vector<std::uint8_t> EncodeRoundJournalBatch(
	const std::vector<RoundSample> &samples);

enum class RoundJournalStatus {
	// Empty, or not a journal (no magic).
	Missing,
	// A journal of a version this build does not read.
	UnknownVersion,
	// The magic, but no readable header record.
	NoConfig,
	// The config header was read; samples holds every good batch.
	Ok,
};

struct RoundJournal {
	RoundJournalStatus status = RoundJournalStatus::Missing;
	RoundConfig config;

	// The samples of every batch read, in order.
	std::vector<RoundSample> samples;

	// The number of batch records read.
	int batches = 0;

	// The end of the last good record, and whether something that is not a
	// whole good record follows it (a torn append, a CRC failure, or a batch
	// naming a track that was not declared). Reading stops at the first such
	// record.
	std::uint64_t goodBytes = 0;
	bool damaged = false;
};

[[nodiscard]] RoundJournal ReadRoundJournal(
	const std::uint8_t *data,
	std::size_t size);

} // namespace Purple
