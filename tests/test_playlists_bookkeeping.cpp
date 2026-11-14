/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists.h"
#include "purple/purple_playlists_local.h"
#include "purple/purple_sync_json.h"
#include "purple/purple_types.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <vector>

using namespace Purple;

namespace {

auto Checks = 0;
auto Failures = 0;

void Check(bool good, const char *condition, int line) {
	++Checks;
	if (!good) {
		++Failures;
		std::printf("FAIL line %d: %s\n", line, condition);
	}
}

#define CHECK(cond) Check(bool(cond), #cond, __LINE__)

QString MakeHexHash(QChar ch) {
	return QString(64, ch);
}

PlaylistLocalState MakeCombinedBookkeepingState() {
	PlaylistLocalState state = DefaultPlaylistLocalState();
	state.sync = false;
	state.chat = PeerToken{ PeerTokenKind::Channel, 9001 };
	state.unsyncedSince = uint64_t(1700000000123ULL);
	state.status = PlaylistLocalStepStatus{
		PlaylistLocalSyncResult::Problem,
		uint64_t(1700000555000ULL),
		QJsonObject{
			{ u"future_route"_q, QJsonObject{{ u"attempt"_q, 2 }} },
		},
	};
	state.memo.emplace(
		2,
		PlaylistLocalMemoEntry{
			1ULL,
			PlaylistLocalMemoState::Valid,
			1,
			MakeHexHash(u'a'),
			QJsonObject{{ u"future_note"_q, QJsonArray{ 1, 2 } }},
		});
	state.memo.emplace(
		10,
		PlaylistLocalMemoEntry{
			std::numeric_limits<uint64_t>::max(),
			PlaylistLocalMemoState::Invalid,
			kMaximumPlaylistMessageId,
			std::nullopt,
			{},
		});
	state.memo.emplace(
		1,
		PlaylistLocalMemoEntry{
			55ULL,
			PlaylistLocalMemoState::NewerFormat,
			3,
			std::nullopt,
			{},
		});
	state.memo.emplace(
		kMaximumPlaylistMessageId,
		PlaylistLocalMemoEntry{
			777ULL,
			PlaylistLocalMemoState::Oversized,
			4,
			std::nullopt,
			{},
		});
	Playlist hiddenSnap;
	hiddenSnap.title = PlaylistTitleRegister{ u"Hidden Keep"_q, 100 };
	hiddenSnap.deleted = PlaylistDeletedRegister{ false, 200 };
	state.hidden.emplace(
		u"pl-aaaaaaaaaaaaaaaa"_q,
		PlaylistLocalHiddenEntry{
			PlaylistNoticeReason::PurgedElsewhere,
			std::move(hiddenSnap),
			{},
		});
	state.dueSeen.emplace(
		u"pl-aaaaaaaaaaaaaaaa"_q,
		PlaylistLocalDueSeenEntry{
			PlaylistAutoPurgeDueSeen{ 200, 300 },
			{},
		});
	state.recentUse.emplace(u"pl-aaaaaaaaaaaaaaaa"_q, 0);
	state.preserved.insert(
		u"old_chats"_q,
		QJsonObject{{ u"channels"_q, QJsonArray{ 101, 202 } }});
	state.preserved.insert(u"future_root"_q, u"kept"_q);
	return state;
}

void TestDefaultEmptyFullAndOldChatsOpaqueness() {
	const auto def = DefaultPlaylistLocalState();
	CHECK(!def.chat.has_value());
	CHECK(def.memo.empty());
	CHECK(def.status == PlaylistLocalStepStatus{});
	CHECK(!def.status.lastStepAtMs.has_value());
	CHECK(!def.unsyncedSince.has_value());
	CHECK(bool(ValidatePlaylistLocalState(def)));

	const auto parsedEmptyObj = ParsePlaylistLocalState(R"({"memo":{},"status":{}})");
	CHECK(bool(parsedEmptyObj));
	CHECK(parsedEmptyObj.state == def);

	// Non-empty memo without bound chat must be refused identically on parse & write.
	const auto rawMemoNoChat = QByteArray(
		R"({"memo":{"1":{"classifier":1,"document_id":"2","state":"oversized"}}})");
	const auto parsedNoChat = ParsePlaylistLocalState(rawMemoNoChat);
	CHECK(parsedNoChat.status == PlaylistLocalStatus::Invalid);
	CHECK(parsedNoChat.error == PlaylistLocalError::MissingField);
	CHECK(parsedNoChat.state == PlaylistLocalState{});

	PlaylistLocalState cppNoChat;
	cppNoChat.memo.emplace(
		1,
		PlaylistLocalMemoEntry{
			2ULL,
			PlaylistLocalMemoState::Oversized,
			1,
			std::nullopt,
			{},
		});
	CHECK(ValidatePlaylistLocalState(cppNoChat).error == PlaylistLocalError::MissingField);
	const auto wroteNoChat = SerializePlaylistLocalState(cppNoChat);
	CHECK(!bool(wroteNoChat) && wroteNoChat.json.isEmpty());

	// Full combined round-trip and old_chats invariance across bookkeeping mutations.
	const auto full = MakeCombinedBookkeepingState();
	const auto fullWrote = SerializePlaylistLocalState(full, 4242);
	CHECK(bool(fullWrote));
	const auto fullParsed = ParsePlaylistLocalState(fullWrote.json, 4242);
	CHECK(bool(fullParsed));
	CHECK(fullParsed.state == full);
	CHECK(!fullParsed.state.preserved.contains(u"chat"_q));
	CHECK(!fullParsed.state.preserved.contains(u"memo"_q));
	CHECK(!fullParsed.state.preserved.contains(u"status"_q));
	CHECK(!fullParsed.state.preserved.contains(u"unsynced_since"_q));
	CHECK(fullParsed.state.preserved.value(u"old_chats"_q)
		== full.preserved.value(u"old_chats"_q));

	auto mutated = fullParsed.state;
	mutated.sync = true;
	mutated.chat = PeerToken{};
	mutated.memo.erase(2);
	mutated.status = PlaylistLocalStepStatus{
		PlaylistLocalSyncResult::Synced,
		uint64_t(0),
		{},
	};
	mutated.unsyncedSince = std::nullopt;
	const auto mutatedWrote = SerializePlaylistLocalState(mutated, 4242);
	CHECK(bool(mutatedWrote));
	const auto mutatedParsed = ParsePlaylistLocalState(mutatedWrote.json, 4242);
	CHECK(bool(mutatedParsed));
	CHECK(mutatedParsed.state == mutated);
	CHECK(mutatedParsed.state.preserved.value(u"old_chats"_q)
		== full.preserved.value(u"old_chats"_q));

	// Format > 1 stays NewerFormat even alongside invalid bookkeeping payloads.
	const auto newerWithGarbage = ParsePlaylistLocalState(
		R"({"chat":123,"format":2,"memo":{"0":"bad"},"status":"oops","unsynced_since":0})");
	CHECK(newerWithGarbage.status == PlaylistLocalStatus::NewerFormat);
	CHECK(newerWithGarbage.error == PlaylistLocalError::None);
	CHECK(newerWithGarbage.state == PlaylistLocalState{});
}

void TestChatBindingAndNormalization() {
	constexpr auto selfId = uint64_t(4242);

	const auto parsedSelf = ParsePlaylistLocalState(R"({"chat":"self"})", selfId);
	CHECK(bool(parsedSelf));
	CHECK(parsedSelf.state.chat == PeerToken{});

	const auto parsedNumericSelf = ParsePlaylistLocalState(
		R"({"chat":"user:4242"})",
		selfId);
	CHECK(bool(parsedNumericSelf));
	CHECK(parsedNumericSelf.state.chat == PeerToken{});
	const auto numericSelfWrote = SerializePlaylistLocalState(parsedNumericSelf.state, selfId);
	CHECK(bool(numericSelfWrote) && numericSelfWrote.json.contains(R"("chat":"self")"));
	CHECK(!numericSelfWrote.json.contains("user:4242"));

	// C++ PeerToken{User, selfId} normalizes identically on write without leaking ID.
	PlaylistLocalState cppSelfUser;
	cppSelfUser.chat = PeerToken{ PeerTokenKind::User, selfId };
	const auto cppSelfWrote = SerializePlaylistLocalState(cppSelfUser, selfId);
	CHECK(bool(cppSelfWrote) && cppSelfWrote.json.contains(R"("chat":"self")"));
	CHECK(!cppSelfWrote.json.contains("user:4242"));

	// Channel preserves exact identity across different accounts.
	const auto maxChannelRaw = QByteArray(
		R"({"chat":"channel:9223372036854775807"})");
	const auto parsedChannelA = ParsePlaylistLocalState(maxChannelRaw, selfId);
	const auto parsedChannelB = ParsePlaylistLocalState(maxChannelRaw, 9999);
	CHECK(bool(parsedChannelA) && bool(parsedChannelB));
	CHECK(parsedChannelA.state.chat == parsedChannelB.state.chat);
	CHECK(parsedChannelA.state.chat
		== (PeerToken{ PeerTokenKind::Channel, uint64_t(std::numeric_limits<int64_t>::max()) }));

	// Unrelated User/Chat tokens and invalid tokens or types must be rejected.
	CHECK(ParsePlaylistLocalState(R"({"chat":null})").error == PlaylistLocalError::FieldType);
	CHECK(ParsePlaylistLocalState(R"({"chat":123})").error == PlaylistLocalError::FieldType);
	CHECK(ParsePlaylistLocalState(R"({"chat":{}})").error == PlaylistLocalError::FieldType);

	for (const QByteArray badChat : {
			QByteArray(R"({"chat":""})"),
			QByteArray(R"({"chat":"self:1"})"),
			QByteArray(R"({"chat":"user:4242"})"), // selfId == 0
			QByteArray(R"({"chat":"user:9999"})"),
			QByteArray(R"({"chat":"chat:11"})"),
			QByteArray(R"({"chat":"channel:0"})"),
			QByteArray(R"({"chat":"channel:01"})"),
			QByteArray(R"({"chat":"channel:9223372036854775808"})"),
			QByteArray(R"({"chat":" channel:1"})"),
			QByteArray(R"({"chat":"channel:1 "})"),
		}) {
		const auto res = ParsePlaylistLocalState(badChat, 0);
		CHECK(res.status == PlaylistLocalStatus::Invalid);
		CHECK(res.error == PlaylistLocalError::InvalidChat);
	}
	CHECK(ParsePlaylistLocalState(R"({"chat":"user:4242"})", 9999).error
		== PlaylistLocalError::InvalidChat);

	for (const PeerToken badToken : {
			PeerToken{ PeerTokenKind::Self, 5 },
			PeerToken{ PeerTokenKind::Chat, 11 },
			PeerToken{ PeerTokenKind::User, 9999 },
			PeerToken{ PeerTokenKind::Channel, 0 },
			PeerToken{ PeerTokenKind::Channel, uint64_t(std::numeric_limits<int64_t>::max()) + 1ULL },
		}) {
		PlaylistLocalState s;
		s.chat = badToken;
		CHECK(ValidatePlaylistLocalState(s, selfId).error == PlaylistLocalError::InvalidChat);
		CHECK(!bool(SerializePlaylistLocalState(s, selfId)));
	}
}

void TestMemoValidationStatesBoundsAndCanonicalOrder() {
	auto state = MakeCombinedBookkeepingState();
	const auto written = SerializePlaylistLocalState(state, 4242);
	CHECK(bool(written));

	// Canonical JSON sorts decimal message ID keys lexicographically: "1" < "10" < "2" < "2147483647".
	const auto pos1 = written.json.indexOf(R"("1":{)");
	const auto pos10 = written.json.indexOf(R"("10":{)");
	const auto pos2 = written.json.indexOf(R"("2":{)");
	const auto posMax = written.json.indexOf(R"("2147483647":{)");
	CHECK(pos1 > 0 && pos10 > pos1 && pos2 > pos10 && posMax > pos2);

	const auto canonical = CanonicalizeSyncJson(written.json, std::nullopt);
	CHECK(bool(canonical) && canonical.canonical == written.json);

	const auto reparsed = ParsePlaylistLocalState(written.json, 4242);
	CHECK(bool(reparsed));
	CHECK(reparsed.state.memo == state.memo);
	CHECK(!reparsed.state.memo.at(2).preserved.contains(u"canonical_hash"_q));
	CHECK(!reparsed.state.memo.at(2).preserved.contains(u"classifier"_q));
	CHECK(!reparsed.state.memo.at(2).preserved.contains(u"document_id"_q));
	CHECK(!reparsed.state.memo.at(2).preserved.contains(u"state"_q));
	CHECK(reparsed.state.memo.at(2).preserved.contains(u"future_note"_q));

	// Stale typed keys inside memo[id].preserved must never leak, including canonical_hash on Oversized.
	PlaylistLocalState staleMemoState;
	staleMemoState.chat = PeerToken{};
	staleMemoState.memo.emplace(
		5,
		PlaylistLocalMemoEntry{
			88ULL,
			PlaylistLocalMemoState::Oversized,
			7,
			std::nullopt,
			QJsonObject{
				{ u"canonical_hash"_q, MakeHexHash(u'f') },
				{ u"classifier"_q, 999 },
				{ u"document_id"_q, u"999"_q },
				{ u"state"_q, u"valid"_q },
				{ u"future_tag"_q, u"kept"_q },
			},
		});
	const auto staleWritten = SerializePlaylistLocalState(staleMemoState);
	CHECK(bool(staleWritten));
	CHECK(!staleWritten.json.contains("canonical_hash"));
	const auto staleReparsed = ParsePlaylistLocalState(staleWritten.json);
	CHECK(bool(staleReparsed));
	CHECK(staleReparsed.state.memo.at(5).state == PlaylistLocalMemoState::Oversized);
	CHECK(!staleReparsed.state.memo.at(5).canonicalHash.has_value());
	CHECK(staleReparsed.state.memo.at(5).classifier == 7);
	CHECK(staleReparsed.state.memo.at(5).documentId == 88ULL);
	CHECK(staleReparsed.state.memo.at(5).preserved
		== (QJsonObject{{ u"future_tag"_q, u"kept"_q }}));

	// Forbidden transient/unavailable/vanished/unknown states & invalid entries (alongside valid entry).
	const auto validPart = QByteArray(
		R"("1":{"canonical_hash":")")
		+ MakeHexHash(u'b').toUtf8()
		+ QByteArray(R"(","classifier":1,"document_id":"10","state":"valid"})");

	const auto makeWithSecond = [&](const QByteArray &entryJson) {
		return QByteArray(R"({"chat":"self","memo":{)")
			+ validPart
			+ QByteArray(",")
			+ entryJson
			+ QByteArray("}}");
	};

	for (const QByteArray badEntry : {
			QByteArray(R"("0":{"classifier":1,"document_id":"2","state":"invalid"})"),
			QByteArray(R"("-1":{"classifier":1,"document_id":"2","state":"invalid"})"),
			QByteArray(R"("+1":{"classifier":1,"document_id":"2","state":"invalid"})"),
			QByteArray(R"("02":{"classifier":1,"document_id":"2","state":"invalid"})"),
			QByteArray(R"(" 2":{"classifier":1,"document_id":"2","state":"invalid"})"),
			QByteArray(R"("2 ":{"classifier":1,"document_id":"2","state":"invalid"})"),
			QByteArray(R"("2147483648":{"classifier":1,"document_id":"2","state":"invalid"})"),
			QByteArray(R"("2":{"classifier":1,"state":"invalid"})"),
			QByteArray(R"("2":{"classifier":1,"document_id":"2"})"),
			QByteArray(R"("2":{"document_id":"2","state":"invalid"})"),
			QByteArray(R"("2":{"classifier":1,"document_id":2,"state":"invalid"})"),
			QByteArray(R"("2":{"classifier":1,"document_id":"0","state":"invalid"})"),
			QByteArray(R"("2":{"classifier":1,"document_id":"01","state":"invalid"})"),
			QByteArray(R"("2":{"classifier":1,"document_id":"18446744073709551616","state":"invalid"})"),
			QByteArray(R"("2":{"classifier":0,"document_id":"2","state":"invalid"})"),
			QByteArray(R"("2":{"classifier":-1,"document_id":"2","state":"invalid"})"),
			QByteArray(R"("2":{"classifier":2147483648,"document_id":"2","state":"invalid"})"),
			QByteArray(R"("2":{"classifier":"1","document_id":"2","state":"invalid"})"),
			QByteArray(R"("2":{"classifier":1,"document_id":"2","state":"transient"})"),
			QByteArray(R"("2":{"classifier":1,"document_id":"2","state":"unavailable"})"),
			QByteArray(R"("2":{"classifier":1,"document_id":"2","state":"vanished"})"),
			QByteArray(R"("2":{"classifier":1,"document_id":"2","state":"unknown"})"),
			QByteArray(R"("2":{"classifier":1,"document_id":"2","state":"unknown_ownership"})"),
			QByteArray(R"("2":{"classifier":1,"document_id":"2","state":"Valid"})"),
			QByteArray(R"("2":{"classifier":1,"document_id":"2","state":"valid"})"),
			QByteArray(R"("2":{"canonical_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","classifier":1,"document_id":"2","state":"valid"})"),
			QByteArray(R"("2":{"canonical_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","classifier":1,"document_id":"2","state":"valid"})"),
			QByteArray(R"("2":{"canonical_hash":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","classifier":1,"document_id":"2","state":"valid"})"),
			QByteArray(R"("2":{"canonical_hash":"gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg","classifier":1,"document_id":"2","state":"valid"})"),
			QByteArray(R"("2":{"canonical_hash":null,"classifier":1,"document_id":"2","state":"invalid"})"),
			QByteArray(R"("2":{"canonical_hash":"","classifier":1,"document_id":"2","state":"newer_format"})"),
			QByteArray(R"("2":{"canonical_hash":")") + MakeHexHash(u'c').toUtf8()
				+ QByteArray(R"(","classifier":1,"document_id":"2","state":"oversized"})"),
		}) {
		const auto res = ParsePlaylistLocalState(makeWithSecond(badEntry));
		CHECK(res.status == PlaylistLocalStatus::Invalid);
		CHECK(res.state == PlaylistLocalState{});
	}

	// Duplicate message ID key inside memo.
	const auto dupRes = ParsePlaylistLocalState(
		QByteArray(R"({"chat":"self","memo":{)") + validPart + "," + validPart + "}}");
	CHECK(dupRes.error == PlaylistLocalError::InvalidJson);

	// C++ validation failures.
	{
		PlaylistLocalState badCpp = staleMemoState;
		badCpp.memo.emplace(0, badCpp.memo.at(5));
		CHECK(ValidatePlaylistLocalState(badCpp).error == PlaylistLocalError::InvalidMemo);
		CHECK(!bool(SerializePlaylistLocalState(badCpp)));
	}
	{
		PlaylistLocalState badValidHash;
		badValidHash.chat = PeerToken{};
		badValidHash.memo.emplace(
			1,
			PlaylistLocalMemoEntry{
				2ULL,
				PlaylistLocalMemoState::Valid,
				1,
				MakeHexHash(u'A'),
				{},
			});
		CHECK(ValidatePlaylistLocalState(badValidHash).error == PlaylistLocalError::InvalidMemo);
	}
	{
		PlaylistLocalState unexpectedHash;
		unexpectedHash.chat = PeerToken{};
		unexpectedHash.memo.emplace(
			1,
			PlaylistLocalMemoEntry{
				2ULL,
				PlaylistLocalMemoState::Invalid,
				1,
				QString(),
				{},
			});
		CHECK(ValidatePlaylistLocalState(unexpectedHash).error == PlaylistLocalError::InvalidMemo);
	}
}

void TestStatusUnsyncedSinceAndCrossFieldIndependence() {
	const std::vector<std::pair<QByteArray, PlaylistLocalSyncResult>> allResults = {
		{ "idle", PlaylistLocalSyncResult::Idle },
		{ "synced", PlaylistLocalSyncResult::Synced },
		{ "offline", PlaylistLocalSyncResult::Offline },
		{ "wait", PlaylistLocalSyncResult::Wait },
		{ "problem", PlaylistLocalSyncResult::Problem },
		{ "failed", PlaylistLocalSyncResult::Failed },
		{ "oversized", PlaylistLocalSyncResult::Oversized },
		{ "newer_format", PlaylistLocalSyncResult::NewerFormat },
	};

	// Confirm unsynced_since and sync=false survive across EVERY status result unchanged.
	for (const auto &[name, enumVal] : allResults) {
		PlaylistLocalState s = DefaultPlaylistLocalState();
		s.sync = false;
		s.chat = PeerToken{ PeerTokenKind::Channel, 123 };
		s.unsyncedSince = uint64_t(1);
		s.status.result = enumVal;
		s.status.lastStepAtMs = uint64_t(0);
		s.status.preserved.insert(u"future_flag"_q, true);

		const auto wrote = SerializePlaylistLocalState(s);
		CHECK(bool(wrote));
		CHECK(wrote.json.contains(R"("last_step_at_ms":0)"));
		CHECK(wrote.json.contains(QByteArray(R"("result":")") + name + "\""));
		const auto reparsed = ParsePlaylistLocalState(wrote.json);
		CHECK(bool(reparsed));
		CHECK(reparsed.state == s);
		CHECK(reparsed.state.unsyncedSince == std::optional<uint64_t>(1));
		CHECK(!reparsed.state.sync);
	}

	// Omitted last_step_at_ms must stay absent (distinct from 0).
	const auto statusAbsentTs = ParsePlaylistLocalState(R"({"status":{"result":"offline"}})");
	CHECK(bool(statusAbsentTs));
	CHECK(statusAbsentTs.state.status.result == PlaylistLocalSyncResult::Offline);
	CHECK(!statusAbsentTs.state.status.lastStepAtMs.has_value());
	const auto statusAbsentTsWrote = SerializePlaylistLocalState(statusAbsentTs.state);
	CHECK(bool(statusAbsentTsWrote) && !statusAbsentTsWrote.json.contains("last_step_at_ms"));

	// Missing result defaults to idle while preserving last_step_at_ms = 0 and unknown fields.
	const auto defaultIdleStatus = ParsePlaylistLocalState(
		R"({"status":{"future_detail":"keep","last_step_at_ms":0}})");
	CHECK(bool(defaultIdleStatus));
	CHECK(defaultIdleStatus.state.status.result == PlaylistLocalSyncResult::Idle);
	CHECK(defaultIdleStatus.state.status.lastStepAtMs == std::optional<uint64_t>(0));
	CHECK(defaultIdleStatus.state.status.preserved
		== (QJsonObject{{ u"future_detail"_q, u"keep"_q }}));

	// Max stamp boundaries for last_step_at_ms and unsynced_since.
	const auto maxBounds = ParsePlaylistLocalState(
		R"({"status":{"last_step_at_ms":4503599627370496,"result":"synced"},"unsynced_since":4503599627370496})");
	CHECK(bool(maxBounds));
	CHECK(maxBounds.state.status.lastStepAtMs == std::optional<uint64_t>(kMaximumPlaylistStamp));
	CHECK(maxBounds.state.unsyncedSince == std::optional<uint64_t>(kMaximumPlaylistStamp));

	// Stale typed keys inside status.preserved are overridden by typed fields.
	PlaylistLocalState staleStatus;
	staleStatus.status.result = PlaylistLocalSyncResult::Synced;
	staleStatus.status.lastStepAtMs = uint64_t(42);
	staleStatus.status.preserved.insert(u"result"_q, u"failed"_q);
	staleStatus.status.preserved.insert(u"last_step_at_ms"_q, 999);
	staleStatus.status.preserved.insert(u"future_hint"_q, u"x"_q);
	const auto staleStatusWrote = SerializePlaylistLocalState(staleStatus);
	CHECK(bool(staleStatusWrote));
	const auto staleStatusParsed = ParsePlaylistLocalState(staleStatusWrote.json);
	CHECK(bool(staleStatusParsed));
	CHECK(staleStatusParsed.state.status.result == PlaylistLocalSyncResult::Synced);
	CHECK(staleStatusParsed.state.status.lastStepAtMs == std::optional<uint64_t>(42));
	CHECK(staleStatusParsed.state.status.preserved
		== (QJsonObject{{ u"future_hint"_q, u"x"_q }}));

	// Invalid status and unsynced_since values must never downgrade to idle or clear.
	for (const QByteArray bad : {
			QByteArray(R"({"status":"idle"})"),
			QByteArray(R"({"status":{"result":1}})"),
			QByteArray(R"({"status":{"result":""}})"),
			QByteArray(R"({"status":{"result":"unknown"}})"),
			QByteArray(R"({"status":{"result":"waiting"}})"),
			QByteArray(R"({"status":{"last_step_at_ms":-1,"result":"idle"}})"),
			QByteArray(R"({"status":{"last_step_at_ms":4503599627370497,"result":"idle"}})"),
			QByteArray(R"({"status":{"last_step_at_ms":"0","result":"idle"}})"),
			QByteArray(R"({"unsynced_since":0})"),
			QByteArray(R"({"unsynced_since":-1})"),
			QByteArray(R"({"unsynced_since":4503599627370497})"),
			QByteArray(R"({"unsynced_since":"1"})"),
			QByteArray(R"({"unsynced_since":null})"),
		}) {
		const auto res = ParsePlaylistLocalState(bad);
		CHECK(res.status == PlaylistLocalStatus::Invalid);
		CHECK(res.state == PlaylistLocalState{});
	}

	// Nesting depth inside status.preserved (root=0 -> status=1 -> k=2): 128 valid, 129 rejected.
	QJsonValue exact = QJsonArray();
	for (auto i = 2; i < 128; ++i) {
		exact = QJsonArray{ exact };
	}
	PlaylistLocalState statusDepthOk = DefaultPlaylistLocalState();
	statusDepthOk.status.preserved.insert(u"k"_q, exact);
	const auto statusDepthWrote = SerializePlaylistLocalState(statusDepthOk);
	CHECK(bool(statusDepthWrote));
	CHECK(bool(ParsePlaylistLocalState(statusDepthWrote.json)));

	PlaylistLocalState statusDepthTooDeep = DefaultPlaylistLocalState();
	statusDepthTooDeep.status.preserved.insert(u"k"_q, QJsonArray{ exact });
	CHECK(ValidatePlaylistLocalState(statusDepthTooDeep).error
		== PlaylistLocalError::InvalidPreservedJson);
}

void TestImmutabilityAndOversizedCombined() {
	constexpr auto selfId = uint64_t(4242);
	auto state = MakeCombinedBookkeepingState();

	Playlist bigHidden;
	bigHidden.title = PlaylistTitleRegister{ u"Huge Bookkeeping Keep"_q, 100 };
	bigHidden.deleted = PlaylistDeletedRegister{ false, 200 };
	auto songs = std::map<uint64_t, PlaylistSongRegister>();
	constexpr auto kSongCount = uint64_t(24000);
	const auto titlePrefix = QString(70, u'T');
	const auto perf = QString(80, u'P');
	for (uint64_t i = 1; i <= kSongCount; ++i) {
		songs.emplace(
			i,
			PlaylistSongRegister{
				PlaylistSongValue{ PlaylistSource{
					.peer = PeerToken{ PeerTokenKind::User, selfId },
					.msg = int32_t(i),
					.username = QString(),
					.alt = {},
					.meta = PlaylistSongMeta{ titlePrefix + QString::number(i), perf, 200 },
					.since = 1000 + i,
				} },
				2000 + i,
			});
	}
	bigHidden.songs = std::move(songs);
	state.hidden.at(u"pl-aaaaaaaaaaaaaaaa"_q).playlist = std::move(bigHidden);

	const auto stateSnapshot = state;
	const auto wrote = SerializePlaylistLocalState(state, selfId);
	CHECK(state == stateSnapshot);
	CHECK(bool(wrote));
	CHECK(wrote.json.size() > 4 * 1024 * 1024);

	// Legacy 1-arg 4 MiB cap remains strictly enforced.
	const auto legacyCap = CanonicalizeSyncJson(wrote.json);
	CHECK(!legacyCap && legacyCap.error == SyncJsonErrorKind::SizeLimit);

	// Uncapped local read preserves all bookkeeping + oversized hidden snapshot idempotently.
	const auto jsonCopy = wrote.json;
	const auto parsed = ParsePlaylistLocalState(jsonCopy, selfId);
	CHECK(jsonCopy == wrote.json);
	CHECK(bool(parsed));
	auto expectedParsed = state;
	for (auto &entry : expectedParsed.hidden.at(u"pl-aaaaaaaaaaaaaaaa"_q).playlist.songs.value()) {
		entry.second.value.source->peer = PeerToken{ PeerTokenKind::Self };
	}
	CHECK(parsed.state == expectedParsed);
	const auto rewritten = SerializePlaylistLocalState(parsed.state, selfId);
	CHECK(bool(rewritten) && rewritten.json == wrote.json);
	CHECK(ParsePlaylistLocalState(rewritten.json, selfId).state == parsed.state);
}

} // namespace

int main() {
	TestDefaultEmptyFullAndOldChatsOpaqueness();
	TestChatBindingAndNormalization();
	TestMemoValidationStatesBoundsAndCanonicalOrder();
	TestStatusUnsyncedSinceAndCrossFieldIndependence();
	TestImmutabilityAndOversizedCombined();

	std::printf("Checks: %d, Failures: %d\n", Checks, Failures);
	return Failures == 0 ? 0 : 1;
}
