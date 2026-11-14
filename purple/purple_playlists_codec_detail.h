/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_playlists_codec.h"

#include <QtCore/QByteArray>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

#include <zlib.h>

namespace Purple::PlaylistsCodecDetail {

inline constexpr auto kCodecChunkBytes = qsizetype(32) * 1024;
inline constexpr auto kInputFeedBytes = qsizetype(1) * 1024 * 1024;
inline constexpr auto kGzipLevel = 6;
inline constexpr auto kGzipWindowBits = 16 + MAX_WBITS;
inline constexpr auto kGzipOsUnknown = 255;

struct DeflateGuard {
	z_stream stream{};
	bool active = false;

	~DeflateGuard() {
		if (active) {
			deflateEnd(&stream);
		}
	}
};

struct InflateGuard {
	z_stream stream{};
	bool active = false;

	~InflateGuard() {
		if (active) {
			inflateEnd(&stream);
		}
	}
};

struct PassResult {
	bool ok = false;
	qsizetype produced = 0;
	PlaylistCodecResult failure;
};

struct QByteArrayOutputFactory {
	struct Buffer {
		QByteArray bytes;

		[[nodiscard]] bool allocate(qsizetype exactSize) {
			if (exactSize < 0) {
				return false;
			}
			if (exactSize > 0) {
				bytes = QByteArray(exactSize, Qt::Uninitialized);
				return bytes.size() == exactSize;
			}
			bytes = QByteArray();
			return true;
		}

		[[nodiscard]] char *writableData() {
			return bytes.isEmpty() ? nullptr : bytes.data();
		}

		[[nodiscard]] QByteArray take() {
			return std::move(bytes);
		}
	};

	[[nodiscard]] Buffer create() const {
		return {};
	}
};

[[nodiscard]] inline bool ValidateCap(int64_t cap) noexcept {
	return cap >= 0
		&& cap <= int64_t(std::numeric_limits<qsizetype>::max())
		&& cap <= int64_t(QByteArray::maxSize());
}

[[nodiscard]] inline bool ValidateCaps(const PlaylistCodecCaps &caps) noexcept {
	return ValidateCap(caps.maximumCompressedBytes)
		&& ValidateCap(caps.maximumUncompressedBytes);
}

[[nodiscard]] inline PlaylistCodecResult MakeCodecFailure(
		PlaylistCodecStatus status,
		PlaylistCodecError error) {
	return { QByteArray(), status, error };
}

inline void FeedInputChunk(
		z_stream &stream,
		const QByteArray &input,
		qsizetype &offset) {
	static constexpr Bytef kEmptyInput = 0;
	if (stream.avail_in != 0) {
		return;
	}
	if (offset < input.size()) {
		const auto remaining = input.size() - offset;
		const auto feed = static_cast<uInt>(std::min<qsizetype>(remaining, kInputFeedBytes));
		stream.next_in = const_cast<Bytef *>(
			reinterpret_cast<const Bytef *>(input.constData() + offset));
		stream.avail_in = feed;
		offset += static_cast<qsizetype>(feed);
	} else {
		stream.next_in = const_cast<Bytef *>(&kEmptyInput);
	}
}

// WHY: Pass 1 validates caps and counts exact bytes with 32 KiB scratch without trusting
// ISIZE, so Pass 2 allocates once without QByteArray realloc overlap or huge-cap reserves.
[[nodiscard]] inline bool RecordBoundedChunk(
		const std::array<Bytef, kCodecChunkBytes> &chunk,
		qsizetype have,
		qsizetype &produced,
		int64_t maximumBytes,
		std::optional<qsizetype> expectedLength,
		char *outPtr,
		PlaylistCodecError limitError,
		PlaylistCodecResult &failure) {
	if (have <= 0) {
		return true;
	}
	if (int64_t(have) > maximumBytes - int64_t(produced)) {
		failure = MakeCodecFailure(PlaylistCodecStatus::Oversized, limitError);
		return false;
	}
	if (expectedLength.has_value()) {
		if (have > *expectedLength - produced || outPtr == nullptr) {
			failure = MakeCodecFailure(
				PlaylistCodecStatus::Invalid,
				PlaylistCodecError::CodecFailure);
			return false;
		}
		std::memcpy(outPtr + produced, chunk.data(), static_cast<std::size_t>(have));
	}
	produced += have;
	return true;
}

[[nodiscard]] inline PassResult RunEncodePass(
		const QByteArray &plainBytes,
		int64_t maximumCompressedBytes,
		std::optional<qsizetype> expectedLength,
		char *outPtr) {
	DeflateGuard guard;
	// windowBits 16+15 selects gzip wrapper; mtime=0 and os=255 keep output deterministic per zlib build.
	if (deflateInit2(
			&guard.stream,
			kGzipLevel,
			Z_DEFLATED,
			kGzipWindowBits,
			8,
			Z_DEFAULT_STRATEGY) != Z_OK) {
		return {
			false,
			0,
			MakeCodecFailure(PlaylistCodecStatus::Invalid, PlaylistCodecError::CodecFailure),
		};
	}
	guard.active = true;

	gz_header header{};
	header.time = 0;
	header.os = kGzipOsUnknown;
	if (deflateSetHeader(&guard.stream, &header) != Z_OK) {
		return {
			false,
			0,
			MakeCodecFailure(PlaylistCodecStatus::Invalid, PlaylistCodecError::CodecFailure),
		};
	}

	std::array<Bytef, kCodecChunkBytes> chunk{};
	qsizetype inputOffset = 0;
	qsizetype produced = 0;

	while (true) {
		FeedInputChunk(guard.stream, plainBytes, inputOffset);
		const auto flush = (inputOffset >= plainBytes.size()) ? Z_FINISH : Z_NO_FLUSH;
		const auto beforeIn = guard.stream.avail_in;
		guard.stream.next_out = chunk.data();
		guard.stream.avail_out = static_cast<uInt>(chunk.size());

		const auto ret = deflate(&guard.stream, flush);
		if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
			return {
				false,
				0,
				MakeCodecFailure(PlaylistCodecStatus::Invalid, PlaylistCodecError::CodecFailure),
			};
		}
		const auto have = static_cast<qsizetype>(chunk.size() - guard.stream.avail_out);
		if ((ret == Z_BUF_ERROR && have == 0)
			|| (beforeIn == guard.stream.avail_in && have == 0 && ret != Z_STREAM_END)) {
			return {
				false,
				0,
				MakeCodecFailure(PlaylistCodecStatus::Invalid, PlaylistCodecError::CodecFailure),
			};
		}
		PlaylistCodecResult failure;
		if (!RecordBoundedChunk(
				chunk,
				have,
				produced,
				maximumCompressedBytes,
				expectedLength,
				outPtr,
				PlaylistCodecError::CompressedSizeLimit,
				failure)) {
			return { false, 0, std::move(failure) };
		}
		if (ret == Z_STREAM_END) {
			if (guard.stream.avail_in != 0 || inputOffset < plainBytes.size()) {
				return {
					false,
					0,
					MakeCodecFailure(PlaylistCodecStatus::Invalid, PlaylistCodecError::CodecFailure),
				};
			}
			if (expectedLength.has_value() && produced != *expectedLength) {
				return {
					false,
					0,
					MakeCodecFailure(PlaylistCodecStatus::Invalid, PlaylistCodecError::CodecFailure),
				};
			}
			return { true, produced, {} };
		}
	}
}

[[nodiscard]] inline PassResult RunDecodePass(
		const QByteArray &gzipBytes,
		int64_t maximumUncompressedBytes,
		std::optional<qsizetype> expectedLength,
		char *outPtr) {
	InflateGuard guard;
	if (inflateInit2(&guard.stream, kGzipWindowBits) != Z_OK) {
		return {
			false,
			0,
			MakeCodecFailure(PlaylistCodecStatus::Invalid, PlaylistCodecError::CodecFailure),
		};
	}
	guard.active = true;

	std::array<Bytef, kCodecChunkBytes> chunk{};
	qsizetype inputOffset = 0;
	qsizetype produced = 0;

	while (true) {
		FeedInputChunk(guard.stream, gzipBytes, inputOffset);
		guard.stream.next_out = chunk.data();
		guard.stream.avail_out = static_cast<uInt>(chunk.size());

		const auto beforeIn = guard.stream.avail_in;
		const auto ret = inflate(&guard.stream, Z_NO_FLUSH);
		if (ret != Z_OK && ret != Z_STREAM_END) {
			return {
				false,
				0,
				MakeCodecFailure(
					PlaylistCodecStatus::Invalid,
					(ret == Z_MEM_ERROR)
						? PlaylistCodecError::CodecFailure
						: PlaylistCodecError::InvalidGzip),
			};
		}
		const auto have = static_cast<qsizetype>(chunk.size() - guard.stream.avail_out);
		PlaylistCodecResult failure;
		if (!RecordBoundedChunk(
				chunk,
				have,
				produced,
				maximumUncompressedBytes,
				expectedLength,
				outPtr,
				PlaylistCodecError::UncompressedSizeLimit,
				failure)) {
			return { false, 0, std::move(failure) };
		}
		if (ret == Z_STREAM_END) {
			if (guard.stream.avail_in != 0 || inputOffset < gzipBytes.size()) {
				return {
					false,
					0,
					MakeCodecFailure(PlaylistCodecStatus::Invalid, PlaylistCodecError::InvalidGzip),
				};
			}
			if (expectedLength.has_value() && produced != *expectedLength) {
				return {
					false,
					0,
					MakeCodecFailure(PlaylistCodecStatus::Invalid, PlaylistCodecError::CodecFailure),
				};
			}
			return { true, produced, {} };
		}
		if ((guard.stream.avail_in == 0 && inputOffset >= gzipBytes.size() && have == 0)
			|| (beforeIn == guard.stream.avail_in && have == 0)) {
			return {
				false,
				0,
				MakeCodecFailure(PlaylistCodecStatus::Invalid, PlaylistCodecError::InvalidGzip),
			};
		}
	}
}

template <typename OutputFactory>
[[nodiscard]] PlaylistCodecResult EncodePlaylistBytesWithFactory(
		const QByteArray &plainBytes,
		const PlaylistCodecCaps &caps,
		OutputFactory &&factory) {
	if (!ValidateCaps(caps)) {
		return MakeCodecFailure(PlaylistCodecStatus::Invalid, PlaylistCodecError::InvalidCap);
	}
	if (int64_t(plainBytes.size()) > caps.maximumUncompressedBytes) {
		return MakeCodecFailure(
			PlaylistCodecStatus::Oversized,
			PlaylistCodecError::UncompressedSizeLimit);
	}

	const auto pass1 = RunEncodePass(
		plainBytes,
		caps.maximumCompressedBytes,
		std::nullopt,
		nullptr);
	if (!pass1.ok) {
		return pass1.failure;
	}
	if (pass1.produced <= 0) {
		return MakeCodecFailure(PlaylistCodecStatus::Invalid, PlaylistCodecError::CodecFailure);
	}

	auto buffer = factory.create();
	if (!buffer.allocate(pass1.produced)) {
		return MakeCodecFailure(PlaylistCodecStatus::Invalid, PlaylistCodecError::CodecFailure);
	}

	const auto pass2 = RunEncodePass(
		plainBytes,
		caps.maximumCompressedBytes,
		pass1.produced,
		buffer.writableData());
	if (!pass2.ok) {
		return pass2.failure;
	}
	return { buffer.take(), PlaylistCodecStatus::Valid, PlaylistCodecError::None };
}

template <typename OutputFactory>
[[nodiscard]] PlaylistCodecResult DecodePlaylistBytesWithFactory(
		const QByteArray &gzipBytes,
		const PlaylistCodecCaps &caps,
		OutputFactory &&factory) {
	if (!ValidateCaps(caps)) {
		return MakeCodecFailure(PlaylistCodecStatus::Invalid, PlaylistCodecError::InvalidCap);
	}
	if (int64_t(gzipBytes.size()) > caps.maximumCompressedBytes) {
		return MakeCodecFailure(
			PlaylistCodecStatus::Oversized,
			PlaylistCodecError::CompressedSizeLimit);
	}
	if (gzipBytes.size() < 2
		|| static_cast<unsigned char>(gzipBytes.at(0)) != 0x1F
		|| static_cast<unsigned char>(gzipBytes.at(1)) != 0x8B) {
		return MakeCodecFailure(PlaylistCodecStatus::Invalid, PlaylistCodecError::InvalidGzip);
	}

	const auto pass1 = RunDecodePass(
		gzipBytes,
		caps.maximumUncompressedBytes,
		std::nullopt,
		nullptr);
	if (!pass1.ok) {
		return pass1.failure;
	}

	auto buffer = factory.create();
	if (!buffer.allocate(pass1.produced)) {
		return MakeCodecFailure(PlaylistCodecStatus::Invalid, PlaylistCodecError::CodecFailure);
	}

	const auto pass2 = RunDecodePass(
		gzipBytes,
		caps.maximumUncompressedBytes,
		pass1.produced,
		buffer.writableData());
	if (!pass2.ok) {
		return pass2.failure;
	}
	return { buffer.take(), PlaylistCodecStatus::Valid, PlaylistCodecError::None };
}

} // namespace Purple::PlaylistsCodecDetail
