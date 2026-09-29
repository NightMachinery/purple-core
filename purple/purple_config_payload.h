/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_config_sync.h"
#include "purple/purple_sync_envelope.h"

#include <QtCore/QByteArray>

#include <cstdint>
#include <optional>
#include <vector>

namespace Purple {

enum class ConfigPayloadStatus {
	Valid,
	NewerSchema,
	Invalid,
};

enum class ConfigPayloadError {
	None,
	InvalidEnvelope,
	WrongStream,
	MissingField,
	FieldType,
	InvalidValue,
	InvalidKey,
	FingerprintMismatch,
	InvalidAncestry,
	TomlSyntax,
	SchemaMismatch,
};

struct ConfigPayloadInspection {
	ConfigPayloadStatus status = ConfigPayloadStatus::Invalid;
	ConfigPayloadError error = ConfigPayloadError::None;
	ConfigVersion version;
	QByteArray text;
	int schema = 0;
	uint64_t writerWarnings = 0;
	std::vector<QString> localWarnings;
	QString tomlError;

	[[nodiscard]] explicit operator bool() const {
		return status != ConfigPayloadStatus::Invalid;
	}
};

struct ConfigRecordBuildInput {
	QByteArray text;
	std::vector<ConfigVersion> parents;
	std::optional<ConfigVersion> version;
	QString space;
	QString install;
	QString device;
	QString platform;
	QString app;
	uint64_t seq = 0;
	uint64_t at = 0;
};

enum class ConfigRecordBuildStatus {
	Valid,
	NewerSchema,
	Invalid,
};

enum class ConfigRecordBuildError {
	None,
	InvalidUtf8,
	TomlSyntax,
	InvalidParents,
	InvalidVersion,
	Envelope,
	SelfInspection,
};

struct ConfigRecordBuildResult {
	ConfigRecordBuildStatus status = ConfigRecordBuildStatus::Invalid;
	ConfigRecordBuildError error = ConfigRecordBuildError::None;
	SyncEnvelopeError envelopeError = SyncEnvelopeError::None;
	ConfigPayloadError payloadError = ConfigPayloadError::None;
	QByteArray canonical;
	ConfigVersion version;
	QString payloadHash;
	std::vector<QString> localWarnings;
	QString tomlError;

	[[nodiscard]] explicit operator bool() const {
		return status == ConfigRecordBuildStatus::Valid;
	}
};

[[nodiscard]] ConfigPayloadInspection InspectConfigPayload(
	const SyncEnvelopeParseResult &envelope);
[[nodiscard]] ConfigRecordBuildResult BuildConfigRecord(
	const ConfigRecordBuildInput &input);

} // namespace Purple
