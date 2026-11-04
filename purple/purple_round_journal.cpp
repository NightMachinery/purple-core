/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_round_journal.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace Purple {
namespace {

constexpr auto kFileHeaderSize = std::size_t(8);
constexpr auto kRecordHeadSize = std::size_t(8);
constexpr auto kRecordCrcSize = std::size_t(4);
constexpr auto kSampleRecordSize = std::size_t(24);
constexpr auto kMaxPayload = std::uint32_t(16 * 1024 * 1024);
constexpr auto kMaxBatch = std::uint32_t(65536);
constexpr auto kMaxTracks = std::size_t(8);
constexpr auto kMaxCsd = std::size_t(4);
constexpr auto kMaxCsdSize = std::size_t(64 * 1024);

constexpr auto kRecordHeader = std::uint8_t(1);
constexpr auto kRecordTrack = std::uint8_t(2);
constexpr auto kRecordBatch = std::uint8_t(3);

constexpr auto kFlagSplitMdat = std::uint16_t(0x0001);
constexpr auto kFlagAvccVideo = std::uint16_t(0x0002);
constexpr auto kSampleSync = std::uint8_t(0x01);

[[nodiscard]] const std::array<std::uint32_t, 256> &CrcTable() {
	static const auto result = [] {
		auto table = std::array<std::uint32_t, 256>();
		for (auto i = std::uint32_t(0); i != 256; ++i) {
			auto value = i;
			for (auto bit = 0; bit != 8; ++bit) {
				value = (value & 1U) ? ((value >> 1) ^ 0xEDB88320U) : (value >> 1);
			}
			table[i] = value;
		}
		return table;
	}();
	return result;
}

class Writer {
public:
	void u8(std::uint8_t value) {
		_bytes.push_back(value);
	}
	void u16(std::uint16_t value) {
		u8(std::uint8_t(value));
		u8(std::uint8_t(value >> 8));
	}
	void u32(std::uint32_t value) {
		u16(std::uint16_t(value));
		u16(std::uint16_t(value >> 16));
	}
	void u64(std::uint64_t value) {
		u32(std::uint32_t(value));
		u32(std::uint32_t(value >> 32));
	}
	void raw(const std::uint8_t *data, std::size_t size) {
		_bytes.insert(_bytes.end(), data, data + size);
	}
	[[nodiscard]] std::vector<std::uint8_t> &bytes() {
		return _bytes;
	}

private:
	std::vector<std::uint8_t> _bytes;

};

class Reader {
public:
	Reader(const std::uint8_t *data, std::size_t size)
	: _data(data)
	, _size(size) {
	}

	[[nodiscard]] bool has(std::size_t count) const {
		return _size - _position >= count;
	}
	[[nodiscard]] std::size_t position() const {
		return _position;
	}
	[[nodiscard]] std::uint8_t u8() {
		return _data[_position++];
	}
	[[nodiscard]] std::uint16_t u16() {
		const auto low = u8();
		return std::uint16_t(low | (std::uint16_t(u8()) << 8));
	}
	[[nodiscard]] std::uint32_t u32() {
		const auto low = u16();
		return std::uint32_t(low) | (std::uint32_t(u16()) << 16);
	}
	[[nodiscard]] std::uint64_t u64() {
		const auto low = u32();
		return std::uint64_t(low) | (std::uint64_t(u32()) << 32);
	}
	[[nodiscard]] const std::uint8_t *take(std::size_t count) {
		const auto result = _data + _position;
		_position += count;
		return result;
	}

private:
	const std::uint8_t *_data = nullptr;
	std::size_t _size = 0;
	std::size_t _position = 0;

};

// Wraps a payload into a record: type, three zero bytes, the payload length,
// the payload, and the CRC-32 of everything before the CRC.
void AppendRecord(
		std::vector<std::uint8_t> &out,
		std::uint8_t type,
		const std::vector<std::uint8_t> &payload) {
	const auto start = out.size();
	auto head = Writer();
	head.u8(type);
	head.u8(0);
	head.u16(0);
	head.u32(std::uint32_t(payload.size()));
	out.insert(out.end(), head.bytes().begin(), head.bytes().end());
	out.insert(out.end(), payload.begin(), payload.end());
	auto crc = Writer();
	crc.u32(RoundJournalCrc(out.data() + start, out.size() - start));
	out.insert(out.end(), crc.bytes().begin(), crc.bytes().end());
}

[[nodiscard]] bool ReadTrack(
		const std::uint8_t *payload,
		std::size_t size,
		std::size_t expectedIndex,
		RoundTrack &track) {
	auto reader = Reader(payload, size);
	if (!reader.has(4)) {
		return false;
	}
	const auto index = reader.u8();
	const auto kind = reader.u8();
	const auto mimeSize = reader.u8();
	const auto csdCount = reader.u8();
	if (index != expectedIndex
		|| (kind != std::uint8_t(RoundTrackKind::Video)
			&& kind != std::uint8_t(RoundTrackKind::Audio))
		|| csdCount > kMaxCsd
		|| !reader.has(std::size_t(mimeSize) + 12)) {
		return false;
	}
	track.kind = RoundTrackKind(kind);
	const auto mime = reader.take(mimeSize);
	track.mime.assign(reinterpret_cast<const char*>(mime), mimeSize);
	track.timescale = reader.u32();
	const auto a = reader.u32();
	const auto b = reader.u32();
	if (track.kind == RoundTrackKind::Video) {
		track.width = a;
		track.height = b;
	} else {
		track.sampleRate = a;
		track.channels = b;
	}
	for (auto i = 0; i != csdCount; ++i) {
		if (!reader.has(4)) {
			return false;
		}
		const auto length = reader.u32();
		if (length > kMaxCsdSize || !reader.has(length)) {
			return false;
		}
		const auto bytes = reader.take(length);
		track.csd.emplace_back(bytes, bytes + length);
	}
	return reader.position() == size;
}

} // namespace

int RoundConfig::videoTrack() const {
	for (auto i = std::size_t(0); i != tracks.size(); ++i) {
		if (tracks[i].kind == RoundTrackKind::Video) {
			return int(i);
		}
	}
	return -1;
}

int RoundConfig::audioTrack() const {
	for (auto i = std::size_t(0); i != tracks.size(); ++i) {
		if (tracks[i].kind == RoundTrackKind::Audio) {
			return int(i);
		}
	}
	return -1;
}

std::uint32_t RoundJournalCrc(const std::uint8_t *data, std::size_t size) {
	const auto &table = CrcTable();
	auto crc = 0xFFFFFFFFU;
	for (auto i = std::size_t(0); i != size; ++i) {
		crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	}
	return crc ^ 0xFFFFFFFFU;
}

std::vector<std::uint8_t> EncodeRoundJournalStart(const RoundConfig &config) {
	auto out = std::vector<std::uint8_t>{ 'P', 'J', 'R', '1' };
	auto version = Writer();
	version.u16(kRoundJournalVersion);
	version.u16(0);
	out.insert(out.end(), version.bytes().begin(), version.bytes().end());

	auto header = Writer();
	header.u16(std::uint16_t((config.splitMdat ? kFlagSplitMdat : 0)
		| (config.avccVideo ? kFlagAvccVideo : 0)));
	header.u16(config.rotation);
	header.u32(config.width);
	header.u32(config.height);
	header.u32(0);
	AppendRecord(out, kRecordHeader, header.bytes());

	for (auto i = std::size_t(0); i != config.tracks.size(); ++i) {
		const auto track = EncodeRoundJournalTrack(
			std::uint8_t(i),
			config.tracks[i]);
		out.insert(out.end(), track.begin(), track.end());
	}
	return out;
}

std::vector<std::uint8_t> EncodeRoundJournalTrack(
		std::uint8_t index,
		const RoundTrack &track) {
	auto payload = Writer();
	const auto mimeSize = std::min(track.mime.size(), std::size_t(255));
	const auto csdCount = std::min(track.csd.size(), kMaxCsd);
	payload.u8(index);
	payload.u8(std::uint8_t(track.kind));
	payload.u8(std::uint8_t(mimeSize));
	payload.u8(std::uint8_t(csdCount));
	payload.raw(
		reinterpret_cast<const std::uint8_t*>(track.mime.data()),
		mimeSize);
	payload.u32(track.timescale);
	const auto video = (track.kind == RoundTrackKind::Video);
	payload.u32(video ? track.width : track.sampleRate);
	payload.u32(video ? track.height : track.channels);
	for (auto i = std::size_t(0); i != csdCount; ++i) {
		const auto &csd = track.csd[i];
		const auto size = std::min(csd.size(), kMaxCsdSize);
		payload.u32(std::uint32_t(size));
		payload.raw(csd.data(), size);
	}
	auto out = std::vector<std::uint8_t>();
	AppendRecord(out, kRecordTrack, payload.bytes());
	return out;
}

std::vector<std::uint8_t> EncodeRoundJournalBatch(
		const std::vector<RoundSample> &samples) {
	auto out = std::vector<std::uint8_t>();
	for (auto from = std::size_t(0); from < samples.size(); from += kMaxBatch) {
		const auto till = std::min(samples.size(), from + kMaxBatch);
		auto payload = Writer();
		payload.u32(std::uint32_t(till - from));
		for (auto i = from; i != till; ++i) {
			const auto &sample = samples[i];
			payload.u8(sample.track);
			payload.u8(sample.sync ? kSampleSync : 0);
			payload.u16(0);
			payload.u32(sample.size);
			payload.u64(sample.offset);
			payload.u64(std::uint64_t(sample.ptsUs));
		}
		AppendRecord(out, kRecordBatch, payload.bytes());
	}
	return out;
}

RoundJournal ReadRoundJournal(const std::uint8_t *data, std::size_t size) {
	auto result = RoundJournal();
	if (size < kFileHeaderSize || std::memcmp(data, "PJR1", 4)) {
		return result;
	}
	auto reader = Reader(data, size);
	(void)reader.take(4);
	if (reader.u16() != kRoundJournalVersion) {
		result.status = RoundJournalStatus::UnknownVersion;
		return result;
	}
	(void)reader.u16();
	result.status = RoundJournalStatus::NoConfig;
	result.goodBytes = kFileHeaderSize;
	auto headerRead = false;
	while (reader.position() < size) {
		const auto start = reader.position();
		if (!reader.has(kRecordHeadSize)) {
			result.damaged = true;
			break;
		}
		const auto type = reader.u8();
		(void)reader.take(3);
		const auto length = reader.u32();
		if (length > kMaxPayload || !reader.has(std::size_t(length) + kRecordCrcSize)) {
			result.damaged = true;
			break;
		}
		const auto payload = reader.take(length);
		const auto crc = reader.u32();
		if (crc != RoundJournalCrc(data + start, kRecordHeadSize + length)) {
			result.damaged = true;
			break;
		}
		if (!headerRead) {
			if (type != kRecordHeader || length < 16) {
				result.damaged = true;
				break;
			}
			auto header = Reader(payload, length);
			const auto flags = header.u16();
			result.config.splitMdat = (flags & kFlagSplitMdat) != 0;
			result.config.avccVideo = (flags & kFlagAvccVideo) != 0;
			result.config.rotation = header.u16();
			result.config.width = header.u32();
			result.config.height = header.u32();
			headerRead = true;
			result.status = RoundJournalStatus::Ok;
		} else if (type == kRecordTrack) {
			auto track = RoundTrack();
			if (result.config.tracks.size() >= kMaxTracks
				|| !ReadTrack(
					payload,
					length,
					result.config.tracks.size(),
					track)) {
				result.damaged = true;
				break;
			}
			result.config.tracks.push_back(std::move(track));
		} else if (type == kRecordBatch) {
			auto batch = Reader(payload, length);
			const auto count = (length >= 4) ? batch.u32() : 0;
			if (!count
				|| count > kMaxBatch
				|| length != 4 + count * kSampleRecordSize) {
				result.damaged = true;
				break;
			}
			auto samples = std::vector<RoundSample>();
			samples.reserve(count);
			auto valid = true;
			for (auto i = std::uint32_t(0); i != count; ++i) {
				auto sample = RoundSample();
				sample.track = batch.u8();
				sample.sync = (batch.u8() & kSampleSync) != 0;
				(void)batch.u16();
				sample.size = batch.u32();
				sample.offset = batch.u64();
				sample.ptsUs = std::int64_t(batch.u64());
				// A sample whose end does not fit 64 bits is damage, so no
				// reader of samples can meet an offset + size that wraps.
				if (sample.track >= result.config.tracks.size()
					|| sample.offset > UINT64_MAX - sample.size) {
					valid = false;
					break;
				}
				samples.push_back(sample);
			}
			if (!valid) {
				result.damaged = true;
				break;
			}
			result.samples.insert(
				result.samples.end(),
				samples.begin(),
				samples.end());
			++result.batches;
		}
		// Any other record type is a later addition of the same version:
		// its CRC was good, so it is skipped.
		result.goodBytes = reader.position();
	}
	return result;
}

} // namespace Purple
