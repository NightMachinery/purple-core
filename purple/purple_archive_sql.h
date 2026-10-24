/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_archive_rows.h"
#include "purple/purple_archive_store.h"

namespace Purple {

inline constexpr auto kArchiveSqlSchemaVersion = 1;
inline constexpr auto kArchiveSqlPageLimit = 128;
inline constexpr auto kArchiveSqlSliceRowLimit = 256;
inline constexpr auto kArchiveSqlStatementLimit = 2048;
inline constexpr auto kArchiveSqlBindByteLimit = 16 * 1024 * 1024;
inline constexpr auto kArchiveSqlPurgeMediaLimit = 128;
inline constexpr auto kArchiveSqlPurgeEnvelopeLimit = 64 * 1024;
inline constexpr auto kArchiveSqlPurgeBlobEnvelopeLimit = 4 * 1024;
inline constexpr auto kArchiveSqlInlineBodyLimit = 16 * 1024;
inline constexpr auto kArchiveSqlBodyByteLimit = kArchiveCodecByteLimit;
using ArchiveSqlValue = std::variant<std::nullptr_t, int64, QString, QByteArray>;

enum class ArchiveSqlExpectation { Done, OneRow, OneChange, Rows, AnyChanges, AtMostOneChange };
enum class ArchiveSqlChangeCounter { Direct, Total };

struct ArchiveSqlCountRange {
	int64 minimum = 0;
	int64 maximum = 0;
};

struct ArchiveSqlStatement {
	QString sql;
	std::vector<ArchiveSqlValue> binds;
	ArchiveSqlExpectation expectation = ArchiveSqlExpectation::Done;
	ArchiveSqlCountRange rows;
	ArchiveSqlCountRange changes;
	ArchiveSqlChangeCounter counter = ArchiveSqlChangeCounter::Total;

	ArchiveSqlStatement() = default;
	ArchiveSqlStatement(QString query, std::vector<ArchiveSqlValue> values = {},
		ArchiveSqlExpectation expected = ArchiveSqlExpectation::Done);
};

[[nodiscard]] bool CheckArchiveSqlResult(const ArchiveSqlStatement &statement,
	bool reachedDone, int64 rows, int64 directChanges, int64 totalChangesDelta);
[[nodiscard]] bool AcknowledgeArchiveSqlCommit(bool allResultsChecked, bool commitDurable);

enum class ArchiveSqlStatus { Ready, Invalid, ReadOnly, EncodingFailed, TooLarge, RetentionGap };

struct ArchiveSqlProgram {
	ArchiveSqlStatus status = ArchiveSqlStatus::Invalid;
	std::vector<ArchiveSqlStatement> statements;
	QString retentionGapReason;
	[[nodiscard]] explicit operator bool() const {
		return status == ArchiveSqlStatus::Ready;
	}
};

struct ArchiveSqlSecurity {
	QString storeId;
	QString keyId;
	ArchiveCrypto crypto;
	std::function<std::optional<QByteArray>(const QByteArray &)> hmacSha256;
};

enum class ArchiveIndexKind { Account, Chat, Message, Version, Event, Blob, Job, Lease, Media, Maintenance };

struct ArchiveTranscriptCursor {
	int64 date = 0;
	int64 messageId = 0;
	QString messageKey;
};

struct ArchiveStoredRow {
	QString id;
	int64 position = 0;
	ArchiveRow row;
};

struct ArchiveSqlVersionMetadata {
	ArchiveMessageKey key;
	QString versionId;
	QJsonObject preserved;
	friend bool operator==(const ArchiveSqlVersionMetadata &, const ArchiveSqlVersionMetadata &) = default;
};

struct ArchiveSqlSlice {
	ArchiveState state;
	std::vector<ArchiveSqlVersionMetadata> versionMetadata;
};

[[nodiscard]] QString ArchiveSqlToken(const AccountKey &account,
	ArchiveIndexKind kind, const QByteArray &identity, const ArchiveSqlSecurity &security);
[[nodiscard]] QString ArchiveSqlMessageId(const ArchiveMessageKey &key, const ArchiveSqlSecurity &security);
[[nodiscard]] QString ArchiveSqlChatId(const ArchiveChatKey &key,
	const AccountKey &account, const ArchiveSqlSecurity &security);
[[nodiscard]] QString ArchiveSqlChildId(const ArchiveMessageKey &key,
	ArchiveIndexKind kind, const QString &id, const ArchiveSqlSecurity &security);
[[nodiscard]] QString ArchiveSqlOrder(uint64_t order);
[[nodiscard]] ArchiveVersionSnapshot ArchiveSqlVersionSnapshot(const ArchiveMessageKey &key,
	const ArchiveVersion &version,
	const std::vector<ArchiveSqlVersionMetadata> &metadata = {});
[[nodiscard]] std::optional<QByteArray> SealArchiveSqlRow(const AccountKey &account,
	const QString &id, ArchiveRowValue value, const ArchiveSqlSecurity &security);

[[nodiscard]] std::optional<ArchiveRow> PrepareArchiveSqlVersion(const ArchiveVersionSnapshot &snapshot);
[[nodiscard]] std::optional<QByteArray> SealArchiveSqlStoredRow(const AccountKey &account,
	const QString &id, const ArchiveRow &row, const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlProgram StoreArchiveSqlVersionBody(const ArchiveRow &row,
	const ArchiveVersionSnapshot &snapshot, const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlStatement ArchiveSqlReadBodyChunks(const ArchiveRow &row,
	const ArchiveSqlSecurity &security);
[[nodiscard]] std::optional<ArchiveRow> RestoreArchiveSqlVersionBody(const ArchiveRow &row,
	const std::vector<QByteArray> &chunks, const ArchiveSqlSecurity &security);
struct ArchiveSqlCapturePlan {
	ArchiveTransaction transaction;
	ArchiveSqlProgram program;
};
[[nodiscard]] ArchiveSqlCapturePlan PlanArchiveSqlCapture(const ArchiveState &slice,
	const ArchiveEvent &event, const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlCapturePlan PlanArchiveSqlCapture(const ArchiveSqlSlice &slice,
	const ArchiveEvent &event, const ArchiveSqlSecurity &security);

[[nodiscard]] std::vector<QString> ArchiveSqlConnectionSetup();
[[nodiscard]] std::vector<QString> ArchiveSqlSchema();
[[nodiscard]] ArchiveSqlStatus CheckArchiveSqlVersion(int64 version);
[[nodiscard]] ArchiveSqlProgram InitializeArchiveSql(const ArchiveState &empty,
	const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlProgram TranslateArchiveSql(const ArchiveTransaction &boundedSlice,
	const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlProgram TranslateArchiveSql(const ArchiveTransaction &boundedSlice,
	const ArchiveSqlSlice &restored, const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlStatement ArchiveSqlReadMessage(const ArchiveMessageKey &key,
	const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlStatement ArchiveSqlReadReceipt(const ArchiveMessageKey &key,
	const QString &eventId, const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlStatement ArchiveSqlReadRecord(const AccountKey &account,
	ArchiveRecordKind kind, const QString &id, const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlStatement ArchiveSqlReadChatEpoch(const AccountKey &account,
	const ArchiveChatKey &chat, const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlStatement ArchiveSqlVersionJobs(const ArchiveMessageKey &key,
	const QString &version, int limit, const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlStatement ArchiveSqlReadVersion(const ArchiveMessageKey &key,
	const QString &version, const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlStatement ArchiveSqlReadHead(const ArchiveMessageKey &key,
	const ArchiveSqlSecurity &security);
[[nodiscard]] ArchiveSqlStatement ArchiveSqlReadVersions(const ArchiveMessageKey &key,
	int limit, const ArchiveSqlSecurity &security, const QString &beforeOrder = {});
[[nodiscard]] ArchiveSqlStatement ArchiveSqlDeletedTranscript(const AccountKey &account,
	const ArchiveChatKey &chat, int limit, const ArchiveSqlSecurity &security,
	const std::optional<ArchiveTranscriptCursor> &before = {});
[[nodiscard]] ArchiveSqlStatement ArchiveSqlReadPage(ArchiveRecordKind kind,
	int limit, const QString &after = {});
[[nodiscard]] std::optional<ArchiveSqlSlice> RestoreArchiveSqlSlice(
	std::vector<ArchiveStoredRow> rows, const ArchiveSqlSecurity &security);
[[nodiscard]] bool IsBoundedArchiveSqlProgram(const ArchiveSqlProgram &program);

}
