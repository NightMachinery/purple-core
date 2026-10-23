/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_archive_model.h"

namespace Purple {

inline constexpr auto kArchiveCodecByteLimit = 4 * 1024 * 1024;

enum class ArchiveCodecStatus { Valid, NewerSchema, Unsupported, Invalid };

struct ArchiveDecodeResult {
	ArchiveCodecStatus status = ArchiveCodecStatus::Invalid;
	std::optional<ArchiveState> state;
	QByteArray opaque;
	QString error;

	[[nodiscard]] explicit operator bool() const {
		return status == ArchiveCodecStatus::Valid;
	}
};

struct ArchiveVersionSnapshot {
	ArchiveMessageKey key;
	ArchiveVersion version;
	QJsonObject preserved;
	friend bool operator==(const ArchiveVersionSnapshot &, const ArchiveVersionSnapshot &) = default;
};

struct ArchiveVersionDecodeResult {
	ArchiveCodecStatus status = ArchiveCodecStatus::Invalid;
	std::optional<ArchiveVersionSnapshot> snapshot;
	QByteArray opaque;
	QString error;
};

[[nodiscard]] ArchiveVersionDecodeResult DecodeArchiveVersionSnapshot(const QByteArray &bytes);
[[nodiscard]] std::optional<QByteArray> EncodeArchiveVersionSnapshot(
	const ArchiveVersionSnapshot &snapshot);
[[nodiscard]] ArchiveDecodeResult DecodeArchiveSnapshot(const QByteArray &bytes);
[[nodiscard]] std::optional<QByteArray> EncodeArchiveSnapshot(const ArchiveState &state);

}
