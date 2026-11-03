/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_ogg_repair.h"

#include "purple/purple_recording_file.h"

#include <array>
#include <cstring>

namespace Purple {
namespace {

constexpr auto kHeaderSize = std::size_t(27);
constexpr auto kCrcOffset = std::size_t(22);
constexpr auto kFlagsOffset = std::size_t(5);

[[nodiscard]] const std::array<std::uint32_t, 256> &CrcTable() {
	static const auto result = [] {
		auto table = std::array<std::uint32_t, 256>();
		for (auto i = std::uint32_t(0); i != 256; ++i) {
			auto value = i << 24;
			for (auto bit = 0; bit != 8; ++bit) {
				value = (value & 0x80000000U)
					? ((value << 1) ^ 0x04C11DB7U)
					: (value << 1);
			}
			table[i] = value;
		}
		return table;
	}();
	return result;
}

[[nodiscard]] std::uint32_t CrcUpdate(
		std::uint32_t crc,
		const std::uint8_t *data,
		std::size_t size) {
	const auto &table = CrcTable();
	for (auto i = std::size_t(0); i != size; ++i) {
		crc = (crc << 8) ^ table[((crc >> 24) ^ data[i]) & 0xFF];
	}
	return crc;
}

[[nodiscard]] std::uint32_t Read32(const std::uint8_t *data) {
	return std::uint32_t(data[0])
		| (std::uint32_t(data[1]) << 8)
		| (std::uint32_t(data[2]) << 16)
		| (std::uint32_t(data[3]) << 24);
}

void Write32(std::uint8_t *data, std::uint32_t value) {
	data[0] = std::uint8_t(value);
	data[1] = std::uint8_t(value >> 8);
	data[2] = std::uint8_t(value >> 16);
	data[3] = std::uint8_t(value >> 24);
}

// The CRC of a page with its CRC field taken as zero.
[[nodiscard]] std::uint32_t PageCrc(
		const std::uint8_t *page,
		std::size_t size) {
	static constexpr std::uint8_t kZero[4] = { 0, 0, 0, 0 };
	auto crc = CrcUpdate(0, page, kCrcOffset);
	crc = CrcUpdate(crc, kZero, 4);
	return CrcUpdate(crc, page + kCrcOffset + 4, size - kCrcOffset - 4);
}

// The page starting at `offset', if it is whole and its CRC matches.
[[nodiscard]] bool ReadPage(
		const std::uint8_t *data,
		std::size_t size,
		std::size_t offset,
		OggPage &page) {
	if (size - offset < kHeaderSize) {
		return false;
	}
	const auto header = data + offset;
	if (std::memcmp(header, "OggS", 4) || header[4] != 0) {
		return false;
	}
	const auto segments = std::size_t(header[26]);
	if (size - offset < kHeaderSize + segments) {
		return false;
	}
	auto body = std::size_t(0);
	for (auto i = std::size_t(0); i != segments; ++i) {
		body += header[kHeaderSize + i];
	}
	const auto total = kHeaderSize + segments + body;
	if (size - offset < total) {
		return false;
	} else if (PageCrc(header, total) != Read32(header + kCrcOffset)) {
		return false;
	}
	page.offset = offset;
	page.size = std::uint32_t(total);
	page.flags = header[kFlagsOffset];
	page.granule = std::int64_t(
		std::uint64_t(Read32(header + 6))
			| (std::uint64_t(Read32(header + 10)) << 32));
	page.serial = Read32(header + 14);
	page.sequence = Read32(header + 18);
	page.packetEnds = !segments || header[kHeaderSize + segments - 1] < 255;
	return true;
}

// OpusHead: "OpusHead", version, channels, pre-skip (LE16), ...
void ReadOpusHead(
		const std::uint8_t *data,
		const OggPage &first,
		OggScan &scan) {
	const auto header = data + first.offset;
	const auto segments = std::size_t(header[26]);
	const auto body = header + kHeaderSize + segments;
	const auto bodySize = first.size - kHeaderSize - segments;
	if (bodySize >= 19 && !std::memcmp(body, "OpusHead", 8)) {
		scan.opus = true;
		scan.preSkip = int(body[10]) | (int(body[11]) << 8);
	}
}

void Finish(OggScan &scan) {
	scan.lastGranule = -1;
	for (auto i = scan.pages.size(); i != 0; --i) {
		if (scan.pages[i - 1].granule >= 0) {
			scan.lastGranule = scan.pages[i - 1].granule;
			break;
		}
	}
	scan.endOfStream = !scan.pages.empty()
		&& (scan.pages.back().flags & OggPage::kEndOfStream);
	scan.goodBytes = scan.pages.empty()
		? 0
		: (scan.pages.back().offset + scan.pages.back().size);
	if (scan.opus && scan.lastGranule >= 0) {
		const auto samples = scan.lastGranule - scan.preSkip;
		scan.durationMs = (samples > 0) ? (samples / 48) : 0;
	} else {
		scan.durationMs = -1;
	}
}

} // namespace

std::uint32_t OggCrc(const std::uint8_t *data, std::size_t size) {
	return CrcUpdate(0, data, size);
}

OggScan ScanOgg(const std::uint8_t *data, std::size_t size) {
	auto result = OggScan();
	result.totalBytes = size;
	auto offset = std::size_t(0);
	auto page = OggPage();
	while (offset < size && ReadPage(data, size, offset, page)) {
		if (result.pages.empty()) {
			ReadOpusHead(data, page, result);
		} else {
			// One logical stream, numbered without gaps: a good page of
			// another stream, or one out of sequence (stale bytes of an
			// earlier recording in a reused file), ends the walk.
			const auto &previous = result.pages.back();
			if (page.serial != result.pages.front().serial
				|| page.sequence != previous.sequence + 1) {
				break;
			}
		}
		result.pages.push_back(page);
		offset += page.size;
	}
	Finish(result);
	return result;
}

OggFinish FinaliseOgg(std::vector<std::uint8_t> &bytes) {
	auto result = OggFinish();
	result.scan = ScanOgg(bytes.data(), bytes.size());
	if (result.scan.clean()) {
		result.status = OggFinishStatus::Clean;
		return result;
	} else if (result.scan.pages.empty() || !result.scan.hasAudio()) {
		result.status = OggFinishStatus::Unrecoverable;
		return result;
	}
	// The end-of-stream page must carry the final granule position (RFC
	// 7845 end trimming), so trailing pages on which no packet ends go too.
	// hasAudio() guarantees a page with a granule remains.
	auto &pages = result.scan.pages;
	while (pages.back().granule < 0) {
		pages.pop_back();
	}
	Finish(result.scan);
	result.status = OggFinishStatus::Finalised;
	result.droppedBytes = bytes.size() - result.scan.goodBytes;
	bytes.resize(std::size_t(result.scan.goodBytes));
	auto &last = result.scan.pages.back();
	if (!(last.flags & OggPage::kEndOfStream)) {
		const auto header = bytes.data() + last.offset;
		last.flags |= OggPage::kEndOfStream;
		header[kFlagsOffset] = last.flags;
		Write32(header + kCrcOffset, PageCrc(header, last.size));
		result.endOfStreamSet = true;
	}
	result.scan.totalBytes = bytes.size();
	Finish(result.scan);
	return result;
}

OggFinish FinaliseOggFile(const std::string &path) {
	auto bytes = std::vector<std::uint8_t>();
	if (!RecordingFile::ReadAll(path, bytes)) {
		auto result = OggFinish();
		result.ioError = true;
		return result;
	}
	auto result = FinaliseOgg(bytes);
	if (result.status != OggFinishStatus::Finalised) {
		return result;
	}
	// Only the last page's header changes, so write that header back and cut
	// the file rather than rewrite it whole.
	const auto &last = result.scan.pages.back();
	const auto ok = (!result.endOfStreamSet
			|| RecordingFile::WriteAt(
				path,
				last.offset,
				bytes.data() + last.offset,
				kHeaderSize))
		&& RecordingFile::Truncate(path, result.scan.goodBytes);
	if (!ok) {
		result.status = OggFinishStatus::Unrecoverable;
		result.ioError = true;
	}
	return result;
}

} // namespace Purple
