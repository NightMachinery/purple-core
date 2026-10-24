/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_archive_codec.h"
#include "purple/purple_archive_rows.h"

#include "purple/purple_sync_json.h"
#include "purple/purple_archive_internal.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>

#include <type_traits>
#include <algorithm>
#include <limits>

namespace Purple {
namespace {

QJsonValue Write(const ArchiveChatKey &value);
bool Read(const QJsonValue &json, ArchiveChatKey &value, bool &unsupported);
QJsonValue Write(const ArchiveMessageKey &value);
bool Read(const QJsonValue &json, ArchiveMessageKey &value, bool &unsupported);
QJsonValue Write(const ArchiveMediaDescriptor &value);
bool Read(const QJsonValue &json, ArchiveMediaDescriptor &value, bool &unsupported);
QJsonValue Write(const ArchiveContent &value);
bool Read(const QJsonValue &json, ArchiveContent &value, bool &unsupported);
QJsonValue Write(const ArchiveSourceBinding &value);
bool Read(const QJsonValue &json, ArchiveSourceBinding &value, bool &unsupported);
QJsonValue Write(const ArchiveSource &value);
bool Read(const QJsonValue &json, ArchiveSource &value, bool &unsupported);
QJsonValue Write(const ArchiveObservation &value);
bool Read(const QJsonValue &json, ArchiveObservation &value, bool &unsupported);
QJsonValue Write(const ArchiveMediaReference &value);
bool Read(const QJsonValue &json, ArchiveMediaReference &value, bool &unsupported);
QJsonValue Write(const ArchiveMissingSlot &value);
bool Read(const QJsonValue &json, ArchiveMissingSlot &value, bool &unsupported);
QJsonValue Write(const ArchiveMissingSlots &value);
bool Read(const QJsonValue &json, ArchiveMissingSlots &value, bool &unsupported);
QJsonValue Write(const ArchiveVersion &value);
bool Read(const QJsonValue &json, ArchiveVersion &value, bool &unsupported);
QJsonValue Write(const ArchiveEventReceipt &value);
bool Read(const QJsonValue &json, ArchiveEventReceipt &value, bool &unsupported);
QJsonValue Write(const ArchiveMessage &value);
bool Read(const QJsonValue &json, ArchiveMessage &value, bool &unsupported);
QJsonValue Write(const ArchiveChatGeneration &value);
bool Read(const QJsonValue &json, ArchiveChatGeneration &value, bool &unsupported);
QJsonValue Write(const ArchiveBlob &value);
bool Read(const QJsonValue &json, ArchiveBlob &value, bool &unsupported);
QJsonValue Write(const ArchiveMediaJob &value);
bool Read(const QJsonValue &json, ArchiveMediaJob &value, bool &unsupported);
QJsonValue Write(const ArchiveReaderLease &value);
bool Read(const QJsonValue &json, ArchiveReaderLease &value, bool &unsupported);
QJsonValue Write(const ArchiveState &value);
bool Read(const QJsonValue &json, ArchiveState &value, bool &unsupported);

QJsonValue Write(const QString &value) { return value; }
QJsonValue Write(const QByteArray &value) { return QString::fromLatin1(value.toBase64()); }
QJsonValue Write(const QJsonObject &value) { return value; }
QJsonValue Write(const QJsonArray &value) { return value; }
QJsonValue Write(const QJsonValue &value) { return value; }
QJsonValue Write(bool value) { return value; }
QJsonValue Write(int value) { return value; }
QJsonValue Write(int64 value) { return QString::number(value); }
QJsonValue Write(uint64_t value) { return QString::number(value); }
QJsonValue Write(const AccountKey &value) {
	return AccountKeyText(value.environment, value.userId).value_or(QString());
}
QJsonValue Write(const ArchiveGeneration &value) {
	return QJsonArray{ Write(value.account), Write(value.chat) };
}

template <typename T> requires std::is_enum_v<T>
QJsonValue Write(T value) { return int(value); }

template <typename T>
QJsonValue Write(const std::optional<T> &value) {
	return value ? Write(*value) : QJsonValue(QJsonValue::Null);
}

template <typename T>
QJsonValue Write(const std::vector<T> &value) {
	auto array = QJsonArray();
	for (const auto &entry : value) {
		array.push_back(Write(entry));
	}
	return array;
}

bool Read(const QJsonValue &json, QString &value, bool &) {
	if (!json.isString()) { return false; }
	value = json.toString();
	return true;
}

bool Read(const QJsonValue &json, QByteArray &value, bool &) {
	if (!json.isString()) { return false; }
	const auto text = json.toString();
	const auto latin = text.toLatin1();
	if (QString::fromLatin1(latin) != text) { return false; }
	const auto result = QByteArray::fromBase64Encoding(latin, QByteArray::AbortOnBase64DecodingErrors);
	if (!result || result.decoded.toBase64() != latin) { return false; }
	value = result.decoded;
	return true;
}

bool Read(const QJsonValue &json, QJsonObject &value, bool &) {
	if (!json.isObject()) { return false; }
	value = json.toObject();
	return true;
}

bool Read(const QJsonValue &json, QJsonArray &value, bool &) {
	if (!json.isArray()) { return false; }
	value = json.toArray();
	return true;
}

bool Read(const QJsonValue &json, QJsonValue &value, bool &) {
	if (json.isUndefined()) { return false; }
	value = json;
	return true;
}

bool Read(const QJsonValue &json, bool &value, bool &) {
	if (!json.isBool()) { return false; }
	value = json.toBool();
	return true;
}

bool Read(const QJsonValue &json, int &value, bool &) {
	if (!json.isDouble()) { return false; }
	const auto number = json.toInteger(-1);
	if (number < 0 || number > std::numeric_limits<int>::max()) { return false; }
	value = int(number);
	return true;
}

bool Read(const QJsonValue &json, int64 &value, bool &) {
	if (!json.isString()) { return false; }
	auto ok = false;
	value = json.toString().toLongLong(&ok);
	return ok && QString::number(value) == json.toString();
}

bool Read(const QJsonValue &json, uint64_t &value, bool &) {
	if (!json.isString()) { return false; }
	auto ok = false;
	value = json.toString().toULongLong(&ok);
	return ok && QString::number(value) == json.toString();
}

bool Read(const QJsonValue &json, AccountKey &value, bool &) {
	if (!json.isString()) { return false; }
	const auto result = ParseAccountKey(json.toString());
	if (!result) { return false; }
	value = *result;
	return true;
}

bool Read(const QJsonValue &json, ArchiveGeneration &value, bool &unsupported) {
	if (!json.isArray()) { return false; }
	const auto array = json.toArray();
	return array.size() == 2
		&& Read(array[0], value.account, unsupported)
		&& Read(array[1], value.chat, unsupported);
}

bool Read(const QJsonValue &json, ArchiveChatKind &value, bool &unsupported) {
	if (!json.isDouble()) { return false; }
	const auto number = json.toInteger(-1);
	if (number < 0) { return false; }
	if (number > int(ArchiveChatKind::Secret)) { unsupported = true; return false; }
	value = ArchiveChatKind(number);
	return true;
}

bool Read(const QJsonValue &json, ArchiveCollection &value, bool &unsupported) {
	if (!json.isDouble()) { return false; }
	const auto number = json.toInteger(-1);
	if (number < 0) { return false; }
	if (number > int(ArchiveCollection::QuickReply)) { unsupported = true; return false; }
	value = ArchiveCollection(number);
	return true;
}

bool Read(const QJsonValue &json, ArchiveOrigin &value, bool &unsupported) {
	if (!json.isDouble()) { return false; }
	const auto number = json.toInteger(-1);
	if (number < 0) { return false; }
	if (number > int(ArchiveOrigin::SendMetadata)) { unsupported = true; return false; }
	value = ArchiveOrigin(number);
	return true;
}

bool Read(const QJsonValue &json, ArchiveCompleteness &value, bool &unsupported) {
	if (!json.isDouble()) { return false; }
	const auto number = json.toInteger(-1);
	if (number < 0) { return false; }
	if (number > int(ArchiveCompleteness::Opaque)) { unsupported = true; return false; }
	value = ArchiveCompleteness(number);
	return true;
}

bool Read(const QJsonValue &json, ArchiveSourceKind &value, bool &unsupported) {
	if (!json.isDouble()) { return false; }
	const auto number = json.toInteger(-1);
	if (number < 0) { return false; }
	if (number > int(ArchiveSourceKind::ReceivedMarkdownFile)) { unsupported = true; return false; }
	value = ArchiveSourceKind(number);
	return true;
}

bool Read(const QJsonValue &json, ArchiveMediaState &value, bool &unsupported) {
	if (!json.isDouble()) { return false; }
	const auto number = json.toInteger(-1);
	if (number < 0) { return false; }
	if (number > int(ArchiveMediaState::Failed)) { unsupported = true; return false; }
	value = ArchiveMediaState(number);
	return true;
}

bool Read(const QJsonValue &json, ArchiveJobState &value, bool &unsupported) {
	if (!json.isDouble()) { return false; }
	const auto number = json.toInteger(-1);
	if (number < 0) { return false; }
	if (number > int(ArchiveJobState::Cancelled)) { unsupported = true; return false; }
	value = ArchiveJobState(number);
	return true;
}

bool Read(const QJsonValue &json, ArchiveBlobState &value, bool &unsupported) {
	if (!json.isDouble()) { return false; }
	const auto number = json.toInteger(-1);
	if (number < 0) { return false; }
	if (number > int(ArchiveBlobState::GarbagePending)) { unsupported = true; return false; }
	value = ArchiveBlobState(number);
	return true;
}

template <typename T>
bool Read(const QJsonValue &json, std::optional<T> &value, bool &unsupported) {
	if (json.isNull()) { value.reset(); return true; }
	auto entry = T();
	if (!Read(json, entry, unsupported)) { return false; }
	value = std::move(entry);
	return true;
}

template <typename T>
bool Read(const QJsonValue &json, std::vector<T> &value, bool &unsupported) {
	if (!json.isArray()) { return false; }
	for (const auto &entry : json.toArray()) {
		auto decoded = T();
		if (!Read(entry, decoded, unsupported)) { return false; }
		value.push_back(std::move(decoded));
	}
	return true;
}

class ObjectReader {
public:
	ObjectReader(const QJsonValue &json, bool &unsupported)
	: _object(json.toObject())
	, _unsupported(unsupported)
	, _valid(json.isObject()) {
	}

	template <typename T>
	void field(const QString &key, T &value) {
		if (_valid) {
			_valid = _object.contains(key) && Read(_object.take(key), value, _unsupported);
		}
	}

	template <typename T>
	void optionalField(const QString &key, T &value) {
		if (_object.contains(key)) { field(key, value); }
	}

	[[nodiscard]] bool ok() const { return _valid; }
	[[nodiscard]] QJsonObject remaining() { return std::move(_object); }

private:
	QJsonObject _object;
	bool &_unsupported;
	bool _valid = false;
};

QJsonValue Write(const ArchiveChatKey &value) {
	auto object = value.preserved;
	object.insert(u"kind"_q, Write(value.kind));
	object.insert(u"id"_q, Write(value.id));
	return object;
}

bool Read(const QJsonValue &json, ArchiveChatKey &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"kind"_q, value.kind);
	reader.field(u"id"_q, value.id);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveMessageKey &value) {
	auto object = value.preserved;
	object.insert(u"account"_q, Write(value.account));
	object.insert(u"chat"_q, Write(value.chat));
	object.insert(u"messageId"_q, Write(value.messageId));
	object.insert(u"collection"_q, Write(value.collection));
	object.insert(u"collectionId"_q, Write(value.collectionId));
	object.insert(u"representation"_q, Write(value.representation));
	return object;
}

bool Read(const QJsonValue &json, ArchiveMessageKey &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"account"_q, value.account);
	reader.field(u"chat"_q, value.chat);
	reader.field(u"messageId"_q, value.messageId);
	reader.field(u"collection"_q, value.collection);
	reader.field(u"collectionId"_q, value.collectionId);
	reader.field(u"representation"_q, value.representation);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveMediaDescriptor &value) {
	auto object = value.preserved;
	object.insert(u"identity"_q, Write(value.identity));
	object.insert(u"kind"_q, Write(value.kind));
	object.insert(u"attributes"_q, Write(value.attributes));
	return object;
}

bool Read(const QJsonValue &json, ArchiveMediaDescriptor &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"identity"_q, value.identity);
	reader.field(u"kind"_q, value.kind);
	reader.field(u"attributes"_q, value.attributes);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveContent &value) {
	auto object = value.preserved;
	object.insert(u"text"_q, Write(value.text));
	object.insert(u"caption"_q, Write(value.caption));
	object.insert(u"entities"_q, Write(value.entities));
	object.insert(u"rich"_q, Write(value.rich));
	object.insert(u"media"_q, Write(value.media));
	object.insert(u"opaqueFormat"_q, Write(value.opaqueFormat));
	object.insert(u"opaque"_q, Write(value.opaque));
	return object;
}

bool Read(const QJsonValue &json, ArchiveContent &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"text"_q, value.text);
	reader.field(u"caption"_q, value.caption);
	reader.field(u"entities"_q, value.entities);
	reader.field(u"rich"_q, value.rich);
	reader.field(u"media"_q, value.media);
	reader.field(u"opaqueFormat"_q, value.opaqueFormat);
	reader.field(u"opaque"_q, value.opaque);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveSourceBinding &value) {
	return ArchiveInternal::SourceBindingObject(value);
}

bool Read(const QJsonValue &json, ArchiveSourceBinding &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"key"_q, value.key);
	reader.field(u"version"_q, value.version);
	reader.optionalField(u"localMessageId"_q, value.localMessageId);
	reader.optionalField(u"randomId"_q, value.randomId);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveSource &value) {
	auto object = value.preserved;
	object.insert(u"kind"_q, Write(value.kind));
	object.insert(u"contentHash"_q, Write(value.contentHash));
	object.insert(u"representation"_q, Write(value.representation));
	object.insert(u"bytes"_q, Write(value.bytes));
	object.insert(u"revision"_q, Write(value.revision));
	if (value.binding) { object.insert(u"binding"_q, Write(value.binding)); }
	return object;
}

bool Read(const QJsonValue &json, ArchiveSource &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"kind"_q, value.kind);
	reader.field(u"contentHash"_q, value.contentHash);
	reader.field(u"representation"_q, value.representation);
	reader.field(u"bytes"_q, value.bytes);
	reader.field(u"revision"_q, value.revision);
	reader.optionalField(u"binding"_q, value.binding);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveObservation &value) {
	auto object = value.preserved;
	object.insert(u"observedAt"_q, Write(value.observedAt));
	object.insert(u"serverTime"_q, Write(value.serverTime));
	object.insert(u"topicId"_q, Write(value.topicId));
	object.insert(u"representation"_q, Write(value.representation));
	object.insert(u"context"_q, Write(value.context));
	object.insert(u"transport"_q, Write(value.transport));
	object.insert(u"payloadFormat"_q, Write(value.payloadFormat));
	object.insert(u"payload"_q, Write(value.payload));
	return object;
}

bool Read(const QJsonValue &json, ArchiveObservation &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"observedAt"_q, value.observedAt);
	reader.field(u"serverTime"_q, value.serverTime);
	reader.field(u"topicId"_q, value.topicId);
	reader.field(u"representation"_q, value.representation);
	reader.field(u"context"_q, value.context);
	reader.field(u"transport"_q, value.transport);
	reader.field(u"payloadFormat"_q, value.payloadFormat);
	reader.field(u"payload"_q, value.payload);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveMediaReference &value) {
	auto object = value.preserved;
	object.insert(u"identity"_q, Write(value.identity));
	object.insert(u"state"_q, Write(value.state));
	object.insert(u"blobId"_q, Write(value.blobId));
	return object;
}

bool Read(const QJsonValue &json, ArchiveMediaReference &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"identity"_q, value.identity);
	reader.field(u"state"_q, value.state);
	reader.field(u"blobId"_q, value.blobId);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveMissingSlot &value) {
	auto object = value.preserved;
	object.insert(u"path"_q, Write(value.path));
	return object;
}

bool Read(const QJsonValue &json, ArchiveMissingSlot &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"path"_q, value.path);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveMissingSlots &value) {
	auto object = value.preserved;
	object.insert(u"revision"_q, Write(value.revision));
	object.insert(u"content"_q, Write(value.content));
	object.insert(u"media"_q, Write(value.media));
	return object;
}

bool Read(const QJsonValue &json, ArchiveMissingSlots &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"revision"_q, value.revision);
	reader.field(u"content"_q, value.content);
	reader.field(u"media"_q, value.media);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveVersion &value) {
	auto object = value.preserved;
	object.insert(u"id"_q, Write(value.id));
	object.insert(u"order"_q, Write(value.order));
	object.insert(u"revision"_q, Write(value.revision));
	object.insert(u"origin"_q, Write(value.origin));
	object.insert(u"firstObservedAt"_q, Write(value.firstObservedAt));
	object.insert(u"content"_q, Write(value.content));
	object.insert(u"missing"_q, Write(value.missing));
	object.insert(u"completeness"_q, Write(value.completeness));
	object.insert(u"observation"_q, Write(value.observation));
	object.insert(u"source"_q, Write(value.source));
	object.insert(u"media"_q, Write(value.media));
	object.insert(u"missingPreimage"_q, Write(value.missingPreimage));
	return object;
}

bool Read(const QJsonValue &json, ArchiveVersion &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"id"_q, value.id);
	reader.field(u"order"_q, value.order);
	reader.field(u"revision"_q, value.revision);
	reader.field(u"origin"_q, value.origin);
	reader.field(u"firstObservedAt"_q, value.firstObservedAt);
	reader.field(u"content"_q, value.content);
	reader.field(u"missing"_q, value.missing);
	reader.field(u"completeness"_q, value.completeness);
	reader.field(u"observation"_q, value.observation);
	reader.optionalField(u"source"_q, value.source);
	reader.field(u"media"_q, value.media);
	reader.field(u"missingPreimage"_q, value.missingPreimage);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveEventReceipt &value) {
	auto object = value.preserved;
	object.insert(u"id"_q, Write(value.id));
	object.insert(u"fingerprint"_q, Write(value.fingerprint));
	return object;
}

bool Read(const QJsonValue &json, ArchiveEventReceipt &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"id"_q, value.id);
	reader.field(u"fingerprint"_q, value.fingerprint);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveMessage &value) {
	auto object = value.preserved;
	object.insert(u"key"_q, Write(value.key));
	object.insert(u"migrationAliases"_q, Write(value.migrationAliases));
	object.insert(u"versions"_q, Write(value.versions));
	object.insert(u"receipts"_q, Write(value.receipts));
	object.insert(u"order"_q, Write(value.order));
	object.insert(u"deleted"_q, Write(value.deleted));
	object.insert(u"payloadPurged"_q, Write(value.payloadPurged));
	object.insert(u"deletionOrigin"_q, Write(value.deletionOrigin));
	object.insert(u"deletedObservedAt"_q, Write(value.deletedObservedAt));
	return object;
}

bool Read(const QJsonValue &json, ArchiveMessage &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"key"_q, value.key);
	reader.field(u"migrationAliases"_q, value.migrationAliases);
	reader.field(u"versions"_q, value.versions);
	reader.field(u"receipts"_q, value.receipts);
	reader.field(u"order"_q, value.order);
	reader.field(u"deleted"_q, value.deleted);
	reader.field(u"payloadPurged"_q, value.payloadPurged);
	reader.field(u"deletionOrigin"_q, value.deletionOrigin);
	reader.field(u"deletedObservedAt"_q, value.deletedObservedAt);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveChatGeneration &value) {
	auto object = value.preserved;
	object.insert(u"chat"_q, Write(value.chat));
	object.insert(u"generation"_q, Write(value.generation));
	return object;
}

bool Read(const QJsonValue &json, ArchiveChatGeneration &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"chat"_q, value.chat);
	reader.field(u"generation"_q, value.generation);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveBlob &value) {
	auto object = value.preserved;
	object.insert(u"id"_q, Write(value.id));
	object.insert(u"size"_q, Write(value.size));
	object.insert(u"state"_q, Write(value.state));
	return object;
}

bool Read(const QJsonValue &json, ArchiveBlob &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"id"_q, value.id);
	reader.field(u"size"_q, value.size);
	reader.field(u"state"_q, value.state);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveMediaJob &value) {
	auto object = value.preserved;
	object.insert(u"id"_q, Write(value.id));
	object.insert(u"key"_q, Write(value.key));
	object.insert(u"version"_q, Write(value.version));
	object.insert(u"mediaIdentity"_q, Write(value.mediaIdentity));
	object.insert(u"generation"_q, Write(value.generation));
	object.insert(u"expectedSize"_q, Write(value.expectedSize));
	object.insert(u"expectedHash"_q, Write(value.expectedHash));
	object.insert(u"state"_q, Write(value.state));
	object.insert(u"blobId"_q, Write(value.blobId));
	object.insert(u"completedSize"_q, Write(value.completedSize));
	object.insert(u"failure"_q, Write(value.failure));
	return object;
}

bool Read(const QJsonValue &json, ArchiveMediaJob &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"id"_q, value.id);
	reader.field(u"key"_q, value.key);
	reader.field(u"version"_q, value.version);
	reader.field(u"mediaIdentity"_q, value.mediaIdentity);
	reader.field(u"generation"_q, value.generation);
	reader.field(u"expectedSize"_q, value.expectedSize);
	reader.field(u"expectedHash"_q, value.expectedHash);
	reader.field(u"state"_q, value.state);
	reader.field(u"blobId"_q, value.blobId);
	reader.field(u"completedSize"_q, value.completedSize);
	reader.field(u"failure"_q, value.failure);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveReaderLease &value) {
	auto object = value.preserved;
	object.insert(u"id"_q, Write(value.id));
	object.insert(u"key"_q, Write(value.key));
	object.insert(u"version"_q, Write(value.version));
	object.insert(u"blobId"_q, Write(value.blobId));
	object.insert(u"generation"_q, Write(value.generation));
	object.insert(u"revoked"_q, Write(value.revoked));
	return object;
}

bool Read(const QJsonValue &json, ArchiveReaderLease &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"id"_q, value.id);
	reader.field(u"key"_q, value.key);
	reader.field(u"version"_q, value.version);
	reader.field(u"blobId"_q, value.blobId);
	reader.field(u"generation"_q, value.generation);
	reader.field(u"revoked"_q, value.revoked);
	value.preserved = reader.remaining();
	return reader.ok();
}

QJsonValue Write(const ArchiveState &value) {
	auto object = value.preserved;
	object.insert(u"version"_q, Write(value.version));
	object.insert(u"account"_q, Write(value.account));
	object.insert(u"revision"_q, Write(value.revision));
	object.insert(u"accountGeneration"_q, Write(value.accountGeneration));
	object.insert(u"chatGenerations"_q, Write(value.chatGenerations));
	object.insert(u"messages"_q, Write(value.messages));
	object.insert(u"blobs"_q, Write(value.blobs));
	object.insert(u"jobs"_q, Write(value.jobs));
	object.insert(u"leases"_q, Write(value.leases));
	return object;
}

bool Read(const QJsonValue &json, ArchiveState &value, bool &unsupported) {
	auto reader = ObjectReader(json, unsupported);
	reader.field(u"version"_q, value.version);
	reader.field(u"account"_q, value.account);
	reader.field(u"revision"_q, value.revision);
	reader.field(u"accountGeneration"_q, value.accountGeneration);
	reader.field(u"chatGenerations"_q, value.chatGenerations);
	reader.field(u"messages"_q, value.messages);
	reader.field(u"blobs"_q, value.blobs);
	reader.field(u"jobs"_q, value.jobs);
	reader.field(u"leases"_q, value.leases);
	value.preserved = reader.remaining();
	return reader.ok();
}

}

bool ArchiveInternal::IsCodecFields(const ArchiveState &state) {
	auto decoded = ArchiveState();
	auto unsupported = false;
	if (state.preserved.contains(u"format"_q)
		|| !Read(Write(state), decoded, unsupported) || decoded != state) {
		return false;
	}
	for (const auto &message : state.messages) {
		for (const auto &version : message.versions) {
			if (!EncodeArchiveVersionSnapshot({ message.key, version })) {
				return false;
			}
		}
	}
	return true;
}

namespace {

bool IsVersionSnapshot(const ArchiveVersionSnapshot &snapshot) {
	const auto &key = snapshot.key;
	const auto &version = snapshot.version;
	if (!IsArchiveMessageKey(key) || !IsArchiveToken(version.id)
		|| !IsArchiveToken(version.revision)
		|| version.firstObservedAt < 0
		|| (version.origin != ArchiveOrigin::Baseline && version.origin != ArchiveOrigin::AuthoritativeEdit)
		|| !ArchiveInternal::IsContent(version.content)
		|| !ArchiveInternal::IsMissingSlots(version.missing, version.content, version.completeness, version.revision)
		|| !ArchiveInternal::IsObservation(version.observation)
		|| !ArchiveInternal::IsContentCompleteness(version.content, version.completeness)
		|| version.observation.representation != key.representation
		|| (version.source && (!ArchiveInternal::IsSource(*version.source)
			|| !ArchiveInternal::SourceOwnedBy(*version.source, key, version.id)))
		|| version.media.size() != version.content.media.size()
		|| snapshot.preserved.contains(u"format"_q) || snapshot.preserved.contains(u"schema"_q)
		|| snapshot.preserved.contains(u"key"_q) || snapshot.preserved.contains(u"snapshot"_q)) {
		return false;
	}
	auto ids = std::vector<QString>();
	for (const auto &ref : version.media) {
		if (std::find(ids.begin(), ids.end(), ref.identity) != ids.end()
			|| std::none_of(version.content.media.begin(), version.content.media.end(),
				[&](const auto &media) { return media.identity == ref.identity; })
			|| int(ref.state) < 0 || int(ref.state) > int(ArchiveMediaState::Failed)
			|| (ref.state == ArchiveMediaState::Complete ? !IsArchiveHash(ref.blobId) : !ref.blobId.isEmpty())) {
			return false;
		}
		ids.push_back(ref.identity);
	}
	auto decodedKey = ArchiveMessageKey();
	auto decodedVersion = ArchiveVersion();
	auto unsupported = false;
	return Read(Write(key), decodedKey, unsupported) && decodedKey == key
		&& Read(Write(version), decodedVersion, unsupported) && decodedVersion == version;
}

}

std::optional<QByteArray> EncodeArchiveVersionSnapshot(const ArchiveVersionSnapshot &snapshot) {
	if (!IsVersionSnapshot(snapshot)) {
		return std::nullopt;
	}
	auto object = snapshot.preserved;
	object.insert(u"format"_q, u"purple-archive-version"_q);
	object.insert(u"schema"_q, kArchiveSchemaVersion);
	object.insert(u"key"_q, Write(snapshot.key));
	object.insert(u"snapshot"_q, Write(snapshot.version));
	const auto canonical = CanonicalizeSyncJson(QJsonDocument(object).toJson(QJsonDocument::Compact));
	return canonical ? std::optional(canonical.canonical) : std::nullopt;
}

ArchiveVersionDecodeResult DecodeArchiveVersionSnapshot(const QByteArray &bytes) {
	auto result = ArchiveVersionDecodeResult();
	result.opaque = bytes;
	const auto canonical = CanonicalizeSyncJson(bytes);
	if (!canonical) {
		if (canonical.error == SyncJsonErrorKind::NonInteger
			|| canonical.error == SyncJsonErrorKind::NumberRange) {
			result.status = ArchiveCodecStatus::Unsupported;
		}
		result.error = u"Invalid or unsupported archive version JSON"_q;
		return result;
	}
	auto object = QJsonDocument::fromJson(canonical.canonical).object();
	const auto schema = object.take(u"schema"_q);
	if (object.take(u"format"_q) != QJsonValue(u"purple-archive-version"_q)
		|| !schema.isDouble() || schema.toInteger(-1) < 1) {
		result.error = u"Invalid archive version format or schema"_q;
		return result;
	} else if (schema.toInteger() > kArchiveSchemaVersion) {
		result.status = ArchiveCodecStatus::NewerSchema;
		return result;
	}
	auto snapshot = ArchiveVersionSnapshot();
	auto unsupported = false;
	if (!object.contains(u"key"_q) || !Read(object.take(u"key"_q), snapshot.key, unsupported)
		|| !object.contains(u"snapshot"_q) || !Read(object.take(u"snapshot"_q), snapshot.version, unsupported)) {
		result.status = unsupported ? ArchiveCodecStatus::Unsupported : ArchiveCodecStatus::Invalid;
		result.error = u"Invalid or unsupported archive version fields"_q;
		return result;
	}
	snapshot.preserved = object;
	if (!IsVersionSnapshot(snapshot)) {
		result.error = u"Inconsistent archive version fields"_q;
		return result;
	}
	result.status = ArchiveCodecStatus::Valid;
	result.snapshot = std::move(snapshot);
	return result;
}

ArchiveDecodeResult DecodeArchiveSnapshot(const QByteArray &bytes) {
	auto result = ArchiveDecodeResult();
	result.opaque = bytes;
	if (bytes.size() > kArchiveCodecByteLimit) {
		result.error = u"Archive snapshot exceeds the byte limit"_q;
		return result;
	}
	const auto canonical = CanonicalizeSyncJson(bytes);
	if (!canonical) {
		result.error = u"Archive snapshot is not strict integer JSON"_q;
		if (canonical.error == SyncJsonErrorKind::NonInteger
			|| canonical.error == SyncJsonErrorKind::NumberRange) {
			result.status = ArchiveCodecStatus::Unsupported;
		}
		return result;
	}
	const auto document = QJsonDocument::fromJson(canonical.canonical);
	if (!document.isObject()) {
		result.error = u"Archive snapshot must be an object"_q;
		return result;
	}
	auto object = document.object();
	const auto version = object.value(u"version"_q);
	if (object.take(u"format"_q) != QJsonValue(u"purple-archive"_q)
		|| !version.isDouble() || version.toInteger(-1) < 1) {
		result.error = u"Invalid archive format or schema version"_q;
		return result;
	} else if (version.toInteger() > kArchiveSchemaVersion) {
		result.status = ArchiveCodecStatus::NewerSchema;
		return result;
	}
	auto state = ArchiveState();
	auto unsupported = false;
	if (!Read(object, state, unsupported) || !IsArchiveState(state)) {
		result.status = unsupported ? ArchiveCodecStatus::Unsupported : ArchiveCodecStatus::Invalid;
		result.error = u"Invalid or unsupported archive fields"_q;
		return result;
	}
	result.status = ArchiveCodecStatus::Valid;
	result.state = std::move(state);
	return result;
}

std::optional<QByteArray> EncodeArchiveSnapshot(const ArchiveState &state) {
	if (!IsArchiveState(state)) {
		return std::nullopt;
	}
	auto object = Write(state).toObject();
	object.insert(u"format"_q, u"purple-archive"_q);
	const auto canonical = CanonicalizeSyncJson(QJsonDocument(object).toJson(QJsonDocument::Compact));
	if (!canonical || canonical.canonical.size() > kArchiveCodecByteLimit) {
		return std::nullopt;
	}
	return canonical.canonical;
}

ArchiveRecordKind ArchiveRowKind(const ArchiveRow &row) {
	return std::visit([](const auto &value) {
		using T = std::decay_t<decltype(value)>;
		if constexpr (std::is_same_v<T, ArchiveState>) { return ArchiveRecordKind::Account; }
		else if constexpr (std::is_same_v<T, ArchiveMessage>) { return ArchiveRecordKind::Message; }
		else if constexpr (std::is_same_v<T, ArchiveVersionSnapshot>) { return ArchiveRecordKind::Version; }
		else if constexpr (std::is_same_v<T, ArchiveEventReceipt>) { return ArchiveRecordKind::Event; }
		else if constexpr (std::is_same_v<T, ArchiveBlob>) { return ArchiveRecordKind::Blob; }
		else if constexpr (std::is_same_v<T, ArchiveMediaJob>) { return ArchiveRecordKind::Job; }
		else if constexpr (std::is_same_v<T, ArchiveReaderLease>) { return ArchiveRecordKind::Lease; }
		else { return ArchiveRecordKind::Generation; }
	}, row.value);
}

std::optional<QByteArray> EncodeArchiveRow(const ArchiveRow &row) {
	if (!row.preserved.isEmpty()) {
		return std::nullopt;
	}
	auto object = row.preserved;
	object.insert(u"format"_q, row.body ? u"purple-archive-body-row"_q : u"purple-archive-row"_q);
	if (row.body) {
		if (ArchiveRowKind(row) != ArchiveRecordKind::Version || !IsArchiveHash(row.body->hash)
			|| row.body->bytes < 1 || row.body->bytes > kArchiveCodecByteLimit) { return {}; }
		object.insert(u"bodyHash"_q, row.body->hash);
		object.insert(u"bodyBytes"_q, QString::number(row.body->bytes));
	}
	object.insert(u"schema"_q, 1);
	object.insert(u"kind"_q, int(ArchiveRowKind(row)));
	auto valid = true;
	object.insert(u"value"_q, std::visit([&](const auto &value) -> QJsonValue {
		using T = std::decay_t<decltype(value)>;
		if constexpr (std::is_same_v<T, ArchiveVersionSnapshot>) {
			const auto bytes = EncodeArchiveVersionSnapshot(value);
			valid = bytes.has_value();
			return bytes ? QJsonDocument::fromJson(*bytes).object() : QJsonObject();
		} else {
			auto decoded = T();
			auto unsupported = false;
			const auto json = Write(value);
			valid = Read(json, decoded, unsupported) && decoded == value;
			return json;
		}
	}, row.value));
	if (!valid) { return std::nullopt; }
	const auto encoded = CanonicalizeSyncJson(QJsonDocument(object).toJson(QJsonDocument::Compact));
	if (!encoded || encoded.canonical.size() > kArchiveCodecByteLimit) { return std::nullopt; }
	return encoded.canonical;
}

ArchiveRowDecodeResult DecodeArchiveRow(const QByteArray &bytes) {
	if (bytes.size() > kArchiveCodecByteLimit) { return {}; }
	const auto canonical = CanonicalizeSyncJson(bytes);
	if (!canonical) {
		return { (canonical.error == SyncJsonErrorKind::NonInteger
			|| canonical.error == SyncJsonErrorKind::NumberRange)
			? ArchiveCodecStatus::Unsupported : ArchiveCodecStatus::Invalid, {} };
	}
	auto object = QJsonDocument::fromJson(canonical.canonical).object();
	const auto schema = object.take(u"schema"_q);
	const auto format = object.take(u"format"_q);
	const auto hasBody = format == QJsonValue(u"purple-archive-body-row"_q);
	if ((!hasBody && format != QJsonValue(u"purple-archive-row"_q))
		|| !schema.isDouble() || schema.toInteger(-1) < 1) { return {}; }
	if (schema.toInteger() > 1) { return { ArchiveCodecStatus::NewerSchema, {} }; }
	const auto kind = object.take(u"kind"_q);
	if (!kind.isDouble()) { return {}; }
	const auto payload = object.take(u"value"_q);
	auto row = ArchiveRow();
	if (hasBody) {
		const auto hash = object.take(u"bodyHash"_q);
		const auto bytes = object.take(u"bodyBytes"_q);
		auto validBytes = false;
		const auto size = bytes.toString().toLongLong(&validBytes);
		if (kind.toInteger(-1) != int(ArchiveRecordKind::Version) || !hash.isString() || !IsArchiveHash(hash.toString())
			|| !bytes.isString() || !validBytes || QString::number(size) != bytes.toString()
			|| size < 1 || size > kArchiveCodecByteLimit) { return {}; }
		row.body = ArchiveVersionBody{ hash.toString(), size };
	}
	auto unsupported = false;
	auto valid = false;
	const auto read = [&]<typename T>() {
		auto value = T();
		valid = Read(payload, value, unsupported);
		row.value = std::move(value);
	};
	switch (kind.toInteger(-1)) {
	case int(ArchiveRecordKind::Account): read.operator()<ArchiveState>(); break;
	case int(ArchiveRecordKind::Message): read.operator()<ArchiveMessage>(); break;
	case int(ArchiveRecordKind::Version): {
		const auto decoded = DecodeArchiveVersionSnapshot(QJsonDocument(payload.toObject()).toJson(QJsonDocument::Compact));
		if (!decoded.snapshot) { return { decoded.status, {} }; }
		row.value = *decoded.snapshot;
		valid = true;
	} break;
	case int(ArchiveRecordKind::Event): read.operator()<ArchiveEventReceipt>(); break;
	case int(ArchiveRecordKind::Blob): read.operator()<ArchiveBlob>(); break;
	case int(ArchiveRecordKind::Job): read.operator()<ArchiveMediaJob>(); break;
	case int(ArchiveRecordKind::Lease): read.operator()<ArchiveReaderLease>(); break;
	case int(ArchiveRecordKind::Generation): read.operator()<ArchiveChatGeneration>(); break;
	default: return { ArchiveCodecStatus::Unsupported, {} };
	}
	if (!valid) { return { unsupported ? ArchiveCodecStatus::Unsupported : ArchiveCodecStatus::Invalid, {} }; }
	if (!object.isEmpty()) { return { ArchiveCodecStatus::Unsupported, {} }; }
	return { ArchiveCodecStatus::Valid, std::move(row) };
}

}
