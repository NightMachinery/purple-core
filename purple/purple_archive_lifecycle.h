/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_archive_sql.h"

namespace Purple {

enum class ArchiveRemovalAction {
	WriteTombstone,
	FenceWork,
	DrainReadersAndClose,
	DeleteDatabase,
	DeleteWal,
	DeleteShm,
	DeleteRollbackJournal,
	DeleteBlobDirectory,
	DeleteStagingDirectory,
	DeleteWrappedKey,
	DeleteOwnedDirectoryAndBinding,
	ClearTombstone,
	Done,
};

struct ArchiveRemoval {
	ArchiveStoreEpoch epoch;
	QString operationId;
	ArchiveRemovalAction action = ArchiveRemovalAction::WriteTombstone;
	friend bool operator==(const ArchiveRemoval &, const ArchiveRemoval &) = default;
};

struct ArchiveRemovalAck {
	AccountKey account;
	QString storeId;
	QString operationId;
	uint64_t generation = 0;
	ArchiveRemovalAction action = ArchiveRemovalAction::WriteTombstone;
	bool success = false;
	bool durable = false;
	uint64_t readers = 0;
	uint64_t writers = 0;
	bool databaseClosed = false;
};

enum class ArchiveRemovalStatus { Advanced, Wait, Invalid };

struct ArchiveRemovalResult {
	ArchiveRemovalStatus status = ArchiveRemovalStatus::Invalid;
	ArchiveRemoval state;
};

[[nodiscard]] bool CanRunArchiveWork(
	const ArchiveStoreEpoch &captured,
	const ArchiveStoreEpoch &current);
[[nodiscard]] std::optional<ArchiveRemoval> BeginArchiveRemoval(
	const ArchiveStoreEpoch &current,
	const QString &operationId);
[[nodiscard]] std::optional<QByteArray> EncodeArchiveRemovalTombstone(
	const ArchiveRemoval &removal);
[[nodiscard]] std::optional<ArchiveRemoval> ResumeArchiveRemoval(
	const QByteArray &durableTombstone);
[[nodiscard]] ArchiveRemovalResult AdvanceArchiveRemoval(
	const ArchiveRemoval &removal,
	const ArchiveRemovalAck &ack);

struct ArchiveUsage {
	uint64_t databaseBytes = 0;
	uint64_t walBytes = 0;
	uint64_t shmBytes = 0;
	uint64_t blobBytes = 0;
	uint64_t stagingBytes = 0;
};

[[nodiscard]] std::optional<uint64_t> TotalArchiveUsage(const ArchiveUsage &usage);

struct ArchiveUsageEntry {
	QString token;
	uint64_t bytes = 0;
};

struct ArchiveUsageScan {
	ArchiveStoreEpoch epoch;
	QString cursor;
	uint64_t bytes = 0;
	bool complete = false;
};

[[nodiscard]] std::optional<ArchiveUsageScan> ContinueArchiveUsage(
	const ArchiveUsageScan &scan,
	const ArchiveStoreEpoch &current,
	const std::vector<ArchiveUsageEntry> &page,
	bool complete);

struct ArchiveCaptureGap {
	AccountKey account;
	QString storeId;
	QString id;
	int64 observedAt = 0;
	QString reason;
	QJsonObject details;
};

[[nodiscard]] ArchiveSqlProgram PlanArchiveCaptureGap(
	const ArchiveCaptureGap &gap,
	const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlStatement ArchiveSqlReadCaptureGaps(int limit);

}
