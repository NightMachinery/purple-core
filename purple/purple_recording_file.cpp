/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_recording_file.h"

#include <filesystem>
#include <fstream>
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

bool ReadAll(const std::string &path, std::vector<std::uint8_t> &bytes) {
	auto error = std::error_code();
	const auto native = Native(path);
	const auto size = std::filesystem::file_size(native, error);
	if (error) {
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
}

bool WriteAt(
		const std::string &path,
		std::uint64_t offset,
		const std::uint8_t *data,
		std::size_t size) {
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
}

bool Truncate(const std::string &path, std::uint64_t size) {
	auto error = std::error_code();
	std::filesystem::resize_file(Native(path), size, error);
	return !error;
}

} // namespace Purple::RecordingFile
