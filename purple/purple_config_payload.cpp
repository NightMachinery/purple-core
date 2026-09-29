/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_config_payload.h"

#include "purple/purple_settings.h"
#include "purple/purple_state.h"

#include <QtCore/QJsonArray>
#include <QtCore/QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Purple {
namespace {

constexpr auto kMaximumSafeInteger = 9007199254740991.0;
constexpr auto kMaximumSafeIntegerUnsigned = uint64_t(9007199254740991ULL);

[[nodiscard]] bool IntegerInRange(
		const QJsonValue &value,
		double minimum,
		double maximum) {
	if (!value.isDouble()) {
		return false;
	}
	const auto number = value.toDouble();
	return std::isfinite(number)
		&& number >= minimum
		&& number <= maximum
		&& std::floor(number) == number;
}

[[nodiscard]] bool ValidExistingVersion(
		const ConfigVersion &version,
		const QByteArray &text) {
	const auto key = ParseConfigVersionKey(version.key);
	if (!key || key->fingerprint != SettingsFingerprint(text)
		|| version.parents.size() > 2
		|| version.lineage.size() > 64) {
		return false;
	}
	auto parents = QSet<QString>();
	auto maximum = uint64_t(0);
	for (const auto &parent : version.parents) {
		const auto parsed = ParseConfigVersionKey(parent);
		if (!parsed || parents.contains(parent)) {
			return false;
		}
		parents.insert(parent);
		maximum = std::max(maximum, parsed->generation);
	}
	auto lineage = QSet<QString>();
	for (const auto &ancestor : version.lineage) {
		const auto parsed = ParseConfigVersionKey(ancestor);
		if (!parsed || parsed->generation >= key->generation
			|| lineage.contains(ancestor)) {
			return false;
		}
		lineage.insert(ancestor);
	}
	if (parents.empty()) {
		return key->generation == 1 && lineage.empty();
	}
	if (maximum == std::numeric_limits<uint64_t>::max()
		|| key->generation != maximum + 1) {
		return false;
	}
	return std::all_of(parents.begin(), parents.end(), [&](const auto &parent) {
		return lineage.contains(parent);
	});
}

} // namespace

ConfigPayloadInspection InspectConfigPayload(
		const SyncEnvelopeParseResult &envelope) {
	auto result = ConfigPayloadInspection();
	if (envelope.status != SyncEnvelopeStatus::Valid) {
		result.error = ConfigPayloadError::InvalidEnvelope;
		return result;
	}
	const auto document = envelope.envelope.document;
	if (document.value(u"stream"_q).toString() != u"config"_q) {
		result.error = ConfigPayloadError::WrongStream;
		return result;
	}
	const auto payload = document.value(u"payload"_q).toObject();
	for (const auto &field : {
		u"schema"_q,
		u"key"_q,
		u"parents"_q,
		u"lineage"_q,
		u"warnings"_q,
		u"text"_q,
	}) {
		if (!payload.contains(field)) {
			result.error = ConfigPayloadError::MissingField;
			return result;
		}
	}
	const auto schema = payload.value(u"schema"_q);
	const auto warnings = payload.value(u"warnings"_q);
	const auto key = payload.value(u"key"_q);
	const auto parents = payload.value(u"parents"_q);
	const auto lineage = payload.value(u"lineage"_q);
	const auto text = payload.value(u"text"_q);
	if (!schema.isDouble()
		|| !warnings.isDouble()
		|| !key.isString()
		|| !parents.isArray()
		|| !lineage.isArray()
		|| !text.isString()) {
		result.error = ConfigPayloadError::FieldType;
		return result;
	}
	if (!IntegerInRange(
			schema,
			1,
			std::numeric_limits<int>::max())
		|| !IntegerInRange(warnings, 0, kMaximumSafeInteger)) {
		result.error = ConfigPayloadError::InvalidValue;
		return result;
	}
	result.schema = int(schema.toDouble());
	result.writerWarnings = uint64_t(warnings.toDouble());
	result.version.key = key.toString();
	const auto keyParts = ParseConfigVersionKey(result.version.key);
	if (!keyParts) {
		result.error = ConfigPayloadError::InvalidKey;
		return result;
	}
	result.text = text.toString().toUtf8();
	if (keyParts->fingerprint != SettingsFingerprint(result.text)) {
		result.error = ConfigPayloadError::FingerprintMismatch;
		return result;
	}
	const auto parentArray = parents.toArray();
	const auto lineageArray = lineage.toArray();
	if (parentArray.size() > 2 || lineageArray.size() > 64) {
		result.error = ConfigPayloadError::InvalidAncestry;
		return result;
	}
	auto parentSet = QSet<QString>();
	for (const auto &entry : parentArray) {
		if (!entry.isString()) {
			result.error = ConfigPayloadError::FieldType;
			return result;
		}
		const auto parent = entry.toString();
		if (!ParseConfigVersionKey(parent) || parentSet.contains(parent)) {
			result.error = ConfigPayloadError::InvalidAncestry;
			return result;
		}
		result.version.parents.push_back(parent);
		parentSet.insert(parent);
	}
	auto lineageSet = QSet<QString>();
	for (const auto &entry : lineageArray) {
		if (!entry.isString()) {
			result.error = ConfigPayloadError::FieldType;
			return result;
		}
		const auto ancestor = entry.toString();
		const auto parts = ParseConfigVersionKey(ancestor);
		if (!parts
			|| parts->generation >= keyParts->generation
			|| lineageSet.contains(ancestor)) {
			result.error = ConfigPayloadError::InvalidAncestry;
			return result;
		}
		result.version.lineage.push_back(ancestor);
		lineageSet.insert(ancestor);
	}
	if (result.version.parents.empty()) {
		if (keyParts->generation != 1 || !lineageSet.empty()) {
			result.error = ConfigPayloadError::InvalidAncestry;
			return result;
		}
	} else {
		auto maximum = uint64_t(0);
		for (const auto &parent : result.version.parents) {
			const auto parts = ParseConfigVersionKey(parent);
			if (!lineageSet.contains(parent)) {
				result.error = ConfigPayloadError::InvalidAncestry;
				return result;
			}
			maximum = std::max(maximum, parts->generation);
		}
		if (maximum == std::numeric_limits<uint64_t>::max()
			|| keyParts->generation != maximum + 1) {
			result.error = ConfigPayloadError::InvalidAncestry;
			return result;
		}
	}
	const auto parsed = ParseSettings(text.toString(), u"settings.toml"_q);
	result.localWarnings = parsed.warnings;
	if (!parsed.ok()) {
		result.error = ConfigPayloadError::TomlSyntax;
		result.tomlError = parsed.error;
		return result;
	}
	if (result.schema != parsed.settings.version) {
		result.error = ConfigPayloadError::SchemaMismatch;
		return result;
	}
	result.status = (result.schema > kSettingsVersion)
		? ConfigPayloadStatus::NewerSchema
		: ConfigPayloadStatus::Valid;
	return result;
}

ConfigRecordBuildResult BuildConfigRecord(
		const ConfigRecordBuildInput &input) {
	auto result = ConfigRecordBuildResult();
	const auto text = QString::fromUtf8(input.text);
	if (text.toUtf8() != input.text) {
		result.error = ConfigRecordBuildError::InvalidUtf8;
		return result;
	}
	const auto parsedSettings = ParseSettings(text, u"settings.toml"_q);
	result.localWarnings = parsedSettings.warnings;
	if (!parsedSettings.ok()) {
		result.error = ConfigRecordBuildError::TomlSyntax;
		result.tomlError = parsedSettings.error;
		return result;
	}
	if (parsedSettings.settings.version > kSettingsVersion) {
		result.status = ConfigRecordBuildStatus::NewerSchema;
		return result;
	}
	if (input.version && !input.parents.empty()) {
		result.error = ConfigRecordBuildError::InvalidParents;
		return result;
	}
	const auto version = input.version
		? input.version
		: MakeConfigVersion(input.text, input.parents);
	if (!version) {
		result.error = ConfigRecordBuildError::InvalidParents;
		return result;
	}
	if (input.version && !ValidExistingVersion(*version, input.text)) {
		result.error = ConfigRecordBuildError::InvalidVersion;
		return result;
	}
	result.version = *version;
	if (input.seq > kMaximumSafeIntegerUnsigned
		|| input.at > kMaximumSafeIntegerUnsigned) {
		result.error = ConfigRecordBuildError::Envelope;
		result.envelopeError = SyncEnvelopeError::FieldType;
		return result;
	}
	auto parents = QJsonArray();
	auto lineage = QJsonArray();
	for (const auto &key : version->parents) {
		parents.append(key);
	}
	for (const auto &key : version->lineage) {
		lineage.append(key);
	}
	const auto document = QJsonObject{
		{ u"purple_sync"_q, 1 },
		{ u"stream"_q, u"config"_q },
		{ u"space"_q, input.space },
		{ u"writer"_q, QJsonObject{
			{ u"install"_q, input.install },
			{ u"device"_q, input.device },
			{ u"platform"_q, input.platform },
			{ u"app"_q, input.app },
		} },
		{ u"seq"_q, QJsonValue(qint64(input.seq)) },
		{ u"at"_q, QJsonValue(qint64(input.at)) },
		{ u"payload"_q, QJsonObject{
			{ u"schema"_q, parsedSettings.settings.version },
			{ u"key"_q, version->key },
			{ u"parents"_q, parents },
			{ u"lineage"_q, lineage },
			{ u"warnings"_q, QJsonValue(qint64(result.localWarnings.size())) },
			{ u"text"_q, text },
		} },
	};
	const auto written = SerializeSyncEnvelope(SyncEnvelope{ document });
	if (!written) {
		result.error = ConfigRecordBuildError::Envelope;
		result.envelopeError = written.error;
		return result;
	}
	const auto parsed = ParseSyncEnvelope(written.canonical);
	if (!parsed) {
		result.error = ConfigRecordBuildError::SelfInspection;
		result.envelopeError = parsed.error;
		return result;
	}
	const auto inspected = InspectConfigPayload(parsed);
	if (inspected.status != ConfigPayloadStatus::Valid
		|| inspected.version.key != version->key
		|| inspected.version.parents != version->parents
		|| inspected.version.lineage != version->lineage
		|| inspected.text != input.text
		|| inspected.writerWarnings != result.localWarnings.size()) {
		result.error = ConfigRecordBuildError::SelfInspection;
		result.payloadError = inspected.error;
		return result;
	}
	result.status = ConfigRecordBuildStatus::Valid;
	result.canonical = written.canonical;
	result.payloadHash = parsed.envelope.document
		.value(u"payload_sha256"_q).toString();
	return result;
}

} // namespace Purple
