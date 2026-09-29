/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace Purple {

struct ConfigVersion;

enum class SyncLocalStream {
	Config,
	Library,
};

struct SyncIssuedRecord {
	uint64_t seq = 0;
	QString recordHash;
	QJsonObject preserved;
};

struct SyncLocalStreamState {
	uint64_t seq = 0;
	uint64_t pendingSeq = 0;
	uint64_t confirmedSeq = 0;
	QString ownHash;
	std::vector<SyncIssuedRecord> issuedRecords;
};

struct SyncLocalConfigState {
	QString base;
	std::vector<QString> baseLineage;
	std::vector<QString> equiv;
	QString pending;
	std::map<QString, uint64_t> seenSeq;
};

struct SyncOwnMessage {
	QString space;
	SyncLocalStream stream = SyncLocalStream::Config;
	int32_t messageId = 0;
	QString install;
	QString device;
	uint64_t seq = 0;
	QString payloadHash;
	QString recordHash;
	QJsonObject preserved;
};

struct SyncLocalState {
	int version = 1;
	QString install;
	QString createdDevice;
	QString space;
	QString bindingToken;
	SyncLocalStreamState config;
	SyncLocalStreamState library;
	SyncLocalConfigState configData;
	std::vector<SyncOwnMessage> ownMessages;
	QJsonObject preserved;
};

enum class SyncLocalStatus {
	Valid,
	NewerVersion,
	Invalid,
};

enum class SyncLocalError {
	None,
	InvalidJson,
	SizeLimit,
	MissingField,
	FieldType,
	InvalidId,
	InvalidValue,
	InvalidCounters,
	InvalidHash,
	InvalidConfig,
	InvalidOwnMessages,
	InvalidIssuedRecords,
	InvalidBindingToken,
};

struct SyncLocalParseResult {
	SyncLocalStatus status = SyncLocalStatus::Invalid;
	SyncLocalError error = SyncLocalError::None;
	SyncLocalState state;

	[[nodiscard]] explicit operator bool() const {
		return status == SyncLocalStatus::Valid;
	}
};

struct SyncLocalWriteResult {
	QByteArray canonical;
	SyncLocalStatus status = SyncLocalStatus::Invalid;
	SyncLocalError error = SyncLocalError::None;

	[[nodiscard]] explicit operator bool() const {
		return status == SyncLocalStatus::Valid;
	}
};

enum class SyncReservationError {
	None,
	InvalidState,
	InvalidHash,
	SequenceExhausted,
};

struct SyncReservation {
	SyncLocalState state;
	uint64_t seq = 0;
	SyncReservationError error = SyncReservationError::None;

	[[nodiscard]] explicit operator bool() const {
		return error == SyncReservationError::None;
	}
};

enum class OwnRecordObservationKind {
	Unresolved,
	Absent,
	Present,
};

struct OwnRecordObservation {
	OwnRecordObservationKind kind = OwnRecordObservationKind::Unresolved;
	uint64_t seq = 0;
	QString payloadHash;
};

enum class SyncCloneVerdict {
	PendingReconcile,
	NoClone,
	DeviceMismatch,
	RemoteAhead,
	HashMismatch,
	InvalidObservation,
};

enum class SyncAccountBindingVerdict {
	Bound,
	MissingStateToken,
	MissingAccountToken,
	InvalidAccountToken,
	Mismatch,
	InvalidState,
};

struct SyncConfirmation {
	SyncLocalState state;
	SyncCloneVerdict verdict = SyncCloneVerdict::PendingReconcile;
	bool changed = false;
};

enum class SyncOwnMessageError {
	None,
	InvalidState,
	InvalidMessageId,
	InvalidRecord,
	RecordMismatch,
	SequenceRegression,
	IdConflict,
	CapacityExceeded,
	NotFound,
	NotOlder,
	NotAbsent,
	UnissuedRecord,
};

enum class SyncIssueError {
	None,
	InvalidState,
	InvalidRecord,
	RecordMismatch,
};

struct SyncIssueResult {
	SyncLocalState state;
	SyncIssueError error = SyncIssueError::None;
	bool changed = false;

	[[nodiscard]] explicit operator bool() const {
		return error == SyncIssueError::None;
	}
};

struct SyncOwnMessageResult {
	SyncLocalState state;
	SyncOwnMessageError error = SyncOwnMessageError::None;
	bool changed = false;

	[[nodiscard]] explicit operator bool() const {
		return error == SyncOwnMessageError::None;
	}
};

struct SyncOwnDeleteCheck {
	SyncOwnMessageError error = SyncOwnMessageError::None;

	[[nodiscard]] explicit operator bool() const {
		return error == SyncOwnMessageError::None;
	}
};

enum class SyncOwnMessagePresence {
	Present,
	Absent,
};

[[nodiscard]] SyncOwnMessageResult RecordConfirmedOwnConfigMessage(
	const SyncLocalState &state,
	int32_t messageId,
	const QByteArray &canonicalRecord);
[[nodiscard]] SyncIssueResult AppendIssuedConfigRecord(
	const SyncLocalState &state,
	const QByteArray &canonicalRecord);
[[nodiscard]] SyncOwnMessageResult AdoptIssuedOwnConfigMessage(
	const SyncLocalState &state,
	int32_t messageId,
	const QByteArray &canonicalRecord);
[[nodiscard]] SyncOwnDeleteCheck CheckOwnConfigMessageDeletion(
	const SyncLocalState &state,
	int32_t messageId,
	const QByteArray &canonicalRecord);
[[nodiscard]] SyncOwnMessageResult RemoveAbsentOwnConfigMessage(
	const SyncLocalState &state,
	int32_t messageId,
	SyncOwnMessagePresence presence);

[[nodiscard]] SyncConfirmation ConfirmConfigReadBack(
	const SyncLocalState &state,
	const QString &currentDevice,
	const OwnRecordObservation &observation,
	const ConfigVersion &stagedVersion);

[[nodiscard]] SyncLocalParseResult ParseSyncLocalState(
	const QByteArray &json);
[[nodiscard]] SyncLocalWriteResult SerializeSyncLocalState(
	const SyncLocalState &state);
[[nodiscard]] std::optional<QString> FormatSyncBindingToken(
	const QByteArray &entropy);
[[nodiscard]] SyncAccountBindingVerdict CheckSyncAccountBinding(
	const SyncLocalState &state,
	const QByteArray &accountPrefToken);
[[nodiscard]] SyncReservation ReserveSyncSeq(
	const SyncLocalState &state,
	SyncLocalStream stream,
	const QString &payloadHash);
[[nodiscard]] SyncCloneVerdict CheckSyncClone(
	const SyncLocalState &state,
	const QString &currentDevice,
	SyncLocalStream stream,
	const OwnRecordObservation &observation);
[[nodiscard]] SyncConfirmation ConfirmSyncReadBack(
	const SyncLocalState &state,
	const QString &currentDevice,
	SyncLocalStream stream,
	const OwnRecordObservation &observation);

} // namespace Purple
