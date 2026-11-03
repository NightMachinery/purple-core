/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
// A small deterministic fuzz loop over the three recording parsers: the Ogg
// finaliser, the round video journal reader and the MP4 repair. Each starts
// from a valid synthetic input, mutates it (truncation, byte flips, junk,
// random bytes, and for the journal well-formed records with hostile values
// and good CRCs) and checks invariants that must hold for any input. Seeded,
// so a failure reproduces; sized to run in a few seconds at -O0. Build it
// with -fsanitize=address,undefined to turn every invariant into a memory and
// overflow check as well.
#include "purple/purple_mp4_repair.h"
#include "purple/purple_ogg_repair.h"
#include "purple/purple_round_journal.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <random>

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
using Engine = std::mt19937_64;

constexpr auto kIterations = 3000;

[[nodiscard]] std::uint64_t Below(Engine &engine, std::uint64_t bound) {
	return bound ? (engine() % bound) : 0;
}

// --- Ogg ---

void PutLe32(Bytes &out, std::size_t at, std::uint32_t value) {
	for (auto i = 0; i != 4; ++i) {
		out[at + i] = std::uint8_t(value >> (8 * i));
	}
}

void OggPageOut(Bytes &out, std::uint8_t flags, std::int64_t granule, std::uint32_t sequence, const Bytes &packet) {
	const auto start = out.size();
	const char magic[] = "OggS";
	out.insert(out.end(), magic, magic + 4);
	out.push_back(0);
	out.push_back(flags);
	out.resize(out.size() + 20);
	PutLe32(out, start + 6, std::uint32_t(std::uint64_t(granule)));
	PutLe32(out, start + 10, std::uint32_t(std::uint64_t(granule) >> 32));
	PutLe32(out, start + 14, 0x0BADF00DU);
	PutLe32(out, start + 18, sequence);
	auto left = packet.size();
	auto lacing = Bytes();
	while (left >= 255) {
		lacing.push_back(255);
		left -= 255;
	}
	lacing.push_back(std::uint8_t(left));
	out.push_back(std::uint8_t(lacing.size()));
	out.insert(out.end(), lacing.begin(), lacing.end());
	out.insert(out.end(), packet.begin(), packet.end());
	PutLe32(out, start + 22, OggCrc(out.data() + start, out.size() - start));
}

[[nodiscard]] Bytes OggStream(Engine &engine) {
	auto out = Bytes();
	auto head = Bytes{ 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 1, 0x38, 0x01, 0x80, 0xBB, 0, 0, 0, 0, 0 };
	OggPageOut(out, 0x02, 0, 0, head);
	OggPageOut(out, 0, 0, 1, Bytes{ 'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 0, 0, 0, 0, 0, 0, 0, 0 });
	const auto packets = 1 + int(Below(engine, 40));
	const auto eos = Below(engine, 2) == 0;
	for (auto i = 0; i != packets; ++i) {
		auto packet = Bytes(20 + Below(engine, 300));
		for (auto &byte : packet) {
			byte = std::uint8_t(engine());
		}
		const auto last = (i + 1 == packets);
		OggPageOut(out, (last && eos) ? 0x04 : 0, 312 + std::int64_t(i + 1) * 960, std::uint32_t(2 + i), packet);
	}
	return out;
}

// Truncation, flips, junk or noise, chosen at random.
void Mutate(Engine &engine, Bytes &bytes) {
	switch (Below(engine, 5)) {
	case 0:
		bytes.resize(std::size_t(Below(engine, bytes.size() + 1)));
		break;
	case 1:
		for (auto i = Below(engine, 8) + 1; i != 0 && !bytes.empty(); --i) {
			bytes[std::size_t(Below(engine, bytes.size()))] ^= std::uint8_t(1 + Below(engine, 255));
		}
		break;
	case 2:
		for (auto i = Below(engine, 600); i != 0; --i) {
			bytes.push_back(std::uint8_t(engine()));
		}
		break;
	case 3:
		for (auto &byte : bytes) {
			byte = std::uint8_t(engine());
		}
		break;
	default:
		break;
	}
}

void FuzzOgg(Engine &engine) {
	Section = "fuzz ogg";
	auto ok = true;
	for (auto i = 0; i != kIterations; ++i) {
		auto bytes = OggStream(engine);
		Mutate(engine, bytes);
		const auto original = bytes;
		const auto scan = ScanOgg(bytes.data(), bytes.size());
		ok = ok && scan.goodBytes <= bytes.size();
		const auto finish = FinaliseOgg(bytes);
		switch (finish.status) {
		case OggFinishStatus::Unrecoverable:
		case OggFinishStatus::Clean:
			ok = ok && (bytes == original);
			break;
		case OggFinishStatus::Finalised: {
			ok = ok && bytes.size() <= original.size()
				&& finish.scan.clean()
				&& finish.scan.pages.back().granule >= 0;
			auto again = bytes;
			const auto second = FinaliseOgg(again);
			ok = ok && second.status == OggFinishStatus::Clean
				&& again == bytes
				&& second.scan.durationMs == finish.scan.durationMs;
		} break;
		}
	}
	CHECK(ok);
}

// --- Round video ---

void PutBe32(Bytes &out, std::size_t at, std::uint32_t value) {
	out[at] = std::uint8_t(value >> 24);
	out[at + 1] = std::uint8_t(value >> 16);
	out[at + 2] = std::uint8_t(value >> 8);
	out[at + 3] = std::uint8_t(value);
}

[[nodiscard]] RoundConfig FuzzConfig(bool split) {
	auto config = RoundConfig();
	config.splitMdat = split;
	auto video = RoundTrack();
	video.kind = RoundTrackKind::Video;
	video.mime = "video/avc";
	video.timescale = 90000;
	video.csd = { { 0x67, 0x42 }, { 0x68, 0xCE } };
	auto audio = RoundTrack();
	audio.kind = RoundTrackKind::Audio;
	audio.mime = "audio/mp4a-latm";
	audio.timescale = 48000;
	audio.sampleRate = 48000;
	audio.channels = 1;
	audio.csd = { { 0x11, 0x88 } };
	config.tracks = { video, audio };
	return config;
}

// Audio frames here describe themselves (0x21, then half their size), so the
// stub measure needs no table and survives any mutation. It reads two bytes
// at most and never past `available'.
[[nodiscard]] int StubMeasure(const RoundTrack &, const std::uint8_t *data, std::size_t available) {
	if (available < 2 || data[0] != 0x21) {
		return 0;
	}
	const auto size = std::size_t(data[1]) * 2;
	return (size >= 2 && size <= available) ? int(size) : 0;
}

struct Round {
	Bytes media;
	std::vector<RoundSample> samples;
};

[[nodiscard]] Round RoundStream(Engine &engine, bool split) {
	auto result = Round();
	const std::uint8_t ftyp[] = { 0, 0, 0, 16, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 2, 0 };
	result.media.assign(ftyp, ftyp + sizeof(ftyp));
	auto mdatAt = result.media.size();
	auto since = std::size_t(0);
	const auto mdat = [&] {
		mdatAt = result.media.size();
		const std::uint8_t header[] = { 0, 0, 0, 16, 'm', 'd', 'a', 't', 0, 0, 0, 0, 0, 0, 0, 0 };
		result.media.insert(result.media.end(), header, header + 16);
	};
	mdat();
	auto audio = 0;
	const auto frames = 5 + int(Below(engine, 25));
	for (auto frame = 0; frame != frames; ++frame) {
		const auto time = std::int64_t(frame) * 33333;
		while (std::int64_t(audio) * 21333 <= time) {
			auto bytes = Bytes(2 * (60 + Below(engine, 140)));
			for (auto &byte : bytes) {
				byte = std::uint8_t(engine());
			}
			bytes[0] = 0x21;
			bytes[1] = std::uint8_t(bytes.size() / 2);
			result.samples.push_back({ 1, true, std::uint32_t(bytes.size()), result.media.size(), std::int64_t(audio) * 21333 });
			result.media.insert(result.media.end(), bytes.begin(), bytes.end());
			since += bytes.size();
			++audio;
		}
		auto bytes = Bytes(200 + Below(engine, 2000));
		for (auto &byte : bytes) {
			byte = std::uint8_t(engine());
		}
		PutBe32(bytes, 0, std::uint32_t(bytes.size() - 4));
		bytes[4] = (frame == 0) ? 0x65 : 0x41;
		bytes[5] |= 0x80;
		result.samples.push_back({ 0, frame == 0, std::uint32_t(bytes.size()), result.media.size(), time });
		result.media.insert(result.media.end(), bytes.begin(), bytes.end());
		since += bytes.size();
		if (split && since >= 8 * 1024) {
			result.media[mdatAt + 3] = 1;
			since = 0;
			mdat();
		}
	}
	return result;
}

// One sample field set to an extreme or random value; the batch is then
// encoded again, so its CRC is good.
void Hostile(Engine &engine, std::vector<RoundSample> &samples) {
	if (samples.empty()) {
		return;
	}
	static constexpr std::uint64_t kExtremes[] = {
		0, 1, 0xFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL,
		0xFFFFFFFFFFFFFFF0ULL, 0xFFFFFFFFFFFFFFFFULL,
	};
	auto &sample = samples[std::size_t(Below(engine, samples.size()))];
	const auto value = Below(engine, 2)
		? kExtremes[Below(engine, sizeof(kExtremes) / sizeof(kExtremes[0]))]
		: std::uint64_t(engine());
	switch (Below(engine, 4)) {
	case 0: sample.offset = value; break;
	case 1: sample.size = std::uint32_t(value); break;
	case 2: sample.ptsUs = std::int64_t(value); break;
	default: sample.track = std::uint8_t(value); break;
	}
}

void FuzzRound(Engine &engine) {
	Section = "fuzz round";
	auto inside = true;
	auto bounded = true;
	auto timed = true;
	auto counted = true;
	for (auto i = 0; i != kIterations; ++i) {
		const auto split = Below(engine, 2) == 0;
		const auto config = FuzzConfig(split);
		auto round = RoundStream(engine, split);
		auto journaled = std::vector<RoundSample>(
			round.samples.begin(),
			round.samples.begin() + std::ptrdiff_t(Below(engine, round.samples.size() + 1)));
		const auto mode = Below(engine, 4);
		if (mode == 0) {
			Hostile(engine, journaled);
			Hostile(engine, journaled);
		}
		auto journal = EncodeRoundJournalStart(config);
		const auto batch = EncodeRoundJournalBatch(journaled);
		journal.insert(journal.end(), batch.begin(), batch.end());
		if (mode == 1) {
			Mutate(engine, journal);
		} else if (mode == 2) {
			Mutate(engine, round.media);
		}

		const auto read = ReadRoundJournal(journal.data(), journal.size());
		counted = counted && read.goodBytes <= journal.size();
		for (const auto &sample : read.samples) {
			bounded = bounded
				&& sample.track < read.config.tracks.size()
				&& sample.offset <= std::numeric_limits<std::uint64_t>::max() - sample.size;
		}

		const auto got = RecoverRound(round.media.data(), round.media.size(), journal.data(), journal.size(), StubMeasure);
		bounded = bounded
			&& got.scanEnd <= round.media.size()
			&& got.unparsedBytes <= round.media.size()
			&& got.exactSamples + got.repairedSamples == got.samples.size()
			&& got.exactSamples + got.journalDropped <= read.samples.size()
			&& (got.status == RoundRecoveryStatus::Unrecoverable || !got.samples.empty());
		for (const auto &sample : got.samples) {
			inside = inside
				&& sample.offset <= round.media.size()
				&& sample.size <= round.media.size() - sample.offset
				&& sample.size > 0;
			timed = timed
				&& sample.ptsUs >= 0
				&& sample.ptsUs < 2 * kRoundMaxPtsUs;
		}
	}
	CHECK(inside);
	CHECK(bounded);
	CHECK(timed);
	CHECK(counted);
}

} // namespace

int main() {
	auto engine = Engine(0x5EED5EEDULL);
	FuzzOgg(engine);
	FuzzRound(engine);
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
