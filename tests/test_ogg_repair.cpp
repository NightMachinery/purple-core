/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_ogg_repair.h"

#include "purple/purple_recording_file.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
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

void Put32(Bytes &out, std::size_t at, std::uint32_t value) {
	for (auto i = 0; i != 4; ++i) {
		out[at + i] = std::uint8_t(value >> (8 * i));
	}
}

// One Ogg page holding the given packets, each laced in full on this page.
// `openTail' makes the last packet continue onto a next page (its lacing ends
// in 255, so its size must be a multiple of 255), as a page cut mid-packet
// would.
void AppendPage(
		Bytes &out,
		std::uint8_t flags,
		std::int64_t granule,
		std::uint32_t sequence,
		const std::vector<Bytes> &packets,
		bool openTail = false,
		std::uint32_t serial = 0x50555250U) {
	auto lacing = Bytes();
	auto body = Bytes();
	for (auto p = std::size_t(0); p != packets.size(); ++p) {
		const auto &packet = packets[p];
		auto left = packet.size();
		while (left >= 255) {
			lacing.push_back(255);
			left -= 255;
		}
		const auto last = (p + 1 == packets.size());
		if (!(last && openTail)) {
			lacing.push_back(std::uint8_t(left));
		}
		body.insert(body.end(), packet.begin(), packet.end());
	}
	const auto start = out.size();
	const char magic[] = "OggS";
	out.insert(out.end(), magic, magic + 4);
	out.push_back(0);
	out.push_back(flags);
	out.resize(out.size() + 8 + 4 + 4 + 4);
	Put32(out, start + 6, std::uint32_t(std::uint64_t(granule)));
	Put32(out, start + 10, std::uint32_t(std::uint64_t(granule) >> 32));
	Put32(out, start + 14, serial);
	Put32(out, start + 18, sequence);
	out.push_back(std::uint8_t(lacing.size()));
	out.insert(out.end(), lacing.begin(), lacing.end());
	out.insert(out.end(), body.begin(), body.end());
	// CRC over the page with a zero CRC field, which it still is here.
	Put32(out, start + 22, OggCrc(out.data() + start, out.size() - start));
}

[[nodiscard]] Bytes OpusHead(int preSkip) {
	auto result = Bytes{ 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 1 };
	result.push_back(std::uint8_t(preSkip));
	result.push_back(std::uint8_t(preSkip >> 8));
	result.insert(result.end(), { 0x80, 0xBB, 0, 0, 0, 0, 0 });
	return result;
}

[[nodiscard]] Bytes Packet(int index, int size) {
	auto result = Bytes(std::size_t(size));
	for (auto i = 0; i != size; ++i) {
		result[i] = std::uint8_t(index * 31 + i * 7);
	}
	return result;
}

// A voice file as the recorders write it: OpusHead (BOS), OpusTags, then one
// 20 ms packet (960 samples at 48 kHz) per page, the last page EOS.
[[nodiscard]] Bytes Voice(int packets, bool eos = true, int preSkip = 312) {
	auto out = Bytes();
	AppendPage(out, OggPage::kBeginOfStream, 0, 0, { OpusHead(preSkip) });
	const auto tags = Bytes{ 'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 0, 0, 0, 0, 0, 0, 0, 0 };
	AppendPage(out, 0, 0, 1, { tags });
	for (auto i = 0; i != packets; ++i) {
		const auto last = (i + 1 == packets);
		AppendPage(
			out,
			(last && eos) ? OggPage::kEndOfStream : 0,
			preSkip + std::int64_t(i + 1) * 960,
			std::uint32_t(2 + i),
			{ Packet(i, 40 + (i % 5) * 70) });
	}
	return out;
}

[[nodiscard]] bool AllPagesValid(const Bytes &bytes) {
	const auto scan = ScanOgg(bytes.data(), bytes.size());
	return scan.goodBytes == bytes.size();
}

void TestCrc() {
	Section = "crc";
	// The Ogg CRC of "123456789" (CRC-32/MPEG-2 without its init and xor).
	const auto text = std::string("123456789");
	CHECK(OggCrc(
		reinterpret_cast<const std::uint8_t*>(text.data()),
		text.size()) == 0x89A1897FU);
	CHECK(OggCrc(nullptr, 0) == 0);
}

void TestClean() {
	Section = "clean";
	auto bytes = Voice(50);
	const auto original = bytes;
	const auto scan = ScanOgg(bytes.data(), bytes.size());
	CHECK(scan.pages.size() == 52);
	CHECK(scan.clean());
	CHECK(scan.opus);
	CHECK(scan.preSkip == 312);
	CHECK(scan.hasAudio());
	CHECK(scan.pages.front().flags & OggPage::kBeginOfStream);
	CHECK(scan.pages.back().sequence == 51);
	const auto finish = FinaliseOgg(bytes);
	CHECK(finish.status == OggFinishStatus::Clean);
	CHECK(bytes == original);
	CHECK(finish.droppedBytes == 0);
	CHECK(!finish.endOfStreamSet);
}

void TestDuration() {
	Section = "duration";
	// 50 packets of 20 ms: 1000 ms after the pre-skip.
	auto bytes = Voice(50);
	CHECK(ScanOgg(bytes.data(), bytes.size()).durationMs == 1000);
	CHECK(ScanOgg(bytes.data(), bytes.size()).lastGranule == 312 + 50 * 960);

	auto zero = Voice(3, true, 0);
	CHECK(ScanOgg(zero.data(), zero.size()).durationMs == 60);

	// A pre-skip larger than the audio clamps to 0, not negative.
	auto skip = Bytes();
	AppendPage(skip, OggPage::kBeginOfStream, 0, 0, { OpusHead(2000) });
	AppendPage(skip, OggPage::kEndOfStream, 960, 1, { Packet(1, 50) });
	CHECK(ScanOgg(skip.data(), skip.size()).durationMs == 0);
	CHECK(ScanOgg(skip.data(), skip.size()).hasAudio());

	// A page on which no packet ends carries granule -1; the duration comes
	// from the last page that has one.
	auto open = Voice(10, false);
	// 510 bytes laced as 255, 255 and no terminator: the packet goes on.
	AppendPage(open, 0, -1, 12, { Packet(99, 510) }, true);
	const auto scan = ScanOgg(open.data(), open.size());
	CHECK(scan.pages.size() == 13);
	CHECK(!scan.pages.back().packetEnds);
	CHECK(scan.pages.back().granule == -1);
	CHECK(scan.lastGranule == 312 + 10 * 960);
	CHECK(scan.durationMs == 200);

	// Not Opus: the walk works, the duration is unknown.
	auto vorbis = Bytes();
	AppendPage(vorbis, OggPage::kBeginOfStream, 0, 0, { Packet(1, 30) });
	AppendPage(vorbis, OggPage::kEndOfStream, 4410, 1, { Packet(2, 30) });
	const auto other = ScanOgg(vorbis.data(), vorbis.size());
	CHECK(!other.opus);
	CHECK(other.clean());
	CHECK(other.durationMs == -1);
}

void TestMissingEos() {
	Section = "missing eos";
	auto bytes = Voice(20, false);
	const auto size = bytes.size();
	const auto before = ScanOgg(bytes.data(), bytes.size());
	CHECK(!before.endOfStream);
	CHECK(!before.clean());
	const auto finish = FinaliseOgg(bytes);
	CHECK(finish.status == OggFinishStatus::Finalised);
	CHECK(finish.endOfStreamSet);
	CHECK(finish.droppedBytes == 0);
	CHECK(bytes.size() == size);
	CHECK(finish.scan.clean());
	CHECK(finish.scan.durationMs == 400);
	// The patched page's CRC is right: a fresh walk accepts every page.
	CHECK(AllPagesValid(bytes));
	const auto after = ScanOgg(bytes.data(), bytes.size());
	CHECK(after.clean());
	CHECK(after.pages.size() == 22);
	// The same bytes as a writer that set the flag itself.
	CHECK(bytes == Voice(20, true));
}

void TestTruncatedPage() {
	Section = "truncated page";
	const auto whole = Voice(30, false);
	const auto scan = ScanOgg(whole.data(), whole.size());
	const auto &last = scan.pages.back();
	const auto lastStart = std::size_t(last.offset);
	// Cut at every byte inside the last page: it is dropped, the one before it
	// becomes the end of the stream.
	auto allGood = true;
	for (auto cut = lastStart + 1; cut != whole.size(); ++cut) {
		auto bytes = Bytes(whole.begin(), whole.begin() + cut);
		const auto finish = FinaliseOgg(bytes);
		allGood = allGood
			&& finish.status == OggFinishStatus::Finalised
			&& bytes.size() == lastStart
			&& finish.droppedBytes == cut - lastStart
			&& finish.endOfStreamSet
			&& finish.scan.pages.size() == 31
			&& finish.scan.durationMs == 29 * 20
			&& AllPagesValid(bytes)
			&& finish.scan.clean();
	}
	CHECK(allGood);

	// Cut exactly at a page boundary: nothing dropped, the flag set.
	auto boundary = Bytes(whole.begin(), whole.begin() + lastStart);
	const auto finish = FinaliseOgg(boundary);
	CHECK(finish.status == OggFinishStatus::Finalised);
	CHECK(finish.droppedBytes == 0);
	CHECK(finish.endOfStreamSet);

	// A finished file followed by junk (a mirror that wrote past the end):
	// the junk goes, the flag was already there.
	auto junk = Voice(5);
	const auto good = junk.size();
	junk.insert(junk.end(), { 'O', 'g', 'g', 'S', 0, 0, 1, 2 });
	const auto trimmed = FinaliseOgg(junk);
	CHECK(trimmed.status == OggFinishStatus::Finalised);
	CHECK(!trimmed.endOfStreamSet);
	CHECK(junk.size() == good);
	CHECK(trimmed.scan.clean());
}

void TestCorruptMiddle() {
	Section = "corrupt middle";
	auto bytes = Voice(40);
	const auto scan = ScanOgg(bytes.data(), bytes.size());
	const auto &bad = scan.pages[20];
	// One flipped body byte breaks that page's CRC; the walk stops there even
	// though every later page is good.
	bytes[std::size_t(bad.offset + bad.size - 1)] ^= 0x5A;
	const auto broken = ScanOgg(bytes.data(), bytes.size());
	CHECK(broken.pages.size() == 20);
	CHECK(broken.goodBytes == bad.offset);
	const auto finish = FinaliseOgg(bytes);
	CHECK(finish.status == OggFinishStatus::Finalised);
	CHECK(bytes.size() == bad.offset);
	CHECK(finish.scan.pages.size() == 20);
	CHECK(finish.scan.durationMs == 18 * 20);
	CHECK(finish.endOfStreamSet);
	CHECK(AllPagesValid(bytes));

	// A broken capture pattern stops the walk as well.
	auto pattern = Voice(10);
	const auto at = ScanOgg(pattern.data(), pattern.size()).pages[5].offset;
	pattern[std::size_t(at)] = 'X';
	CHECK(ScanOgg(pattern.data(), pattern.size()).pages.size() == 5);

	// A page claiming more body than the file has is torn, not read past.
	auto lying = Voice(4);
	const auto lastAt = std::size_t(
		ScanOgg(lying.data(), lying.size()).pages.back().offset);
	lying[lastAt + 26] = 255;
	CHECK(ScanOgg(lying.data(), lying.size()).pages.size() == 5);
}

// P3-3: one logical stream, numbered without gaps. Stale pages of an earlier
// recording left in a reused file have good CRCs but another serial or a
// sequence that does not follow; the walk stops there.
void TestStaleStream() {
	Section = "stale stream";
	auto bytes = Voice(10, false);
	const auto size = bytes.size();
	// An earlier recording's pages: another serial.
	AppendPage(bytes, 0, 312 + 500 * 960, 12, { Packet(7, 100) }, false, 0x11111111U);
	AppendPage(bytes, 0, 312 + 501 * 960, 13, { Packet(8, 100) }, false, 0x11111111U);
	const auto other = ScanOgg(bytes.data(), bytes.size());
	CHECK(other.pages.size() == 12);
	CHECK(other.goodBytes == size);
	CHECK(other.durationMs == 200);
	auto copy = bytes;
	const auto finish = FinaliseOgg(copy);
	CHECK(finish.status == OggFinishStatus::Finalised);
	CHECK(copy.size() == size);
	CHECK(finish.scan.durationMs == 200);

	// The same serial, but the sequence jumps (a gap, or old pages).
	auto gap = Voice(10, false);
	AppendPage(gap, 0, 312 + 500 * 960, 40, { Packet(7, 100) });
	CHECK(ScanOgg(gap.data(), gap.size()).pages.size() == 12);
	auto repeat = Voice(10, false);
	AppendPage(repeat, 0, 312 + 500 * 960, 5, { Packet(7, 100) });
	CHECK(ScanOgg(repeat.data(), repeat.size()).pages.size() == 12);
	// The next sequence number is accepted.
	auto next = Voice(10, false);
	AppendPage(next, 0, 312 + 11 * 960, 12, { Packet(7, 100) });
	CHECK(ScanOgg(next.data(), next.size()).pages.size() == 13);
}

// P3-4: the end-of-stream flag goes on a page with a granule position.
void TestEosGranule() {
	Section = "eos granule";
	auto open = Voice(10, false);
	const auto size = open.size();
	AppendPage(open, 0, -1, 12, { Packet(99, 510) }, true);
	AppendPage(open, OggPage::kContinued, -1, 13, { Packet(98, 255) }, true);
	const auto finish = FinaliseOgg(open);
	CHECK(finish.status == OggFinishStatus::Finalised);
	CHECK(open.size() == size);
	CHECK(finish.droppedBytes > 0);
	CHECK(finish.endOfStreamSet);
	CHECK(finish.scan.pages.size() == 12);
	CHECK(finish.scan.pages.back().granule == 312 + 10 * 960);
	CHECK(finish.scan.durationMs == 200);
	CHECK(open == Voice(10, true));
	auto again = open;
	CHECK(FinaliseOgg(again).status == OggFinishStatus::Clean);
	CHECK(again == open);
}

void TestUnrecoverable() {
	Section = "unrecoverable";
	auto empty = Bytes();
	CHECK(FinaliseOgg(empty).status == OggFinishStatus::Unrecoverable);

	auto garbage = Bytes(4096, 0);
	const auto copy = garbage;
	CHECK(FinaliseOgg(garbage).status == OggFinishStatus::Unrecoverable);
	CHECK(garbage == copy);

	// Only the two Opus header pages: nothing was recorded.
	auto headers = Voice(0);
	const auto headersCopy = headers;
	const auto finish = FinaliseOgg(headers);
	CHECK(finish.status == OggFinishStatus::Unrecoverable);
	CHECK(headers == headersCopy);
	CHECK(finish.scan.pages.size() == 2);
	CHECK(!finish.scan.hasAudio());

	// Torn inside the OpusHead page.
	auto torn = Voice(3);
	torn.resize(20);
	CHECK(FinaliseOgg(torn).status == OggFinishStatus::Unrecoverable);
}

void TestFile() {
	Section = "file";
	const auto directory = std::filesystem::temp_directory_path()
		/ "purple-core-test-ogg";
	std::filesystem::create_directories(directory);
	const auto path = (directory / "voice.ogg").string();

	const auto whole = Voice(25, false);
	const auto lastStart = std::size_t(
		ScanOgg(whole.data(), whole.size()).pages.back().offset);
	auto torn = Bytes(whole.begin(), whole.begin() + lastStart + 10);
	{
		auto file = std::fopen(path.c_str(), "wb");
		std::fwrite(torn.data(), 1, torn.size(), file);
		std::fclose(file);
	}
	const auto finish = FinaliseOggFile(path);
	CHECK(finish.status == OggFinishStatus::Finalised);
	CHECK(!finish.ioError);
	CHECK(finish.droppedBytes == 10);
	auto onDisk = Bytes();
	CHECK(RecordingFile::ReadAll(path, onDisk));
	CHECK(onDisk.size() == lastStart);
	CHECK(ScanOgg(onDisk.data(), onDisk.size()).clean());
	CHECK(onDisk == Voice(24, true));

	// P3-1: a file above the cap is refused, not read in part.
	auto capped = Bytes{ 1, 2, 3 };
	CHECK(!RecordingFile::ReadAll(path, capped, onDisk.size() - 1));
	CHECK(capped.empty());
	CHECK(RecordingFile::ReadAll(path, capped, onDisk.size()));
	CHECK(capped == onDisk);

	// Finalising again finds it clean and leaves it alone.
	CHECK(FinaliseOggFile(path).status == OggFinishStatus::Clean);

	const auto missing = FinaliseOggFile((directory / "none.ogg").string());
	CHECK(missing.status == OggFinishStatus::Unrecoverable);
	CHECK(missing.ioError);
	std::filesystem::remove_all(directory);
}

} // namespace

int main() {
	TestCrc();
	TestClean();
	TestDuration();
	TestMissingEos();
	TestTruncatedPage();
	TestCorruptMiddle();
	TestStaleStream();
	TestEosGranule();
	TestUnrecoverable();
	TestFile();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
