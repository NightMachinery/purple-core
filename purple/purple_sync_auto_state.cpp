/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_auto.h"

#include "purple/purple_config_payload.h"
#include "purple/purple_sync_json.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

#include <algorithm>
#include <limits>

namespace Purple {
namespace {

const auto Fields = std::vector<QString>{
	u"version"_q, u"account"_q, u"enabled"_q, u"event"_q, u"ticket"_q, u"run"_q, u"runStopped"_q,
	u"session"_q, u"active"_q, u"pending"_q, u"commitOnly"_q, u"lastCheck"_q,
	u"checked"_q, u"queuedCheck"_q, u"queuedSave"_q, u"savedFingerprint"_q,
	u"retries"_q, u"retryAt"_q, u"retired"_q, u"editRefused"_q, u"uncertain"_q,
	u"retiring"_q, u"held"_q, u"expectedState"_q, u"expectedFile"_q, u"record"_q,
	u"messageId"_q, u"noticed"_q,
};

bool Fingerprint(const QString &text) {
	return ParseConfigVersionKey(u"1."_q + text).has_value();
}

QJsonObject Object(const SyncAutoRecord &value) {
	auto object = value.preserved;
	object.insert(u"version"_q, value.version);
	object.insert(u"account"_q, AccountKeyText(value.account.environment, value.account.userId).value_or(QString()));
	object.insert(u"enabled"_q, value.enabled);
	object.insert(u"event"_q, QString::number(value.event));
	object.insert(u"ticket"_q, QString::number(value.ticket));
	object.insert(u"run"_q, QString::number(value.run));
	object.insert(u"runStopped"_q, value.runStopped);
	object.insert(u"session"_q, value.session);
	object.insert(u"active"_q, value.active);
	object.insert(u"pending"_q, int(value.pending));
	object.insert(u"commitOnly"_q, value.commitOnly);
	object.insert(u"lastCheck"_q, QString::number(value.lastCheck));
	object.insert(u"checked"_q, value.checked);
	object.insert(u"queuedCheck"_q, value.queuedCheck);
	object.insert(u"queuedSave"_q, value.queuedSave);
	object.insert(u"savedFingerprint"_q, value.savedFingerprint);
	object.insert(u"retries"_q, value.retries);
	object.insert(u"retryAt"_q, QString::number(value.retryAt));
	object.insert(u"retired"_q, value.retired);
	object.insert(u"editRefused"_q, value.editRefused);
	object.insert(u"uncertain"_q, value.uncertain);
	object.insert(u"retiring"_q, value.retiring);
	object.insert(u"held"_q, int(value.held));
	object.insert(u"expectedState"_q, QString::fromLatin1(value.expectedState.toBase64()));
	object.insert(u"expectedFile"_q, QString::fromLatin1(value.expectedFile.toBase64()));
	object.insert(u"record"_q, QString::fromLatin1(value.record.toBase64()));
	object.insert(u"messageId"_q, value.messageId);
	auto noticed = QJsonArray();
	for (const auto reason : value.noticed) { noticed.push_back(int(reason)); }
	object.insert(u"noticed"_q, noticed);
	return object;
}

class Reader {
public:
	explicit Reader(QJsonObject object) : _object(std::move(object)) {}
	void read(const QString &name, bool &value) {
		const auto json = _object.take(name);
		_ok = _ok && json.isBool();
		value = json.toBool();
	}
	void read(const QString &name, QString &value) {
		const auto json = _object.take(name);
		_ok = _ok && json.isString();
		value = json.toString();
	}
	void read(const QString &name, QByteArray &value) {
		auto text = QString();
		read(name, text);
		const auto decoded = QByteArray::fromBase64Encoding(text.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
		_ok = _ok && decoded && QString::fromLatin1(decoded.decoded.toBase64()) == text;
		value = decoded.decoded;
	}
	void read(const QString &name, int &value) {
		const auto json = _object.take(name);
		const auto number = json.toInteger(-1);
		_ok = _ok && json.isDouble() && number >= 0 && number <= std::numeric_limits<int>::max();
		value = int(number);
	}
	void read(const QString &name, uint64_t &value) {
		auto text = QString();
		read(name, text);
		auto ok = false;
		value = text.toULongLong(&ok);
		_ok = _ok && ok && QString::number(value) == text;
	}
	void read(const QString &name, int64_t &value) {
		auto text = QString();
		read(name, text);
		auto ok = false;
		value = text.toLongLong(&ok);
		_ok = _ok && ok && value >= 0 && QString::number(value) == text;
	}
	QJsonValue take(const QString &name) { return _object.take(name); }
	bool ok() const { return _ok; }
	QJsonObject remaining() const { return _object; }
private:
	QJsonObject _object;
	bool _ok = true;
};

}

bool IsSyncAutoRecord(const SyncAutoRecord &record) {
	if (record.version != kSyncAutoVersion || !AccountKeyText(record.account.environment, record.account.userId)
		|| int(record.pending) < 0 || int(record.pending) > int(SyncAutoAction::Retire)
		|| (record.active && (record.session.isEmpty() || !record.ticket || record.pending == SyncAutoAction::NoOp))
		|| int(record.held) < 0 || int(record.held) > int(SyncAutoReason::StateChanged)
		|| record.session.size() > 256 || (record.commitOnly && record.pending != SyncAutoAction::Reconcile)
		|| record.lastCheck < 0 || (!record.checked && record.lastCheck != 0)
		|| (record.runStopped && (record.active || record.retryAt || !record.run
			|| record.pending == SyncAutoAction::NoOp))
		|| (record.held == SyncAutoReason::RetryStopped && !record.runStopped)
		|| record.retryAt < 0 || (record.retryAt && (record.active || !record.retries))
		|| record.retries < 0 || record.retries > 3 || record.retired < 0 || record.retired > kSyncAutoRetireLimit
		|| (!record.savedFingerprint.isEmpty() && !Fingerprint(record.savedFingerprint))
		|| record.expectedState.size() > 1024 * 1024 || record.expectedFile.size() > kSyncSettingsMaximumBytes
		|| record.record.size() > kSyncConfigRecordMaximumBytes || record.messageId < 0
		|| (record.retiring && record.messageId <= 0)
		|| (!record.expectedState.isEmpty() && !ParseSyncLocalState(record.expectedState))
		|| (!record.record.isEmpty() && !InspectConfigPayload(ParseSyncEnvelope(record.record)))) {
		return false;
	}
	for (const auto &field : Fields) {
		if (record.preserved.contains(field)) { return false; }
	}
	for (auto i = size_t(0); i != record.noticed.size(); ++i) {
		const auto reason = record.noticed[i];
		if (reason == SyncAutoReason::None || int(reason) < 0 || int(reason) > int(SyncAutoReason::StateChanged)
			|| std::find(record.noticed.begin(), record.noticed.begin() + i, reason) != record.noticed.begin() + i) {
			return false;
		}
	}
	return bool(CanonicalizeSyncJson(QJsonDocument(Object(record)).toJson(QJsonDocument::Compact)));
}

std::optional<QByteArray> SerializeSyncAutoRecord(const SyncAutoRecord &record) {
	if (!IsSyncAutoRecord(record)) { return std::nullopt; }
	const auto json = CanonicalizeSyncJson(QJsonDocument(Object(record)).toJson(QJsonDocument::Compact));
	return json && json.canonical.size() <= kSyncAutoRecordLimit
		? std::make_optional(json.canonical) : std::nullopt;
}

SyncAutoDecodeResult ParseSyncAutoRecord(const QByteArray &bytes) {
	auto result = SyncAutoDecodeResult();
	result.opaque = bytes;
	if (bytes.size() > kSyncAutoRecordLimit) { return result; }
	const auto canonical = CanonicalizeSyncJson(bytes);
	if (!canonical) { return result; }
	const auto document = QJsonDocument::fromJson(canonical.canonical);
	if (!document.isObject()) { return result; }
	auto object = document.object();
	const auto version = object.value(u"version"_q);
	if (version.isDouble() && version.toInteger() > kSyncAutoVersion) {
		result.status = SyncAutoCodecStatus::NewerVersion;
		return result;
	}
	auto reader = Reader(object);
	auto record = SyncAutoRecord();
	reader.read(u"version"_q, record.version);
	auto account = QString();
	reader.read(u"account"_q, account);
	const auto parsedAccount = ParseAccountKey(account);
	if (!parsedAccount) { return result; }
	record.account = *parsedAccount;
	reader.read(u"enabled"_q, record.enabled);
	reader.read(u"event"_q, record.event);
	reader.read(u"ticket"_q, record.ticket);
	reader.read(u"run"_q, record.run);
	reader.read(u"runStopped"_q, record.runStopped);
	reader.read(u"session"_q, record.session);
	reader.read(u"active"_q, record.active);
	auto action = 0;
	reader.read(u"pending"_q, action);
	record.pending = SyncAutoAction(action);
	reader.read(u"commitOnly"_q, record.commitOnly);
	reader.read(u"lastCheck"_q, record.lastCheck);
	reader.read(u"checked"_q, record.checked);
	reader.read(u"queuedCheck"_q, record.queuedCheck);
	reader.read(u"queuedSave"_q, record.queuedSave);
	reader.read(u"savedFingerprint"_q, record.savedFingerprint);
	reader.read(u"retries"_q, record.retries);
	reader.read(u"retryAt"_q, record.retryAt);
	reader.read(u"retired"_q, record.retired);
	reader.read(u"editRefused"_q, record.editRefused);
	reader.read(u"uncertain"_q, record.uncertain);
	reader.read(u"retiring"_q, record.retiring);
	auto held = 0;
	reader.read(u"held"_q, held);
	record.held = SyncAutoReason(held);
	reader.read(u"expectedState"_q, record.expectedState);
	reader.read(u"expectedFile"_q, record.expectedFile);
	reader.read(u"record"_q, record.record);
	reader.read(u"messageId"_q, record.messageId);
	const auto noticed = reader.take(u"noticed"_q);
	if (!noticed.isArray()) { return result; }
	for (const auto &value : noticed.toArray()) {
		if (!value.isDouble() || value.toInteger(-1) < 0
			|| value.toInteger() > int(SyncAutoReason::StateChanged)) { return result; }
		record.noticed.push_back(SyncAutoReason(value.toInt()));
	}
	record.preserved = reader.remaining();
	if (!reader.ok() || !IsSyncAutoRecord(record)) { return result; }
	result.status = SyncAutoCodecStatus::Valid;
	result.record = std::move(record);
	result.opaque.clear();
	return result;
}

}
