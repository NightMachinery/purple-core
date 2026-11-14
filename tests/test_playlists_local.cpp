/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists.h"
#include "purple/purple_playlists_local.h"
#include "purple/purple_playlists_ops.h"
#include "purple/purple_sync_json.h"
#include "purple/purple_types.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <cmath>
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

QByteArray MakeEntropy(uint8_t fill) {
	return QByteArray(int(kPlaylistIdEntropyBytes), char(fill));
}

PlaylistSource MakeSource(
		PeerToken peer,
		int32_t msg,
		uint64_t since,
		const QString &title = u"Track"_q,
		const QString &performer = u"Artist"_q,
		int32_t duration = 180,
		const QString &username = QString(),
		std::vector<PlaylistSourceRef> alt = {}) {
	return PlaylistSource{
		.peer = peer,
		.msg = msg,
		.username = username,
		.alt = std::move(alt),
		.meta = PlaylistSongMeta{ title, performer, duration },
		.since = since,
	};
}

Playlist MakeVisibleSnapshot(uint64_t selfId) {
	Playlist pl;
	pl.title = PlaylistTitleRegister{ u"Night Drive"_q, 100 };
	pl.deleted = PlaylistDeletedRegister{ false, 150 };
	pl.order = PlaylistOrderRegister{ { 10, 2 }, 250 };
	pl.songs = std::map<uint64_t, PlaylistSongRegister>{
		{
			2,
			PlaylistSongRegister{
				PlaylistSongValue{ MakeSource(
					PeerToken{ PeerTokenKind::User, selfId },
					55,
					200,
					u"Two"_q,
					u"Perf B"_q,
					210,
					u"user_main"_q,
					{
						PlaylistSourceRef{ PeerToken{ PeerTokenKind::Chat, 12 }, 11, u"alt_one"_q },
						PlaylistSourceRef{ PeerToken{ PeerTokenKind::Channel, 34 }, 22, QString() },
						PlaylistSourceRef{ PeerToken{ PeerTokenKind::User, selfId }, 33, u"alt_self"_q },
					}) },
				300,
			},
		},
		{
			10,
			PlaylistSongRegister{
				PlaylistSongValue{ MakeSource(
					PeerToken{ PeerTokenKind::Channel, 9001 },
					77,
					180,
					u"Ten"_q,
					u"Perf A"_q,
					145) },
				260,
			},
		},
		{
			99,
			PlaylistSongRegister{
				PlaylistSongValue{ std::nullopt },
				400,
			},
		},
	};
	return pl;
}

void TestDefaultsEmptyObjectAndCorruptInputs() {
	const auto def = DefaultPlaylistLocalState();
	CHECK(def.format == 1);
	CHECK(def.sync);
	CHECK(def.dueSeen.empty());
	CHECK(def.recentUse.empty());
	CHECK(def.hidden.empty());
	CHECK(def.preserved.isEmpty());
	CHECK(bool(ValidatePlaylistLocalState(def)));

	const auto written = SerializePlaylistLocalState(def);
	CHECK(bool(written));
	CHECK(written.json == R"({"format":1,"sync":true})");

	const auto parsedDefault = ParsePlaylistLocalState(written.json);
	CHECK(bool(parsedDefault));
	CHECK(parsedDefault.state == def);

	const auto parsedEmptyObj = ParsePlaylistLocalState("{}");
	CHECK(bool(parsedEmptyObj));
	CHECK(parsedEmptyObj.state == def);

	const auto parsedExplicitEmpty = ParsePlaylistLocalState(
		R"({"due_seen":{},"format":1,"hidden":{},"recent_use":{},"sync":true})");
	CHECK(bool(parsedExplicitEmpty));
	CHECK(parsedExplicitEmpty.state == def);

	for (const QByteArray bad : {
			QByteArray(),
			QByteArray("   \n\t "),
			QByteArray("{"),
			QByteArray("[]"),
			QByteArray("null"),
			QByteArray("true"),
			QByteArray("123"),
			QByteArray("\"text\""),
			QByteArray("{\"sync\":true,\"sync\":false}"),
			QByteArray("{\"future_memo\":1.5}"),
			QByteArray("{\"future_memo\":\"\\uD800\"}"),
			QByteArray("{\"future_memo\":\"bad\xFF\"}"),
		}) {
		const auto res = ParsePlaylistLocalState(bad);
		CHECK(res.status == PlaylistLocalStatus::Invalid);
		CHECK(!bool(res));
		CHECK(res.state == PlaylistLocalState{});
	}
}

void TestSyncFalseAndUnknownPreservedSeparation() {
	const QByteArray raw = R"({
		"due_seen": {
			"pl-aaaaaaaaaaaaaaaa": {
				"deletion_stamp": 1000,
				"first_seen_at_ms": 2000,
				"note": {"retry": 3}
			}
		},
		"future_chat": {"cursor": 42, "negative": -9007199254740991, "zero": -0},
		"future_memo": [1, "x", null, true],
		"future_status": "idle",
		"old_chats": {"peers": ["u1"]},
		"recent_use": {"pl-bbbbbbbbbbbbbbbb": 0},
		"sync": false
	})";
	const auto parsed = ParsePlaylistLocalState(raw);
	CHECK(bool(parsed));
	CHECK(!parsed.state.sync);
	CHECK(parsed.state.format == 1);
	CHECK(!parsed.state.preserved.contains(u"format"_q));
	CHECK(!parsed.state.preserved.contains(u"sync"_q));
	CHECK(!parsed.state.preserved.contains(u"due_seen"_q));
	CHECK(!parsed.state.preserved.contains(u"recent_use"_q));
	CHECK(!parsed.state.preserved.contains(u"hidden"_q));
	CHECK(parsed.state.preserved.contains(u"future_chat"_q));
	CHECK(parsed.state.preserved.contains(u"future_memo"_q));
	CHECK(parsed.state.preserved.contains(u"future_status"_q));
	CHECK(parsed.state.preserved.contains(u"old_chats"_q));

	const auto dueId = u"pl-aaaaaaaaaaaaaaaa"_q;
	CHECK(parsed.state.dueSeen.at(dueId).value
		== (PlaylistAutoPurgeDueSeen{ 1000, 2000 }));
	CHECK(!parsed.state.dueSeen.at(dueId).preserved.contains(u"deletion_stamp"_q));
	CHECK(!parsed.state.dueSeen.at(dueId).preserved.contains(u"first_seen_at_ms"_q));
	CHECK(parsed.state.dueSeen.at(dueId).preserved.contains(u"note"_q));
	CHECK(PlaylistLocalDueSeenValues(parsed.state)
		== (std::map<QString, PlaylistAutoPurgeDueSeen>{ { dueId, { 1000, 2000 } } }));
	CHECK(parsed.state.recentUse.at(u"pl-bbbbbbbbbbbbbbbb"_q) == 0);

	// Mutate typed state and ensure unknown keys survive verbatim.
	auto updated = parsed.state;
	updated.recentUse.emplace(dueId, kMaximumPlaylistStamp);
	const auto serialized1 = SerializePlaylistLocalState(updated);
	CHECK(bool(serialized1));
	const auto reparsed1 = ParsePlaylistLocalState(serialized1.json);
	CHECK(bool(reparsed1));
	CHECK(reparsed1.state == updated);
	CHECK(!reparsed1.state.sync);

	// Toggle sync to true and clear maps; unknown keys must still survive,
	// while empty maps disappear cleanly without leaving stale copies.
	updated.sync = true;
	updated.dueSeen.clear();
	updated.recentUse.clear();
	// Inject stale copies of known keys in preserved; typed fields must override.
	updated.preserved.insert(u"sync"_q, false);
	updated.preserved.insert(u"format"_q, 77);
	updated.preserved.insert(u"due_seen"_q, QJsonObject{{ u"stale"_q, 1 }});
	updated.preserved.insert(u"recent_use"_q, QJsonObject{{ u"stale"_q, 2 }});
	updated.preserved.insert(u"hidden"_q, QJsonObject{{ u"stale"_q, 3 }});

	const auto serialized2 = SerializePlaylistLocalState(updated);
	CHECK(bool(serialized2));
	const auto canonical2 = CanonicalizeSyncJson(serialized2.json, std::nullopt);
	CHECK(bool(canonical2) && canonical2.canonical == serialized2.json);
	CHECK(!serialized2.json.contains("\"due_seen\""));
	CHECK(!serialized2.json.contains("\"recent_use\""));
	CHECK(!serialized2.json.contains("\"hidden\""));

	const auto reparsed2 = ParsePlaylistLocalState(serialized2.json);
	CHECK(bool(reparsed2));
	CHECK(reparsed2.state.sync);
	CHECK(reparsed2.state.format == 1);
	CHECK(reparsed2.state.dueSeen.empty());
	CHECK(reparsed2.state.recentUse.empty());
	CHECK(reparsed2.state.hidden.empty());
	CHECK(reparsed2.state.preserved == parsed.state.preserved);

	// Entry-level preserved overrides for due_seen and hidden.
	auto staleDue = PlaylistLocalDueSeenEntry{
		PlaylistAutoPurgeDueSeen{ 111, 222 },
		QJsonObject{
			{ u"deletion_stamp"_q, 999 },
			{ u"first_seen_at_ms"_q, 888 },
			{ u"extra"_q, u"kept"_q },
		},
	};
	PlaylistLocalState dueState;
	dueState.sync = false;
	dueState.dueSeen.emplace(dueId, std::move(staleDue));
	const auto dueWritten = SerializePlaylistLocalState(dueState);
	CHECK(bool(dueWritten));
	const auto dueReparsed = ParsePlaylistLocalState(dueWritten.json);
	CHECK(bool(dueReparsed));
	CHECK(dueReparsed.state.dueSeen.at(dueId).value
		== (PlaylistAutoPurgeDueSeen{ 111, 222 }));
	CHECK(dueReparsed.state.dueSeen.at(dueId).preserved
		== (QJsonObject{{ u"extra"_q, u"kept"_q }}));

	// Verify UTF-16 code-unit sorting in preserved objects (U+D83D.. < U+FFFD).
	PlaylistLocalState utf16State = DefaultPlaylistLocalState();
	utf16State.preserved.insert(QString(QChar(0xFFFD)), 1);
	utf16State.preserved.insert(u"😀"_q, 2);
	const auto utf16Written = SerializePlaylistLocalState(utf16State);
	CHECK(bool(utf16Written));
	const auto utf16Canonical = CanonicalizeSyncJson(utf16Written.json, std::nullopt);
	CHECK(bool(utf16Canonical) && utf16Canonical.canonical == utf16Written.json);
	CHECK(utf16Written.json.indexOf("😀") < utf16Written.json.indexOf("\xEF\xBF\xBD"));

	const auto badSync = ParsePlaylistLocalState(R"({"sync":"false"})");
	CHECK(badSync.status == PlaylistLocalStatus::Invalid);
	CHECK(badSync.error == PlaylistLocalError::FieldType);
}

void TestDueSeenRecentUseAndHiddenSnapshots() {
	constexpr auto selfId = uint64_t(4242);
	const auto idA = u"pl-aaaaaaaaaaaaaaaa"_q;
	const auto idB = u"pl-cccccccccccccccc"_q;

	// Complete hidden snapshot + partial-but-visible snapshot (absent order & songs).
	Playlist partial;
	partial.title = PlaylistTitleRegister{ u"Partial Visible"_q, 777 };
	partial.deleted = PlaylistDeletedRegister{ false, 888 };

	PlaylistLocalState state;
	state.sync = false;
	state.dueSeen.emplace(
		idA,
		PlaylistLocalDueSeenEntry{
			PlaylistAutoPurgeDueSeen{ kMinimumPlaylistStamp, kMaximumPlaylistStamp },
			QJsonObject{{ u"source"_q, u"memo"_q }},
		});
	state.recentUse.emplace(idA, 0);
	state.recentUse.emplace(idB, kMaximumPlaylistStamp);
	state.hidden.emplace(
		idA,
		PlaylistLocalHiddenEntry{
			PlaylistNoticeReason::PurgedElsewhere,
			MakeVisibleSnapshot(selfId),
			QJsonObject{
				{ u"reason"_q, u"deleted"_q },
				{ u"playlist"_q, QJsonObject{{ u"stale"_q, true }} },
				{ u"badge"_q, QJsonArray{ 1, 2 } },
			},
		});
	state.hidden.emplace(
		idB,
		PlaylistLocalHiddenEntry{
			PlaylistNoticeReason::DeletedElsewhere,
			partial,
			{},
		});

	const auto written = SerializePlaylistLocalState(state, selfId);
	CHECK(bool(written));
	// Self user ID must normalize to "self" cleanly and never leak raw peer token.
	CHECK(!written.json.contains("user:4242"));
	CHECK(written.json.contains("\"peer\":\"self\""));
	CHECK(written.json.contains("\"reason\":\"purged\""));
	CHECK(written.json.contains("\"reason\":\"deleted\""));
	// Lexicographical song order "10" precedes "2".
	const auto song10Pos = written.json.indexOf("\"10\":[");
	const auto song2Pos = written.json.indexOf("\"2\":[");
	CHECK(song10Pos > 0 && song2Pos > song10Pos);

	const auto canonical = CanonicalizeSyncJson(written.json, std::nullopt);
	CHECK(bool(canonical) && canonical.canonical == written.json);

	const auto reparsed = ParsePlaylistLocalState(written.json, selfId);
	CHECK(bool(reparsed));
	CHECK(reparsed.state.hidden.at(idA).reason == PlaylistNoticeReason::PurgedElsewhere);
	CHECK(!reparsed.state.hidden.at(idA).preserved.contains(u"reason"_q));
	CHECK(!reparsed.state.hidden.at(idA).preserved.contains(u"playlist"_q));
	CHECK(reparsed.state.hidden.at(idA).preserved
		== (QJsonObject{{ u"badge"_q, QJsonArray{ 1, 2 } }}));
	const auto &snapA = reparsed.state.hidden.at(idA).playlist;
	CHECK(snapA.title == (PlaylistTitleRegister{ u"Night Drive"_q, 100 }));
	CHECK(snapA.deleted == (PlaylistDeletedRegister{ false, 150 }));
	CHECK(snapA.order == (PlaylistOrderRegister{ { 10, 2 }, 250 }));
	CHECK(snapA.songs->at(2).value.source->peer == PeerToken{});
	CHECK(snapA.songs->at(2).value.source->alt.size() == 3);
	CHECK(snapA.songs->at(2).value.source->alt[2].peer == PeerToken{});
	CHECK(snapA.songs->at(2).stamp == 300);
	CHECK(!snapA.songs->at(99).value.source.has_value());

	const auto &snapB = reparsed.state.hidden.at(idB).playlist;
	CHECK(snapB.title == (PlaylistTitleRegister{ u"Partial Visible"_q, 777 }));
	CHECK(snapB.deleted == (PlaylistDeletedRegister{ false, 888 }));
	CHECK(!snapB.order.has_value());
	CHECK(!snapB.songs.has_value());

	// Canonical idempotence on normalized parsed value.
	const auto rewritten = SerializePlaylistLocalState(reparsed.state, selfId);
	CHECK(bool(rewritten) && rewritten.json == written.json);

	// Detached CopyPlaylist compatibility after purge!
	auto created = CreatePlaylist(
		PlaylistLibrary{},
		u"Night Drive"_q,
		MakeEntropy(0x03),
		100,
		selfId);
	CHECK(created);
	auto purgedLib = PlaylistLibrary();
	purgedLib.purged.insert(idA);
	const auto copied = CopyPlaylist(
		purgedLib,
		reparsed.state.hidden.at(idA).playlist,
		MakeEntropy(0x04),
		900,
		selfId);
	CHECK(bool(copied));
	CHECK(copied.library.purged.contains(idA));
	CHECK(copied.addedCount == 2);
	const auto &copiedPl = copied.library.playlists.at(copied.playlistId);
	CHECK(copiedPl.title == (PlaylistTitleRegister{ u"Night Drive (copy)"_q, 900 }));
	CHECK(PlaylistVisibleSongIds(copiedPl) == (std::vector<uint64_t>{ 10, 2 }));
	CHECK(copiedPl.songs->at(2).value.source->alt.size() == 3);
}

void TestValidationBoundariesAndErrors() {
	// Representative exact error enum checks alongside exhaustive boundary coverage.
	CHECK(ParsePlaylistLocalState("{").error == PlaylistLocalError::InvalidJson);
	CHECK(ParsePlaylistLocalState("[]").error == PlaylistLocalError::FieldType);
	CHECK(ParsePlaylistLocalState(R"({"due_seen":{"bad-id":{"deletion_stamp":1,"first_seen_at_ms":1}}})").error
		== PlaylistLocalError::InvalidId);
	CHECK(ParsePlaylistLocalState(R"({"due_seen":{"pl-aaaaaaaaaaaaaaaa":{"deletion_stamp":1}}})").error
		== PlaylistLocalError::MissingField);
	CHECK(ParsePlaylistLocalState(R"({"due_seen":{"pl-aaaaaaaaaaaaaaaa":{"deletion_stamp":0,"first_seen_at_ms":1}}})").error
		== PlaylistLocalError::InvalidStamp);
	CHECK(ParsePlaylistLocalState(R"({"recent_use":{"pl-aaaaaaaaaaaaaaaa":4503599627370497}})").error
		== PlaylistLocalError::InvalidRecentUse);
	CHECK(ParsePlaylistLocalState(R"({"hidden":{"pl-aaaaaaaaaaaaaaaa":{"playlist":{"deleted":[false,1],"title":["Ok",1]},"reason":"moved"}}})").error
		== PlaylistLocalError::InvalidReason);
	CHECK(ParsePlaylistLocalState(R"({"hidden":{"pl-aaaaaaaaaaaaaaaa":{"playlist":{"deleted":[true,1],"title":["AlreadyDeleted",1]},"reason":"deleted"}}})").error
		== PlaylistLocalError::InvalidSnapshot);
	{
		const auto badPl = ParsePlaylistLocalState(
			R"({"hidden":{"pl-aaaaaaaaaaaaaaaa":{"playlist":{"deleted":[false,1],"extra":1,"title":["Ok",1]},"reason":"purged"}}})");
		CHECK(badPl.error == PlaylistLocalError::InvalidPlaylist);
		CHECK(badPl.playlistError == PlaylistLibraryError::UnknownKey);

		const auto badPlTitle = ParsePlaylistLocalState(
			R"({"hidden":{"pl-aaaaaaaaaaaaaaaa":{"playlist":{"deleted":[false,1],"title":["  Trimmed  ",1]},"reason":"deleted"}}})");
		CHECK(badPlTitle.error == PlaylistLocalError::InvalidPlaylist);
		CHECK(badPlTitle.playlistError == PlaylistLibraryError::InvalidTitle);
	}

	// Invalid due_seen cases.
	for (const QByteArray bad : {
			QByteArray(R"({"due_seen":{"bad-id":{"deletion_stamp":1,"first_seen_at_ms":1}}})"),
			QByteArray(R"({"due_seen":{"pl-aaaaaaaaaaaaaaaa":{"deletion_stamp":0,"first_seen_at_ms":1}}})"),
			QByteArray(R"({"due_seen":{"pl-aaaaaaaaaaaaaaaa":{"deletion_stamp":1,"first_seen_at_ms":0}}})"),
			QByteArray(R"({"due_seen":{"pl-aaaaaaaaaaaaaaaa":{"deletion_stamp":4503599627370497,"first_seen_at_ms":1}}})"),
			QByteArray(R"({"due_seen":{"pl-aaaaaaaaaaaaaaaa":{"deletion_stamp":1}}})"),
			QByteArray(R"({"due_seen":{"pl-aaaaaaaaaaaaaaaa":{"deletion_stamp":"1","first_seen_at_ms":1}}})"),
			QByteArray(R"({"due_seen":[]})"),
		}) {
		CHECK(ParsePlaylistLocalState(bad).status == PlaylistLocalStatus::Invalid);
	}

	// Invalid recent_use cases.
	for (const QByteArray bad : {
			QByteArray(R"({"recent_use":{"not-pl":0}})"),
			QByteArray(R"({"recent_use":{"pl-aaaaaaaaaaaaaaaa":-1}})"),
			QByteArray(R"({"recent_use":{"pl-aaaaaaaaaaaaaaaa":4503599627370497}})"),
			QByteArray(R"({"recent_use":{"pl-aaaaaaaaaaaaaaaa":"5"}})"),
			QByteArray(R"({"recent_use":123})"),
		}) {
		CHECK(ParsePlaylistLocalState(bad).status == PlaylistLocalStatus::Invalid);
	}

	// Invalid hidden cases (reason, missing title/deleted, deleted=true, unknown key inside playlist, bad meta).
	for (const QByteArray bad : {
			QByteArray(R"({"hidden":{"bad":{"playlist":{"deleted":[false,1],"title":["Ok",1]},"reason":"deleted"}}})"),
			QByteArray(R"({"hidden":{"pl-aaaaaaaaaaaaaaaa":{"reason":"deleted"}}})"),
			QByteArray(R"({"hidden":{"pl-aaaaaaaaaaaaaaaa":{"playlist":{"deleted":[false,1],"title":["Ok",1]}}}})") ,
			QByteArray(R"({"hidden":{"pl-aaaaaaaaaaaaaaaa":{"playlist":{"deleted":[false,1],"title":["Ok",1]},"reason":"moved"}}})"),
			QByteArray(R"({"hidden":{"pl-aaaaaaaaaaaaaaaa":{"playlist":{"deleted":[false,1],"title":["Ok",1]},"reason":1}}})"),
			QByteArray(R"({"hidden":{"pl-aaaaaaaaaaaaaaaa":{"playlist":{"title":["MissingDeleted",1]},"reason":"deleted"}}})"),
			QByteArray(R"({"hidden":{"pl-aaaaaaaaaaaaaaaa":{"playlist":{"deleted":[false,1]},"reason":"deleted"}}})"),
			QByteArray(R"({"hidden":{"pl-aaaaaaaaaaaaaaaa":{"playlist":{"deleted":[true,1],"title":["AlreadyDeleted",1]},"reason":"deleted"}}})"),
			QByteArray(R"({"hidden":{"pl-aaaaaaaaaaaaaaaa":{"playlist":{"deleted":[false,1],"extra":1,"title":["Ok",1]},"reason":"purged"}}})"),
			QByteArray(R"({"hidden":{"pl-aaaaaaaaaaaaaaaa":{"playlist":{"deleted":[false,0],"title":["ZeroStamp",1]},"reason":"purged"}}})"),
		}) {
		const auto parsed = ParsePlaylistLocalState(bad);
		CHECK(parsed.status == PlaylistLocalStatus::Invalid);
		CHECK(parsed.state == PlaylistLocalState{});
	}

	// Refuse recognized memo/chat/status fields carrying malformed or old arbitrary shapes.
	for (const QByteArray bad : {
			QByteArray(R"({"chat":{"cursor":42}})"),
			QByteArray(R"({"memo":[1,"x",null,true]})"),
			QByteArray(R"({"status":"idle"})"),
			QByteArray(R"({"chat":"self","memo":{"1":{"classifier":1,"document_id":"2","state":"unknown"}}})"),
			QByteArray(R"({"memo":{"1":{"classifier":1,"document_id":"2","state":"oversized"}}})"),
			QByteArray(R"({"sync":false,"unsynced_since":0})"),
		}) {
		const auto parsed = ParsePlaylistLocalState(bad);
		CHECK(parsed.status == PlaylistLocalStatus::Invalid);
		CHECK(parsed.state == PlaylistLocalState{});
	}

	// C++ invalid preserved JSON values and selfUserId validation.
	{
		PlaylistLocalState badFloat = DefaultPlaylistLocalState();
		badFloat.preserved.insert(u"future_memo"_q, 1.25);
		CHECK(ValidatePlaylistLocalState(badFloat).error == PlaylistLocalError::InvalidPreservedJson);
		const auto badWrite = SerializePlaylistLocalState(badFloat);
		CHECK(!bool(badWrite) && badWrite.json.isEmpty());
	}
	{
		PlaylistLocalState badInf = DefaultPlaylistLocalState();
		badInf.preserved.insert(u"future_memo"_q, std::numeric_limits<double>::infinity());
		CHECK(ValidatePlaylistLocalState(badInf).error == PlaylistLocalError::InvalidPreservedJson);
	}
	{
		PlaylistLocalState badSurrogate = DefaultPlaylistLocalState();
		badSurrogate.preserved.insert(u"future_memo"_q, QString(QChar(0xD800)));
		CHECK(ValidatePlaylistLocalState(badSurrogate).error == PlaylistLocalError::InvalidPreservedJson);
	}
	{
		// Nesting depth boundary: depth 128 valid, depth 129 rejected.
		// Root object = 0; value under root key = 1; wrapping 127 nested arrays makes innermost [] at depth 128.
		constexpr auto kMaximumJsonDepth = 128;
		QJsonValue exact = QJsonArray();
		for (auto i = 1; i < kMaximumJsonDepth; ++i) {
			exact = QJsonArray{ exact };
		}
		PlaylistLocalState depthOk = DefaultPlaylistLocalState();
		depthOk.preserved.insert(u"future_memo"_q, exact);
		const auto wroteOk = SerializePlaylistLocalState(depthOk);
		CHECK(bool(wroteOk));
		CHECK(bool(CanonicalizeSyncJson(wroteOk.json, std::nullopt)));
		CHECK(bool(ParsePlaylistLocalState(wroteOk.json)));

		PlaylistLocalState depthTooDeep = DefaultPlaylistLocalState();
		depthTooDeep.preserved.insert(u"future_memo"_q, QJsonArray{ exact });
		CHECK(ValidatePlaylistLocalState(depthTooDeep).error == PlaylistLocalError::InvalidPreservedJson);
		CHECK(!bool(SerializePlaylistLocalState(depthTooDeep)));
	}
	{
		const auto badSelf = uint64_t(std::numeric_limits<int64_t>::max()) + 1ULL;
		CHECK(ValidatePlaylistLocalState(DefaultPlaylistLocalState(), badSelf).error
			== PlaylistLocalError::InvalidSelfUserId);
		CHECK(ParsePlaylistLocalState("{}", badSelf).error
			== PlaylistLocalError::InvalidSelfUserId);
	}
}

void TestNewerFormatHandling() {
	const auto parsedNewer = ParsePlaylistLocalState(
		R"({"format":2,"future":{"deep":[1,2]},"sync":"ignored"})");
	CHECK(parsedNewer.status == PlaylistLocalStatus::NewerFormat);
	CHECK(parsedNewer.error == PlaylistLocalError::None);
	CHECK(!bool(parsedNewer));
	CHECK(parsedNewer.state == PlaylistLocalState{});

	PlaylistLocalState newerState;
	newerState.format = 2;
	CHECK(ValidatePlaylistLocalState(newerState).status == PlaylistLocalStatus::NewerFormat);
	const auto newerWrite = SerializePlaylistLocalState(newerState);
	CHECK(newerWrite.status == PlaylistLocalStatus::NewerFormat);
	CHECK(newerWrite.json.isEmpty());

	for (const QByteArray badFmt : {
			QByteArray(R"({"format":0})"),
			QByteArray(R"({"format":-1})"),
			QByteArray(R"({"format":"1"})"),
			QByteArray(R"({"format":true})"),
			QByteArray(R"({"format":1.5})"),
		}) {
		const auto res = ParsePlaylistLocalState(badFmt);
		CHECK(res.status == PlaylistLocalStatus::Invalid);
	}

	for (const int badInt : { 0, -5 }) {
		PlaylistLocalState bad;
		bad.format = badInt;
		CHECK(ValidatePlaylistLocalState(bad).error == PlaylistLocalError::InvalidFormat);
		CHECK(SerializePlaylistLocalState(bad).error == PlaylistLocalError::InvalidFormat);
	}
}

void TestOversizedHiddenSnapshotAndCanonicalBudget() {
	constexpr auto selfId = uint64_t(777);
	Playlist bigPlaylist;
	bigPlaylist.title = PlaylistTitleRegister{ u"Huge Snapshot"_q, 1000 };
	bigPlaylist.deleted = PlaylistDeletedRegister{ false, 1100 };
	auto songs = std::map<uint64_t, PlaylistSongRegister>();
	auto order = std::vector<uint64_t>();
	// 24000 entries cleanly exceed 4 MiB (~7.5 MiB) without allocating excessive test time.
	constexpr auto kSongCount = uint64_t(24000);
	order.reserve(kSongCount);
	const auto fillerTitle = QString(70, u'T');
	const auto fillerPerf = QString(80, u'P');
	for (uint64_t i = 1; i <= kSongCount; ++i) {
		order.push_back(i);
		songs.emplace(
			i,
			PlaylistSongRegister{
				PlaylistSongValue{ MakeSource(
					(i % 2 == 0)
						? PeerToken{ PeerTokenKind::User, selfId }
						: PeerToken{ PeerTokenKind::Channel, 500 + i },
					int32_t(i),
					1000 + i,
					fillerTitle + QString::number(i),
					fillerPerf,
					200) },
				2000 + i,
			});
	}
	bigPlaylist.order = PlaylistOrderRegister{ std::move(order), 3000 };
	bigPlaylist.songs = std::move(songs);

	PlaylistLocalState state;
	state.sync = false;
	state.dueSeen.emplace(
		u"pl-aaaaaaaaaaaaaaaa"_q,
		PlaylistLocalDueSeenEntry{
			PlaylistAutoPurgeDueSeen{ 1100, 50000 },
			QJsonObject{{ u"tag"_q, 1 }},
		});
	state.recentUse.emplace(u"pl-aaaaaaaaaaaaaaaa"_q, 999999);
	state.hidden.emplace(
		u"pl-aaaaaaaaaaaaaaaa"_q,
		PlaylistLocalHiddenEntry{
			PlaylistNoticeReason::PurgedElsewhere,
			std::move(bigPlaylist),
			QJsonObject{{ u"hint"_q, u"kept"_q }},
		});
	state.preserved.insert(u"future_memo"_q, QJsonObject{{ u"v"_q, 1 }});

	const auto serialized = SerializePlaylistLocalState(state, selfId);
	CHECK(bool(serialized));
	CHECK(serialized.json.size() > 4 * 1024 * 1024);

	// 1-arg CanonicalizeSyncJson must keep its 4 MiB legacy settings guard intact.
	const auto legacyCapReject = CanonicalizeSyncJson(serialized.json);
	CHECK(!legacyCapReject);
	CHECK(legacyCapReject.error == SyncJsonErrorKind::SizeLimit);

	// Uncapped ParsePlaylistLocalState must succeed cleanly, and an explicit 4 MiB
	// maximumBytes bound must reject cleanly with SizeLimit.
	const auto explicitCapReject = ParsePlaylistLocalState(
		serialized.json,
		selfId,
		qsizetype(4 * 1024 * 1024));
	CHECK(explicitCapReject.status == PlaylistLocalStatus::Invalid);
	CHECK(explicitCapReject.error == PlaylistLocalError::SizeLimit);

	const auto parsed = ParsePlaylistLocalState(serialized.json, selfId);
	CHECK(bool(parsed));
	CHECK(!parsed.state.sync);
	CHECK(parsed.state.hidden.at(u"pl-aaaaaaaaaaaaaaaa"_q).playlist.songs->size()
		== kSongCount);
	CHECK(parsed.state.preserved == (QJsonObject{{ u"future_memo"_q, QJsonObject{{ u"v"_q, 1 }} }}));

	const auto reserialized = SerializePlaylistLocalState(parsed.state, selfId);
	CHECK(bool(reserialized));
	CHECK(reserialized.json == serialized.json);
}

} // namespace

int main() {
	TestDefaultsEmptyObjectAndCorruptInputs();
	TestSyncFalseAndUnknownPreservedSeparation();
	TestDueSeenRecentUseAndHiddenSnapshots();
	TestValidationBoundariesAndErrors();
	TestNewerFormatHandling();
	TestOversizedHiddenSnapshotAndCanonicalBudget();

	std::printf("Checks: %d, Failures: %d\n", Checks, Failures);
	return Failures == 0 ? 0 : 1;
}
