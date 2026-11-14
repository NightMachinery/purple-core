/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists.h"
#include "purple/purple_sync_json.h"
#include "purple/purple_types.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <random>
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

#define CHECK(condition) Check(bool(condition), #condition, __LINE__)

PlaylistSource MakeSource(
		PeerToken peer,
		int32_t msg,
		uint64_t since,
		QString title = u"Song"_q,
		QString perf = u"Artist"_q,
		int32_t dur = 245,
		QString un = QString(),
		std::vector<PlaylistSourceRef> alt = {}) {
	auto source = PlaylistSource();
	source.peer = peer;
	source.msg = msg;
	source.username = std::move(un);
	source.alt = std::move(alt);
	source.meta = PlaylistSongMeta{ std::move(title), std::move(perf), dur };
	source.since = since;
	return source;
}

PlaylistSongRegister ActiveSong(const PlaylistSource &source, uint64_t stamp) {
	return { PlaylistSongValue{ source }, stamp };
}

PlaylistSongRegister RemovedSong(uint64_t stamp) {
	return { PlaylistSongValue{ std::nullopt }, stamp };
}

void TestSyncJsonBudgetExtension() {
	const auto overFourMib = QByteArray(4 * 1024 * 1024 + 1, ' ');
	const auto legacyLimit = CanonicalizeSyncJson(overFourMib);
	CHECK(!legacyLimit);
	CHECK(legacyLimit.error == SyncJsonErrorKind::SizeLimit);
	CHECK(legacyLimit.offset == 4 * 1024 * 1024);

	const auto customLimit = CanonicalizeSyncJson("{\"a\":1}", qsizetype(4));
	CHECK(!customLimit);
	CHECK(customLimit.error == SyncJsonErrorKind::SizeLimit);
	CHECK(customLimit.offset == 4);

	auto bigJson = QByteArray("{\"k\":\"");
	bigJson.append(QByteArray(4 * 1024 * 1024 + 128, 'x'));
	bigJson.append("\"}");
	CHECK(bigJson.size() > 4 * 1024 * 1024);
	const auto legacyRejectBig = CanonicalizeSyncJson(bigJson);
	CHECK(!legacyRejectBig);
	CHECK(legacyRejectBig.error == SyncJsonErrorKind::SizeLimit);

	const auto uncappedOk = CanonicalizeSyncJson(bigJson, std::nullopt);
	CHECK(bool(uncappedOk));
	CHECK(uncappedOk.canonical == bigJson);

	auto libJson = QByteArray("{\"format\":1,\"playlists\":{");
	const auto entrySuffix = QByteArray(
		":{\"deleted\":[false,100],\"order\":[[\"1\"],100],\"songs\":{\"1\":[false,100]},\"title\":[\"Mix\",100]}");
	const auto alphabet = "abcdefghijklmnopqrstuvwxyz234567";
	for (auto i = 0; libJson.size() <= 4 * 1024 * 1024 + 65536; ++i) {
		if (i > 0) {
			libJson.append(',');
		}
		auto id = QByteArray("\"pl-");
		auto v = uint32_t(i);
		for (auto j = 0; j < 16; ++j) {
			id.append(alphabet[v & 31]);
			v >>= 5;
		}
		id.append('"');
		libJson.append(id);
		libJson.append(entrySuffix);
	}
	libJson.append("},\"purged\":{}}");
	CHECK(libJson.size() > 4 * 1024 * 1024);

	const auto localParsed = ParsePlaylistLibrary(libJson);
	CHECK(bool(localParsed));
	const auto localSerialized = SerializePlaylistLibrary(localParsed.library);
	CHECK(bool(localSerialized));
	CHECK(localSerialized.canonical.size() > 4 * 1024 * 1024);
	CHECK(ParsePlaylistLibrary(localSerialized.canonical).library == localParsed.library);

	const auto cappedReject = ParsePlaylistLibrary(
		libJson,
		0,
		qsizetype(4 * 1024 * 1024));
	CHECK(!cappedReject);
	CHECK(cappedReject.error == PlaylistLibraryError::SizeLimit);
}

void TestGoldenAndCanonicalBytes() {
	const auto goldenInput = QByteArray(R"({
  "purged": {"pl-zq5wd2ha6kcm3tve": 1},
  "format": 1,
  "playlists": {
    "pl-k3f7q2abx7mnp4rt": {
      "title": ["Evening", 1790000000123],
      "deleted": [false, 1790000000123],
      "order": [["5361234567890123456"], 1790000000456],
      "songs": {
        "5361234567890999999": [false, 1790000000900],
        "5361234567890123456": [
          {"peer": "channel:1234567890",
           "msg": 5521,
           "un": "somechannel",
           "alt": [{"peer": "user:777000111", "msg": 88}],
           "meta": {"title": "Song", "perf": "Artist", "dur": 245},
           "since": 1790000000200},
          1790000000300]
      }
    }
  }
})");
	const auto expectedCanonical = QByteArray(
		"{\"format\":1,\"playlists\":{\"pl-k3f7q2abx7mnp4rt\":{"
		"\"deleted\":[false,1790000000123],"
		"\"order\":[[\"5361234567890123456\"],1790000000456],"
		"\"songs\":{"
		"\"5361234567890123456\":[{\"alt\":[{\"msg\":88,\"peer\":\"user:777000111\"}],"
		"\"meta\":{\"dur\":245,\"perf\":\"Artist\",\"title\":\"Song\"},"
		"\"msg\":5521,\"peer\":\"channel:1234567890\",\"since\":1790000000200,"
		"\"un\":\"somechannel\"},1790000000300],"
		"\"5361234567890999999\":[false,1790000000900]},"
		"\"title\":[\"Evening\",1790000000123]}},"
		"\"purged\":{\"pl-zq5wd2ha6kcm3tve\":1}}");

	const auto parsed = ParsePlaylistLibrary(goldenInput);
	CHECK(bool(parsed));
	const auto serialized = SerializePlaylistLibrary(parsed.library);
	CHECK(bool(serialized));
	CHECK(serialized.canonical == expectedCanonical);
	CHECK(CanonicalizeSyncJson(serialized.canonical, std::nullopt).canonical
		== expectedCanonical);

	const auto empty = ParsePlaylistLibrary("{\"purged\":{},\"playlists\":{},\"format\":1}");
	CHECK(bool(empty));
	CHECK(SerializePlaylistLibrary(empty.library).canonical
		== "{\"format\":1,\"playlists\":{},\"purged\":{}}");

	auto keyOrderLib = PlaylistLibrary();
	auto pl = Playlist();
	pl.songs = std::map<uint64_t, PlaylistSongRegister>{
		{ 2ULL, RemovedSong(10) },
		{ 10ULL, RemovedSong(10) },
		{ std::numeric_limits<uint64_t>::max(), RemovedSong(10) },
	};
	keyOrderLib.playlists.emplace(u"pl-aaaaaaaaaaaaaaaa"_q, std::move(pl));
	const auto keyOrderBytes = SerializePlaylistLibrary(keyOrderLib);
	CHECK(bool(keyOrderBytes));
	CHECK(keyOrderBytes.canonical
		== "{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"songs\":{"
		"\"10\":[false,10],"
		"\"18446744073709551615\":[false,10],"
		"\"2\":[false,10]}}},\"purged\":{}}");
	CHECK(ParsePlaylistLibrary(keyOrderBytes.canonical).library == keyOrderLib);
}

void TestPartialAndSelfNormalization() {
	const auto partialJson = QByteArray(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"title\":[\"Mix\",5]},"
		"\"pl-bbbbbbbbbbbbbbbb\":{\"songs\":{}}},\"purged\":{}}");
	const auto parsed = ParsePlaylistLibrary(partialJson);
	CHECK(bool(parsed));
	CHECK(parsed.library.playlists.at(u"pl-aaaaaaaaaaaaaaaa"_q).isPartial());
	CHECK(!parsed.library.playlists.at(u"pl-aaaaaaaaaaaaaaaa"_q).songs.has_value());
	CHECK(parsed.library.playlists.at(u"pl-bbbbbbbbbbbbbbbb"_q).songs.has_value());
	CHECK(parsed.library.playlists.at(u"pl-bbbbbbbbbbbbbbbb"_q).songs->empty());
	CHECK(SerializePlaylistLibrary(parsed.library).canonical == partialJson);

	const auto selfInput = QByteArray(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"songs\":{"
		"\"1\":[{\"meta\":{\"dur\":1,\"perf\":\"\",\"title\":\"\"},"
		"\"msg\":7,\"peer\":\"user:42\",\"since\":10},10]}}},"
		"\"purged\":{}}");
	const auto parsedSelf = ParsePlaylistLibrary(selfInput, 42);
	CHECK(bool(parsedSelf));
	const auto &song = parsedSelf.library.playlists.at(u"pl-aaaaaaaaaaaaaaaa"_q)
		.songs->at(1).value.source;
	CHECK(song.has_value());
	CHECK(song->peer == PeerToken());
	const auto serSelf = SerializePlaylistLibrary(parsedSelf.library, 42);
	CHECK(bool(serSelf));
	CHECK(!serSelf.canonical.contains("user:42"));
	CHECK(serSelf.canonical.contains("\"peer\":\"self\""));

	auto inMemorySelf = PlaylistLibrary();
	auto pl = Playlist();
	pl.songs = std::map<uint64_t, PlaylistSongRegister>{
		{ 1ULL, ActiveSong(MakeSource({ PeerTokenKind::User, 42 }, 7, 10, QString(), QString(), 1), 10) },
	};
	inMemorySelf.playlists.emplace(u"pl-aaaaaaaaaaaaaaaa"_q, std::move(pl));
	const auto serInMem = SerializePlaylistLibrary(inMemorySelf, 42);
	CHECK(bool(serInMem));
	CHECK(!serInMem.canonical.contains("user:42"));
	CHECK(serInMem.canonical.contains("\"peer\":\"self\""));
	CHECK(serInMem.canonical == serSelf.canonical);

	const auto badSelf = ParsePlaylistLibrary(
		"{\"format\":1,\"playlists\":{},\"purged\":{}}",
		uint64_t(std::numeric_limits<int64_t>::max()) + 1);
	CHECK(!badSelf);
	CHECK(badSelf.error == PlaylistLibraryError::InvalidSelfUserId);
}

void TestValidationRejectionsAndBoundaries() {
	const auto expectInvalid = [](
			const QByteArray &json,
			PlaylistLibraryError expected,
			uint64_t selfUserId = 0) {
		const auto res = ParsePlaylistLibrary(json, selfUserId);
		CHECK(res.status == PlaylistLibraryStatus::Invalid);
		CHECK(res.error == expected);
	};

	for (const auto &newer : {
		QByteArray("{\"format\":2}"),
		QByteArray("{\"format\":2,\"future\":[1,2,3]}"),
		QByteArray("{\"format\":9007199254740991,\"playlists\":{},\"purged\":{}}"),
	}) {
		const auto res = ParsePlaylistLibrary(newer);
		CHECK(res.status == PlaylistLibraryStatus::NewerFormat);
		CHECK(res.error == PlaylistLibraryError::None);
	}
	auto newerMem = PlaylistLibrary();
	newerMem.format = 2;
	CHECK(SerializePlaylistLibrary(newerMem).status == PlaylistLibraryStatus::NewerFormat);
	CHECK(JoinPlaylistLibraries(newerMem, PlaylistLibrary()).status
		== PlaylistLibraryStatus::NewerFormat);
	CHECK(PlaylistLibraryContains(newerMem, PlaylistLibrary()).status
		== PlaylistLibraryStatus::NewerFormat);

	expectInvalid("[]", PlaylistLibraryError::FieldType);
	expectInvalid("{\"playlists\":{},\"purged\":{}}", PlaylistLibraryError::MissingField);
	expectInvalid("{\"format\":0,\"playlists\":{},\"purged\":{}}", PlaylistLibraryError::InvalidFormat);
	expectInvalid("{\"format\":-1,\"playlists\":{},\"purged\":{}}", PlaylistLibraryError::InvalidFormat);
	expectInvalid("{\"format\":\"1\",\"playlists\":{},\"purged\":{}}", PlaylistLibraryError::FieldType);
	expectInvalid("{\"format\":1.5,\"playlists\":{},\"purged\":{}}", PlaylistLibraryError::InvalidJson);
	expectInvalid("{\"format\":1,\"playlists\":{},\"purged\":{},\"extra\":1}", PlaylistLibraryError::UnknownKey);
	expectInvalid("{\"format\":1,\"playlists\":{}}", PlaylistLibraryError::MissingField);
	expectInvalid("{\"format\":1,\"playlists\":{},\"purged\":{},\"format\":1}", PlaylistLibraryError::InvalidJson);
	expectInvalid("{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"title\":[\"\\ud800\",1]}},\"purged\":{}}",
		PlaylistLibraryError::InvalidJson);
	expectInvalid(QByteArray("{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"title\":[\"")
		+ "\xc0\xaf" + "\",1]}},\"purged\":{}}", PlaylistLibraryError::InvalidJson);

	expectInvalid("{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{}},\"purged\":{}}",
		PlaylistLibraryError::EmptyPlaylist);
	expectInvalid("{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"title\":[\"Mix\",1],\"x\":1}},\"purged\":{}}",
		PlaylistLibraryError::UnknownKey);
	expectInvalid("{\"format\":1,\"playlists\":{\"pl-bad\":{}},\"purged\":{}}",
		PlaylistLibraryError::InvalidId);
	expectInvalid("{\"format\":1,\"playlists\":{\"pl-0000000000000000\":{\"title\":[\"Mix\",1]}},\"purged\":{}}",
		PlaylistLibraryError::InvalidId);
	expectInvalid("{\"format\":1,\"playlists\":{\"pl-Aaaaaaaaaaaaaaaa\":{\"title\":[\"Mix\",1]}},\"purged\":{}}",
		PlaylistLibraryError::InvalidId);

	expectInvalid("{\"format\":1,\"playlists\":{},\"purged\":{\"pl-aaaaaaaaaaaaaaaa\":2}}",
		PlaylistLibraryError::InvalidPurged);
	expectInvalid("{\"format\":1,\"playlists\":{},\"purged\":{\"pl-aaaaaaaaaaaaaaaa\":true}}",
		PlaylistLibraryError::FieldType);
	expectInvalid("{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"title\":[\"Mix\",1]}},\"purged\":{\"pl-aaaaaaaaaaaaaaaa\":1}}",
		PlaylistLibraryError::PurgedPlaylistPresent);

	const auto maxStampJson = QByteArray(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"deleted\":[false,4503599627370496]}},\"purged\":{}}");
	CHECK(bool(ParsePlaylistLibrary(maxStampJson)));
	expectInvalid(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"deleted\":[false,4503599627370497]}},\"purged\":{}}",
		PlaylistLibraryError::InvalidStamp);
	expectInvalid(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"deleted\":[false,0]}},\"purged\":{}}",
		PlaylistLibraryError::InvalidStamp);

	expectInvalid(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"order\":[[\"0\"],1]}},\"purged\":{}}",
		PlaylistLibraryError::InvalidDocumentId);
	expectInvalid(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"order\":[[\"01\"],1]}},\"purged\":{}}",
		PlaylistLibraryError::InvalidDocumentId);
	expectInvalid(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"order\":[[\"18446744073709551616\"],1]}},\"purged\":{}}",
		PlaylistLibraryError::InvalidDocumentId);
	expectInvalid(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"order\":[[\"5\",\"5\"],1]}},\"purged\":{}}",
		PlaylistLibraryError::InvalidOrder);
	CHECK(bool(ParsePlaylistLibrary(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"order\":[[\"99\",\"18446744073709551615\"],1],\"songs\":{}}},\"purged\":{}}")));

	const auto title64Emoji = QString::fromUtf8(
		QByteArray("\xf0\x9f\x8e\xb5").repeated(64));
	const auto title65Emoji = QString::fromUtf8(
		QByteArray("\xf0\x9f\x8e\xb5").repeated(65));
	auto titleLib = PlaylistLibrary();
	titleLib.playlists[u"pl-aaaaaaaaaaaaaaaa"_q].title = PlaylistTitleRegister{ title64Emoji, 1 };
	CHECK(bool(SerializePlaylistLibrary(titleLib)));
	CHECK(bool(ParsePlaylistLibrary(SerializePlaylistLibrary(titleLib).canonical)));
	titleLib.playlists[u"pl-aaaaaaaaaaaaaaaa"_q].title = PlaylistTitleRegister{ title65Emoji, 1 };
	CHECK(SerializePlaylistLibrary(titleLib).error == PlaylistLibraryError::InvalidTitle);

	for (const auto &badTitle : {
		u""_q, u" "_q, u" Mix"_q, u"Mix "_q, u"\u00a0Mix"_q,
		u"A\nB"_q, u"A\rB"_q, u"A\tB"_q, u"A\u007fB"_q, u"A\u2028B"_q,
	}) {
		titleLib.playlists[u"pl-aaaaaaaaaaaaaaaa"_q].title = PlaylistTitleRegister{ badTitle, 1 };
		CHECK(SerializePlaylistLibrary(titleLib).error == PlaylistLibraryError::InvalidTitle);
	}

	const auto meta96Emoji = QString::fromUtf8(
		QByteArray("\xf0\x9f\x8e\xb5").repeated(96));
	const auto meta97Emoji = QString::fromUtf8(
		QByteArray("\xf0\x9f\x8e\xb5").repeated(97));
	auto metaLib = PlaylistLibrary();
	metaLib.playlists[u"pl-aaaaaaaaaaaaaaaa"_q].songs = std::map<uint64_t, PlaylistSongRegister>{
		{ 1ULL, ActiveSong(MakeSource({ PeerTokenKind::Channel, 9 }, 2147483647, 4503599627370496ULL,
			meta96Emoji, meta96Emoji, 2147483647), 4503599627370496ULL) },
	};
	CHECK(bool(SerializePlaylistLibrary(metaLib)));
	CHECK(bool(ParsePlaylistLibrary(SerializePlaylistLibrary(metaLib).canonical)));
	metaLib.playlists[u"pl-aaaaaaaaaaaaaaaa"_q].songs->at(1).value.source->meta.title = meta97Emoji;
	CHECK(SerializePlaylistLibrary(metaLib).error == PlaylistLibraryError::InvalidMeta);

	auto loneSurrogate = QString();
	loneSurrogate.append(QChar(0xD800));
	metaLib.playlists[u"pl-aaaaaaaaaaaaaaaa"_q].songs->at(1).value.source->meta.title = loneSurrogate;
	CHECK(SerializePlaylistLibrary(metaLib).error == PlaylistLibraryError::InvalidMeta);

	expectInvalid(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"songs\":{\"1\":[true,1]}}},\"purged\":{}}",
		PlaylistLibraryError::InvalidValue);
	expectInvalid(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"songs\":{\"1\":["
		"{\"meta\":{\"dur\":0,\"perf\":\"\",\"title\":\"\"},\"msg\":1,\"peer\":\"self\",\"since\":11},10]}}},\"purged\":{}}",
		PlaylistLibraryError::InvalidSince);
	expectInvalid(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"songs\":{\"1\":["
		"{\"alt\":[],\"meta\":{\"dur\":0,\"perf\":\"\",\"title\":\"\"},\"msg\":1,\"peer\":\"self\",\"since\":5},10]}}},\"purged\":{}}",
		PlaylistLibraryError::InvalidAlt);
	expectInvalid(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"songs\":{\"1\":["
		"{\"alt\":[{\"msg\":1,\"peer\":\"user:42\"}],\"meta\":{\"dur\":0,\"perf\":\"\",\"title\":\"\"},"
		"\"msg\":1,\"peer\":\"self\",\"since\":5},10]}}},\"purged\":{}}",
		PlaylistLibraryError::InvalidAlt,
		42);
	expectInvalid(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"songs\":{\"1\":["
		"{\"alt\":[{\"msg\":2,\"peer\":\"chat:5\"},{\"msg\":2,\"peer\":\"chat:5\"}],"
		"\"meta\":{\"dur\":0,\"perf\":\"\",\"title\":\"\"},\"msg\":1,\"peer\":\"self\",\"since\":5},10]}}},\"purged\":{}}",
		PlaylistLibraryError::InvalidAlt);
	expectInvalid(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"songs\":{\"1\":["
		"{\"alt\":[{\"msg\":2,\"peer\":\"chat:1\"},{\"msg\":3,\"peer\":\"chat:1\"},"
		"{\"msg\":4,\"peer\":\"chat:1\"},{\"msg\":5,\"peer\":\"chat:1\"}],"
		"\"meta\":{\"dur\":0,\"perf\":\"\",\"title\":\"\"},\"msg\":1,\"peer\":\"self\",\"since\":5},10]}}},\"purged\":{}}",
		PlaylistLibraryError::InvalidAlt);
	expectInvalid(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"songs\":{\"1\":["
		"{\"alt\":[{\"msg\":1,\"peer\":\"channel:5\",\"un\":\"other\"}],"
		"\"meta\":{\"dur\":0,\"perf\":\"\",\"title\":\"\"},"
		"\"msg\":1,\"peer\":\"channel:5\",\"since\":5,\"un\":\"chan\"},10]}}},\"purged\":{}}",
		PlaylistLibraryError::InvalidAlt);
	CHECK(bool(ParsePlaylistLibrary(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"songs\":{\"1\":["
		"{\"alt\":[{\"msg\":2,\"peer\":\"chat:1\"},{\"msg\":3,\"peer\":\"chat:2\"},"
		"{\"msg\":4,\"peer\":\"channel:3\",\"un\":\"abcd\"}],"
		"\"meta\":{\"dur\":0,\"perf\":\"\",\"title\":\"\"},\"msg\":1,\"peer\":\"self\",\"since\":5},10]}}},\"purged\":{}}")));
	expectInvalid(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"songs\":{\"1\":["
		"{\"meta\":{\"dur\":0,\"perf\":\"\",\"title\":\"\"},\"msg\":1,\"peer\":\"channel:1\",\"since\":5,\"un\":\"abc\"},10]}}},\"purged\":{}}",
		PlaylistLibraryError::InvalidUsername);
	expectInvalid(
		"{\"format\":1,\"playlists\":{\"pl-aaaaaaaaaaaaaaaa\":{\"songs\":{\"1\":["
		"{\"meta\":{\"dur\":0,\"perf\":\"\",\"title\":\"\"},\"msg\":1,\"peer\":\"channel:1\",\"since\":5,\"un\":\"bad-name\"},10]}}},\"purged\":{}}",
		PlaylistLibraryError::InvalidUsername);
}

void TestJoinContainsAndTies() {
	const auto idA = u"pl-aaaaaaaaaaaaaaaa"_q;
	const auto idB = u"pl-bbbbbbbbbbbbbbbb"_q;

	auto left = PlaylistLibrary();
	left.playlists[idA].deleted = PlaylistDeletedRegister{ false, 100 };
	left.playlists[idA].title = PlaylistTitleRegister{ u"Evening"_q, 100 };
	left.playlists[idA].order = PlaylistOrderRegister{ { 1ULL, 2ULL }, 100 };
	left.playlists[idA].songs = std::map<uint64_t, PlaylistSongRegister>{
		{ 1ULL, RemovedSong(100) },
		{ 2ULL, ActiveSong(MakeSource({ PeerTokenKind::User, 1 }, 5, 50), 100) },
		{ 3ULL, ActiveSong(MakeSource({ PeerTokenKind::User, 42 }, 10, 50), 100) },
	};

	auto right = PlaylistLibrary();
	right.playlists[idA].deleted = PlaylistDeletedRegister{ true, 100 };
	right.playlists[idA].title = PlaylistTitleRegister{ u"Morning"_q, 100 };
	right.playlists[idA].order = PlaylistOrderRegister{ { 2ULL, 1ULL }, 100 };
	right.playlists[idA].songs = std::map<uint64_t, PlaylistSongRegister>{
		{ 1ULL, ActiveSong(MakeSource({ PeerTokenKind::Self, 0 }, 9, 80), 100) },
		{ 2ULL, RemovedSong(101) },
		{ 3ULL, ActiveSong(MakeSource({ PeerTokenKind::Self, 0 }, 20, 50), 100) },
	};

	const auto lr = JoinPlaylistLibraries(left, right, 42);
	const auto rl = JoinPlaylistLibraries(right, left, 42);
	CHECK(bool(lr) && bool(rl));
	CHECK(SerializePlaylistLibrary(lr.library, 42).canonical
		== SerializePlaylistLibrary(rl.library, 42).canonical);
	const auto &joinedPl = lr.library.playlists.at(idA);
	CHECK(joinedPl.deleted == (PlaylistDeletedRegister{ true, 100 }));
	CHECK(joinedPl.title == (PlaylistTitleRegister{ u"Morning"_q, 100 }));
	CHECK(joinedPl.order == (PlaylistOrderRegister{ { 2ULL, 1ULL }, 100 }));
	CHECK(!joinedPl.songs->at(1).value.isRemoved());
	CHECK(joinedPl.songs->at(1).value.source->since == 80);
	CHECK(joinedPl.songs->at(2).value.isRemoved());
	CHECK(joinedPl.songs->at(2).stamp == 101);
	CHECK(joinedPl.songs->at(3).value.source->peer == PeerToken());
	CHECK(joinedPl.songs->at(3).value.source->msg == 20);

	const auto cLrL = PlaylistLibraryContains(lr.library, left, 42);
	const auto cLrR = PlaylistLibraryContains(lr.library, right, 42);
	const auto cLLr = PlaylistLibraryContains(left, lr.library, 42);
	CHECK(cLrL.valid() && cLrL.contains);
	CHECK(cLrR.valid() && cLrR.contains);
	CHECK(cLLr.valid() && !cLLr.contains);

	auto noSongs = PlaylistLibrary();
	noSongs.playlists[idB].title = PlaylistTitleRegister{ u"OnlyTitle"_q, 10 };
	auto emptySongs = noSongs;
	emptySongs.playlists[idB].songs = std::map<uint64_t, PlaylistSongRegister>{};
	const auto cEmptyContainsNo = PlaylistLibraryContains(emptySongs, noSongs);
	const auto cNoContainsEmpty = PlaylistLibraryContains(noSongs, emptySongs);
	CHECK(cEmptyContainsNo.valid() && cEmptyContainsNo.contains);
	CHECK(cNoContainsEmpty.valid() && !cNoContainsEmpty.contains);

	auto purgedB = PlaylistLibrary();
	purgedB.purged.insert(idA);
	const auto joinedPurged = JoinPlaylistLibraries(left, purgedB, 42);
	CHECK(bool(joinedPurged));
	CHECK(joinedPurged.library.playlists.empty());
	CHECK(joinedPurged.library.purged.contains(idA));
	const auto cPurgedContainsLeft = PlaylistLibraryContains(purgedB, left, 42);
	CHECK(cPurgedContainsLeft.valid() && cPurgedContainsLeft.contains);
}

PlaylistLibrary RandomLibrary(std::mt19937_64 &rng) {
	static constexpr const char *kIds[] = {
		"pl-aaaaaaaaaaaaaaaa",
		"pl-bbbbbbbbbbbbbbbb",
		"pl-cccccccccccccccc",
		"pl-dddddddddddddddd",
	};
	static const uint64_t kDocIds[] = {
		1ULL,
		2ULL,
		10ULL,
		5361234567890123456ULL,
		std::numeric_limits<uint64_t>::max(),
	};
	auto lib = PlaylistLibrary();
	for (const auto *rawId : kIds) {
		const auto id = QString::fromLatin1(rawId);
		const auto role = rng() % 5;
		if (role == 0) {
			continue;
		}
		if (role == 1) {
			lib.purged.insert(id);
			continue;
		}
		auto pl = Playlist();
		if (rng() % 3 != 0) {
			const auto stamp = 1 + (rng() % 8);
			pl.title = PlaylistTitleRegister{
				(rng() % 2 == 0) ? u"Alpha"_q : u"Beta"_q,
				stamp,
			};
		}
		if (rng() % 3 != 0) {
			pl.deleted = PlaylistDeletedRegister{
				(rng() % 2 == 0),
				1 + (rng() % 8),
			};
		}
		if (rng() % 3 != 0) {
			auto order = std::vector<uint64_t>();
			for (const auto docId : kDocIds) {
				if (rng() % 2 == 0) {
					order.push_back(docId);
				}
			}
			std::shuffle(order.begin(), order.end(), rng);
			pl.order = PlaylistOrderRegister{ std::move(order), 1 + (rng() % 8) };
		}
		if (rng() % 3 != 0 || pl.isPartial() && !pl.title && !pl.deleted && !pl.order) {
			auto songs = std::map<uint64_t, PlaylistSongRegister>();
			for (const auto docId : kDocIds) {
				const auto kind = rng() % 4;
				if (kind == 0) {
					continue;
				}
				const auto stamp = 1 + (rng() % 8);
				if (kind == 1) {
					songs.emplace(docId, RemovedSong(stamp));
				} else {
					const auto since = 1 + (rng() % stamp);
					const auto peerKind = PeerTokenKind(rng() % 4);
					const auto peer = (peerKind == PeerTokenKind::Self)
						? PeerToken()
						: PeerToken{ peerKind, 1 + (rng() % 50) };
					auto alt = std::vector<PlaylistSourceRef>();
					if (rng() % 2 == 0) {
						alt.push_back({ PeerToken{ PeerTokenKind::Chat, 999 }, int32_t(1 + rng() % 100), {} });
					}
					songs.emplace(
						docId,
						ActiveSong(
							MakeSource(
								peer,
								int32_t(1 + rng() % 500),
								since,
								(rng() % 2 == 0) ? u"Track A"_q : u"Track B"_q,
								u"Artist"_q,
								int32_t(rng() % 400),
								(peerKind == PeerTokenKind::Channel && rng() % 2 == 0)
									? u"channame"_q : QString(),
								std::move(alt)),
							stamp));
				}
			}
			pl.songs = std::move(songs);
		}
		lib.playlists.emplace(id, std::move(pl));
	}
	return lib;
}

void TestSeededJoinProperties() {
	auto rng = std::mt19937_64(0x506c6179ULL);
	for (auto i = 0; i < 200; ++i) {
		const auto a = RandomLibrary(rng);
		const auto b = RandomLibrary(rng);
		const auto c = RandomLibrary(rng);

		const auto aa = JoinPlaylistLibraries(a, a, 42);
		CHECK(bool(aa));
		CHECK(SerializePlaylistLibrary(aa.library, 42).canonical
			== SerializePlaylistLibrary(a, 42).canonical);

		const auto ab = JoinPlaylistLibraries(a, b, 42);
		const auto ba = JoinPlaylistLibraries(b, a, 42);
		CHECK(bool(ab) && bool(ba));
		const auto abBytes = SerializePlaylistLibrary(ab.library, 42);
		const auto baBytes = SerializePlaylistLibrary(ba.library, 42);
		CHECK(bool(abBytes) && bool(baBytes));
		CHECK(abBytes.canonical == baBytes.canonical);
		CHECK(CanonicalizeSyncJson(abBytes.canonical, std::nullopt).canonical
			== abBytes.canonical);
		CHECK(ParsePlaylistLibrary(abBytes.canonical, 42).library == ab.library);

		const auto ab_c = JoinPlaylistLibraries(ab.library, c, 42);
		const auto bc = JoinPlaylistLibraries(b, c, 42);
		const auto a_bc = JoinPlaylistLibraries(a, bc.library, 42);
		CHECK(bool(ab_c) && bool(bc) && bool(a_bc));
		CHECK(SerializePlaylistLibrary(ab_c.library, 42).canonical
			== SerializePlaylistLibrary(a_bc.library, 42).canonical);

		const auto cA = PlaylistLibraryContains(ab.library, a, 42);
		const auto cB = PlaylistLibraryContains(ab.library, b, 42);
		CHECK(cA.valid() && cA.contains);
		CHECK(cB.valid() && cB.contains);
	}
}

} // namespace

int main() {
	TestSyncJsonBudgetExtension();
	TestGoldenAndCanonicalBytes();
	TestPartialAndSelfNormalization();
	TestValidationRejectionsAndBoundaries();
	TestJoinContainsAndTies();
	TestSeededJoinProperties();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
