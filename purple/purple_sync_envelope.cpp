/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_envelope.h"

#include "purple/purple_sync_json.h"
#include "purple/purple_types.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonValue>

#include <cmath>
#include <cstdint>

namespace Purple {
namespace {

constexpr auto kMaximumSafeInteger = 9007199254740991.0;
constexpr auto kConfigBytes = 256 * 1024;
constexpr auto kLibraryBytes = 4 * 1024 * 1024;
constexpr auto kMetadataBytes = 256;
constexpr auto kMaximumDepth = 128;

struct Validation {
	SyncEnvelopeStatus status = SyncEnvelopeStatus::Valid;
	SyncEnvelopeError error = SyncEnvelopeError::None;
};

[[nodiscard]] Validation Invalid(SyncEnvelopeError error) {
	return { SyncEnvelopeStatus::Invalid, error };
}

[[nodiscard]] bool SafeInteger(const QJsonValue &value, double minimum) {
	if (!value.isDouble()) {
		return false;
	}
	const auto number = value.toDouble();
	return std::isfinite(number)
		&& number >= minimum
		&& number <= kMaximumSafeInteger
		&& std::floor(number) == number;
}

[[nodiscard]] bool ValidUnicode(const QString &value) {
	for (auto index = qsizetype(0); index != value.size(); ++index) {
		const auto code = value[index].unicode();
		if (code >= 0xDC00 && code <= 0xDFFF) {
			return false;
		}
		if (code >= 0xD800 && code <= 0xDBFF) {
			if (++index == value.size()) {
				return false;
			}
			const auto second = value[index].unicode();
			if (second < 0xDC00 || second > 0xDFFF) {
				return false;
			}
		}
	}
	return true;
}

[[nodiscard]] bool ValidJsonValue(const QJsonValue &value, int depth) {
	if (depth > kMaximumDepth) {
		return false;
	}
	if (value.isString()) {
		return ValidUnicode(value.toString());
	}
	if (value.isDouble()) {
		return SafeInteger(value, -kMaximumSafeInteger);
	}
	if (value.isObject()) {
		const auto object = value.toObject();
		for (auto it = object.begin(); it != object.end(); ++it) {
			if (!ValidUnicode(it.key())
				|| !ValidJsonValue(it.value(), depth + 1)) {
				return false;
			}
		}
		return true;
	}
	if (value.isArray()) {
		for (const auto &item : value.toArray()) {
			if (!ValidJsonValue(item, depth + 1)) {
				return false;
			}
		}
		return true;
	}
	return value.isNull() || value.isBool();
}

[[nodiscard]] bool ValidId(const QString &value, const char *prefix) {
	const auto bytes = value.toUtf8();
	if (bytes.size() != 29 || !bytes.startsWith(prefix)) {
		return false;
	}
	for (auto index = 3; index != 29; ++index) {
		const auto character = bytes[index];
		if (!((character >= 'a' && character <= 'z')
			|| (character >= '2' && character <= '7'))) {
			return false;
		}
	}
	return QByteArray("aeimquy4").contains(bytes.back());
}

[[nodiscard]] std::optional<QString> FormatId(
		const QByteArray &entropy,
		const char *prefix) {
	if (entropy.size() != 16) {
		return std::nullopt;
	}
	constexpr auto alphabet = "abcdefghijklmnopqrstuvwxyz234567";
	auto result = QByteArray(prefix);
	result.reserve(29);
	auto buffer = uint32_t(0);
	auto bits = 0;
	for (const auto byte : entropy) {
		buffer = (buffer << 8) | uint8_t(byte);
		bits += 8;
		while (bits >= 5) {
			bits -= 5;
			result.append(alphabet[(buffer >> bits) & 31]);
		}
		buffer &= (uint32_t(1) << bits) - 1;
	}
	if (bits != 0) {
		result.append(alphabet[(buffer << (5 - bits)) & 31]);
	}
	return QString::fromLatin1(result);
}

[[nodiscard]] bool ValidMetadata(const QJsonValue &value) {
	if (!value.isString()) {
		return false;
	}
	const auto text = value.toString();
	return !text.isEmpty()
		&& ValidUnicode(text)
		&& text.toUtf8().size() <= kMetadataBytes;
}

[[nodiscard]] bool ValidHash(const QString &value) {
	if (value.size() != 64) {
		return false;
	}
	for (const auto character : value) {
		if (!((character >= u'0' && character <= u'9')
			|| (character >= u'a' && character <= u'f'))) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] QByteArray CanonicalPayload(const QJsonObject &payload) {
	const auto serialized = QJsonDocument(payload).toJson(QJsonDocument::Compact);
	const auto result = CanonicalizeSyncJson(serialized);
	return result ? result.canonical : QByteArray();
}

[[nodiscard]] Validation Validate(
		const QJsonObject &document,
		qsizetype sourceBytes,
		bool checkHash) {
	const auto major = document.value(u"purple_sync"_q);
	if (major.isUndefined()) {
		return Invalid(SyncEnvelopeError::MissingField);
	}
	if (!SafeInteger(major, 1)) {
		return Invalid(SyncEnvelopeError::FieldType);
	}
	if (major.toDouble() > 1) {
		return { SyncEnvelopeStatus::NewerMajor };
	}
	const auto stream = document.value(u"stream"_q);
	if (stream.isUndefined()) {
		return Invalid(SyncEnvelopeError::MissingField);
	}
	if (!stream.isString()) {
		return Invalid(SyncEnvelopeError::FieldType);
	}
	const auto streamName = stream.toString();
	const auto limit = (streamName == u"config")
		? kConfigBytes : (streamName == u"library")
		? kLibraryBytes : 0;
	if (!limit) {
		return Invalid(SyncEnvelopeError::InvalidValue);
	}
	if (sourceBytes > limit) {
		return Invalid(SyncEnvelopeError::SizeLimit);
	}
	const auto encoding = document.value(u"encoding"_q);
	if (!encoding.isUndefined()) {
		if (!encoding.isString()) {
			return Invalid(SyncEnvelopeError::FieldType);
		}
		if (encoding.toString() != u"identity") {
			return { SyncEnvelopeStatus::UnsupportedEncoding };
		}
	}
	const auto space = document.value(u"space"_q);
	if (space.isUndefined()) {
		return Invalid(SyncEnvelopeError::MissingField);
	}
	if (!space.isString()) {
		return Invalid(SyncEnvelopeError::FieldType);
	}
	if (!IsSyncSpaceId(space.toString())) {
		return Invalid(SyncEnvelopeError::InvalidId);
	}
	const auto writer = document.value(u"writer"_q);
	if (writer.isUndefined()) {
		return Invalid(SyncEnvelopeError::MissingField);
	}
	if (!writer.isObject()) {
		return Invalid(SyncEnvelopeError::FieldType);
	}
	const auto writerObject = writer.toObject();
	const auto install = writerObject.value(u"install"_q);
	if (install.isUndefined()) {
		return Invalid(SyncEnvelopeError::MissingField);
	}
	if (!install.isString()) {
		return Invalid(SyncEnvelopeError::FieldType);
	}
	if (!IsSyncInstallId(install.toString())) {
		return Invalid(SyncEnvelopeError::InvalidId);
	}
	for (const auto &field : { u"device"_q, u"platform"_q, u"app"_q }) {
		const auto value = writerObject.value(field);
		if (value.isUndefined()) {
			return Invalid(SyncEnvelopeError::MissingField);
		}
		if (!value.isString()) {
			return Invalid(SyncEnvelopeError::FieldType);
		}
		if (!ValidMetadata(value)) {
			return Invalid(SyncEnvelopeError::InvalidValue);
		}
	}
	for (const auto &field : { u"seq"_q, u"at"_q }) {
		const auto value = document.value(field);
		if (value.isUndefined()) {
			return Invalid(SyncEnvelopeError::MissingField);
		}
		if (!SafeInteger(value, field == u"seq" ? 1 : 0)) {
			return Invalid(SyncEnvelopeError::FieldType);
		}
	}
	const auto payload = document.value(u"payload"_q);
	if (payload.isUndefined()) {
		return Invalid(SyncEnvelopeError::MissingField);
	}
	if (!payload.isObject()) {
		return Invalid(SyncEnvelopeError::FieldType);
	}
	if (checkHash) {
		const auto hash = document.value(u"payload_sha256"_q);
		if (hash.isUndefined()) {
			return Invalid(SyncEnvelopeError::MissingField);
		}
		if (!hash.isString()) {
			return Invalid(SyncEnvelopeError::FieldType);
		}
		if (!ValidHash(hash.toString())) {
			return Invalid(SyncEnvelopeError::InvalidHash);
		}
		const auto canonical = CanonicalPayload(payload.toObject());
		if (canonical.isEmpty()) {
			return Invalid(SyncEnvelopeError::InvalidJson);
		}
		const auto computed = QCryptographicHash::hash(
			canonical,
			QCryptographicHash::Sha256).toHex();
		if (hash.toString().toUtf8() != computed) {
			return Invalid(SyncEnvelopeError::HashMismatch);
		}
	}
	return {};
}

} // namespace

bool IsSyncSpaceId(const QString &value) {
	return ValidId(value, "sp-");
}

bool IsSyncInstallId(const QString &value) {
	return ValidId(value, "in-");
}

std::optional<QString> FormatSyncSpaceId(const QByteArray &entropy) {
	return FormatId(entropy, "sp-");
}

std::optional<QString> FormatSyncInstallId(const QByteArray &entropy) {
	return FormatId(entropy, "in-");
}

SyncEnvelopeParseResult ParseSyncEnvelope(const QByteArray &json) {
	if (json.size() > kLibraryBytes) {
		return { SyncEnvelopeStatus::Invalid, SyncEnvelopeError::SizeLimit };
	}
	const auto canonical = CanonicalizeSyncJson(json);
	if (!canonical) {
		const auto error = (canonical.error == SyncJsonErrorKind::SizeLimit)
			? SyncEnvelopeError::SizeLimit : SyncEnvelopeError::InvalidJson;
		return { SyncEnvelopeStatus::Invalid, error };
	}
	const auto parsed = QJsonDocument::fromJson(canonical.canonical);
	if (!parsed.isObject()) {
		return { SyncEnvelopeStatus::Invalid, SyncEnvelopeError::FieldType };
	}
	const auto document = parsed.object();
	const auto validation = Validate(document, json.size(), true);
	if (validation.status != SyncEnvelopeStatus::Valid) {
		return { validation.status, validation.error };
	}
	return {
		SyncEnvelopeStatus::Valid,
		SyncEnvelopeError::None,
		SyncEnvelope{ document },
	};
}

SyncEnvelopeWriteResult SerializeSyncEnvelope(
		const SyncEnvelope &envelope) {
	if (!ValidJsonValue(QJsonValue(envelope.document), 0)) {
		return { {}, SyncEnvelopeStatus::Invalid, SyncEnvelopeError::InvalidJson };
	}
	auto document = envelope.document;
	const auto validation = Validate(document, 0, false);
	if (validation.status != SyncEnvelopeStatus::Valid) {
		return { {}, validation.status, validation.error };
	}
	const auto payload = CanonicalPayload(document.value(u"payload"_q).toObject());
	if (payload.isEmpty()) {
		return { {}, SyncEnvelopeStatus::Invalid, SyncEnvelopeError::InvalidJson };
	}
	const auto hash = QCryptographicHash::hash(
		payload,
		QCryptographicHash::Sha256).toHex();
	document.insert(u"payload_sha256"_q, QString::fromLatin1(hash));
	const auto serialized = QJsonDocument(document).toJson(QJsonDocument::Compact);
	const auto canonical = CanonicalizeSyncJson(serialized);
	if (!canonical) {
		const auto error = (canonical.error == SyncJsonErrorKind::SizeLimit)
			? SyncEnvelopeError::SizeLimit : SyncEnvelopeError::InvalidJson;
		return { {}, SyncEnvelopeStatus::Invalid, error };
	}
	const auto final = Validate(document, canonical.canonical.size(), true);
	if (final.status != SyncEnvelopeStatus::Valid) {
		return { {}, final.status, final.error };
	}
	return {
		canonical.canonical,
		SyncEnvelopeStatus::Valid,
		SyncEnvelopeError::None,
	};
}

} // namespace Purple
