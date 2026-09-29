/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_sync_envelope.h"

#include <QtCore/QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace Purple {

struct SyncDirectoryCandidate {
	int64_t messageId = 0;
	uint64_t documentId = 0;
	uint64_t editDate = 0;
	bool original = true;
	SyncEnvelopeStatus status = SyncEnvelopeStatus::Invalid;
	std::optional<SyncEnvelopeHeader> header;
	QString recordHash;
	bool payloadValidated = false;
};

struct SyncDirectoryGroup {
	QString space;
	QString stream;
	QString install;
	std::vector<SyncDirectoryCandidate> records;
	std::vector<SyncDirectoryCandidate> headCandidates;
	bool ambiguous = false;
	bool supportedHead = false;
};

struct SyncDirectory {
	std::vector<SyncDirectoryGroup> groups;
	std::optional<QString> selectedSpace;
	std::optional<QString> publishableSpace;
	bool complete = false;
	bool unreadableCandidate = false;
	bool messageIdCollision = false;
	bool canCreateSpace = false;
};

[[nodiscard]] SyncDirectory ResolveSyncDirectory(
	const std::vector<SyncDirectoryCandidate> &candidates,
	bool inventoryComplete);

} // namespace Purple
