/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "tests/archive_storage_test_support.h"

#include <QtCore/QTemporaryDir>
#include "purple/purple_archive_source.h"

#include <sqlite3.h>

#include <algorithm>
#include <set>

using namespace ArchiveTest;
using namespace ArchiveStorageTest;

namespace {

using Row = std::vector<ArchiveSqlValue>;

class Database {
public:
	explicit Database(QString path) : _path(std::move(path)) {
		CHECK(sqlite3_open(_path.toUtf8().constData(), &_db) == SQLITE_OK);
		for (const auto &sql : ArchiveSqlConnectionSetup()) {
			CHECK(sqlite3_exec(_db, sql.toUtf8().constData(), nullptr, nullptr, nullptr) == SQLITE_OK);
		}
		const auto fk = query({ u"PRAGMA foreign_keys"_q, {}, ArchiveSqlExpectation::OneRow });
		const auto wal = query({ u"PRAGMA journal_mode"_q, {}, ArchiveSqlExpectation::OneRow });
		const auto sync = query({ u"PRAGMA synchronous"_q, {}, ArchiveSqlExpectation::OneRow });
		CHECK(fk && std::get<int64>((*fk)[0][0]) == 1);
		CHECK(wal && std::get<QString>((*wal)[0][0]) == u"wal"_q);
		CHECK(sync && std::get<int64>((*sync)[0][0]) == 2);
	}

	~Database() { CHECK(sqlite3_close(_db) == SQLITE_OK); }

	std::optional<std::vector<Row>> query(const ArchiveSqlStatement &statement) {
		if (statement.sql.isEmpty()) { return {}; }
		auto prepared = static_cast<sqlite3_stmt *>(nullptr);
		const auto sql = statement.sql.toUtf8();
		const auto beforeChanges = sqlite3_total_changes64(_db);
		if (sqlite3_prepare_v2(_db, sql.constData(), int(sql.size()), &prepared, nullptr) != SQLITE_OK) {
			std::printf("SQL prepare: %s (%s)\n", sqlite3_errmsg(_db), sql.constData());
			return {};
		}
		auto finalize = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(prepared, sqlite3_finalize);
		if (sqlite3_bind_parameter_count(prepared) != int(statement.binds.size())) { return {}; }
		for (auto i = size_t(0); i != statement.binds.size(); ++i) {
			const auto status = std::visit([&](const auto &value) {
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, std::nullptr_t>) {
					return sqlite3_bind_null(prepared, int(i + 1));
				} else if constexpr (std::is_same_v<T, int64>) {
					return sqlite3_bind_int64(prepared, int(i + 1), value);
				} else if constexpr (std::is_same_v<T, QString>) {
					const auto text = value.toUtf8();
					return sqlite3_bind_text(prepared, int(i + 1), text.constData(), int(text.size()), SQLITE_TRANSIENT);
				} else {
					return sqlite3_bind_blob(prepared, int(i + 1), value.constData(), int(value.size()), SQLITE_TRANSIENT);
				}
			}, statement.binds[i]);
			if (status != SQLITE_OK) { return {}; }
		}
		auto rows = std::vector<Row>();
		auto status = sqlite3_step(prepared);
		while (status == SQLITE_ROW) {
			auto row = Row();
			for (auto i = 0; i != sqlite3_column_count(prepared); ++i) {
				switch (sqlite3_column_type(prepared, i)) {
				case SQLITE_INTEGER: row.push_back(int64(sqlite3_column_int64(prepared, i))); break;
				case SQLITE_TEXT: row.push_back(QString::fromUtf8(
					static_cast<const char *>(sqlite3_column_blob(prepared, i)), sqlite3_column_bytes(prepared, i))); break;
				case SQLITE_BLOB: row.push_back(QByteArray(
					static_cast<const char *>(sqlite3_column_blob(prepared, i)), sqlite3_column_bytes(prepared, i))); break;
				default: row.push_back(nullptr); break;
				}
			}
			rows.push_back(std::move(row));
			if (statement.rows.maximum >= 0 && int64(rows.size()) > statement.rows.maximum) { return {}; }
			status = sqlite3_step(prepared);
		}
		if (status != SQLITE_DONE) {
			std::printf("SQL step: %s\n", sqlite3_errmsg(_db));
			return {};
		}
		const auto directChanges = sqlite3_changes64(_db);
		const auto totalChanges = sqlite3_total_changes64(_db) - beforeChanges;
		if (sqlite3_finalize(finalize.release()) != SQLITE_OK) { return {}; }
		if (!CheckArchiveSqlResult(statement, true, int64(rows.size()), directChanges, totalChanges)) { return {}; }
		return rows;
	}

	bool run(const ArchiveSqlProgram &program, size_t stopBefore = std::numeric_limits<size_t>::max()) {
		if (!program) { return false; }
		for (auto i = size_t(0); i != program.statements.size(); ++i) {
			if (i == stopBefore || !query(program.statements[i])) {
				sqlite3_exec(_db, "ROLLBACK", nullptr, nullptr, nullptr);
				return false;
			}
		}
		return true;
	}

	std::optional<ArchiveSqlSlice> loadSlice(const AccountKey &account, const ArchiveSqlSecurity &security) {
		const auto metadata = query({ u"SELECT schema_version,account_token,store_id FROM archive_meta WHERE singleton=1"_q,
			{}, ArchiveSqlExpectation::OneRow });
		if (!metadata || std::get<int64>((*metadata)[0][0]) != kArchiveSqlSchemaVersion
			|| std::get<QString>((*metadata)[0][1]) != ArchiveSqlToken(account, ArchiveIndexKind::Account, "account", security)
			|| std::get<QString>((*metadata)[0][2]) != security.storeId) { return {}; }
		const auto kinds = std::vector<ArchiveRecordKind>{ ArchiveRecordKind::Account, ArchiveRecordKind::Message,
			ArchiveRecordKind::Version, ArchiveRecordKind::Event, ArchiveRecordKind::Blob,
			ArchiveRecordKind::Job, ArchiveRecordKind::Lease, ArchiveRecordKind::Generation };
		auto records = std::vector<ArchiveStoredRow>();
		for (const auto kind : kinds) {
			auto cursor = QString();
			for (;;) {
				const auto rows = query(ArchiveSqlReadPage(kind, kArchiveSqlPageLimit, cursor));
				if (!rows) { return {}; }
				for (const auto &row : *rows) {
					const auto &id = std::get<QString>(row[0]);
					const auto decoded = OpenArchiveEnvelope({ account, security.storeId, kind, id.toUtf8() },
						security.keyId, std::get<QByteArray>(row[2]), security.crypto);
					if (!decoded) { return {}; }
					const auto payload = DecodeArchiveRow(decoded.bytes);
					if (!payload.row || ArchiveRowKind(*payload.row) != kind) { return {}; }
					auto restored = payload.row;
					if (restored->body) {
						const auto chunks = query(ArchiveSqlReadBodyChunks(*restored, security));
						if (!chunks) { return {}; }
						auto frames = std::vector<QByteArray>();
						for (const auto &chunk : *chunks) { frames.push_back(std::get<QByteArray>(chunk[0])); }
						restored = RestoreArchiveSqlVersionBody(*restored, frames, security);
						if (!restored) { return {}; }
					}
					records.push_back({ id, std::get<int64>(row[1]), *restored });
					if (records.size() > kArchiveSqlSliceRowLimit) { return {}; }
					cursor = id;
				}
				if (rows->size() < kArchiveSqlPageLimit || kind == ArchiveRecordKind::Account) { break; }
			}
		}
		return RestoreArchiveSqlSlice(std::move(records), security);
	}

	std::optional<ArchiveState> load(const AccountKey &account, const ArchiveSqlSecurity &security) {
		const auto slice = loadSlice(account, security);
		return slice ? std::optional(slice->state) : std::nullopt;
	}

	sqlite3 *raw() { return _db; }

private:
	QString _path;
	sqlite3 *_db = nullptr;
};

ArchiveState HeaderOnly(ArchiveState state) {
	state.messages.clear(); state.blobs.clear(); state.jobs.clear(); state.leases.clear(); state.chatGenerations.clear();
	return state;
}

void ApplySql(Database &database, ArchiveState &state, const ArchiveTransaction &plan,
		const ArchiveSqlSecurity &security, bool crashMatrix = false) {
	CHECK(plan);
	if (!plan) { return; }
	const auto program = TranslateArchiveSql(plan, security);
	CHECK(program);
	if (crashMatrix) {
		for (auto cut = size_t(1); cut < program.statements.size(); ++cut) {
			CHECK(!database.run(program, cut));
			CHECK(database.load(state.account, security) == state);
		}
	}
	CHECK(database.run(program));
	CHECK(database.load(state.account, security) == plan.next);
	state = plan.next;
	CHECK(!database.run(program));
	CHECK(database.load(state.account, security) == state);
}

std::optional<ArchiveRow> OpenRow(const AccountKey &account, const QString &id, ArchiveRecordKind kind,
		const QByteArray &frame, const ArchiveSqlSecurity &security) {
	const auto opened = OpenArchiveEnvelope({ account, security.storeId, kind, id.toUtf8() }, security.keyId, frame, security.crypto);
	if (!opened) { return {}; }
	const auto decoded = DecodeArchiveRow(opened.bytes);
	return decoded.row && ArchiveRowKind(*decoded.row) == kind ? decoded.row : std::nullopt;
}

std::vector<ArchiveSqlPurgeRow> PurgeRows(Database &database, const ArchiveSqlPurge &purge,
		const std::vector<Row> &rows, const ArchiveSqlSecurity &security) {
	auto result = std::vector<ArchiveSqlPurgeRow>();
	const auto kind = purge.phase == ArchivePurgePhase::Messages ? ArchiveRecordKind::Message
		: purge.phase == ArchivePurgePhase::Versions ? ArchiveRecordKind::Version
		: purge.phase == ArchivePurgePhase::Jobs ? ArchiveRecordKind::Job : ArchiveRecordKind::Lease;
	for (const auto &row : rows) {
		const auto &id = std::get<QString>(row[0]);
		const auto decoded = OpenRow(purge.account, id, kind, std::get<QByteArray>(row[1]), security);
		CHECK(decoded);
		if (!decoded) { return {}; }
		auto input = ArchiveSqlPurgeRow{ { id, 0, *decoded }, std::get<QByteArray>(row[1]) };
		if (purge.phase == ArchivePurgePhase::Messages && purge.kind == ArchivePurgeKind::EditHistory) {
			const auto &message = std::get<ArchiveMessage>(decoded->value);
			const auto head = database.query({ u"SELECT head_version FROM messages WHERE message_key=?"_q, { id }, ArchiveSqlExpectation::OneRow });
			const auto bytes = database.query(ArchiveSqlPurgeHead(purge, message.key, security));
			CHECK(head && bytes && bytes->size() == 1);
			const auto headRow = OpenRow(purge.account, id + '/' + std::get<QString>((*head)[0][0]), ArchiveRecordKind::Version,
				std::get<QByteArray>((*bytes)[0][0]), security);
			CHECK(headRow);
			input.head = std::get<ArchiveVersionSnapshot>(headRow->value);
			input.headFrame = std::get<QByteArray>((*bytes)[0][0]);
		}
		const auto snapshot = input.head ? &*input.head
			: kind == ArchiveRecordKind::Version ? &std::get<ArchiveVersionSnapshot>(input.stored.row.value) : nullptr;
		if (snapshot) {
			for (const auto &media : snapshot->version.content.media) {
				const auto owned = database.query(ArchiveSqlFindOwnedMedia(purge.account, media.identity, media.kind, security));
				CHECK(owned);
				if (!owned->empty()) {
					const auto blobRow = OpenRow(purge.account, std::get<QString>((*owned)[0][0]), ArchiveRecordKind::Blob,
						std::get<QByteArray>((*owned)[0][1]), security);
					CHECK(blobRow);
					input.copies.push_back({ media.identity, media.kind, std::get<ArchiveBlob>(blobRow->value).id,
						std::get<QString>((*owned)[0][2]), std::get<QByteArray>((*owned)[0][3]),
						std::get<QByteArray>((*owned)[0][1]), std::get<int64>((*owned)[0][4]) });
				}
			}
		}
		result.push_back(std::move(input));
	}
	return result;
}

ArchiveSqlPurge ReloadPurge(Database &database, const ArchiveSqlPurge &purge, const ArchiveSqlSecurity &security) {
	const auto row = database.query({ u"SELECT operation_id,envelope,phase,cursor FROM maintenance WHERE singleton=1"_q, {}, ArchiveSqlExpectation::OneRow });
	CHECK(row);
	const auto opened = OpenArchiveEnvelope({ purge.account, security.storeId, ArchiveRecordKind::Maintenance,
		std::get<QString>((*row)[0][0]).toUtf8() }, security.keyId, std::get<QByteArray>((*row)[0][1]), security.crypto);
	CHECK(opened);
	const auto decoded = DecodeArchiveSqlPurge(opened.bytes);
	CHECK(decoded);
	CHECK(decoded && int64(decoded->phase) == std::get<int64>((*row)[0][2]));
	CHECK(decoded && decoded->cursor == std::get<QString>((*row)[0][3]));
	return *decoded;
}

void ContinuePurge(Database &database, ArchiveSqlPurge purge, const ArchiveSqlSecurity &security,
		bool crashMatrix = false, uint64_t *processed = nullptr) {
	for (auto iteration = 0; iteration != 10000; ++iteration) {
		auto rows = std::vector<ArchiveSqlPurgeRow>();
		if (purge.phase != ArchivePurgePhase::Finish) {
			const auto page = database.query(ArchiveSqlPurgePage(purge, kArchiveSqlPageLimit, security));
			CHECK(page && page->size() <= kArchiveSqlPageLimit);
			rows = PurgeRows(database, purge, *page, security);
			if (processed && purge.phase == ArchivePurgePhase::Messages) { *processed += rows.size(); }
		}
		if (crashMatrix && !rows.empty()) {
			auto changed = purge;
			changed.kind = purge.kind == ArchivePurgeKind::Trashbin ? ArchivePurgeKind::EditHistory : ArchivePurgeKind::Trashbin;
			const auto wrongIntent = ContinueArchiveSqlPurge(changed, rows, security);
			if (wrongIntent.program) { CHECK(!database.run(wrongIntent.program)); }
			auto wrongFrame = rows;
			wrongFrame[0].frame[0] ^= 1;
			CHECK(!ContinueArchiveSqlPurge(purge, wrongFrame, security).program);
			if (rows.size() > 1) {
				auto skipped = rows;
				skipped.erase(skipped.begin());
				const auto skip = ContinueArchiveSqlPurge(purge, skipped, security);
				CHECK(skip.program && !database.run(skip.program));
			}
			if (purge.phase == ArchivePurgePhase::Versions && !rows[0].copies.empty()) {
				auto missingCopy = rows;
				missingCopy[0].copies.clear();
				const auto missing = ContinueArchiveSqlPurge(purge, missingCopy, security);
				CHECK(missing.program && !database.run(missing.program));
				auto wrongCopy = rows;
				wrongCopy[0].copies[0].blobId = QString(64, 'b');
				const auto wrong = ContinueArchiveSqlPurge(purge, wrongCopy, security);
				CHECK(!wrong.program && wrong.program.statements.empty());
				for (auto mutation = 0; mutation != 5; ++mutation) {
					auto changed = rows;
					auto &proof = changed[0].copies[0];
					if (mutation == 0) { proof.ownerFrame.clear(); }
					if (mutation == 1) { proof.blobFrame.clear(); }
					if (mutation == 2) { ++proof.position; }
					if (mutation == 3) { proof.kind = u"photo"_q; }
					if (mutation == 4) { proof.ownerVersionId[0] = proof.ownerVersionId[0] == 'a' ? 'b' : 'a'; }
					const auto invalid = ContinueArchiveSqlPurge(purge, changed, security);
					CHECK(!invalid.program || !database.run(invalid.program));
				}
				auto total = 0;
				auto counted = security;
				counted.hmacSha256 = [&](const QByteArray &bytes) { ++total; return security.hmacSha256(bytes); };
				CHECK(ContinueArchiveSqlPurge(purge, rows, counted).program);
				for (auto failure = 1; failure <= total; ++failure) {
					auto call = 0;
					counted.hmacSha256 = [&](const QByteArray &bytes) -> std::optional<QByteArray> {
						return ++call == failure ? std::nullopt : security.hmacSha256(bytes);
					};
					const auto invalid = ContinueArchiveSqlPurge(purge, rows, counted);
					CHECK(!invalid.program && invalid.program.statements.empty()
						&& invalid.releaseStagedJobs.empty() && invalid.revokeReaders.empty());
				}
			}
		}
		const auto plan = ContinueArchiveSqlPurge(purge, rows, security);
		CHECK(plan.program && IsBoundedArchiveSqlProgram(plan.program));
		CHECK(plan.program.statements.size() <= kArchiveSqlStatementLimit);
		if (crashMatrix) {
			for (auto cut = size_t(1); cut < plan.program.statements.size(); ++cut) {
				CHECK(!database.run(plan.program, cut));
				const auto reload = ReloadPurge(database, purge, security);
				CHECK(reload.phase == purge.phase && reload.cursor == purge.cursor);
			}
		}
		CHECK(database.run(plan.program));
		if (crashMatrix) { CHECK(!database.run(plan.program)); }
		if (plan.complete) { return; }
		purge = ReloadPurge(database, plan.next, security);
	}
	CHECK(false);
}

void ApplyPurge(Database &database, ArchiveState &state, const ArchivePurgeRequest &request,
		const ArchiveSqlSecurity &security) {
	const auto expected = PlanArchivePurge(state, request);
	CHECK(expected);
	auto epochs = std::vector<ArchiveChatGeneration>();
	if (request.scope.chat) {
		auto chats = request.scope.migrationAliases;
		chats.push_back(*request.scope.chat);
		for (const auto &chat : chats) {
			const auto existing = std::find_if(state.chatGenerations.begin(), state.chatGenerations.end(), [&](const auto &entry) { return SameArchiveChat(entry.chat, chat); });
			epochs.push_back(existing == state.chatGenerations.end() ? ArchiveChatGeneration{ chat, 0 } : *existing);
		}
	}
	const auto plan = BeginArchiveSqlPurge(HeaderOnly(state), request, epochs, u"purge-op"_q, security);
	CHECK(plan.program);
	for (auto cut = size_t(1); cut < plan.program.statements.size(); ++cut) {
		CHECK(!database.run(plan.program, cut));
		CHECK(database.load(state.account, security) == state);
	}
	CHECK(database.run(plan.program));
	CHECK(!database.run(TranslateArchiveSql(PlanArchiveEvent(state, Event(state, u"stale"_q, 10)), security)));
	ContinuePurge(database, plan.next, security, true);
	CHECK(database.load(state.account, security) == expected.next);
	state = expected.next;
}

void PlansAndMedia(const QString &path) {
	Section = "SQLite bounded plans, purge, readers and GC";
	auto database = Database(path);
	auto state = Empty();
	state.preserved[u"futureMetadata"_q] = u"preserve"_q;
	const auto security = Security();
	CHECK(database.run(InitializeArchiveSql(state, security)));
	auto event = Event(state, u"first"_q, 1, u"private original"_q);
	AddMedia(event);
	event.content->preserved[u"futureSemantic"_q] = u"keep"_q;
	event.source = ArchiveSource{ ArchiveSourceKind::ComposeMarkdown, ArchiveContentHash(*event.content),
		event.key.representation, "**source**\r\n", event.id };
	event.source->binding = ArchiveSourceBinding{ event.key, event.id, -1, std::numeric_limits<int64>::max() };
	ApplySql(database, state, PlanArchiveEvent(state, event), security, true);
	CHECK(ArchiveSourceMatches(Message(state).versions[0]));
	const auto stage = Job(state, u"download-1"_q, u"first"_q);
	ApplySql(database, state, PlanArchiveMediaStage(state, stage), security, true);
	ApplySql(database, state, PlanArchiveMediaCompletion(state, stage.id, MediaSuccess()), security, true);
	auto second = Event(state, u"second"_q, 2, u"edited"_q, ArchiveOrigin::AuthoritativeEdit);
	ApplySql(database, state, PlanArchiveEvent(state, second), security, true);
	CHECK(database.query(ArchiveSqlReadVersions(Key(), 10, security))->size() == 2);
	auto slice = HeaderOnly(state);
	slice.messages.push_back(state.messages[0]);
	slice.messages[0].versions = { state.messages[0].versions.back() };
	slice.messages[0].receipts = { state.messages[0].receipts.back() };
	auto third = Event(slice, u"third"_q, 3, u"back to attachment"_q, ArchiveOrigin::AuthoritativeEdit);
	AddMedia(third);
	const auto thirdPlan = PlanArchiveEvent(slice, third);
	CHECK(thirdPlan && database.run(TranslateArchiveSql(thirdPlan, security)));
	state.revision = thirdPlan.next.revision;
	state.messages[0].order = thirdPlan.next.messages[0].order;
	state.messages[0].versions.push_back(thirdPlan.next.messages[0].versions.back());
	state.messages[0].receipts.push_back(thirdPlan.next.messages[0].receipts.back());
	CHECK(state.messages[0].versions.back().media[0].state == ArchiveMediaState::Missing);
	CHECK(database.load(state.account, security) == state);
	const auto lease = ArchiveReaderLease{ u"reader-1"_q, Key(), u"first"_q, BlobId,
		ArchiveGenerationFor(state, Key().chat) };
	ApplySql(database, state, PlanArchiveReaderAcquire(state, lease), security, true);
	ApplyPurge(database, state, Purge(state, ArchivePurgeKind::EditHistory, Key().chat), security);
	CHECK(Message(state).versions.size() == 1 && state.leases[0].revoked);
	CHECK(CountArchiveBlobReferences(state, BlobId).readers == 1);
	CHECK(database.query(ArchiveSqlGarbagePage(10))->empty());
	ApplySql(database, state, PlanArchiveEvent(state,
		Event(state, u"delete"_q, 4, {}, ArchiveOrigin::AuthoritativeDelete)), security, true);
	CHECK(database.query(ArchiveSqlDeletedTranscript(state.account, Key().chat, 10, security))->size() == 1);
	ApplyPurge(database, state, Purge(state, ArchivePurgeKind::Trashbin), security);
	CHECK(Message(state).versions.empty());
	CHECK(database.query(ArchiveSqlGarbagePage(10))->empty());
	ApplySql(database, state, PlanArchiveReaderRelease(state, lease.id), security, true);
	const auto reference = PlanArchiveGarbageCollection(state);
	const auto gc = PlanArchiveSqlGarbage(HeaderOnly(state), state.blobs, security);
	CHECK(gc.program && gc.deleteOwnedBlobs == std::vector<QString>{ BlobId });
	CHECK(database.run(gc.program));
	CHECK(database.load(state.account, security) == reference.next);
	state = reference.next;
	const auto completed = CompleteArchiveSqlGarbage(HeaderOnly(state), state.blobs, security);
	CHECK(completed.program && database.run(completed.program));
	state = CompleteArchiveGarbageCollection(state, { BlobId }).next;
	CHECK(database.load(state.account, security) == state);
	const auto gap = ArchiveCaptureGap{ state.account, security.storeId, u"gap-1"_q, 123, u"source-unavailable"_q };
	CHECK(database.run(PlanArchiveCaptureGap(gap, security)));
	CHECK(database.run(PlanArchiveCaptureGap(gap, security)));
	auto conflict = gap;
	conflict.reason = u"disk-full"_q;
	CHECK(!database.run(PlanArchiveCaptureGap(conflict, security)));
	CHECK(database.query(ArchiveSqlReadCaptureGaps(10))->size() == 1);
}

void PagesAndSlices(const QString &path) {
	Section = "SQLite bounded message slices preserve older history";
	auto database = Database(path);
	auto all = Empty({ 0, std::numeric_limits<int64>::max() });
	const auto security = Security();
	CHECK(database.run(InitializeArchiveSql(all, security)));
	auto ids = std::set<QString>();
	for (auto i = 0; i != 9; ++i) {
		auto state = HeaderOnly(all);
		auto key = Key(all.account, std::numeric_limits<int64>::max(), 100 + i / 2);
		key.representation = i % 2 ? u"translation:fa"_q : u"original"_q;
		if (i == 8) { key.collection = ArchiveCollection::QuickReply; key.collectionId = std::numeric_limits<int64>::max(); }
		const auto base = PlanArchiveEvent(state, Event(state, u"base"_q + QString::number(i), 1, u"body"_q, ArchiveOrigin::Baseline, key));
		CHECK(database.run(TranslateArchiveSql(base, security)));
		state = base.next;
		const auto gone = PlanArchiveEvent(state, Event(state, u"gone"_q + QString::number(i), 2, {}, ArchiveOrigin::MediaExpiry, key));
		CHECK(database.run(TranslateArchiveSql(gone, security)));
		all.revision = gone.next.revision;
		all.messages.push_back(gone.next.messages[0]);
		ids.insert(ArchiveSqlMessageId(key, security));
	}
	CHECK(database.load(all.account, security) == all);
	auto cursor = std::optional<ArchiveTranscriptCursor>();
	auto seen = std::set<QString>();
	for (;;) {
		const auto rows = database.query(ArchiveSqlDeletedTranscript(all.account, all.messages[0].key.chat, 2, security, cursor));
		CHECK(rows);
		if (rows->empty()) { break; }
		for (const auto &row : *rows) {
			const auto &id = std::get<QString>(row[0]);
			CHECK(seen.insert(id).second);
			cursor = ArchiveTranscriptCursor{ std::get<int64>(row[1]), std::get<int64>(row[2]), id };
		}
	}
	CHECK(seen == ids);
	auto state = HeaderOnly(all);
	const auto secret = Key(all.account, 999, std::numeric_limits<int64>::min(), ArchiveChatKind::Secret);
	const auto wide = PlanArchiveEvent(state, Event(state, u"secret"_q, std::numeric_limits<uint64_t>::max(), u"secret body"_q, ArchiveOrigin::Baseline, secret));
	CHECK(database.run(TranslateArchiveSql(wide, security)));
	CHECK(database.query(ArchiveSqlReadHead(secret, security))->size() == 1);
	const auto text = database.query({ u"SELECT causal_order FROM messages WHERE message_key=?"_q,
		{ ArchiveSqlMessageId(secret, security) }, ArchiveSqlExpectation::OneRow });
	CHECK(std::get<QString>((*text)[0][0]) == ArchiveSqlOrder(std::numeric_limits<uint64_t>::max()));
	CHECK(!TranslateArchiveSql(PlanArchivePurge(all, Purge(all, ArchivePurgeKind::Trashbin)), security));
}

void LongHistoryAndAliases(const QString &path) {
	Section = "bounded heads retain long history and alias purges stay scoped";
	auto database = Database(path);
	auto state = Empty();
	const auto security = Security();
	CHECK(database.run(InitializeArchiveSql(state, security)));
	for (auto i = 0; i != 300; ++i) {
		const auto plan = PlanArchiveEvent(state, Event(state, u"edit"_q + QString::number(i), uint64_t(i + 1),
			i % 2 ? u"A"_q : u"B"_q, i == 0 ? ArchiveOrigin::Baseline : ArchiveOrigin::AuthoritativeEdit));
		const auto program = TranslateArchiveSql(plan, security);
		CHECK(program && program.statements.size() < 20);
		CHECK(database.run(program));
		state = plan.next;
		state.messages[0].versions = { state.messages[0].versions.back() };
		state.messages[0].receipts = { state.messages[0].receipts.back() };
	}
	const auto count = database.query({ u"SELECT COUNT(*) FROM versions"_q, {}, ArchiveSqlExpectation::OneRow });
	CHECK(std::get<int64>((*count)[0][0]) == 300);
	auto cursor = QString();
	auto historyCount = 0;
	for (;;) {
		const auto page = database.query(ArchiveSqlReadVersions(Key(), 64, security, cursor));
		CHECK(page);
		if (page->empty()) { break; }
		historyCount += int(page->size());
		cursor = std::get<QString>(page->back()[1]);
	}
	CHECK(historyCount == 300);
	const auto request = Purge(state, ArchivePurgeKind::EditHistory);
	const auto start = BeginArchiveSqlPurge(HeaderOnly(state), request, {}, u"long-history"_q, security);
	CHECK(start.program && database.run(start.program));
	ContinuePurge(database, start.next, security);
	state.revision++;
	state.accountGeneration++;
	const auto retained = database.query({ u"SELECT COUNT(*) FROM versions"_q, {}, ArchiveSqlExpectation::OneRow });
	const auto receipts = database.query({ u"SELECT COUNT(*) FROM events"_q, {}, ArchiveSqlExpectation::OneRow });
	CHECK(std::get<int64>((*retained)[0][0]) == 1);
	CHECK(std::get<int64>((*receipts)[0][0]) == 300);
	CHECK(database.query(ArchiveSqlReadReceipt(Key(), u"edit0"_q, security))->size() == 1);
	auto aliasDatabase = Database(path + u".aliases"_q);
	auto all = Empty();
	CHECK(aliasDatabase.run(InitializeArchiveSql(all, security)));
	for (auto chatId : { 45, 46, 47 }) {
		auto single = HeaderOnly(all);
		const auto key = Key(all.account, chatId, 100);
		const auto plan = PlanArchiveEvent(single, Event(single, u"base"_q, 1, u"body"_q, ArchiveOrigin::Baseline, key));
		CHECK(aliasDatabase.run(TranslateArchiveSql(plan, security)));
		single = plan.next;
		const auto gone = PlanArchiveEvent(single, Event(single, u"gone"_q, 2, {}, ArchiveOrigin::AuthoritativeDelete, key));
		CHECK(aliasDatabase.run(TranslateArchiveSql(gone, security)));
		all.revision = gone.next.revision;
		all.messages.push_back(gone.next.messages[0]);
	}
	CHECK(aliasDatabase.load(all.account, security) == all);
	auto scoped = Purge(all, ArchivePurgeKind::Trashbin, Key().chat);
	scoped.scope.migrationAliases.push_back({ ArchiveChatKind::Channel, 46 });
	ApplyPurge(aliasDatabase, all, scoped, security);
	CHECK(all.messages[0].payloadPurged);
	CHECK(all.messages[1].payloadPurged);
	CHECK(!all.messages[2].payloadPurged);
	CHECK(all.chatGenerations.size() == 2);
}

void CrossMessagePurgeCopies(const QString &path, ArchivePurgeKind kind) {
	Section = "reuse media before any source message is purged";
	auto database = Database(path);
	auto state = Empty();
	const auto security = Security();
	CHECK(database.run(InitializeArchiveSql(state, security)));
	auto event = Event(state, u"source-a"_q, 1);
	AddMedia(event);
	ApplySql(database, state, PlanArchiveEvent(state, event), security);
	ApplySql(database, state, PlanArchiveMediaStage(state, Job(state, u"copy"_q, u"source-a"_q)), security);
	ApplySql(database, state, PlanArchiveMediaCompletion(state, u"copy"_q, MediaSuccess()), security);
	ApplySql(database, state, PlanArchiveEvent(state, Event(state, u"source-b"_q, 2, u"no attachment"_q, ArchiveOrigin::AuthoritativeEdit)), security);
	auto target = Key(state.account, 46, 1);
	while (ArchiveSqlMessageId(target, security) < ArchiveSqlMessageId(Key(), security)) { ++target.messageId; }
	auto slice = HeaderOnly(state);
	auto incoming = Event(slice, u"target"_q, 1, u"target body"_q, ArchiveOrigin::Baseline, target);
	AddMedia(incoming);
	const auto targetPlan = PlanArchiveEvent(slice, incoming);
	CHECK(database.run(TranslateArchiveSql(targetPlan, security)));
	state.revision = targetPlan.next.revision;
	state.messages.push_back(targetPlan.next.messages[0]);
	CHECK(state.messages[1].versions[0].media[0].state == ArchiveMediaState::Missing);
	if (kind == ArchivePurgeKind::Trashbin) {
		auto sourceSlice = state;
		sourceSlice.messages.resize(1);
		const auto deleted = PlanArchiveEvent(sourceSlice, Event(sourceSlice, u"source-gone"_q, 3, {}, ArchiveOrigin::AuthoritativeDelete));
		CHECK(database.run(TranslateArchiveSql(deleted, security)));
		state.revision = deleted.next.revision;
		state.messages[0] = deleted.next.messages[0];
	}
	CHECK(database.load(state.account, security) == state);
	ApplyPurge(database, state, Purge(state, kind), security);
	CHECK(state.messages[1].versions[0].media[0].state == ArchiveMediaState::Complete);
	CHECK(CountArchiveBlobReferences(state, BlobId).versions == 1);
}

void PendingBlobProtection(const QString &path) {
	Section = "bounded completion cannot revive a GC-pending blob";
	auto database = Database(path);
	auto state = Empty();
	const auto security = Security();
	CHECK(database.run(InitializeArchiveSql(state, security)));
	const auto blob = ArchiveBlob{ BlobId, 7, ArchiveBlobState::GarbagePending };
	const auto token = ArchiveSqlToken(state.account, ArchiveIndexKind::Blob, blob.id.toUtf8(), security);
	const auto frame = SealArchiveSqlRow(state.account, token, blob, security);
	CHECK(frame);
	CHECK(database.query({ u"INSERT INTO blobs(blob_id,bytes,state,envelope) VALUES(?,'7',1,?)"_q,
		{ token, *frame }, ArchiveSqlExpectation::OneChange }));
	auto event = Event(state, u"incoming"_q, 1);
	AddMedia(event);
	const auto baseline = PlanArchiveEvent(state, event);
	CHECK(database.run(TranslateArchiveSql(baseline, security)));
	state = baseline.next;
	const auto stage = PlanArchiveMediaStage(state, Job(state, u"download"_q, u"incoming"_q));
	CHECK(database.run(TranslateArchiveSql(stage, security)));
	state = stage.next;
	const auto completion = PlanArchiveMediaCompletion(state, u"download"_q, MediaSuccess());
	CHECK(completion);
	CHECK(!database.run(TranslateArchiveSql(completion, security)));
	const auto pending = database.query({ u"SELECT state FROM blobs WHERE blob_id=?"_q, { token }, ArchiveSqlExpectation::OneRow });
	CHECK(std::get<int64>((*pending)[0][0]) == 1);
	const auto refs = database.query({ u"SELECT state FROM media_refs"_q, {}, ArchiveSqlExpectation::Rows });
	CHECK(refs->size() == 1 && std::get<int64>((*refs)[0][0]) == int64(ArchiveMediaState::Staged));
}

void FailureInjection(const QString &path) {
	Section = "SQLite BUSY, commit and result-count failures";
	auto database = Database(path);
	auto blocker = Database(path);
	auto state = Empty();
	const auto security = Security();
	CHECK(database.run(InitializeArchiveSql(state, security)));
	const auto plan = PlanArchiveEvent(state, Event(state, u"failure-test"_q, 1));
	const auto program = TranslateArchiveSql(plan, security);
	CHECK(program);
	CHECK(sqlite3_exec(blocker.raw(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);
	CHECK(!database.run(program));
	CHECK(database.load(state.account, security) == state);
	CHECK(sqlite3_exec(blocker.raw(), "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK);
	auto wrongCounts = program;
	wrongCounts.statements[1].rows = { 0, 0 };
	CHECK(!database.run(wrongCounts));
	CHECK(database.load(state.account, security) == state);
	sqlite3_commit_hook(database.raw(), [](void *) { return 1; }, nullptr);
	CHECK(!database.run(program));
	sqlite3_commit_hook(database.raw(), nullptr, nullptr);
	CHECK(database.load(state.account, security) == state);
	CHECK(database.run(program));
	state = plan.next;
	CHECK(database.load(state.account, security) == state);
	CHECK(database.query(ArchiveSqlReadMessage(Key(), security))->size() == 1);
	CHECK(database.query(ArchiveSqlReadReceipt(Key(), u"failure-test"_q, security))->size() == 1);
	const auto columns = database.query({ u"PRAGMA table_info(messages)"_q, {}, ArchiveSqlExpectation::Rows });
	CHECK(columns);
	for (const auto &column : *columns) {
		const auto name = std::get<QString>(column[1]);
		CHECK(name != u"chat_id"_q && name != u"collection_id"_q && name != u"representation"_q);
	}
	const auto indices = database.query({ u"SELECT chat_token,message_key,head_version FROM messages"_q, {}, ArchiveSqlExpectation::Rows });
	for (const auto &value : (*indices)[0]) { CHECK(IsArchiveHash(std::get<QString>(value))); }
	const auto pageCount = database.query({ u"PRAGMA page_count"_q, {}, ArchiveSqlExpectation::OneRow });
	const auto pages = std::get<int64>((*pageCount)[0][0]);
	CHECK(database.query({ u"PRAGMA max_page_count="_q + QString::number(pages), {}, ArchiveSqlExpectation::OneRow }));
	const auto large = PlanArchiveEvent(state, Event(state, u"disk-full"_q, 2, QString(200000, 'x'), ArchiveOrigin::AuthoritativeEdit));
	CHECK(!database.run(TranslateArchiveSql(large, security)));
	CHECK(database.load(state.account, security) == state);
	CHECK(database.query({ u"PRAGMA max_page_count=1000000000"_q, {}, ArchiveSqlExpectation::OneRow }));
}

void LargeBoundedAccount(const QString &path) {
	Section = "100000 message account uses bounded pages";
	auto database = Database(path);
	auto header = Empty();
	header.revision = 100000;
	const auto security = Security();
	CHECK(database.run(InitializeArchiveSql(header, security)));
	CHECK(sqlite3_exec(database.raw(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);
	auto statement = static_cast<sqlite3_stmt *>(nullptr);
	CHECK(sqlite3_prepare_v2(database.raw(), "INSERT INTO messages(message_key,chat_kind,chat_token,message_id,deleted,purged,transcript_date,head_version,causal_order,envelope) VALUES(?,2,?,?,1,0,100,'','00000000000000000001',?)", -1, &statement, nullptr) == SQLITE_OK);
	const auto chat = ArchiveSqlChatId(Key().chat, header.account, security).toUtf8();
	auto success = true;
	for (auto i = 1; i <= 100000; ++i) {
		auto message = ArchiveMessage();
		message.key = Key(header.account, 45, i);
		message.order = 1;
		message.deleted = true;
		message.deletionOrigin = ArchiveOrigin::AuthoritativeDelete;
		message.deletedObservedAt = 100;
		const auto token = ArchiveSqlMessageId(message.key, security).toUtf8();
		const auto frame = SealArchiveSqlRow(header.account, QString::fromLatin1(token), message, security);
		if (!frame) { success = false; break; }
		if (sqlite3_bind_text(statement, 1, token.constData(), int(token.size()), SQLITE_TRANSIENT) != SQLITE_OK
			|| sqlite3_bind_text(statement, 2, chat.constData(), int(chat.size()), SQLITE_TRANSIENT) != SQLITE_OK
			|| sqlite3_bind_int64(statement, 3, i) != SQLITE_OK
			|| sqlite3_bind_blob(statement, 4, frame->constData(), int(frame->size()), SQLITE_TRANSIENT) != SQLITE_OK
			|| sqlite3_step(statement) != SQLITE_DONE || sqlite3_reset(statement) != SQLITE_OK) { success = false; break; }
	}
	CHECK(success);
	CHECK(sqlite3_finalize(statement) == SQLITE_OK);
	CHECK(sqlite3_exec(database.raw(), "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK);
	const auto request = Purge(header, ArchivePurgeKind::Trashbin);
	const auto start = BeginArchiveSqlPurge(header, request, {}, u"large-purge"_q, security);
	CHECK(start.program && start.program.statements.size() < 12);
	CHECK(database.run(start.program));
	const auto prepare = ContinueArchiveSqlPurge(start.next, {}, security);
	CHECK(prepare.program && database.run(prepare.program));
	const auto messages = ReloadPurge(database, prepare.next, security);
	CHECK(messages.phase == ArchivePurgePhase::Messages);
	const auto skipped = ContinueArchiveSqlPurge(messages, {}, security);
	CHECK(skipped.program && !database.run(skipped.program));
	auto processed = uint64_t(0);
	ContinuePurge(database, messages, security, false, &processed);
	CHECK(processed == 100000);
	const auto count = database.query({ u"SELECT COUNT(*) FROM messages WHERE purged=1"_q, {}, ArchiveSqlExpectation::OneRow });
	CHECK(count && std::get<int64>((*count)[0][0]) == 100000);
}


void HmacFailureRecovery(const QString &path) {
	Section = "review HMAC failures never commit and healthy recovery matches begin";
	auto database = Database(path);
	auto state = Empty();
	const auto security = Security();
	CHECK(database.run(InitializeArchiveSql(state, security)));
	auto event = Event(state, u"body"_q, 1);
	AddMedia(event);
	ApplySql(database, state, PlanArchiveEvent(state, event), security);
	auto failing = security;
	failing.hmacSha256 = [base = security.hmacSha256](const QByteArray &bytes) -> std::optional<QByteArray> {
		return bytes.contains("scope/") ? std::nullopt : base(bytes);
	};
	const auto request = Purge(state, ArchivePurgeKind::Trashbin);
	const auto scopeFailure = BeginArchiveSqlPurge(HeaderOnly(state), request, {}, u"purge"_q, failing);
	CHECK(!scopeFailure.program && scopeFailure.program.statements.empty());
	CHECK(!database.run(scopeFailure.program));
	CHECK(database.load(state.account, security) == state);
	const auto begin = BeginArchiveSqlPurge(HeaderOnly(state), request, {}, u"purge"_q, security);
	CHECK(begin.program && database.run(begin.program));
	const auto page = database.query(ArchiveSqlPurgePage(begin.next, 128, security));
	CHECK(page && page->size() == 1);
	const auto input = PurgeRows(database, begin.next, *page, security);
	failing.hmacSha256 = [base = security.hmacSha256](const QByteArray &bytes) -> std::optional<QByteArray> {
		return bytes.contains("document:555/document") ? std::nullopt : base(bytes);
	};
	const auto refused = ContinueArchiveSqlPurge(begin.next, input, failing);
	CHECK(!refused.program && refused.program.statements.empty() && !database.run(refused.program));
	const auto tokens = database.query({ u"SELECT media_token FROM media_refs"_q, {}, ArchiveSqlExpectation::Rows });
	CHECK(tokens && tokens->size() == 1 && !std::get<QString>((*tokens)[0][0]).isEmpty());
	ContinuePurge(database, ReloadPurge(database, begin.next, security), security);
	const auto gap = ArchiveCaptureGap{ state.account, security.storeId, u"gap"_q, 123, u"source-unavailable"_q };
	failing.hmacSha256 = [base = security.hmacSha256](const QByteArray &bytes) -> std::optional<QByteArray> {
		return bytes.contains("gap-fingerprint/") ? std::nullopt : base(bytes);
	};
	const auto gapFailure = PlanArchiveCaptureGap(gap, failing);
	CHECK(!gapFailure && gapFailure.statements.empty() && !database.run(gapFailure));
	CHECK(database.run(PlanArchiveCaptureGap(gap, security)));
	CHECK(database.run(PlanArchiveCaptureGap(gap, security)));
}

void IndexAssociationSubstitution(const QString &path, bool inverse) {
	Section = inverse ? "inverse substitution toward complete attachment refuses" : "review owned-copy plaintext substitution refuses";
	auto database = Database(path);
	auto state = Empty();
	const auto security = Security();
	CHECK(database.run(InitializeArchiveSql(state, security)));
	auto source = Event(state, u"source"_q, 1);
	AddMedia(source);
	ApplySql(database, state, PlanArchiveEvent(state, source), security);
	ApplySql(database, state, PlanArchiveMediaStage(state, Job(state, u"source-job"_q, u"source"_q)), security);
	ApplySql(database, state, PlanArchiveMediaCompletion(state, u"source-job"_q, MediaSuccess()), security);
	if (!inverse) {
		ApplySql(database, state, PlanArchiveEvent(state, Event(state, u"source-edited"_q, 2, u"no attachment"_q, ArchiveOrigin::AuthoritativeEdit)), security);
	}
	const auto targetKey = Key(state.account, 46, 1);
	auto slice = HeaderOnly(state);
	auto target = Event(slice, u"target"_q, 1, u"target body"_q, ArchiveOrigin::Baseline, targetKey);
	target.content->media.push_back({ u"document:999"_q, u"document"_q });
	const auto insert = PlanArchiveEvent(slice, target);
	CHECK(insert && database.run(TranslateArchiveSql(insert, security)));
	state.revision = insert.next.revision;
	state.messages.push_back(insert.next.messages[0]);
	CHECK(database.load(state.account, security) == state);
	const auto forgedToken = ArchiveSqlToken(state.account, ArchiveIndexKind::Media,
		inverse ? QByteArray("document:555/document") : QByteArray("document:999/document"), security);
	const auto forgedOwner = inverse ? targetKey : Key();
	CHECK(database.query({ u"UPDATE media_refs SET media_token=? WHERE message_key=?"_q,
		{ forgedToken, ArchiveSqlMessageId(forgedOwner, security) }, ArchiveSqlExpectation::OneChange }));
	const auto original = state;
	const auto request = Purge(state, ArchivePurgeKind::EditHistory);
	const auto begin = BeginArchiveSqlPurge(HeaderOnly(state), request, {}, u"purge"_q, security);
	CHECK(begin.program && database.run(begin.program));
	const auto page = database.query(ArchiveSqlPurgePage(begin.next, 128, security));
	CHECK(page && page->size() == 1);
	const auto inputs = PurgeRows(database, begin.next, *page, security);
	const auto attempt = ContinueArchiveSqlPurge(begin.next, inputs, security);
	CHECK(!attempt.program || !database.run(attempt.program));
	const auto unchanged = database.query({ u"SELECT envelope FROM versions WHERE message_key=?"_q,
		{ ArchiveSqlMessageId(targetKey, security) }, ArchiveSqlExpectation::OneRow });
	CHECK(unchanged);
	const auto targetId = ArchiveSqlChildId(targetKey, ArchiveIndexKind::Version, u"target"_q, security);
	const auto decoded = OpenRow(state.account, targetId, ArchiveRecordKind::Version,
		std::get<QByteArray>((*unchanged)[0][0]), security);
	CHECK(decoded && std::get<ArchiveVersionSnapshot>(decoded->value).version == original.messages.back().versions[0]);
	const auto pins = database.query(ArchiveSqlGarbagePage(128));
	CHECK(pins && pins->empty());
	std::printf("INDEX_ASSOCIATION_FIXED: inverse=%d refused=1 target_unchanged=1 pins_retained=1\n", inverse);
}

void MetadataRestoreAndRewrite(const QString &path) {
	Section = "future snapshot wrapper metadata remains byte exact through SQL rewrite";
	auto database = Database(path);
	auto state = Empty();
	const auto security = Security();
	CHECK(database.run(InitializeArchiveSql(state, security)));
	auto event = Event(state, u"base"_q, 1);
	AddMedia(event);
	ApplySql(database, state, PlanArchiveEvent(state, event), security);
	auto snapshot = ArchiveVersionSnapshot{ Key(), state.messages[0].versions[0] };
	snapshot.preserved[u"futureMetadata"_q] = u"preserve"_q;
	const auto expected = EncodeArchiveRow({ snapshot });
	const auto id = ArchiveSqlChildId(Key(), ArchiveIndexKind::Version, u"base"_q, security);
	const auto frame = SealArchiveSqlRow(state.account, id, snapshot, security);
	CHECK(frame && expected);
	CHECK(database.query({ u"UPDATE versions SET envelope=?"_q, { *frame }, ArchiveSqlExpectation::OneChange }));
	const auto restored = database.loadSlice(state.account, security);
	CHECK(restored);
	auto loaded = *restored;
	state = loaded.state;
	CHECK(loaded.versionMetadata[0].preserved == snapshot.preserved);
	CHECK(EncodeArchiveRow({ ArchiveSqlVersionSnapshot(Key(), state.messages[0].versions[0], loaded.versionMetadata) }) == expected);
	const auto applyMetadata = [&](const ArchiveTransaction &plan) {
		CHECK(plan);
		const auto program = TranslateArchiveSql(plan, loaded, security);
		CHECK(program && database.run(program));
		loaded.state = plan.next;
		state = plan.next;
		const auto actual = database.loadSlice(state.account, security);
		CHECK(actual && actual->state == state && actual->versionMetadata == loaded.versionMetadata);
	};
	applyMetadata(PlanArchiveMediaStage(state, Job(state, u"metadata-job"_q, u"base"_q)));
	CHECK(loaded.versionMetadata[0].preserved == snapshot.preserved);
	applyMetadata(PlanArchiveMediaCompletion(state, u"metadata-job"_q, MediaSuccess()));
	CHECK(loaded.versionMetadata[0].preserved == snapshot.preserved);
	const auto rewritten = EncodeArchiveRow({ ArchiveSqlVersionSnapshot(Key(), state.messages[0].versions[0], loaded.versionMetadata) });
	CHECK(rewritten);
	const auto request = Purge(state, ArchivePurgeKind::EditHistory);
	ApplyPurge(database, state, request, security);
	const auto rows = database.query(ArchiveSqlReadVersion(Key(), u"base"_q, security));
	CHECK(rows && rows->size() == 1);
	const auto opened = OpenArchiveEnvelope({ state.account, security.storeId, ArchiveRecordKind::Version, id.toUtf8() },
		security.keyId, std::get<QByteArray>((*rows)[0][0]), security.crypto);
	CHECK(opened && opened.bytes == *rewritten);
	const auto finalRow = DecodeArchiveRow(opened.bytes);
	CHECK(finalRow.row && std::get<ArchiveVersionSnapshot>(finalRow.row->value).preserved == snapshot.preserved);
	CHECK(database.load(state.account, security) == state);
}


void SamePageOwner(const QString &path) {
	Section = "validate original owners before any same-page rewrite";
	auto database = Database(path);
	const auto security = Security();
	auto state = Empty();
	CHECK(database.run(InitializeArchiveSql(state, security)));
	auto source = Event(state, u"source"_q, 1);
	AddMedia(source);
	source.content->media.push_back({ u"document:666"_q, u"document"_q });
	ApplySql(database, state, PlanArchiveEvent(state, source), security);
	ApplySql(database, state, PlanArchiveMediaStage(state, Job(state, u"job"_q, u"source"_q)), security);
	ApplySql(database, state, PlanArchiveMediaCompletion(state, u"job"_q, MediaSuccess()), security);
	const auto sourceId = ArchiveSqlMessageId(Key(), security);
	auto targetKey = Key(state.account, 46, 1);
	while (ArchiveSqlMessageId(targetKey, security) <= sourceId) { ++targetKey.messageId; }
	const auto slice = HeaderOnly(state);
	auto target = Event(slice, u"target"_q, 1, u"target"_q, ArchiveOrigin::Baseline, targetKey);
	AddMedia(target);
	const auto added = PlanArchiveEvent(slice, target);
	CHECK(added && database.run(TranslateArchiveSql(added, security)));
	state.revision = added.next.revision;
	state.messages.push_back(added.next.messages[0]);
	CHECK(database.load(state.account, security) == state);
	const auto request = Purge(state, ArchivePurgeKind::EditHistory);
	const auto expected = PlanArchivePurge(state, request);
	const auto begin = BeginArchiveSqlPurge(HeaderOnly(state), request, {}, u"page"_q, security);
	CHECK(begin.program && database.run(begin.program));
	const auto page = database.query(ArchiveSqlPurgePage(begin.next, 128, security));
	CHECK(page && page->size() == 2);
	const auto inputs = PurgeRows(database, begin.next, *page, security);
	CHECK(inputs[0].stored.id.startsWith(sourceId));
	const auto next = ContinueArchiveSqlPurge(begin.next, inputs, security);
	CHECK(next.program && next.processedRows == 2 && database.run(next.program));
	ContinuePurge(database, next.next, security);
	CHECK(expected && database.load(state.account, security) == expected.next);
}

size_t ManyAttachments(const QString &path, int count, int targetCount = 1) {
	Section = "deduplicated owner validation remains linear and resumes prefixes";
	auto database = Database(path);
	const auto security = Security();
	auto state = Empty();
	CHECK(database.run(InitializeArchiveSql(state, security)));
	auto source = Event(state, u"source"_q, 1);
	for (auto i = 0; i != count; ++i) {
		source.content->media.push_back({ u"document:"_q + QString::number(1000 + i), u"document"_q });
	}
	ApplySql(database, state, PlanArchiveEvent(state, source), security);
	for (auto i = 0; i != count; ++i) {
		auto job = Job(state, u"job-"_q + QString::number(i), u"source"_q);
		job.mediaIdentity = source.content->media[size_t(i)].identity;
		const auto stage = PlanArchiveMediaStage(state, job);
		CHECK(stage && database.run(TranslateArchiveSql(stage, security)));
		state = stage.next;
		const auto done = PlanArchiveMediaCompletion(state, job.id, MediaSuccess());
		CHECK(done && database.run(TranslateArchiveSql(done, security)));
		state = done.next;
	}
	ApplySql(database, state, PlanArchiveEvent(state, Event(state, u"edited"_q, 2, u"none"_q, ArchiveOrigin::AuthoritativeEdit)), security);
	for (auto i = 0; i != targetCount; ++i) {
		const auto key = Key(state.account, 46, i + 1);
		const auto slice = HeaderOnly(state);
		auto target = Event(slice, u"target"_q, 1, u"target"_q, ArchiveOrigin::Baseline, key);
		target.content->media = source.content->media;
		const auto added = PlanArchiveEvent(slice, target);
		CHECK(added && database.run(TranslateArchiveSql(added, security)));
		state.revision = added.next.revision;
		state.messages.push_back(added.next.messages[0]);
	}
	CHECK(database.load(state.account, security) == state);
	const auto request = Purge(state, ArchivePurgeKind::EditHistory);
	const auto expected = PlanArchivePurge(state, request);
	const auto begin = BeginArchiveSqlPurge(HeaderOnly(state), request, {}, u"many"_q, security);
	CHECK(begin.program && database.run(begin.program));
	const auto page = database.query(ArchiveSqlPurgePage(begin.next, 128, security));
	CHECK(page && int(page->size()) == targetCount);
	const auto inputs = PurgeRows(database, begin.next, *page, security);
	CHECK(!inputs.empty() && int(inputs[0].copies.size()) == count);
	const auto next = ContinueArchiveSqlPurge(begin.next, inputs, security);
	CHECK(next.program && next.processedRows > 0 && database.run(next.program));
	if (targetCount == 1) {
		CHECK(next.processedRows == 1);
		CHECK(next.program.statements.size() <= size_t(3 * count + 32));
		const auto &frame = inputs[0].copies[0].ownerFrame;
		auto ownerGuards = 0;
		for (const auto &statement : next.program.statements) {
			for (const auto &bind : statement.binds) {
				if (const auto bytes = std::get_if<QByteArray>(&bind); bytes && *bytes == frame) { ++ownerGuards; }
			}
		}
		CHECK(ownerGuards == 1);
	} else {
		CHECK(next.processedRows < inputs.size());
	}
	std::printf("MANY_ATTACHMENTS_FIXED refs=%d targets=%d statements=%zu processed=%zu\n",
		count, targetCount, next.program.statements.size(), next.processedRows);
	ContinuePurge(database, next.next, security);
	CHECK(expected && database.load(state.account, security) == expected.next);
	return next.program.statements.size();
}

void LargeBodyPurge(const QString &path, int sourceBytes, ArchivePurgeKind kind, bool ownChat) {
	Section = "chunked genuine Markdown retains exact bytes and never blocks scoped purge";
	auto database = Database(path);
	const auto security = Security();
	auto state = Empty();
	CHECK(database.run(InitializeArchiveSql(state, security)));
	const auto small = PlanArchiveEvent(state, Event(state, u"small"_q, 1));
	CHECK(small && database.run(TranslateArchiveSql(small, security)));
	state = HeaderOnly(small.next);
	const auto key = Key(state.account, 999, 1);
	auto event = Event(state, u"markdown-file"_q, 1, u"notes.md"_q, ArchiveOrigin::Baseline, key);
	AddMedia(event);
	auto markdown = QByteArray();
	while (markdown.size() < sourceBytes) { markdown += "# Notes\n\nA retained Markdown paragraph.\n\n"; }
	markdown.truncate(sourceBytes);
	event.source = ArchiveSource{ ArchiveSourceKind::ReceivedMarkdownFile, ArchiveContentHash(*event.content),
		key.representation, markdown, event.id };
	event.source->binding = ArchiveSourceBinding{ key, event.id };
	const auto capture = PlanArchiveSqlCapture(state, event, security);
	if (sourceBytes == 10 * 1024 * 1024) {
		CHECK(!capture.program && capture.program.status == ArchiveSqlStatus::RetentionGap);
		CHECK(!capture.program.retentionGapReason.isEmpty() && capture.program.statements.empty());
		CHECK(database.run(PlanArchiveCaptureGap({ state.account, security.storeId, u"too-large"_q,
			2, capture.program.retentionGapReason }, security)));
		CHECK(database.query(ArchiveSqlReadCaptureGaps(10))->size() == 1);
	} else {
		CHECK(capture.transaction && capture.program);
		if (sourceBytes == 50000 && ownChat && kind == ArchivePurgeKind::Trashbin) {
			for (auto cut = size_t(1); cut < capture.program.statements.size(); ++cut) {
				CHECK(!database.run(capture.program, cut));
				const auto chunks = database.query({ u"SELECT COUNT(*) FROM body_chunks"_q, {}, ArchiveSqlExpectation::OneRow });
				CHECK(chunks && std::get<int64>((*chunks)[0][0]) == 0);
			}
		}
		CHECK(database.run(capture.program));
		state = capture.transaction.next;
		const auto stored = database.query(ArchiveSqlReadVersion(key, event.id, security));
		CHECK(stored && stored->size() == 1);
		const auto id = ArchiveSqlChildId(key, ArchiveIndexKind::Version, event.id, security);
		const auto projection = OpenRow(state.account, id, ArchiveRecordKind::Version,
			std::get<QByteArray>((*stored)[0][0]), security);
		CHECK(projection && projection->body);
		CHECK(std::get<QByteArray>((*stored)[0][0]).size() < kArchiveSqlInlineBodyLimit);
		const auto chunkRows = database.query(ArchiveSqlReadBodyChunks(*projection, security));
		CHECK(chunkRows && chunkRows->size() <= 4);
		auto chunks = std::vector<QByteArray>();
		for (const auto &chunk : *chunkRows) { chunks.push_back(std::get<QByteArray>(chunk[0])); }
		const auto restored = RestoreArchiveSqlVersionBody(*projection, chunks, security);
		CHECK(restored && std::get<ArchiveVersionSnapshot>(restored->value).version.source->bytes == markdown);
		auto altered = chunks;
		altered[0][altered[0].size() - 1] ^= 1;
		CHECK(!RestoreArchiveSqlVersionBody(*projection, altered, security));
		altered = chunks;
		altered.pop_back();
		CHECK(!RestoreArchiveSqlVersionBody(*projection, altered, security));
		auto wrong = *projection;
		wrong.body->hash[0] = wrong.body->hash[0] == 'a' ? 'b' : 'a';
		CHECK(!RestoreArchiveSqlVersionBody(wrong, chunks, security));
		const auto loaded = database.load(state.account, security);
		CHECK(loaded);
		const auto copy = ChooseCopySource(*loaded, { key, event.id, event.id, ArchiveContentHash(*event.content),
			key.representation, ArchiveCopyExtent::FullBody, ArchiveCopyTarget::CurrentMessage,
			ArchiveGenerationFor(*loaded, key.chat) });
		CHECK(copy.validTarget && copy.kind == ArchiveCopySourceKind::GenuineSource && copy.bytes == markdown);
		if (kind == ArchivePurgeKind::EditHistory) {
			event = Event(state, u"markdown-new"_q, 2, u"new notes.md"_q, ArchiveOrigin::AuthoritativeEdit, key);
			AddMedia(event);
			event.source = ArchiveSource{ ArchiveSourceKind::ReceivedMarkdownFile, ArchiveContentHash(*event.content),
				key.representation, markdown, event.id };
			event.source->binding = ArchiveSourceBinding{ key, event.id };
			const auto edited = PlanArchiveSqlCapture(state, event, security);
			CHECK(edited.program && database.run(edited.program));
			state = edited.transaction.next;
		}
		const auto deleted = PlanArchiveEvent(state, Event(state, u"deleted"_q, 3, {}, ArchiveOrigin::AuthoritativeDelete, key));
		CHECK(deleted && database.run(TranslateArchiveSql(deleted, security)));
		state = deleted.next;
	}
	const auto loaded = database.load(state.account, security);
	CHECK(loaded);
	const auto chat = ownChat ? key.chat : Key().chat;
	const auto header = HeaderOnly(*loaded);
	const auto beforeBodies = database.query({ u"SELECT COUNT(*) FROM body_refs"_q, {}, ArchiveSqlExpectation::OneRow });
	const auto begin = BeginArchiveSqlPurge(header, Purge(header, kind, chat), {{ chat, 0 }}, u"large-purge"_q, security);
	CHECK(begin.program && database.run(begin.program));
	ContinuePurge(database, begin.next, security, sourceBytes == 50000 && ownChat);
	const auto gate = database.query({ u"SELECT maintenance_id FROM archive_meta"_q, {}, ArchiveSqlExpectation::OneRow });
	CHECK(gate && std::get<QString>((*gate)[0][0]).isEmpty());
	const auto after = database.load(state.account, security);
	CHECK(after);
	const auto expected = PlanArchivePurge(*loaded, Purge(*loaded, kind, chat));
	CHECK(expected && *after == expected.next);
	const auto afterBodies = database.query({ u"SELECT COUNT(*) FROM body_refs"_q, {}, ArchiveSqlExpectation::OneRow });
	const auto bodyCount = std::get<int64>((*afterBodies)[0][0]);
	CHECK(bodyCount == (sourceBytes == 10 * 1024 * 1024 ? 0
		: !ownChat ? std::get<int64>((*beforeBodies)[0][0]) : kind == ArchivePurgeKind::Trashbin ? 0 : 1));
	if (sourceBytes != 10 * 1024 * 1024 && (!ownChat || kind == ArchivePurgeKind::EditHistory)) {
		const auto version = FindArchiveVersion(*after, key, event.id);
		CHECK(version && version->source && version->source->bytes == markdown);
	}
	const auto orphaned = database.query({ u"SELECT COUNT(*) FROM body_chunks c LEFT JOIN body_refs r USING(message_key,version_id) "
		"WHERE r.message_key IS NULL"_q, {}, ArchiveSqlExpectation::OneRow });
	CHECK(orphaned && std::get<int64>((*orphaned)[0][0]) == 0);
}

void PurgeAdmission(const QString &path) {
	Section = "unfit units refuse before the maintenance gate or epoch changes";
	auto database = Database(path);
	const auto security = Security();
	auto state = Empty();
	CHECK(database.run(InitializeArchiveSql(state, security)));
	auto event = Event(state, u"large"_q, 1);
	for (auto i = 0; i <= kArchiveSqlPurgeMediaLimit; ++i) {
		event.content->media.push_back({ u"document:"_q + QString::number(i), u"document"_q });
	}
	const auto capture = PlanArchiveSqlCapture(state, event, security);
	CHECK(!capture.program && capture.program.status == ArchiveSqlStatus::RetentionGap);
	CHECK(!capture.program.retentionGapReason.isEmpty() && capture.program.statements.empty());
	CHECK(database.load(state.account, security) == state);
	const auto begin = BeginArchiveSqlPurge(HeaderOnly(state), Purge(state, ArchivePurgeKind::EditHistory), {}, u"fit"_q, security);
	CHECK(begin.program && database.run(begin.program));
	ContinuePurge(database, begin.next, security);
}

}

int main() {
	const auto directory = QTemporaryDir();
	CHECK(directory.isValid());
	for (const auto bytes : { 50000, 1024 * 1024, 10 * 1024 * 1024 }) {
		for (const auto kind : { ArchivePurgeKind::Trashbin, ArchivePurgeKind::EditHistory }) {
			for (const auto own : { false, true }) {
				LargeBodyPurge(directory.path() + QString::number(bytes) + '-' + QString::number(int(kind))
					+ '-' + QString::number(int(own)) + u".sqlite"_q, bytes, kind, own);
			}
		}
	}
	SamePageOwner(directory.path() + u"/same-owner.sqlite"_q);
	const auto fifty = ManyAttachments(directory.path() + u"/many50.sqlite"_q, 50);
	const auto larger = ManyAttachments(directory.path() + u"/many128.sqlite"_q, 128);
	CHECK(larger < fifty * 3);
	ManyAttachments(directory.path() + u"/prefix.sqlite"_q, 64, 32);
	PurgeAdmission(directory.path() + u"/admission.sqlite"_q);
	HmacFailureRecovery(directory.path() + u"/hmac-review.sqlite"_q);
	IndexAssociationSubstitution(directory.path() + u"/index-review.sqlite"_q, false);
	IndexAssociationSubstitution(directory.path() + u"/index-inverse.sqlite"_q, true);
	MetadataRestoreAndRewrite(directory.path() + u"/metadata-review.sqlite"_q);
	PlansAndMedia(directory.path() + u"/plans.sqlite"_q);
	PagesAndSlices(directory.path() + u"/pages.sqlite"_q);
	LongHistoryAndAliases(directory.path() + u"/history.sqlite"_q);
	CrossMessagePurgeCopies(directory.path() + u"/copy-trash.sqlite"_q, ArchivePurgeKind::Trashbin);
	CrossMessagePurgeCopies(directory.path() + u"/copy-history.sqlite"_q, ArchivePurgeKind::EditHistory);
	PendingBlobProtection(directory.path() + u"/pending.sqlite"_q);
	FailureInjection(directory.path() + u"/failures.sqlite"_q);
	LargeBoundedAccount(directory.path() + u"/large.sqlite"_q);
	return Finish();
}
