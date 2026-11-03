/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_recording_file.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>

namespace Purple::RecordingFile {
namespace {

[[nodiscard]] std::filesystem::path Native(const std::string &path) {
#if defined(__cpp_char8_t)
	return std::filesystem::path(std::u8string(path.begin(), path.end()));
#else
	return std::filesystem::u8path(path);
#endif
}

} // namespace

bool ReadAll(
		const std::string &path,
		std::vector<std::uint8_t> &bytes,
		std::uint64_t maxBytes) try {
	bytes.clear();
	auto error = std::error_code();
	const auto native = Native(path);
	const auto size = std::filesystem::file_size(native, error);
	// Checked before the cast, so a 32-bit build never reads a prefix of a
	// file of 4 GiB or more as if it were the whole file.
	if (error
		|| size > maxBytes
		|| size > std::uint64_t(std::numeric_limits<std::size_t>::max())) {
		return false;
	}
	auto file = std::ifstream(native, std::ios::binary);
	if (!file) {
		return false;
	}
	bytes.resize(std::size_t(size));
	if (size && !file.read(
			reinterpret_cast<char*>(bytes.data()),
			std::streamsize(size))) {
		bytes.clear();
		return false;
	}
	return true;
} catch (...) {
	// bad_alloc, or a path std::filesystem refuses: never into JNI.
	bytes.clear();
	return false;
}

bool WriteAt(
		const std::string &path,
		std::uint64_t offset,
		const std::uint8_t *data,
		std::size_t size) try {
	auto file = std::fstream(
		Native(path),
		std::ios::binary | std::ios::in | std::ios::out);
	if (!file) {
		return false;
	}
	file.seekp(std::streamoff(offset));
	file.write(reinterpret_cast<const char*>(data), std::streamsize(size));
	file.flush();
	return bool(file);
} catch (...) {
	return false;
}

bool Truncate(const std::string &path, std::uint64_t size) try {
	auto error = std::error_code();
	std::filesystem::resize_file(Native(path), size, error);
	return !error;
} catch (...) {
	return false;
}

} // namespace Purple::RecordingFile
