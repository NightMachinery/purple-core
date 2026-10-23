/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_archive_model.h"

namespace Purple {

enum class ArchiveCopySourceKind { StructuralWriter, GenuineSource };
enum class ArchiveCopyExtent { FullBody, Selection, Fragment };
enum class ArchiveCopyTarget { CurrentMessage, ArchivedVersion };

struct ArchiveCopyRequest {
	ArchiveMessageKey key;
	QString version;
	QString revision;
	QString contentHash;
	QString displayedRepresentation = u"original"_q;
	ArchiveCopyExtent extent = ArchiveCopyExtent::FullBody;
	ArchiveCopyTarget target = ArchiveCopyTarget::CurrentMessage;
	ArchiveGeneration generation;
};

struct ArchiveCopySourceChoice {
	ArchiveCopySourceKind kind = ArchiveCopySourceKind::StructuralWriter;
	bool validTarget = false;
	bool partial = true;
	QByteArray bytes;
};

[[nodiscard]] ArchiveCopySourceChoice ChooseCopySource(
	const ArchiveState &state,
	const ArchiveCopyRequest &request);

}
