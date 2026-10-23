/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "tests/archive_test_support.h"
#include "purple/purple_archive_source.h"

using namespace ArchiveTest;

namespace {

ArchiveSource Source(const ArchiveEvent &event,
		ArchiveSourceKind kind = ArchiveSourceKind::ComposeMarkdown) {
	auto result = ArchiveSource{ kind, ArchiveContentHash(*event.content),
		event.key.representation, QByteArray("**A**\r\n"), event.id };
	result.binding = ArchiveSourceBinding{ event.key, event.id, -100,
		std::numeric_limits<int64>::max() };
	return result;
}

ArchiveCopyRequest Copy(const ArchiveState &state,
		const ArchiveMessageKey &key = Key(), const QString &id = QString()) {
	const auto &message = Message(state, key);
	const auto version = id.isEmpty() ? &message.versions.back()
		: FindArchiveVersion(state, key, id);
	CHECK(version);
	auto request = ArchiveCopyRequest();
	request.key = key;
	request.version = version->id;
	request.revision = version->revision;
	request.contentHash = ArchiveContentHash(version->content);
	request.displayedRepresentation = key.representation;
	request.generation = ArchiveGenerationFor(state, key.chat);
	return request;
}

ArchiveEvent Bind(const ArchiveState &state, QString id, const ArchiveSource &source) {
	auto result = Event(state, std::move(id), 0, {}, ArchiveOrigin::SendMetadata,
		source.binding->key);
	result.source = source;
	result.targetVersion = source.binding->version;
	result.targetRevision = source.revision;
	result.expectedContentHash = source.contentHash;
	return result;
}

void Genuine(const ArchiveState &state, const ArchiveCopyRequest &request,
		const QByteArray &bytes = QByteArray("**A**\r\n")) {
	const auto choice = ChooseCopySource(state, request);
	CHECK(choice.validTarget);
	CHECK(!choice.partial);
	CHECK(choice.kind == ArchiveCopySourceKind::GenuineSource);
	CHECK(choice.bytes == bytes);
}

void Structural(const ArchiveState &state, const ArchiveCopyRequest &request,
		bool valid = true, bool partial = false) {
	const auto choice = ChooseCopySource(state, request);
	CHECK(choice.kind == ArchiveCopySourceKind::StructuralWriter);
	CHECK(choice.bytes.isEmpty());
	CHECK(choice.validTarget == valid);
	CHECK(choice.partial == partial);
}

void TestFullBodyAndFragments() {
	Section = "exact full body, genuine bytes and structural fragments";
	auto state = Empty();
	auto event = Event(state, u"a1"_q, 1);
	event.content->caption = u"caption"_q;
	event.content->entities = QJsonArray{ QJsonObject{ { u"type"_q, u"bold"_q } } };
	event.content->rich = QJsonObject{ { u"blocks"_q, QJsonArray{ u"body"_q } } };
	event.content->preserved.insert(u"futureSemantic"_q, u"keep"_q);
	event.source = Source(event);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	const auto request = Copy(state);
	Genuine(state, request);
	for (const auto extent : { ArchiveCopyExtent::Selection, ArchiveCopyExtent::Fragment }) {
		auto selected = request;
		selected.extent = extent;
		Structural(state, selected);
	}
	for (auto i = 0; i != 5; ++i) {
		auto changed = *event.content;
		if (i == 0) { changed.text += u" changed"_q; }
		if (i == 1) { changed.caption.clear(); }
		if (i == 2) { changed.entities = {}; }
		if (i == 3) { changed.rich = QJsonValue(); }
		if (i == 4) { changed.preserved = {}; }
		auto fragmentHash = request;
		fragmentHash.contentHash = ArchiveContentHash(changed);
		Structural(state, fragmentHash, false, true);
	}
	auto ordinary = Empty();
	CHECK(Apply(ordinary, PlanArchiveEvent(ordinary, Event(ordinary, u"incoming"_q, 1, u"**received text**"_q))));
	CHECK(!Message(ordinary).versions[0].source);
	Structural(ordinary, Copy(ordinary));
	auto legacy = Empty();
	event = Event(legacy, u"legacy"_q, 1);
	event.source = Source(event);
	event.source->binding.reset();
	CHECK(Apply(legacy, PlanArchiveEvent(legacy, event)));
	CHECK(ArchiveSourceMatches(Message(legacy).versions[0]));
	Structural(legacy, Copy(legacy));
	auto verified = Source(event);
	CHECK(Apply(legacy, PlanArchiveEvent(legacy, Bind(legacy, u"verify-legacy"_q, verified))));
	Genuine(legacy, Copy(legacy));
	for (const auto completeness : { ArchiveCompleteness::Partial, ArchiveCompleteness::Opaque }) {
		auto incomplete = Empty();
		auto captured = Event(incomplete, u"incomplete"_q, 1);
		captured.completeness = completeness;
		if (completeness == ArchiveCompleteness::Opaque) {
			captured.content->opaqueFormat = u"rich-v1"_q;
			captured.content->opaque = QByteArray("opaque structure");
		}
		captured.source = Source(captured);
		CHECK(Apply(incomplete, PlanArchiveEvent(incomplete, captured)));
		Structural(incomplete, Copy(incomplete), true, true);
	}
}

void TestMarkdownFileIdentity() {
	Section = "original Markdown document identity and explicit empty source";
	auto state = Empty();
	auto event = Event(state, u"md-file"_q, 1, u"rendered file body"_q);
	AddMedia(event);
	event.content->media[0].attributes = QJsonObject{
		{ u"name"_q, u"original.md"_q },
		{ u"mime"_q, u"text/markdown"_q },
	};
	event.source = Source(event, ArchiveSourceKind::ReceivedMarkdownFile);
	event.source->binding->localMessageId.reset();
	event.source->binding->randomId.reset();
	event.source->bytes = QByteArray::fromHex("efbbbf23204f726967696e616c0d0a");
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	const auto request = Copy(state);
	Genuine(state, request, event.source->bytes);
	auto differentDocument = *event.content;
	differentDocument.media[0].identity = u"document:556"_q;
	auto wrongFile = request;
	wrongFile.contentHash = ArchiveContentHash(differentDocument);
	Structural(state, wrongFile, false, true);
	auto edited = Event(state, u"other-file"_q, 2, event.content->text,
		ArchiveOrigin::AuthoritativeEdit);
	edited.content = differentDocument;
	CHECK(Apply(state, PlanArchiveEvent(state, edited)));
	Structural(state, request, false, true);
	Structural(state, Copy(state));
	auto original = request;
	original.target = ArchiveCopyTarget::ArchivedVersion;
	Genuine(state, original, event.source->bytes);

	auto empty = Empty();
	event = Event(empty, u"empty-file"_q, 1, QString());
	AddMedia(event);
	event.source = Source(event, ArchiveSourceKind::ReceivedMarkdownFile);
	event.source->bytes.clear();
	event.source->binding->localMessageId.reset();
	event.source->binding->randomId.reset();
	CHECK(Apply(empty, PlanArchiveEvent(empty, event)));
	Genuine(empty, Copy(empty), QByteArray());
}

void TestEditsAndRepresentations() {
	Section = "causal edits, ABA and displayed translations";
	auto state = Empty();
	auto event = Event(state, u"a1"_q, 1);
	event.source = Source(event);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	auto original = Copy(state);
	CHECK(Apply(state, PlanArchiveEvent(state,
		Event(state, u"b2"_q, 2, u"B"_q, ArchiveOrigin::AuthoritativeEdit))));
	Structural(state, original, false, true);
	original.target = ArchiveCopyTarget::ArchivedVersion;
	Genuine(state, original);
	CHECK(Apply(state, PlanArchiveEvent(state,
		Event(state, u"a3"_q, 3, u"A"_q, ArchiveOrigin::AuthoritativeEdit))));
	CHECK(Message(state).versions.size() == 3);
	CHECK(!Message(state).versions.back().source);
	Structural(state, Copy(state));
	Genuine(state, original);
	auto equal = Empty();
	event = Event(equal, u"initial"_q, 1);
	event.source = Source(event);
	CHECK(Apply(equal, PlanArchiveEvent(equal, event)));
	const auto before = Copy(equal);
	CHECK(Apply(equal, PlanArchiveEvent(equal,
		Event(equal, u"same-body-edit"_q, 2, u"A"_q, ArchiveOrigin::AuthoritativeEdit))));
	CHECK(Message(equal).versions.size() == 1);
	Structural(equal, before, false, true);
	Structural(equal, Copy(equal));
	CHECK(PlanArchiveEvent(equal, Bind(equal, u"stale-bind"_q, *event.source)).status == ArchivePlanStatus::Stale);
	auto current = *event.source;
	current.revision = Message(equal).versions[0].revision;
	CHECK(Apply(equal, PlanArchiveEvent(equal, Bind(equal, u"current-bind"_q, current))));
	Genuine(equal, Copy(equal));

	auto translatedKey = Key();
	translatedKey.representation = u"translation:fa"_q;
	event = Event(state, u"translated"_q, 1, u"translated display"_q,
		ArchiveOrigin::Baseline, translatedKey);
	event.source = Source(event);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	CHECK(ArchiveSourceMatches(Message(state, translatedKey).versions[0]));
	Structural(state, Copy(state, translatedKey));
	auto switched = original;
	switched.displayedRepresentation = translatedKey.representation;
	Structural(state, switched, false, true);
	switched = Copy(state, translatedKey);
	switched.displayedRepresentation = u"original"_q;
	Structural(state, switched, false, true);
	Genuine(state, original);
}

void TestSendBinding() {
	Section = "local/random acknowledgement binds without an edit";
	auto state = Empty();
	const auto event = Event(state, u"server-body"_q, 10);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	const auto originalVersion = Message(state).versions[0];
	const auto originalOrder = Message(state).order;
	auto binding = Bind(state, u"ack"_q, Source(event));
	binding.observation.context.insert(u"delivery"_q, u"sent"_q);
	CHECK(Apply(state, PlanArchiveEvent(state, binding)));
	const auto &version = Message(state).versions[0];
	CHECK(Message(state).versions.size() == 1);
	CHECK(Message(state).order == originalOrder);
	CHECK(version.id == originalVersion.id && version.revision == originalVersion.revision);
	CHECK(version.order == originalVersion.order && version.origin == originalVersion.origin);
	CHECK(version.firstObservedAt == originalVersion.firstObservedAt);
	CHECK(version.content == originalVersion.content);
	CHECK(version.source && version.source->binding->localMessageId == -100);
	CHECK(version.source && version.source->binding->randomId == std::numeric_limits<int64>::max());
	Genuine(state, Copy(state));
	CHECK(PlanArchiveEvent(state, binding).status == ArchivePlanStatus::NoChange);
	auto conflict = binding;
	conflict.source->binding->randomId = 123;
	CHECK(PlanArchiveEvent(state, conflict).status == ArchivePlanStatus::Conflict);
	conflict = binding;
	conflict.id = u"other-source"_q;
	conflict.source->bytes = QByteArray("different source");
	CHECK(PlanArchiveEvent(state, conflict).status == ArchivePlanStatus::Conflict);
	for (auto i = 0; i != 6; ++i) {
		auto stale = binding;
		stale.id = u"bad-%1"_q.arg(i);
		if (i == 0) { stale.source->revision = u"old-revision"_q; }
		if (i == 1) { stale.source->binding->version = u"old-version"_q; }
		if (i == 2) { stale.source->binding->key.account.environment = 1; }
		if (i == 3) { stale.source->binding.reset(); }
		if (i == 4) { stale.expectedContentHash = QString(64, u'f'); }
		if (i == 5) { stale.source->contentHash = QString(64, u'f'); }
		CHECK(PlanArchiveEvent(state, stale).status == ArchivePlanStatus::Stale);
	}
	auto invalid = binding;
	invalid.id = u"zero-random"_q;
	invalid.source->binding->randomId = 0;
	CHECK(PlanArchiveEvent(state, invalid).status == ArchivePlanStatus::Invalid);

	auto otherAccount = Empty({ 1, state.account.userId });
	const auto foreignEvent = Event(otherAccount, u"server-body"_q, 10, u"A"_q,
		ArchiveOrigin::Baseline, Key(otherAccount.account));
	CHECK(Apply(otherAccount, PlanArchiveEvent(otherAccount, foreignEvent)));
	Structural(otherAccount, Copy(state), false, true);
	Structural(otherAccount, Copy(otherAccount, foreignEvent.key));
	auto corrupt = state;
	corrupt.messages[0].versions[0].source->binding->key = foreignEvent.key;
	CHECK(!IsArchiveState(corrupt));
	CHECK(!EncodeArchiveSnapshot(corrupt));
	Structural(corrupt, Copy(state), false, true);
}

void TestPurgeOwnership() {
	Section = "purge removes owned source and invalidates queued copies";
	auto state = Empty();
	auto a = Event(state, u"a1"_q, 1);
	a.source = Source(a);
	a.source->bytes = QByteArray("erased original source");
	CHECK(Apply(state, PlanArchiveEvent(state, a)));
	auto old = Copy(state);
	old.target = ArchiveCopyTarget::ArchivedVersion;
	auto b = Event(state, u"b2"_q, 2, u"B"_q, ArchiveOrigin::AuthoritativeEdit);
	b.source = Source(b);
	b.source->bytes = QByteArray("erased latest source");
	CHECK(Apply(state, PlanArchiveEvent(state, b)));
	const auto queued = Copy(state);
	const auto otherKey = Key(state.account, 99, 67);
	auto other = Event(state, u"other"_q, 1, u"A"_q, ArchiveOrigin::Baseline, otherKey);
	other.source = Source(other);
	CHECK(Apply(state, PlanArchiveEvent(state, other)));
	const auto otherCopy = Copy(state, otherKey);
	CHECK(Apply(state, PlanArchivePurge(state, Purge(state, ArchivePurgeKind::EditHistory, Key().chat))));
	CHECK(Message(state).versions.size() == 1);
	CHECK(Message(state).versions[0].source == b.source);
	CHECK(!FindArchiveVersion(state, Key(), u"a1"_q));
	Structural(state, old, false, true);
	Structural(state, queued, false, true);
	Genuine(state, Copy(state), b.source->bytes);
	Genuine(state, otherCopy);
	CHECK(Apply(state, PlanArchiveEvent(state,
		Event(state, u"delete"_q, 3, {}, ArchiveOrigin::AuthoritativeDelete))));
	Structural(state, Copy(state), false, true);
	auto archived = Copy(state);
	archived.target = ArchiveCopyTarget::ArchivedVersion;
	Genuine(state, archived, b.source->bytes);
	const auto retainedSource = *Message(state).versions.back().source;
	auto late = Bind(state, u"late-binding"_q, retainedSource);
	CHECK(Apply(state, PlanArchivePurge(state, Purge(state, ArchivePurgeKind::Trashbin, Key().chat))));
	CHECK(Message(state).versions.empty() && Message(state).payloadPurged);
	Structural(state, archived, false, true);
	archived.generation = ArchiveGenerationFor(state, Key().chat);
	Structural(state, archived, false, true);
	CHECK(PlanArchiveEvent(state, late).status == ArchivePlanStatus::Stale);
	late.generation = archived.generation;
	CHECK(PlanArchiveEvent(state, late).status != ArchivePlanStatus::Ready);
	Genuine(state, otherCopy);
	const auto encoded = EncodeArchiveSnapshot(state);
	CHECK(encoded && !encoded->contains(a.source->bytes.toBase64()));
	CHECK(encoded && !encoded->contains(b.source->bytes.toBase64()));
}

void TestSourceCodec() {
	Section = "schema 1 source extensions, unknown fields, absent values and wide ids";
	auto state = Empty({ 1, std::numeric_limits<int64>::max() });
	const auto key = Key(state.account, std::numeric_limits<int64>::max(),
		std::numeric_limits<int64>::min(), ArchiveChatKind::Secret);
	auto event = Event(state, u"wide-body"_q, std::numeric_limits<uint64_t>::max(),
		u"A"_q, ArchiveOrigin::Baseline, key);
	event.source = Source(event, ArchiveSourceKind::ReceivedMarkdownFile);
	event.source->bytes = QByteArray::fromHex("efbbbf2320410d0a0001ff");
	event.source->binding->localMessageId = std::numeric_limits<int64>::min();
	event.source->binding->randomId = std::numeric_limits<int64>::max();
	const auto unknown = QJsonObject{ { u"future"_q, QJsonArray{ 7, true, u"keep"_q } } };
	event.source->preserved = unknown;
	event.source->binding->preserved = unknown;
	event.source->binding->key.preserved = unknown;
	event.source->binding->key.chat.preserved = unknown;
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	Genuine(state, Copy(state, key), event.source->bytes);
	const auto snapshot = ArchiveVersionSnapshot{ key, Message(state, key).versions[0], unknown };
	const auto encoded = EncodeArchiveVersionSnapshot(snapshot);
	CHECK(encoded);
	if (!encoded) { return; }
	CHECK(encoded->contains("\"localMessageId\":\"-9223372036854775808\""));
	CHECK(encoded->contains("\"randomId\":\"9223372036854775807\""));
	const auto decoded = DecodeArchiveVersionSnapshot(*encoded);
	CHECK(decoded.status == ArchiveCodecStatus::Valid);
	CHECK(decoded.snapshot && *decoded.snapshot == snapshot);
	CHECK(decoded.snapshot && EncodeArchiveVersionSnapshot(*decoded.snapshot) == encoded);
	const auto object = QJsonDocument::fromJson(*encoded).object();
	for (auto i = 0; i != 3; ++i) {
		auto legacy = object;
		auto version = legacy.value(u"snapshot"_q).toObject();
		if (i == 0) {
			version.remove(u"source"_q);
		} else if (i == 1) {
			version.insert(u"source"_q, QJsonValue(QJsonValue::Null));
		} else {
			auto source = version.value(u"source"_q).toObject();
			source.remove(u"binding"_q);
			version.insert(u"source"_q, source);
		}
		legacy.insert(u"snapshot"_q, version);
		const auto parsed = DecodeArchiveVersionSnapshot(QJsonDocument(legacy).toJson());
		CHECK(parsed.status == ArchiveCodecStatus::Valid);
		CHECK(parsed.snapshot.has_value());
		if (parsed.snapshot) {
			CHECK(i == 2 ? !parsed.snapshot->version.source->binding : !parsed.snapshot->version.source);
			CHECK(EncodeArchiveVersionSnapshot(*parsed.snapshot).has_value());
		}
	}
	for (const auto invalid : { QJsonValue(9223372036854775807LL),
			QJsonValue(u"00"_q), QJsonValue(u"0"_q), QJsonValue(u"9223372036854775808"_q) }) {
		auto bad = object;
		auto version = bad.value(u"snapshot"_q).toObject();
		auto source = version.value(u"source"_q).toObject();
		auto binding = source.value(u"binding"_q).toObject();
		binding.insert(u"randomId"_q, invalid);
		source.insert(u"binding"_q, binding);
		version.insert(u"source"_q, source);
		bad.insert(u"snapshot"_q, version);
		const auto bytes = QJsonDocument(bad).toJson();
		const auto parsed = DecodeArchiveVersionSnapshot(bytes);
		CHECK(parsed.status != ArchiveCodecStatus::Valid && !parsed.snapshot);
		CHECK(parsed.opaque == bytes);
	}
	auto future = object;
	auto version = future.value(u"snapshot"_q).toObject();
	auto source = version.value(u"source"_q).toObject();
	source.insert(u"kind"_q, 2);
	version.insert(u"source"_q, source);
	future.insert(u"snapshot"_q, version);
	const auto bytes = QJsonDocument(future).toJson();
	CHECK(DecodeArchiveVersionSnapshot(bytes).status == ArchiveCodecStatus::Unsupported);
	CHECK(DecodeArchiveVersionSnapshot(bytes).opaque == bytes);
	auto collision = snapshot;
	collision.version.source->binding->preserved.insert(u"randomId"_q, u"not-an-alias"_q);
	CHECK(!EncodeArchiveVersionSnapshot(collision));
}

}

int main() {
	TestFullBodyAndFragments();
	TestMarkdownFileIdentity();
	TestEditsAndRepresentations();
	TestSendBinding();
	TestPurgeOwnership();
	TestSourceCodec();
	return Finish();
}
