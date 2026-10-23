/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_archive_model.h"

namespace Purple {

enum class ArchivePlanStatus {
	Ready,
	NoChange,
	Invalid,
	ReadOnly,
	Stale,
	Conflict,
	Purged,
	NotFound,
	Exhausted,
};
enum class ArchiveStoreOperation {
	CompareRevision,
	PutMessage,
	PutBlob,
	EraseBlob,
	PutJob,
	PutLease,
	EraseLease,
	PutGenerations,
	PutRevision,
};

struct ArchiveStoreWrite {
	ArchiveStoreOperation operation = ArchiveStoreOperation::CompareRevision;
	std::optional<ArchiveMessageKey> message;
	QString id;
};

struct ArchiveTransaction {
	ArchivePlanStatus status = ArchivePlanStatus::Invalid;
	ArchiveState expected;
	ArchiveState next;
	std::vector<ArchiveStoreWrite> writes;
	std::vector<QString> releaseStagedJobs;
	std::vector<QString> revokeReaders;
	std::vector<QString> deleteOwnedBlobs;

	[[nodiscard]] explicit operator bool() const {
		return status == ArchivePlanStatus::Ready;
	}
};

enum class ArchiveCommitStatus { Applied, AlreadyApplied, NotCommitted, Stale, Invalid };

struct ArchiveCompletion {
	ArchiveCommitStatus status = ArchiveCommitStatus::Invalid;
	ArchiveState state;
};

enum class ArchivePurgeKind { Trashbin, EditHistory };

struct ArchivePurgeScope {
	AccountKey account;
	std::optional<ArchiveChatKey> chat;
	std::vector<ArchiveChatKey> migrationAliases;
};

struct ArchivePurgeRequest {
	ArchivePurgeKind kind = ArchivePurgeKind::Trashbin;
	ArchivePurgeScope scope;
	uint64_t expectedRevision = 0;
};

struct ArchiveMediaResult {
	bool success = false;
	QString blobId;
	QString verifiedHash;
	uint64_t size = 0;
	QString failure;
};

struct ArchiveBlobReferences {
	uint64_t versions = 0;
	uint64_t readers = 0;
};

[[nodiscard]] ArchiveTransaction PlanArchiveEvent(
	const ArchiveState &state,
	const ArchiveEvent &event);
[[nodiscard]] ArchiveTransaction PlanArchivePurge(
	const ArchiveState &state,
	const ArchivePurgeRequest &request);
[[nodiscard]] ArchiveTransaction PlanArchiveMediaStage(
	const ArchiveState &state,
	const ArchiveMediaJob &job);
[[nodiscard]] ArchiveTransaction PlanArchiveMediaCompletion(
	const ArchiveState &state,
	const QString &jobId,
	const ArchiveMediaResult &result);
[[nodiscard]] ArchiveTransaction PlanArchiveReaderAcquire(
	const ArchiveState &state,
	const ArchiveReaderLease &lease);
[[nodiscard]] ArchiveTransaction PlanArchiveReaderRelease(
	const ArchiveState &state,
	const QString &leaseId);
[[nodiscard]] ArchiveBlobReferences CountArchiveBlobReferences(
	const ArchiveState &state,
	const QString &blobId);
[[nodiscard]] ArchiveTransaction PlanArchiveGarbageCollection(
	const ArchiveState &state);
[[nodiscard]] ArchiveTransaction CompleteArchiveGarbageCollection(
	const ArchiveState &state,
	const std::vector<QString> &removedBlobIds);
[[nodiscard]] ArchiveCompletion CompleteArchiveTransaction(
	const ArchiveState &current,
	const ArchiveTransaction &transaction,
	bool committed);

}
