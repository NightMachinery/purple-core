#pragma once

#include "purple/purple_types.h"
#include "purple/purple_peer_token.h"

#include <array>
#include <optional>
#include <vector>

namespace Purple {

struct Settings;
enum class ChatKind : uchar;
enum class MediaReplyMode : uchar { Off, Own, Received, Both };
enum class MediaReplyMedia : uchar { Other, Voice, Round };
enum class MediaReplyOrigin : uchar { History, LiveNew, OutgoingAck };

struct MediaReplyRow {
	std::optional<MediaReplyMode> voice;
	std::optional<bool> roundFollow;
	std::optional<MediaReplyMode> round;
	bool operator==(const MediaReplyRow &) const = default;
};

struct MediaReplySet {
	QString name;
	QString list;
	std::vector<PeerToken> peers;
	MediaReplyRow row;
	bool operator==(const MediaReplySet &) const = default;
};

struct MediaReplySettings {
	QString text = u"@TranscribeThisAI"_q;
	std::array<MediaReplyRow, 3> defaults;
	std::vector<MediaReplySet> sets;
	bool operator==(const MediaReplySettings &) const = default;
};

struct MediaReplyResolved {
	MediaReplyMode voice = MediaReplyMode::Off;
	MediaReplyMode round = MediaReplyMode::Off;
	bool roundFollow = true;
	QString set;
	bool operator==(const MediaReplyResolved &) const = default;
};

struct MediaReplyMessage {
	QString account;
	PeerToken peer;
	ChatKind kind = ChatKind(0);
	int message = 0;
	int topic = 0;
	int64 date = 0;
	MediaReplyMedia media = MediaReplyMedia::Other;
	MediaReplyOrigin origin = MediaReplyOrigin::History;
	bool own = false;
	bool serverConfirmed = false;
	bool scheduled = false;
	bool operator==(const MediaReplyMessage &) const = default;
};

struct MediaReplyAction {
	MediaReplyMessage message;
	QString text;
	uint64_t randomId = 0;
	uint64_t revision = 0;
	bool operator==(const MediaReplyAction &) const = default;
};

[[nodiscard]] std::optional<MediaReplyMode> ParseMediaReplyMode(
	const QString &text);
[[nodiscard]] QString MediaReplyModeName(MediaReplyMode mode);
[[nodiscard]] MediaReplyResolved ResolveMediaReply(
	const Settings &settings, const PeerToken &peer, ChatKind kind);
[[nodiscard]] std::optional<MediaReplyAction> PlanMediaReply(
	const Settings &settings,
	const MediaReplyMessage &message,
	const QString &account,
	int64 activationCutoff,
	uint64_t revision);
[[nodiscard]] bool MediaReplyStillCurrent(
	const Settings &settings,
	const MediaReplyAction &action,
	const QString &account,
	int64 activationCutoff,
	uint64_t revision);
[[nodiscard]] uint64_t MediaReplyRandomId(int serverMessageId);

struct MediaReplyJournal {
	QString account;
	int64 cutoff = 0;
	uint64_t revision = 0;
	std::vector<MediaReplyAction> reserved;
	bool operator==(const MediaReplyJournal &) const = default;
};

[[nodiscard]] bool ReserveMediaReply(
	MediaReplyJournal &journal, const MediaReplyAction &action);
[[nodiscard]] QByteArray EncodeMediaReplyJournal(const MediaReplyJournal &journal);
[[nodiscard]] std::optional<MediaReplyJournal> DecodeMediaReplyJournal(
	const QByteArray &bytes, const QString &account);

} // namespace Purple
