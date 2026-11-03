/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_mp4_repair.h"

#include <algorithm>
#include <cstring>

namespace Purple {
namespace {

[[nodiscard]] std::uint32_t Big32(const std::uint8_t *data) {
	return (std::uint32_t(data[0]) << 24)
		| (std::uint32_t(data[1]) << 16)
		| (std::uint32_t(data[2]) << 8)
		| std::uint32_t(data[3]);
}

[[nodiscard]] bool BoxType(const std::uint8_t *data, const char *type) {
	return !std::memcmp(data + 4, type, 4);
}

enum class NalStart {
	Invalid,
	// SEI, SPS, PPS or an access unit delimiter.
	NonVcl,
	// A slice whose first_mb_in_slice is 0: the start of a picture.
	SliceStart,
	// A slice of the same picture (first_mb_in_slice above 0).
	SliceMore,
};

// Reads a NAL unit header, and the first bit of a slice header: ue(v) 0 is a
// single 1 bit, so first_mb_in_slice is 0 exactly when that bit is set. The
// nal_ref_idc rules are H.264's: never 0 for IDR, SPS and PPS, always 0 for
// SEI and access unit delimiters.
[[nodiscard]] NalStart ReadNal(
		const std::uint8_t *nal,
		std::size_t size,
		bool &idr) {
	if (!size || (nal[0] & 0x80)) {
		return NalStart::Invalid;
	}
	const auto type = nal[0] & 0x1F;
	const auto reference = (nal[0] >> 5) & 0x03;
	idr = (type == 5);
	switch (type) {
	case 1:
	case 5:
		if ((type == 5 && !reference) || size < 2) {
			return NalStart::Invalid;
		}
		return (nal[1] & 0x80) ? NalStart::SliceStart : NalStart::SliceMore;
	case 6:
	case 9:
		return reference ? NalStart::Invalid : NalStart::NonVcl;
	case 7:
	case 8:
		return reference ? NalStart::NonVcl : NalStart::Invalid;
	}
	return NalStart::Invalid;
}

struct VideoFound {
	std::uint32_t size = 0;
	bool sync = false;
};

class Scanner {
public:
	Scanner(
		const std::uint8_t *data,
		std::size_t size,
		const RoundConfig &config,
		const AacFrameMeasure &measure)
	: _data(data)
	, _size(size)
	, _config(config)
	, _measure(measure)
	, _audio(config.audioTrack()) {
	}

	// A box between samples: an mdat header, which is skipped, or a free,
	// skip or wide box. Returns the bytes to skip, or 0.
	[[nodiscard]] std::size_t box(std::size_t at) const {
		const auto left = _size - at;
		if (left < 8) {
			return 0;
		}
		const auto header = _data + at;
		const auto size32 = Big32(header);
		if (BoxType(header, "mdat")) {
			if (size32 == 1) {
				return (left >= 16) ? 16 : 0;
			} else if (size32 != 0 && size32 < 8) {
				return 0;
			}
			// MP4Builder always writes 16 bytes: a small box carries 8 zero
			// bytes where a large one has its 64-bit size. An FFmpeg header
			// is 8 bytes, followed by sample data.
			if (left >= 16) {
				auto zero = true;
				for (auto i = 8; i != 16; ++i) {
					zero = zero && !header[i];
				}
				if (zero) {
					return 16;
				}
			}
			return 8;
		} else if (BoxType(header, "free")
			|| BoxType(header, "skip")
			|| BoxType(header, "wide")) {
			return (size32 >= 8 && size32 <= left) ? size32 : 0;
		}
		return 0;
	}

	[[nodiscard]] bool moov(std::size_t at) const {
		return (_size - at >= 8) && BoxType(_data + at, "moov");
	}

	// A whole video sample at `at', without the lookahead.
	[[nodiscard]] VideoFound video(std::size_t at) const {
		return _config.avccVideo ? avcc(at) : single(at);
	}

	// MP4Builder: one length over the whole sample.
	[[nodiscard]] VideoFound single(std::size_t at) const {
		const auto left = _size - at;
		if (left < 5) {
			return {};
		}
		const auto length = Big32(_data + at);
		if (!length
			|| std::uint64_t(length) + 4 > left
			|| std::uint64_t(length) + 4 > kRoundMaxVideoSample) {
			return {};
		}
		auto idr = false;
		const auto start = ReadNal(_data + at + 4, length, idr);
		if (start != NalStart::NonVcl && start != NalStart::SliceStart) {
			return {};
		}
		const auto type = _data[at + 4] & 0x1F;
		return { length + 4, idr || type == 7 };
	}

	// FFmpeg: an access unit of length-prefixed NAL units. Non-VCL units
	// attach to the picture after them; the picture's further slices
	// (first_mb_in_slice above 0) continue it.
	[[nodiscard]] VideoFound avcc(std::size_t at) const {
		auto position = at;
		auto sawSlice = false;
		auto sync = false;
		while (true) {
			const auto left = _size - position;
			if (left < 5) {
				break;
			}
			const auto length = Big32(_data + position);
			if (!length
				|| std::uint64_t(length) + 4 > left
				|| std::uint64_t(position - at) + length + 4
					> kRoundMaxVideoSample) {
				break;
			}
			auto idr = false;
			const auto start = ReadNal(_data + position + 4, length, idr);
			const auto continues = sawSlice
				? (start == NalStart::SliceMore)
				: (start == NalStart::NonVcl
					|| start == NalStart::SliceStart);
			if (!continues) {
				break;
			}
			sawSlice = sawSlice || (start == NalStart::SliceStart);
			sync = sync || idr;
			position += length + 4;
		}
		if (!sawSlice) {
			return {};
		}
		return { std::uint32_t(position - at), sync };
	}

	// The bytes of one AAC frame at `at', or 0.
	[[nodiscard]] std::size_t audio(std::size_t at) const {
		if (_audio < 0 || !_measure) {
			return 0;
		}
		const auto available = std::min(
			_size - at,
			std::size_t(kRoundMaxAacFrame));
		if (!available) {
			return 0;
		}
		const auto consumed = _measure(
			_config.tracks[_audio],
			_data + at,
			available);
		return (consumed > 0 && std::size_t(consumed) <= available)
			? std::size_t(consumed)
			: 0;
	}

	// Whether what is left from `at' is only a torn last sample: a video
	// length running past the end, or fewer bytes than the largest AAC frame
	// (which covers a torn mdat header too).
	[[nodiscard]] bool tornTail(std::size_t at) const {
		const auto left = _size - at;
		if (left <= kRoundMaxAacFrame) {
			return true;
		} else if (left >= 5) {
			const auto length = Big32(_data + at);
			auto idr = false;
			return std::uint64_t(length) + 4 > left
				&& std::uint64_t(length) + 4 <= kRoundMaxVideoSample
				&& ReadNal(_data + at + 4, left - 4, idr)
					!= NalStart::Invalid;
		}
		return false;
	}

	// The one-step lookahead after a video sample.
	[[nodiscard]] bool boundary(std::size_t at) const {
		return (at == _size)
			|| box(at)
			|| moov(at)
			|| video(at).size
			|| audio(at)
			|| tornTail(at);
	}

private:
	const std::uint8_t *_data = nullptr;
	std::size_t _size = 0;
	const RoundConfig &_config;
	const AacFrameMeasure &_measure;
	int _audio = -1;

};

// Exact frame times on the audio clock: frame n of a track that starts at
// `base' is at base + n * 1024 / rate, in microseconds.
[[nodiscard]] std::int64_t AudioTime(
		std::int64_t base,
		std::int64_t index,
		std::uint32_t rate) {
	return base + (index * 1024 * 1000000) / std::int64_t(rate);
}

[[nodiscard]] std::uint32_t AudioRate(const RoundConfig &config) {
	const auto audio = config.audioTrack();
	if (audio < 0) {
		return 0;
	}
	const auto &track = config.tracks[audio];
	return track.sampleRate
		? track.sampleRate
		: track.timescale
		? track.timescale
		: 44100;
}

// Whether a journaled sample still matches the media file at its offset.
[[nodiscard]] bool MatchesMedia(
		const std::uint8_t *media,
		const RoundConfig &config,
		const RoundSample &sample) {
	if (sample.track >= config.tracks.size()
		|| config.tracks[sample.track].kind != RoundTrackKind::Video) {
		return true;
	} else if (sample.size < 5) {
		return false;
	}
	const auto at = media + sample.offset;
	const auto length = Big32(at);
	auto idr = false;
	const auto start = ReadNal(at + 4, sample.size - 4, idr);
	if (start == NalStart::Invalid) {
		return false;
	}
	return config.avccVideo
		? (std::uint64_t(length) + 4 <= sample.size)
		: (std::uint64_t(length) + 4 == sample.size);
}

} // namespace

RoundRecovery RepairRound(
		const std::uint8_t *media,
		std::size_t mediaSize,
		const RoundConfig &config,
		const std::vector<RoundSample> &exact,
		const AacFrameMeasure &measure) {
	auto result = RoundRecovery();
	result.config = config;
	const auto videoTrack = config.videoTrack();
	const auto audioTrack = config.audioTrack();
	if (videoTrack < 0) {
		result.reason = RoundUnrecoverable::NoVideoTrack;
		return result;
	}

	auto start = std::size_t(0);
	if (!exact.empty()) {
		start = std::size_t(std::min<std::uint64_t>(
			exact.back().end(),
			mediaSize));
	} else if (mediaSize >= 8
		&& BoxType(media, "ftyp")
		&& Big32(media) >= 8
		&& Big32(media) <= mediaSize) {
		start = Big32(media);
	} else {
		result.reason = RoundUnrecoverable::NotMp4;
		return result;
	}
	result.samples = exact;
	result.exactSamples = exact.size();
	result.scanStart = start;

	// The clocks after the exact part, and the encoder offset it shows.
	const auto rate = AudioRate(config);
	auto audioEnd = std::int64_t(-1);
	auto audioBase = std::int64_t(0);
	auto audioIndex = std::int64_t(0);
	auto lastVideo = std::int64_t(-1);
	auto haveVideo = false;
	auto offsets = std::vector<std::int64_t>();
	for (const auto &sample : exact) {
		if (int(sample.track) == audioTrack && rate) {
			audioBase = sample.ptsUs;
			audioIndex = 1;
			audioEnd = AudioTime(sample.ptsUs, 1, rate);
		} else if (int(sample.track) == videoTrack) {
			if (audioEnd >= 0) {
				offsets.push_back(sample.ptsUs - audioEnd);
			}
			lastVideo = sample.ptsUs;
			haveVideo = true;
		}
	}
	if (!offsets.empty()) {
		const auto middle = offsets.begin() + offsets.size() / 2;
		std::nth_element(offsets.begin(), middle, offsets.end());
		result.encoderOffsetUs = *middle;
	}

	const auto scanner = Scanner(media, mediaSize, config, measure);
	auto position = start;
	auto finished = false;
	while (position < mediaSize) {
		if (scanner.moov(position)) {
			finished = true;
			break;
		} else if (const auto skip = scanner.box(position)) {
			position += skip;
			continue;
		}
		const auto video = scanner.video(position);
		if (video.size && scanner.boundary(position + video.size)) {
			auto anchor = (audioEnd >= 0)
				? (audioEnd + result.encoderOffsetUs)
				: rate
				? result.encoderOffsetUs
				: haveVideo
				? (lastVideo + 33333)
				: 0;
			if (haveVideo) {
				anchor = std::max(anchor, lastVideo + kRoundMinVideoStepUs);
			}
			auto sample = RoundSample();
			sample.track = std::uint8_t(videoTrack);
			sample.sync = video.sync;
			sample.size = video.size;
			sample.offset = position;
			sample.ptsUs = std::max<std::int64_t>(anchor, 0);
			result.samples.push_back(sample);
			++result.repairedSamples;
			lastVideo = sample.ptsUs;
			haveVideo = true;
			position += video.size;
			continue;
		}
		if (const auto frame = scanner.audio(position)) {
			auto sample = RoundSample();
			sample.track = std::uint8_t(audioTrack);
			sample.sync = true;
			sample.size = std::uint32_t(frame);
			sample.offset = position;
			sample.ptsUs = AudioTime(audioBase, audioIndex, rate);
			result.samples.push_back(sample);
			++result.repairedSamples;
			++audioIndex;
			audioEnd = AudioTime(audioBase, audioIndex, rate);
			position += frame;
			continue;
		}
		break;
	}
	result.scanEnd = position;
	result.unparsedBytes = mediaSize - position;
	const auto partial = !finished
		&& position < mediaSize
		&& !scanner.tornTail(position);
	if (result.samples.empty()) {
		result.reason = RoundUnrecoverable::NoSamples;
		result.status = RoundRecoveryStatus::Unrecoverable;
	} else if (partial) {
		result.status = RoundRecoveryStatus::Partial;
	} else if (result.repairedSamples) {
		result.status = RoundRecoveryStatus::Repaired;
	} else {
		result.status = RoundRecoveryStatus::Exact;
	}
	return result;
}

RoundRecovery RecoverRound(
		const std::uint8_t *media,
		std::size_t mediaSize,
		const std::uint8_t *journal,
		std::size_t journalSize,
		const AacFrameMeasure &measure) {
	const auto read = ReadRoundJournal(journal, journalSize);
	if (read.status != RoundJournalStatus::Ok || read.config.tracks.empty()) {
		auto result = RoundRecovery();
		result.config = read.config;
		result.reason = (read.status == RoundJournalStatus::UnknownVersion)
			? RoundUnrecoverable::UnknownVersion
			: RoundUnrecoverable::NoConfig;
		return result;
	}
	auto exact = std::vector<RoundSample>();
	exact.reserve(read.samples.size());
	auto previousEnd = std::uint64_t(0);
	for (const auto &sample : read.samples) {
		if (sample.end() > mediaSize
			|| sample.offset < previousEnd
			|| !sample.size
			|| !MatchesMedia(media, read.config, sample)) {
			break;
		}
		exact.push_back(sample);
		previousEnd = sample.end();
	}
	auto result = RepairRound(media, mediaSize, read.config, exact, measure);
	result.journalDropped = read.samples.size() - exact.size();
	return result;
}

} // namespace Purple
