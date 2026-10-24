/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_archive_lifecycle.h"

namespace Purple {

struct ArchiveTopicScope {
	ArchiveChatKey chat;
	int64 id = 0;
};

struct ArchiveViewScope {
	ArchivePurgeScope messages;
	std::optional<ArchiveTopicScope> topic;
	std::optional<ArchiveCollection> collection;
	std::optional<QString> representation;
};

enum class ArchiveTranscriptOrder { Chronological, ReverseChronological };
enum class ArchiveDeletedStatus { NotDeleted, Deleted, Expired, Consumed };

struct ArchivePurgeConfirmation {
	ArchiveStoreEpoch epoch;
	ArchivePurgeRequest request;
	uint64_t accountGeneration = 0;
	QString token;
};

[[nodiscard]] bool ArchiveDeletedViewMatches(const ArchiveViewScope &scope,
	const ArchiveMessage &message, const ArchiveVersionSnapshot *head);
[[nodiscard]] ArchiveDeletedStatus ArchiveMessageDeletedStatus(const ArchiveMessage &message);
[[nodiscard]] std::optional<ArchivePurgeConfirmation> MakeArchivePurgeConfirmation(
	const ArchiveState &state, const ArchiveStoreEpoch &epoch,
	ArchivePurgeKind kind, const ArchivePurgeScope &scope);
[[nodiscard]] std::optional<ArchivePurgeRequest> ConfirmArchivePurge(
	const ArchiveState &current, const ArchiveStoreEpoch &epoch,
	ArchivePurgeKind currentKind, const ArchivePurgeScope &currentScope,
	const ArchivePurgeConfirmation &confirmation);

[[nodiscard]] ArchiveSqlStatement ArchiveSqlDeletedView(const ArchiveViewScope &scope,
	int limit, const ArchiveSqlSecurity &security,
	const std::optional<ArchiveTranscriptCursor> &cursor = {},
	ArchiveTranscriptOrder order = ArchiveTranscriptOrder::Chronological);
[[nodiscard]] ArchiveSqlStatement ArchiveSqlScopeCounts(const ArchiveViewScope &scope,
	const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlStatement ArchiveSqlScopeBlobs(const ArchiveViewScope &scope,
	int limit, const ArchiveSqlSecurity &security, const QString &after = {});

}
