/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists.h"
#include "purple/purple_playlists_ops.h"
#include "purple/purple_playlists_policy.h"
#include "purple/purple_types.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <cstdio>
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

PlaylistSongCandidate MakeQualifiedCandidate() {
	return PlaylistSongCandidate{
		.documentId = 1001,
		.messageId = 55,
		.hasAudioAttribute = true,
		.isVoiceMessage = false,
		.isRegularServerMessage = true,
		.isMainMessageMedia = true,
	};
}

PlaylistAutoPurgeContext MakeCompleteSyncContext(uint64_t nowMs) {
	return PlaylistAutoPurgeContext{
		.syncEnabled = true,
		.nowMs = nowMs,
		.selfUserId = 0,
		.serverRequestSucceeded = true,
		.listingPagedToEnd = true,
		.missingMemoEntriesResolved = true,
		.allCandidatesClassifiedOrRead = true,
		.noTransientReadFailure = true,
		.noUnknownOwnership = true,
		.vanishedMemoRecordCount = 0,
		.newValidRecordCount = 0,
		.noPresentNewerFormatRecord = true,
	};
}

void TestQualifySong() {
	const auto valid = MakeQualifiedCandidate();
	CHECK(QualifySong(valid, false).reason == SongQualificationReason::Qualified);
	CHECK(bool(QualifySong(valid, false)));
	CHECK(QualifySong(valid, true).reason == SongQualificationReason::Qualified);

	auto protectedSong = valid;
	protectedSong.isProtectedContent = true;
	CHECK(QualifySong(protectedSong, false).reason
		== SongQualificationReason::ProtectedContentRestricted);
	CHECK(!QualifySong(protectedSong, false).qualified());
	CHECK(QualifySong(protectedSong, true).reason
		== SongQualificationReason::Qualified);

	auto defaultUnknown = PlaylistSongCandidate{};
	defaultUnknown.documentId = 10;
	defaultUnknown.messageId = 20;
	CHECK(!QualifySong(defaultUnknown, true));

	auto onlyAudio = defaultUnknown;
	onlyAudio.hasAudioAttribute = true;
	onlyAudio.isRegularServerMessage = true;
	CHECK(QualifySong(onlyAudio, true).reason
		== SongQualificationReason::NotMainMessageMedia);

	auto notRegular = valid;
	notRegular.isRegularServerMessage = false;
	CHECK(QualifySong(notRegular, true).reason
		== SongQualificationReason::NotRegularServerMessage);

	auto noAudio = valid;
	noAudio.hasAudioAttribute = false;
	CHECK(QualifySong(noAudio, true).reason
		== SongQualificationReason::MissingAudioAttribute);

	auto voiceWithAudio = valid;
	voiceWithAudio.isVoiceMessage = true;
	CHECK(QualifySong(voiceWithAudio, true).reason
		== SongQualificationReason::VoiceMessage);

	auto scheduled = valid;
	scheduled.isScheduled = true;
	CHECK(QualifySong(scheduled, true).reason
		== SongQualificationReason::ScheduledMessage);

	auto localUnsent = valid;
	localUnsent.isLocalUnsent = true;
	CHECK(QualifySong(localUnsent, true).reason
		== SongQualificationReason::LocalUnsentMessage);

	auto sponsored = valid;
	sponsored.isSponsored = true;
	CHECK(QualifySong(sponsored, true).reason
		== SongQualificationReason::SponsoredMessage);

	auto story = valid;
	story.isStory = true;
	CHECK(QualifySong(story, true).reason
		== SongQualificationReason::StoryMessage);

	auto profile = valid;
	profile.isProfileMusic = true;
	CHECK(QualifySong(profile, true).reason
		== SongQualificationReason::ProfileMusicItem);

	auto profileWithoutRegular = valid;
	profileWithoutRegular.isRegularServerMessage = false;
	profileWithoutRegular.isProfileMusic = true;
	CHECK(QualifySong(profileWithoutRegular, true).reason
		== SongQualificationReason::ProfileMusicItem);

	auto secret = valid;
	secret.isSecretChat = true;
	CHECK(QualifySong(secret, true).reason
		== SongQualificationReason::SecretChatMessage);

	auto richSubtrack = valid;
	richSubtrack.isRichMediaSubtrack = true;
	CHECK(QualifySong(richSubtrack, true).reason
		== SongQualificationReason::RichMediaSubtrack);

	auto ttl = valid;
	ttl.hasSelfDestructTimer = true;
	CHECK(QualifySong(ttl, true).reason
		== SongQualificationReason::SelfDestructTimer);

	auto zeroDoc = valid;
	zeroDoc.documentId = 0;
	CHECK(QualifySong(zeroDoc, true).reason
		== SongQualificationReason::InvalidDocumentId);

	auto zeroMsg = valid;
	zeroMsg.messageId = 0;
	CHECK(QualifySong(zeroMsg, true).reason
		== SongQualificationReason::InvalidMessageId);

	auto negMsg = valid;
	negMsg.messageId = -5;
	CHECK(QualifySong(negMsg, true).reason
		== SongQualificationReason::InvalidMessageId);

	PlaylistLibrary lib;
	const auto created = CreatePlaylist(
		lib,
		u"Existing"_q,
		QByteArray(int(kPlaylistIdEntropyBytes), '\x10'),
		100);
	const auto added = AddPlaylistSongs(
		created.library,
		created.playlistId,
		{ { 1001, PeerToken{ PeerTokenKind::Chat, 9 }, 55, QString(), { u"Keep"_q, u"Art"_q, 120 } } },
		110);
	CHECK(added);
	CHECK(!QualifySong(protectedSong, false));
	CHECK(PlaylistVisibleSongIds(added.library.playlists.at(created.playlistId))
		== std::vector<uint64_t>{ 1001 });
}

void TestPrepareSongMeta() {
	const auto basic = PrepareSongMeta({
		.title = u"Symphony"_q,
		.performer = u"Orchestra"_q,
		.fileName = u"fallback.mp3"_q,
		.durationSeconds = 240,
	});
	CHECK(bool(basic));
	CHECK(basic.meta.title == u"Symphony"_q);
	CHECK(basic.meta.performer == u"Orchestra"_q);
	CHECK(basic.meta.duration == 240);

	const auto fallback = PrepareSongMeta({
		.title = QString(),
		.performer = QString(),
		.fileName = u"fallback_name.mp3"_q,
		.durationSeconds = 0,
	});
	CHECK(bool(fallback));
	CHECK(fallback.meta.title == u"fallback_name.mp3"_q);
	CHECK(fallback.meta.performer.isEmpty());
	CHECK(fallback.meta.duration == 0);

	const auto emptyBoth = PrepareSongMeta({
		.title = QString(),
		.performer = QString(),
		.fileName = QString(),
		.durationSeconds = int64_t(kMaximumPlaylistDurationSeconds),
	});
	CHECK(bool(emptyBoth));
	CHECK(emptyBoth.meta.title.isEmpty());
	CHECK(emptyBoth.meta.duration == kMaximumPlaylistDurationSeconds);

	QString noteEmoji;
	noteEmoji.append(QChar(0xD83C));
	noteEmoji.append(QChar(0xDFB5));
	QString exact96Supp;
	for (int i = 0; i < 96; ++i) {
		exact96Supp.append(noteEmoji);
	}
	auto over97Supp = exact96Supp;
	over97Supp.append(noteEmoji);

	const auto suppTruncated = PrepareSongMeta({
		.title = over97Supp,
		.performer = QString(95, u'a') + noteEmoji + QString(5, u'z'),
		.fileName = u"ignored.mp3"_q,
		.durationSeconds = 10,
	});
	CHECK(bool(suppTruncated));
	CHECK(suppTruncated.meta.title == exact96Supp);
	CHECK(suppTruncated.meta.title.size() == 192);
	CHECK(suppTruncated.meta.performer == QString(95, u'a') + noteEmoji);
	CHECK(suppTruncated.meta.performer.size() == 97);
	CHECK(IsValidPlaylistMetaText(suppTruncated.meta.title));
	CHECK(IsValidPlaylistMetaText(suppTruncated.meta.performer));

	const auto boundaryDrop = PrepareSongMeta({
		.title = QString(96, u'b') + noteEmoji,
		.performer = u"P"_q,
		.fileName = QString(),
		.durationSeconds = 1,
	});
	CHECK(bool(boundaryDrop));
	CHECK(boundaryDrop.meta.title == QString(96, u'b'));

	QString unpairedHigh = u"abc"_q;
	unpairedHigh.append(QChar(0xD800));
	CHECK(PrepareSongMeta({ unpairedHigh, u"ok"_q, u"fallback"_q, 1 }).error
		== SongMetaPrepareError::InvalidTitleUnicode);
	CHECK(PrepareSongMeta({ QString(), u"ok"_q, unpairedHigh, 1 }).error
		== SongMetaPrepareError::InvalidTitleUnicode);

	QString lateUnpaired = QString(100, u'x');
	lateUnpaired.append(QChar(0xDC00));
	CHECK(PrepareSongMeta({ lateUnpaired, u"ok"_q, QString(), 1 }).error
		== SongMetaPrepareError::InvalidTitleUnicode);
	CHECK(PrepareSongMeta({ u"ok"_q, lateUnpaired, QString(), 1 }).error
		== SongMetaPrepareError::InvalidPerformerUnicode);

	QString reversedSurrogate;
	reversedSurrogate.append(QChar(0xDC00));
	reversedSurrogate.append(QChar(0xD800));
	CHECK(PrepareSongMeta({ u"ok"_q, reversedSurrogate, QString(), 1 }).error
		== SongMetaPrepareError::InvalidPerformerUnicode);

	CHECK(PrepareSongMeta({ u"ok"_q, u"p"_q, QString(), -1 }).error
		== SongMetaPrepareError::InvalidDuration);
	CHECK(PrepareSongMeta({ u"ok"_q, u"p"_q, QString(), int64_t(kMaximumPlaylistDurationSeconds) + 1 }).error
		== SongMetaPrepareError::InvalidDuration);
}

void TestSortPlaylistChatOrder() {
	const auto ordered = SortPlaylistChatOrder({
		{ .documentId = 900, .messageId = 1, .fromMigratedBasicGroup = false },
		{ .documentId = 300, .messageId = 50, .fromMigratedBasicGroup = true },
		{ .documentId = 200, .messageId = 50, .fromMigratedBasicGroup = true },
		{ .documentId = 100, .messageId = 10, .fromMigratedBasicGroup = true },
		{ .documentId = 500, .messageId = 2, .fromMigratedBasicGroup = false },
		{ .documentId = 400, .messageId = 2, .fromMigratedBasicGroup = false },
	});
	CHECK(bool(ordered));
	const std::vector<PlaylistChatOrderItem> expected{
		{ .documentId = 100, .messageId = 10, .fromMigratedBasicGroup = true },
		{ .documentId = 200, .messageId = 50, .fromMigratedBasicGroup = true },
		{ .documentId = 300, .messageId = 50, .fromMigratedBasicGroup = true },
		{ .documentId = 900, .messageId = 1, .fromMigratedBasicGroup = false },
		{ .documentId = 400, .messageId = 2, .fromMigratedBasicGroup = false },
		{ .documentId = 500, .messageId = 2, .fromMigratedBasicGroup = false },
	};
	CHECK(ordered.items == expected);

	CHECK(SortPlaylistChatOrder({ { 0, 1, false } }).error
		== PlaylistChatOrderError::InvalidDocumentId);
	CHECK(SortPlaylistChatOrder({ { 10, 0, true } }).error
		== PlaylistChatOrderError::InvalidMessageId);
	CHECK(SortPlaylistChatOrder({ { 10, -2, false } }).error
		== PlaylistChatOrderError::InvalidMessageId);
}

PlaylistLibrary BuildSampleDeletedLibrary(
		const QString &idA,
		uint64_t stampA,
		const QString &idActive,
		const QString &idKeepDeleted,
		uint64_t stampKeepDeleted) {
	PlaylistLibrary lib;
	lib = CreatePlaylist(lib, u"Delete A"_q, QByteArray(int(kPlaylistIdEntropyBytes), '\x01'), 10).library;
	lib = AddPlaylistSongs(lib, idA, { { 1, PeerToken{ PeerTokenKind::Chat, 1 }, 1, QString(), { u"A"_q, u"B"_q, 10 } } }, 15).library;
	lib = DeletePlaylist(lib, idA, stampA).library;

	lib = CreatePlaylist(lib, u"Active B"_q, QByteArray(int(kPlaylistIdEntropyBytes), '\x02'), 10).library;
	lib = AddPlaylistSongs(lib, idActive, { { 2, PeerToken{ PeerTokenKind::Chat, 2 }, 2, QString(), { u"C"_q, u"D"_q, 20 } } }, 20).library;

	lib = CreatePlaylist(lib, u"Recent Delete C"_q, QByteArray(int(kPlaylistIdEntropyBytes), '\x03'), 10).library;
	lib = DeletePlaylist(lib, idKeepDeleted, stampKeepDeleted).library;
	return lib;
}

void TestAutoPurgeDefaultsAndInvalidObservations() {
	const auto id = u"pl-aaaaaaaaaaaaaaaa"_q;
	auto library = PlaylistLibrary();
	library.playlists[id].deleted = PlaylistDeletedRegister{ true, 1000 };
	const auto now = uint64_t(1000) + kAutoPurgeRetentionMs;
	auto context = PlaylistAutoPurgeContext();
	context.nowMs = now;
	context.serverRequestSucceeded = true;
	CHECK(context.syncEnabled);
	const auto unspecifiedSync = AutoPurgePlaylists(library, {}, context);
	CHECK(unspecifiedSync.status == PlaylistAutoPurgeStatus::BlockedIncompleteDiscovery);
	CHECK(unspecifiedSync.library == library && unspecifiedSync.purgedCount() == 0);

	context = MakeCompleteSyncContext(now);
	for (const auto invalidFirstSeen : { uint64_t(0), uint64_t(1000), now - 1 }) {
		const auto observed = AutoPurgePlaylists(library,
			{ { id, { 1000, invalidFirstSeen } } }, context);
		CHECK(observed);
		CHECK(observed.library == library && observed.purgedCount() == 0);
		CHECK(observed.dueSeen.at(id) == (PlaylistAutoPurgeDueSeen{ 1000, now }));
	}
	const auto first = AutoPurgePlaylists(library, {}, context);
	context.nowMs = now + kAutoPurgeMinimumObservationMs;
	const auto later = AutoPurgePlaylists(library, first.dueSeen, context);
	CHECK(later && later.purgedCount() == 1);
}

void TestAutoPurgeSyncOnBoundariesAndGates() {
	const auto idA = *FormatPlaylistId(QByteArray(int(kPlaylistIdEntropyBytes), '\x01'));
	const auto idActive = *FormatPlaylistId(QByteArray(int(kPlaylistIdEntropyBytes), '\x02'));
	const auto idRecent = *FormatPlaylistId(QByteArray(int(kPlaylistIdEntropyBytes), '\x03'));

	const uint64_t stampA = 1000;
	const uint64_t stampRecent = 10000000;
	const auto lib = BuildSampleDeletedLibrary(idA, stampA, idActive, idRecent, stampRecent);

	const auto failClosed = AutoPurgePlaylists(lib, {}, PlaylistAutoPurgeContext{ .syncEnabled = true, .nowMs = stampA + kAutoPurgeRetentionMs });
	CHECK(!failClosed);
	CHECK(failClosed.library == lib);
	CHECK(failClosed.dueSeen.empty());
	CHECK(failClosed.purgedCount() == 0);

	const auto almostDue = AutoPurgePlaylists(
		lib,
		{},
		MakeCompleteSyncContext(stampA + kAutoPurgeRetentionMs - 1));
	CHECK(bool(almostDue));
	CHECK(almostDue.dueSeen.empty());
	CHECK(almostDue.purgedCount() == 0);

	const uint64_t t0 = stampA + kAutoPurgeRetentionMs;
	const auto staleInputSeen = std::map<QString, PlaylistAutoPurgeDueSeen>{
		{ idActive, { 10, 100 } },
		{ idRecent, { stampRecent, 100 } },
		{ u"pl-7777777777777777"_q, { 50, 100 } },
	};
	const auto firstDue = AutoPurgePlaylists(lib, staleInputSeen, MakeCompleteSyncContext(t0));
	CHECK(bool(firstDue));
	CHECK(firstDue.purgedCount() == 0);
	CHECK(firstDue.library == lib);
	CHECK(firstDue.dueSeen.size() == 1);
	CHECK(firstDue.dueSeen.at(idA) == (PlaylistAutoPurgeDueSeen{ stampA, t0 }));

	auto incompleteCases = std::vector<PlaylistAutoPurgeContext>{};
	{
		auto c = MakeCompleteSyncContext(t0 + kAutoPurgeMinimumObservationMs);
		c.serverRequestSucceeded = false;
		incompleteCases.push_back(c);
	}
	{
		auto c = MakeCompleteSyncContext(t0 + kAutoPurgeMinimumObservationMs);
		c.listingPagedToEnd = false;
		incompleteCases.push_back(c);
	}
	{
		auto c = MakeCompleteSyncContext(t0 + kAutoPurgeMinimumObservationMs);
		c.missingMemoEntriesResolved = false;
		incompleteCases.push_back(c);
	}
	{
		auto c = MakeCompleteSyncContext(t0 + kAutoPurgeMinimumObservationMs);
		c.allCandidatesClassifiedOrRead = false;
		incompleteCases.push_back(c);
	}
	{
		auto c = MakeCompleteSyncContext(t0 + kAutoPurgeMinimumObservationMs);
		c.noTransientReadFailure = false;
		incompleteCases.push_back(c);
	}
	{
		auto c = MakeCompleteSyncContext(t0 + kAutoPurgeMinimumObservationMs);
		c.noUnknownOwnership = false;
		incompleteCases.push_back(c);
	}
	{
		auto c = MakeCompleteSyncContext(t0 + kAutoPurgeMinimumObservationMs);
		c.vanishedMemoRecordCount = 1;
		c.newValidRecordCount = 0;
		incompleteCases.push_back(c);
	}
	for (const auto &ctx : incompleteCases) {
		const auto blocked = AutoPurgePlaylists(lib, firstDue.dueSeen, ctx);
		CHECK(!blocked);
		CHECK(blocked.library == lib);
		CHECK(blocked.dueSeen == firstDue.dueSeen);
		CHECK(blocked.purgedCount() == 0);

		const auto blockedEmpty = AutoPurgePlaylists(lib, {}, ctx);
		CHECK(!blockedEmpty);
		CHECK(blockedEmpty.library == lib);
		CHECK(blockedEmpty.dueSeen.empty());
	}

	const auto newerRecordBlocked = AutoPurgePlaylists(
		lib,
		firstDue.dueSeen,
		[&] {
			auto c = MakeCompleteSyncContext(t0 + kAutoPurgeMinimumObservationMs);
			c.noPresentNewerFormatRecord = false;
			return c;
		}());
	CHECK(newerRecordBlocked.status == PlaylistAutoPurgeStatus::BlockedNewerFormatRecord);
	CHECK(newerRecordBlocked.library == lib);
	CHECK(newerRecordBlocked.dueSeen.empty());
	CHECK(newerRecordBlocked.purgedCount() == 0);

	const auto postNewerCleared = AutoPurgePlaylists(
		lib,
		newerRecordBlocked.dueSeen,
		MakeCompleteSyncContext(t0 + kAutoPurgeMinimumObservationMs));
	CHECK(bool(postNewerCleared));
	CHECK(postNewerCleared.purgedCount() == 0);
	CHECK(postNewerCleared.dueSeen.at(idA)
		== (PlaylistAutoPurgeDueSeen{ stampA, t0 + kAutoPurgeMinimumObservationMs }));

	const auto before15m = AutoPurgePlaylists(
		lib,
		firstDue.dueSeen,
		MakeCompleteSyncContext(t0 + kAutoPurgeMinimumObservationMs - 1));
	CHECK(bool(before15m));
	CHECK(before15m.purgedCount() == 0);
	CHECK(before15m.dueSeen == firstDue.dueSeen);

	const uint64_t tRewindBase = t0 + 120000;
	const auto rewindStart = AutoPurgePlaylists(
		lib,
		{},
		MakeCompleteSyncContext(tRewindBase));
	CHECK(bool(rewindStart));
	CHECK(rewindStart.dueSeen.at(idA) == (PlaylistAutoPurgeDueSeen{ stampA, tRewindBase }));

	const auto rewound = AutoPurgePlaylists(
		lib,
		rewindStart.dueSeen,
		MakeCompleteSyncContext(tRewindBase - 60000));
	CHECK(bool(rewound));
	CHECK(rewound.purgedCount() == 0);
	CHECK(rewound.dueSeen == rewindStart.dueSeen);

	const auto postRewindWait = AutoPurgePlaylists(
		lib,
		rewound.dueSeen,
		MakeCompleteSyncContext(tRewindBase + kAutoPurgeMinimumObservationMs - 1));
	CHECK(bool(postRewindWait));
	CHECK(postRewindWait.purgedCount() == 0);
	CHECK(postRewindWait.dueSeen == rewindStart.dueSeen);

	const auto vanishedWithNew = AutoPurgePlaylists(
		lib,
		postRewindWait.dueSeen,
		[&] {
			auto c = MakeCompleteSyncContext(tRewindBase + kAutoPurgeMinimumObservationMs);
			c.vanishedMemoRecordCount = 1;
			c.newValidRecordCount = 1;
			return c;
		}());
	CHECK(bool(vanishedWithNew));
	CHECK(vanishedWithNew.purgedIds == std::vector<QString>{ idA });
	CHECK(vanishedWithNew.purgedCount() == 1);
	CHECK(vanishedWithNew.dueSeen.empty());
	CHECK(!vanishedWithNew.library.playlists.contains(idA));
	CHECK(vanishedWithNew.library.purged.contains(idA));
	CHECK(vanishedWithNew.library.playlists.at(idActive) == lib.playlists.at(idActive));
	CHECK(vanishedWithNew.library.playlists.at(idRecent) == lib.playlists.at(idRecent));

	const auto secondRun = AutoPurgePlaylists(
		vanishedWithNew.library,
		{ { idA, { stampA, t0 } } },
		MakeCompleteSyncContext(t0 + kAutoPurgeMinimumObservationMs + 1000));
	CHECK(bool(secondRun));
	CHECK(secondRun.purgedCount() == 0);
	CHECK(secondRun.dueSeen.empty());
	CHECK(secondRun.library == vanishedWithNew.library);
}

void TestAutoPurgeResetPartialMaxStampSyncOffAndSize() {
	const auto idPartial = *FormatPlaylistId(QByteArray(int(kPlaylistIdEntropyBytes), '\x04'));
	const auto idRelinkedDelete = *FormatPlaylistId(QByteArray(int(kPlaylistIdEntropyBytes), '\x05'));

	PlaylistLibrary lib;
	lib.playlists.emplace(idPartial, Playlist{
		.title = std::nullopt,
		.deleted = PlaylistDeletedRegister{ true, kMaximumPlaylistStamp },
		.order = std::nullopt,
		.songs = std::nullopt,
	});
	lib = CreatePlaylist(lib, u"Redel"_q, QByteArray(int(kPlaylistIdEntropyBytes), '\x05'), 100).library;
	lib = DeletePlaylist(lib, idRelinkedDelete, 200).library;
	lib = RestorePlaylist(lib, idRelinkedDelete, 250).library;
	lib = DeletePlaylist(lib, idRelinkedDelete, 300).library;
	lib.playlists.at(idRelinkedDelete).title = PlaylistTitleRegister{ u"Redel"_q, kMaximumPlaylistStamp };

	const auto initialStampSeen = std::map<QString, PlaylistAutoPurgeDueSeen>{
		{ idRelinkedDelete, { 200, 100 } },
	};
	const uint64_t dueChangedNow = 300 + kAutoPurgeRetentionMs + 2000000;
	const auto resetChanged = AutoPurgePlaylists(
		lib,
		initialStampSeen,
		MakeCompleteSyncContext(dueChangedNow));
	CHECK(bool(resetChanged));
	CHECK(resetChanged.purgedCount() == 0);
	CHECK(resetChanged.dueSeen.at(idRelinkedDelete) == (PlaylistAutoPurgeDueSeen{ 300, dueChangedNow }));

	const uint64_t highNow = kMaximumPlaylistStamp + kAutoPurgeRetentionMs;
	const auto firstMaxStampDue = AutoPurgePlaylists(
		resetChanged.library,
		resetChanged.dueSeen,
		MakeCompleteSyncContext(highNow));
	CHECK(bool(firstMaxStampDue));
	CHECK(firstMaxStampDue.purgedIds == std::vector<QString>{ idRelinkedDelete });
	CHECK(firstMaxStampDue.dueSeen.at(idPartial) == (PlaylistAutoPurgeDueSeen{ kMaximumPlaylistStamp, highNow }));

	const auto purgedMaxStamp = AutoPurgePlaylists(
		firstMaxStampDue.library,
		firstMaxStampDue.dueSeen,
		MakeCompleteSyncContext(highNow + kAutoPurgeMinimumObservationMs));
	CHECK(bool(purgedMaxStamp));
	CHECK(purgedMaxStamp.purgedIds == std::vector<QString>{ idPartial });
	CHECK(purgedMaxStamp.library.purged.contains(idPartial));
	CHECK(purgedMaxStamp.library.purged.contains(idRelinkedDelete));

	const auto syncOffNoServer = AutoPurgePlaylists(
		lib,
		initialStampSeen,
		PlaylistAutoPurgeContext{
			.syncEnabled = false,
			.nowMs = highNow,
			.serverRequestSucceeded = false,
		});
	CHECK(syncOffNoServer.status == PlaylistAutoPurgeStatus::BlockedServerRequestRequired);
	CHECK(syncOffNoServer.library == lib);
	CHECK(syncOffNoServer.dueSeen == initialStampSeen);

	const auto syncOffOk = AutoPurgePlaylists(
		lib,
		initialStampSeen,
		PlaylistAutoPurgeContext{
			.syncEnabled = false,
			.nowMs = highNow,
			.serverRequestSucceeded = true,
		});
	CHECK(bool(syncOffOk));
	CHECK(syncOffOk.purgedCount() == 2);
	CHECK(syncOffOk.dueSeen.empty());
	CHECK(syncOffOk.library.purged.contains(idPartial));
	CHECK(syncOffOk.library.purged.contains(idRelinkedDelete));

	PlaylistLibrary invalidLib;
	invalidLib.format = 0;
	const auto refusedInvalid = AutoPurgePlaylists(
		invalidLib,
		initialStampSeen,
		MakeCompleteSyncContext(highNow));
	CHECK(refusedInvalid.status == PlaylistAutoPurgeStatus::InvalidLibrary);
	CHECK(refusedInvalid.library == invalidLib);
	CHECK(refusedInvalid.dueSeen == initialStampSeen);

	PlaylistLibrary newerLib;
	newerLib.format = 2;
	const auto refusedNewer = AutoPurgePlaylists(
		newerLib,
		initialStampSeen,
		MakeCompleteSyncContext(highNow));
	CHECK(refusedNewer.status == PlaylistAutoPurgeStatus::NewerFormatLibrary);
	CHECK(refusedNewer.library == newerLib);
	CHECK(refusedNewer.dueSeen == initialStampSeen);

	PlaylistLibrary oversized;
	const auto idHuge = *FormatPlaylistId(QByteArray(int(kPlaylistIdEntropyBytes), '\x06'));
	oversized = CreatePlaylist(oversized, u"Big"_q, QByteArray(int(kPlaylistIdEntropyBytes), '\x06'), 10).library;
	const QString longText(96, u'm');
	auto &bigSongs = *oversized.playlists.at(idHuge).songs;
	for (uint64_t i = 1; i <= 90000; ++i) {
		bigSongs.emplace(1000000ULL + i, PlaylistSongRegister{
			PlaylistSongValue{ PlaylistSource{
				.peer = PeerToken{ PeerTokenKind::Chat, 1 },
				.msg = int32_t(i),
				.username = QString(),
				.alt = {},
				.meta = PlaylistSongMeta{ longText, longText, 180 },
				.since = 10,
			} },
			10,
		});
	}
	oversized.playlists.emplace(idPartial, Playlist{
		.title = std::nullopt,
		.deleted = PlaylistDeletedRegister{ true, 100 },
		.order = std::nullopt,
		.songs = std::nullopt,
	});
	const auto serializedOversized = SerializePlaylistLibrary(oversized);
	CHECK(serializedOversized);
	CHECK(serializedOversized.canonical.size() > kPlaylistLocalEditMaximumBytes);
	const auto purgedOversized = AutoPurgePlaylists(
		oversized,
		{},
		PlaylistAutoPurgeContext{
			.syncEnabled = false,
			.nowMs = 100 + kAutoPurgeRetentionMs,
			.serverRequestSucceeded = true,
		});
	CHECK(bool(purgedOversized));
	CHECK(purgedOversized.purgedIds == std::vector<QString>{ idPartial });
	CHECK(purgedOversized.library.purged.contains(idPartial));
	CHECK(purgedOversized.library.playlists.contains(idHuge));
}

} // namespace

int main() {
	TestQualifySong();
	TestPrepareSongMeta();
	TestSortPlaylistChatOrder();
	TestAutoPurgeDefaultsAndInvalidObservations();
	TestAutoPurgeSyncOnBoundariesAndGates();
	TestAutoPurgeResetPartialMaxStampSyncOffAndSize();

	if (Failures > 0) {
		std::printf("FAILED %d/%d checks\n", Failures, Checks);
		return 1;
	}
	std::printf("PASSED %d checks\n", Checks);
	return 0;
}
