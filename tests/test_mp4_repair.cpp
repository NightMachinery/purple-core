/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_mp4_repair.h"
#include "purple/purple_round_journal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

using namespace Purple;

namespace {
auto Checks = 0;
auto Failures = 0;
const char *Section = "";
void Report(bool condition, const char *expression, int line) {
	++Checks;
	if (!condition) { ++Failures; std::printf("FAIL %s:%d %s\n", Section, line, expression); std::fflush(stdout); }
}
#define CHECK(condition) Report(bool(condition), #condition, __LINE__)

using Bytes = std::vector<std::uint8_t>;

constexpr auto kRate = 44100;
constexpr auto kFrameUs = 1024.0 * 1000000.0 / kRate;

[[nodiscard]] RoundConfig Config(bool split, bool avcc = false) {
	auto result = RoundConfig();
	result.splitMdat = split;
	result.avccVideo = avcc;
	result.width = 384;
	result.height = 384;
	result.rotation = 90;
	auto video = RoundTrack();
	video.kind = RoundTrackKind::Video;
	video.mime = "video/avc";
	video.timescale = 90000;
	video.width = 384;
	video.height = 384;
	video.csd = { { 0x67, 0x42, 0xC0, 0x1E, 0xDA }, { 0x68, 0xCE, 0x3C, 0x80 } };
	auto audio = RoundTrack();
	audio.kind = RoundTrackKind::Audio;
	audio.mime = "audio/mp4a-latm";
	audio.timescale = kRate;
	audio.sampleRate = kRate;
	audio.channels = 1;
	audio.csd = { { 0x12, 0x08 } };
	result.tracks = { video, audio };
	return result;
}

// A deterministic byte source.
struct Random {
	std::uint32_t state = 0x12345678;
	std::uint8_t next() {
		state = state * 1103515245U + 12345U;
		return std::uint8_t(state >> 16);
	}
};

// What MP4Builder writes, byte for byte as far as the scanner can see: ftyp,
// a 16-byte mdat header before the first sample and, with split, a new one
// before the first sample after every 32 KB (the previous one patched to its
// 64-bit size), then the samples in arrival order.
struct BuilderStream {
	explicit BuilderStream(bool split, bool avcc = false)
	: split(split)
	, avcc(avcc) {
		const std::uint8_t ftyp[] = {
			0, 0, 0, 24, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm',
			0, 0, 2, 0, 'i', 's', 'o', 'm', 'i', 's', 'o', '2',
		};
		bytes.assign(ftyp, ftyp + sizeof(ftyp));
		wroteSinceLastMdat = bytes.size();
	}

	void mdatHeader() {
		mdatAt = bytes.size();
		const std::uint8_t header[] = { 0, 0, 0, 16, 'm', 'd', 'a', 't', 0, 0, 0, 0, 0, 0, 0, 0 };
		bytes.insert(bytes.end(), header, header + 16);
		wroteSinceLastMdat += 16;
		mdatContent = 0;
	}

	void patchMdat() {
		const auto size = std::uint64_t(mdatContent + 16);
		bytes[mdatAt + 0] = 0;
		bytes[mdatAt + 1] = 0;
		bytes[mdatAt + 2] = 0;
		bytes[mdatAt + 3] = 1;
		for (auto i = 0; i != 8; ++i) {
			bytes[mdatAt + 8 + i] = std::uint8_t(size >> (8 * (7 - i)));
		}
	}

	void write(std::uint8_t track, const Bytes &sample, std::int64_t pts, bool sync) {
		if (newMdat) {
			mdatHeader();
			newMdat = false;
		}
		mdatContent += sample.size();
		wroteSinceLastMdat += sample.size();
		if (wroteSinceLastMdat >= 32 * 1024) {
			if (split) {
				patchMdat();
				newMdat = true;
			}
			wroteSinceLastMdat = 0;
		}
		auto record = RoundSample();
		record.track = track;
		record.sync = sync;
		record.size = std::uint32_t(sample.size());
		record.offset = bytes.size();
		record.ptsUs = pts;
		truth.push_back(record);
		bytes.insert(bytes.end(), sample.begin(), sample.end());
	}

	bool split = false;
	bool avcc = false;
	Bytes bytes;
	std::vector<RoundSample> truth;
	std::size_t mdatAt = 0;
	std::size_t mdatContent = 0;
	std::size_t wroteSinceLastMdat = 0;
	bool newMdat = true;
};

void PutBig32(Bytes &out, std::size_t at, std::uint32_t value) {
	out[at] = std::uint8_t(value >> 24);
	out[at + 1] = std::uint8_t(value >> 16);
	out[at + 2] = std::uint8_t(value >> 8);
	out[at + 3] = std::uint8_t(value);
}

// One NAL unit with its 4-byte length: header byte, a slice header whose
// first bit is first_mb_in_slice == 0 (for slices), then payload.
[[nodiscard]] Bytes Nal(std::uint8_t header, std::size_t size, Random &random, bool firstMb = true) {
	auto result = Bytes(size + 4);
	PutBig32(result, 0, std::uint32_t(size));
	result[4] = header;
	for (auto i = std::size_t(5); i != result.size(); ++i) {
		result[i] = random.next();
	}
	const auto type = header & 0x1F;
	if ((type == 1 || type == 5) && size >= 2) {
		result[5] = firstMb ? (result[5] | 0x80) : (result[5] & 0x7F);
	}
	return result;
}

// An MP4Builder video sample: one length over the whole sample.
[[nodiscard]] Bytes VideoSample(bool idr, std::size_t size, Random &random) {
	return Nal(idr ? 0x65 : 0x41, size - 4, random);
}

// A raw AAC frame. Real frames start with an element id (0x21 for a single
// channel element here), which read as a length is far past any file.
[[nodiscard]] Bytes AudioFrame(std::size_t size, Random &random) {
	auto result = Bytes(size);
	for (auto &byte : result) {
		byte = random.next();
	}
	result[0] = 0x21;
	return result;
}

// The client's decoder, stubbed: it knows where the real frames are.
struct Measure {
	const std::uint8_t *base = nullptr;
	std::map<std::uint64_t, std::uint32_t> frames;
	mutable int calls = 0;

	[[nodiscard]] AacFrameMeasure bind() const {
		return [this](const RoundTrack &audio, const std::uint8_t *data, std::size_t available) {
			++calls;
			if (audio.kind != RoundTrackKind::Audio || audio.csd.empty()) {
				return 0;
			}
			const auto i = frames.find(std::uint64_t(data - base));
			return (i != frames.end() && i->second <= available) ? int(i->second) : 0;
		};
	}
};

[[nodiscard]] Measure MeasureFor(const BuilderStream &stream, const Bytes &bytes, int audioTrack = 1) {
	auto result = Measure();
	result.base = bytes.data();
	for (const auto &sample : stream.truth) {
		if (sample.track == audioTrack) {
			result.frames.emplace(sample.offset, sample.size);
		}
	}
	return result;
}

[[nodiscard]] std::int64_t AudioPts(int index) {
	return (std::int64_t(index) * 1024 * 1000000) / kRate;
}

// A recording: audio frames on the exact audio clock, video frames at the
// given rate for each span, written in arrival order. A video frame arrives
// after every audio frame starting no later than its time plus `latencyUs'
// (the video encoder is that much slower).
[[nodiscard]] BuilderStream Record(
		bool split,
		std::vector<std::pair<int, int>> spans, // fps, milliseconds
		std::int64_t latencyUs = 0,
		std::uint32_t seed = 7) {
	auto stream = BuilderStream(split);
	auto random = Random{ seed };
	auto videoTimes = std::vector<std::int64_t>();
	auto t = std::int64_t(0);
	for (const auto &[fps, ms] : spans) {
		const auto step = 1000000 / fps;
		const auto till = t + std::int64_t(ms) * 1000;
		for (; t < till; t += step) {
			videoTimes.push_back(t);
		}
	}
	const auto total = t;
	auto audio = 0;
	auto frame = 0;
	for (const auto time : videoTimes) {
		while (AudioPts(audio) <= time + latencyUs && AudioPts(audio) < total) {
			stream.write(1, AudioFrame(150 + (audio * 37) % 250, random), AudioPts(audio), true);
			++audio;
		}
		const auto idr = !(frame % 30);
		stream.write(0, VideoSample(idr, idr ? 2500 : 600 + (frame * 53) % 900, random), time, idr);
		++frame;
	}
	while (AudioPts(audio) < total) {
		stream.write(1, AudioFrame(150 + (audio * 37) % 250, random), AudioPts(audio), true);
		++audio;
	}
	return stream;
}

[[nodiscard]] Bytes JournalFor(const RoundConfig &config, const std::vector<RoundSample> &samples, std::size_t perBatch = 77) {
	auto out = EncodeRoundJournalStart(config);
	for (auto from = std::size_t(0); from < samples.size(); from += perBatch) {
		const auto till = std::min(samples.size(), from + perBatch);
		const auto batch = EncodeRoundJournalBatch({ samples.begin() + from, samples.begin() + till });
		out.insert(out.end(), batch.begin(), batch.end());
	}
	return out;
}

// The recovered samples are where the real ones are, of the same size and
// track and sync flag.
[[nodiscard]] bool SameLayout(const std::vector<RoundSample> &got, const std::vector<RoundSample> &truth, std::size_t count) {
	if (got.size() != count || truth.size() < count) {
		return false;
	}
	for (auto i = std::size_t(0); i != count; ++i) {
		if (got[i].offset != truth[i].offset
			|| got[i].size != truth[i].size
			|| got[i].track != truth[i].track
			|| got[i].sync != truth[i].sync) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] std::size_t CompleteBefore(const std::vector<RoundSample> &truth, std::uint64_t cut) {
	auto result = std::size_t(0);
	while (result != truth.size() && truth[result].end() <= cut) {
		++result;
	}
	return result;
}

void TestJournalFormat() {
	Section = "journal format";
	const auto config = Config(true);
	auto samples = std::vector<RoundSample>();
	for (auto i = 0; i != 200; ++i) {
		auto sample = RoundSample();
		sample.track = std::uint8_t(i % 2);
		sample.sync = !(i % 30);
		sample.size = std::uint32_t(100 + i);
		sample.offset = std::uint64_t(40 + i * 1000);
		sample.ptsUs = std::int64_t(i) * 33333 - 5;
		samples.push_back(sample);
	}
	const auto bytes = JournalFor(config, samples, 50);
	CHECK(!std::memcmp(bytes.data(), "PJR1", 4));
	const auto read = ReadRoundJournal(bytes.data(), bytes.size());
	CHECK(read.status == RoundJournalStatus::Ok);
	CHECK(!read.damaged);
	CHECK(read.goodBytes == bytes.size());
	CHECK(read.batches == 4);
	CHECK(read.samples == samples);
	CHECK(read.config.splitMdat);
	CHECK(!read.config.avccVideo);
	CHECK(read.config.width == 384 && read.config.height == 384);
	CHECK(read.config.rotation == 90);
	CHECK(read.config.tracks.size() == 2);
	CHECK(read.config.videoTrack() == 0);
	CHECK(read.config.audioTrack() == 1);
	if (read.config.tracks.size() == 2) {
		const auto &video = read.config.tracks[0];
		const auto &audio = read.config.tracks[1];
		CHECK(video.mime == "video/avc");
		CHECK(video.timescale == 90000);
		CHECK(video.width == 384 && video.height == 384);
		CHECK(video.csd == config.tracks[0].csd);
		CHECK(audio.mime == "audio/mp4a-latm");
		CHECK(audio.sampleRate == kRate && audio.channels == 1);
		CHECK(audio.csd == config.tracks[1].csd);
	}
	// 24 bytes a sample, 16 bytes of record framing and count a batch.
	const auto start = EncodeRoundJournalStart(config).size();
	CHECK(bytes.size() == start + 4 * (8 + 4 + 4) + 200 * 24);

	// The CRC is zlib's.
	const auto text = std::string("123456789");
	CHECK(RoundJournalCrc(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()) == 0xCBF43926U);

	// Off: the config header alone.
	const auto off = EncodeRoundJournalStart(config);
	const auto header = ReadRoundJournal(off.data(), off.size());
	CHECK(header.status == RoundJournalStatus::Ok);
	CHECK(header.samples.empty());
	CHECK(header.config.tracks.size() == 2);
	CHECK(!header.damaged);
	CHECK(off.size() < 200);

	// Tracks declared one by one, the way the muxer learns them.
	auto bare = config;
	bare.tracks.clear();
	auto incremental = EncodeRoundJournalStart(bare);
	const auto first = EncodeRoundJournalTrack(0, config.tracks[0]);
	const auto firstBatch = EncodeRoundJournalBatch({ samples[0] });
	const auto second = EncodeRoundJournalTrack(1, config.tracks[1]);
	const auto secondBatch = EncodeRoundJournalBatch({ samples[1] });
	for (const auto *part : { &first, &firstBatch, &second, &secondBatch }) {
		incremental.insert(incremental.end(), part->begin(), part->end());
	}
	const auto stepwise = ReadRoundJournal(incremental.data(), incremental.size());
	CHECK(stepwise.status == RoundJournalStatus::Ok);
	CHECK(!stepwise.damaged);
	CHECK(stepwise.samples.size() == 2);
	CHECK(stepwise.config.tracks.size() == 2);

	// A batch naming a track not declared yet stops the read.
	auto early = EncodeRoundJournalStart(bare);
	early.insert(early.end(), first.begin(), first.end());
	early.insert(early.end(), secondBatch.begin(), secondBatch.end());
	const auto undeclared = ReadRoundJournal(early.data(), early.size());
	CHECK(undeclared.damaged);
	CHECK(undeclared.samples.empty());

	// A track record out of order stops it too.
	auto skipped = EncodeRoundJournalStart(bare);
	skipped.insert(skipped.end(), second.begin(), second.end());
	CHECK(ReadRoundJournal(skipped.data(), skipped.size()).damaged);
}

void TestJournalDamage() {
	Section = "journal damage";
	const auto config = Config(false);
	auto samples = std::vector<RoundSample>();
	for (auto i = 0; i != 100; ++i) {
		auto sample = RoundSample();
		sample.track = 1;
		sample.size = 200;
		sample.offset = 40 + i * 200;
		sample.ptsUs = AudioPts(i);
		samples.push_back(sample);
	}
	const auto bytes = JournalFor(config, samples, 10);
	const auto start = EncodeRoundJournalStart(config).size();
	const auto batchSize = std::size_t(8 + 4 + 10 * 24 + 4);

	// A flipped byte in the fourth batch: three batches survive.
	auto flipped = bytes;
	flipped[start + 3 * batchSize + 30] ^= 0x01;
	const auto bad = ReadRoundJournal(flipped.data(), flipped.size());
	CHECK(bad.status == RoundJournalStatus::Ok);
	CHECK(bad.damaged);
	CHECK(bad.batches == 3);
	CHECK(bad.samples.size() == 30);
	CHECK(bad.goodBytes == start + 3 * batchSize);

	// A torn append at every byte of the last batch: nine batches survive.
	auto allTorn = true;
	for (auto cut = start + 9 * batchSize + 1; cut != bytes.size(); ++cut) {
		const auto torn = ReadRoundJournal(bytes.data(), cut);
		allTorn = allTorn
			&& torn.status == RoundJournalStatus::Ok
			&& torn.damaged
			&& torn.samples.size() == 90
			&& torn.goodBytes == start + 9 * batchSize;
	}
	CHECK(allTorn);

	// Not a journal, another version, a damaged header.
	CHECK(ReadRoundJournal(nullptr, 0).status == RoundJournalStatus::Missing);
	const auto junk = Bytes(64, 0x55);
	CHECK(ReadRoundJournal(junk.data(), junk.size()).status == RoundJournalStatus::Missing);
	auto later = bytes;
	later[4] = 2;
	CHECK(ReadRoundJournal(later.data(), later.size()).status == RoundJournalStatus::UnknownVersion);
	auto header = bytes;
	header[12] ^= 0xFF;
	const auto noConfig = ReadRoundJournal(header.data(), header.size());
	CHECK(noConfig.status == RoundJournalStatus::NoConfig);
	CHECK(noConfig.samples.empty());
}

// Journals with good CRCs and values no recorder writes: they must never
// make the recovery read outside the media file or overflow its timing.
void TestHostileJournal() {
	Section = "hostile journal";
	auto stream = Record(false, { { 30, 1000 } });
	const auto &bytes = stream.bytes;
	const auto config = Config(false);
	const auto measure = MeasureFor(stream, bytes);
	const auto recover = [&](std::vector<RoundSample> samples) {
		const auto journal = JournalFor(config, samples);
		return RecoverRound(bytes.data(), bytes.size(), journal.data(), journal.size(), measure.bind());
	};
	const auto inside = [&](const RoundRecovery &got) {
		for (const auto &sample : got.samples) {
			if (sample.offset > bytes.size() || sample.size > bytes.size() - sample.offset) {
				return false;
			}
		}
		return true;
	};
	const auto good = std::vector<RoundSample>(stream.truth.begin(), stream.truth.begin() + 20);

	// P1-1: offset + size wraps past 2^64. The reader calls it damage.
	for (const auto track : { std::uint8_t(0), std::uint8_t(1) }) {
		auto wrap = RoundSample();
		wrap.track = track;
		wrap.size = 32;
		wrap.offset = UINT64_MAX - 15;
		const auto journal = JournalFor(config, { wrap });
		const auto read = ReadRoundJournal(journal.data(), journal.size());
		CHECK(read.damaged);
		CHECK(read.samples.empty());
		const auto got = recover({ wrap });
		CHECK(got.exactSamples == 0);
		CHECK(inside(got));
		CHECK(got.status == RoundRecoveryStatus::Repaired);
		// Handed straight to RepairRound, it is dropped there as well.
		const auto direct = RepairRound(bytes.data(), bytes.size(), config, { wrap }, measure.bind());
		CHECK(direct.exactSamples == 0);
		CHECK(direct.journalDropped == 1);
		CHECK(inside(direct));
	}

	// Offsets past the end, a size past the end, a size of 0, a sample inside
	// ftyp: each ends the exact part there.
	const auto cutAt = [&](RoundSample bad) {
		auto samples = good;
		samples.insert(samples.begin() + 10, bad);
		const auto got = recover(samples);
		return got.exactSamples == 10
			&& got.journalDropped == 11
			&& inside(got)
			&& SameLayout(got.samples, stream.truth, stream.truth.size());
	};
	auto bad = stream.truth[10];
	bad.offset = bytes.size() + 1;
	CHECK(cutAt(bad));
	bad = stream.truth[10];
	bad.size = std::uint32_t(bytes.size());
	CHECK(cutAt(bad));
	bad = stream.truth[10];
	bad.size = 0;
	CHECK(cutAt(bad));
	auto inFtyp = stream.truth[0];
	inFtyp.track = 1;
	inFtyp.offset = 4;
	inFtyp.size = 16;
	const auto ftyp = recover({ inFtyp });
	CHECK(ftyp.exactSamples == 0);
	CHECK(ftyp.journalDropped == 1);
	CHECK(SameLayout(ftyp.samples, stream.truth, stream.truth.size()));

	// P2-1: extreme times. Each ends the exact part, and the repaired times
	// stay in range (UBSan would flag the old arithmetic here).
	const auto timed = [&](std::size_t index, std::int64_t pts) {
		auto samples = good;
		samples[index].ptsUs = pts;
		const auto got = recover(samples);
		auto sane = true;
		for (const auto &sample : got.samples) {
			sane = sane && sample.ptsUs >= 0 && sample.ptsUs < kRoundMaxPtsUs;
		}
		return got.exactSamples == index && sane && inside(got);
	};
	const auto audioAt = std::size_t(std::find_if(good.begin() + 5, good.end(), [](const RoundSample &s) { return s.track == 1; }) - good.begin());
	const auto videoAt = std::size_t(std::find_if(good.begin() + 5, good.end(), [](const RoundSample &s) { return s.track == 0; }) - good.begin());
	CHECK(timed(audioAt, INT64_MAX));
	CHECK(timed(audioAt, INT64_MAX - 21000));
	CHECK(timed(videoAt, INT64_MIN));
	CHECK(timed(videoAt, -1));
	CHECK(timed(videoAt, kRoundMaxPtsUs));
	// A time just inside the range is kept.
	{
		auto samples = std::vector<RoundSample>(good.begin(), good.begin() + 1);
		samples[0].ptsUs = kRoundMaxPtsUs - 1;
		const auto got = recover(samples);
		CHECK(got.exactSamples == 1);
		auto sane = true;
		for (const auto &sample : got.samples) {
			sane = sane && sample.ptsUs >= 0;
		}
		CHECK(sane);
	}
	// Going back by more than a frame within a track is damage; within a
	// frame is jitter and kept.
	{
		auto samples = good;
		samples[videoAt].ptsUs = 0;
		const auto back = recover(samples);
		CHECK(back.exactSamples == videoAt);
		auto jitter = good;
		const auto previousVideo = std::find_if(
			std::make_reverse_iterator(jitter.begin() + videoAt),
			jitter.rend(),
			[](const RoundSample &s) { return s.track == 0; });
		if (previousVideo != jitter.rend()) {
			jitter[videoAt].ptsUs = previousVideo->ptsUs - 1000;
			CHECK(recover(jitter).exactSamples == good.size());
		}
	}
	// A track index past the declared tracks never reaches RepairRound.
	auto unknown = good[3];
	unknown.track = 5;
	const auto direct = RepairRound(bytes.data(), bytes.size(), config, { good[0], unknown }, measure.bind());
	CHECK(direct.exactSamples == 1);
	CHECK(inside(direct));
}

void CheckRecovery(bool split) {
	auto stream = Record(split, { { 30, 3000 } });
	const auto &bytes = stream.bytes;
	const auto config = Config(split);
	const auto measure = MeasureFor(stream, bytes);
	const auto header = EncodeRoundJournalStart(config);

	// Off: repair from the start finds every sample.
	const auto off = RecoverRound(bytes.data(), bytes.size(), header.data(), header.size(), measure.bind());
	CHECK(off.status == RoundRecoveryStatus::Repaired);
	CHECK(off.reason == RoundUnrecoverable::None);
	CHECK(off.exactSamples == 0);
	CHECK(off.repairedSamples == stream.truth.size());
	CHECK(SameLayout(off.samples, stream.truth, stream.truth.size()));
	CHECK(off.scanStart == 24);
	CHECK(off.scanEnd == bytes.size());
	CHECK(off.unparsedBytes == 0);
	// Audio is exact.
	auto audioExact = true;
	for (auto i = std::size_t(0); i != off.samples.size(); ++i) {
		if (off.samples[i].track == 1) {
			audioExact = audioExact && (off.samples[i].ptsUs == stream.truth[i].ptsUs);
		}
	}
	CHECK(audioExact);

	// Journal on and complete: exact.
	const auto full = JournalFor(config, stream.truth);
	const auto exact = RecoverRound(bytes.data(), bytes.size(), full.data(), full.size(), measure.bind());
	CHECK(exact.status == RoundRecoveryStatus::Exact);
	CHECK(exact.samples == stream.truth);
	CHECK(exact.exactSamples == stream.truth.size());
	CHECK(exact.repairedSamples == 0);
	CHECK(exact.journalDropped == 0);

	// The journal holds samples whose bytes never reached the disk: they are
	// dropped, and the rest is exact.
	const auto cut = std::size_t(stream.truth[stream.truth.size() - 10].offset + 3);
	const auto beyond = RecoverRound(bytes.data(), cut, full.data(), full.size(), measure.bind());
	CHECK(beyond.status == RoundRecoveryStatus::Exact);
	CHECK(beyond.journalDropped == 10);
	CHECK(beyond.exactSamples == stream.truth.size() - 10);
	CHECK(beyond.repairedSamples == 0);

	// Truncation at every byte of the last three samples, with the journal
	// Off: every complete sample is kept, the torn one is dropped, and it is
	// not reported as partial. The one exception is the lookahead's price: a
	// last complete video sample followed only by a torn audio frame or a
	// torn mdat header is dropped too, never anything earlier.
	auto allPrefix = true;
	const auto from = std::size_t(stream.truth[stream.truth.size() - 3].offset);
	for (auto end = from; end <= bytes.size(); ++end) {
		const auto copy = Bytes(bytes.begin(), bytes.begin() + end);
		const auto local = MeasureFor(stream, copy);
		const auto got = RecoverRound(copy.data(), copy.size(), header.data(), header.size(), local.bind());
		const auto complete = CompleteBefore(stream.truth, end);
		const auto lastIsVideo = complete > 0
			&& stream.truth[complete - 1].track == 0
			&& stream.truth[complete - 1].end() < end;
		const auto ok = (got.status == RoundRecoveryStatus::Repaired)
			&& (SameLayout(got.samples, stream.truth, complete)
				|| (lastIsVideo
					&& SameLayout(got.samples, stream.truth, complete - 1)));
		if (!ok) {
			std::printf("split=%d cut=%zu complete=%zu got=%zu status=%d\n",
				int(split), end, complete, got.samples.size(), int(got.status));
		}
		allPrefix = allPrefix && ok;
	}
	CHECK(allPrefix);
}

void TestRecovery() {
	Section = "recovery unsplit";
	CheckRecovery(false);
	Section = "recovery split";
	CheckRecovery(true);

	Section = "split layout";
	// The split stream really has several mdat headers, all skipped.
	auto stream = Record(true, { { 30, 3000 } });
	auto headers = 0;
	for (auto i = std::size_t(0); i + 8 <= stream.bytes.size(); ++i) {
		headers += !std::memcmp(stream.bytes.data() + i + 4, "mdat", 4);
	}
	CHECK(headers > 3);
}

void TestVariableFrameRate() {
	Section = "variable frame rate";
	// 30 fps, then a low-light 15 fps, then 30 again.
	auto stream = Record(false, { { 30, 2000 }, { 15, 2000 }, { 30, 1000 } });
	const auto config = Config(false);
	const auto measure = MeasureFor(stream, stream.bytes);
	const auto header = EncodeRoundJournalStart(config);
	const auto got = RecoverRound(stream.bytes.data(), stream.bytes.size(), header.data(), header.size(), measure.bind());
	CHECK(got.status == RoundRecoveryStatus::Repaired);
	CHECK(got.samples.size() == stream.truth.size());
	auto worst = 0.;
	auto increasing = true;
	auto previous = std::int64_t(-1);
	for (auto i = std::size_t(0); i != got.samples.size() && i != stream.truth.size(); ++i) {
		if (got.samples[i].track != 0) {
			continue;
		}
		worst = std::max(worst, double(std::llabs(got.samples[i].ptsUs - stream.truth[i].ptsUs)));
		increasing = increasing && (got.samples[i].ptsUs > previous);
		previous = got.samples[i].ptsUs;
	}
	CHECK(worst <= kFrameUs);
	CHECK(increasing);
	// The last video frame lands near the real end, not halfway (as a
	// nominal 30 fps would put it).
	const auto lastVideo = std::find_if(got.samples.rbegin(), got.samples.rend(), [](const RoundSample &s) { return s.track == 0; });
	CHECK(lastVideo != got.samples.rend() && lastVideo->ptsUs > 4900000);
	CHECK(got.encoderOffsetUs == 0);
}

void TestJournalPlusTail() {
	Section = "journal plus tail";
	// The video encoder runs 50 ms behind the audio one: the offset the
	// exact part shows carries the repaired tail.
	auto stream = Record(true, { { 30, 2000 }, { 15, 1000 } }, 50000);
	const auto config = Config(true);
	const auto measure = MeasureFor(stream, stream.bytes);
	const auto journaled = std::size_t(stream.truth.size() * 2 / 3);
	const auto journal = JournalFor(config, { stream.truth.begin(), stream.truth.begin() + journaled });
	const auto got = RecoverRound(stream.bytes.data(), stream.bytes.size(), journal.data(), journal.size(), measure.bind());
	CHECK(got.status == RoundRecoveryStatus::Repaired);
	CHECK(got.exactSamples == journaled);
	CHECK(got.repairedSamples == stream.truth.size() - journaled);
	CHECK(got.scanStart == stream.truth[journaled - 1].end());
	CHECK(SameLayout(got.samples, stream.truth, stream.truth.size()));
	CHECK(got.encoderOffsetUs < -20000 && got.encoderOffsetUs > -80000);
	auto exactSame = true;
	for (auto i = std::size_t(0); i != journaled; ++i) {
		exactSame = exactSame && (got.samples[i] == stream.truth[i]);
	}
	CHECK(exactSame);
	auto worst = 0.;
	auto audioExact = true;
	for (auto i = journaled; i != got.samples.size(); ++i) {
		const auto error = double(std::llabs(got.samples[i].ptsUs - stream.truth[i].ptsUs));
		if (got.samples[i].track == 0) {
			worst = std::max(worst, error);
		} else {
			audioExact = audioExact && (error <= 1);
		}
	}
	CHECK(worst <= kFrameUs);
	CHECK(audioExact);

	// A stale journal whose bytes were rewritten (V2 trimming): the
	// mismatched samples are dropped and repaired from there.
	auto rewritten = stream.bytes;
	const auto victim = std::find_if(
		stream.truth.begin() + journaled / 2,
		stream.truth.end(),
		[](const RoundSample &s) { return s.track == 0; });
	const auto victimIndex = std::size_t(victim - stream.truth.begin());
	PutBig32(rewritten, std::size_t(victim->offset), victim->size + 100);
	const auto stale = RecoverRound(rewritten.data(), rewritten.size(), journal.data(), journal.size(), measure.bind());
	CHECK(stale.exactSamples == victimIndex);
	CHECK(stale.journalDropped == journaled - victimIndex);
	CHECK(stale.status == RoundRecoveryStatus::Partial);
	CHECK(SameLayout(stale.samples, stream.truth, victimIndex));
}

void TestLookahead() {
	Section = "lookahead";
	auto stream = BuilderStream(false);
	auto random = Random{ 3 };
	stream.write(0, VideoSample(true, 2000, random), 0, true);
	stream.write(1, AudioFrame(200, random), AudioPts(0), true);
	// An audio frame whose first bytes read as a valid video length (100)
	// and a valid slice header; 104 bytes on lies the middle of the frame,
	// which is no boundary, so the lookahead rejects the video reading.
	auto trick = Bytes(300, 0xFF);
	trick[0] = 0;
	trick[1] = 0;
	trick[2] = 0;
	trick[3] = 100;
	trick[4] = 0x41;
	trick[5] = 0x88;
	stream.write(1, trick, AudioPts(1), true);
	for (auto i = 0; i != 40; ++i) {
		stream.write(0, VideoSample(false, 700, random), 40000 + i * 33333, false);
		stream.write(1, AudioFrame(180, random), AudioPts(2 + i), true);
	}
	const auto config = Config(false);
	const auto measure = MeasureFor(stream, stream.bytes);
	const auto header = EncodeRoundJournalStart(config);
	const auto got = RecoverRound(stream.bytes.data(), stream.bytes.size(), header.data(), header.size(), measure.bind());
	CHECK(got.status == RoundRecoveryStatus::Repaired);
	CHECK(SameLayout(got.samples, stream.truth, stream.truth.size()));
	CHECK(got.samples.size() > 2 && got.samples[2].size == 300 && got.samples[2].track == 1);
}

// P3-2: in the last 8 KB the lookahead still holds. An audio frame that reads
// as a small length and a slice header, near the end of the data, is not
// taken for video.
void TestLookaheadNearEnd() {
	Section = "lookahead near end";
	auto stream = BuilderStream(false);
	auto random = Random{ 9 };
	for (auto i = 0; i != 20; ++i) {
		stream.write(0, VideoSample(i == 0, 700, random), i * 33333, i == 0);
		stream.write(1, AudioFrame(180, random), AudioPts(i), true);
	}
	auto trick = Bytes(300, 0xFF);
	trick[3] = 100;
	trick[4] = 0x41;
	trick[5] = 0x88;
	trick[0] = trick[1] = trick[2] = 0;
	stream.write(1, trick, AudioPts(20), true);
	stream.write(1, AudioFrame(180, random), AudioPts(21), true);
	const auto config = Config(false);
	const auto measure = MeasureFor(stream, stream.bytes);
	const auto header = EncodeRoundJournalStart(config);
	const auto tail = stream.bytes.size() - stream.truth[stream.truth.size() - 2].offset;
	CHECK(tail < kRoundMaxAacFrame);
	const auto got = RecoverRound(stream.bytes.data(), stream.bytes.size(), header.data(), header.size(), measure.bind());
	CHECK(got.status == RoundRecoveryStatus::Repaired);
	CHECK(SameLayout(got.samples, stream.truth, stream.truth.size()));
}

void TestStops() {
	Section = "stops";
	auto stream = Record(false, { { 30, 2000 } });
	const auto config = Config(false);
	const auto header = EncodeRoundJournalStart(config);

	// Garbage in the middle: everything before it, reported partial.
	auto broken = stream.bytes;
	const auto middle = stream.truth[stream.truth.size() / 2];
	std::fill(broken.begin() + middle.offset, broken.begin() + middle.offset + 64, std::uint8_t(0xEE));
	auto measure = MeasureFor(stream, broken);
	measure.frames.erase(middle.offset);
	const auto partial = RecoverRound(broken.data(), broken.size(), header.data(), header.size(), measure.bind());
	CHECK(partial.status == RoundRecoveryStatus::Partial);
	CHECK(partial.scanEnd == middle.offset);
	CHECK(partial.unparsedBytes == broken.size() - middle.offset);
	CHECK(SameLayout(partial.samples, stream.truth, stream.truth.size() / 2));

	// A finished file: the scan stops at moov and is not partial.
	auto finished = stream.bytes;
	const std::uint8_t moov[] = { 0, 0, 0x40, 0, 'm', 'o', 'o', 'v' };
	finished.insert(finished.end(), moov, moov + 8);
	finished.resize(finished.size() + 0x4000 - 8, 0x33);
	const auto finishedMeasure = MeasureFor(stream, finished);
	const auto done = RecoverRound(finished.data(), finished.size(), header.data(), header.size(), finishedMeasure.bind());
	CHECK(done.status == RoundRecoveryStatus::Repaired);
	CHECK(done.samples.size() == stream.truth.size());
	CHECK(done.scanEnd == stream.bytes.size());

	// Re-check N1: a crash tearing the audio frame after the last whole video
	// sample drops that sample (the lookahead's documented price) but is a
	// torn tail, so Repaired, also when sample plus tail pass 8 KB.
	for (const auto n : { 4000, 12000 }) {
		auto torn = BuilderStream(false);
		auto random = Random{ 21 };
		torn.write(0, VideoSample(true, 5000, random), 0, true);
		torn.write(1, AudioFrame(300, random), AudioPts(0), true);
		torn.write(1, AudioFrame(300, random), AudioPts(1), true);
		torn.write(0, VideoSample(false, std::size_t(n), random), 33333, false);
		torn.write(1, AudioFrame(300, random), AudioPts(2), true);
		const auto cut = std::size_t(torn.truth.back().offset + 50);
		const auto copy = Bytes(torn.bytes.begin(), torn.bytes.begin() + cut);
		const auto local = MeasureFor(torn, copy);
		const auto got = RecoverRound(copy.data(), copy.size(), header.data(), header.size(), local.bind());
		CHECK(got.status == RoundRecoveryStatus::Repaired);
		CHECK(SameLayout(got.samples, torn.truth, 3));
		CHECK(got.unparsedBytes == std::uint64_t(n) + 50);
	}

	// No config header: unrecoverable, whatever the media holds.
	const auto good = MeasureFor(stream, stream.bytes);
	const auto none = RecoverRound(stream.bytes.data(), stream.bytes.size(), nullptr, 0, good.bind());
	CHECK(none.status == RoundRecoveryStatus::Unrecoverable);
	CHECK(none.reason == RoundUnrecoverable::NoConfig);
	CHECK(none.samples.empty());

	auto later = header;
	later[4] = 9;
	const auto version = RecoverRound(stream.bytes.data(), stream.bytes.size(), later.data(), later.size(), good.bind());
	CHECK(version.reason == RoundUnrecoverable::UnknownVersion);

	// A header without tracks is no config either.
	auto bare = config;
	bare.tracks.clear();
	const auto empty = EncodeRoundJournalStart(bare);
	CHECK(RecoverRound(stream.bytes.data(), stream.bytes.size(), empty.data(), empty.size(), good.bind()).reason
		== RoundUnrecoverable::NoConfig);

	// Not an MP4.
	const auto noise = Bytes(5000, 0x21);
	const auto notMp4 = RecoverRound(noise.data(), noise.size(), header.data(), header.size(), good.bind());
	CHECK(notMp4.reason == RoundUnrecoverable::NotMp4);

	// ftyp and an mdat header, then nothing.
	const auto bareFile = Bytes(stream.bytes.begin(), stream.bytes.begin() + 24 + 16);
	const auto nothing = RecoverRound(bareFile.data(), bareFile.size(), header.data(), header.size(), good.bind());
	CHECK(nothing.status == RoundRecoveryStatus::Unrecoverable);
	CHECK(nothing.reason == RoundUnrecoverable::NoSamples);

	// No measure: nothing past the first audio frame. This stream starts
	// with one, so nothing at all.
	CHECK(stream.truth.front().track == 1);
	const auto deaf = RecoverRound(stream.bytes.data(), stream.bytes.size(), header.data(), header.size(), AacFrameMeasure());
	CHECK(deaf.status == RoundRecoveryStatus::Unrecoverable);
	CHECK(deaf.reason == RoundUnrecoverable::NoSamples);

	// With video first, the video the lookahead can vouch for is kept (the
	// second frame is followed by audio, which nothing can measure) and the
	// rest is partial.
	auto videoFirst = BuilderStream(false);
	auto random = Random{ 5 };
	videoFirst.write(0, VideoSample(true, 3000, random), 0, true);
	videoFirst.write(0, VideoSample(false, 800, random), 33333, false);
	for (auto i = 0; i != 60; ++i) {
		videoFirst.write(1, AudioFrame(200, random), AudioPts(i), true);
	}
	const auto half = RecoverRound(videoFirst.bytes.data(), videoFirst.bytes.size(), header.data(), header.size(), AacFrameMeasure());
	CHECK(half.status == RoundRecoveryStatus::Partial);
	CHECK(SameLayout(half.samples, videoFirst.truth, 1));
	CHECK(half.scanEnd == videoFirst.truth[1].offset);
}

void TestAvcc() {
	Section = "avcc";
	// Desktop's muxer: AVCC access units, SEI and SPS/PPS in front of the
	// IDR picture, and a picture in two slices.
	auto stream = BuilderStream(false, true);
	auto random = Random{ 11 };
	auto audio = 0;
	for (auto frame = 0; frame != 60; ++frame) {
		const auto time = std::int64_t(frame) * 33333;
		while (AudioPts(audio) <= time) {
			stream.write(1, AudioFrame(170, random), AudioPts(audio), true);
			++audio;
		}
		auto unit = Bytes();
		const auto append = [&](const Bytes &nal) { unit.insert(unit.end(), nal.begin(), nal.end()); };
		const auto idr = !(frame % 20);
		if (idr) {
			append(Nal(0x06, 30, random));
			append(Nal(0x67, 10, random));
			append(Nal(0x68, 4, random));
			append(Nal(0x65, 900, random));
			append(Nal(0x65, 700, random, false));
		} else {
			append(Nal(0x41, 400 + frame * 7, random));
		}
		stream.write(0, unit, time, idr);
	}
	const auto config = Config(false, true);
	const auto measure = MeasureFor(stream, stream.bytes);
	const auto header = EncodeRoundJournalStart(config);
	const auto got = RecoverRound(stream.bytes.data(), stream.bytes.size(), header.data(), header.size(), measure.bind());
	CHECK(got.status == RoundRecoveryStatus::Repaired);
	CHECK(SameLayout(got.samples, stream.truth, stream.truth.size()));

	// The same bytes read as MP4Builder framing would split the first IDR
	// unit at its SEI, so the flag matters.
	auto single = config;
	single.avccVideo = false;
	const auto wrongHeader = EncodeRoundJournalStart(single);
	const auto wrong = RecoverRound(stream.bytes.data(), stream.bytes.size(), wrongHeader.data(), wrongHeader.size(), measure.bind());
	CHECK(!SameLayout(wrong.samples, stream.truth, stream.truth.size()));
}

} // namespace

int main() {
	TestJournalFormat();
	TestJournalDamage();
	TestHostileJournal();
	TestRecovery();
	TestVariableFrameRate();
	TestJournalPlusTail();
	TestLookahead();
	TestLookaheadNearEnd();
	TestStops();
	TestAvcc();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
