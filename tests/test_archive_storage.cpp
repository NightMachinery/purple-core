/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "tests/archive_storage_test_support.h"
#include "purple/purple_archive_media_lifecycle.h"
#include "purple/purple_archive_views.h"

#include <algorithm>

using namespace ArchiveTest;
using namespace ArchiveStorageTest;

namespace {

void Envelopes() {
	Section = "envelopes";
	const auto context = ArchiveEnvelopeContext{ Empty().account, u"store-1"_q,
		ArchiveRecordKind::Version, "2/45/0/0/67/original/ev1" };
	auto crypto = FakeCrypto();
	const auto bytes = QByteArray("private body\0source", 19);
	const auto sealed = SealArchiveEnvelope(context, u"key-1"_q, bytes, crypto);
	CHECK(sealed);
	CHECK(!sealed.bytes.contains(bytes));
	CHECK(OpenArchiveEnvelope(context, u"key-1"_q, sealed.bytes, crypto).bytes == bytes);
	CHECK(SealArchiveEnvelope(context, u"key-1"_q, bytes, crypto).bytes != sealed.bytes);
	for (auto length = qsizetype(0); length < sealed.bytes.size(); ++length) {
		CHECK(!OpenArchiveEnvelope(context, u"key-1"_q, sealed.bytes.left(length), crypto));
	}
	for (auto i = qsizetype(0); i != sealed.bytes.size(); ++i) {
		auto corrupted = sealed.bytes;
		corrupted[i] ^= 0x40;
		CHECK(!OpenArchiveEnvelope(context, u"key-1"_q, corrupted, crypto));
	}
	CHECK(!OpenArchiveEnvelope(context, u"key-1"_q, sealed.bytes + "x", crypto));
	CHECK(OpenArchiveEnvelope(context, u"key-2"_q, sealed.bytes, crypto).status == ArchiveEnvelopeStatus::WrongKey);
	auto other = context;
	other.account.userId++;
	CHECK(OpenArchiveEnvelope(other, u"key-1"_q, sealed.bytes, crypto).status == ArchiveEnvelopeStatus::CryptoFailure);
	other = context;
	other.account.environment = 1;
	CHECK(!OpenArchiveEnvelope(other, u"key-1"_q, sealed.bytes, crypto));
	other = context;
	other.storeId = u"store-2"_q;
	CHECK(!OpenArchiveEnvelope(other, u"key-1"_q, sealed.bytes, crypto));
	other = context;
	other.kind = ArchiveRecordKind::Message;
	CHECK(!OpenArchiveEnvelope(other, u"key-1"_q, sealed.bytes, crypto));
	other = context;
	other.rowId += "x";
	CHECK(!OpenArchiveEnvelope(other, u"key-1"_q, sealed.bytes, crypto));
	auto future = sealed.bytes;
	future[9] = 2;
	CHECK(OpenArchiveEnvelope(context, u"key-1"_q, future, {}).status == ArchiveEnvelopeStatus::NewerVersion);
	future = sealed.bytes;
	future[11] = 2;
	CHECK(OpenArchiveEnvelope(context, u"key-1"_q, future, {}).status == ArchiveEnvelopeStatus::UnsupportedSuite);
	CHECK(!SealArchiveEnvelope(context, u"key-1"_q, bytes, {}));
	crypto.random = [](int) { return std::optional(QByteArray(3, 'x')); };
	CHECK(SealArchiveEnvelope(context, u"key-1"_q, bytes, crypto).status == ArchiveEnvelopeStatus::CryptoFailure);
	crypto = FakeCrypto();
	crypto.sealAes256Gcm = [](const auto &, const auto &, const auto &, const auto &) {
		return std::optional(ArchiveSealedBytes{ {}, {} });
	};
	CHECK(!SealArchiveEnvelope(context, u"key-1"_q, bytes, crypto));
	crypto = FakeCrypto();
	crypto.openAes256Gcm = [](const auto &, const auto &, const auto &, const auto &) {
		return std::optional(QByteArray("wrong-size"));
	};
	CHECK(!OpenArchiveEnvelope(context, u"key-1"_q, sealed.bytes, crypto));
	CHECK(!SealArchiveEnvelope(context, u"key-1"_q, QByteArray(kArchiveEnvelopeByteLimit + 1, 'a'), FakeCrypto()));
	CHECK(SealArchiveEnvelope(context, u"key-1"_q, {}, FakeCrypto()));
	CHECK(!IsArchiveEnvelopeContext({ { 0, 0 }, u"store"_q, ArchiveRecordKind::Account, "x" }));
	CHECK(!IsArchiveEnvelopeContext({ Empty().account, u"store"_q, ArchiveRecordKind::Account, {} }));
	CHECK(!ArchiveMediaChunkContext(Empty().account, u"store"_q, u"bad"_q, 1, 0));
	CHECK(ArchiveMediaChunkSize(0, 0) == 0);
	CHECK(!ArchiveMediaChunkSize(0, 1));
	CHECK(ArchiveMediaChunkSize(kArchiveMediaChunkBytes + 7, 0) == kArchiveMediaChunkBytes);
	CHECK(ArchiveMediaChunkSize(kArchiveMediaChunkBytes + 7, 1) == 7);
	CHECK(!ArchiveMediaChunkSize(kArchiveMediaChunkBytes + 7, 2));
	const auto chunk0 = ArchiveMediaChunkContext(Empty().account, u"store"_q, BlobId,
		kArchiveMediaChunkBytes + 7, 0);
	const auto chunk1 = ArchiveMediaChunkContext(Empty().account, u"store"_q, BlobId,
		kArchiveMediaChunkBytes + 7, 1);
	CHECK(chunk0 && chunk1);
	const auto media = SealArchiveEnvelope(*chunk0, u"key-1"_q, QByteArray(kArchiveMediaChunkBytes, 'm'), FakeCrypto());
	CHECK(!OpenArchiveEnvelope(*chunk1, u"key-1"_q, media.bytes, FakeCrypto()));
	CHECK(ArchiveMediaChunkSize(std::numeric_limits<uint64_t>::max(),
		(std::numeric_limits<uint64_t>::max() - 1) / kArchiveMediaChunkBytes).has_value());
}

void Keys() {
	Section = "key contract";
	const auto key = QByteArray(kArchiveDataKeyBytes, 'k');
	auto crypto = FakeCrypto();
	const auto wrapped = WrapArchiveDataKey(Empty().account, u"store"_q,
		u"data-key"_q, u"platform-key"_q, key, crypto);
	CHECK(wrapped);
	CHECK(!wrapped.bytes.contains(key));
	CHECK(UnwrapArchiveDataKey(Empty().account, u"store"_q, u"data-key"_q,
		u"platform-key"_q, wrapped.bytes, crypto).bytes == key);
	CHECK(!UnwrapArchiveDataKey(Empty().account, u"store"_q, u"data-key-2"_q,
		u"platform-key"_q, wrapped.bytes, crypto));
	CHECK(!UnwrapArchiveDataKey(Empty().account, u"store"_q, u"data-key"_q,
		u"platform-key-2"_q, wrapped.bytes, crypto));
	CHECK(!UnwrapArchiveDataKey({ 0, 999 }, u"store"_q, u"data-key"_q,
		u"platform-key"_q, wrapped.bytes, crypto));
	CHECK(!WrapArchiveDataKey(Empty().account, u"store"_q,
		u"data-key"_q, u"platform-key"_q, key.left(31), crypto));
	const auto rotated = WrapArchiveDataKey(Empty().account, u"store"_q,
		u"data-key"_q, u"platform-new"_q, key, crypto);
	CHECK(rotated && rotated.bytes != wrapped.bytes);
	CHECK(UnwrapArchiveDataKey(Empty().account, u"store"_q, u"data-key"_q,
		u"platform-new"_q, rotated.bytes, crypto).bytes == key);
	const auto openDesktop = [](ArchiveKeyFacts facts) {
		facts.capability = { ArchiveKeyPlatform::Desktop, 0, true };
		return PlanArchiveKeyOpen(facts);
	};
	CHECK(PlanArchiveKeyOpen({}) == ArchiveKeyAction::UnsupportedPlatform);
	CHECK(openDesktop({}) == ArchiveKeyAction::Create);
	CHECK(openDesktop({ true, true, true }) == ArchiveKeyAction::Unlock);
	CHECK(openDesktop({ true, true, false }) == ArchiveKeyAction::Unreadable);
	CHECK(openDesktop({ true, false, true }) == ArchiveKeyAction::Unreadable);
	CHECK(openDesktop({ false, true, true }) == ArchiveKeyAction::Unreadable);
	CHECK(openDesktop({ true, true, true, false, 2 }) == ArchiveKeyAction::ReadOnly);
	CHECK(openDesktop({ true, true, true, false, 1, 2 }) == ArchiveKeyAction::ReadOnly);
	CHECK(openDesktop({ true, true, true, true, 2, 2 }) == ArchiveKeyAction::FinishRemoval);
	CHECK(openDesktop({ false, false, true, false, 0 }) == ArchiveKeyAction::Invalid);
	for (auto api : { 21, 22, 23, 34 }) {
		auto facts = ArchiveKeyFacts();
		facts.capability = { ArchiveKeyPlatform::Android, api, true };
		CHECK(PlanArchiveKeyOpen(facts) == (api < 23 ? ArchiveKeyAction::UnsupportedPlatform : ArchiveKeyAction::Create));
	}
	auto unavailable = ArchiveKeyFacts();
	unavailable.capability.wrappingAvailable = false;
	CHECK(PlanArchiveKeyOpen(unavailable) == ArchiveKeyAction::UnsupportedPlatform);
	const auto derivation = ArchiveKeyDerivationFor(Empty().account, u"store"_q, u"profile"_q, ArchiveKeyPurpose::DesktopWrapping);
	CHECK(derivation && derivation->outputBytes == 32);
	CHECK(!ArchiveKeyDerivationFor(Empty().account, u"store"_q, {}, ArchiveKeyPurpose::DesktopWrapping));
	const auto index = ArchiveKeyDerivationFor(Empty().account, u"store"_q, {}, ArchiveKeyPurpose::IndexTokens);
	CHECK(index && index->salt != derivation->salt);
	CHECK(ArchiveKeyDerivationFor({ 0, 124 }, u"store"_q, u"profile"_q, ArchiveKeyPurpose::DesktopWrapping)->info != derivation->info);
}

void Removal() {
	Section = "logout removal crash matrix";
	const auto epoch = ArchiveStoreEpoch{ Empty().account, u"store"_q, 8 };
	CHECK(CanRunArchiveWork(epoch, epoch));
	const auto started = BeginArchiveRemoval(epoch, u"logout-1"_q);
	CHECK(started);
	CHECK(started->epoch.generation == 9);
	CHECK(!CanRunArchiveWork(epoch, started->epoch));
	CHECK(!BeginArchiveRemoval(started->epoch, u"logout-2"_q));
	auto exhausted = epoch;
	exhausted.generation = std::numeric_limits<uint64_t>::max();
	CHECK(!BeginArchiveRemoval(exhausted, u"logout"_q));
	const auto tombstone = EncodeArchiveRemovalTombstone(*started);
	CHECK(tombstone);
	const auto resumed = ResumeArchiveRemoval(*tombstone);
	CHECK(resumed && resumed->action == ArchiveRemovalAction::FenceWork);
	CHECK(resumed->epoch == started->epoch);
	CHECK(!ResumeArchiveRemoval(tombstone->left(tombstone->size() - 1)));
	CHECK(!ResumeArchiveRemoval(QByteArray("{}")));
	auto future = QJsonDocument::fromJson(*tombstone).object();
	future[u"version"_q] = 2;
	CHECK(!ResumeArchiveRemoval(QJsonDocument(future).toJson()));
	future[u"version"_q] = 1;
	future[u"generation"_q] = u"09"_q;
	CHECK(!ResumeArchiveRemoval(QJsonDocument(future).toJson()));
	auto removal = *started;
	auto actions = 0;
	while (removal.action != ArchiveRemovalAction::Done) {
		++actions;
		auto ack = Ack(removal);
		ack.account.userId++;
		CHECK(AdvanceArchiveRemoval(removal, ack).status == ArchiveRemovalStatus::Invalid);
		ack = Ack(removal);
		ack.storeId += u"other"_q;
		CHECK(AdvanceArchiveRemoval(removal, ack).status == ArchiveRemovalStatus::Invalid);
		ack = Ack(removal);
		ack.generation++;
		CHECK(AdvanceArchiveRemoval(removal, ack).status == ArchiveRemovalStatus::Invalid);
		ack = Ack(removal);
		ack.operationId += u"other"_q;
		CHECK(AdvanceArchiveRemoval(removal, ack).status == ArchiveRemovalStatus::Invalid);
		ack = Ack(removal);
		ack.success = false;
		const auto failed = AdvanceArchiveRemoval(removal, ack);
		CHECK(failed.status == ArchiveRemovalStatus::Wait && failed.state == removal);
		if (removal.action != ArchiveRemovalAction::DrainReadersAndClose) {
			ack = Ack(removal);
			ack.durable = false;
			const auto nondurable = AdvanceArchiveRemoval(removal, ack);
			CHECK(nondurable.status == ArchiveRemovalStatus::Wait && nondurable.state == removal);
		} else {
			for (auto i = 0; i != 3; ++i) {
				ack = Ack(removal);
				if (i == 0) { ack.readers = 1; }
				if (i == 1) { ack.writers = 1; }
				if (i == 2) { ack.databaseClosed = false; }
				CHECK(AdvanceArchiveRemoval(removal, ack).status == ArchiveRemovalStatus::Wait);
			}
		}
		const auto restart = ResumeArchiveRemoval(*EncodeArchiveRemovalTombstone(removal));
		CHECK(restart && restart->action == ArchiveRemovalAction::FenceWork);
		CHECK(!CanRunArchiveWork(epoch, restart->epoch));
		const auto next = AdvanceArchiveRemoval(removal, Ack(removal));
		CHECK(next.status == ArchiveRemovalStatus::Advanced);
		removal = next.state;
	}
	CHECK(actions == 12);
	CHECK(AdvanceArchiveRemoval(removal, Ack(removal)).status == ArchiveRemovalStatus::Invalid);
	auto relogin = epoch;
	relogin.generation = removal.epoch.generation;
	relogin.storeId = u"fresh-store"_q;
	CHECK(CanRunArchiveWork(relogin, relogin));
	CHECK(!CanRunArchiveWork(epoch, relogin));
	auto otherAccount = relogin;
	otherAccount.account.userId++;
	CHECK(!CanRunArchiveWork(relogin, otherAccount));
	CHECK(TotalArchiveUsage({ 10, 20, 30, 40, 50 }) == 150);
	CHECK(TotalArchiveUsage({}) == 0);
	CHECK(!TotalArchiveUsage({ std::numeric_limits<uint64_t>::max(), 1 }));
	auto scan = ArchiveUsageScan{ epoch };
	for (auto start = 0; start < 100000; start += kArchiveSqlPageLimit) {
		auto page = std::vector<ArchiveUsageEntry>();
		for (auto i = start; i < std::min(start + kArchiveSqlPageLimit, 100000); ++i) {
			page.push_back({ QString::number(i + 1).rightJustified(20, '0'), 2 });
		}
		const auto next = ContinueArchiveUsage(scan, epoch, page, start + kArchiveSqlPageLimit >= 100000);
		CHECK(next);
		scan = *next;
	}
	CHECK(scan.complete && scan.bytes == 200000);
	CHECK(!ContinueArchiveUsage(scan, epoch, {}, true));
	CHECK(!ContinueArchiveUsage({ epoch }, epoch, std::vector<ArchiveUsageEntry>(129), false));
	CHECK(!ContinueArchiveUsage({ epoch }, relogin, {}, false));
	CHECK(!ContinueArchiveUsage({ epoch, {}, std::numeric_limits<uint64_t>::max() }, epoch, { { u"a"_q, 1 } }, true));
}

void RowsAndPlans() {
	Section = "SQL row codecs and plans";
	auto state = Empty();
	state.preserved[u"futureMeta"_q] = u"preserve"_q;
	const auto security = Security();
	CHECK(InitializeArchiveSql(state, security));
	const auto one = ArchiveSqlStatement{ u"SELECT 1"_q, {}, ArchiveSqlExpectation::OneRow };
	CHECK(CheckArchiveSqlResult(one, true, 1, 99, 0));
	CHECK(!CheckArchiveSqlResult(one, false, 1, 0, 0));
	CHECK(!CheckArchiveSqlResult(one, true, 0, 0, 0));
	CHECK(!CheckArchiveSqlResult(one, true, 2, 0, 0));
	CHECK(!CheckArchiveSqlResult(one, true, 1, 0, 1));
	const auto write = ArchiveSqlStatement{ u"UPDATE a SET b=1"_q, {}, ArchiveSqlExpectation::OneChange };
	CHECK(CheckArchiveSqlResult(write, true, 0, 1, 12));
	CHECK(!CheckArchiveSqlResult(write, true, 0, 0, 0));
	CHECK(!CheckArchiveSqlResult(write, true, 0, 2, 2));
	CHECK(!AcknowledgeArchiveSqlCommit(true, false));
	CHECK(!AcknowledgeArchiveSqlCommit(false, true));
	CHECK(AcknowledgeArchiveSqlCommit(true, true));
	CHECK(ArchiveSqlMessageId(Key(), security) != ArchiveSqlMessageId(Key({ 0, 124 }), security));
	auto otherStore = security;
	otherStore.storeId += u"-other"_q;
	CHECK(ArchiveSqlMessageId(Key(), security) != ArchiveSqlMessageId(Key(), otherStore));
	auto noMac = security;
	noMac.hmacSha256 = {};
	CHECK(!InitializeArchiveSql(state, noMac));
	CHECK(ArchiveSqlReadPage(ArchiveRecordKind::Message, 129).sql.isEmpty());
	CHECK(CheckArchiveSqlVersion(0) == ArchiveSqlStatus::Invalid);
	CHECK(CheckArchiveSqlVersion(1) == ArchiveSqlStatus::SchemaMismatch);
	CHECK(CheckArchiveSqlVersion(2) == ArchiveSqlStatus::Ready);
	CHECK(CheckArchiveSqlVersion(3) == ArchiveSqlStatus::ReadOnly);
	CHECK(CheckArchiveSqlVersion(int64(1) + (int64(1) << 32)) == ArchiveSqlStatus::ReadOnly);
	auto event = Event(state, u"first"_q, 1);
	event.content->preserved[u"futureBody"_q] = u"keep"_q;
	const auto plan = PlanArchiveEvent(state, event);
	CHECK(plan);
	const auto program = TranslateArchiveSql(plan, security);
	CHECK(program);
	CHECK(program.statements.front().sql == u"BEGIN IMMEDIATE"_q);
	CHECK(program.statements.back().sql == u"COMMIT"_q);
	CHECK(program.statements[1].expectation == ArchiveSqlExpectation::OneRow);
	CHECK(program.statements[program.statements.size() - 2].expectation == ArchiveSqlExpectation::OneChange);
	for (const auto &sql : program.statements) {
		CHECK(sql.sql.count('?') == int(sql.binds.size()));
	}
	auto broken = security;
	broken.crypto = {};
	const auto refused = TranslateArchiveSql(plan, broken);
	CHECK(!refused && refused.statements.empty());
	CHECK(!TranslateArchiveSql({}, security));
	auto tooMany = plan;
	tooMany.next.messages.push_back(tooMany.next.messages[0]);
	tooMany.next.messages.back().key.messageId++;
	CHECK(TranslateArchiveSql(tooMany, security).status == ArchiveSqlStatus::TooLarge);
	tooMany = plan;
	tooMany.next.messages[0].receipts.resize(100000);
	CHECK(TranslateArchiveSql(tooMany, security).status == ArchiveSqlStatus::TooLarge);
	auto hugeHeader = Empty();
	hugeHeader.accountGeneration = std::numeric_limits<uint64_t>::max();
	CHECK(!BeginArchiveSqlPurge(hugeHeader, Purge(hugeHeader, ArchivePurgeKind::Trashbin), {}, u"purge"_q, security).program);
	CHECK(!PlanArchiveSqlGarbage(state, std::vector<ArchiveBlob>(129), security).program);
	CHECK(!CompleteArchiveSqlGarbage(state, { ArchiveBlob{ BlobId, 7, ArchiveBlobState::Complete } }, security).program);
	auto badPlan = plan;
	badPlan.next.revision++;
	CHECK(!TranslateArchiveSql(badPlan, security));
	badPlan = plan;
	badPlan.next.version++;
	CHECK(TranslateArchiveSql(badPlan, security).status == ArchiveSqlStatus::ReadOnly);
	CHECK(ArchiveSqlDeletedTranscript(state.account, Key().chat, 0, security).sql.isEmpty());
	CHECK(ArchiveSqlDeletedTranscript(state.account, Key().chat, 1001, security).sql.isEmpty());
	CHECK(!ArchiveSqlDeletedTranscript(state.account, Key().chat, 2, security).sql.isEmpty());
	CHECK(ArchiveSqlMessageId(Key(), security) != ArchiveSqlMessageId(Key({ 0, 123 }, 45, 67, ArchiveChatKind::Group), security));
	CHECK(IsArchiveHash(ArchiveSqlMessageId(Key(), security)));
	CHECK(!ArchiveSqlMessageId(Key(), security).contains(u"original"_q));
	CHECK(ArchiveSqlChatId({ ArchiveChatKind::Channel, 0 }, state.account, security).isEmpty());
	const auto row = ArchiveRow{ ArchiveVersionSnapshot{ plan.next.messages[0].key,
		plan.next.messages[0].versions[0] } };
	const auto encoded = EncodeArchiveRow(row);
	CHECK(encoded);
	const auto decoded = DecodeArchiveRow(*encoded);
	CHECK(decoded.status == ArchiveCodecStatus::Valid && decoded.row);
	CHECK(EncodeArchiveRow(*decoded.row) == encoded);
	for (const auto &value : std::vector<ArchiveRowValue>{ state, plan.next.messages[0],
		plan.next.messages[0].receipts[0], ArchiveBlob{ BlobId, 7 },
		Job(state, u"job"_q, u"first"_q),
		ArchiveReaderLease{ u"reader"_q, Key(), u"first"_q, BlobId }, ArchiveChatGeneration{ Key().chat, 1 } }) {
		const auto bytes = EncodeArchiveRow({ value });
		CHECK(bytes);
		const auto restored = DecodeArchiveRow(*bytes);
		CHECK(restored.status == ArchiveCodecStatus::Valid && restored.row);
		CHECK(restored.row && restored.row->value == value);
	}
	auto json = QJsonDocument::fromJson(*encoded).object();
	json[u"schema"_q] = 2;
	CHECK(DecodeArchiveRow(QJsonDocument(json).toJson()).status == ArchiveCodecStatus::NewerSchema);
	json[u"schema"_q] = 1;
	json[u"kind"_q] = 99;
	CHECK(DecodeArchiveRow(QJsonDocument(json).toJson()).status == ArchiveCodecStatus::Unsupported);
	json[u"kind"_q] = int(ArchiveRecordKind::Version);
	json[u"futureMeaning"_q] = true;
	CHECK(DecodeArchiveRow(QJsonDocument(json).toJson()).status == ArchiveCodecStatus::Unsupported);
	CHECK(!RestoreArchiveSqlSlice({}, security));
	const auto headerSlice = RestoreArchiveSqlSlice({ { u"account"_q, 0, { state } } }, security);
	CHECK(headerSlice && headerSlice->state == state && headerSlice->versionMetadata.empty());
	CHECK(!RestoreArchiveSqlSlice({ { u"wrong"_q, 0, { state } } }, security));
	CHECK(!RestoreArchiveSqlSlice({ { u"account"_q, 0, { state } }, { u"account"_q, 0, { state } } }, security));
	const auto gap = ArchiveCaptureGap{ state.account, security.storeId, u"gap-1"_q, 123, u"disk-full"_q };
	CHECK(PlanArchiveCaptureGap(gap, security));
	auto wrongGap = gap;
	wrongGap.storeId = u"other-store"_q;
	CHECK(!PlanArchiveCaptureGap(wrongGap, security));
	CHECK(ArchiveSqlReadCaptureGaps(0).sql.isEmpty());
	CHECK(!ArchiveSqlReadCaptureGaps(10).sql.isEmpty());
}


template <typename Call>
void FailEveryDerivation(Call call) {
	const auto base = Security();
	auto calls = 0;
	auto security = base;
	security.hmacSha256 = [&](const QByteArray &bytes) { ++calls; return base.hmacSha256(bytes); };
	CHECK(call(security));
	CHECK(calls > 0);
	const auto total = calls;
	for (auto failing = 1; failing <= total; ++failing) {
		calls = 0;
		security.hmacSha256 = [&](const QByteArray &bytes) -> std::optional<QByteArray> {
			if (++calls == failing) { return {}; }
			return base.hmacSha256(bytes);
		};
		const auto refused = call(security);
		CHECK(!refused && refused.statements.empty());
	}
	security.hmacSha256 = [](const QByteArray &) { return std::optional(QByteArray(31, 'x')); };
	const auto shortMac = call(security);
	CHECK(!shortMac && shortMac.statements.empty());
}

ArchiveSqlProgram ReadAsProgram(ArchiveSqlStatement statement) {
	return statement.sql.isEmpty() ? ArchiveSqlProgram()
		: ArchiveSqlProgram{ ArchiveSqlStatus::Ready, { std::move(statement) } };
}

void SelectiveDerivationFailures() {
	Section = "every SQL HMAC failure refuses the complete operation";
	const auto state = Empty();
	const auto security = Security();
	auto event = Event(state, u"body"_q, 1);
	AddMedia(event);
	const auto plan = PlanArchiveEvent(state, event);
	FailEveryDerivation([&](const auto &key) { return InitializeArchiveSql(state, key); });
	FailEveryDerivation([&](const auto &key) { return TranslateArchiveSql(plan, key); });
	const auto gap = ArchiveCaptureGap{ state.account, security.storeId, u"gap"_q, 123, u"source-unavailable"_q };
	FailEveryDerivation([&](const auto &key) { return PlanArchiveCaptureGap(gap, key); });
	auto request = Purge(state, ArchivePurgeKind::Trashbin, Key().chat);
	request.scope.migrationAliases.push_back({ ArchiveChatKind::Group, 46 });
	const auto epochs = std::vector<ArchiveChatGeneration>{ { Key().chat, 0 }, { request.scope.migrationAliases[0], 0 } };
	FailEveryDerivation([&](const auto &key) { return BeginArchiveSqlPurge(state, request, epochs, u"scope"_q, key).program; });
	const auto begin = BeginArchiveSqlPurge(state, Purge(state, ArchivePurgeKind::Trashbin), {}, u"purge"_q, security);
	const auto snapshot = ArchiveVersionSnapshot{ Key(), plan.next.messages[0].versions[0] };
	const auto id = ArchiveSqlChildId(Key(), ArchiveIndexKind::Version, snapshot.version.id, security);
	const auto frame = SealArchiveSqlRow(state.account, id, snapshot, security);
	CHECK(frame);
	const auto rows = std::vector<ArchiveSqlPurgeRow>{ { { id, 0, { snapshot } }, *frame } };
	FailEveryDerivation([&](const auto &key) { return ContinueArchiveSqlPurge(begin.next, rows, key).program; });
	FailEveryDerivation([&](const auto &key) { return ContinueArchiveSqlPurge(begin.next, {}, key).program; });
	FailEveryDerivation([&](const auto &key) { return PlanArchiveSqlGarbage(state, { { BlobId, 7 }, { QString(64, 'b'), 8 } }, key).program; });
	FailEveryDerivation([&](const auto &key) { return CompleteArchiveSqlGarbage(state, { { BlobId, 7, ArchiveBlobState::GarbagePending } }, key).program; });
	FailEveryDerivation([&](const auto &key) { return ReadAsProgram(ArchiveSqlPurgePage(begin.next, 128, key)); });
	FailEveryDerivation([&](const auto &key) { return ReadAsProgram(ArchiveSqlPurgeHead(begin.next, Key(), key)); });
	FailEveryDerivation([&](const auto &key) { return ReadAsProgram(ArchiveSqlFindOwnedMedia(state.account, u"document:555"_q, u"document"_q, key)); });
	FailEveryDerivation([&](const auto &key) { return ReadAsProgram(ArchiveSqlReadMessage(Key(), key)); });
	FailEveryDerivation([&](const auto &key) { return ReadAsProgram(ArchiveSqlReadReceipt(Key(), u"body"_q, key)); });
	FailEveryDerivation([&](const auto &key) { return ReadAsProgram(ArchiveSqlReadRecord(state.account, ArchiveRecordKind::Blob, BlobId, key)); });
	FailEveryDerivation([&](const auto &key) { return ReadAsProgram(ArchiveSqlReadChatEpoch(state.account, Key().chat, key)); });
	FailEveryDerivation([&](const auto &key) { return ReadAsProgram(ArchiveSqlVersionJobs(Key(), u"body"_q, 128, key)); });
	FailEveryDerivation([&](const auto &key) { return ReadAsProgram(ArchiveSqlReadVersion(Key(), u"body"_q, key)); });
	FailEveryDerivation([&](const auto &key) { return ReadAsProgram(ArchiveSqlReadHead(Key(), key)); });
	FailEveryDerivation([&](const auto &key) { return ReadAsProgram(ArchiveSqlReadVersions(Key(), 128, key)); });
	FailEveryDerivation([&](const auto &key) { return ReadAsProgram(ArchiveSqlDeletedTranscript(state.account, Key().chat, 128, key)); });
}

void FutureSnapshotMetadata() {
	Section = "future snapshot wrapper fields survive seal restore and rewrite";
	const auto plan = PlanArchiveEvent(Empty(), Event(Empty(), u"base"_q, 1));
	auto snapshot = ArchiveVersionSnapshot{ Key(), plan.next.messages[0].versions[0] };
	snapshot.preserved[u"futureMetadata"_q] = u"preserve"_q;
	snapshot.preserved[u"futureNested"_q] = QJsonArray{ QJsonObject{ { u"unknown"_q, true } }, 9007199254740991LL };
	const auto oldBytes = EncodeArchiveVersionSnapshot(snapshot);
	CHECK(oldBytes && DecodeArchiveVersionSnapshot(*oldBytes).snapshot == snapshot);
	const auto bytes = EncodeArchiveRow({ snapshot });
	CHECK(bytes);
	const auto decoded = DecodeArchiveRow(*bytes);
	CHECK(decoded.status == ArchiveCodecStatus::Valid && decoded.row);
	CHECK(decoded.row && EncodeArchiveRow(*decoded.row) == bytes);
	const auto security = Security();
	const auto id = ArchiveSqlChildId(snapshot.key, ArchiveIndexKind::Version, snapshot.version.id, security);
	const auto frame = SealArchiveSqlRow(snapshot.key.account, id, snapshot, security);
	CHECK(frame);
	const auto opened = OpenArchiveEnvelope({ snapshot.key.account, security.storeId, ArchiveRecordKind::Version, id.toUtf8() },
		security.keyId, *frame, security.crypto);
	CHECK(opened && opened.bytes == *bytes);
	auto shell = plan.next.messages[0];
	shell.versions.clear(); shell.receipts.clear();
	auto header = plan.next;
	header.messages.clear();
	const auto restored = RestoreArchiveSqlSlice({ { u"account"_q, 0, { header } },
		{ ArchiveSqlMessageId(shell.key, security), 1, { shell } }, { id, 0, *decoded.row },
		{ ArchiveSqlChildId(shell.key, ArchiveIndexKind::Event, u"base"_q, security), 1,
			{ plan.next.messages[0].receipts[0] } } }, security);
	CHECK(restored);
	CHECK(restored && restored->versionMetadata[0].preserved == snapshot.preserved);
	CHECK(restored && EncodeArchiveRow({ ArchiveSqlVersionSnapshot(snapshot.key, restored->state.messages[0].versions[0], restored->versionMetadata) }) == bytes);
	CHECK(restored && EncodeArchiveVersionSnapshot(ArchiveSqlVersionSnapshot(snapshot.key, restored->state.messages[0].versions[0], restored->versionMetadata)) == oldBytes);
	if (restored) {
		const auto event = Event(restored->state, u"future-edit"_q, 2, u"edited"_q, ArchiveOrigin::AuthoritativeEdit);
		const auto capture = PlanArchiveSqlCapture(*restored, event, security);
		CHECK(capture.transaction && capture.program);
		auto invalid = *restored;
		invalid.versionMetadata.push_back(invalid.versionMetadata[0]);
		const auto refused = PlanArchiveSqlCapture(invalid, event, security);
		CHECK(!refused.program && refused.program.statements.empty()
			&& refused.program.status == ArchiveSqlStatus::RetentionGap);
		RoundTrip(restored->state);
		const auto normalized = ArchiveSqlVersionSnapshot(snapshot.key, restored->state.messages[0].versions[0], restored->versionMetadata);
		CHECK(EncodeArchiveRow({ normalized }) == bytes);
		CHECK(restored->state.messages[0].versions[0].preserved == snapshot.version.preserved);
	}
}


void OldSchemaOneExtensions() {
	Section = "original schema1 object and scalar extension fixtures remain opaque";
	const auto ObjectVersion = QByteArray(R"fixture({"format":"purple-archive-version","key":{"account":"0-123","chat":{"id":"45","kind":2},"collection":0,"collectionId":"0","messageId":"67","representation":"original"},"schema":1,"snapshot":{"completeness":1,"content":{"caption":"","entities":[],"media":[],"opaque":"","opaqueFormat":"","rich":null,"text":"A"},"firstObservedAt":"100","id":"base","media":[],"missing":{"content":[],"media":[],"revision":"base"},"missingPreimage":false,"observation":{"context":{},"observedAt":"100","payload":"","payloadFormat":"","representation":"original","serverTime":"90","topicId":"0","transport":{}},"order":"1","origin":0,"revision":"base","snapshotPreserved":{"future":"opaque"},"source":null}})fixture");
	const auto ObjectRow = QByteArray(R"fixture({"format":"purple-archive-row","kind":3,"schema":1,"value":{"format":"purple-archive-version","key":{"account":"0-123","chat":{"id":"45","kind":2},"collection":0,"collectionId":"0","messageId":"67","representation":"original"},"schema":1,"snapshot":{"completeness":1,"content":{"caption":"","entities":[],"media":[],"opaque":"","opaqueFormat":"","rich":null,"text":"A"},"firstObservedAt":"100","id":"base","media":[],"missing":{"content":[],"media":[],"revision":"base"},"missingPreimage":false,"observation":{"context":{},"observedAt":"100","payload":"","payloadFormat":"","representation":"original","serverTime":"90","topicId":"0","transport":{}},"order":"1","origin":0,"revision":"base","snapshotPreserved":{"future":"opaque"},"source":null}}})fixture");
	const auto objectVersion = DecodeArchiveVersionSnapshot(ObjectVersion);
	CHECK(objectVersion.snapshot);
	CHECK(objectVersion.snapshot && EncodeArchiveVersionSnapshot(*objectVersion.snapshot) == ObjectVersion);
	CHECK(objectVersion.snapshot && objectVersion.snapshot->version.preserved.contains(u"snapshotPreserved"_q));
	const auto objectRow = DecodeArchiveRow(ObjectRow);
	CHECK(objectRow.row && EncodeArchiveRow(*objectRow.row) == ObjectRow);
	const auto ScalarVersion = QByteArray(R"fixture({"format":"purple-archive-version","key":{"account":"0-123","chat":{"id":"45","kind":2},"collection":0,"collectionId":"0","messageId":"67","representation":"original"},"schema":1,"snapshot":{"completeness":1,"content":{"caption":"","entities":[],"media":[],"opaque":"","opaqueFormat":"","rich":null,"text":"A"},"firstObservedAt":"100","id":"base","media":[],"missing":{"content":[],"media":[],"revision":"base"},"missingPreimage":false,"observation":{"context":{},"observedAt":"100","payload":"","payloadFormat":"","representation":"original","serverTime":"90","topicId":"0","transport":{}},"order":"1","origin":0,"revision":"base","snapshotPreserved":"future opaque","source":null}})fixture");
	const auto ScalarRow = QByteArray(R"fixture({"format":"purple-archive-row","kind":3,"schema":1,"value":{"format":"purple-archive-version","key":{"account":"0-123","chat":{"id":"45","kind":2},"collection":0,"collectionId":"0","messageId":"67","representation":"original"},"schema":1,"snapshot":{"completeness":1,"content":{"caption":"","entities":[],"media":[],"opaque":"","opaqueFormat":"","rich":null,"text":"A"},"firstObservedAt":"100","id":"base","media":[],"missing":{"content":[],"media":[],"revision":"base"},"missingPreimage":false,"observation":{"context":{},"observedAt":"100","payload":"","payloadFormat":"","representation":"original","serverTime":"90","topicId":"0","transport":{}},"order":"1","origin":0,"revision":"base","snapshotPreserved":"future opaque","source":null}}})fixture");
	const auto scalarVersion = DecodeArchiveVersionSnapshot(ScalarVersion);
	CHECK(scalarVersion.snapshot);
	CHECK(scalarVersion.snapshot && EncodeArchiveVersionSnapshot(*scalarVersion.snapshot) == ScalarVersion);
	CHECK(scalarVersion.snapshot && scalarVersion.snapshot->version.preserved.contains(u"snapshotPreserved"_q));
	const auto scalarRow = DecodeArchiveRow(ScalarRow);
	CHECK(scalarRow.row && EncodeArchiveRow(*scalarRow.row) == ScalarRow);
}

}

void ChunkedBodies() {
	Section = "storage bodies preserve text, rich structures, sources and unknown fields";
	const auto security = Security();
	auto event = Event(Empty(), u"body"_q, 1, QString(50000, 't'));
	event.content->caption = QString(50000, 'c');
	event.content->rich = QJsonObject{ { u"large"_q, QString(50000, 'r') } };
	event.content->entities = QJsonArray{ QJsonObject{ { u"future"_q, QString(50000, 'e') } } };
	event.content->preserved[u"futureContent"_q] = QString(50000, 'p');
	const auto transaction = PlanArchiveEvent(Empty(), event);
	CHECK(transaction);
	auto snapshot = ArchiveVersionSnapshot{ event.key, Message(transaction.next).versions[0] };
	snapshot.preserved[u"futureWrapper"_q] = QJsonObject{ { u"large"_q, QString(50000, 'w') } };
	snapshot.version.preserved[u"snapshotPreserved"_q] = QString(50000, 'v');
	const auto prepared = PrepareArchiveSqlVersion(snapshot);
	CHECK(prepared && prepared->body && EncodeArchiveRow(*prepared)->size() < kArchiveSqlInlineBodyLimit);
	const auto stored = StoreArchiveSqlVersionBody(*prepared, snapshot, security);
	CHECK(stored);
	auto frames = std::vector<QByteArray>();
	for (const auto &statement : stored.statements) {
		if (statement.sql.startsWith(u"INSERT INTO body_chunks"_q)) { frames.push_back(std::get<QByteArray>(statement.binds[3])); }
	}
	const auto restored = RestoreArchiveSqlVersionBody(*prepared, frames, security);
	CHECK(restored && !restored->body && std::get<ArchiveVersionSnapshot>(restored->value) == snapshot);
	const auto encoded = EncodeArchiveRow(*prepared);
	const auto decoded = DecodeArchiveRow(*encoded);
	CHECK(decoded.row && decoded.row->body == prepared->body);
	auto shell = Message(transaction.next);
	shell.versions.clear();
	shell.receipts.clear();
	auto header = Empty();
	header.revision = transaction.next.revision;
	CHECK(!RestoreArchiveSqlSlice({ { u"account"_q, 0, { header } },
		{ ArchiveSqlMessageId(event.key, security), 0, { shell } },
		{ ArchiveSqlChildId(event.key, ArchiveIndexKind::Version, snapshot.version.id, security), 0, *prepared } }, security));
	CHECK(RestoreArchiveSqlVersionBody(*decoded.row, frames, security)->value == restored->value);
	for (auto fail = 1; fail <= 3; ++fail) {
		auto calls = 0;
		auto failed = security;
		failed.hmacSha256 = [&](const QByteArray &input) -> std::optional<QByteArray> {
			return ++calls == fail ? std::nullopt : security.hmacSha256(input);
		};
		const auto invalid = StoreArchiveSqlVersionBody(*prepared, snapshot, failed);
		CHECK(!invalid && invalid.statements.empty());
	}
	auto oversized = Empty();
	oversized.messages.resize(100000);
	const auto bounded = PlanArchiveSqlCapture(oversized, Event(Empty(), u"bounded"_q, 1), security);
	CHECK(!bounded.program && bounded.program.statements.empty()
		&& bounded.program.retentionGapReason == u"archive-slice-limit"_q);
	const auto initial = PlanArchiveSqlCapture(Empty(), Event(Empty(), u"repeat"_q, 1), security);
	CHECK(initial.program && initial.transaction);
	const auto duplicate = PlanArchiveSqlCapture(initial.transaction.next,
		Event(Empty(), u"repeat"_q, 1), security);
	CHECK(duplicate.transaction.status == ArchivePlanStatus::NoChange && duplicate.program
		&& duplicate.program.statements.empty() && duplicate.program.retentionGapReason.isEmpty());
}

ArchiveMediaLifecycleFact LifecycleFact(const ArchiveState &state,
		const ArchiveStoreEpoch &epoch, const ArchiveMessageKey &key = Key()) {
	const auto &version = Message(state, key).versions.back();
	auto fact = ArchiveMediaLifecycleFact();
	fact.source = { epoch, key, version.id, version.revision, ArchiveContentHash(version.content),
		version.content.media[0].identity, ArchiveGenerationFor(state, key.chat) };
	fact.eventId = u"lifecycle"_q;
	fact.order = 2;
	fact.observedAt = 110;
	fact.destroysContent = true;
	fact.lifetime = ArchiveMediaLifetime::Ttl;
	fact.transition = ArchiveMediaTransition::Expiry;
	return fact;
}

void MediaLifecycleFacts() {
	Section = "bound TTL view-once secret voice and round lifecycle facts";
	for (const auto lifetime : { ArchiveMediaLifetime::Ttl, ArchiveMediaLifetime::ViewOnce, ArchiveMediaLifetime::Secret }) {
		for (const auto &kind : { u"photo"_q, u"video"_q, u"voice"_q, u"round-video"_q }) {
			auto state = Empty();
			auto key = Key();
			if (lifetime == ArchiveMediaLifetime::Secret) { key.chat.kind = ArchiveChatKind::Secret; key.messageId = -123; }
			auto event = Event(state, u"original"_q, 1, u"captured caption"_q, ArchiveOrigin::Baseline, key);
			AddMedia(event);
			event.content->media[0].kind = kind;
			CHECK(Apply(state, PlanArchiveEvent(state, event)));
			const auto epoch = ArchiveStoreEpoch{ state.account, u"media-store"_q, 7 };
			for (const auto transition : { ArchiveMediaTransition::Expiry, ArchiveMediaTransition::Consumption,
				ArchiveMediaTransition::ExpiredReplacement }) {
				auto fact = LifecycleFact(state, epoch, key);
				fact.lifetime = lifetime;
				fact.transition = transition;
				const auto plan = PlanArchiveMediaLifecycle(state, epoch, fact);
				CHECK(plan.transaction && plan.event && plan.captureBeforeDestruction);
				CHECK(plan.retention == ArchiveMediaRetention::Unavailable);
				const auto &deleted = Message(plan.transaction.next, key);
				CHECK(deleted.deleted && deleted.versions == Message(state, key).versions);
				CHECK(ArchiveMessageDeletedStatus(deleted) == (transition == ArchiveMediaTransition::Consumption
					? ArchiveDeletedStatus::Consumed : ArchiveDeletedStatus::Expired));
				CHECK(PlanArchiveMediaLifecycle(plan.transaction.next, epoch, fact).transaction.status == ArchivePlanStatus::NoChange);
				RoundTrip(plan.transaction.next);
			}
			auto fact = LifecycleFact(state, epoch, key);
			fact.lifetime = lifetime;
			fact.transition = ArchiveMediaTransition::CacheEviction;
			for (const auto bytes : { ArchiveAvailableBytes::Unavailable, ArchiveAvailableBytes::Partial, ArchiveAvailableBytes::Complete }) {
				fact.bytes = bytes;
				const auto plan = PlanArchiveMediaLifecycle(state, epoch, fact);
				CHECK(plan.transaction.status == ArchivePlanStatus::NoChange && !plan.event && !plan.captureBeforeDestruction);
				CHECK(plan.retention == (bytes == ArchiveAvailableBytes::Partial ? ArchiveMediaRetention::Partial
					: bytes == ArchiveAvailableBytes::Complete ? ArchiveMediaRetention::NeedsCopy : ArchiveMediaRetention::Unavailable));
			}
			fact.transition = ArchiveMediaTransition::Consumption;
			fact.lifetime = ArchiveMediaLifetime::Ordinary;
			fact.destroysContent = false;
			CHECK(PlanArchiveMediaLifecycle(state, epoch, fact).transaction.status == ArchivePlanStatus::NoChange);
			fact.destroysContent = true;
			CHECK(PlanArchiveMediaLifecycle(state, epoch, fact).transaction.status == ArchivePlanStatus::Invalid);
		}
	}
}

void MediaEpochsAndSharedTrash() {
	Section = "media source epoch revision purge and shared current/trash blob";
	auto state = Empty();
	auto event = Event(state, u"original"_q, 1);
	AddMedia(event);
	CHECK(Apply(state, PlanArchiveEvent(state, event)));
	const auto epoch = ArchiveStoreEpoch{ state.account, u"media-store"_q, 7 };
	const auto source = LifecycleFact(state, epoch);
	for (auto test = 0; test != 7; ++test) {
		auto fact = source;
		auto current = epoch;
		if (test == 0) { fact.source.version = u"other"_q; }
		if (test == 1) { fact.source.revision = u"other"_q; }
		if (test == 2) { fact.source.contentHash = QString(64, 'b'); }
		if (test == 3) { ++current.generation; }
		if (test == 4) { current.storeId = u"new-store"_q; }
		if (test == 5) { current.removalPending = true; }
		if (test == 6) { ++current.account.environment; }
		CHECK(PlanArchiveMediaLifecycle(state, current, fact).transaction.status == ArchivePlanStatus::Stale);
	}
	auto newer = state;
	CHECK(Apply(newer, PlanArchiveEvent(newer, Event(newer, u"edited"_q, 2, u"new body"_q, ArchiveOrigin::AuthoritativeEdit))));
	CHECK(PlanArchiveMediaLifecycle(newer, epoch, source).transaction.status == ArchivePlanStatus::Stale);
	const auto job = Job(state, u"copy"_q, u"original"_q);
	CHECK(Apply(state, PlanArchiveMediaStage(state, job)));
	auto changed = epoch;
	++changed.generation;
	CHECK(PlanArchiveScopedMediaCompletion(state, epoch, changed, job.id, MediaSuccess()).status == ArchivePlanStatus::Stale);
	CHECK(Apply(state, PlanArchiveScopedMediaCompletion(state, epoch, epoch, job.id, MediaSuccess())));
	const auto other = Key(state.account, 45, 68);
	auto currentEvent = Event(state, u"current"_q, 1, u"current"_q, ArchiveOrigin::Baseline, other);
	AddMedia(currentEvent);
	CHECK(Apply(state, PlanArchiveEvent(state, currentEvent)));
	CHECK(CountArchiveBlobReferences(state, BlobId).versions == 2);
	const auto expired = PlanArchiveMediaLifecycle(state, epoch, source);
	CHECK(expired.retention == ArchiveMediaRetention::Owned);
	CHECK(Apply(state, expired.transaction));
	CHECK(Message(state).deleted && !Message(state, other).deleted);
	CHECK(Apply(state, PlanArchivePurge(state, Purge(state, ArchivePurgeKind::Trashbin, Key().chat))));
	CHECK(Message(state).payloadPurged && Message(state, other).versions[0].media[0].state == ArchiveMediaState::Complete);
	CHECK(CountArchiveBlobReferences(state, BlobId).versions == 1);
	CHECK(PlanArchiveScopedMediaCompletion(state, epoch, epoch, job.id, MediaSuccess()).status == ArchivePlanStatus::Stale);
	CHECK(PlanArchiveMediaLifecycle(state, epoch, source).transaction.status == ArchivePlanStatus::Stale);
	CHECK(PlanArchiveGarbageCollection(state).status == ArchivePlanStatus::NoChange);
}

void ViewScopesAndConfirmation() {
	Section = "explicit account/chat/alias confirmation scope and guarded query facts";
	const auto state = Empty();
	const auto epoch = ArchiveStoreEpoch{ state.account, u"view-store"_q, 9 };
	auto scope = ArchivePurgeScope{ state.account, Key().chat, { { ArchiveChatKind::Group, 44 }, { ArchiveChatKind::Group, 43 } } };
	const auto confirmation = MakeArchivePurgeConfirmation(state, epoch, ArchivePurgeKind::Trashbin, scope);
	CHECK(confirmation);
	if (!confirmation) { return; }
	CHECK(ConfirmArchivePurge(state, epoch, ArchivePurgeKind::Trashbin, scope, *confirmation));
	std::reverse(scope.migrationAliases.begin(), scope.migrationAliases.end());
	CHECK(MakeArchivePurgeConfirmation(state, epoch, ArchivePurgeKind::Trashbin, scope)->token == confirmation->token);
	for (auto test = 0; test != 11; ++test) {
		auto header = state;
		auto current = epoch;
		auto selected = scope;
		auto displayed = *confirmation;
		auto kind = ArchivePurgeKind::Trashbin;
		if (test == 0) { ++header.revision; }
		if (test == 1) { ++current.generation; }
		if (test == 2) { current.storeId = u"other"_q; }
		if (test == 3) { selected.chat.reset(); selected.migrationAliases.clear(); }
		if (test == 4) { selected.migrationAliases.pop_back(); }
		if (test == 5) { ++selected.account.environment; }
		if (test == 6) { kind = ArchivePurgeKind::EditHistory; }
		if (test == 7) { ++displayed.request.expectedRevision; }
		if (test == 8) { current.removalPending = true; }
		if (test == 9) { ++header.accountGeneration; }
		if (test == 10) { displayed.request.kind = ArchivePurgeKind::EditHistory; }
		CHECK(!ConfirmArchivePurge(header, current, kind, selected, displayed));
	}
	auto view = ArchiveViewScope{ scope, ArchiveTopicScope{ Key().chat, std::numeric_limits<int64>::max() },
		ArchiveCollection::Received, u"original"_q };
	const auto security = Security();
	CHECK(!ArchiveSqlDeletedView(view, 128, security).sql.isEmpty());
	CHECK(ArchiveSqlDeletedView(view, 0, security).sql.isEmpty());
	CHECK(ArchiveSqlScopeBlobs(view, 129, security).sql.isEmpty());
	view.topic->chat.id++;
	CHECK(ArchiveSqlScopeCounts(view, security).sql.isEmpty());
	view.topic->chat = Key().chat;
	FailEveryDerivation([&](const auto &key) { return ReadAsProgram(ArchiveSqlDeletedView(view, 128, key)); });
	FailEveryDerivation([&](const auto &key) { return ReadAsProgram(ArchiveSqlScopeCounts(view, key)); });
	FailEveryDerivation([&](const auto &key) { return ReadAsProgram(ArchiveSqlScopeBlobs(view, 128, key)); });
}

void ReviewLifecycleFences() {
	Section = "review lifecycle handoff and historical completion";
	auto state = Empty();
	auto first = Event(state, u"v1"_q, 1, u"TTL v1"_q);
	AddMedia(first);
	CHECK(Apply(state, PlanArchiveEvent(state, first)));
	const auto epoch = ArchiveStoreEpoch{ state.account, Security().storeId, 1 };
	auto fact = LifecycleFact(state, epoch, Key());
	fact.eventId = u"expiry"_q;
	fact.order = 3;
	const auto prepared = PlanArchiveMediaLifecycle(state, epoch, fact);
	CHECK(prepared.event);
	if (!prepared.event) { return; }
	CHECK(prepared.event->lifecycleEpoch == epoch);
	auto zeroEpoch = epoch;
	zeroEpoch.generation = 0;
	auto zeroFact = fact;
	zeroFact.source.epoch = zeroEpoch;
	CHECK(PlanArchiveMediaLifecycle(state, zeroEpoch, zeroFact).transaction);
	auto expiredState = prepared.transaction.next;
	CHECK(PlanArchiveEvent(expiredState, *prepared.event).status == ArchivePlanStatus::NoChange);

	CHECK(PlanArchiveSqlCapture(state, *prepared.event, Security(), &epoch).transaction);
	CHECK(PlanArchiveSqlCapture(state, *prepared.event, Security()).transaction.status == ArchivePlanStatus::Stale);
	auto otherEpoch = epoch;
	otherEpoch.generation++;
	CHECK(PlanArchiveSqlCapture(state, *prepared.event, Security(), &otherEpoch).transaction.status == ArchivePlanStatus::Stale);
	otherEpoch = epoch;
	otherEpoch.account.userId++;
	CHECK(PlanArchiveSqlCapture(state, *prepared.event, Security(), &otherEpoch).transaction.status == ArchivePlanStatus::Stale);
	otherEpoch = epoch;
	otherEpoch.storeId += u"-other"_q;
	CHECK(PlanArchiveSqlCapture(state, *prepared.event, Security(), &otherEpoch).transaction.status == ArchivePlanStatus::Stale);
	CHECK(Apply(state, PlanArchiveMediaStage(state, Job(state, u"copy-v1"_q, u"v1"_q))));
	const auto staged = state;
	CHECK(state.jobs[0].revision == u"v1"_q);
	CHECK(state.jobs[0].contentHash == ArchiveContentHash(*first.content));
	auto second = Event(state, u"v2"_q, 2, u"non-expiring v2"_q, ArchiveOrigin::AuthoritativeEdit);
	AddMedia(second);
	second.content->media[0].identity = u"document:other"_q;
	CHECK(Apply(state, PlanArchiveEvent(state, second)));
	CHECK(PlanArchiveEvent(state, *prepared.event).status == ArchivePlanStatus::Stale);
	auto changedPurge = staged;
	changedPurge.accountGeneration++;
	changedPurge.jobs[0].state = ArchiveJobState::Cancelled;
	changedPurge.messages[0].versions[0].media[0].state = ArchiveMediaState::Missing;
	CHECK(IsArchiveState(changedPurge));
	CHECK(PlanArchiveSqlCapture(changedPurge, *prepared.event, Security(), &epoch).transaction.status == ArchivePlanStatus::Stale);
	const auto refused = PlanArchiveSqlCapture(state, *prepared.event, Security(), &epoch);
	CHECK(refused.transaction.status == ArchivePlanStatus::Stale);
	CHECK(refused.program.statements.empty());
	const auto current = Message(state).versions.back();
	CHECK(Apply(state, PlanArchiveScopedMediaCompletion(state, epoch, epoch, u"copy-v1"_q, MediaSuccess())));
	CHECK(Message(state).versions.front().media[0].state == ArchiveMediaState::Complete);
	CHECK(Message(state).versions.back() == current);
	auto changed = staged;
	auto revision = Event(changed, u"revision-v1"_q, 2, u"TTL v1"_q, ArchiveOrigin::AuthoritativeEdit);
	AddMedia(revision);
	CHECK(Apply(changed, PlanArchiveEvent(changed, revision)));
	CHECK(PlanArchiveMediaCompletion(changed, u"copy-v1"_q, MediaSuccess()).mediaRefusal == ArchiveMediaRefusal::RevisionMismatch);
	CHECK(changed.jobs[0].state == ArchiveJobState::Cancelled);
	CHECK(Message(changed).versions[0].media[0].state == ArchiveMediaState::Missing);
	CHECK(Apply(changed, PlanArchiveMediaStage(changed, Job(changed, u"copy-new-revision"_q, u"v1"_q))));
	CHECK(Apply(changed, PlanArchiveMediaCompletion(changed, u"copy-new-revision"_q, MediaSuccess())));
	changed = staged;
	changed.jobs[0].revision = u"different-revision"_q;
	CHECK(IsArchiveState(changed));
	CHECK(PlanArchiveMediaCompletion(changed, u"copy-v1"_q, MediaSuccess()).mediaRefusal == ArchiveMediaRefusal::RevisionMismatch);
	changed = staged;
	changed.jobs[0].contentHash = QString(64, 'b');
	CHECK(IsArchiveState(changed));
	CHECK(PlanArchiveMediaCompletion(changed, u"copy-v1"_q, MediaSuccess()).mediaRefusal == ArchiveMediaRefusal::ContentHashMismatch);
	changed = staged;
	changed.messages[0].versions[0].content.text = u"changed"_q;
	CHECK(IsArchiveState(changed));
	CHECK(PlanArchiveMediaCompletion(changed, u"copy-v1"_q, MediaSuccess()).mediaRefusal == ArchiveMediaRefusal::ContentHashMismatch);
	changed = staged;
	CHECK(Apply(changed, PlanArchiveEvent(changed, second)));
	CHECK(Apply(changed, PlanArchivePurge(changed, Purge(changed, ArchivePurgeKind::EditHistory))));
	CHECK(PlanArchiveMediaCompletion(changed, u"copy-v1"_q, MediaSuccess()).mediaRefusal == ArchiveMediaRefusal::PurgeEpochMismatch);
	changed = staged;
	changed.messages[0].versions.clear();
	changed.jobs[0].state = ArchiveJobState::Failed;
	changed.jobs[0].failure = u"lost-version"_q;
	CHECK(IsArchiveState(changed));
	CHECK(PlanArchiveMediaCompletion(changed, u"copy-v1"_q, MediaSuccess()).mediaRefusal == ArchiveMediaRefusal::VersionMissing);
	const auto fingerprint = ArchiveSqlExpectedSchemaFingerprint();
	const auto schema = ArchiveSqlSchema();
	CHECK(CheckArchiveSqlSchema(2, fingerprint, schema) == ArchiveSqlStatus::Ready);
	CHECK(CheckArchiveSqlSchema(1, fingerprint, schema) == ArchiveSqlStatus::SchemaMismatch);
	CHECK(CheckArchiveSqlSchema(2, u"wrong"_q, schema) == ArchiveSqlStatus::SchemaMismatch);
	auto drift = schema;
	drift[0] += u" "_q;
	CHECK(CheckArchiveSqlSchema(2, fingerprint, drift) == ArchiveSqlStatus::Ready);
	drift[0].replace(u"schema_fingerprint TEXT"_q, u"draft_drift TEXT"_q);
	CHECK(CheckArchiveSqlSchema(2, fingerprint, drift) == ArchiveSqlStatus::SchemaMismatch);
}

int main() {
	ReviewLifecycleFences();
	MediaLifecycleFacts();
	MediaEpochsAndSharedTrash();
	ViewScopesAndConfirmation();
	ChunkedBodies();
	Envelopes();
	Keys();
	Removal();
	RowsAndPlans();
	SelectiveDerivationFailures();
	FutureSnapshotMetadata();
	OldSchemaOneExtensions();
	return Finish();
}
