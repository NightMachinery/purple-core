/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_local_choice.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

#include <cstdint>
#include <optional>
#include <vector>

namespace Purple {

inline constexpr auto kArchiveSchemaVersion = 1;

enum class ArchiveChatKind { User = 0, Group = 1, Channel = 2, Secret = 3 };
enum class ArchiveCollection { Received = 0, Scheduled = 1, QuickReply = 2 };
enum class ArchiveOrigin {
	Baseline = 0,
	AuthoritativeEdit = 1,
	AuthoritativeDelete = 2,
	LocalDelete = 3,
	MediaExpiry = 4,
	MediaConsumption = 5,
	Hydration = 6,
	SendMetadata = 7,
};
enum class ArchiveCompleteness { Partial = 0, Complete = 1, Opaque = 2 };
enum class ArchiveSourceKind { ComposeMarkdown = 0, ReceivedMarkdownFile = 1 };
enum class ArchiveMediaState { Missing = 0, Staged = 1, Complete = 2, Failed = 3 };
enum class ArchiveJobState { Staged = 0, Complete = 1, Failed = 2, Cancelled = 3 };
enum class ArchiveBlobState { Complete = 0, GarbagePending = 1 };

struct ArchiveChatKey {
	ArchiveChatKind kind = ArchiveChatKind::User;
	int64 id = 0;
	QJsonObject preserved;
	friend bool operator==(const ArchiveChatKey &, const ArchiveChatKey &) = default;
};

struct ArchiveMessageKey {
	AccountKey account;
	ArchiveChatKey chat;
	int64 messageId = 0;
	ArchiveCollection collection = ArchiveCollection::Received;
	int64 collectionId = 0;
	QString representation = u"original"_q;
	QJsonObject preserved;
	friend bool operator==(const ArchiveMessageKey &, const ArchiveMessageKey &) = default;
};

struct ArchiveGeneration {
	uint64_t account = 0;
	uint64_t chat = 0;
	friend bool operator==(const ArchiveGeneration &, const ArchiveGeneration &) = default;
};

struct ArchiveMediaDescriptor {
	QString identity;
	QString kind;
	QJsonObject attributes;
	QJsonObject preserved;
	friend bool operator==(const ArchiveMediaDescriptor &, const ArchiveMediaDescriptor &) = default;
};

struct ArchiveContent {
	QString text;
	QString caption;
	QJsonArray entities;
	QJsonValue rich;
	std::vector<ArchiveMediaDescriptor> media;
	QString opaqueFormat;
	QByteArray opaque;
	QJsonObject preserved;
	friend bool operator==(const ArchiveContent &, const ArchiveContent &) = default;
};

struct ArchiveSourceBinding {
	ArchiveMessageKey key;
	QString version;
	std::optional<int64> localMessageId;
	std::optional<int64> randomId;
	QJsonObject preserved;
	friend bool operator==(const ArchiveSourceBinding &, const ArchiveSourceBinding &) = default;
};

struct ArchiveSource {
	ArchiveSourceKind kind = ArchiveSourceKind::ComposeMarkdown;
	QString contentHash;
	QString representation;
	QByteArray bytes;
	QString revision;
	QJsonObject preserved;
	std::optional<ArchiveSourceBinding> binding;
	friend bool operator==(const ArchiveSource &, const ArchiveSource &) = default;
};

struct ArchiveObservation {
	int64 observedAt = 0;
	int64 serverTime = 0;
	int64 topicId = 0;
	QString representation = u"original"_q;
	QJsonObject context;
	QJsonObject transport;
	QString payloadFormat;
	QByteArray payload;
	QJsonObject preserved;
	friend bool operator==(const ArchiveObservation &, const ArchiveObservation &) = default;
};

struct ArchiveMediaReference {
	QString identity;
	ArchiveMediaState state = ArchiveMediaState::Missing;
	QString blobId;
	QJsonObject preserved;
	friend bool operator==(const ArchiveMediaReference &, const ArchiveMediaReference &) = default;
};

struct ArchiveMissingSlot {
	std::vector<QString> path;
	QJsonObject preserved;
	friend bool operator==(const ArchiveMissingSlot &, const ArchiveMissingSlot &) = default;
};

struct ArchiveMissingSlots {
	QString revision;
	std::vector<ArchiveMissingSlot> content;
	std::vector<QString> media;
	QJsonObject preserved;
	friend bool operator==(const ArchiveMissingSlots &, const ArchiveMissingSlots &) = default;
};

struct ArchiveVersion {
	QString id;
	QString revision;
	uint64_t order = 0;
	ArchiveOrigin origin = ArchiveOrigin::Baseline;
	int64 firstObservedAt = 0;
	ArchiveContent content;
	ArchiveMissingSlots missing;
	ArchiveCompleteness completeness = ArchiveCompleteness::Partial;
	ArchiveObservation observation;
	std::optional<ArchiveSource> source;
	std::vector<ArchiveMediaReference> media;
	bool missingPreimage = false;
	QJsonObject preserved;
	friend bool operator==(const ArchiveVersion &, const ArchiveVersion &) = default;
};

struct ArchiveEventReceipt {
	QString id;
	QString fingerprint;
	QJsonObject preserved;
	friend bool operator==(const ArchiveEventReceipt &, const ArchiveEventReceipt &) = default;
};

struct ArchiveMessage {
	ArchiveMessageKey key;
	std::vector<ArchiveChatKey> migrationAliases;
	std::vector<ArchiveVersion> versions;
	std::vector<ArchiveEventReceipt> receipts;
	uint64_t order = 0;
	bool deleted = false;
	bool payloadPurged = false;
	std::optional<ArchiveOrigin> deletionOrigin;
	int64 deletedObservedAt = 0;
	QJsonObject preserved;
	friend bool operator==(const ArchiveMessage &, const ArchiveMessage &) = default;
};

struct ArchiveEvent {
	ArchiveMessageKey key;
	QString id;
	uint64_t order = 0;
	ArchiveOrigin origin = ArchiveOrigin::Baseline;
	ArchiveGeneration generation;
	std::optional<ArchiveContent> content;
	ArchiveMissingSlots missing;
	ArchiveCompleteness completeness = ArchiveCompleteness::Partial;
	ArchiveObservation observation;
	std::optional<ArchiveSource> source;
	QString targetVersion;
	QString targetRevision;
	QString expectedContentHash;
	std::vector<ArchiveChatKey> migrationAliases;
};

struct ArchiveChatGeneration {
	ArchiveChatKey chat;
	uint64_t generation = 0;
	QJsonObject preserved;
	friend bool operator==(const ArchiveChatGeneration &, const ArchiveChatGeneration &) = default;
};

struct ArchiveBlob {
	QString id;
	uint64_t size = 0;
	ArchiveBlobState state = ArchiveBlobState::Complete;
	QJsonObject preserved;
	friend bool operator==(const ArchiveBlob &, const ArchiveBlob &) = default;
};

struct ArchiveMediaJob {
	QString id;
	ArchiveMessageKey key;
	QString version;
	QString mediaIdentity;
	ArchiveGeneration generation;
	std::optional<uint64_t> expectedSize;
	QString expectedHash;
	ArchiveJobState state = ArchiveJobState::Staged;
	QString blobId;
	uint64_t completedSize = 0;
	QString failure;
	QJsonObject preserved;
	friend bool operator==(const ArchiveMediaJob &, const ArchiveMediaJob &) = default;
};

struct ArchiveReaderLease {
	QString id;
	ArchiveMessageKey key;
	QString version;
	QString blobId;
	ArchiveGeneration generation;
	bool revoked = false;
	QJsonObject preserved;
	friend bool operator==(const ArchiveReaderLease &, const ArchiveReaderLease &) = default;
};

struct ArchiveState {
	int version = kArchiveSchemaVersion;
	AccountKey account;
	uint64_t revision = 0;
	uint64_t accountGeneration = 0;
	std::vector<ArchiveChatGeneration> chatGenerations;
	std::vector<ArchiveMessage> messages;
	std::vector<ArchiveBlob> blobs;
	std::vector<ArchiveMediaJob> jobs;
	std::vector<ArchiveReaderLease> leases;
	QJsonObject preserved;
	friend bool operator==(const ArchiveState &, const ArchiveState &) = default;
};

[[nodiscard]] bool IsArchiveChatKey(const ArchiveChatKey &key);
[[nodiscard]] bool IsArchiveMessageKey(const ArchiveMessageKey &key);
[[nodiscard]] bool SameArchiveChat(const ArchiveChatKey &a, const ArchiveChatKey &b);
[[nodiscard]] bool SameArchiveMessage(const ArchiveMessageKey &a, const ArchiveMessageKey &b);
[[nodiscard]] bool IsArchiveToken(const QString &token);
[[nodiscard]] bool IsArchiveHash(const QString &hash);
[[nodiscard]] QByteArray CanonicalArchiveContent(const ArchiveContent &content);
[[nodiscard]] QString ArchiveContentHash(const ArchiveContent &content);
[[nodiscard]] bool SameArchiveContent(const ArchiveContent &a, const ArchiveContent &b);
[[nodiscard]] bool IsArchiveState(const ArchiveState &state);
[[nodiscard]] ArchiveGeneration ArchiveGenerationFor(
	const ArchiveState &state,
	const ArchiveChatKey &chat);
[[nodiscard]] const ArchiveMessage *FindArchiveMessage(
	const ArchiveState &state,
	const ArchiveMessageKey &key);
[[nodiscard]] const ArchiveVersion *FindArchiveVersion(
	const ArchiveState &state,
	const ArchiveMessageKey &key,
	const QString &version);
[[nodiscard]] bool ArchiveSourceMatches(const ArchiveVersion &version);

}
