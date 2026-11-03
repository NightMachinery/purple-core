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

// The few file operations the recording repairs need, on UTF-8 paths, with
// std::filesystem underneath so Windows paths outside the ANSI code page work
// too. Every call reports failure instead of throwing.
namespace Purple::RecordingFile {

// The whole file into `bytes'. False if it cannot be opened or read.
[[nodiscard]] bool ReadAll(
	const std::string &path,
	std::vector<std::uint8_t> &bytes);

// Overwrites `size' bytes at `offset' without truncating the file.
[[nodiscard]] bool WriteAt(
	const std::string &path,
	std::uint64_t offset,
	const std::uint8_t *data,
	std::size_t size);

// Cuts the file to `size' bytes.
[[nodiscard]] bool Truncate(const std::string &path, std::uint64_t size);

} // namespace Purple::RecordingFile
