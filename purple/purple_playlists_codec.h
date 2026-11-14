/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include <QtCore/QByteArray>

#include <cstdint>

namespace Purple {

inline constexpr auto kPlaylistRecordMaximumCompressedBytes = qsizetype(10) * 1024 * 1024;
inline constexpr auto kPlaylistRecordMaximumUncompressedBytes = qsizetype(32) * 1024 * 1024;
inline constexpr auto kPlaylistShareMaximumCompressedBytes = qsizetype(4) * 1024 * 1024;
inline constexpr auto kPlaylistShareMaximumUncompressedBytes = qsizetype(16) * 1024 * 1024;

enum class PlaylistCodecStatus {
	Valid,
	Invalid,
	Oversized,
};

enum class PlaylistCodecError {
	None,
	InvalidCap,
	InvalidGzip,
	CompressedSizeLimit,
	UncompressedSizeLimit,
	CodecFailure,
};

struct PlaylistCodecCaps {
	int64_t maximumCompressedBytes = kPlaylistRecordMaximumCompressedBytes;
	int64_t maximumUncompressedBytes = kPlaylistRecordMaximumUncompressedBytes;

	[[nodiscard]] bool operator==(const PlaylistCodecCaps &) const = default;
};

inline constexpr PlaylistCodecCaps kPlaylistRecordCodecCaps{
	kPlaylistRecordMaximumCompressedBytes,
	kPlaylistRecordMaximumUncompressedBytes,
};

inline constexpr PlaylistCodecCaps kPlaylistShareCodecCaps{
	kPlaylistShareMaximumCompressedBytes,
	kPlaylistShareMaximumUncompressedBytes,
};

struct PlaylistCodecResult {
	QByteArray bytes;
	PlaylistCodecStatus status = PlaylistCodecStatus::Invalid;
	PlaylistCodecError error = PlaylistCodecError::None;

	[[nodiscard]] explicit operator bool() const {
		return status == PlaylistCodecStatus::Valid;
	}
	[[nodiscard]] bool operator==(const PlaylistCodecResult &) const = default;
};

// Byte-only gzip codec; callers enforce canonical JSON validity after decode.
[[nodiscard]] PlaylistCodecResult EncodePlaylistBytes(
	const QByteArray &plainBytes,
	const PlaylistCodecCaps &caps = kPlaylistRecordCodecCaps);

[[nodiscard]] PlaylistCodecResult DecodePlaylistBytes(
	const QByteArray &gzipBytes,
	const PlaylistCodecCaps &caps = kPlaylistRecordCodecCaps);

} // namespace Purple
