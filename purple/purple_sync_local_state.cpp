/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_local_state.h"

#include "purple/purple_config_sync.h"
#include "purple/purple_config_payload.h"
#include "purple/purple_sync_envelope.h"
#include "purple/purple_sync_json.h"
#include "purple/purple_types.h"

#include <QtCore/QJsonArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QSet>

#include <cmath>
#include <algorithm>
#include <limits>

namespace Purple {
namespace {

constexpr auto kMaximumSafeInteger = uint64_t(9007199254740991ULL);
constexpr auto kMaximumStateBytes = 4 * 1024 * 1024;
constexpr auto kMaximumDeviceBytes = 256;
constexpr auto kMaximumOwnMessages = 256;
constexpr auto kMaximumIssuedRecords = 32;
constexpr auto kBindingTokenBytes = 16;

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

[[nodiscard]] bool ValidBindingToken(const QString &value) {
	if (value.size() != 2 * kBindingTokenBytes) {
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
	if (object.contains(u"issued_records"_q)) {
		const auto value = object.value(u"issued_records"_q);
		if (!value.isArray()
			|| value.toArray().size() > kMaximumIssuedRecords) {
			error = SyncLocalError::InvalidIssuedRecords;
			return false;
		}
		for (const auto &item : value.toArray()) {
			if (!item.isObject()) {
				error = SyncLocalError::InvalidIssuedRecords;
				return false;
			}
			const auto entry = item.toObject();
			const auto issuedSeq = entry.value(u"seq"_q);
			const auto issuedHash = entry.value(u"record_sha256"_q);
			if (!SafeInteger(issuedSeq)
				|| issuedSeq.toDouble() < 1
				|| issuedSeq.toDouble() > double(stream.seq)
				|| !issuedHash.isString()
				|| !ValidHash(issuedHash.toString())
				|| (!stream.issuedRecords.empty()
					&& issuedSeq.toDouble()
						<= double(stream.issuedRecords.back().seq))) {
				error = SyncLocalError::InvalidIssuedRecords;
				return false;
			}
			stream.issuedRecords.push_back({
				uint64_t(issuedSeq.toDouble()),
				issuedHash.toString(),
				entry,
			});
		}
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
	if (!stream.issuedRecords.empty()
		|| object.contains(u"issued_records"_q)) {
		auto entries = QJsonArray();
		for (const auto &issued : stream.issuedRecords) {
			auto entry = issued.preserved;
			entry.insert(u"seq"_q, QJsonValue(qint64(issued.seq)));
			entry.insert(u"record_sha256"_q, issued.recordHash);
			entries.append(entry);
		}
		object.insert(u"issued_records"_q, entries);
	}
}

[[nodiscard]] bool ReadOwnMessages(
		const QJsonValue &value,
		const SyncLocalState &state,
		std::vector<SyncOwnMessage> &messages) {
	if (!value.isArray() || value.toArray().size() > kMaximumOwnMessages) {
		return false;
	}
	auto seen = QSet<int32_t>();
	for (const auto &item : value.toArray()) {
		if (!item.isObject()) {
			return false;
		}
		const auto object = item.toObject();
		const auto space = object.value(u"space"_q);
		const auto stream = object.value(u"stream"_q);
		const auto messageId = object.value(u"message_id"_q);
		const auto install = object.value(u"install"_q);
		const auto device = object.value(u"device"_q);
		const auto seq = object.value(u"seq"_q);
		const auto hash = object.value(u"payload_sha256"_q);
		const auto recordHash = object.value(u"record_sha256"_q);
		if (!space.isString() || !stream.isString()
			|| !SafeInteger(messageId)
			|| !install.isString() || !device.isString()
			|| !SafeInteger(seq) || !hash.isString()
			|| !recordHash.isString()
			|| !IsSyncSpaceId(space.toString())
			|| stream.toString() != u"config"_q
			|| messageId.toDouble() < 1
			|| messageId.toDouble() > std::numeric_limits<int32_t>::max()
			|| !IsSyncInstallId(install.toString())
			|| install.toString() != state.install
			|| device.toString().isEmpty()
			|| device.toString() != state.createdDevice
			|| seq.toDouble() < 1
			|| seq.toDouble() > double(state.config.confirmedSeq)
			|| !ValidHash(hash.toString())
			|| !ValidHash(recordHash.toString())) {
			return false;
		}
		const auto id = int32_t(messageId.toDouble());
		if (seen.contains(id)) {
			return false;
		}
		seen.insert(id);
		messages.push_back({
			space.toString(),
			SyncLocalStream::Config,
			id,
			install.toString(),
			device.toString(),
			uint64_t(seq.toDouble()),
			hash.toString(),
			recordHash.toString(),
			object,
		});
	}
	return true;
}

[[nodiscard]] QJsonArray WriteOwnMessages(
		const std::vector<SyncOwnMessage> &messages) {
	auto result = QJsonArray();
	for (const auto &message : messages) {
		auto object = message.preserved;
		object.insert(u"space"_q, message.space);
		object.insert(u"stream"_q, u"config"_q);
		object.insert(u"message_id"_q, message.messageId);
		object.insert(u"install"_q, message.install);
		object.insert(u"device"_q, message.device);
		object.insert(u"seq"_q, QJsonValue(qint64(message.seq)));
		object.insert(u"payload_sha256"_q, message.payloadHash);
		object.insert(u"record_sha256"_q, message.recordHash);
		result.append(object);
	}
	return result;
}

struct CheckedOwnConfigRecord {
	QString space;
	QString install;
	QString device;
	uint64_t seq = 0;
	QString payloadHash;
	QString recordHash;
};

[[nodiscard]] std::optional<CheckedOwnConfigRecord> CheckOwnConfigRecord(
		const QByteArray &canonicalRecord) {
	const auto parsed = ParseSyncEnvelope(canonicalRecord);
	if (!parsed || InspectConfigPayload(parsed).status
		!= ConfigPayloadStatus::Valid) {
		return std::nullopt;
	}
	const auto canonical = SerializeSyncEnvelope(parsed.envelope);
	if (!canonical || canonical.canonical != canonicalRecord) {
		return std::nullopt;
	}
	const auto document = parsed.envelope.document;
	const auto writer = document.value(u"writer"_q).toObject();
	return CheckedOwnConfigRecord{
		document.value(u"space"_q).toString(),
		writer.value(u"install"_q).toString(),
		writer.value(u"device"_q).toString(),
		uint64_t(document.value(u"seq"_q).toDouble()),
		document.value(u"payload_sha256"_q).toString(),
		QString::fromLatin1(QCryptographicHash::hash(
			canonicalRecord, QCryptographicHash::Sha256).toHex()),
	};
}

[[nodiscard]] bool SameOwnMessage(
		const SyncOwnMessage &message,
		const CheckedOwnConfigRecord &record) {
	return message.stream == SyncLocalStream::Config
		&& message.space == record.space
		&& message.install == record.install
		&& message.device == record.device
		&& message.seq == record.seq
		&& message.payloadHash == record.payloadHash
		&& message.recordHash == record.recordHash;
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
	const auto bindingToken = document.value(u"binding_token"_q);
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
	if (!bindingToken.isUndefined()
		&& (!bindingToken.isString()
			|| !ValidBindingToken(bindingToken.toString()))) {
		return Invalid(SyncLocalError::InvalidBindingToken);
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
	state.bindingToken = bindingToken.toString();
	state.preserved = document;
	auto error = SyncLocalError::None;
	if (!ReadStream(config.toObject(), state.config, error)
		|| !ReadStream(library.toObject(), state.library, error)
		|| !ReadConfig(config.toObject(), state.configData, error)) {
		return Invalid(error);
	}
	if (document.contains(u"own_messages"_q)
		&& !ReadOwnMessages(document.value(u"own_messages"_q),
			state, state.ownMessages)) {
		return Invalid(SyncLocalError::InvalidOwnMessages);
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
	if (!state.bindingToken.isEmpty()
		&& !ValidBindingToken(state.bindingToken)) {
		return { {}, SyncLocalStatus::Invalid,
			SyncLocalError::InvalidBindingToken };
	}
	for (const auto stream : { &state.config, &state.library }) {
		if (stream->seq > kMaximumSafeInteger
			|| stream->pendingSeq > kMaximumSafeInteger
			|| stream->confirmedSeq > kMaximumSafeInteger) {
			return { {}, SyncLocalStatus::Invalid,
				SyncLocalError::InvalidCounters };
		}
		if (stream->issuedRecords.size() > kMaximumIssuedRecords) {
			return { {}, SyncLocalStatus::Invalid,
				SyncLocalError::InvalidIssuedRecords };
		}
		auto previous = uint64_t(0);
		for (const auto &issued : stream->issuedRecords) {
			if (issued.seq <= previous || issued.seq > stream->seq
				|| !ValidHash(issued.recordHash)) {
				return { {}, SyncLocalStatus::Invalid,
					SyncLocalError::InvalidIssuedRecords };
			}
			previous = issued.seq;
		}
	}
	for (const auto &entry : state.configData.seenSeq) {
		if (entry.second > kMaximumSafeInteger) {
			return { {}, SyncLocalStatus::Invalid,
				SyncLocalError::InvalidConfig };
		}
	}
	if (state.ownMessages.size() > kMaximumOwnMessages) {
		return { {}, SyncLocalStatus::Invalid,
			SyncLocalError::InvalidOwnMessages };
	}
	for (const auto &message : state.ownMessages) {
		if (message.stream != SyncLocalStream::Config) {
			return { {}, SyncLocalStatus::Invalid,
				SyncLocalError::InvalidOwnMessages };
		}
	}
	auto document = state.preserved;
	document.insert(u"version"_q, state.version);
	document.insert(u"install"_q, state.install);
	document.insert(u"created_device"_q, state.createdDevice);
	document.insert(u"space"_q, state.space);
	if (state.bindingToken.isEmpty()) {
		document.remove(u"binding_token"_q);
	} else {
		document.insert(u"binding_token"_q, state.bindingToken);
	}
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
	if (!state.ownMessages.empty()
		|| document.contains(u"own_messages"_q)) {
		document.insert(u"own_messages"_q,
			WriteOwnMessages(state.ownMessages));
	}
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

std::optional<QString> FormatSyncBindingToken(const QByteArray &entropy) {
	if (entropy.size() != kBindingTokenBytes) {
		return std::nullopt;
	}
	return QString::fromLatin1(entropy.toHex());
}

SyncAccountBindingVerdict CheckSyncAccountBinding(
		const SyncLocalState &state,
		const QByteArray &accountPrefToken) {
	if (!SerializeSyncLocalState(state)) {
		return SyncAccountBindingVerdict::InvalidState;
	}
	if (state.bindingToken.isEmpty()) {
		return SyncAccountBindingVerdict::MissingStateToken;
	}
	if (accountPrefToken.isEmpty()) {
		return SyncAccountBindingVerdict::MissingAccountToken;
	}
	if (!ValidBindingToken(QString::fromLatin1(accountPrefToken))
		|| accountPrefToken.size() != 2 * kBindingTokenBytes) {
		return SyncAccountBindingVerdict::InvalidAccountToken;
	}
	return (accountPrefToken == state.bindingToken.toLatin1())
		? SyncAccountBindingVerdict::Bound
		: SyncAccountBindingVerdict::Mismatch;
}

SyncPublishDecision PlanSyncPublish(
		const SyncLocalState &state,
		const QByteArray &accountPrefToken,
		const QString &currentDevice,
		SyncLocalStream stream,
		const QString &desiredPayloadHash,
		const SyncPublishPolicy &policy,
		const SyncPublishObservations &observations) {
	if ((stream != SyncLocalStream::Config
			&& stream != SyncLocalStream::Library)
		|| CheckSyncAccountBinding(state, accountPrefToken)
			!= SyncAccountBindingVerdict::Bound
		|| state.space.isEmpty()
		|| currentDevice.isEmpty()
		|| currentDevice != state.createdDevice
		|| !policy.enabled
		|| (stream == SyncLocalStream::Library
			&& !policy.libraryPayloadValidated)) {
		return { SyncPublishAction::Pause };
	}
	if (!observations.discoveryComplete
		|| observations.ownRecord.kind
			== OwnRecordObservationKind::Unresolved) {
		return { SyncPublishAction::Wait };
	}
	if (CheckSyncClone(state, currentDevice, stream,
			observations.ownRecord) != SyncCloneVerdict::NoClone) {
		return { SyncPublishAction::Pause };
	}
	const auto &local = Select(state, stream);
	const auto &own = observations.ownRecord;
	if ((own.kind == OwnRecordObservationKind::Absent
			&& local.confirmedSeq != 0)
		|| (own.kind == OwnRecordObservationKind::Present
			&& own.seq != local.confirmedSeq
			&& own.seq != local.pendingSeq)
		|| (own.kind == OwnRecordObservationKind::Present
			&& local.confirmedSeq == 0
			&& own.seq != local.pendingSeq)) {
		return { SyncPublishAction::Pause };
	}
	if (observations.publishInFlight) {
		return { SyncPublishAction::Wait };
	}
	if (local.pendingSeq != 0) {
		if (!observations.stagedRecordMatches) {
			return { SyncPublishAction::Pause };
		}
		const auto issued = std::find_if(
			local.issuedRecords.begin(), local.issuedRecords.end(),
			[&](const auto &entry) {
				return entry.seq == local.pendingSeq;
			});
		if (issued == local.issuedRecords.end()
			|| issued->recordHash != observations.stagedRecordHash) {
			return { SyncPublishAction::Pause };
		}
		if (own.kind == OwnRecordObservationKind::Present
			&& own.seq == local.confirmedSeq) {
			const auto previous = std::find_if(
				state.ownMessages.begin(), state.ownMessages.end(),
				[&](const auto &message) {
					return message.stream == stream
						&& message.space == state.space
						&& message.seq == own.seq
						&& message.payloadHash == own.payloadHash;
				});
			if (previous == state.ownMessages.end()) {
				return { SyncPublishAction::Pause };
			}
		}
		if (own.kind == OwnRecordObservationKind::Present
			&& own.seq == local.pendingSeq) {
			return observations.ready
				? SyncPublishDecision{ SyncPublishAction::Reconcile }
				: SyncPublishDecision{ SyncPublishAction::Wait };
		}
		if (observations.attempt == SyncPublishAttempt::MayHaveReachedServer) {
			return observations.ready
				? SyncPublishDecision{ SyncPublishAction::Reconcile }
				: SyncPublishDecision{ SyncPublishAction::Wait };
		}
		if (observations.attempt != SyncPublishAttempt::Unsent
			&& observations.attempt
				!= SyncPublishAttempt::ReconciledAbsent) {
			return { SyncPublishAction::Pause };
		}
		if (!observations.ready) {
			return { SyncPublishAction::Wait };
		}
	} else {
		if (!ValidHash(desiredPayloadHash)) {
			return { SyncPublishAction::Wait };
		}
		if (own.kind == OwnRecordObservationKind::Present) {
			const auto confirmed = std::find_if(
				state.ownMessages.begin(), state.ownMessages.end(),
				[&](const auto &message) {
					return message.stream == stream
						&& message.space == state.space
						&& message.seq == local.confirmedSeq
						&& message.payloadHash == own.payloadHash;
				});
			if (confirmed == state.ownMessages.end()) {
				return observations.ready
					? SyncPublishDecision{ SyncPublishAction::Reconcile }
					: SyncPublishDecision{ SyncPublishAction::Wait };
			}
		}
		if (local.confirmedSeq == 0
			|| desiredPayloadHash != local.ownHash) {
			return observations.ready
				? SyncPublishDecision{ SyncPublishAction::ReserveAndStage }
				: SyncPublishDecision{ SyncPublishAction::Wait };
		}
		if (!observations.ready) {
			return { SyncPublishAction::Wait };
		}
		for (const auto &message : state.ownMessages) {
			if (message.stream != stream
				|| message.seq >= local.confirmedSeq) {
				continue;
			}
			return { SyncPublishAction::RetireCandidate,
				message.messageId };
		}
		return { SyncPublishAction::Wait };
	}
	if (own.kind == OwnRecordObservationKind::Absent
		|| observations.editRefused
		|| !policy.editEnabled) {
		return { SyncPublishAction::Post };
	}
	const auto head = std::find_if(
		state.ownMessages.begin(), state.ownMessages.end(),
		[&](const auto &message) {
			return message.stream == stream
				&& message.space == state.space
				&& message.messageId == observations.ownHead.messageId
				&& message.seq == local.confirmedSeq
				&& message.payloadHash == own.payloadHash;
		});
	if (head == state.ownMessages.end()) {
		return { SyncPublishAction::Reconcile };
	}
	if (!observations.ownHead.fresh) {
		return { SyncPublishAction::Reconcile, head->messageId };
	}
	return (observations.ownHead.recordHash == head->recordHash)
		? SyncPublishDecision{ SyncPublishAction::Edit, head->messageId }
		: SyncPublishDecision{ SyncPublishAction::Pause };
}

SyncIssueResult AppendIssuedConfigRecord(
		const SyncLocalState &state,
		const QByteArray &canonicalRecord) {
	if (!SerializeSyncLocalState(state)) {
		return { state, SyncIssueError::InvalidState };
	}
	const auto record = CheckOwnConfigRecord(canonicalRecord);
	if (!record) {
		return { state, SyncIssueError::InvalidRecord };
	}
	if (state.createdDevice.isEmpty()
		|| record->space != state.space
		|| record->install != state.install
		|| record->device != state.createdDevice
		|| state.config.pendingSeq == 0
		|| record->seq != state.config.pendingSeq
		|| record->payloadHash != state.config.ownHash) {
		return { state, SyncIssueError::RecordMismatch };
	}
	const auto &issued = state.config.issuedRecords;
	if (!issued.empty() && issued.back().seq >= record->seq) {
		return (issued.back().seq == record->seq
			&& issued.back().recordHash == record->recordHash)
			? SyncIssueResult{ state }
			: SyncIssueResult{ state, SyncIssueError::RecordMismatch };
	}
	auto next = state;
	auto &records = next.config.issuedRecords;
	if (records.size() == kMaximumIssuedRecords) {
		records.erase(records.begin());
	}
	records.push_back({ record->seq, record->recordHash });
	if (!SerializeSyncLocalState(next)) {
		return { state, SyncIssueError::InvalidState };
	}
	return { std::move(next), SyncIssueError::None, true };
}

SyncOwnMessageResult AdoptIssuedOwnConfigMessage(
		const SyncLocalState &state,
		int32_t messageId,
		const QByteArray &canonicalRecord) {
	if (messageId <= 0) {
		return { state, SyncOwnMessageError::InvalidMessageId };
	}
	if (!SerializeSyncLocalState(state)) {
		return { state, SyncOwnMessageError::InvalidState };
	}
	const auto record = CheckOwnConfigRecord(canonicalRecord);
	if (!record) {
		return { state, SyncOwnMessageError::InvalidRecord };
	}
	if (state.createdDevice.isEmpty()
		|| record->install != state.install
		|| record->device != state.createdDevice
		|| record->seq > state.config.confirmedSeq) {
		return { state, SyncOwnMessageError::RecordMismatch };
	}
	const auto issued = std::find_if(
		state.config.issuedRecords.begin(), state.config.issuedRecords.end(),
		[&](const auto &entry) { return entry.seq == record->seq; });
	if (issued == state.config.issuedRecords.end()
		|| issued->recordHash != record->recordHash) {
		return { state, SyncOwnMessageError::UnissuedRecord };
	}
	const auto existing = std::find_if(
		state.ownMessages.begin(), state.ownMessages.end(),
		[=](const auto &entry) { return entry.messageId == messageId; });
	if (existing != state.ownMessages.end()) {
		return SameOwnMessage(*existing, *record)
			? SyncOwnMessageResult{ state }
			: SyncOwnMessageResult{ state, SyncOwnMessageError::IdConflict };
	}
	if (state.ownMessages.size() == kMaximumOwnMessages) {
		return { state, SyncOwnMessageError::CapacityExceeded };
	}
	auto next = state;
	next.ownMessages.push_back({
		record->space,
		SyncLocalStream::Config,
		messageId,
		record->install,
		record->device,
		record->seq,
		record->payloadHash,
		record->recordHash,
	});
	if (!SerializeSyncLocalState(next)) {
		return { state, SyncOwnMessageError::InvalidState };
	}
	return { std::move(next), SyncOwnMessageError::None, true };
}

SyncOwnMessageResult RecordConfirmedOwnConfigMessage(
		const SyncLocalState &state,
		int32_t messageId,
		const QByteArray &canonicalRecord) {
	if (messageId <= 0) {
		return { state, SyncOwnMessageError::InvalidMessageId };
	}
	if (!SerializeSyncLocalState(state)) {
		return { state, SyncOwnMessageError::InvalidState };
	}
	const auto record = CheckOwnConfigRecord(canonicalRecord);
	if (!record) {
		return { state, SyncOwnMessageError::InvalidRecord };
	}
	if (state.createdDevice.isEmpty()
		|| record->space != state.space
		|| record->install != state.install
		|| record->device != state.createdDevice
		|| state.config.confirmedSeq == 0
		|| record->seq != state.config.confirmedSeq
		|| record->payloadHash != state.config.ownHash) {
		return { state, SyncOwnMessageError::RecordMismatch };
	}
	auto next = state;
	const auto existing = std::find_if(
		next.ownMessages.begin(), next.ownMessages.end(),
		[=](const auto &entry) { return entry.messageId == messageId; });
	if (existing != next.ownMessages.end()) {
		if (existing->space != record->space) {
			return { state, SyncOwnMessageError::IdConflict };
		}
		if (existing->seq > record->seq) {
			return { state, SyncOwnMessageError::SequenceRegression };
		}
		if (existing->seq == record->seq) {
			return SameOwnMessage(*existing, *record)
				? SyncOwnMessageResult{ state }
				: SyncOwnMessageResult{ state,
					SyncOwnMessageError::IdConflict };
		}
		*existing = {
			record->space,
			SyncLocalStream::Config,
			messageId,
			record->install,
			record->device,
			record->seq,
			record->payloadHash,
			record->recordHash,
			existing->preserved,
		};
	} else {
		if (next.ownMessages.size() == kMaximumOwnMessages) {
			return { state, SyncOwnMessageError::CapacityExceeded };
		}
		next.ownMessages.push_back({
			record->space,
			SyncLocalStream::Config,
			messageId,
			record->install,
			record->device,
			record->seq,
			record->payloadHash,
			record->recordHash,
		});
	}
	if (!SerializeSyncLocalState(next)) {
		return { state, SyncOwnMessageError::InvalidState };
	}
	return { std::move(next), SyncOwnMessageError::None, true };
}

SyncOwnDeleteCheck CheckOwnConfigMessageDeletion(
		const SyncLocalState &state,
		int32_t messageId,
		const QByteArray &canonicalRecord) {
	if (messageId <= 0) {
		return { SyncOwnMessageError::InvalidMessageId };
	}
	if (!SerializeSyncLocalState(state)) {
		return { SyncOwnMessageError::InvalidState };
	}
	const auto existing = std::find_if(
		state.ownMessages.begin(), state.ownMessages.end(),
		[=](const auto &entry) { return entry.messageId == messageId; });
	if (existing == state.ownMessages.end()) {
		return { SyncOwnMessageError::NotFound };
	}
	if (existing->seq >= state.config.confirmedSeq) {
		return { SyncOwnMessageError::NotOlder };
	}
	const auto record = CheckOwnConfigRecord(canonicalRecord);
	if (!record) {
		return { SyncOwnMessageError::InvalidRecord };
	}
	return SameOwnMessage(*existing, *record)
		? SyncOwnDeleteCheck{}
		: SyncOwnDeleteCheck{ SyncOwnMessageError::RecordMismatch };
}

SyncOwnMessageResult RemoveAbsentOwnConfigMessage(
		const SyncLocalState &state,
		int32_t messageId,
		SyncOwnMessagePresence presence) {
	if (messageId <= 0) {
		return { state, SyncOwnMessageError::InvalidMessageId };
	}
	if (!SerializeSyncLocalState(state)) {
		return { state, SyncOwnMessageError::InvalidState };
	}
	if (presence != SyncOwnMessagePresence::Absent) {
		return { state, SyncOwnMessageError::NotAbsent };
	}
	auto next = state;
	const auto existing = std::find_if(
		next.ownMessages.begin(), next.ownMessages.end(),
		[=](const auto &entry) { return entry.messageId == messageId; });
	if (existing == next.ownMessages.end()) {
		return { state, SyncOwnMessageError::NotFound };
	}
	if (existing->seq >= state.config.confirmedSeq) {
		return { state, SyncOwnMessageError::NotOlder };
	}
	next.ownMessages.erase(existing);
	if (!SerializeSyncLocalState(next)) {
		return { state, SyncOwnMessageError::InvalidState };
	}
	return { std::move(next), SyncOwnMessageError::None, true };
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

SyncConfirmation ConfirmConfigReadBack(
		const SyncLocalState &state,
		const QString &currentDevice,
		const OwnRecordObservation &observation,
		const ConfigVersion &stagedVersion) {
	if (!state.createdDevice.isEmpty() && currentDevice.isEmpty()) {
		return { state, SyncCloneVerdict::DeviceMismatch, false };
	}
	auto result = ConfirmSyncReadBack(
		state, currentDevice, SyncLocalStream::Config, observation);
	if (!result.changed) {
		return result;
	}
	const auto staged = ParseConfigVersionKey(stagedVersion.key);
	if (!staged || state.configData.pending != stagedVersion.key) {
		return { state, SyncCloneVerdict::InvalidObservation, false };
	}
	const auto oldBase = ParseConfigVersionKey(state.configData.base);
	if (!oldBase || oldBase->fingerprint != staged->fingerprint) {
		result.state.configData.equiv.clear();
	}
	result.state.configData.base = stagedVersion.key;
	result.state.configData.baseLineage = stagedVersion.lineage;
	result.state.configData.pending.clear();
	if (!SerializeSyncLocalState(result.state)) {
		return { state, SyncCloneVerdict::InvalidObservation, false };
	}
	return result;
}

} // namespace Purple
