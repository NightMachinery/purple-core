/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_archive_model.h"

namespace Purple::ArchiveInternal {

[[nodiscard]] QJsonObject ContentObject(const ArchiveContent &content);
[[nodiscard]] bool IsCodecFields(const ArchiveState &state);
[[nodiscard]] bool IsMigrationAliases(const ArchiveChatKey &chat,
	const std::vector<ArchiveChatKey> &aliases);
[[nodiscard]] bool IsContent(const ArchiveContent &content);
[[nodiscard]] QJsonObject MissingSlotsObject(const ArchiveMissingSlots &missing);
[[nodiscard]] bool IsMissingSlots(const ArchiveMissingSlots &missing,
	const ArchiveContent &content, ArchiveCompleteness completeness,
	const QString &revision);
[[nodiscard]] bool IsObservation(const ArchiveObservation &observation);
[[nodiscard]] QJsonObject SourceBindingObject(const ArchiveSourceBinding &binding);
[[nodiscard]] bool IsSource(const ArchiveSource &source);
[[nodiscard]] bool SourceOwnedBy(const ArchiveSource &source,
	const ArchiveMessageKey &key, const QString &version);
[[nodiscard]] bool IsOrigin(ArchiveOrigin origin);
[[nodiscard]] bool IsCompleteness(ArchiveCompleteness completeness);
[[nodiscard]] bool IsContentCompleteness(const ArchiveContent &content,
	ArchiveCompleteness completeness);
[[nodiscard]] bool IsDeletion(ArchiveOrigin origin);
[[nodiscard]] bool IsGeneration(const ArchiveState &state,
	const ArchiveMessageKey &key, const ArchiveGeneration &generation);

}
