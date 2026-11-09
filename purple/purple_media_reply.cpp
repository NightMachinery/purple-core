#include "purple/purple_media_reply.h"

#include "purple/purple_settings.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <algorithm>

namespace Purple {
namespace {

constexpr auto kJournalLimit = 4096;

int KindIndex(ChatKind kind) {
	return kind == ChatKind::Group ? 1 : kind == ChatKind::Channel ? 2 : 0;
}

bool Allows(MediaReplyMode mode, bool own) {
	return mode == MediaReplyMode::Both
		|| mode == (own ? MediaReplyMode::Own : MediaReplyMode::Received);
}

bool ValidMessage(const MediaReplyMessage &message) {
	const auto peerKindMatches = (message.peer.kind == PeerTokenKind::User
		&& (message.kind == ChatKind::Private || message.kind == ChatKind::Bot))
		|| (message.peer.kind == PeerTokenKind::Chat && message.kind == ChatKind::Group)
		|| (message.peer.kind == PeerTokenKind::Channel
			&& (message.kind == ChatKind::Group || message.kind == ChatKind::Channel));
	return peerKindMatches && !message.account.isEmpty()
		&& message.peer.kind != PeerTokenKind::Self
		&& message.peer.id > 0
		&& message.message > 0
		&& message.topic >= 0
		&& message.serverConfirmed
		&& !message.scheduled
		&& (message.media == MediaReplyMedia::Voice
			|| message.media == MediaReplyMedia::Round)
		&& (message.origin == MediaReplyOrigin::LiveNew
			|| (message.origin == MediaReplyOrigin::OutgoingAck && message.own));
}

} // namespace

std::optional<MediaReplyMode> ParseMediaReplyMode(const QString &text) {
	if (text == u"off"_q) return MediaReplyMode::Off;
	if (text == u"own"_q) return MediaReplyMode::Own;
	if (text == u"received"_q) return MediaReplyMode::Received;
	if (text == u"both"_q) return MediaReplyMode::Both;
	return std::nullopt;
}

QString MediaReplyModeName(MediaReplyMode mode) {
	switch (mode) {
	case MediaReplyMode::Off: return u"off"_q;
	case MediaReplyMode::Own: return u"own"_q;
	case MediaReplyMode::Received: return u"received"_q;
	case MediaReplyMode::Both: return u"both"_q;
	}
	return u"off"_q;
}

MediaReplyResolved ResolveMediaReply(
		const Settings &settings, const PeerToken &peer, ChatKind kind) {
	const auto &policy = settings.mediaReply;
	const auto &defaults = policy.defaults[KindIndex(kind)];
	auto row = defaults;
	auto result = MediaReplyResolved();
	for (const auto &set : policy.sets) {
		if (!std::count(set.peers.begin(), set.peers.end(), peer)) {
			continue;
		}
		if (set.row.voice) row.voice = set.row.voice;
		if (set.row.roundFollow) row.roundFollow = set.row.roundFollow;
		if (set.row.round) row.round = set.row.round;
		result.set = set.name;
		break;
	}
	result.voice = row.voice.value_or(MediaReplyMode::Off);
	result.roundFollow = row.roundFollow.value_or(true);
	result.round = result.roundFollow
		? result.voice
		: row.round.value_or(MediaReplyMode::Off);
	return result;
}

uint64_t MediaReplyRandomId(int serverMessageId) {
	return serverMessageId > 0
		? UINT64_C(0x5055524100000000) | uint32_t(serverMessageId)
		: 0;
}

std::optional<MediaReplyAction> PlanMediaReply(
		const Settings &settings,
		const MediaReplyMessage &message,
		const QString &account,
		int64 activationCutoff,
		uint64_t revision) {
	if (message.account != account || !revision || activationCutoff <= 0
		|| message.date <= activationCutoff || !ValidMessage(message)
		|| settings.mediaReply.text.trimmed().isEmpty()
		|| settings.mediaReply.text.size() > 4096) {
		return std::nullopt;
	}
	const auto resolved = ResolveMediaReply(settings, message.peer, message.kind);
	const auto mode = message.media == MediaReplyMedia::Voice
		? resolved.voice : resolved.round;
	if (!Allows(mode, message.own)) {
		return std::nullopt;
	}
	return MediaReplyAction{
		message, settings.mediaReply.text, MediaReplyRandomId(message.message), revision };
}

bool MediaReplyStillCurrent(
		const Settings &settings,
		const MediaReplyAction &action,
		const QString &account,
		int64 activationCutoff,
		uint64_t revision) {
	const auto planned = PlanMediaReply(
		settings, action.message, account, activationCutoff, revision);
	return planned && *planned == action;
}

bool ReserveMediaReply(MediaReplyJournal &journal, const MediaReplyAction &action) {
	if (journal.account != action.message.account || journal.revision != action.revision
		|| action.message.date <= journal.cutoff || !ValidMessage(action.message)
		|| action.randomId != MediaReplyRandomId(action.message.message)
		|| journal.reserved.size() >= kJournalLimit) {
		return false;
	}
	for (const auto &previous : journal.reserved) {
		if (previous.message.peer == action.message.peer
			&& previous.message.message == action.message.message) {
			return false;
		}
	}
	journal.reserved.push_back(action);
	return true;
}

QByteArray EncodeMediaReplyJournal(const MediaReplyJournal &journal) {
	auto entries = QJsonArray();
	for (const auto &action : journal.reserved) {
		const auto &m = action.message;
		entries.push_back(QJsonObject{
			{u"peer"_q, PeerTokenText(m.peer).value_or(QString())},
			{u"id"_q, m.message}, {u"topic"_q, m.topic},
			{u"date"_q, QString::number(m.date)},
			{u"kind"_q, int(m.kind)}, {u"media"_q, int(m.media)},
			{u"origin"_q, int(m.origin)}, {u"own"_q, m.own},
			{u"text"_q, action.text},
			{u"revision"_q, QString::number(action.revision)} });
	}
	return QJsonDocument(QJsonObject{
		{u"version"_q, 1}, {u"account"_q, journal.account},
		{u"cutoff"_q, QString::number(journal.cutoff)},
		{u"revision"_q, QString::number(journal.revision)},
		{u"reserved"_q, entries} }).toJson(QJsonDocument::Compact);
}

std::optional<MediaReplyJournal> DecodeMediaReplyJournal(
		const QByteArray &bytes, const QString &account) {
	if (bytes.size() > 32 * 1024 * 1024) return std::nullopt;
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return std::nullopt;
	}
	const auto root = document.object();
	auto result = MediaReplyJournal();
	result.account = root[u"account"_q].toString();
	result.cutoff = root[u"cutoff"_q].toString().toLongLong();
	result.revision = root[u"revision"_q].toString().toULongLong();
	if (root[u"version"_q].toInt() != 1 || result.account != account
		|| account.isEmpty() || result.cutoff <= 0 || !result.revision
		|| !root[u"reserved"_q].isArray()) return std::nullopt;
	const auto entries = root[u"reserved"_q].toArray();
	if (entries.size() > kJournalLimit) return std::nullopt;
	for (const auto value : entries) {
		if (!value.isObject()) return std::nullopt;
		const auto entry = value.toObject();
		const auto peer = ParsePeerToken(entry[u"peer"_q].toString());
		if (!peer) return std::nullopt;
		auto action = MediaReplyAction();
		auto &m = action.message;
		m.account = account;
		m.peer = *peer;
		m.message = entry[u"id"_q].toInt();
		m.topic = entry[u"topic"_q].toInt(-1);
		m.date = entry[u"date"_q].toString().toLongLong();
		const auto kind = entry[u"kind"_q].toInt(-1);
		const auto media = entry[u"media"_q].toInt(-1);
		const auto origin = entry[u"origin"_q].toInt(-1);
		if (kind < 0 || kind > int(ChatKind::Bot)
			|| media < 1 || media > 2 || origin < 1 || origin > 2
			|| !entry[u"own"_q].isBool()) return std::nullopt;
		m.kind = ChatKind(kind);
		m.media = MediaReplyMedia(media);
		m.origin = MediaReplyOrigin(origin);
		m.own = entry[u"own"_q].toBool();
		m.serverConfirmed = true;
		action.text = entry[u"text"_q].toString();
		action.revision = entry[u"revision"_q].toString().toULongLong();
		action.randomId = MediaReplyRandomId(m.message);
		if (action.text.isEmpty() || action.text.size() > 4096
			|| !action.revision || !ValidMessage(m)) return std::nullopt;
		for (const auto &previous : result.reserved) {
			if (previous.message.peer == m.peer && previous.message.message == m.message) {
				return std::nullopt;
			}
		}
		result.reserved.push_back(action);
	}
	return result;
}

} // namespace Purple
