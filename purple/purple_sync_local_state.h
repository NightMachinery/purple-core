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
#include <vector>

namespace Purple {

enum class SyncLocalStream {
	Config,
	Library,
};

struct SyncLocalStreamState {
	uint64_t seq = 0;
	uint64_t pendingSeq = 0;
	uint64_t confirmedSeq = 0;
	QString ownHash;
};

struct SyncLocalConfigState {
	QString base;
	std::vector<QString> baseLineage;
	std::vector<QString> equiv;
	QString pending;
	std::map<QString, uint64_t> seenSeq;
};

struct SyncLocalState {
	int version = 1;
	QString install;
	QString createdDevice;
	QString space;
	SyncLocalStreamState config;
	SyncLocalStreamState library;
	SyncLocalConfigState configData;
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

struct SyncConfirmation {
	SyncLocalState state;
	SyncCloneVerdict verdict = SyncCloneVerdict::PendingReconcile;
	bool changed = false;
};

[[nodiscard]] SyncLocalParseResult ParseSyncLocalState(
	const QByteArray &json);
[[nodiscard]] SyncLocalWriteResult SerializeSyncLocalState(
	const SyncLocalState &state);
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
