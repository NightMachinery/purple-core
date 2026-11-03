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

// Finalising an interrupted Ogg/Opus voice recording (F10, voice recovery).
//
// A recorder that crashes leaves an Ogg file whose last page is torn or
// missing and whose final page lacks the end-of-stream flag. Finalising it:
//
//   1. walks the Ogg pages from the start and checks each page's CRC;
//   2. truncates after the last good page (a torn page, a bad CRC or bytes
//      that are not a page stop the walk; nothing after it is kept, even if
//      a later page would be good);
//   3. sets the end-of-stream flag on that page and recomputes its CRC;
//   4. reads the duration from its granule position (Opus: 48 kHz, minus the
//      OpusHead pre-skip).
//
// Pure C++17 with no Qt, so both clients compile it: Android through its JNI
// library, Desktop directly. The page walker is public because Desktop's
// preview trim cuts on page boundaries with it. See
// docs/media-recording-design.md, F10 "Voice", and docs/recording-formats.md.
namespace Purple {

// One Ogg page, as the walker found it.
struct OggPage {
	// Byte offset of "OggS" and the page's total size, header included.
	std::uint64_t offset = 0;
	std::uint32_t size = 0;

	// The header fields. granule is -1 on a page where no packet ends.
	std::int64_t granule = -1;
	std::uint32_t serial = 0;
	std::uint32_t sequence = 0;
	std::uint8_t flags = 0;

	// Whether the page's last packet ends on it (its last lacing value is
	// below 255). False when the packet continues onto the next page.
	bool packetEnds = true;

	static constexpr std::uint8_t kContinued = 0x01;
	static constexpr std::uint8_t kBeginOfStream = 0x02;
	static constexpr std::uint8_t kEndOfStream = 0x04;
};

// The result of walking a stream.
struct OggScan {
	// The good prefix, in file order.
	std::vector<OggPage> pages;

	// End of the last good page, and the size that was walked. Bytes past
	// goodBytes are torn, corrupt or not Ogg.
	std::uint64_t goodBytes = 0;
	std::uint64_t totalBytes = 0;

	// The last good page carries the end-of-stream flag.
	bool endOfStream = false;

	// The first packet is an OpusHead; preSkip is its pre-skip (48 kHz).
	bool opus = false;
	int preSkip = 0;

	// The last granule position of the good prefix (-1 for none), and the
	// duration it gives in milliseconds: for Opus (granule - preSkip) / 48,
	// at least 0; -1 when the stream is not Opus or has no granule.
	std::int64_t lastGranule = -1;
	std::int64_t durationMs = -1;

	// Whether the good prefix holds any audio, not only the Opus headers.
	[[nodiscard]] bool hasAudio() const {
		return lastGranule > 0;
	}

	// Whether the file is already a finished stream: every byte is a good
	// page and the last one ends the stream.
	[[nodiscard]] bool clean() const {
		return !pages.empty() && endOfStream && goodBytes == totalBytes;
	}
};

// The Ogg CRC-32 (polynomial 0x04C11DB7, not reflected, initial value 0, no
// final xor), over the bytes given. A page's CRC is computed with its own
// CRC field set to zero.
[[nodiscard]] std::uint32_t OggCrc(const std::uint8_t *data, std::size_t size);

// Walks the pages of an Ogg stream held in memory. Never reads past `size'.
[[nodiscard]] OggScan ScanOgg(const std::uint8_t *data, std::size_t size);

enum class OggFinishStatus {
	// Already finished: nothing was changed.
	Clean,
	// Truncated after the last good page and/or given the end-of-stream flag.
	Finalised,
	// No good page with audio: the bytes are left as they were, so the bin
	// can keep them raw.
	Unrecoverable,
};

struct OggFinish {
	OggFinishStatus status = OggFinishStatus::Unrecoverable;

	// The stream as it is after finalising (for Unrecoverable, as found).
	OggScan scan;

	// Bytes dropped from the end, and whether the flag had to be set.
	std::uint64_t droppedBytes = 0;
	bool endOfStreamSet = false;

	// Set by FinaliseOggFile when the file could not be read or written; the
	// status is then Unrecoverable and the file may be untouched.
	bool ioError = false;
};

// Finalises a stream held in memory: truncates `bytes' after the last good
// page and sets the end-of-stream flag on it. Unrecoverable leaves `bytes'
// unchanged.
[[nodiscard]] OggFinish FinaliseOgg(std::vector<std::uint8_t> &bytes);

// The same for a file, in place: the last good page's header is rewritten
// and the file is truncated. The path is UTF-8 on every platform.
[[nodiscard]] OggFinish FinaliseOggFile(const std::string &path);

} // namespace Purple
