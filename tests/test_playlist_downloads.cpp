#include "purple/purple_types.h"
#include "purple/purple_playlists_download_state.h"
#include "purple/purple_playlists_download_job.h"
#include <QtCore/QJsonDocument>
#include <iostream>

int main() {
    using namespace Purple;
    int count = 0, failures = 0;
    const auto check = [&](bool value, const char *name) {
        ++count;
        if (!value) { ++failures; std::cerr << name << '\n'; }
    };
    const auto id = u"pl-aaaaaaaaaaaaaaaa"_q;
    const auto token = QString(32, u'a');
    const auto ready = PlaylistDownloadStatus::Ready;
    auto local = DefaultPlaylistLocalState();
    local.preserved.insert(u"other"_q, QJsonObject{ {u"unknown"_q, 17} });
    auto state = PlaylistDownloadState();
    state.preserved.insert(u"future"_q, u"kept"_q);
    state.preferences[id] = PlaylistKeepDownloaded::Off;
    check(SetPlaylistDownloadState(local, state) == ready, "desktop write");
    auto desktop = local;
    check(ReadPlaylistDownloadState(local, PlaylistDownloadClient::Android).state.preferences.empty(), "android never adopts desktop");
    auto android = PlaylistDownloadState();
    android.preferences[id] = PlaylistKeepDownloaded::On;
    check(SetPlaylistDownloadState(local, android, PlaylistDownloadClient::Android) == ready, "android write");
    check(ReadPlaylistDownloadState(local).state == state, "android preserves desktop");
    check(ReadPlaylistDownloadState(local, PlaylistDownloadClient::Android).state == android, "android typed roundtrip");
    check(local.preserved.value(u"other"_q) == desktop.preserved.value(u"other"_q), "other clients preserved");
    auto bad = local;
    bad.preserved.insert(u"android_playlist_downloads"_q, QJsonObject{ {u"format"_q, 2} });
    auto before = bad;
    check(ReadPlaylistDownloadState(bad, PlaylistDownloadClient::Android).status == PlaylistDownloadStatus::Newer, "newer client state refuses");
    check(SetPlaylistDownloadState(bad, {}, PlaylistDownloadClient::Android) == PlaylistDownloadStatus::Newer && bad == before, "newer immutable");
    check(ReadPlaylistDownloadState(bad).state == state, "newer Android does not block Desktop");
    for (int value : {-1, 2, 123456}) {
        auto copy = local;
        check(!ReadPlaylistDownloadState(copy, PlaylistDownloadClient(value)), "invalid client refuses read");
        check(SetPlaylistDownloadState(copy, {}, PlaylistDownloadClient(value)) == PlaylistDownloadStatus::Invalid && copy == local, "invalid client refuses write");
    }
    auto intent = PlaylistOwnedDownloadIntent{ id, 55, u"mp3"_q, 123, {}, {}, QJsonObject{ {u"unknown_intent"_q, 9} } };
    auto apply = [&](PlaylistDownloadChange op, const QString &t, const PlaylistOwnedDownloadIntent &i) {
        return ApplyPlaylistDownloadChange(android, op, id, t, i);
    };
    check(apply(PlaylistDownloadChange::Reserve, token, intent) == ready, "reservation before creation");
    check(android.pending.at(token).identity.isEmpty(), "reservation has no identity");
    before = local;
    check(SetPlaylistDownloadState(local, android, PlaylistDownloadClient::Android) == ready, "intent stored");
    const auto encoded = SerializePlaylistLocalState(local, 1);
    check(bool(encoded), "local serializes");
    const auto reopened = ParsePlaylistLocalState(encoded.json, 1);
    check(bool(reopened) && ReadPlaylistDownloadState(reopened.state, PlaylistDownloadClient::Android).state == android, "restart intent roundtrip");
    const auto checkpoint = android;
    check(apply(PlaylistDownloadChange::Reserve, token, intent) == PlaylistDownloadStatus::Busy && android == checkpoint, "duplicate token cannot replace intent");
    check(apply(PlaylistDownloadChange::Reserve, QString(32, u'b'), intent) == PlaylistDownloadStatus::Busy && android == checkpoint, "one intent per document");
    check(apply(PlaylistDownloadChange::Complete, token, intent) == PlaylistDownloadStatus::Invalid && android == checkpoint, "cannot publish reservation");
    intent.identity = "12:34";
    check(apply(PlaylistDownloadChange::Checkpoint, token, intent) == ready, "identity checkpoint");
    auto wrong = intent;
    wrong.identity = "12:35";
    const auto owned = android;
    check(apply(PlaylistDownloadChange::Checkpoint, token, wrong) == PlaylistDownloadStatus::Changed && android == owned, "identity cannot change");
    wrong = intent; wrong.size++;
    check(apply(PlaylistDownloadChange::Checkpoint, token, wrong) == PlaylistDownloadStatus::Changed && android == owned, "size cannot change");
    intent.sha256 = QByteArray(64, 'a');
    check(apply(PlaylistDownloadChange::Checkpoint, token, intent) == ready, "complete digest checkpoint");
    const auto completed = android;
    wrong = intent; wrong.sha256 = QByteArray(64, 'b');
    check(apply(PlaylistDownloadChange::Complete, token, wrong) == PlaylistDownloadStatus::Changed && android == completed, "digest match required");
    check(apply(PlaylistDownloadChange::Checkpoint, token, wrong) == PlaylistDownloadStatus::Changed && android == completed, "complete digest immutable");
    check(apply(PlaylistDownloadChange::Complete, token, intent) == ready && android.pending.empty(), "publish inventory transition");
    check(android.files.at(55).preserved.value(u"unknown_intent"_q).toInt() == 9, "unknown intent carries through inventory");
    const auto kept = android;
    check(apply(PlaylistDownloadChange::ForgetFile, {}, wrong) == PlaylistDownloadStatus::Changed && android == kept, "wrong digest cannot forget file");
    check(apply(PlaylistDownloadChange::ForgetFile, {}, intent) == ready && android.files.empty(), "matching file can be forgotten after owned removal");
    for (const auto ext : {u"../mp3"_q, u"MP3"_q, u""_q, QString(13, u'a')}) {
        auto invalid = intent; invalid.identity.clear(); invalid.sha256.clear(); invalid.extension = ext;
        const auto prior = android;
        check(apply(PlaylistDownloadChange::Reserve, token, invalid) == PlaylistDownloadStatus::Invalid && android == prior, "invalid reservation transactional");
    }
    for (const auto identity : {QByteArray(":12"), QByteArray("1::2"), QByteArray("1:2:"), QByteArray("foreign"), QByteArray(257, '1')}) {
        auto invalid = intent; invalid.identity.clear(); invalid.sha256.clear();
        check(apply(PlaylistDownloadChange::Reserve, token, invalid) == ready, "fresh reservation for malformed identity");
        invalid.identity = identity;
        const auto prior = android;
        check(apply(PlaylistDownloadChange::Checkpoint, token, invalid) == PlaylistDownloadStatus::Invalid && android == prior, "malformed identity cannot checkpoint");
        invalid.identity.clear();
        check(apply(PlaylistDownloadChange::ForgetIntent, token, invalid) == ready, "explicit matching reservation release");
    }
    for (int n = 0; n < 9; ++n) {
        auto policy = PlaylistDownloadJobPolicy(n);
        std::vector<PlaylistDownloadTicket> oldTickets;
        while (!policy.progress().terminal) {
            const auto tickets = policy.admit();
            check(tickets.size() <= 2 && policy.progress().active <= 2, "shared two-active bound");
            check(policy.admit().empty(), "no third active admission");
            for (const auto ticket : tickets) {
                check(!policy.finish(ticket, PlaylistDownloadPhase(1234)), "invalid phase refused");
                check(policy.current(ticket), "invalid phase retains active row");
                check(policy.finish(ticket, ticket.index % 2 ? PlaylistDownloadPhase::Downloaded : PlaylistDownloadPhase::Busy), "complete admitted ticket");
                check(!policy.finish(ticket, PlaylistDownloadPhase::Downloaded), "duplicate delivery refused");
                oldTickets.push_back(ticket);
            }
        }
        check(policy.progress().total == std::size_t(n), "total remains exact");
        const auto retry = policy.retry();
        check(retry == (n > 0), "only retryable rows reopened");
        for (const auto ticket : oldTickets) check(!policy.current(ticket), "all old generation tickets revoked");
        const auto running = policy.admit();
        policy.cancel();
        check(policy.progress().terminal && policy.progress().cancelled && policy.progress().active == 0, "cancel terminal without deleting completed rows");
        check(policy.admit().empty() && !policy.retry(), "cancel never restarts");
        for (const auto ticket : running) check(!policy.finish(ticket, PlaylistDownloadPhase::Downloaded), "late completion refused");
    }
    std::cout << "PLAYLIST_DOWNLOAD_CORE_CHECKS=" << count << '\n';
    return failures ? 1 : 0;
}
