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

#include <optional>

namespace Purple {

enum class SyncEnvelopeStatus {
	Valid,
	NewerMajor,
	UnsupportedEncoding,
	Invalid,
};

enum class SyncEnvelopeError {
	None,
	InvalidJson,
	SizeLimit,
	MissingField,
	FieldType,
	InvalidId,
	InvalidValue,
	InvalidHash,
	HashMismatch,
};

struct SyncEnvelope {
	QJsonObject document;
};

struct SyncEnvelopeParseResult {
	SyncEnvelopeStatus status = SyncEnvelopeStatus::Invalid;
	SyncEnvelopeError error = SyncEnvelopeError::None;
	SyncEnvelope envelope;

	[[nodiscard]] explicit operator bool() const {
		return status == SyncEnvelopeStatus::Valid;
	}
};

struct SyncEnvelopeWriteResult {
	QByteArray canonical;
	SyncEnvelopeStatus status = SyncEnvelopeStatus::Invalid;
	SyncEnvelopeError error = SyncEnvelopeError::None;

	[[nodiscard]] explicit operator bool() const {
		return status == SyncEnvelopeStatus::Valid;
	}
};

[[nodiscard]] bool IsSyncSpaceId(const QString &value);
[[nodiscard]] bool IsSyncInstallId(const QString &value);
[[nodiscard]] std::optional<QString> FormatSyncSpaceId(
	const QByteArray &entropy);
[[nodiscard]] std::optional<QString> FormatSyncInstallId(
	const QByteArray &entropy);
[[nodiscard]] SyncEnvelopeParseResult ParseSyncEnvelope(
	const QByteArray &json);
[[nodiscard]] SyncEnvelopeWriteResult SerializeSyncEnvelope(
	const SyncEnvelope &envelope);

} // namespace Purple
