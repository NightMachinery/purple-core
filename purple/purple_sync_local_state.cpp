/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_local_state.h"

#include "purple/purple_config_sync.h"
#include "purple/purple_sync_envelope.h"
#include "purple/purple_sync_json.h"
#include "purple/purple_types.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSet>

#include <cmath>

namespace Purple {
namespace {

constexpr auto kMaximumSafeInteger = uint64_t(9007199254740991ULL);
constexpr auto kMaximumStateBytes = 4 * 1024 * 1024;
constexpr auto kMaximumDeviceBytes = 256;

[[nodiscard]] SyncLocalParseResult Invalid(SyncLocalError error) {
	return { SyncLocalStatus::Invalid, error, {} };
}

[[nodiscard]] bool SafeInteger(const QJsonValue &value) {
	if (!value.isDouble()) {
		return false;
	}
	const auto number = value.toDouble();
	return std::isfinite(number)
		&& number >= 0
		&& number <= double(kMaximumSafeInteger)
		&& std::floor(number) == number;
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

[[nodiscard]] SyncLocalStreamState &Select(
		SyncLocalState &state,
		SyncLocalStream stream) {
	return (stream == SyncLocalStream::Config)
		? state.config : state.library;
}

[[nodiscard]] const SyncLocalStreamState &Select(
		const SyncLocalState &state,
		SyncLocalStream stream) {
	return (stream == SyncLocalStream::Config)
		? state.config : state.library;
}

[[nodiscard]] bool ReadStream(
		const QJsonObject &object,
		SyncLocalStreamState &stream,
		SyncLocalError &error) {
	for (const auto &field : {
		u"seq"_q,
		u"pending_seq"_q,
		u"confirmed_seq"_q,
		u"own_hash"_q,
	}) {
		if (!object.contains(field)) {
			error = SyncLocalError::MissingField;
			return false;
		}
	}
	const auto seq = object.value(u"seq"_q);
	const auto pending = object.value(u"pending_seq"_q);
	const auto confirmed = object.value(u"confirmed_seq"_q);
	const auto hash = object.value(u"own_hash"_q);
	if (!SafeInteger(seq)
		|| !SafeInteger(pending)
		|| !SafeInteger(confirmed)
		|| !hash.isString()) {
		error = SyncLocalError::FieldType;
		return false;
	}
	stream.seq = uint64_t(seq.toDouble());
	stream.pendingSeq = uint64_t(pending.toDouble());
	stream.confirmedSeq = uint64_t(confirmed.toDouble());
	stream.ownHash = hash.toString();
	if ((stream.pendingSeq == 0 && stream.confirmedSeq != stream.seq)
		|| (stream.pendingSeq != 0
			&& (stream.pendingSeq != stream.seq
				|| stream.confirmedSeq >= stream.seq))) {
		error = SyncLocalError::InvalidCounters;
		return false;
	}
	if ((stream.seq == 0 && !stream.ownHash.isEmpty())
		|| (stream.seq != 0 && !ValidHash(stream.ownHash))) {
		error = SyncLocalError::InvalidHash;
		return false;
	}
	return true;
}

[[nodiscard]] bool ReadKeys(
		const QJsonValue &value,
		std::vector<QString> &keys,
		int limit,
		SyncLocalError &error) {
	if (!value.isArray()) {
		error = SyncLocalError::FieldType;
		return false;
	}
	const auto array = value.toArray();
	if (array.size() > limit) {
		error = SyncLocalError::InvalidConfig;
		return false;
	}
	auto seen = QSet<QString>();
	for (const auto &item : array) {
		if (!item.isString()) {
			error = SyncLocalError::FieldType;
			return false;
		}
		const auto key = item.toString();
		if (!ParseConfigVersionKey(key) || seen.contains(key)) {
			error = SyncLocalError::InvalidConfig;
			return false;
		}
		keys.push_back(key);
		seen.insert(key);
	}
	return true;
}

[[nodiscard]] bool ReadConfig(
		const QJsonObject &object,
		SyncLocalConfigState &config,
		SyncLocalError &error) {
	for (const auto &field : {
		u"base"_q,
		u"base_lineage"_q,
		u"equiv"_q,
		u"pending"_q,
		u"seen_seq"_q,
	}) {
		if (!object.contains(field)) {
			error = SyncLocalError::MissingField;
			return false;
		}
	}
	const auto base = object.value(u"base"_q);
	const auto pending = object.value(u"pending"_q);
	const auto seen = object.value(u"seen_seq"_q);
	if (!base.isString() || !pending.isString() || !seen.isObject()) {
		error = SyncLocalError::FieldType;
		return false;
	}
	config.base = base.toString();
	config.pending = pending.toString();
	if ((!config.base.isEmpty() && !ParseConfigVersionKey(config.base))
		|| (!config.pending.isEmpty()
			&& !ParseConfigVersionKey(config.pending))) {
		error = SyncLocalError::InvalidConfig;
		return false;
	}
	if (!ReadKeys(object.value(u"base_lineage"_q),
			config.baseLineage, 64, error)
		|| !ReadKeys(object.value(u"equiv"_q),
			config.equiv, 16, error)) {
		return false;
	}
	if (config.base.isEmpty() && !config.baseLineage.empty()) {
		error = SyncLocalError::InvalidConfig;
		return false;
	}
	if (!config.base.isEmpty()) {
		const auto generation = ParseConfigVersionKey(config.base)->generation;
		for (const auto &ancestor : config.baseLineage) {
			if (ParseConfigVersionKey(ancestor)->generation >= generation) {
				error = SyncLocalError::InvalidConfig;
				return false;
			}
		}
	}
	const auto seenObject = seen.toObject();
	for (auto it = seenObject.begin(); it != seenObject.end(); ++it) {
		if (!IsSyncInstallId(it.key()) || !SafeInteger(it.value())) {
			error = SyncLocalError::InvalidConfig;
			return false;
		}
		config.seenSeq[it.key()] = uint64_t(it.value().toDouble());
	}
	return true;
}

[[nodiscard]] QJsonArray WriteKeys(const std::vector<QString> &keys) {
	auto result = QJsonArray();
	for (const auto &key : keys) {
		result.append(key);
	}
	return result;
}

void WriteStream(QJsonObject &object, const SyncLocalStreamState &stream) {
	object.insert(u"seq"_q, QJsonValue(qint64(stream.seq)));
	object.insert(u"pending_seq"_q, QJsonValue(qint64(stream.pendingSeq)));
	object.insert(u"confirmed_seq"_q, QJsonValue(qint64(stream.confirmedSeq)));
	object.insert(u"own_hash"_q, stream.ownHash);
}

} // namespace

SyncLocalParseResult ParseSyncLocalState(const QByteArray &json) {
	if (json.size() > kMaximumStateBytes) {
		return Invalid(SyncLocalError::SizeLimit);
	}
	const auto canonical = CanonicalizeSyncJson(json);
	if (!canonical) {
		const auto error = (canonical.error == SyncJsonErrorKind::SizeLimit)
			? SyncLocalError::SizeLimit : SyncLocalError::InvalidJson;
		return Invalid(error);
	}
	const auto parsed = QJsonDocument::fromJson(canonical.canonical);
	if (!parsed.isObject()) {
		return Invalid(SyncLocalError::FieldType);
	}
	const auto document = parsed.object();
	const auto version = document.value(u"version"_q);
	if (version.isUndefined()) {
		return Invalid(SyncLocalError::MissingField);
	}
	if (!SafeInteger(version) || version.toDouble() == 0) {
		return Invalid(SyncLocalError::FieldType);
	}
	if (version.toDouble() > 1) {
		return { SyncLocalStatus::NewerVersion, SyncLocalError::None, {} };
	}
	for (const auto &field : {
		u"install"_q,
		u"created_device"_q,
		u"space"_q,
		u"streams"_q,
	}) {
		if (!document.contains(field)) {
			return Invalid(SyncLocalError::MissingField);
		}
	}
	const auto install = document.value(u"install"_q);
	const auto createdDevice = document.value(u"created_device"_q);
	const auto space = document.value(u"space"_q);
	const auto streams = document.value(u"streams"_q);
	if (!install.isString() || !createdDevice.isString()
		|| !space.isString() || !streams.isObject()) {
		return Invalid(SyncLocalError::FieldType);
	}
	if (!IsSyncInstallId(install.toString())
		|| !IsSyncSpaceId(space.toString())) {
		return Invalid(SyncLocalError::InvalidId);
	}
	if (createdDevice.toString().toUtf8().size() > kMaximumDeviceBytes) {
		return Invalid(SyncLocalError::InvalidValue);
	}
	const auto streamObjects = streams.toObject();
	if (!streamObjects.contains(u"config"_q)
		|| !streamObjects.contains(u"library"_q)) {
		return Invalid(SyncLocalError::MissingField);
	}
	const auto config = streamObjects.value(u"config"_q);
	const auto library = streamObjects.value(u"library"_q);
	if (!config.isObject() || !library.isObject()) {
		return Invalid(SyncLocalError::FieldType);
	}
	auto state = SyncLocalState();
	state.install = install.toString();
	state.createdDevice = createdDevice.toString();
	state.space = space.toString();
	state.preserved = document;
	auto error = SyncLocalError::None;
	if (!ReadStream(config.toObject(), state.config, error)
		|| !ReadStream(library.toObject(), state.library, error)
		|| !ReadConfig(config.toObject(), state.configData, error)) {
		return Invalid(error);
	}
	return { SyncLocalStatus::Valid, SyncLocalError::None, std::move(state) };
}

SyncLocalWriteResult SerializeSyncLocalState(const SyncLocalState &state) {
	if (state.version > 1) {
		return { {}, SyncLocalStatus::NewerVersion };
	}
	if (state.version != 1) {
		return { {}, SyncLocalStatus::Invalid, SyncLocalError::InvalidValue };
	}
	for (const auto stream : { &state.config, &state.library }) {
		if (stream->seq > kMaximumSafeInteger
			|| stream->pendingSeq > kMaximumSafeInteger
			|| stream->confirmedSeq > kMaximumSafeInteger) {
			return { {}, SyncLocalStatus::Invalid,
				SyncLocalError::InvalidCounters };
		}
	}
	for (const auto &entry : state.configData.seenSeq) {
		if (entry.second > kMaximumSafeInteger) {
			return { {}, SyncLocalStatus::Invalid,
				SyncLocalError::InvalidConfig };
		}
	}
	auto document = state.preserved;
	document.insert(u"version"_q, state.version);
	document.insert(u"install"_q, state.install);
	document.insert(u"created_device"_q, state.createdDevice);
	document.insert(u"space"_q, state.space);
	auto streams = document.value(u"streams"_q).toObject();
	auto config = streams.value(u"config"_q).toObject();
	auto library = streams.value(u"library"_q).toObject();
	WriteStream(config, state.config);
	WriteStream(library, state.library);
	config.insert(u"base"_q, state.configData.base);
	config.insert(u"base_lineage"_q, WriteKeys(state.configData.baseLineage));
	config.insert(u"equiv"_q, WriteKeys(state.configData.equiv));
	config.insert(u"pending"_q, state.configData.pending);
	auto seen = QJsonObject();
	for (const auto &[install, seq] : state.configData.seenSeq) {
		seen.insert(install, QJsonValue(qint64(seq)));
	}
	config.insert(u"seen_seq"_q, seen);
	streams.insert(u"config"_q, config);
	streams.insert(u"library"_q, library);
	document.insert(u"streams"_q, streams);
	const auto json = QJsonDocument(document).toJson(QJsonDocument::Compact);
	const auto canonical = CanonicalizeSyncJson(json);
	if (!canonical) {
		const auto error = (canonical.error == SyncJsonErrorKind::SizeLimit)
			? SyncLocalError::SizeLimit : SyncLocalError::InvalidJson;
		return { {}, SyncLocalStatus::Invalid, error };
	}
	const auto validated = ParseSyncLocalState(canonical.canonical);
	if (!validated) {
		return { {}, validated.status, validated.error };
	}
	return { canonical.canonical, SyncLocalStatus::Valid };
}

SyncReservation ReserveSyncSeq(
		const SyncLocalState &state,
		SyncLocalStream stream,
		const QString &payloadHash) {
	if (!SerializeSyncLocalState(state)) {
		return { {}, 0, SyncReservationError::InvalidState };
	}
	if (!ValidHash(payloadHash)) {
		return { {}, 0, SyncReservationError::InvalidHash };
	}
	if (Select(state, stream).seq == kMaximumSafeInteger) {
		return { {}, 0, SyncReservationError::SequenceExhausted };
	}
	auto next = state;
	auto &streamState = Select(next, stream);
	++streamState.seq;
	streamState.pendingSeq = streamState.seq;
	streamState.ownHash = payloadHash;
	const auto issued = streamState.seq;
	return { std::move(next), issued };
}

SyncCloneVerdict CheckSyncClone(
		const SyncLocalState &state,
		const QString &currentDevice,
		SyncLocalStream stream,
		const OwnRecordObservation &observation) {
	if (!currentDevice.isEmpty()
		&& !state.createdDevice.isEmpty()
		&& currentDevice != state.createdDevice) {
		return SyncCloneVerdict::DeviceMismatch;
	}
	if (observation.kind == OwnRecordObservationKind::Unresolved) {
		return SyncCloneVerdict::PendingReconcile;
	}
	if (observation.kind == OwnRecordObservationKind::Absent) {
		return SyncCloneVerdict::NoClone;
	}
	if (observation.kind != OwnRecordObservationKind::Present
		|| observation.seq == 0
		|| observation.seq > kMaximumSafeInteger
		|| !ValidHash(observation.payloadHash)) {
		return SyncCloneVerdict::InvalidObservation;
	}
	const auto &local = Select(state, stream);
	if (observation.seq > local.seq) {
		return SyncCloneVerdict::RemoteAhead;
	}
	if (observation.seq == local.seq
		&& observation.payloadHash != local.ownHash) {
		return SyncCloneVerdict::HashMismatch;
	}
	return SyncCloneVerdict::NoClone;
}

SyncConfirmation ConfirmSyncReadBack(
		const SyncLocalState &state,
		const QString &currentDevice,
		SyncLocalStream stream,
		const OwnRecordObservation &observation) {
	auto result = SyncConfirmation();
	result.state = state;
	result.verdict = CheckSyncClone(
		state, currentDevice, stream, observation);
	if (result.verdict != SyncCloneVerdict::NoClone
		|| observation.kind != OwnRecordObservationKind::Present) {
		return result;
	}
	auto &local = Select(result.state, stream);
	if (local.pendingSeq != 0 && observation.seq == local.seq) {
		local.confirmedSeq = local.seq;
		local.pendingSeq = 0;
		result.changed = true;
	}
	return result;
}

} // namespace Purple
