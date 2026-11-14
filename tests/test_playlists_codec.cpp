/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists_codec.h"

#include "purple/purple_playlists.h"
#include "purple/purple_playlists_codec_detail.h"
#include "purple/purple_types.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <malloc/malloc.h>
#endif

#include <zlib.h>

namespace {

using namespace Purple;

int Checks = 0;
int Failures = 0;

void Check(bool ok, const char *expr, int line) {
	++Checks;
	if (!ok) {
		++Failures;
		std::printf("FAIL line %d: %s\n", line, expr);
	}
}

#define CHECK(cond) Check(bool(cond), #cond, __LINE__)

[[nodiscard]] uint32_t ReadLe32(const QByteArray &bytes, qsizetype offset) {
	uint32_t value = 0;
	for (int i = 0; i < 4; ++i) {
		value |= uint32_t(static_cast<unsigned char>(bytes.at(offset + i))) << (8 * i);
	}
	return value;
}

void WriteLe32(QByteArray &bytes, qsizetype offset, uint32_t value) {
	for (int i = 0; i < 4; ++i) {
		bytes[offset + i] = static_cast<char>((value >> (8 * i)) & 0xFFU);
	}
}

[[nodiscard]] QByteArray BuildRepeatedByteGzip(qsizetype totalPlainBytes, char ch) {
	z_stream strm{};
	if (deflateInit2(&strm, 6, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
		return {};
	}
	const QByteArray pattern(32768, ch);
	static constexpr Bytef kEmpty = 0;
	QByteArray out;
	std::vector<Bytef> chunk(32768);
	qsizetype remaining = totalPlainBytes;
	while (true) {
		int flush = Z_NO_FLUSH;
		if (strm.avail_in == 0 && remaining > 0) {
			const auto n = static_cast<uInt>(std::min<qsizetype>(remaining, pattern.size()));
			strm.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(pattern.constData()));
			strm.avail_in = n;
			remaining -= static_cast<qsizetype>(n);
		} else if (strm.avail_in == 0) {
			strm.next_in = const_cast<Bytef *>(&kEmpty);
			flush = Z_FINISH;
		}
		if (remaining == 0) {
			flush = Z_FINISH;
		}
		strm.next_out = chunk.data();
		strm.avail_out = static_cast<uInt>(chunk.size());
		const auto rc = deflate(&strm, flush);
		if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
			deflateEnd(&strm);
			return {};
		}
		const auto have = static_cast<qsizetype>(chunk.size() - strm.avail_out);
		if (have > 0) {
			out.append(reinterpret_cast<const char *>(chunk.data()), have);
		}
		if (rc == Z_STREAM_END) {
			deflateEnd(&strm);
			return out;
		}
	}
}

[[nodiscard]] QByteArray IndependentDeflateGzip(
		const QByteArray &plain,
		int level = Z_DEFAULT_COMPRESSION,
		gz_header *customHeader = nullptr) {
	z_stream strm{};
	const auto initRc = deflateInit2(
		&strm,
		level,
		Z_DEFLATED,
		16 + MAX_WBITS,
		8,
		Z_DEFAULT_STRATEGY);
	if (initRc != Z_OK) {
		return {};
	}
	if (customHeader != nullptr && deflateSetHeader(&strm, customHeader) != Z_OK) {
		deflateEnd(&strm);
		return {};
	}
	static constexpr Bytef kEmpty = 0;
	QByteArray out;
	std::vector<Bytef> chunk(32768);
	qsizetype offset = 0;
	while (true) {
		if (strm.avail_in == 0 && offset < plain.size()) {
			const auto n = std::min<qsizetype>(plain.size() - offset, 1 << 20);
			strm.next_in = const_cast<Bytef *>(
				reinterpret_cast<const Bytef *>(plain.constData() + offset));
			strm.avail_in = static_cast<uInt>(n);
			offset += n;
		} else if (strm.avail_in == 0) {
			strm.next_in = const_cast<Bytef *>(&kEmpty);
		}
		const auto flush = (offset >= plain.size()) ? Z_FINISH : Z_NO_FLUSH;
		strm.next_out = chunk.data();
		strm.avail_out = static_cast<uInt>(chunk.size());
		const auto rc = deflate(&strm, flush);
		if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
			deflateEnd(&strm);
			return {};
		}
		const auto have = static_cast<qsizetype>(chunk.size() - strm.avail_out);
		if (have > 0) {
			out.append(reinterpret_cast<const char *>(chunk.data()), have);
		}
		if (rc == Z_STREAM_END) {
			deflateEnd(&strm);
			return out;
		}
	}
}

[[nodiscard]] QByteArray IndependentDeflateWithWindowBits(const QByteArray &plain, int windowBits) {
	z_stream strm{};
	if (deflateInit2(&strm, 6, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
		return {};
	}
	strm.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(plain.constData()));
	strm.avail_in = static_cast<uInt>(plain.size());
	QByteArray out(deflateBound(&strm, strm.avail_in) + 64, '\0');
	strm.next_out = reinterpret_cast<Bytef *>(out.data());
	strm.avail_out = static_cast<uInt>(out.size());
	const auto rc = deflate(&strm, Z_FINISH);
	const auto produced = static_cast<qsizetype>(strm.total_out);
	deflateEnd(&strm);
	if (rc != Z_STREAM_END) {
		return {};
	}
	out.truncate(produced);
	return out;
}

[[nodiscard]] bool IndependentInflateGzip(const QByteArray &gzip, QByteArray &outPlain) {
	z_stream strm{};
	if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
		return false;
	}
	strm.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(gzip.constData()));
	strm.avail_in = static_cast<uInt>(gzip.size());
	std::vector<Bytef> chunk(16384);
	outPlain.clear();
	while (true) {
		strm.next_out = chunk.data();
		strm.avail_out = static_cast<uInt>(chunk.size());
		const auto rc = inflate(&strm, Z_NO_FLUSH);
		const auto have = static_cast<qsizetype>(chunk.size() - strm.avail_out);
		if (have > 0) {
			outPlain.append(reinterpret_cast<const char *>(chunk.data()), have);
		}
		if (rc == Z_STREAM_END) {
			const auto consumedAll = (strm.avail_in == 0);
			inflateEnd(&strm);
			return consumedAll;
		}
		if (rc != Z_OK) {
			inflateEnd(&strm);
			outPlain.clear();
			return false;
		}
	}
}

struct AllocationTrace {
	int allocateCalls = 0;
	int liveBuffers = 0;
	int destroyedWithoutTake = 0;
	qsizetype peakRequestedBytes = 0;
	std::vector<qsizetype> requestedSizes;
	std::vector<qsizetype> allocatedCapacities;
};

struct TrackingOutputFactory {
	std::shared_ptr<AllocationTrace> trace = std::make_shared<AllocationTrace>();
	std::function<void(qsizetype)> onAllocate;

	struct Buffer {
		std::shared_ptr<AllocationTrace> trace;
		std::function<void(qsizetype)> onAllocate;
		QByteArray bytes;
		bool holdingAllocation = false;

		~Buffer() {
			if (holdingAllocation) {
				--trace->liveBuffers;
				++trace->destroyedWithoutTake;
			}
		}

		[[nodiscard]] bool allocate(qsizetype exactSize) {
			++trace->allocateCalls;
			trace->requestedSizes.push_back(exactSize);
			if (exactSize < 0) {
				return false;
			}
			if (exactSize > 0) {
				bytes = QByteArray(exactSize, Qt::Uninitialized);
				if (bytes.size() != exactSize) {
					return false;
				}
				trace->peakRequestedBytes = std::max(trace->peakRequestedBytes, exactSize);
				trace->allocatedCapacities.push_back(bytes.capacity());
				++trace->liveBuffers;
				holdingAllocation = true;
			} else {
				bytes = QByteArray();
				trace->allocatedCapacities.push_back(0);
			}
			if (onAllocate) {
				onAllocate(exactSize);
			}
			return true;
		}

		[[nodiscard]] char *writableData() {
			return bytes.isEmpty() ? nullptr : bytes.data();
		}

		[[nodiscard]] QByteArray take() {
			if (holdingAllocation) {
				--trace->liveBuffers;
				holdingAllocation = false;
			}
			return std::move(bytes);
		}
	};

	[[nodiscard]] Buffer create() const {
		return { trace, onAllocate, {}, false };
	}
};

[[nodiscard]] PlaylistLibrary MakeSampleLibrary() {
	PlaylistLibrary lib;
	Playlist pl;
	pl.title = PlaylistTitleRegister{ u"Codec Sample"_q, 100 };
	pl.deleted = PlaylistDeletedRegister{ false, 100 };
	pl.order = PlaylistOrderRegister{ { 1, 2 }, 150 };
	auto songs = std::map<uint64_t, PlaylistSongRegister>();
	songs.emplace(
		1,
		PlaylistSongRegister{
			PlaylistSongValue{ PlaylistSource{
				.peer = PeerToken{ PeerTokenKind::Self },
				.msg = 42,
				.username = QString(),
				.alt = {},
				.meta = PlaylistSongMeta{ u"Track One"_q, u"Artist"_q, 195 },
				.since = 100,
			} },
			120,
		});
	songs.emplace(
		2,
		PlaylistSongRegister{
			PlaylistSongValue{ PlaylistSource{
				.peer = PeerToken{ PeerTokenKind::Channel, 7001 },
				.msg = 88,
				.username = u"music_ch"_q,
				.alt = {},
				.meta = PlaylistSongMeta{ u"Track Two"_q, QString(), 240 },
				.since = 110,
			} },
			130,
		});
	pl.songs = std::move(songs);
	lib.playlists.emplace(u"pl-codec234567abcde"_q, std::move(pl));
	return lib;
}

void CheckGzipHeaderAndTrailer(const QByteArray &plain, const QByteArray &gzip) {
	CHECK(gzip.size() >= 20);
	CHECK(static_cast<unsigned char>(gzip.at(0)) == 0x1F);
	CHECK(static_cast<unsigned char>(gzip.at(1)) == 0x8B);
	CHECK(static_cast<unsigned char>(gzip.at(2)) == 0x08);
	CHECK(static_cast<unsigned char>(gzip.at(3)) == 0x00);
	CHECK(ReadLe32(gzip, 4) == 0U);
	CHECK(static_cast<unsigned char>(gzip.at(9)) == 0xFF);
	const auto expectedCrc = crc32(
		0L,
		plain.isEmpty() ? Z_NULL : reinterpret_cast<const Bytef *>(plain.constData()),
		static_cast<uInt>(plain.size()));
	const auto expectedIsize = static_cast<uint32_t>(plain.size());
	CHECK(ReadLe32(gzip, gzip.size() - 8) == static_cast<uint32_t>(expectedCrc));
	CHECK(ReadLe32(gzip, gzip.size() - 4) == expectedIsize);
}

// Run before large test fixtures allocate 16/32 MiB plain inputs so malloc HWM reflects codec transient usage.
void TestRealAllocatorBoundBeforeLargeFixtures() {
	const auto gzip16 = BuildRepeatedByteGzip(16 * 1024 * 1024, 'A');
	const auto bomb32 = BuildRepeatedByteGzip(kPlaylistRecordMaximumUncompressedBytes + 1, 'Z');
	const auto bomb34 = BuildRepeatedByteGzip(kPlaylistRecordMaximumUncompressedBytes + 2 * 1024 * 1024, 'B');
	const auto valid32 = BuildRepeatedByteGzip(kPlaylistRecordMaximumUncompressedBytes, 'Z');
	CHECK(!gzip16.isEmpty() && gzip16.size() < 64 * 1024);
	CHECK(!bomb32.isEmpty() && bomb32.size() < 64 * 1024);
	CHECK(!bomb34.isEmpty() && bomb34.size() < 128 * 1024);
	CHECK(!valid32.isEmpty() && valid32.size() < 64 * 1024);

	auto badCrc16 = gzip16;
	badCrc16[badCrc16.size() - 8] = static_cast<char>(static_cast<unsigned char>(badCrc16.at(badCrc16.size() - 8)) ^ 0xFFU);

	auto bomb34TinyIsize = bomb34;
	WriteLe32(bomb34TinyIsize, bomb34TinyIsize.size() - 4, 17U);

#ifdef __APPLE__
	malloc_statistics_t beforeFailures{};
	malloc_zone_statistics(nullptr, &beforeFailures);
#endif
	const auto decBad16 = DecodePlaylistBytes(badCrc16);
	CHECK(decBad16.status == PlaylistCodecStatus::Invalid);
	CHECK(decBad16.error == PlaylistCodecError::InvalidGzip);
	CHECK(decBad16.bytes.isEmpty());

	const auto decBomb32 = DecodePlaylistBytes(bomb32);
	CHECK(decBomb32.status == PlaylistCodecStatus::Oversized);
	CHECK(decBomb32.error == PlaylistCodecError::UncompressedSizeLimit);
	CHECK(decBomb32.bytes.isEmpty());

	const auto decBomb34TinyIsize = DecodePlaylistBytes(bomb34TinyIsize);
	CHECK(decBomb34TinyIsize.status == PlaylistCodecStatus::Oversized);
	CHECK(decBomb34TinyIsize.error == PlaylistCodecError::UncompressedSizeLimit);
	CHECK(decBomb34TinyIsize.bytes.isEmpty());
#ifdef __APPLE__
	malloc_statistics_t afterFailures{};
	malloc_zone_statistics(nullptr, &afterFailures);
	// Pass 1 must abort 16 MiB bad-CRC and bombs using only ~32 KiB scratch + zlib state.
	CHECK(afterFailures.max_size_in_use >= beforeFailures.size_in_use);
	CHECK(afterFailures.max_size_in_use - beforeFailures.size_in_use < size_t(2) * 1024 * 1024);
#endif

#ifdef __APPLE__
	malloc_statistics_t beforeDecode32{};
	malloc_zone_statistics(nullptr, &beforeDecode32);
#endif
	const auto dec32 = DecodePlaylistBytes(valid32);
#ifdef __APPLE__
	malloc_statistics_t afterDecode32{};
	malloc_zone_statistics(nullptr, &afterDecode32);
#endif
	CHECK(bool(dec32));
	CHECK(dec32.bytes.size() == kPlaylistRecordMaximumUncompressedBytes);
	CHECK(dec32.bytes.capacity() >= kPlaylistRecordMaximumUncompressedBytes);
	CHECK(dec32.bytes.capacity() <= kPlaylistRecordMaximumUncompressedBytes + 4096);
	CHECK(dec32.bytes.at(0) == 'Z' && dec32.bytes.at(dec32.bytes.size() - 1) == 'Z');
#ifdef __APPLE__
	// Single 32 MiB allocation must stay far below old 16+32=48 MiB realloc overlap or 64 MiB duplicate.
	CHECK(afterDecode32.max_size_in_use - beforeDecode32.size_in_use >= size_t(32) * 1024 * 1024);
	CHECK(afterDecode32.max_size_in_use - beforeDecode32.size_in_use < size_t(34) * 1024 * 1024);
#endif

#ifdef __APPLE__
	malloc_statistics_t beforeEncode32{};
	malloc_zone_statistics(nullptr, &beforeEncode32);
#endif
	const auto enc32 = EncodePlaylistBytes(dec32.bytes);
#ifdef __APPLE__
	malloc_statistics_t afterEncode32{};
	malloc_zone_statistics(nullptr, &afterEncode32);
#endif
	CHECK(bool(enc32));
	CHECK(enc32.bytes.size() < 64 * 1024);
	CheckGzipHeaderAndTrailer(dec32.bytes, enc32.bytes);
	CHECK(enc32.bytes.capacity() >= enc32.bytes.size());
	CHECK(enc32.bytes.capacity() <= enc32.bytes.size() + 4096);
#ifdef __APPLE__
	// Compressible 32 MiB encode allocates ~32 KiB compressed output, never 32 MiB deflateBound.
	CHECK(afterEncode32.max_size_in_use - beforeEncode32.max_size_in_use < size_t(2) * 1024 * 1024);
#endif
	CHECK(DecodePlaylistBytes(enc32.bytes).bytes == dec32.bytes);
}

void TestTwoPassSingleAllocationAndPass2Guards() {
	// Small, odd, and chunk boundaries allocate ONCE at verified exact length with zero realloc growth.
	for (const qsizetype size : { 0, 1, 13, 37, 32767, 32768, 32769, 65536, 65537 }) {
		QByteArray plain(size, '\0');
		for (qsizetype i = 0; i < size; ++i) {
			plain[i] = static_cast<char>((i * 73 + 19) & 0xFF);
		}
		TrackingOutputFactory encFactory;
		const auto enc = PlaylistsCodecDetail::EncodePlaylistBytesWithFactory(
			plain,
			kPlaylistRecordCodecCaps,
			encFactory);
		CHECK(bool(enc));
		CHECK(encFactory.trace->allocateCalls == 1);
		CHECK(encFactory.trace->requestedSizes.size() == 1);
		CHECK(encFactory.trace->requestedSizes.front() == enc.bytes.size());
		CHECK(encFactory.trace->peakRequestedBytes == enc.bytes.size());
		CHECK(encFactory.trace->liveBuffers == 0);
		CHECK(encFactory.trace->destroyedWithoutTake == 0);
		CHECK(enc.bytes.capacity() >= enc.bytes.size());
		CHECK(enc.bytes.capacity() <= enc.bytes.size() + 4096);

		TrackingOutputFactory decFactory;
		const auto dec = PlaylistsCodecDetail::DecodePlaylistBytesWithFactory(
			enc.bytes,
			kPlaylistRecordCodecCaps,
			decFactory);
		CHECK(bool(dec) && dec.bytes == plain);
		CHECK(decFactory.trace->allocateCalls == 1);
		CHECK(decFactory.trace->requestedSizes.size() == 1);
		CHECK(decFactory.trace->requestedSizes.front() == size);
		CHECK(decFactory.trace->peakRequestedBytes == size);
		CHECK(decFactory.trace->liveBuffers == 0);
		CHECK(decFactory.trace->destroyedWithoutTake == 0);
		if (size == 0) {
			CHECK(dec.bytes.capacity() == 0);
		} else {
			CHECK(dec.bytes.capacity() >= size);
			CHECK(dec.bytes.capacity() <= size + 4096);
		}
	}

	// Huge caller caps with small payload still allocate only exact tiny output length.
	{
		const QByteArray tiny = "tiny payload";
		TrackingOutputFactory encHugeFactory;
		const PlaylistCodecCaps hugeCaps{ int64_t(1) << 50, int64_t(QByteArray::maxSize()) };
		const auto encHuge = PlaylistsCodecDetail::EncodePlaylistBytesWithFactory(
			tiny,
			hugeCaps,
			encHugeFactory);
		CHECK(bool(encHuge));
		CHECK(encHugeFactory.trace->allocateCalls == 1);
		CHECK(encHugeFactory.trace->requestedSizes.front() == encHuge.bytes.size());

		TrackingOutputFactory decHugeFactory;
		const auto decHuge = PlaylistsCodecDetail::DecodePlaylistBytesWithFactory(
			encHuge.bytes,
			hugeCaps,
			decHugeFactory);
		CHECK(bool(decHuge) && decHuge.bytes == tiny);
		CHECK(decHugeFactory.trace->allocateCalls == 1);
		CHECK(decHugeFactory.trace->requestedSizes.front() == tiny.size());
	}

	// Pass 1 rejections perform ZERO output allocations even when corrupt CRC/trailing occurs at 16 MiB.
	{
		const auto big16 = BuildRepeatedByteGzip(16 * 1024 * 1024, 'M');
		auto big16BadCrc = big16;
		big16BadCrc[big16BadCrc.size() - 8] = static_cast<char>(
			static_cast<unsigned char>(big16BadCrc.at(big16BadCrc.size() - 8)) ^ 0x01U);
		auto big16Trailing = big16 + QByteArray(1, '\0');
		auto big16Truncated = big16.left(big16.size() - 1);
		const auto bomb34 = BuildRepeatedByteGzip(kPlaylistRecordMaximumUncompressedBytes + 2 * 1024 * 1024, 'B');
		auto bomb34TinyIsize = bomb34;
		WriteLe32(bomb34TinyIsize, bomb34TinyIsize.size() - 4, 17U);
		auto smallHugeIsize = IndependentDeflateGzip(QByteArray(100, 'H'), 6);
		WriteLe32(smallHugeIsize, smallHugeIsize.size() - 4, uint32_t(kPlaylistRecordMaximumUncompressedBytes));
		for (const auto &bad : {
				big16BadCrc,
				big16Trailing,
				big16Truncated,
				bomb34,
				bomb34TinyIsize,
				smallHugeIsize,
				QByteArray("bad"),
			}) {
			TrackingOutputFactory failDecFactory;
			const auto dec = PlaylistsCodecDetail::DecodePlaylistBytesWithFactory(
				bad,
				kPlaylistRecordCodecCaps,
				failDecFactory);
			CHECK(!bool(dec) && dec.bytes.isEmpty());
			CHECK(failDecFactory.trace->allocateCalls == 0);
			CHECK(failDecFactory.trace->peakRequestedBytes == 0);
		}

		QByteArray incompressible(65536, '\0');
		uint32_t rng = 0x2468ACE0U;
		for (qsizetype i = 0; i < incompressible.size(); ++i) {
			rng = rng * 1664525U + 1013904223U;
			incompressible[i] = static_cast<char>((rng >> 24) & 0xFFU);
		}
		TrackingOutputFactory failEncFactory;
		const auto encFail = PlaylistsCodecDetail::EncodePlaylistBytesWithFactory(
			incompressible,
			{ 65536, 65536 },
			failEncFactory);
		CHECK(encFail.status == PlaylistCodecStatus::Oversized);
		CHECK(encFail.error == PlaylistCodecError::CompressedSizeLimit);
		CHECK(encFail.bytes.isEmpty());
		CHECK(failEncFactory.trace->allocateCalls == 0);
		CHECK(failEncFactory.trace->peakRequestedBytes == 0);
	}

	// Pass 2 divergence detection (input altered between Pass 1 and Pass 2) discards allocation and fails closed.
	{
		auto makePaddedGzip = [](const QByteArray &plain, qsizetype targetTotalSize) -> QByteArray {
			const auto base = IndependentDeflateGzip(plain, 6);
			const qsizetype padNeeded = targetTotalSize - base.size() - 1;
			if (padNeeded < 0) {
				return {};
			}
			std::string comment(static_cast<std::size_t>(padNeeded), 'c');
			gz_header hdr{};
			hdr.comment = reinterpret_cast<Bytef *>(comment.data());
			return IndependentDeflateGzip(plain, 6, &hdr);
		};
		const qsizetype frameSize = 256;
		const auto g100 = makePaddedGzip(QByteArray(100, 'X'), frameSize);
		const auto g50 = makePaddedGzip(QByteArray(50, 'Y'), frameSize);
		const auto g150 = makePaddedGzip(QByteArray(150, 'Z'), frameSize);
		CHECK(g100.size() == frameSize && g50.size() == frameSize && g150.size() == frameSize);

		// Pass 2 produces fewer bytes (50 != 100).
		{
			QByteArray mutableGzip = g100;
			char *raw = mutableGzip.data();
			TrackingOutputFactory f;
			f.onAllocate = [&](qsizetype) {
				std::memcpy(raw, g50.constData(), static_cast<std::size_t>(frameSize));
			};
			const auto res = PlaylistsCodecDetail::DecodePlaylistBytesWithFactory(
				mutableGzip,
				kPlaylistRecordCodecCaps,
				f);
			CHECK(res.status == PlaylistCodecStatus::Invalid);
			CHECK(res.error == PlaylistCodecError::CodecFailure);
			CHECK(res.bytes.isEmpty());
			CHECK(f.trace->allocateCalls == 1);
			CHECK(f.trace->liveBuffers == 0);
			CHECK(f.trace->destroyedWithoutTake == 1);
		}

		// Pass 2 produces more bytes (150 > 100) without overflowing allocated 100-byte buffer.
		{
			QByteArray mutableGzip = g100;
			char *raw = mutableGzip.data();
			TrackingOutputFactory f;
			f.onAllocate = [&](qsizetype) {
				std::memcpy(raw, g150.constData(), static_cast<std::size_t>(frameSize));
			};
			const auto res = PlaylistsCodecDetail::DecodePlaylistBytesWithFactory(
				mutableGzip,
				kPlaylistRecordCodecCaps,
				f);
			CHECK(res.status == PlaylistCodecStatus::Invalid);
			CHECK(res.error == PlaylistCodecError::CodecFailure);
			CHECK(res.bytes.isEmpty());
			CHECK(f.trace->liveBuffers == 0);
			CHECK(f.trace->destroyedWithoutTake == 1);
		}

		// Pass 2 sees corrupt CRC/deflate.
		{
			QByteArray mutableGzip = g100;
			char *raw = mutableGzip.data();
			TrackingOutputFactory f;
			f.onAllocate = [&](qsizetype) {
				raw[frameSize - 8] = static_cast<char>(static_cast<unsigned char>(raw[frameSize - 8]) ^ 0xFFU);
			};
			const auto res = PlaylistsCodecDetail::DecodePlaylistBytesWithFactory(
				mutableGzip,
				kPlaylistRecordCodecCaps,
				f);
			CHECK(res.status == PlaylistCodecStatus::Invalid);
			CHECK(res.error == PlaylistCodecError::InvalidGzip);
			CHECK(res.bytes.isEmpty());
			CHECK(f.trace->liveBuffers == 0);
			CHECK(f.trace->destroyedWithoutTake == 1);
		}

		// Encode Pass 2 divergence: shorter compressed output and longer compressed output.
		QByteArray random1024(1024, '\0');
		uint32_t rng = 0xCAFEBABEU;
		for (qsizetype i = 0; i < random1024.size(); ++i) {
			rng = rng * 1664525U + 1013904223U;
			random1024[i] = static_cast<char>((rng >> 24) & 0xFFU);
		}
		const QByteArray repeat1024(1024, 'A');
		{
			QByteArray mutablePlain = random1024;
			char *raw = mutablePlain.data();
			TrackingOutputFactory f;
			f.onAllocate = [&](qsizetype) {
				std::memcpy(raw, repeat1024.constData(), static_cast<std::size_t>(repeat1024.size()));
			};
			const auto res = PlaylistsCodecDetail::EncodePlaylistBytesWithFactory(
				mutablePlain,
				kPlaylistRecordCodecCaps,
				f);
			CHECK(res.status == PlaylistCodecStatus::Invalid);
			CHECK(res.error == PlaylistCodecError::CodecFailure);
			CHECK(res.bytes.isEmpty());
			CHECK(f.trace->liveBuffers == 0);
			CHECK(f.trace->destroyedWithoutTake == 1);
		}
		{
			QByteArray mutablePlain = repeat1024;
			char *raw = mutablePlain.data();
			TrackingOutputFactory f;
			f.onAllocate = [&](qsizetype) {
				std::memcpy(raw, random1024.constData(), static_cast<std::size_t>(random1024.size()));
			};
			const auto res = PlaylistsCodecDetail::EncodePlaylistBytesWithFactory(
				mutablePlain,
				kPlaylistRecordCodecCaps,
				f);
			CHECK(res.status == PlaylistCodecStatus::Invalid);
			CHECK(res.error == PlaylistCodecError::CodecFailure);
			CHECK(res.bytes.isEmpty());
			CHECK(f.trace->liveBuffers == 0);
			CHECK(f.trace->destroyedWithoutTake == 1);
		}
	}
}

void TestRoundTripsHeaderDeterminismAndZlibInterop() {
	// Empty input -> 20-byte RFC 1952 gzip member + exact empty decode.
	const QByteArray empty;
	const auto encEmpty = EncodePlaylistBytes(empty);
	CHECK(bool(encEmpty));
	CHECK(encEmpty.status == PlaylistCodecStatus::Valid);
	CHECK(encEmpty.error == PlaylistCodecError::None);
	CHECK(encEmpty.bytes.size() == 20);
	CheckGzipHeaderAndTrailer(empty, encEmpty.bytes);
	const auto decEmpty = DecodePlaylistBytes(encEmpty.bytes);
	CHECK(bool(decEmpty) && decEmpty.bytes.isEmpty());
	QByteArray zlibEmptyOut;
	CHECK(IndependentInflateGzip(encEmpty.bytes, zlibEmptyOut) && zlibEmptyOut.isEmpty());

	// Deterministic repeated encode and non-JSON raw preservation (must NOT canonicalize JSON).
	const QByteArray nonCanonicalJson = R"({ "z" : 2 , "a" : 1 }
)";
	const auto encJson1 = EncodePlaylistBytes(nonCanonicalJson);
	const auto encJson2 = EncodePlaylistBytes(nonCanonicalJson);
	CHECK(bool(encJson1) && bool(encJson2));
	CHECK(encJson1.bytes == encJson2.bytes);
	CheckGzipHeaderAndTrailer(nonCanonicalJson, encJson1.bytes);
	const auto decJson = DecodePlaylistBytes(encJson1.bytes);
	CHECK(bool(decJson) && decJson.bytes == nonCanonicalJson);

	// Binary bytes (all 256 byte values, embedded NULs, invalid UTF-8) multi-chunk (>100 KiB).
	QByteArray binary;
	binary.reserve(102400);
	for (qsizetype i = 0; i < 102400; ++i) {
		binary.append(static_cast<char>((i * 131 + 17) & 0xFF));
	}
	const auto encBin = EncodePlaylistBytes(binary);
	CHECK(bool(encBin));
	CheckGzipHeaderAndTrailer(binary, encBin.bytes);
	const auto decBin = DecodePlaylistBytes(encBin.bytes);
	CHECK(bool(decBin) && decBin.bytes == binary);
	QByteArray zlibBinOut;
	CHECK(IndependentInflateGzip(encBin.bytes, zlibBinOut) && zlibBinOut == binary);

	// Representative canonical playlist library round-trip -> ParsePlaylistLibrary.
	const auto lib = MakeSampleLibrary();
	const auto serialized = SerializePlaylistLibrary(lib, 999);
	CHECK(bool(serialized));
	const auto encLib = EncodePlaylistBytes(serialized.canonical);
	CHECK(bool(encLib));
	CheckGzipHeaderAndTrailer(serialized.canonical, encLib.bytes);
	const auto decLib = DecodePlaylistBytes(encLib.bytes);
	CHECK(bool(decLib) && decLib.bytes == serialized.canonical);
	const auto reparsed = ParsePlaylistLibrary(decLib.bytes, 999);
	CHECK(bool(reparsed) && reparsed.library == lib);

	// Independent zlib streams (different levels + FNAME/FCOMMENT/FEXTRA/FHCRC/mtime/OS).
	const QByteArray sample = QByteArray("playlist-sync-sample-") + binary.left(5000);
	for (const int lvl : { 1, 9 }) {
		const auto zGzip = IndependentDeflateGzip(sample, lvl);
		CHECK(!zGzip.isEmpty());
		const auto dec = DecodePlaylistBytes(zGzip);
		CHECK(bool(dec) && dec.bytes == sample);
	}
	QByteArray extra("AB\x02\x00XY", 6);
	std::string fname = "library.json";
	std::string fcomment = "interop-check";
	gz_header rich{};
	rich.text = 1;
	rich.time = 1700000123;
	rich.os = 3;
	rich.extra = reinterpret_cast<Bytef *>(extra.data());
	rich.extra_len = static_cast<uInt>(extra.size());
	rich.name = reinterpret_cast<Bytef *>(fname.data());
	rich.comment = reinterpret_cast<Bytef *>(fcomment.data());
	rich.hcrc = 1;
	const auto richGzip = IndependentDeflateGzip(sample, 6, &rich);
	CHECK(!richGzip.isEmpty());
	const auto decRich = DecodePlaylistBytes(richGzip);
	CHECK(bool(decRich) && decRich.bytes == sample);
}

void TestChunkAndCapBoundaries() {
	// Record & share constants match spec (10/32 MiB and 4/16 MiB).
	CHECK(kPlaylistRecordMaximumCompressedBytes == qsizetype(10) * 1024 * 1024);
	CHECK(kPlaylistRecordMaximumUncompressedBytes == qsizetype(32) * 1024 * 1024);
	CHECK(kPlaylistShareMaximumCompressedBytes == qsizetype(4) * 1024 * 1024);
	CHECK(kPlaylistShareMaximumUncompressedBytes == qsizetype(16) * 1024 * 1024);

	// Exact chunk boundaries (32 KiB - 1, 32 KiB, 32 KiB + 1, 64 KiB, 64 KiB + 1).
	for (const qsizetype size : { 32767, 32768, 32769, 65536, 65537 }) {
		QByteArray pattern(size, 'P');
		for (qsizetype i = 0; i < size; i += 257) {
			pattern[i] = static_cast<char>(i & 0xFF);
		}
		const auto encExact = EncodePlaylistBytes(pattern, { 1024 * 1024, size });
		CHECK(bool(encExact));
		CHECK(encExact.bytes.capacity() <= encExact.bytes.size() + 4096);
		const auto decExact = DecodePlaylistBytes(encExact.bytes, { encExact.bytes.size(), size });
		CHECK(bool(decExact) && decExact.bytes == pattern);
		CHECK(decExact.bytes.capacity() <= size + 4096);

		const auto encPlainOver = EncodePlaylistBytes(pattern, { 1024 * 1024, size - 1 });
		CHECK(encPlainOver.status == PlaylistCodecStatus::Oversized);
		CHECK(encPlainOver.error == PlaylistCodecError::UncompressedSizeLimit);
		CHECK(encPlainOver.bytes.isEmpty());

		const auto encCompOver = EncodePlaylistBytes(pattern, { encExact.bytes.size() - 1, size });
		CHECK(encCompOver.status == PlaylistCodecStatus::Oversized);
		CHECK(encCompOver.error == PlaylistCodecError::CompressedSizeLimit);
		CHECK(encCompOver.bytes.isEmpty());

		const auto decPlainOver = DecodePlaylistBytes(encExact.bytes, { encExact.bytes.size(), size - 1 });
		CHECK(decPlainOver.status == PlaylistCodecStatus::Oversized);
		CHECK(decPlainOver.error == PlaylistCodecError::UncompressedSizeLimit);
		CHECK(decPlainOver.bytes.isEmpty());

		const auto decCompOver = DecodePlaylistBytes(encExact.bytes, { encExact.bytes.size() - 1, size });
		CHECK(decCompOver.status == PlaylistCodecStatus::Oversized);
		CHECK(decCompOver.error == PlaylistCodecError::CompressedSizeLimit);
		CHECK(decCompOver.bytes.isEmpty());
	}

	// Uncompressible plain bytes fit plain cap, overflow compressed cap -> Oversized on encode.
	QByteArray incompressible(65536, '\0');
	uint32_t rng = 0x13579BDFU;
	for (qsizetype i = 0; i < incompressible.size(); ++i) {
		rng = rng * 1664525U + 1013904223U;
		incompressible[i] = static_cast<char>((rng >> 24) & 0xFFU);
	}
	const auto encIncompOver = EncodePlaylistBytes(incompressible, { 65536, 65536 });
	CHECK(encIncompOver.status == PlaylistCodecStatus::Oversized);
	CHECK(encIncompOver.error == PlaylistCodecError::CompressedSizeLimit);
	CHECK(encIncompOver.bytes.isEmpty());

	// Zero uncompressed cap allows empty gzip member, rejects nonempty gzip & empty/double input.
	const auto encEmpty = EncodePlaylistBytes(QByteArray(), { 1024, 0 });
	CHECK(bool(encEmpty));
	const auto decEmptyZeroCap = DecodePlaylistBytes(encEmpty.bytes, { 1024, 0 });
	CHECK(bool(decEmptyZeroCap) && decEmptyZeroCap.bytes.isEmpty());

	const auto encOneByte = EncodePlaylistBytes(QByteArray("x"), { 1024, 1 });
	CHECK(bool(encOneByte));
	const auto decOneZeroCap = DecodePlaylistBytes(encOneByte.bytes, { 1024, 0 });
	CHECK(decOneZeroCap.status == PlaylistCodecStatus::Oversized);
	CHECK(decOneZeroCap.error == PlaylistCodecError::UncompressedSizeLimit);
	CHECK(decOneZeroCap.bytes.isEmpty());

	const auto encOneZeroPlainCap = EncodePlaylistBytes(QByteArray("x"), { 1024, 0 });
	CHECK(encOneZeroPlainCap.status == PlaylistCodecStatus::Oversized);
	CHECK(encOneZeroPlainCap.error == PlaylistCodecError::UncompressedSizeLimit);
	CHECK(encOneZeroPlainCap.bytes.isEmpty());

	const auto decNoMemberZeroCap = DecodePlaylistBytes(QByteArray(), { 1024, 0 });
	CHECK(decNoMemberZeroCap.status == PlaylistCodecStatus::Invalid);
	CHECK(decNoMemberZeroCap.error == PlaylistCodecError::InvalidGzip);

	const auto decDoubleEmptyZeroCap = DecodePlaylistBytes(
		encEmpty.bytes + encEmpty.bytes,
		{ 1024, 0 });
	CHECK(decDoubleEmptyZeroCap.status == PlaylistCodecStatus::Invalid);
	CHECK(decDoubleEmptyZeroCap.error == PlaylistCodecError::InvalidGzip);

	// Zero compressed cap: compressed output/input exceeds 0 -> Oversized.
	const auto encZeroComp = EncodePlaylistBytes(QByteArray(), { 0, 1024 });
	CHECK(encZeroComp.status == PlaylistCodecStatus::Oversized);
	CHECK(encZeroComp.error == PlaylistCodecError::CompressedSizeLimit);
	const auto decZeroComp = DecodePlaylistBytes(encEmpty.bytes, { 0, 1024 });
	CHECK(decZeroComp.status == PlaylistCodecStatus::Oversized);
	CHECK(decZeroComp.error == PlaylistCodecError::CompressedSizeLimit);
}

void TestRealBombsShareCapsAndCompressedPreCheck() {
	// 16 MiB exact vs 16 MiB + 1 share caps.
	const QByteArray exact16MiB(kPlaylistShareMaximumUncompressedBytes, 'S');
	const auto enc16 = EncodePlaylistBytes(exact16MiB, kPlaylistShareCodecCaps);
	CHECK(bool(enc16));
	const auto dec16 = DecodePlaylistBytes(enc16.bytes, kPlaylistShareCodecCaps);
	CHECK(bool(dec16) && dec16.bytes.size() == kPlaylistShareMaximumUncompressedBytes);
	CHECK(dec16.bytes == exact16MiB);
	CHECK(dec16.bytes.capacity() <= kPlaylistShareMaximumUncompressedBytes + 4096);

	const QByteArray shareBombPlain(kPlaylistShareMaximumUncompressedBytes + 1, 'S');
	const auto shareBombGzip = IndependentDeflateGzip(shareBombPlain);
	CHECK(!shareBombGzip.isEmpty() && shareBombGzip.size() < 64 * 1024);
	const auto decShareBomb = DecodePlaylistBytes(shareBombGzip, kPlaylistShareCodecCaps);
	CHECK(decShareBomb.status == PlaylistCodecStatus::Oversized);
	CHECK(decShareBomb.error == PlaylistCodecError::UncompressedSizeLimit);
	CHECK(decShareBomb.bytes.isEmpty());

	// 4 MiB exact vs 4 MiB + 1 compressed share cap (checked BEFORE decode).
	QByteArray fourMiBExact(kPlaylistShareMaximumCompressedBytes, '\x00');
	fourMiBExact[0] = '\x1F';
	fourMiBExact[1] = static_cast<char>(0x8B);
	const auto dec4CompCorrupt = DecodePlaylistBytes(fourMiBExact, kPlaylistShareCodecCaps);
	CHECK(dec4CompCorrupt.status == PlaylistCodecStatus::Invalid);
	QByteArray fourMiBPlusOne(kPlaylistShareMaximumCompressedBytes + 1, '\x00');
	const auto dec4CompOver = DecodePlaylistBytes(fourMiBPlusOne, kPlaylistShareCodecCaps);
	CHECK(dec4CompOver.status == PlaylistCodecStatus::Oversized);
	CHECK(dec4CompOver.error == PlaylistCodecError::CompressedSizeLimit);

	// 32 MiB exact vs REAL 32 MiB + 1 expanding bomb under default record caps (10 MiB / 32 MiB).
	const QByteArray exact32MiB(kPlaylistRecordMaximumUncompressedBytes, 'Z');
	const auto enc32 = EncodePlaylistBytes(exact32MiB);
	CHECK(bool(enc32));
	const auto dec32 = DecodePlaylistBytes(enc32.bytes);
	CHECK(bool(dec32) && dec32.bytes.size() == kPlaylistRecordMaximumUncompressedBytes);
	CHECK(dec32.bytes == exact32MiB);
	CHECK(dec32.bytes.capacity() <= kPlaylistRecordMaximumUncompressedBytes + 4096);

	const QByteArray bomb32Plain(kPlaylistRecordMaximumUncompressedBytes + 1, 'Z');
	CHECK(EncodePlaylistBytes(bomb32Plain).status == PlaylistCodecStatus::Oversized);
	const auto bomb32Gzip = IndependentDeflateGzip(bomb32Plain);
	CHECK(!bomb32Gzip.isEmpty() && bomb32Gzip.size() < 64 * 1024);
	const auto decBomb32 = DecodePlaylistBytes(bomb32Gzip);
	CHECK(decBomb32.status == PlaylistCodecStatus::Oversized);
	CHECK(decBomb32.error == PlaylistCodecError::UncompressedSizeLimit);
	CHECK(decBomb32.bytes.isEmpty());

	// 34 MiB bomb also aborts promptly without expanding past cap.
	const auto bomb34Gzip = IndependentDeflateGzip(
		QByteArray(kPlaylistRecordMaximumUncompressedBytes + 2 * 1024 * 1024, 'B'));
	CHECK(!bomb34Gzip.isEmpty() && bomb34Gzip.size() < 128 * 1024);
	const auto decBomb34 = DecodePlaylistBytes(bomb34Gzip);
	CHECK(decBomb34.status == PlaylistCodecStatus::Oversized);
	CHECK(decBomb34.error == PlaylistCodecError::UncompressedSizeLimit);
	CHECK(decBomb34.bytes.isEmpty());

	// >10 MiB compressed input classified Oversized BEFORE inflate (even without gzip magic).
	QByteArray tenMiBPlusOne(kPlaylistRecordMaximumCompressedBytes + 1, '\xAA');
	const auto decTenCompOver = DecodePlaylistBytes(tenMiBPlusOne);
	CHECK(decTenCompOver.status == PlaylistCodecStatus::Oversized);
	CHECK(decTenCompOver.error == PlaylistCodecError::CompressedSizeLimit);
	CHECK(decTenCompOver.bytes.isEmpty());
}

void TestWrappersMembersTrailingTruncationAndCorruptions() {
	const QByteArray msg = "hello playlist codec strict gzip frame";
	const auto enc = EncodePlaylistBytes(msg);
	CHECK(bool(enc));
	const auto gzip = enc.bytes;

	// Missing / wrong magic / zlib / raw / qCompress wrappers.
	for (const QByteArray &bad : {
			QByteArray(),
			QByteArray("\x1F", 1),
			QByteArray("\x1F\x8A\x08\x00\x00\x00\x00\x00\x00\xFF", 10),
			qCompress(msg),
			IndependentDeflateWithWindowBits(msg, MAX_WBITS),
			IndependentDeflateWithWindowBits(msg, -MAX_WBITS),
		}) {
		const auto dec = DecodePlaylistBytes(bad);
		CHECK(dec.status == PlaylistCodecStatus::Invalid);
		CHECK(dec.error == PlaylistCodecError::InvalidGzip);
		CHECK(dec.bytes.isEmpty());
	}

	// Concatenated second member and trailing bytes (zero padding, LF, arbitrary byte).
	const auto enc2 = EncodePlaylistBytes(QByteArray("second"));
	CHECK(bool(enc2));
	for (const QByteArray &trailing : {
			gzip + enc2.bytes,
			gzip + gzip,
			gzip + QByteArray(1, '\0'),
			gzip + QByteArray(16, '\0'),
			gzip + QByteArray("\n"),
			gzip + QByteArray("\xFF", 1),
		}) {
		const auto dec = DecodePlaylistBytes(trailing);
		CHECK(dec.status == PlaylistCodecStatus::Invalid);
		CHECK(dec.error == PlaylistCodecError::InvalidGzip);
		CHECK(dec.bytes.isEmpty());
	}

	// Corrupt header fields (ID1, ID2, CM, reserved FLG bits 0x20/0x40/0x80).
	for (const qsizetype offset : { qsizetype(0), qsizetype(1), qsizetype(2) }) {
		auto broken = gzip;
		broken[offset] = static_cast<char>(static_cast<unsigned char>(broken.at(offset)) ^ 0x01U);
		const auto dec = DecodePlaylistBytes(broken);
		CHECK(dec.status == PlaylistCodecStatus::Invalid);
		CHECK(dec.error == PlaylistCodecError::InvalidGzip);
		CHECK(dec.bytes.isEmpty());
	}
	for (const unsigned char flagBit : { 0x20U, 0x40U, 0x80U }) {
		auto broken = gzip;
		broken[3] = static_cast<char>(static_cast<unsigned char>(broken.at(3)) | flagBit);
		const auto dec = DecodePlaylistBytes(broken);
		CHECK(dec.status == PlaylistCodecStatus::Invalid);
		CHECK(dec.error == PlaylistCodecError::InvalidGzip);
		CHECK(dec.bytes.isEmpty());
	}

	// Corrupt FHCRC, unterminated FNAME, and truncated FEXTRA.
	{
		std::string name = "abc.json";
		gz_header hdr{};
		hdr.name = reinterpret_cast<Bytef *>(name.data());
		hdr.hcrc = 1;
		auto namedGzip = IndependentDeflateGzip(msg, 6, &hdr);
		CHECK(bool(DecodePlaylistBytes(namedGzip)));
		namedGzip[19] = static_cast<char>(static_cast<unsigned char>(namedGzip.at(19)) ^ 0xFFU);
		CHECK(DecodePlaylistBytes(namedGzip).status == PlaylistCodecStatus::Invalid);

		QByteArray unterminated("\x1F\x8B\x08\x08\x00\x00\x00\x00\x00\xFF" "unterminated", 22);
		CHECK(DecodePlaylistBytes(unterminated).status == PlaylistCodecStatus::Invalid);

		QByteArray badExtra("\x1F\x8B\x08\x04\x00\x00\x00\x00\x00\xFF\x64\x00" "abcd", 16);
		CHECK(DecodePlaylistBytes(badExtra).status == PlaylistCodecStatus::Invalid);
	}

	// Flip deflate body bytes (avoiding trailing final-byte padding bits) and every CRC32/ISIZE byte.
	std::vector<qsizetype> flipOffsets{ 10, 11, 10 + (gzip.size() - 18) / 2 };
	for (qsizetype i = gzip.size() - 8; i < gzip.size(); ++i) {
		flipOffsets.push_back(i);
	}
	for (const qsizetype i : flipOffsets) {
		auto broken = gzip;
		broken[i] = static_cast<char>(static_cast<unsigned char>(broken.at(i)) ^ 0x80U);
		const auto dec = DecodePlaylistBytes(broken);
		CHECK(dec.status == PlaylistCodecStatus::Invalid);
		CHECK(dec.error == PlaylistCodecError::InvalidGzip);
		CHECK(dec.bytes.isEmpty());
	}
	{
		auto badCrcWord = gzip;
		WriteLe32(badCrcWord, badCrcWord.size() - 8, ReadLe32(badCrcWord, badCrcWord.size() - 8) ^ 1U);
		CHECK(DecodePlaylistBytes(badCrcWord).status == PlaylistCodecStatus::Invalid);
		auto badIsizeWord = gzip;
		WriteLe32(badIsizeWord, badIsizeWord.size() - 4, ReadLe32(badIsizeWord, badIsizeWord.size() - 4) + 1U);
		CHECK(DecodePlaylistBytes(badIsizeWord).status == PlaylistCodecStatus::Invalid);
	}

	// Truncation at EVERY byte offset from 0 through gzip.size() - 1 fails cleanly with no prefix.
	for (qsizetype len = 0; len < gzip.size(); ++len) {
		const auto dec = DecodePlaylistBytes(gzip.left(len));
		CHECK(dec.status == PlaylistCodecStatus::Invalid);
		CHECK(dec.error == PlaylistCodecError::InvalidGzip);
		CHECK(dec.bytes.isEmpty());
	}
}

void TestInvalidLargeCapsImmutabilityAndRepeatedReuse() {
	const QByteArray plain = "repeated state check";
	const QByteArray plainCopy = plain;
	const auto enc = EncodePlaylistBytes(plain);
	CHECK(bool(enc));
	const QByteArray gzip = enc.bytes;
	const QByteArray gzipCopy = gzip;

	// Negative and unrepresentable caps rejected without wraparound.
	std::vector<PlaylistCodecCaps> badCaps{
		{ -1, 1024 },
		{ 1024, -1 },
		{ -1, -1 },
		{ std::numeric_limits<int64_t>::min(), 1024 },
		{ 1024, std::numeric_limits<int64_t>::min() },
		{ static_cast<int64_t>(std::numeric_limits<uint64_t>::max()), 1024 },
		{ 1024, static_cast<int64_t>(uint64_t(1) << 63) },
	};
	if constexpr (QByteArray::maxSize() < std::numeric_limits<int64_t>::max()) {
		badCaps.push_back({ int64_t(QByteArray::maxSize()) + 1, 1024 });
		badCaps.push_back({ 1024, int64_t(QByteArray::maxSize()) + 1 });
		badCaps.push_back({ std::numeric_limits<int64_t>::max(), 1024 });
		badCaps.push_back({ 1024, std::numeric_limits<int64_t>::max() });
	}
	for (const auto &caps : badCaps) {
		const auto badEnc = EncodePlaylistBytes(plain, caps);
		CHECK(badEnc.status == PlaylistCodecStatus::Invalid);
		CHECK(badEnc.error == PlaylistCodecError::InvalidCap);
		CHECK(badEnc.bytes.isEmpty());

		const auto badDec = DecodePlaylistBytes(gzip, caps);
		CHECK(badDec.status == PlaylistCodecStatus::Invalid);
		CHECK(badDec.error == PlaylistCodecError::InvalidCap);
		CHECK(badDec.bytes.isEmpty());

		const auto badDecEmpty = DecodePlaylistBytes(QByteArray(), caps);
		CHECK(badDecEmpty.status == PlaylistCodecStatus::Invalid);
		CHECK(badDecEmpty.error == PlaylistCodecError::InvalidCap);
	}

	// Arbitrarily large representable caps succeed on small input without overflow or giant reserve.
	for (const auto &hugeCaps : {
			PlaylistCodecCaps{ int64_t(1) << 33, int64_t(1) << 40 },
			PlaylistCodecCaps{ int64_t(1) << 50, int64_t(QByteArray::maxSize()) },
		}) {
		const auto okEnc = EncodePlaylistBytes(plain, hugeCaps);
		CHECK(bool(okEnc) && okEnc.bytes == gzip);
		CHECK(okEnc.bytes.capacity() < 4096);
		const auto okDec = DecodePlaylistBytes(gzip, hugeCaps);
		CHECK(bool(okDec) && okDec.bytes == plain);
		CHECK(okDec.bytes.capacity() < 4096);
	}

	// Repeated alternating failures prove no sticky encoder/decoder state & input immutability.
	CHECK(DecodePlaylistBytes(QByteArray("not gzip")).status == PlaylistCodecStatus::Invalid);
	CHECK(DecodePlaylistBytes(gzip, { gzip.size() - 1, 1024 }).status == PlaylistCodecStatus::Oversized);
	CHECK(DecodePlaylistBytes(gzip, { 1024, 3 }).status == PlaylistCodecStatus::Oversized);
	CHECK(DecodePlaylistBytes(gzip + gzip).status == PlaylistCodecStatus::Invalid);
	CHECK(EncodePlaylistBytes(plain, { 1024, 3 }).status == PlaylistCodecStatus::Oversized);

	const auto afterEnc = EncodePlaylistBytes(plain);
	CHECK(bool(afterEnc) && afterEnc.bytes == gzip);
	const auto afterDec = DecodePlaylistBytes(afterEnc.bytes);
	CHECK(bool(afterDec) && afterDec.bytes == plain);
	CHECK(plain == plainCopy);
	CHECK(gzip == gzipCopy);
}

} // namespace

int main() {
	TestRealAllocatorBoundBeforeLargeFixtures();
	TestTwoPassSingleAllocationAndPass2Guards();
	TestRoundTripsHeaderDeterminismAndZlibInterop();
	TestChunkAndCapBoundaries();
	TestRealBombsShareCapsAndCompressedPreCheck();
	TestWrappersMembersTrailingTruncationAndCorruptions();
	TestInvalidLargeCapsImmutabilityAndRepeatedReuse();

	std::printf("Checks: %d, Failures: %d\n", Checks, Failures);
	return Failures == 0 ? 0 : 1;
}
