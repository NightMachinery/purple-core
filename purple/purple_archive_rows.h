/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_archive_codec.h"
#include "purple/purple_archive_envelope.h"

#include <variant>

namespace Purple {

using ArchiveRowValue = std::variant<ArchiveState, ArchiveMessage,
	ArchiveVersionSnapshot, ArchiveEventReceipt, ArchiveBlob, ArchiveMediaJob,
	ArchiveReaderLease, ArchiveChatGeneration>;

struct ArchiveVersionBody {
	QString hash;
	int64 bytes = 0;
	friend bool operator==(const ArchiveVersionBody &, const ArchiveVersionBody &) = default;
};

struct ArchiveRow {
	ArchiveRowValue value;
	QJsonObject preserved;
	std::optional<ArchiveVersionBody> body;
};

struct ArchiveRowDecodeResult {
	ArchiveCodecStatus status = ArchiveCodecStatus::Invalid;
	std::optional<ArchiveRow> row;
};

[[nodiscard]] ArchiveRecordKind ArchiveRowKind(const ArchiveRow &row);
[[nodiscard]] std::optional<QByteArray> EncodeArchiveRow(const ArchiveRow &row);
[[nodiscard]] ArchiveRowDecodeResult DecodeArchiveRow(const QByteArray &bytes);

}
