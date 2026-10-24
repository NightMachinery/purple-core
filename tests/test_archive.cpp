/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "tests/archive_test_support.h"

using namespace ArchiveTest;

namespace {

void TestCausality() {
	Section = "causality";
	auto state = Empty();
	auto baseline = Event(state, u"base"_q, 10);
	CHECK(Apply(state, PlanArchiveEvent(state, baseline)));
	CHECK(Message(state).versions.size() == 1);
	auto replay = baseline;
	replay.observation.observedAt = 999;
	replay.observation.transport.insert(u"views"_q, 500);
	CHECK(PlanArchiveEvent(state, replay).status == ArchivePlanStatus::NoChange);
	auto edit = Event(state, u"edit-b"_q, 11, u"B"_q, ArchiveOrigin::AuthoritativeEdit);
	CHECK(Apply(state, PlanArchiveEvent(state, edit)));
	edit = Event(state, u"edit-a"_q, 12, u"A"_q, ArchiveOrigin::AuthoritativeEdit);
	CHECK(Apply(state, PlanArchiveEvent(state, edit)));
	CHECK(Message(state).versions.size() == 3);
	CHECK(Message(state).versions[0].content.text == u"A"_q);
	CHECK(Message(state).versions[1].content.text == u"B"_q);
	CHECK(Message(state).versions[2].content.text == u"A"_q);
	CHECK(Message(state).versions[1].observation.serverTime
		== Message(state).versions[2].observation.serverTime);
	CHECK(PlanArchiveEvent(state, baseline).status == ArchivePlanStatus::NoChange);
	CHECK(PlanArchiveEvent(state, Event(state, u"old"_q, 9, u"OLD"_q,
		ArchiveOrigin::AuthoritativeEdit)).status == ArchivePlanStatus::Stale);
	CHECK(PlanArchiveEvent(state, Event(state, u"collision"_q, 12, u"C"_q,
		ArchiveOrigin::AuthoritativeEdit)).status == ArchivePlanStatus::Conflict);
	auto conflict = edit;
	conflict.content->text = u"wrong"_q;
	CHECK(PlanArchiveEvent(state, conflict).status == ArchivePlanStatus::Conflict);
	auto unchanged = Event(state, u"edit-no-content-change"_q, 13, u"A"_q,
		ArchiveOrigin::AuthoritativeEdit);
	CHECK(Apply(state, PlanArchiveEvent(state, unchanged)));
	CHECK(Message(state).versions.size() == 3);
	CHECK(Message(state).order == 13);
	CHECK(PlanArchiveEvent(state, Event(state, u"zero"_q, 0, u"Z"_q,
		ArchiveOrigin::AuthoritativeEdit)).status == ArchivePlanStatus::Invalid);

	auto unknown = Empty();
	CHECK(Apply(unknown, PlanArchiveEvent(unknown,
		Event(unknown, u"first-edited"_q, 20, u"B"_q, ArchiveOrigin::AuthoritativeEdit))));
	CHECK(Message(unknown).versions.size() == 1);
	CHECK(Message(unknown).versions[0].missingPreimage);
	auto absent = Empty();
	const auto deletion = Event(absent, u"unseen-delete"_q, 20, {}, ArchiveOrigin::AuthoritativeDelete);
	CHECK(Apply(absent, PlanArchiveEvent(absent, deletion)));
	CHECK(Message(absent).versions.empty());
	CHECK(Message(absent).deleted);
	CHECK(PlanArchiveEvent(absent, deletion).status == ArchivePlanStatus::NoChange);
}

void TestObservationAndHydration() {
	Section = "observation and hydration";
	auto state = Empty();
	auto baseline = Event(state, u"base"_q, 1);
	baseline.completeness = ArchiveCompleteness::Partial;
	baseline.missing.content = { { { u"rich"_q } } };
	baseline.observation.payloadFormat = u"desktop-tl:200"_q;
	baseline.observation.payload = QByteArray("raw first");
	baseline.observation.context = QJsonObject{
		{ u"sender"_q, u"user:23"_q }, { u"reply"_q, u"66"_q },
	};
	CHECK(Apply(state, PlanArchiveEvent(state, baseline)));
	auto refreshed = Event(state, u"refresh"_q, 1);
	refreshed.completeness = ArchiveCompleteness::Partial;
	refreshed.observation.observedAt = 200;
	refreshed.observation.context = baseline.observation.context;
	refreshed.observation.transport = QJsonObject{
		{ u"reactions"_q, u"new"_q }, { u"views"_q, 9 },
		{ u"quality"_q, u"1080p"_q }, { u"fileReference"_q, u"rotated"_q },
	};
	refreshed.observation.payloadFormat = u"desktop-tl:200"_q;
	refreshed.observation.payload = QByteArray("different raw views and references");
	CHECK(Apply(state, PlanArchiveEvent(state, refreshed)));
	CHECK(Message(state).versions.size() == 1);
	CHECK(Message(state).versions.back().content.text == u"A"_q);
	CHECK(Message(state).versions.back().firstObservedAt == 100);
	CHECK(Message(state).versions.back().observation.observedAt == 200);
	auto send = Event(state, u"send-ack"_q, 0, {}, ArchiveOrigin::SendMetadata);
	send.targetVersion = u"base"_q;
	send.targetRevision = u"base"_q;
	send.observation.context = QJsonObject{ { u"randomId"_q, u"9223372036854775807"_q } };
	send.observation.transport = QJsonObject{ { u"sendState"_q, u"acknowledged"_q } };
	CHECK(Apply(state, PlanArchiveEvent(state, send)));
	CHECK(Message(state).versions.size() == 1);
	CHECK(Message(state).key.messageId == 67);
	CHECK(Message(state).versions[0].observation.context.value(u"sender"_q) == u"user:23"_q);
	CHECK(Message(state).versions[0].observation.context.value(u"reply"_q) == u"66"_q);
	CHECK(Message(state).versions[0].observation.context.value(u"randomId"_q) == u"9223372036854775807"_q);
	CHECK(Message(state).versions[0].observation.transport.value(u"views"_q) == 9);
	CHECK(Message(state).versions[0].observation.transport.value(u"sendState"_q) == u"acknowledged"_q);

	const auto oldHash = ArchiveContentHash(Message(state).versions[0].content);
	auto edit = Event(state, u"edit"_q, 2, u"B"_q, ArchiveOrigin::AuthoritativeEdit);
	CHECK(Apply(state, PlanArchiveEvent(state, edit)));
	auto hydration = Event(state, u"hydrate"_q, 1, u"A"_q, ArchiveOrigin::Hydration);
	hydration.targetVersion = u"base"_q;
	hydration.targetRevision = u"base"_q;
	hydration.expectedContentHash = oldHash;
	hydration.content->rich = QJsonObject{ { u"blocks"_q, QJsonArray{ u"complete old article"_q } } };
	CHECK(Apply(state, PlanArchiveEvent(state, hydration)));
	CHECK(Message(state).versions.size() == 2);
	CHECK(Message(state).versions.front().completeness == ArchiveCompleteness::Complete);
	CHECK(Message(state).versions.back().content.text == u"B"_q);
	auto wrong = hydration;
	wrong.id = u"hydrate-stale"_q;
	CHECK(PlanArchiveEvent(state, wrong).status == ArchivePlanStatus::Stale);
	wrong.expectedContentHash = ArchiveContentHash(Message(state).versions[0].content);
	wrong.content->text = u"newer fetched content"_q;
	CHECK(PlanArchiveEvent(state, wrong).status == ArchivePlanStatus::Conflict);

	auto refresh = Event(state, u"refresh-content"_q, 3, u"C"_q);
	CHECK(PlanArchiveEvent(state, refresh).status == ArchivePlanStatus::Stale);
	refresh.targetVersion = u"edit"_q;
	refresh.targetRevision = u"edit"_q;
	refresh.expectedContentHash = ArchiveContentHash(Message(state).versions.back().content);
	CHECK(Apply(state, PlanArchiveEvent(state, refresh)));
	CHECK(Message(state).versions.size() == 3);
	CHECK(Message(state).versions[1].content.text == u"B"_q);
	CHECK(Message(state).versions.back().origin == ArchiveOrigin::Baseline);
	CHECK(Message(state).versions.back().content.text == u"C"_q);
}

void TestIdentityAndSource() {
	Section = "identities and provenance";
	auto state = Empty();
	const auto max = std::numeric_limits<int64>::max();
	CHECK(IsArchiveMessageKey(Key({ 0, max }, max, max)));
	CHECK(!IsArchiveMessageKey(Key({ 2, 1 })));
	CHECK(!IsArchiveMessageKey(Key({ 0, 0 })));
	CHECK(!IsArchiveMessageKey(Key({ 0, 1 }, 0)));
	CHECK(!IsArchiveMessageKey(Key({ 0, 1 }, 1, -1)));
	CHECK(IsArchiveMessageKey(Key({ 1, max }, max, std::numeric_limits<int64>::min(),
		ArchiveChatKind::Secret)));
	for (const auto kind : { ArchiveChatKind::User, ArchiveChatKind::Group,
		ArchiveChatKind::Channel, ArchiveChatKind::Secret }) {
		auto event = Event(state, QString::number(int(kind)), 1, u"body"_q,
			ArchiveOrigin::Baseline, Key(state.account, 45, 67, kind));
		event.observation.topicId = 1234;
		CHECK(Apply(state, PlanArchiveEvent(state, event)));
	}
	CHECK(state.messages.size() == 4);
	auto other = Empty({ 1, 123 });
	CHECK(PlanArchiveEvent(other, Event(state, u"wrong-account"_q, 1)).status == ArchivePlanStatus::Invalid);
	CHECK(Apply(other, PlanArchiveEvent(other, Event(other, u"other"_q, 1, u"other account"_q,
		ArchiveOrigin::Baseline, Key(other.account)))));
	CHECK(Message(state).versions[0].content.text == u"body"_q);
	CHECK(Message(other, Key(other.account)).versions[0].content.text == u"other account"_q);

	auto original = Empty();
	auto event = Event(original, u"source"_q, 1, u"formatted body"_q);
	event.source = ArchiveSource{ ArchiveSourceKind::ComposeMarkdown,
		ArchiveContentHash(*event.content), u"original"_q, QByteArray("**formatted body**"), event.id };
	CHECK(Apply(original, PlanArchiveEvent(original, event)));
	CHECK(ArchiveSourceMatches(Message(original).versions[0]));
	auto translatedKey = Key();
	translatedKey.representation = u"translation:fa"_q;
	auto translated = Event(original, u"translated"_q, 1, u"translation"_q,
		ArchiveOrigin::Baseline, translatedKey);
	translated.source = event.source;
	CHECK(Apply(original, PlanArchiveEvent(original, translated)));
	CHECK(!ArchiveSourceMatches(Message(original, translatedKey).versions[0]));
	CHECK(original.messages.size() == 2);
	event = Event(original, u"edit-source"_q, 2, u"changed"_q, ArchiveOrigin::AuthoritativeEdit);
	event.source = Message(original).versions[0].source;
	CHECK(Apply(original, PlanArchiveEvent(original, event)));
	CHECK(!ArchiveSourceMatches(Message(original).versions.back()));

	auto migrated = Empty();
	const auto oldKey = Key(state.account, 100, 1, ArchiveChatKind::Group);
	const auto newKey = Key(state.account, 200, 1, ArchiveChatKind::Channel);
	auto old = Event(migrated, u"old"_q, 1, u"old peer"_q, ArchiveOrigin::Baseline, oldKey);
	old.migrationAliases.push_back(newKey.chat);
	CHECK(Apply(migrated, PlanArchiveEvent(migrated, old)));
	auto fresh = Event(migrated, u"new"_q, 1, u"new peer"_q, ArchiveOrigin::Baseline, newKey);
	fresh.migrationAliases.push_back(oldKey.chat);
	fresh.observation.topicId = 99;
	CHECK(Apply(migrated, PlanArchiveEvent(migrated, fresh)));
	CHECK(migrated.messages.size() == 2);
	CHECK(Message(migrated, oldKey).versions[0].content.text == u"old peer"_q);
	CHECK(Message(migrated, newKey).versions[0].observation.topicId == 99);
	auto scheduled = newKey;
	scheduled.collection = ArchiveCollection::Scheduled;
	CHECK(!SameArchiveMessage(scheduled, newKey));
}

void TestTransactions() {
	Section = "transactions and crashes";
	auto state = Empty();
	const auto initial = state;
	const auto event = Event(state, u"base"_q, 1);
	const auto plan = PlanArchiveEvent(state, event);
	CHECK(plan);
	CHECK(plan.writes.front().operation == ArchiveStoreOperation::CompareRevision);
	CHECK(plan.writes.back().operation == ArchiveStoreOperation::PutRevision);
	CHECK(state == initial);
	const auto beforeCommit = CompleteArchiveTransaction(state, plan, false);
	CHECK(beforeCommit.status == ArchiveCommitStatus::NotCommitted);
	CHECK(beforeCommit.state == initial);
	CHECK(Apply(state, plan));
	CHECK(CompleteArchiveTransaction(state, plan, true).status == ArchiveCommitStatus::AlreadyApplied);
	CHECK(PlanArchiveEvent(state, event).status == ArchivePlanStatus::NoChange);
	const auto bytes = EncodeArchiveSnapshot(state);
	CHECK(bytes);
	const auto restored = DecodeArchiveSnapshot(*bytes);
	CHECK(restored.state);
	CHECK(PlanArchiveEvent(*restored.state, event).status == ArchivePlanStatus::NoChange);
	const auto competing = PlanArchiveEvent(initial, Event(initial, u"other"_q, 1, u"other"_q));
	CHECK(CompleteArchiveTransaction(state, competing, true).status == ArchiveCommitStatus::Stale);
	auto impostor = initial;
	impostor.preserved.insert(u"externalChange"_q, true);
	CHECK(CompleteArchiveTransaction(impostor, plan, true).status == ArchiveCommitStatus::Stale);
	auto newer = state;
	newer.version = 2;
	CHECK(PlanArchiveEvent(newer, event).status == ArchivePlanStatus::ReadOnly);
	CHECK(!EncodeArchiveSnapshot(newer));
	auto exhausted = initial;
	exhausted.revision = std::numeric_limits<uint64_t>::max();
	CHECK(PlanArchiveEvent(exhausted, Event(exhausted, u"exhausted"_q, 1)).status
		== ArchivePlanStatus::Exhausted);
}

void TestMediaAndPurge() {
	Section = "media and purge";
	auto state = Empty();
	auto event = Event(state, u"base"_q, 1);
	AddMedia(event);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	const auto job = Job(state, u"job1"_q, u"base"_q);
	CHECK(Apply(state, PlanArchiveMediaStage(state, job)));
	CHECK(PlanArchiveMediaStage(state, job).status == ArchivePlanStatus::NoChange);
	CHECK(Message(state).versions[0].media[0].state == ArchiveMediaState::Staged);
	auto wrong = MediaSuccess();
	wrong.size = 0;
	CHECK(PlanArchiveMediaCompletion(state, job.id, wrong).status == ArchivePlanStatus::Invalid);
	wrong = MediaSuccess();
	wrong.verifiedHash = QString(64, u'b');
	CHECK(PlanArchiveMediaCompletion(state, job.id, wrong).status == ArchivePlanStatus::Invalid);
	CHECK(Apply(state, PlanArchiveMediaCompletion(state, job.id, MediaSuccess())));
	CHECK(PlanArchiveMediaCompletion(state, job.id, MediaSuccess()).status == ArchivePlanStatus::NoChange);
	CHECK(Message(state).versions[0].media[0].state == ArchiveMediaState::Complete);
	CHECK(CountArchiveBlobReferences(state, BlobId).versions == 1);
	CHECK(PlanArchiveGarbageCollection(state).status == ArchivePlanStatus::NoChange);

	event = Event(state, u"edited"_q, 2, u"B"_q, ArchiveOrigin::AuthoritativeEdit);
	AddMedia(event);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	CHECK(PlanArchiveMediaStage(state, Job(state, u"job2"_q, u"edited"_q)).status
		== ArchivePlanStatus::NoChange);
	CHECK(Message(state).versions.back().media[0].state == ArchiveMediaState::Complete);
	CHECK(state.blobs.size() == 1);
	CHECK(CountArchiveBlobReferences(state, BlobId).versions == 2);
	auto lease = ArchiveReaderLease{ u"reader"_q, Key(), u"base"_q, BlobId };
	CHECK(Apply(state, PlanArchiveReaderAcquire(state, lease)));
	CHECK(PlanArchiveReaderAcquire(state, lease).status == ArchivePlanStatus::NoChange);
	CHECK(CountArchiveBlobReferences(state, BlobId).readers == 1);

	event = Event(state, u"deleted"_q, 3, {}, ArchiveOrigin::LocalDelete);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	auto wrongScope = Purge(state, ArchivePurgeKind::Trashbin);
	wrongScope.scope.account = { 0, 456 };
	CHECK(PlanArchivePurge(state, wrongScope).status == ArchivePlanStatus::Invalid);
	const auto staleDialog = Purge(state, ArchivePurgeKind::Trashbin);
	const auto editPurge = PlanArchivePurge(state, Purge(state, ArchivePurgeKind::EditHistory));
	CHECK(editPurge.revokeReaders.size() == 1);
	CHECK(Apply(state, editPurge));
	CHECK(Message(state).versions.size() == 1);
	CHECK(Message(state).versions.back().content.text == u"B"_q);
	CHECK(Message(state).deleted);
	CHECK(state.leases[0].revoked);
	CHECK(CountArchiveBlobReferences(state, BlobId).versions == 1);
	CHECK(CountArchiveBlobReferences(state, BlobId).readers == 1);
	CHECK(PlanArchivePurge(state, staleDialog).status == ArchivePlanStatus::Stale);
	const auto captureBeforePurge = Event(state, u"late"_q, 4);
	const auto trash = PlanArchivePurge(state, Purge(state, ArchivePurgeKind::Trashbin));
	CHECK(Apply(state, trash));
	CHECK(Message(state).payloadPurged);
	CHECK(Message(state).versions.empty());
	CHECK(CountArchiveBlobReferences(state, BlobId).versions == 0);
	CHECK(PlanArchiveEvent(state, captureBeforePurge).status == ArchivePlanStatus::Stale);
	CHECK(PlanArchiveEvent(state, Event(state, u"later"_q, 5)).status == ArchivePlanStatus::Purged);
	CHECK(PlanArchiveGarbageCollection(state).status == ArchivePlanStatus::NoChange);
	CHECK(Apply(state, PlanArchiveReaderRelease(state, lease.id)));
	const auto gc = PlanArchiveGarbageCollection(state);
	CHECK(gc.deleteOwnedBlobs == std::vector<QString>{ BlobId });
	CHECK(Apply(state, gc));
	CHECK(state.blobs[0].state == ArchiveBlobState::GarbagePending);
	CHECK(Apply(state, PlanArchiveGarbageCollection(state)));
	CHECK(Apply(state, CompleteArchiveGarbageCollection(state, { BlobId })));
	CHECK(state.blobs.empty());
	CHECK(CompleteArchiveGarbageCollection(state, { BlobId }).status == ArchivePlanStatus::NoChange);
}

void TestPurgeRacesAndScopes() {
	Section = "purge races and scopes";
	auto state = Empty();
	auto event = Event(state, u"base"_q, 1);
	AddMedia(event);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	const auto job = Job(state, u"stale-job"_q, u"base"_q);
	CHECK(Apply(state, PlanArchiveMediaStage(state, job)));
	const auto completion = PlanArchiveMediaCompletion(state, job.id, MediaSuccess());
	const auto beforePurge = state;
	const auto purge = PlanArchivePurge(state, Purge(state, ArchivePurgeKind::EditHistory, Key().chat));
	CHECK(purge.releaseStagedJobs == std::vector<QString>{ job.id });
	CHECK(Apply(state, purge));
	CHECK(state.jobs[0].state == ArchiveJobState::Cancelled);
	CHECK(Message(state).versions[0].media[0].state == ArchiveMediaState::Missing);
	CHECK(PlanArchiveMediaCompletion(state, job.id, MediaSuccess()).status == ArchivePlanStatus::Stale);
	CHECK(CompleteArchiveTransaction(state, completion, true).status == ArchiveCommitStatus::Stale);
	CHECK(CompleteArchiveTransaction(beforePurge, purge, false).state == beforePurge);
	CHECK(Apply(state, PlanArchiveMediaStage(state, Job(state, u"fresh-job"_q, u"base"_q))));
	auto failure = ArchiveMediaResult();
	failure.failure = u"short-read"_q;
	CHECK(Apply(state, PlanArchiveMediaCompletion(state, u"fresh-job"_q, failure)));
	CHECK(Message(state).versions[0].media[0].state == ArchiveMediaState::Failed);
	CHECK(PlanArchiveMediaCompletion(state, u"fresh-job"_q, failure).status == ArchivePlanStatus::NoChange);
	CHECK(Apply(state, PlanArchiveMediaStage(state, Job(state, u"retry"_q, u"base"_q))));
	CHECK(Apply(state, PlanArchiveMediaCompletion(state, u"retry"_q, MediaSuccess())));

	const auto otherKey = Key(state.account, 999, 67);
	event = Event(state, u"unrelated"_q, 1, u"unrelated"_q, ArchiveOrigin::Baseline, otherKey);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	CHECK(Apply(state, PlanArchiveEvent(state, Event(state, u"delete-other"_q, 2, {},
		ArchiveOrigin::MediaExpiry, otherKey))));
	CHECK(Apply(state, PlanArchivePurge(state, Purge(state, ArchivePurgeKind::Trashbin, Key().chat))));
	CHECK(Message(state, otherKey).versions.size() == 1);
	CHECK(!Message(state, otherKey).payloadPurged);
	CHECK(Apply(state, PlanArchivePurge(state, Purge(state, ArchivePurgeKind::Trashbin))));
	CHECK(Message(state, otherKey).payloadPurged);
	CHECK(Message(state).versions.size() == 1);
	CHECK(CountArchiveBlobReferences(state, BlobId).versions == 1);
}

void TestLeaseAndCollectionRaces() {
	Section = "lease and collection races";
	auto state = Empty();
	auto event = Event(state, u"body"_q, 1);
	AddMedia(event);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	const auto job = Job(state, u"media"_q, u"body"_q);
	CHECK(Apply(state, PlanArchiveMediaStage(state, job)));
	CHECK(Apply(state, PlanArchiveMediaCompletion(state, job.id, MediaSuccess())));
	CHECK(PlanArchiveMediaStage(state, job).status == ArchivePlanStatus::NoChange);
	auto wrongSize = MediaSuccess();
	wrongSize.size = 6;
	CHECK(PlanArchiveMediaCompletion(state, job.id, wrongSize).status == ArchivePlanStatus::Conflict);
	auto reader = ArchiveReaderLease{ u"late-reader"_q, Key(), u"body"_q, BlobId };
	reader.generation = ArchiveGenerationFor(state, Key().chat);
	const auto acquire = PlanArchiveReaderAcquire(state, reader);
	CHECK(acquire);
	const auto purge = PlanArchivePurge(state, Purge(state, ArchivePurgeKind::EditHistory));
	CHECK(Apply(state, purge));
	CHECK(CompleteArchiveTransaction(state, acquire, true).status == ArchiveCommitStatus::Stale);
	CHECK(PlanArchiveReaderAcquire(state, reader).status == ArchivePlanStatus::Stale);
	reader.generation = ArchiveGenerationFor(state, Key().chat);
	CHECK(Apply(state, PlanArchiveReaderAcquire(state, reader)));
	CHECK(Apply(state, PlanArchiveEvent(state, Event(state, u"deleted"_q, 2, {},
		ArchiveOrigin::MediaConsumption))));
	CHECK(Apply(state, PlanArchivePurge(state, Purge(state, ArchivePurgeKind::Trashbin))));
	CHECK(PlanArchiveGarbageCollection(state).status == ArchivePlanStatus::NoChange);
	CHECK(Apply(state, PlanArchiveReaderRelease(state, reader.id)));
	const auto gc = PlanArchiveGarbageCollection(state);
	CHECK(gc);
	const auto crash = CompleteArchiveTransaction(state, gc, false);
	CHECK(crash.state == state);
	CHECK(crash.state.blobs[0].state == ArchiveBlobState::Complete);
	CHECK(Apply(state, gc));
	CHECK(CompleteArchiveGarbageCollection(state, { BlobId }).status == ArchivePlanStatus::Ready);
	const auto restart = DecodeArchiveSnapshot(*EncodeArchiveSnapshot(state));
	CHECK(restart.state);
	const auto retry = PlanArchiveGarbageCollection(*restart.state);
	CHECK(retry.deleteOwnedBlobs == std::vector<QString>{ BlobId });
	CHECK(Apply(state, retry));
	CHECK(Apply(state, CompleteArchiveGarbageCollection(state, { BlobId })));
	CHECK(state.blobs.empty());

	auto exhausted = Empty();
	exhausted.revision = std::numeric_limits<uint64_t>::max();
	exhausted.blobs.push_back({ BlobId, 7 });
	const auto noCommit = PlanArchiveGarbageCollection(exhausted);
	CHECK(noCommit.status == ArchivePlanStatus::Exhausted);
	CHECK(noCommit.deleteOwnedBlobs.empty());
	CHECK(noCommit.writes.empty());
	exhausted = Empty();
	exhausted.accountGeneration = std::numeric_limits<uint64_t>::max();
	CHECK(PlanArchivePurge(exhausted, Purge(exhausted, ArchivePurgeKind::Trashbin)).status
		== ArchivePlanStatus::Exhausted);
	exhausted = Empty();
	exhausted.chatGenerations.push_back({ Key().chat, std::numeric_limits<uint64_t>::max() });
	CHECK(PlanArchivePurge(exhausted, Purge(exhausted, ArchivePurgeKind::EditHistory, Key().chat)).status
		== ArchivePlanStatus::Exhausted);
}

void TestExplicitMigrationPurge() {
	Section = "explicit migration purge";
	auto state = Empty();
	const auto group = Key(state.account, 100, 1, ArchiveChatKind::Group);
	const auto channel = Key(state.account, 200, 1);
	for (const auto &key : { group, channel }) {
		auto event = Event(state, u"base"_q, 1, u"A"_q, ArchiveOrigin::Baseline, key);
		event.migrationAliases.push_back(SameArchiveChat(key.chat, group.chat) ? channel.chat : group.chat);
		CHECK(Apply(state, PlanArchiveEvent(state, event)));
		event = Event(state, u"edit"_q, 2, u"B"_q, ArchiveOrigin::AuthoritativeEdit, key);
		event.observation.topicId = SameArchiveChat(key.chat, channel.chat) ? 99 : 0;
		CHECK(Apply(state, PlanArchiveEvent(state, event)));
	}
	CHECK(Apply(state, PlanArchivePurge(state, Purge(state, ArchivePurgeKind::EditHistory, group.chat))));
	CHECK(Message(state, group).versions.size() == 1);
	CHECK(Message(state, channel).versions.size() == 2);
	auto scope = Purge(state, ArchivePurgeKind::EditHistory, group.chat);
	scope.scope.migrationAliases.push_back(channel.chat);
	CHECK(Apply(state, PlanArchivePurge(state, scope)));
	CHECK(Message(state, channel).versions.size() == 1);
	CHECK(Message(state, channel).versions[0].observation.topicId == 99);
	CHECK(ArchiveGenerationFor(state, group.chat).chat == 2);
	CHECK(ArchiveGenerationFor(state, channel.chat).chat == 1);
	scope = Purge(state, ArchivePurgeKind::Trashbin, group.chat);
	scope.scope.migrationAliases.push_back(Key(state.account, 100, 1, ArchiveChatKind::Secret).chat);
	CHECK(PlanArchivePurge(state, scope).status == ArchivePlanStatus::Invalid);
	CHECK(Message(state, channel).key.chat.id == 200);
}

void TestCanonicalFields() {
	Section = "canonical fields";
	auto content = ArchiveContent();
	content.text = u"text"_q;
	CHECK(SameArchiveContent(content, content));
	const auto baseline = content;
	const auto hash = ArchiveContentHash(content);
	CHECK(IsArchiveHash(hash));
	content.caption = u"caption"_q;
	CHECK(!SameArchiveContent(content, baseline));
	content = baseline;
	content.entities.push_back(QJsonObject{ { u"type"_q, u"bold"_q }, { u"offset"_q, 0 }, { u"length"_q, 4 } });
	CHECK(!SameArchiveContent(content, baseline));
	content = baseline;
	content.rich = QJsonObject{ { u"type"_q, u"article"_q } };
	CHECK(!SameArchiveContent(content, baseline));
	content = baseline;
	content.media.push_back({ u"photo:42"_q, u"photo"_q });
	CHECK(!SameArchiveContent(content, baseline));
	content = baseline;
	content.opaqueFormat = u"unsupported:block"_q;
	content.opaque = QByteArray("raw semantic block");
	CHECK(!SameArchiveContent(content, baseline));
	content = baseline;
	content.preserved.insert(u"futureSemanticField"_q, u"value"_q);
	CHECK(!SameArchiveContent(content, baseline));
	auto one = baseline;
	auto two = baseline;
	one.rich = QJsonObject{ { u"a"_q, 1 }, { u"z"_q, 2 } };
	two.rich = QJsonObject{ { u"z"_q, 2 }, { u"a"_q, 1 } };
	CHECK(SameArchiveContent(one, two));
}

void TestZeroAndFailedMedia() {
	Section = "empty and failed media";
	auto state = Empty();
	auto event = Event(state, u"body"_q, 1);
	AddMedia(event);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	auto job = Job(state, u"unknown-size"_q, u"body"_q);
	job.expectedSize.reset();
	CHECK(Apply(state, PlanArchiveMediaStage(state, job)));
	auto result = MediaSuccess();
	result.size = 0;
	CHECK(PlanArchiveMediaCompletion(state, job.id, result).status == ArchivePlanStatus::Invalid);
	result = {};
	result.failure = u"source-unavailable"_q;
	CHECK(Apply(state, PlanArchiveMediaCompletion(state, job.id, result)));
	CHECK(state.blobs.empty());
	CHECK(PlanArchiveMediaStage(state, job).status == ArchivePlanStatus::NoChange);
	job = Job(state, u"known-empty"_q, u"body"_q);
	job.expectedSize = 0;
	CHECK(Apply(state, PlanArchiveMediaStage(state, job)));
	result = MediaSuccess();
	result.size = 0;
	CHECK(Apply(state, PlanArchiveMediaCompletion(state, job.id, result)));
	CHECK(state.blobs[0].size == 0);
}

void TestHydrationCannotLoseKnownContent() {
	Section = "hydration known-content guard";
	auto state = Empty();
	auto event = Event(state, u"partial"_q, 1);
	AddMedia(event);
	event.completeness = ArchiveCompleteness::Partial;
	event.content->caption = u"known caption"_q;
	event.content->entities.push_back(QJsonObject{ { u"type"_q, u"bold"_q } });
	event.source = ArchiveSource{ ArchiveSourceKind::ComposeMarkdown,
		ArchiveContentHash(*event.content), u"original"_q, QByteArray("A"), event.id };
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	auto hydration = event;
	hydration.id = u"hydrate"_q;
	hydration.origin = ArchiveOrigin::Hydration;
	hydration.targetVersion = event.id;
	hydration.targetRevision = event.id;
	hydration.expectedContentHash = ArchiveContentHash(*event.content);
	hydration.completeness = ArchiveCompleteness::Complete;
	hydration.source.reset();
	auto wrong = hydration;
	wrong.content->text = u"newer fetched body"_q;
	CHECK(PlanArchiveEvent(state, wrong).status == ArchivePlanStatus::Conflict);
	wrong = hydration;
	wrong.content->caption.clear();
	CHECK(PlanArchiveEvent(state, wrong).status == ArchivePlanStatus::Conflict);
	wrong = hydration;
	wrong.content->media.clear();
	CHECK(PlanArchiveEvent(state, wrong).status == ArchivePlanStatus::Conflict);
	wrong = hydration;
	wrong.content->entities = QJsonArray();
	CHECK(PlanArchiveEvent(state, wrong).status == ArchivePlanStatus::Conflict);
	CHECK(Apply(state, PlanArchiveEvent(state, hydration)));
	CHECK(ArchiveSourceMatches(Message(state).versions[0]));
	wrong = hydration;
	wrong.id = u"downgrade"_q;
	wrong.completeness = ArchiveCompleteness::Partial;
	CHECK(PlanArchiveEvent(state, wrong).status == ArchivePlanStatus::Conflict);
	wrong = event;
	wrong.id = u"refresh-downgrade"_q;
	wrong.origin = ArchiveOrigin::Baseline;
	wrong.content->text = u"partial refresh"_q;
	wrong.targetVersion = event.id;
	wrong.targetRevision = event.id;
	wrong.expectedContentHash = hydration.expectedContentHash;
	CHECK(PlanArchiveEvent(state, wrong).status == ArchivePlanStatus::Conflict);
	CHECK(Message(state).versions.size() == 1);
	CHECK(Message(state).versions[0].completeness == ArchiveCompleteness::Complete);
}

void TestHydrationReviewFixtures() {
	Section = "hydration review rich replacement";
	auto state = Empty();
	auto event = Event(state, u"base"_q, 1, u"same text"_q);
	event.completeness = ArchiveCompleteness::Partial;
	event.content->rich = QJsonObject{
		{ u"blocks"_q, QJsonArray{ u"captured original paragraph"_q } },
	};
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	const auto before = state;
	auto hydration = Event(state, u"hydrate"_q, 1, u"same text"_q, ArchiveOrigin::Hydration);
	hydration.targetVersion = u"base"_q;
	hydration.targetRevision = u"base"_q;
	hydration.expectedContentHash = ArchiveContentHash(Message(state).versions[0].content);
	hydration.content->rich = QJsonObject{
		{ u"blocks"_q, QJsonArray{ u"different later paragraph"_q } },
	};
	CHECK(PlanArchiveEvent(state, hydration).status == ArchivePlanStatus::Conflict);
	CHECK(state == before);

	Section = "hydration review unknown field erasure";
	state = Empty();
	event = Event(state, u"base"_q, 1);
	event.completeness = ArchiveCompleteness::Partial;
	event.content->preserved.insert(u"futureBody"_q, u"captured semantic body"_q);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	const auto withUnknown = state;
	hydration = Event(state, u"hydrate"_q, 1, u"A"_q, ArchiveOrigin::Hydration);
	hydration.targetVersion = u"base"_q;
	hydration.targetRevision = u"base"_q;
	hydration.expectedContentHash = ArchiveContentHash(Message(state).versions[0].content);
	CHECK(PlanArchiveEvent(state, hydration).status == ArchivePlanStatus::Conflict);
	CHECK(state == withUnknown);
}

void TestHydrationSemanticPreservation() {
	Section = "hydration semantic preservation";
	auto state = Empty();
	auto event = Event(state, u"partial"_q, 1);
	event.completeness = ArchiveCompleteness::Partial;
	event.content->rich = QJsonObject{
		{ u"blocks"_q, QJsonArray{ u"first"_q, u"second"_q, u"first"_q } },
		{ u"metadata"_q, QJsonObject{ { u"title"_q, u"captured"_q } } },
	};
	event.content->preserved = QJsonObject{
		{ u"futureBody"_q, QJsonObject{
			{ u"text"_q, u"captured"_q }, { u"nullable"_q, QJsonValue::Null },
			{ u"empty"_q, u""_q }, { u"items"_q, QJsonArray{ 1, 2, 1 } },
		} },
	};
	AddMedia(event);
	event.content->media[0].attributes = QJsonObject{
		{ u"dimensions"_q, QJsonObject{ { u"width"_q, 80 } } },
	};
	event.content->media[0].preserved = QJsonObject{
		{ u"futureDescriptor"_q, QJsonObject{ { u"captured"_q, true } } },
	};
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	const auto before = state;
	auto hydration = event;
	hydration.id = u"hydrate"_q;
	hydration.origin = ArchiveOrigin::Hydration;
	hydration.targetVersion = event.id;
	hydration.targetRevision = event.id;
	hydration.expectedContentHash = ArchiveContentHash(*event.content);
	hydration.completeness = ArchiveCompleteness::Complete;
	const auto rejects = [&](const ArchiveContent &content) {
		auto replacement = hydration;
		replacement.content = content;
		CHECK(PlanArchiveEvent(state, replacement).status == ArchivePlanStatus::Conflict);
		CHECK(state == before);
	};
	auto changed = *event.content;
	changed.rich = QJsonValue::Null;
	rejects(changed);
	changed.rich = QJsonObject{ { u"blocks"_q, QJsonArray{ u"first"_q } } };
	rejects(changed);
	changed.rich = QJsonObject{
		{ u"blocks"_q, QJsonArray{ u"second"_q, u"first"_q, u"first"_q } },
		{ u"metadata"_q, QJsonObject{ { u"title"_q, u"captured"_q } } },
	};
	rejects(changed);
	changed = *event.content;
	auto rich = changed.rich.toObject();
	rich.insert(u"metadata"_q, QJsonObject{ { u"title"_q, u"replacement"_q } });
	changed.rich = rich;
	rejects(changed);
	rich = event.content->rich.toObject();
	rich.insert(u"blocks"_q, QJsonArray{ u"first"_q, u"second"_q, u"first"_q, u"later"_q });
	changed.rich = rich;
	rejects(changed);
	changed = *event.content;
	changed.preserved = QJsonObject();
	rejects(changed);
	for (const auto &field : { u"text"_q, u"nullable"_q, u"empty"_q, u"items"_q }) {
		changed = *event.content;
		auto body = changed.preserved.value(u"futureBody"_q).toObject();
		body.remove(field);
		changed.preserved.insert(u"futureBody"_q, body);
		rejects(changed);
		body.insert(field, u"replacement"_q);
		changed.preserved.insert(u"futureBody"_q, body);
		rejects(changed);
	}
	changed = *event.content;
	changed.media[0].attributes = QJsonObject();
	rejects(changed);
	changed.media[0].attributes = QJsonObject{
		{ u"dimensions"_q, QJsonObject{ { u"width"_q, 81 } } },
	};
	rejects(changed);
	changed = *event.content;
	changed.media[0].preserved = QJsonObject();
	rejects(changed);
	changed.media[0].preserved.insert(u"futureDescriptor"_q,
		QJsonObject{ { u"captured"_q, false } });
	rejects(changed);

	hydration.content->rich = QJsonObject{
		{ u"blocks"_q, event.content->rich.toObject().value(u"blocks"_q) },
		{ u"metadata"_q, QJsonObject{
			{ u"title"_q, u"captured"_q }, { u"language"_q, u"en"_q },
		} },
		{ u"footer"_q, u"newly captured"_q },
	};
	auto body = hydration.content->preserved.value(u"futureBody"_q).toObject();
	body.insert(u"newField"_q, u"newly captured"_q);
	hydration.content->preserved.insert(u"futureBody"_q, body);
	hydration.content->media[0].attributes.insert(u"dimensions"_q,
		QJsonObject{ { u"width"_q, 80 }, { u"height"_q, 60 } });
	hydration.content->media[0].preserved.insert(u"futureDescriptor"_q,
		QJsonObject{ { u"captured"_q, true }, { u"newField"_q, 1 } });
	hydration.content->media.push_back({ u"document:556"_q, u"document"_q });
	hydration.content->entities = QJsonArray{ QJsonObject{ { u"type"_q, u"bold"_q } } };
	CHECK(PlanArchiveEvent(state, hydration).status == ArchivePlanStatus::Conflict);
	CHECK(state == before);
	hydration.content = event.content;
	CHECK(Apply(state, PlanArchiveEvent(state, hydration)));
	CHECK(Message(state).versions.size() == 1);
	CHECK(Message(state).versions[0].id == event.id);
	CHECK(Message(state).versions[0].revision == event.id);
	CHECK(Message(state).versions[0].content == *hydration.content);
	CHECK(Message(state).versions[0].completeness == ArchiveCompleteness::Complete);

	Section = "hydration absent structures";
	state = Empty();
	event = Event(state, u"empty-partial"_q, 1, {});
	event.completeness = ArchiveCompleteness::Partial;
	event.missing.content = { { { u"text"_q } }, { { u"rich"_q, u"blocks"_q } } };
	event.content->rich = QJsonObject{ { u"blocks"_q, QJsonArray() } };
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	hydration = Event(state, u"fill"_q, 1, u"captured text"_q, ArchiveOrigin::Hydration);
	hydration.targetVersion = event.id;
	hydration.targetRevision = event.id;
	hydration.expectedContentHash = ArchiveContentHash(*event.content);
	hydration.content->rich = QJsonObject{ { u"blocks"_q, QJsonArray{ u"newly captured"_q } } };
	CHECK(Apply(state, PlanArchiveEvent(state, hydration)));
	CHECK(Message(state).versions[0].content == *hydration.content);
}

void TestHydrationOrderedAndOpaqueContent() {
	Section = "hydration array and opaque preservation";
	auto state = Empty();
	auto event = Event(state, u"partial"_q, 1);
	event.completeness = ArchiveCompleteness::Partial;
	const auto bold = QJsonObject{ { u"type"_q, u"bold"_q } };
	const auto italic = QJsonObject{ { u"type"_q, u"italic"_q } };
	event.content->entities = QJsonArray{ bold, italic, bold };
	AddMedia(event);
	event.content->media.push_back({ u"document:556"_q, u"document"_q });
	event.content->opaqueFormat = u"unknown-content:9"_q;
	event.content->opaque = QByteArray("captured semantic bytes");
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	const auto before = state;
	auto hydration = event;
	hydration.id = u"hydrate"_q;
	hydration.origin = ArchiveOrigin::Hydration;
	hydration.targetVersion = event.id;
	hydration.targetRevision = event.id;
	hydration.expectedContentHash = ArchiveContentHash(*event.content);
	hydration.completeness = ArchiveCompleteness::Complete;
	const auto rejects = [&](const ArchiveContent &content) {
		auto replacement = hydration;
		replacement.content = content;
		CHECK(PlanArchiveEvent(state, replacement).status == ArchivePlanStatus::Conflict);
		CHECK(state == before);
	};
	auto changed = *event.content;
	changed.entities = QJsonArray{ bold, italic };
	rejects(changed);
	changed.entities = QJsonArray{ italic, bold, bold };
	rejects(changed);
	changed.entities = QJsonArray{ bold, italic, bold, italic };
	rejects(changed);
	changed = *event.content;
	std::swap(changed.media[0], changed.media[1]);
	rejects(changed);
	changed = *event.content;
	changed.media.erase(changed.media.begin());
	rejects(changed);
	changed = *event.content;
	changed.media[0].kind = u"photo"_q;
	rejects(changed);
	changed = *event.content;
	changed.opaqueFormat = u"unknown-content:10"_q;
	rejects(changed);
	changed = *event.content;
	changed.opaque = QByteArray("replacement semantic bytes");
	rejects(changed);
	changed.opaqueFormat.clear();
	changed.opaque.clear();
	rejects(changed);
	CHECK(Apply(state, PlanArchiveEvent(state, hydration)));
	CHECK(Message(state).versions[0].content == *event.content);
}

ArchiveEvent BoundHydration(const ArchiveState &state, const ArchiveEvent &captured, QString id) {
	auto event = captured;
	event.generation = ArchiveGenerationFor(state, event.key.chat);
	event.id = std::move(id);
	event.origin = ArchiveOrigin::Hydration;
	event.targetVersion = captured.id;
	event.targetRevision = captured.id;
	event.expectedContentHash = ArchiveContentHash(*captured.content);
	return event;
}

void TestHydrationLaterRevisionAdditions() {
	Section = "later media appended to old capture";
	auto state = Empty();
	auto base = Event(state, u"old"_q, 1, u"old body"_q);
	base.completeness = ArchiveCompleteness::Partial;
	AddMedia(base);
	CHECK(Apply(state, PlanArchiveEvent(state, base)));
	auto edit = Event(state, u"new"_q, 2, u"new body"_q, ArchiveOrigin::AuthoritativeEdit);
	edit.content->media.push_back({ u"document:999"_q, u"document"_q });
	CHECK(Apply(state, PlanArchiveEvent(state, edit)));
	const auto before = state;
	auto hydration = BoundHydration(state, base, u"hydrate-old"_q);
	hydration.content->media.push_back(edit.content->media[0]);
	CHECK(PlanArchiveEvent(state, hydration).status == ArchivePlanStatus::Conflict);
	CHECK(state == before);
	auto job = Job(state, u"download-later-as-old"_q, u"old"_q);
	job.mediaIdentity = u"document:999"_q;
	CHECK(PlanArchiveMediaStage(state, job).status == ArchivePlanStatus::NotFound);
	CHECK(Message(state).versions[0].content.media == base.content->media);
	CHECK(Message(state).versions[1].content.media == edit.content->media);

	Section = "later semantic field injected into old rich object";
	state = Empty();
	base = Event(state, u"old"_q, 1, u"same text"_q);
	base.completeness = ArchiveCompleteness::Partial;
	base.content->rich = QJsonObject{ { u"article"_q, QJsonObject{
		{ u"paragraphs"_q, QJsonArray{ u"captured original paragraph"_q } },
	} } };
	CHECK(Apply(state, PlanArchiveEvent(state, base)));
	edit = Event(state, u"new"_q, 2, u"same text"_q, ArchiveOrigin::AuthoritativeEdit);
	edit.content->rich = QJsonObject{ { u"article"_q, QJsonObject{
		{ u"paragraphs"_q, QJsonArray{ u"captured original paragraph"_q } },
		{ u"footer"_q, u"later added paragraph"_q },
	} } };
	CHECK(Apply(state, PlanArchiveEvent(state, edit)));
	hydration = BoundHydration(state, base, u"hydrate-old"_q);
	hydration.content->rich = edit.content->rich;
	CHECK(PlanArchiveEvent(state, hydration).status == ArchivePlanStatus::Conflict);
	CHECK(Message(state).versions[0].content == *base.content);
	CHECK(Message(state).versions[1].content == *edit.content);
}

void TestDeclaredHydrationSlots() {
	Section = "declared revision-bound hydration slots";
	auto state = Empty();
	auto base = Event(state, u"old"_q, 1, u"same text"_q);
	base.completeness = ArchiveCompleteness::Partial;
	base.content->rich = QJsonObject{ { u"article"_q, QJsonObject{
		{ u"paragraphs"_q, QJsonArray{ u"captured original paragraph"_q } },
	} } };
	base.missing.content = {
		{ { u"rich"_q, u"article"_q, u"paragraphs"_q } },
		{ { u"rich"_q, u"article"_q, u"footer"_q } },
	};
	base.missing.media = { u"document:555"_q };
	AddMedia(base);
	base.content->media[0].attributes = QJsonObject{
		{ u"dimensions"_q, QJsonObject{ { u"width"_q, 80 } } },
	};
	CHECK(Apply(state, PlanArchiveEvent(state, base)));
	CHECK(Message(state).versions[0].missing.revision == base.id);
	CHECK(Message(state).versions[0].missing.content == base.missing.content);
	CHECK(Message(state).versions[0].missing.media == base.missing.media);
	auto edit = Event(state, u"new"_q, 2, u"later body"_q, ArchiveOrigin::AuthoritativeEdit);
	CHECK(Apply(state, PlanArchiveEvent(state, edit)));
	auto hydration = BoundHydration(state, base, u"fill-prefix"_q);
	hydration.content->rich = QJsonObject{ { u"article"_q, QJsonObject{
		{ u"paragraphs"_q, QJsonArray{ u"captured original paragraph"_q, u"same revision tail"_q } },
	} } };
	hydration.missing.content.erase(hydration.missing.content.begin());
	auto changingCaptured = hydration;
	changingCaptured.content->rich = QJsonObject{ { u"article"_q, QJsonObject{
		{ u"paragraphs"_q, QJsonArray{ u"replacement paragraph"_q, u"same revision tail"_q } },
	} } };
	CHECK(PlanArchiveEvent(state, changingCaptured).status == ArchivePlanStatus::Conflict);
	changingCaptured = hydration;
	changingCaptured.content->media[0].attributes = QJsonObject{
		{ u"dimensions"_q, QJsonObject{ { u"width"_q, 81 } } },
	};
	CHECK(PlanArchiveEvent(state, changingCaptured).status == ArchivePlanStatus::Conflict);
	CHECK(Apply(state, PlanArchiveEvent(state, hydration)));
	CHECK(Message(state).versions[0].missing.content.size() == 1);
	CHECK(Message(state).versions[0].missing.content[0].path.back() == u"footer"_q);
	CHECK(Message(state).versions[0].completeness == ArchiveCompleteness::Partial);
	CHECK(Message(state).versions.back().content == *edit.content);

	auto wrong = hydration;
	wrong.id = u"reopen-prefix"_q;
	wrong.expectedContentHash = ArchiveContentHash(Message(state).versions[0].content);
	wrong.missing.content = base.missing.content;
	CHECK(PlanArchiveEvent(state, wrong).status == ArchivePlanStatus::Conflict);
	wrong = hydration;
	wrong.id = u"unfilled-footer"_q;
	wrong.expectedContentHash = ArchiveContentHash(Message(state).versions[0].content);
	wrong.missing.content.clear();
	CHECK(PlanArchiveEvent(state, wrong).status == ArchivePlanStatus::Conflict);
	wrong = hydration;
	wrong.id = u"wrong-slot-revision"_q;
	wrong.missing.revision = edit.id;
	CHECK(PlanArchiveEvent(state, wrong).status == ArchivePlanStatus::Invalid);

	hydration.id = u"fill-footer-media"_q;
	hydration.expectedContentHash = ArchiveContentHash(Message(state).versions[0].content);
	auto article = hydration.content->rich.toObject().value(u"article"_q).toObject();
	article.insert(u"footer"_q, u"same revision footer"_q);
	hydration.content->rich = QJsonObject{ { u"article"_q, article } };
	hydration.content->media[0].attributes = QJsonObject{
		{ u"dimensions"_q, QJsonObject{ { u"width"_q, 80 }, { u"height"_q, 60 } } },
		{ u"mime"_q, u"application/pdf"_q },
	};
	hydration.missing.content.clear();
	hydration.missing.media.clear();
	CHECK(Apply(state, PlanArchiveEvent(state, hydration)));
	CHECK(Message(state).versions[0].missing.content.empty());
	CHECK(Message(state).versions[0].missing.media.empty());
	CHECK(Message(state).versions[0].missing.revision == base.id);
	CHECK(Message(state).versions[0].id == base.id);
	CHECK(Message(state).versions[0].revision == base.id);
	CHECK(Message(state).versions.size() == 2);
	CHECK(Message(state).versions.back().content == *edit.content);
	wrong = hydration;
	wrong.id = u"more-after-filled"_q;
	wrong.expectedContentHash = ArchiveContentHash(Message(state).versions[0].content);
	article.insert(u"later"_q, u"later paragraph"_q);
	wrong.content->rich = QJsonObject{ { u"article"_q, article } };
	CHECK(PlanArchiveEvent(state, wrong).status == ArchivePlanStatus::Conflict);
	wrong = hydration;
	wrong.id = u"metadata-after-filled"_q;
	wrong.expectedContentHash = ArchiveContentHash(Message(state).versions[0].content);
	wrong.content->media[0].attributes.insert(u"size"_q, 123);
	CHECK(PlanArchiveEvent(state, wrong).status == ArchivePlanStatus::Conflict);
	hydration.id = u"complete"_q;
	hydration.expectedContentHash = ArchiveContentHash(Message(state).versions[0].content);
	hydration.completeness = ArchiveCompleteness::Complete;
	CHECK(Apply(state, PlanArchiveEvent(state, hydration)));
	CHECK(Message(state).versions[0].completeness == ArchiveCompleteness::Complete);

	Section = "absent rich needs an explicit slot";
	state = Empty();
	base = Event(state, u"base"_q, 1);
	base.completeness = ArchiveCompleteness::Partial;
	CHECK(Apply(state, PlanArchiveEvent(state, base)));
	hydration = BoundHydration(state, base, u"undeclared-rich"_q);
	hydration.content->rich = QJsonObject{ { u"blocks"_q, QJsonArray{ u"new paragraph"_q } } };
	CHECK(PlanArchiveEvent(state, hydration).status == ArchivePlanStatus::Conflict);
	state = Empty();
	base.missing.content = { { { u"rich"_q } } };
	CHECK(Apply(state, PlanArchiveEvent(state, base)));
	hydration = BoundHydration(state, base, u"declared-rich"_q);
	hydration.content->rich = QJsonObject{ { u"blocks"_q, QJsonArray{ u"same revision paragraph"_q } } };
	hydration.missing.content.clear();
	hydration.completeness = ArchiveCompleteness::Complete;
	CHECK(Apply(state, PlanArchiveEvent(state, hydration)));
	CHECK(Message(state).versions[0].content == *hydration.content);
	CHECK(Message(state).versions[0].missing.content.empty());
}

void TestHydrationSlotRevisionAndCapturedLeaves() {
	Section = "slot authority resets on a later causal revision";
	auto state = Empty();
	auto base = Event(state, u"base"_q, 1);
	base.completeness = ArchiveCompleteness::Partial;
	base.missing.content = { { { u"rich"_q } } };
	CHECK(Apply(state, PlanArchiveEvent(state, base)));
	auto oldHydration = BoundHydration(state, base, u"old-response"_q);
	oldHydration.content->rich = QJsonObject{ { u"blocks"_q, QJsonArray{ u"old body"_q } } };
	oldHydration.missing.content.clear();
	oldHydration.completeness = ArchiveCompleteness::Complete;
	auto equalEdit = Event(state, u"new-revision"_q, 2, u"A"_q, ArchiveOrigin::AuthoritativeEdit);
	equalEdit.completeness = ArchiveCompleteness::Partial;
	CHECK(Apply(state, PlanArchiveEvent(state, equalEdit)));
	CHECK(Message(state).versions.size() == 1);
	CHECK(Message(state).versions[0].revision == equalEdit.id);
	CHECK(Message(state).versions[0].missing.revision == equalEdit.id);
	CHECK(Message(state).versions[0].missing.content.empty());
	CHECK(PlanArchiveEvent(state, oldHydration).status == ArchivePlanStatus::Stale);
	oldHydration.id = u"borrow-old-slot"_q;
	oldHydration.targetRevision = equalEdit.id;
	CHECK(PlanArchiveEvent(state, oldHydration).status == ArchivePlanStatus::Conflict);
	auto sameRevision = Event(state, u"invent-slot"_q, 2);
	sameRevision.completeness = ArchiveCompleteness::Partial;
	sameRevision.missing.content = base.missing.content;
	CHECK(PlanArchiveEvent(state, sameRevision).status == ArchivePlanStatus::Conflict);
	auto nextCapture = Event(state, u"new-baseline"_q, 3);
	nextCapture.completeness = ArchiveCompleteness::Partial;
	nextCapture.missing.content = base.missing.content;
	CHECK(Apply(state, PlanArchiveEvent(state, nextCapture)));
	CHECK(Message(state).versions[0].missing.revision == nextCapture.id);
	oldHydration.id = u"fill-new-slot"_q;
	oldHydration.targetRevision = nextCapture.id;
	CHECK(Apply(state, PlanArchiveEvent(state, oldHydration)));
	CHECK(Message(state).versions[0].missing.content.empty());
	CHECK(Message(state).versions[0].completeness == ArchiveCompleteness::Complete);

	Section = "declared incomplete subtrees retain every captured leaf";
	state = Empty();
	base = Event(state, u"partial"_q, 1);
	base.completeness = ArchiveCompleteness::Partial;
	base.content->rich = QJsonObject{ { u"article"_q, QJsonObject{
		{ u"paragraphs"_q, QJsonArray{ u"captured"_q, u"captured"_q } },
		{ u"nullable"_q, QJsonValue::Null }, { u"empty"_q, u""_q },
	} } };
	base.missing.content = { { { u"rich"_q, u"article"_q } } };
	CHECK(Apply(state, PlanArchiveEvent(state, base)));
	auto hydration = BoundHydration(state, base, u"fill"_q);
	auto article = base.content->rich.toObject().value(u"article"_q).toObject();
	article.insert(u"footer"_q, u"captured missing footer"_q);
	hydration.content->rich = QJsonObject{ { u"article"_q, article } };
	hydration.missing.content.clear();
	hydration.completeness = ArchiveCompleteness::Complete;
	for (const auto &field : { u"nullable"_q, u"empty"_q, u"paragraphs"_q }) {
		auto wrong = hydration;
		auto changed = article;
		changed.insert(field, u"replacement"_q);
		wrong.content->rich = QJsonObject{ { u"article"_q, changed } };
		CHECK(PlanArchiveEvent(state, wrong).status == ArchivePlanStatus::Conflict);
		changed.remove(field);
		wrong.content->rich = QJsonObject{ { u"article"_q, changed } };
		CHECK(PlanArchiveEvent(state, wrong).status == ArchivePlanStatus::Conflict);
	}
	CHECK(Apply(state, PlanArchiveEvent(state, hydration)));
	CHECK(Message(state).versions[0].content == *hydration.content);
}

void TestCollectionIdentity() {
	Section = "collection identity";
	auto state = Empty();
	for (const auto collection : { ArchiveCollection::Received, ArchiveCollection::Scheduled,
		ArchiveCollection::QuickReply }) {
		auto key = Key();
		key.collection = collection;
		key.collectionId = collection == ArchiveCollection::QuickReply ? 1 : 0;
		CHECK(Apply(state, PlanArchiveEvent(state, Event(state, u"base"_q, 1,
			QString::number(int(collection)), ArchiveOrigin::Baseline, key))));
	}
	CHECK(state.messages.size() == 3);
	for (const auto &message : state.messages) {
		CHECK(message.versions.size() == 1);
		CHECK(message.versions[0].content.text == QString::number(int(message.key.collection)));
	}
	auto unsupported = Key();
	unsupported.collection = ArchiveCollection(100);
	CHECK(!IsArchiveMessageKey(unsupported));
	unsupported.collection = ArchiveCollection::QuickReply;
	CHECK(!IsArchiveMessageKey(unsupported));
	unsupported.collectionId = 1;
	CHECK(IsArchiveMessageKey(unsupported));
	auto anotherShortcut = unsupported;
	anotherShortcut.collectionId = 2;
	CHECK(!SameArchiveMessage(unsupported, anotherShortcut));
	CHECK(Apply(state, PlanArchiveEvent(state, Event(state, u"base"_q, 1,
		u"another shortcut"_q, ArchiveOrigin::Baseline, anotherShortcut))));
	CHECK(state.messages.size() == 4);
}

void TestSharedLiveMessageBlobAndPendingPublication() {
	Section = "shared live-message blob and pending publication";
	auto state = Empty();
	const auto deletedKey = Key();
	const auto liveKey = Key(state.account, 45, 68);
	for (const auto &key : { deletedKey, liveKey }) {
		auto event = Event(state, u"body"_q, 1, u"body"_q, ArchiveOrigin::Baseline, key);
		AddMedia(event);
		CHECK(Apply(state, PlanArchiveEvent(state, event)));
		const auto job = Job(state, QString::number(key.messageId), u"body"_q, key);
		const auto stage = PlanArchiveMediaStage(state, job);
		if (stage) {
			CHECK(Apply(state, stage));
			CHECK(Apply(state, PlanArchiveMediaCompletion(state, job.id, MediaSuccess())));
		} else {
			CHECK(stage.status == ArchivePlanStatus::NoChange);
		}
	}
	CHECK(CountArchiveBlobReferences(state, BlobId).versions == 2);
	CHECK(Apply(state, PlanArchiveEvent(state, Event(state, u"delete"_q, 2, {},
		ArchiveOrigin::AuthoritativeDelete, deletedKey))));
	CHECK(Apply(state, PlanArchivePurge(state, Purge(state, ArchivePurgeKind::Trashbin))));
	CHECK(Message(state, deletedKey).payloadPurged);
	CHECK(Message(state, liveKey).versions.size() == 1);
	CHECK(CountArchiveBlobReferences(state, BlobId).versions == 1);
	CHECK(PlanArchiveGarbageCollection(state).status == ArchivePlanStatus::NoChange);
	CHECK(Apply(state, PlanArchiveEvent(state, Event(state, u"delete"_q, 2, {},
		ArchiveOrigin::AuthoritativeDelete, liveKey))));
	CHECK(Apply(state, PlanArchivePurge(state, Purge(state, ArchivePurgeKind::Trashbin))));
	CHECK(Apply(state, PlanArchiveGarbageCollection(state)));
	const auto newKey = Key(state.account, 45, 69);
	auto event = Event(state, u"new"_q, 1, u"new live message"_q, ArchiveOrigin::Baseline, newKey);
	AddMedia(event);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	const auto job = Job(state, u"new-job"_q, u"new"_q, newKey);
	CHECK(Apply(state, PlanArchiveMediaStage(state, job)));
	CHECK(PlanArchiveMediaCompletion(state, job.id, MediaSuccess()).status == ArchivePlanStatus::Conflict);
	CHECK(Message(state, newKey).versions[0].media[0].state == ArchiveMediaState::Staged);
	CHECK(Apply(state, CompleteArchiveGarbageCollection(state, { BlobId })));
	CHECK(Apply(state, PlanArchiveMediaCompletion(state, job.id, MediaSuccess())));
	CHECK(state.blobs.size() == 1);
	CHECK(CountArchiveBlobReferences(state, BlobId).versions == 1);
	CHECK(Message(state, deletedKey).versions.empty());
	CHECK(Message(state, liveKey).versions.empty());
}

void TestCausalSourceAndHydrationBinding() {
	Section = "causal source and hydration binding";
	auto state = Empty();
	auto event = Event(state, u"a1"_q, 1);
	event.completeness = ArchiveCompleteness::Partial;
	event.source = ArchiveSource{ ArchiveSourceKind::ComposeMarkdown,
		ArchiveContentHash(*event.content), u"original"_q, QByteArray("A"), event.id };
	const auto originalSource = event.source;
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	auto hydration = Event(state, u"hydrate-old"_q, 1, u"A"_q, ArchiveOrigin::Hydration);
	hydration.targetVersion = u"a1"_q;
	hydration.targetRevision = u"a1"_q;
	hydration.expectedContentHash = ArchiveContentHash(*event.content);
	event = Event(state, u"a2"_q, 2, u"A"_q, ArchiveOrigin::AuthoritativeEdit);
	event.completeness = ArchiveCompleteness::Partial;
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	CHECK(Message(state).versions.size() == 1);
	CHECK(Message(state).versions[0].id == u"a1"_q);
	CHECK(Message(state).versions[0].revision == u"a2"_q);
	CHECK(!Message(state).versions[0].source);
	CHECK(PlanArchiveEvent(state, hydration).status == ArchivePlanStatus::Stale);
	hydration.id = u"hydrate-current"_q;
	hydration.targetRevision = u"a2"_q;
	hydration.source = originalSource;
	CHECK(Apply(state, PlanArchiveEvent(state, hydration)));
	CHECK(!ArchiveSourceMatches(Message(state).versions[0]));
	hydration.id = u"source-current"_q;
	hydration.source->revision = u"a2"_q;
	CHECK(Apply(state, PlanArchiveEvent(state, hydration)));
	CHECK(ArchiveSourceMatches(Message(state).versions[0]));
	auto invalidSourceVersion = Message(state).versions[0];
	invalidSourceVersion.id.clear();
	CHECK(!ArchiveSourceMatches(invalidSourceVersion));
	event = Event(state, u"baseline-newer"_q, 3);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	CHECK(Message(state).versions.size() == 1);
	CHECK(Message(state).versions[0].revision == u"baseline-newer"_q);
	CHECK(!Message(state).versions[0].source);
	hydration.id = u"hydration-before-baseline"_q;
	CHECK(PlanArchiveEvent(state, hydration).status == ArchivePlanStatus::Stale);
	event = Event(state, u"b"_q, 4, u"B"_q, ArchiveOrigin::AuthoritativeEdit);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	event = Event(state, u"a3"_q, 5, u"A"_q, ArchiveOrigin::AuthoritativeEdit);
	event.source = originalSource;
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	CHECK(Message(state).versions.size() == 3);
	CHECK(Message(state).versions.back().content.text == u"A"_q);
	CHECK(Message(state).versions.back().revision == u"a3"_q);
	CHECK(Message(state).versions.back().source->contentHash
		== ArchiveContentHash(Message(state).versions.back().content));
	CHECK(!ArchiveSourceMatches(Message(state).versions.back()));
}

void TestOpaqueContentIsRetained() {
	Section = "opaque content retention";
	auto state = Empty();
	auto event = Event(state, u"opaque"_q, 1, {});
	event.completeness = ArchiveCompleteness::Opaque;
	event.observation.payloadFormat = u"client-tl:200"_q;
	event.observation.payload = QByteArray("raw transport with unknown content");
	CHECK(PlanArchiveEvent(state, event).status == ArchivePlanStatus::Invalid);
	event.content->opaqueFormat = u"unknown-content:9"_q;
	event.content->opaque = QByteArray("semantic opaque A");
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	auto refresh = event;
	refresh.id = u"refresh-opaque"_q;
	refresh.observation.payload = QByteArray("rotated transport references");
	CHECK(Apply(state, PlanArchiveEvent(state, refresh)));
	CHECK(Message(state).versions.size() == 1);
	auto edit = event;
	edit.id = u"edit-opaque"_q;
	edit.order = 2;
	edit.origin = ArchiveOrigin::AuthoritativeEdit;
	edit.content->opaque = QByteArray("semantic opaque B");
	CHECK(Apply(state, PlanArchiveEvent(state, edit)));
	CHECK(Message(state).versions.size() == 2);
	CHECK(Message(state).versions[0].content.opaque == QByteArray("semantic opaque A"));
	CHECK(Message(state).versions[1].content.opaque == QByteArray("semantic opaque B"));
	CHECK(!ArchiveSourceMatches(Message(state).versions[1]));
}

void TestChangedRefreshRetainsCapturedBodies() {
	Section = "changed refresh retention";
	auto state = Empty();
	auto event = Event(state, u"base-a"_q, 1);
	AddMedia(event);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	const auto job = Job(state, u"old-version-job"_q, u"base-a"_q);
	CHECK(Apply(state, PlanArchiveMediaStage(state, job)));
	auto refresh = Event(state, u"refresh-b"_q, 2, u"B"_q);
	AddMedia(refresh);
	refresh.completeness = ArchiveCompleteness::Partial;
	refresh.targetVersion = u"base-a"_q;
	refresh.targetRevision = u"base-a"_q;
	refresh.expectedContentHash = ArchiveContentHash(*event.content);
	CHECK(Apply(state, PlanArchiveEvent(state, refresh)));
	CHECK(Message(state).versions.size() == 2);
	CHECK(Message(state).versions[0].content.text == u"A"_q);
	CHECK(Message(state).versions[0].completeness == ArchiveCompleteness::Complete);
	CHECK(Message(state).versions[1].content.text == u"B"_q);
	CHECK(Message(state).versions[1].completeness == ArchiveCompleteness::Partial);
	CHECK(Message(state).versions[0].origin == ArchiveOrigin::Baseline);
	CHECK(Message(state).versions[1].origin == ArchiveOrigin::Baseline);
	CHECK(state.jobs[0].state == ArchiveJobState::Staged);
	CHECK(Apply(state, PlanArchiveMediaCompletion(state, job.id, MediaSuccess())));
	CHECK(Message(state).versions[0].media[0].state == ArchiveMediaState::Complete);
	CHECK(Message(state).versions[1].media[0].state == ArchiveMediaState::Missing);
	CHECK(CountArchiveBlobReferences(state, BlobId).versions == 1);
	CHECK(PlanArchiveGarbageCollection(state).status == ArchivePlanStatus::NoChange);
	CHECK(Apply(state, PlanArchivePurge(state, Purge(state, ArchivePurgeKind::EditHistory))));
	CHECK(Message(state).versions.size() == 1);
	CHECK(Message(state).versions[0].content.text == u"B"_q);
	CHECK(CountArchiveBlobReferences(state, BlobId).versions == 1);
	CHECK(PlanArchiveGarbageCollection(state).status == ArchivePlanStatus::NoChange);
}

void TestSharedCaptureAndPurgeReferenceRepair() {
	Section = "shared capture and purge references";
	auto state = Empty();
	for (const auto &entry : { std::pair(u"a"_q, u"A"_q), std::pair(u"b"_q, u"B"_q) }) {
		auto event = Event(state, entry.first, entry.first == u"a"_q ? 1 : 2, entry.second,
			entry.first == u"a"_q ? ArchiveOrigin::Baseline : ArchiveOrigin::AuthoritativeEdit);
		AddMedia(event);
		CHECK(Apply(state, PlanArchiveEvent(state, event)));
		CHECK(Apply(state, PlanArchiveMediaStage(state, Job(state, entry.first, entry.first))));
	}
	const auto completion = PlanArchiveMediaCompletion(state, u"a"_q, MediaSuccess());
	CHECK(completion.releaseStagedJobs.empty());
	CHECK(Apply(state, completion));
	CHECK(state.jobs[1].state == ArchiveJobState::Staged);
	CHECK(Message(state).versions[1].media[0].state == ArchiveMediaState::Staged);
	CHECK(Apply(state, PlanArchiveMediaCompletion(state, u"b"_q, MediaSuccess())));
	CHECK(CountArchiveBlobReferences(state, BlobId).versions == 2);
	state.messages[0].versions.back().media[0].state = ArchiveMediaState::Missing;
	state.messages[0].versions.back().media[0].blobId.clear();
	CHECK(IsArchiveState(state));
	CHECK(Apply(state, PlanArchivePurge(state, Purge(state, ArchivePurgeKind::EditHistory))));
	CHECK(Message(state).versions.size() == 1);
	CHECK(Message(state).versions[0].media[0].state == ArchiveMediaState::Complete);
	CHECK(CountArchiveBlobReferences(state, BlobId).versions == 1);
	CHECK(PlanArchiveGarbageCollection(state).status == ArchivePlanStatus::NoChange);
}

}

int main() {
	TestCausality();
	TestObservationAndHydration();
	TestIdentityAndSource();
	TestTransactions();
	TestMediaAndPurge();
	TestPurgeRacesAndScopes();
	TestLeaseAndCollectionRaces();
	TestExplicitMigrationPurge();
	TestCanonicalFields();
	TestZeroAndFailedMedia();
	TestHydrationCannotLoseKnownContent();
	TestHydrationReviewFixtures();
	TestHydrationSemanticPreservation();
	TestHydrationOrderedAndOpaqueContent();
	TestHydrationLaterRevisionAdditions();
	TestDeclaredHydrationSlots();
	TestHydrationSlotRevisionAndCapturedLeaves();
	TestCollectionIdentity();
	TestSharedLiveMessageBlobAndPendingPublication();
	TestOpaqueContentIsRetained();
	TestChangedRefreshRetainsCapturedBodies();
	TestSharedCaptureAndPurgeReferenceRepair();
	TestCausalSourceAndHydrationBinding();
	return Finish();
}
