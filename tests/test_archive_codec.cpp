/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "tests/archive_test_support.h"

using namespace ArchiveTest;

namespace {

void TestCodec() {
	Section = "codec";
	auto state = Empty({ 1, std::numeric_limits<int64>::max() });
	auto key = Key(state.account, std::numeric_limits<int64>::max(),
		std::numeric_limits<int64>::min(), ArchiveChatKind::Secret);
	auto event = Event(state, u"v1"_q, std::numeric_limits<uint64_t>::max(),
		u"large identity"_q, ArchiveOrigin::Baseline, key);
	event.content->opaqueFormat = u"unknown-rich:v9"_q;
	event.content->opaque = QByteArray::fromHex("00ff1020");
	event.completeness = ArchiveCompleteness::Opaque;
	event.content->preserved.insert(u"futureBody"_q, u"keep"_q);
	event.key.preserved.insert(u"futureKey"_q, true);
	event.key.chat.preserved.insert(u"futureChat"_q, QJsonObject{ { u"x"_q, 3 } });
	event.observation.preserved.insert(u"futureObservation"_q, QJsonArray{ 1, 2, 3 });
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	state.accountGeneration = std::numeric_limits<uint64_t>::max();
	state.revision = std::numeric_limits<uint64_t>::max();
	state.preserved.insert(u"futureRoot"_q, QJsonObject{ { u"opaque"_q, u"exact"_q } });
	state.messages[0].preserved.insert(u"futureMessage"_q, 123);
	state.messages[0].versions[0].preserved.insert(u"futureVersion"_q, false);
	state.messages[0].receipts[0].preserved.insert(u"futureReceipt"_q, u"keep"_q);
	RoundTrip(state);
	const auto encoded = EncodeArchiveSnapshot(state);
	CHECK(encoded);
	CHECK(encoded->contains("\"messageId\":\"-9223372036854775808\""));
	CHECK(encoded->contains("\"order\":\"18446744073709551615\""));
	CHECK(encoded->contains("9223372036854775807"));
	const auto decoded = DecodeArchiveSnapshot(*encoded);
	CHECK(decoded.state && decoded.state->messages[0].key.messageId == std::numeric_limits<int64>::min());
	CHECK(decoded.state && decoded.state->messages[0].versions[0].content.opaque == event.content->opaque);

	auto object = QJsonDocument::fromJson(*encoded).object();
	for (const auto version : { qint64(2), qint64(2147483648LL), qint64(9007199254740991LL) }) {
		auto future = object;
		future.insert(u"version"_q, version);
		const auto raw = QJsonDocument(future).toJson();
		const auto parsed = DecodeArchiveSnapshot(raw);
		CHECK(parsed.status == ArchiveCodecStatus::NewerSchema);
		CHECK(!parsed.state);
		CHECK(parsed.opaque == raw);
	}
	auto futureEnum = object;
	auto messages = futureEnum.value(u"messages"_q).toArray();
	auto message = messages[0].toObject();
	auto futureKey = message.value(u"key"_q).toObject();
	futureKey.insert(u"collection"_q, 19);
	message.insert(u"key"_q, futureKey);
	messages[0] = message;
	futureEnum.insert(u"messages"_q, messages);
	const auto futureRaw = QJsonDocument(futureEnum).toJson();
	const auto unknown = DecodeArchiveSnapshot(futureRaw);
	CHECK(unknown.status == ArchiveCodecStatus::Unsupported);
	CHECK(!unknown.state);
	CHECK(unknown.opaque == futureRaw);

	const auto Invalid = std::vector<QByteArray>{
		QByteArray(), QByteArray("[]"), QByteArray("{}"), QByteArray("{\"version\":1,\"version\":2}"),
		QByteArray("{\"x\":\"\\ud800\"}"), QByteArray("null"), QByteArray("{bad}"),
	};
	for (const auto &raw : Invalid) {
		const auto result = DecodeArchiveSnapshot(raw);
		CHECK(!result);
		CHECK(!result.state);
		CHECK(result.opaque == raw);
	}
	for (const auto &field : { u"version"_q, u"account"_q, u"revision"_q,
		u"accountGeneration"_q, u"messages"_q, u"blobs"_q, u"jobs"_q, u"leases"_q }) {
		auto broken = object;
		broken.remove(field);
		CHECK(!DecodeArchiveSnapshot(QJsonDocument(broken).toJson()));
	}
	for (const auto &value : { QJsonValue(123), QJsonValue(u"01"_q), QJsonValue(u"-1"_q),
		QJsonValue(u"18446744073709551616"_q), QJsonValue(u"+1"_q) }) {
		auto broken = object;
		broken.insert(u"revision"_q, value);
		CHECK(!DecodeArchiveSnapshot(QJsonDocument(broken).toJson()));
	}
	auto unknownFraction = object;
	unknownFraction.insert(u"futureRatio"_q, 0.5);
	const auto fractionRaw = QJsonDocument(unknownFraction).toJson();
	const auto fraction = DecodeArchiveSnapshot(fractionRaw);
	CHECK(fraction.status == ArchiveCodecStatus::Unsupported);
	CHECK(fraction.opaque == fractionRaw);
	CHECK(!fraction.state);
	CHECK(!DecodeArchiveSnapshot(QByteArray(kArchiveCodecByteLimit + 1, ' ')));
}

void TestMissingSlotCodec() {
	Section = "revision-bound missing slot codec";
	auto state = Empty();
	auto event = Event(state, u"partial"_q, 1);
	event.completeness = ArchiveCompleteness::Partial;
	AddMedia(event);
	event.missing.content = { { { u"rich"_q } } };
	event.missing.content[0].preserved.insert(u"futureSlot"_q, u"keep"_q);
	event.missing.media = { u"document:555"_q };
	event.missing.preserved.insert(u"futureMissing"_q, QJsonArray{ 1, 2 });
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	const auto &version = Message(state).versions[0];
	CHECK(version.missing.revision == event.id);
	CHECK(version.missing.preserved == event.missing.preserved);
	const auto bytes = EncodeArchiveVersionSnapshot({ Key(), version });
	CHECK(bytes);
	const auto decoded = DecodeArchiveVersionSnapshot(*bytes);
	CHECK(decoded.status == ArchiveCodecStatus::Valid);
	CHECK(decoded.snapshot && decoded.snapshot->version.missing == version.missing);
	RoundTrip(state);

	const auto invalid = [&](const ArchiveMissingSlots &missing) {
		auto broken = state;
		broken.messages[0].versions[0].missing = missing;
		CHECK(!IsArchiveState(broken));
		CHECK(!EncodeArchiveSnapshot(broken));
		CHECK(!EncodeArchiveVersionSnapshot({ Key(), broken.messages[0].versions[0] }));
	};
	auto missing = version.missing;
	missing.revision = u"later-revision"_q;
	invalid(missing);
	missing = version.missing;
	missing.content.push_back(missing.content[0]);
	invalid(missing);
	missing = version.missing;
	missing.content.push_back({ { u"rich"_q, u"article"_q } });
	invalid(missing);
	missing = version.missing;
	missing.content = { { { u"media"_q } } };
	invalid(missing);
	missing.content = { { {} } };
	invalid(missing);
	missing = version.missing;
	missing.media = { u"document:999"_q };
	invalid(missing);
	missing.media.push_back(u"document:999"_q);
	invalid(missing);
	missing = version.missing;
	missing.preserved.insert(u"revision"_q, u"collision"_q);
	invalid(missing);
	missing = version.missing;
	missing.content[0].preserved.insert(u"path"_q, u"collision"_q);
	invalid(missing);
	auto broken = state;
	broken.messages[0].versions[0].completeness = ArchiveCompleteness::Complete;
	CHECK(!IsArchiveState(broken));
	auto object = QJsonDocument::fromJson(*bytes).object();
	auto snapshot = object.value(u"snapshot"_q).toObject();
	auto wireMissing = snapshot.value(u"missing"_q).toObject();
	wireMissing.insert(u"revision"_q, u"other"_q);
	snapshot.insert(u"missing"_q, wireMissing);
	object.insert(u"snapshot"_q, snapshot);
	CHECK(DecodeArchiveVersionSnapshot(QJsonDocument(object).toJson()).status == ArchiveCodecStatus::Invalid);
	snapshot.remove(u"missing"_q);
	object.insert(u"snapshot"_q, snapshot);
	CHECK(DecodeArchiveVersionSnapshot(QJsonDocument(object).toJson()).status == ArchiveCodecStatus::Invalid);
}

void TestMalformedState() {
	Section = "malformed state";
	auto state = Empty();
	auto event = Event(state, u"baseline"_q, 1);
	AddMedia(event);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	auto broken = state;
	broken.messages.push_back(broken.messages[0]);
	CHECK(!IsArchiveState(broken));
	CHECK(!EncodeArchiveSnapshot(broken));
	broken = state;
	broken.messages[0].versions[0].media[0].state = ArchiveMediaState::Complete;
	broken.messages[0].versions[0].media[0].blobId = BlobId;
	CHECK(!IsArchiveState(broken));
	broken = state;
	broken.messages[0].payloadPurged = true;
	CHECK(!IsArchiveState(broken));
	broken = state;
	broken.messages[0].versions[0].observation.representation = u"translated"_q;
	CHECK(!IsArchiveState(broken));
	broken = state;
	broken.messages[0].versions[0].content.media.push_back(broken.messages[0].versions[0].content.media[0]);
	CHECK(!IsArchiveState(broken));
	broken = state;
	broken.messages[0].versions[0].content.entities = QJsonArray{ 0.5 };
	CHECK(!IsArchiveState(broken));
}

void TestVersionCodecAndAllUnknownFields() {
	Section = "version codec and unknown fields";
	auto state = Empty();
	auto event = Event(state, u"body"_q, 1);
	AddMedia(event);
	event.source = ArchiveSource{ ArchiveSourceKind::ReceivedMarkdownFile,
		ArchiveContentHash(*event.content), u"original"_q, QByteArray("A"), event.id };
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	CHECK(Apply(state, PlanArchiveMediaStage(state, Job(state, u"media"_q, u"body"_q))));
	CHECK(Apply(state, PlanArchiveMediaCompletion(state, u"media"_q, MediaSuccess())));
	auto lease = ArchiveReaderLease{ u"reader"_q, Key(), u"body"_q, BlobId };
	CHECK(Apply(state, PlanArchiveReaderAcquire(state, lease)));
	state.chatGenerations.push_back({ Key().chat, 0 });
	const auto unknown = QJsonObject{ { u"future"_q, QJsonArray{ true, u"preserve"_q, 41 } } };
	state.chatGenerations[0].preserved = unknown;
	state.blobs[0].preserved = unknown;
	state.jobs[0].preserved = unknown;
	state.leases[0].preserved = unknown;
	auto &version = state.messages[0].versions[0];
	version.content.media[0].preserved = unknown;
	version.media[0].preserved = unknown;
	version.source->preserved = unknown;
	version.preserved = unknown;
	CHECK(!ArchiveSourceMatches(version));
	version.source->contentHash = ArchiveContentHash(version.content);
	RoundTrip(state);
	const auto snapshot = ArchiveVersionSnapshot{ state.messages[0].key, version, unknown };
	const auto bytes = EncodeArchiveVersionSnapshot(snapshot);
	CHECK(bytes);
	const auto parsed = DecodeArchiveVersionSnapshot(*bytes);
	CHECK(parsed.status == ArchiveCodecStatus::Valid);
	CHECK(parsed.snapshot && *parsed.snapshot == snapshot);
	CHECK(parsed.snapshot && EncodeArchiveVersionSnapshot(*parsed.snapshot) == bytes);
	CHECK(parsed.snapshot && ArchiveSourceMatches(parsed.snapshot->version));
	CHECK(parsed.snapshot && parsed.snapshot->version.media[0].blobId == BlobId);
	auto object = QJsonDocument::fromJson(*bytes).object();
	object.insert(u"schema"_q, qint64(2147483648LL));
	const auto futureRaw = QJsonDocument(object).toJson();
	const auto future = DecodeArchiveVersionSnapshot(futureRaw);
	CHECK(future.status == ArchiveCodecStatus::NewerSchema);
	CHECK(!future.snapshot);
	CHECK(future.opaque == futureRaw);
	auto invalid = snapshot;
	invalid.version.preserved.insert(u"id"_q, u"collision"_q);
	CHECK(!EncodeArchiveVersionSnapshot(invalid));
	invalid = snapshot;
	invalid.preserved.insert(u"schema"_q, 1);
	CHECK(!EncodeArchiveVersionSnapshot(invalid));
	invalid = snapshot;
	invalid.version.media[0].blobId.clear();
	CHECK(!EncodeArchiveVersionSnapshot(invalid));
	invalid = snapshot;
	invalid.version.content.opaqueFormat = u"opaque:v1"_q;
	invalid.version.content.opaque = QByteArray(kArchiveCodecByteLimit, 'a');
	CHECK(!EncodeArchiveVersionSnapshot(invalid));
}

void TestCheckpointVersusVersionLimit() {
	Section = "checkpoint versus version size limit";
	auto state = Empty();
	auto event = Event(state, u"body"_q, 1, QString(100000, u'x'));
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	const auto message = state.messages[0];
	for (auto id = 1; id <= 45; ++id) {
		auto copy = message;
		copy.key.messageId += id;
		state.messages.push_back(std::move(copy));
	}
	CHECK(IsArchiveState(state));
	CHECK(!EncodeArchiveSnapshot(state));
	for (const auto &entry : state.messages) {
		const auto bytes = EncodeArchiveVersionSnapshot({ entry.key, entry.versions[0] });
		CHECK(bytes);
		const auto decoded = DecodeArchiveVersionSnapshot(*bytes);
		CHECK(decoded.snapshot && SameArchiveMessage(decoded.snapshot->key, entry.key));
	}
}

void TestInvalidRelationships() {
	Section = "invalid relationships";
	auto state = Empty();
	auto event = Event(state, u"body"_q, 1);
	AddMedia(event);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	auto broken = state;
	broken.messages[0].versions[0].media[0].state = ArchiveMediaState::Staged;
	CHECK(!IsArchiveState(broken));
	broken = state;
	broken.messages[0].versions[0].preserved.insert(u"content"_q, QJsonObject());
	CHECK(!IsArchiveState(broken));
	broken = state;
	broken.messages[0].key.preserved.insert(u"messageId"_q, u"67"_q);
	CHECK(!IsArchiveState(broken));
	broken = state;
	broken.preserved.insert(u"format"_q, u"purple-archive"_q);
	CHECK(!IsArchiveState(broken));
	broken = state;
	broken.messages[0].versions[0].observation.transport.insert(u"invalid"_q, 1.5);
	CHECK(!IsArchiveState(broken));
	broken = state;
	broken.messages[0].receipts.clear();
	CHECK(!IsArchiveState(broken));
	CHECK(Apply(state, PlanArchiveMediaStage(state, Job(state, u"job"_q, u"body"_q))));
	broken = state;
	broken.jobs.push_back(broken.jobs[0]);
	broken.jobs.back().id = u"duplicate-active-target"_q;
	CHECK(!IsArchiveState(broken));
	CHECK(Apply(state, PlanArchiveMediaCompletion(state, u"job"_q, MediaSuccess())));
	broken = state;
	broken.jobs[0].completedSize += 1;
	CHECK(!IsArchiveState(broken));
	broken = state;
	broken.blobs[0].state = ArchiveBlobState::GarbagePending;
	CHECK(!IsArchiveState(broken));
}

}

int main() {
	TestCodec();
	TestMalformedState();
	TestMissingSlotCodec();
	TestVersionCodecAndAllUnknownFields();
	TestCheckpointVersusVersionLimit();
	TestInvalidRelationships();
	return Finish();
}
