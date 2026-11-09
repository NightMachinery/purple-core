#include "purple/purple_media_reply.h"
#include "purple/purple_settings.h"
#include "purple/purple_splice.h"
#include "purple/purple_media_reply_splice.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <iostream>

using namespace Purple;
namespace {
int checks = 0;
int failures = 0;
void Check(bool value, const char *what) {
	++checks;
	if (!value) { ++failures; std::cerr << what << '\n'; }
}
MediaReplyMessage Message() {
	return { u"prod:42"_q, {PeerTokenKind::User, 17},
		ChatKind::Private, 101, 0, 1001, MediaReplyMedia::Voice,
		MediaReplyOrigin::LiveNew, false, true, false };
}
}
int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	auto settings = Settings();
	auto message = Message();
	Check(!PlanMediaReply(settings, message, message.account, 1000, 1), "default off");
	Check(settings.mediaReply.text == u"@TranscribeThisAI"_q, "default text");
	for (auto kind = 0; kind != 4; ++kind) {
		message.kind = ChatKind(kind);
		message.peer.kind = kind == 1 ? PeerTokenKind::Chat
			: kind == 2 ? PeerTokenKind::Channel : PeerTokenKind::User;
		for (auto mode = 0; mode != 4; ++mode) {
			for (auto &row : settings.mediaReply.defaults) row.voice = MediaReplyMode(mode);
			for (auto media = 1; media != 3; ++media) {
				message.media = MediaReplyMedia(media);
				for (auto own : {false, true}) {
					message.own = own;
					const auto allowed = mode == 3 || mode == (own ? 1 : 2);
					Check(bool(PlanMediaReply(settings, message, message.account, 1000, 1)) == allowed,
						"direction/type/media matrix");
				}
			}
		}
	}
	message = Message();
	settings.mediaReply.defaults[0] = {MediaReplyMode::Both, true, MediaReplyMode::Off};
	settings.lists = { {u"first"_q, {}, {17}, {}}, {u"second"_q, {}, {17}, {}} };
	settings.mediaReply.sets = {
		{u"first"_q, u"first"_q, {{PeerTokenKind::User, 17}}, {MediaReplyMode::Off, {}, {}}},
		{u"second"_q, u"second"_q, {{PeerTokenKind::User, 17}}, {MediaReplyMode::Both, false, MediaReplyMode::Both}} };
	auto resolved = ResolveMediaReply(settings, message.peer, ChatKind::Private);
	Check(resolved.voice == MediaReplyMode::Off && resolved.round == MediaReplyMode::Off,
		"first set explicit off and resolved inheritance");
	settings.mediaReply.sets[0].row = {};
	resolved = ResolveMediaReply(settings, message.peer, ChatKind::Private);
	Check(resolved.voice == MediaReplyMode::Both && resolved.round == MediaReplyMode::Both,
		"first matching empty set inherits defaults and captures");
	settings.mediaReply.sets[0].row = {{}, false, MediaReplyMode::Off};
	resolved = ResolveMediaReply(settings, message.peer, ChatKind::Private);
	Check(resolved.voice == MediaReplyMode::Both && !resolved.roundFollow
		&& resolved.round == MediaReplyMode::Off, "unchecked Off is distinct");
	settings.mediaReply.sets.clear();
	auto planned = PlanMediaReply(settings, message, message.account, 1000, 1);
	Check(bool(planned), "eligible plan");
	for (const auto origin : {MediaReplyOrigin::History, MediaReplyOrigin::OutgoingAck}) {
		auto bad = message; bad.origin = origin;
		Check(!PlanMediaReply(settings, bad, bad.account, 1000, 1), "history or incoming ack rejected");
	}
	for (auto failure = 0; failure != 9; ++failure) {
		auto bad = message;
		if (failure == 0) bad.media = MediaReplyMedia::Other;
		if (failure == 1) bad.serverConfirmed = false;
		if (failure == 2) bad.scheduled = true;
		if (failure == 3) bad.message = -1;
		if (failure == 4) bad.date = 1000;
		if (failure == 5) bad.account = u"prod:43"_q;
		if (failure == 6) bad.peer.id = 0;
		if (failure == 7) bad.topic = -1;
		if (failure == 8) bad.peer.kind = PeerTokenKind::Self;
		Check(!PlanMediaReply(settings, bad, message.account, 1000, 1), "negative admission");
	}
	Check(!PlanMediaReply(settings, message, message.account, 0, 1), "no activation cutoff");
	Check(!PlanMediaReply(settings, message, message.account, 1000, 0), "no revision");
	Check(MediaReplyStillCurrent(settings, *planned, message.account, 1000, 1), "current action");
	Check(!MediaReplyStillCurrent(settings, *planned, message.account, 1000, 2), "revocation revision");
	Check(!MediaReplyStillCurrent(settings, *planned, u"prod:43"_q, 1000, 1), "account replacement");
	settings.mediaReply.text = u"different"_q;
	const auto changed = PlanMediaReply(settings, message, message.account, 1000, 2);
	Check(changed && changed->randomId == planned->randomId, "text and revision do not evade dedup");
	Check(!MediaReplyStillCurrent(settings, *planned, message.account, 1000, 1), "text revocation");
	auto journal = MediaReplyJournal{message.account, 1000, 1, {}};
	Check(ReserveMediaReply(journal, *planned), "reserve before send");
	Check(!ReserveMediaReply(journal, *planned), "same-client repeated update");
	const auto loaded = DecodeMediaReplyJournal(EncodeMediaReplyJournal(journal), message.account);
	Check(loaded && *loaded == journal, "restart preserves frozen action");
	auto restarted = *loaded;
	Check(!ReserveMediaReply(restarted, *planned), "restart does not resend uncertain action");
	Check(!DecodeMediaReplyJournal(EncodeMediaReplyJournal(journal), u"prod:43"_q), "journal account isolation");
	Check(!DecodeMediaReplyJournal("{}", message.account), "corrupt journal fail closed");
	auto secondClient = MediaReplyJournal{message.account, 1000, 1, {}};
	Check(ReserveMediaReply(secondClient, *planned)
		&& secondClient.reserved.front().randomId == journal.reserved.front().randomId,
		"independent clients send same server dedup identity");
	auto another = *planned; another.message.peer.kind = PeerTokenKind::Channel; another.message.kind = ChatKind::Channel;
	Check(ReserveMediaReply(journal, another), "peer type remains distinct in local journal");
	Check(MediaReplyRandomId(1) != MediaReplyRandomId(2)
		&& MediaReplyRandomId(2147483647) == UINT64_C(0x505552417fffffff)
		&& !MediaReplyRandomId(0), "action ID boundaries");
	const auto text = uR"(# retained
[media_reply.private]
voice = "received" # choice
round_follow_voice = false
round = "off"
[media_reply.sets.first]
list = "first"
peers = ["user:17"]
voice = "off"
[lists.first]
members = [17]
)"_q;
	const auto parsed = ParseSettings(text, u"fixture"_q);
	Check(parsed.ok() && parsed.warnings.empty(), "parser accepts complete media config");
	Check(parsed.settings.mediaReply.sets.size() == 1, "named set parsed");
	Check(ResolveMediaReply(parsed.settings, message.peer, ChatKind::Private).voice == MediaReplyMode::Off,
		"parsed selected override");
	const auto splice = SetMediaReplyValue(text, u"fixture"_q, u"private"_q,
		u"voice"_q, u"both"_q);
	Check(splice.ok() && splice.text.contains(u"# retained"_q)
		&& splice.text.contains(u"# choice"_q), "surgical writer retains comments");
	const auto invalid = ParseSettings(uR"([media_reply.private]
voice = "typo"
)"_q, u"fixture"_q);
	Check(invalid.ok() && !invalid.warnings.empty()
		&& invalid.settings.mediaReply.defaults[0].voice == MediaReplyMode::Off, "bad mode fails off");
	Check(SettingsScope(u"media_reply.private"_q, u"voice"_q) == SettingScope::PerAccount,
		"per-account scope");
	const auto separate = LayerSettings(text, u"owner"_q, QString(), u"other"_q);
	Check(separate.effective.settings.mediaReply.defaults[0].voice == std::nullopt,
		"a new account never inherits another account auto-enable");
	const auto same = LayerSettings(text, u"owner"_q, text, u"owner"_q);
	Check(same.effective.settings.mediaReply.defaults[0].voice == MediaReplyMode::Received,
		"settings owner keeps its own policy");
	const auto inherited = SetMediaReplyValue(text, u"fixture"_q, u"sets.first"_q, u"voice"_q, QJsonValue());
	Check(inherited.ok() && !ParseSettings(inherited.text, u"fixture"_q).settings.mediaReply.sets.front().row.voice,
		"override reset restores inheritance");
	const auto named = SetMediaReplyValue(text, u"fixture"_q, u"sets.second"_q, u"peers"_q,
		QJsonArray{u"channel:17"_q, u"chat:17"_q});
	Check(named.ok() && ParseSettings(named.text, u"fixture"_q).settings.mediaReply.sets.size() == 2,
		"typed set writes preserve equal IDs of different peer types");
	Check(!SetMediaReplyValue(text, u"fixture"_q, u"sets.invalid\nname"_q, u"voice"_q, u"both"_q).ok(),
		"set names cannot inject a table header");
	const auto types = ParseSettings(named.text, u"fixture"_q).settings;
	Check(ResolveMediaReply(types, {PeerTokenKind::Chat, 17}, ChatKind::Group).set == u"second"_q,
		"typed chat does not collide with user of equal bare ID");
	const auto unicode = SetMediaReplyValue(text, u"fixture"_q, u"sets.دوستان و خانواده.1"_q,
		u"voice"_q, u"both"_q);
	const auto unicodeParsed = ParseSettings(unicode.text, u"fixture"_q);
	Check(unicode.ok() && unicodeParsed.ok() && unicodeParsed.settings.mediaReply.sets.back().name
		== u"دوستان و خانواده.1"_q, "human set names are safely quoted");
	const auto unicodeAgain = SetMediaReplyValue(unicode.text, u"fixture"_q, u"sets.دوستان و خانواده.1"_q,
		u"voice"_q, u"off"_q);
	Check(unicodeAgain.ok() && ParseSettings(unicodeAgain.text, u"fixture"_q).settings.mediaReply.sets.back().row.voice
		== MediaReplyMode::Off, "Unicode table edit uses source spans");
	std::cout << "MEDIA_REPLY_CHECKS=" << checks << " failures=" << failures << '\n';
	return failures ? 1 : 0;
}
