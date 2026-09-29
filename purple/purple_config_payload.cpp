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
	for (const auto field : {
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

} // namespace Purple
