/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_archive_sql.h"

namespace Purple {

enum class ArchivePurgePhase { Versions, Messages, Jobs, Leases, Finish };

struct ArchiveSqlPurge {
	AccountKey account;
	QString storeId;
	QString operationId;
	ArchivePurgeKind kind = ArchivePurgeKind::Trashbin;
	std::optional<ArchiveChatKey> chat;
	std::vector<ArchiveChatKey> migrationAliases;
	uint64_t revision = 0;
	uint64_t accountGeneration = 0;
	ArchivePurgePhase phase = ArchivePurgePhase::Versions;
	QString cursor;
};

struct ArchiveOwnedMediaCopy {
	QString identity;
	QString kind;
	QString blobId;
	QString ownerVersionId;
	QByteArray ownerFrame;
	QByteArray blobFrame;
	int64 position = 0;
};

struct ArchiveSqlPurgeRow {
	ArchiveStoredRow stored;
	QByteArray frame;
	std::optional<ArchiveVersionSnapshot> head;
	QByteArray headFrame;
	std::vector<ArchiveOwnedMediaCopy> copies;
};

struct ArchiveSqlMaintenancePlan {
	ArchiveSqlProgram program;
	ArchiveSqlPurge next;
	std::vector<QString> releaseStagedJobs;
	std::vector<QString> revokeReaders;
	bool complete = false;
	size_t processedRows = 0;
};

[[nodiscard]] ArchiveSqlMaintenancePlan BeginArchiveSqlPurge(
	const ArchiveState &header,
	const ArchivePurgeRequest &request,
	const std::vector<ArchiveChatGeneration> &chatEpochs,
	const QString &operationId,
	const ArchiveSqlSecurity &security);
[[nodiscard]] std::optional<ArchiveSqlPurge> DecodeArchiveSqlPurge(const QByteArray &plaintext);
[[nodiscard]] ArchiveSqlStatement ArchiveSqlPurgePage(const ArchiveSqlPurge &purge, int limit,
	const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlMaintenancePlan ContinueArchiveSqlPurge(const ArchiveSqlPurge &purge,
	const std::vector<ArchiveSqlPurgeRow> &rows,
	const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlStatement ArchiveSqlPurgeHead(const ArchiveSqlPurge &purge,
	const ArchiveMessageKey &key, const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlStatement ArchiveSqlFindOwnedMedia(const AccountKey &account,
	const QString &identity, const QString &kind, const ArchiveSqlSecurity &security);

struct ArchiveSqlGcPlan {
	ArchiveSqlProgram program;
	ArchiveState nextHeader;
	QString cursor;
	std::vector<QString> deleteOwnedBlobs;
};

[[nodiscard]] ArchiveSqlStatement ArchiveSqlGarbagePage(int limit, const QString &after = {});
[[nodiscard]] ArchiveSqlGcPlan PlanArchiveSqlGarbage(const ArchiveState &header,
	const std::vector<ArchiveBlob> &candidates, const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlGcPlan CompleteArchiveSqlGarbage(const ArchiveState &header,
	const std::vector<ArchiveBlob> &removed, const ArchiveSqlSecurity &security);

}
