/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_config_payload.h"
#include "purple/purple_sync_directory.h"
#include "purple/purple_sync_envelope.h"
#include "purple/purple_sync_local_state.h"
#include "purple/purple_types.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace Purple {

inline constexpr auto kSyncHistoryPageSize = 100;
inline constexpr auto kSyncRecordMaximumBytes = 4 * 1024 * 1024;

[[nodiscard]] QString SyncRecordCaptionTag();
[[nodiscard]] QString SyncSettingsRecordFileName();
[[nodiscard]] QString SyncPlaylistsRecordFileName();

struct SyncHistoryMessageMeta {
	bool isMessage = false;
	bool forwarded = false;
	bool isDocument = false;
	QString caption;
	std::vector<QString> fileNames;
};

[[nodiscard]] bool IsSyncHistoryCandidate(const SyncHistoryMessageMeta &meta);

struct SyncHistoryPageItem {
	int64_t id = 0;
	bool candidate = false;
};

enum class SyncHistoryPageStatus {
	More,
	Complete,
	Stalled,
};

class SyncHistoryPages final {
public:
	[[nodiscard]] SyncHistoryPageStatus Add(
			const std::vector<SyncHistoryPageItem> &page) {
		if (page.empty()) {
			return SyncHistoryPageStatus::Complete;
		}
		auto oldest = int32_t(0);
		auto previous = int64_t(0);
		for (const auto &item : page) {
			if (item.id <= 0
				|| item.id > std::numeric_limits<int32_t>::max()) {
				return SyncHistoryPageStatus::Stalled;
			}
			if ((_offset && item.id >= _offset)
				|| (previous && item.id >= previous)) {
				return SyncHistoryPageStatus::Stalled;
			}
			previous = item.id;
			oldest = int32_t(item.id);
		}
		for (const auto &item : page) {
			if (item.candidate) {
				_candidates.push_back(int32_t(item.id));
			}
		}
		_count += page.size();
		_offset = oldest;
		return SyncHistoryPageStatus::More;
	}

	[[nodiscard]] int32_t offset() const { return _offset; }
	[[nodiscard]] uint64_t count() const { return _count; }
	[[nodiscard]] const std::vector<int32_t> &candidates() const {
		return _candidates;
	}

private:
	std::vector<int32_t> _candidates;
	uint64_t _count = 0;
	int32_t _offset = 0;

};

enum class SyncHistoryScanStatus {
	Complete,
	Cancelled,
	RequestFailed,
	InvalidResponse,
	Stalled,
};

struct SyncHistoryScanResult {
	SyncHistoryScanStatus status = SyncHistoryScanStatus::InvalidResponse;
	std::vector<int32_t> candidateIds;
	uint64_t scannedCount = 0;
	QString error;

	[[nodiscard]] bool complete() const {
		return status == SyncHistoryScanStatus::Complete;
	}
};

enum class SyncCandidateStatus {
	Valid,
	NewerSchema,
	NewerMajor,
	UnsupportedStream,
	UnsupportedEncoding,
	UnsupportedLibrary,
	Invalid,
	Vanished,
	Changed,
	Oversized,
	Inaccessible,
	RequestFailed,
	Cancelled,
};

struct SyncCandidateRecord {
	int32_t id = 0;
	SyncCandidateStatus status = SyncCandidateStatus::Invalid;
	QByteArray bytes;
	std::optional<SyncEnvelopeHeader> header;
	SyncEnvelopeError envelopeError = SyncEnvelopeError::None;
	ConfigPayloadError configError = ConfigPayloadError::None;
	uint64_t documentId = 0;
	uint64_t editDate = 0;
};

enum class SyncCandidateReadStatus {
	Complete,
	NeedsReview,
	Incomplete,
};

struct SyncCandidateReadResult {
	SyncCandidateReadStatus status = SyncCandidateReadStatus::Incomplete;
	std::vector<SyncCandidateRecord> records;

	[[nodiscard]] bool complete() const {
		return status == SyncCandidateReadStatus::Complete;
	}
};

[[nodiscard]] SyncCandidateRecord ClassifySyncCandidate(
	int32_t id,
	QByteArray bytes);
[[nodiscard]] SyncCandidateReadStatus AggregateSyncCandidateRead(
	const std::vector<SyncCandidateRecord> &records);

enum class SyncAccountInventoryStatus {
	Complete,
	NeedsReview,
	Incomplete,
};

struct SyncAccountInventoryResult {
	SyncAccountInventoryStatus status = SyncAccountInventoryStatus::Incomplete;
	uint64_t accountUserId = 0;
	SyncHistoryScanResult scan;
	std::optional<SyncCandidateReadResult> read;
	SyncDirectory directory;
};

[[nodiscard]] SyncDirectoryCandidate SyncDirectoryCandidateOf(
	const SyncCandidateRecord &record);
[[nodiscard]] SyncAccountInventoryResult FinishSyncAccountInventory(
	uint64_t accountUserId,
	SyncHistoryScanResult scan,
	std::optional<SyncCandidateReadResult> read);

enum class SyncOwnInventoryStatus {
	Incomplete,
	NeedsReview,
	Absent,
	Present,
	PendingFound,
	CloneDetected,
};

struct SyncOwnInventoryResult {
	SyncOwnInventoryStatus status = SyncOwnInventoryStatus::Incomplete;
	OwnRecordObservation observation;
	SyncPublishReadBack head;
	std::optional<int32_t> pendingMessageId;
	SyncCloneVerdict cloneVerdict = SyncCloneVerdict::PendingReconcile;
	std::vector<int32_t> duplicateHeadMessageIds;
};

[[nodiscard]] SyncOwnInventoryResult ReconcileOwnConfigInventory(
	const SyncLocalState &state,
	const SyncAccountInventoryResult &inventory,
	uint64_t expectedAccountUserId,
	const QByteArray &stagedCanonicalRecord = {});

} // namespace Purple
