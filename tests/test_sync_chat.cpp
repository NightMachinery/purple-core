/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_config_payload.h"
#include "purple/purple_peer_token.h"
#include "purple/purple_sync_chat.h"
#include "purple/purple_types.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <random>

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

PeerToken Peer(const QString &text) {
	const auto result = ParsePeerToken(text);
	CHECK(result.has_value());
	return result.value_or(PeerToken());
}

SyncChatEligibilityResult Eligible(SyncChatKind kind = SyncChatKind::Self) {
	auto facts = SyncChatFacts();
	facts.kind = kind;
	facts.canSendFiles = true;
	facts.otherMembers = 0;
	facts.otherAdmins = 0;
	return SyncChatEligibility(facts, SyncChatBroadcastOwnership::Unselected);
}

SyncChatMark Mark(
		const QString &chat,
		int64_t date,
		int32_t id = 1) {
	auto result = SyncChatMark();
	result.location = { Peer(chat), id, date };
	result.status = SyncChatMarkStatus::Supported;
	result.eligibility = Eligible();
	result.freshlyRead = true;
	return result;
}

void PeerTokens() {
	for (const auto &text : {
		u"self"_q, u"user:1"_q, u"chat:42"_q, u"channel:42"_q,
		u"user:9223372036854775807"_q,
	}) {
		const auto peer = ParsePeerToken(text);
		CHECK(peer);
		CHECK(PeerTokenText(*peer) == text);
	}
	for (const auto &text : {
		u""_q, u"Self"_q, u" self"_q, u"self "_q, u"self:0"_q,
		u"user:0"_q, u"chat:-1"_q, u"channel:+1"_q, u"user:01"_q,
		u"user: 1"_q, u"chat:1\n"_q, u"channel:1/2"_q,
		u"user:9223372036854775808"_q, u"user:18446744073709551616"_q,
		u"user:١"_q, u"user:１"_q, u"private:1"_q, u"channel:"_q,
	}) {
		CHECK(!ParsePeerToken(text));
	}
	CHECK(ParsePeerToken(u"user:42"_q, 42) == PeerToken());
	CHECK(ParsePeerToken(u"chat:42"_q, 42) == Peer(u"chat:42"_q));
	CHECK(PeerTokenText(Peer(u"user:42"_q), 42) == u"self"_q);
	CHECK(!PeerTokenText({ PeerTokenKind::Self, 1 }));
	CHECK(!PeerTokenText({ PeerTokenKind::User, 0 }));
	CHECK(!PeerTokenText({ PeerTokenKind(99), 1 }));
	CHECK(!PeerTokenText({ PeerTokenKind::User, uint64_t(-1) }));
	CHECK(!ParsePeerToken(u"self"_q, uint64_t(-1)));
	CHECK(!ComparePeerTokens(PeerToken(), { PeerTokenKind::Chat, 0 }));
	CHECK(*ComparePeerTokens(Peer(u"user:10"_q), Peer(u"user:2"_q)) < 0);
	CHECK(*ComparePeerTokens(Peer(u"channel:2"_q), Peer(u"chat:1"_q)) < 0);
	CHECK(*ComparePeerTokens(Peer(u"user:42"_q), PeerToken(), 42) == 0);
	auto random = std::mt19937_64(0x4b4);
	for (auto i = 0; i != 3000; ++i) {
		const auto id = (random() & uint64_t(std::numeric_limits<int64_t>::max())) | 1;
		const auto peer = PeerToken{ PeerTokenKind(1 + i % 3), id };
		const auto text = PeerTokenText(peer);
		CHECK(text);
		CHECK(ParsePeerToken(*text) == peer);
		CHECK(*ComparePeerTokens(peer, peer) == 0);
		const auto other = PeerToken{ PeerTokenKind(1 + (i + 1) % 3), id };
		CHECK((*ComparePeerTokens(peer, other) < 0)
			== (*ComparePeerTokens(other, peer) > 0));
	}
}

void MarkGrammar() {
	struct Case {
		QString text;
		SyncChatMarkStatus status;
		QString version;
	};
	for (const auto &test : std::vector<Case>{
		{ u"#purplechatsync"_q, SyncChatMarkStatus::Supported, u"1"_q },
		{ u" #PURPLECHATSYNC V1 \r\n\t\n"_q,
			SyncChatMarkStatus::Supported, u"1"_q },
		{ u"arbitrary preface\n\n#purplechatsync v0001"_q,
			SyncChatMarkStatus::Supported, u"1"_q },
		{ u"#purplechatsync v2"_q, SyncChatMarkStatus::NewerVersion, u"2"_q },
		{ u"#purplechatsync v999999999999999999999999999999"_q,
			SyncChatMarkStatus::NewerVersion, u"999999999999999999999999999999"_q },
		{ u"#purplechatsync v00020"_q, SyncChatMarkStatus::NewerVersion, u"20"_q },
		{ u""_q, SyncChatMarkStatus::NotMark, {} },
		{ u"\n \t"_q, SyncChatMarkStatus::NotMark, {} },
		{ u"mention #purplechatsync"_q, SyncChatMarkStatus::NotMark, {} },
		{ u"#purplechatsync\nother line"_q, SyncChatMarkStatus::NotMark, {} },
		{ u"#purplechatsync v0"_q, SyncChatMarkStatus::NotMark, {} },
		{ u"#purplechatsync v000"_q, SyncChatMarkStatus::NotMark, {} },
		{ u"#purplechatsync v-1"_q, SyncChatMarkStatus::NotMark, {} },
		{ u"#purplechatsync v+1"_q, SyncChatMarkStatus::NotMark, {} },
		{ u"#purplechatsync v1.0"_q, SyncChatMarkStatus::NotMark, {} },
		{ u"#purplechatsync v١"_q, SyncChatMarkStatus::NotMark, {} },
		{ u"#purplechatsync v"_q, SyncChatMarkStatus::NotMark, {} },
		{ u"#purplechatsync  v1"_q, SyncChatMarkStatus::NotMark, {} },
		{ u"#purplechatsync\tv1"_q, SyncChatMarkStatus::NotMark, {} },
		{ u"#purplechatsync v1 more"_q, SyncChatMarkStatus::NotMark, {} },
		{ u"#purplechatsync channel:123"_q, SyncChatMarkStatus::NotMark, {} },
		{ u"#purplechatsync v1 channel:123"_q, SyncChatMarkStatus::NotMark, {} },
		{ u"#purplechatsyncer"_q, SyncChatMarkStatus::NotMark, {} },
	}) {
		const auto result = ParseSyncChatMark(
			test.text, false, SyncChatMessageOwnership::Own);
		CHECK(result.status == test.status);
		CHECK(result.version == test.version);
		CHECK(ParseSyncChatMark(test.text, true, SyncChatMessageOwnership::Own)
			.status == SyncChatMarkStatus::NotMark);
		for (const auto owner : {
			SyncChatMessageOwnership::Unknown,
			SyncChatMessageOwnership::Other,
		}) {
			CHECK(ParseSyncChatMark(test.text, false, owner).status
				== SyncChatMarkStatus::NotMark);
		}
	}
	for (auto version = 1; version != 1001; ++version) {
		const auto result = ParseSyncChatMark(
			u"#PurpleChatSync V%1"_q.arg(version), false,
			SyncChatMessageOwnership::Own);
		CHECK(result.status == (version == 1
			? SyncChatMarkStatus::Supported : SyncChatMarkStatus::NewerVersion));
		CHECK(result.version == QString::number(version));
	}
}

void Eligibility() {
	const auto unselected = SyncChatBroadcastOwnership::Unselected;
	auto facts = SyncChatFacts();
	CHECK(!SyncChatEligibility(facts, unselected));
	facts.kind = SyncChatKind::Self;
	facts.canSendFiles = true;
	facts.otherMembers = 0;
	facts.otherAdmins = 0;
	struct RefusalCase {
		bool SyncChatFacts::*field;
		SyncChatRefusal refusal;
	};
	const auto cases = std::vector<RefusalCase>{
		{ &SyncChatFacts::publicUsername, SyncChatRefusal::Public },
		{ &SyncChatFacts::protectedContent, SyncChatRefusal::Protected },
		{ &SyncChatFacts::forum, SyncChatRefusal::TopicRouting },
		{ &SyncChatFacts::linkedDiscussion, SyncChatRefusal::Discussion },
		{ &SyncChatFacts::autoDelete, SyncChatRefusal::AutoDelete },
		{ &SyncChatFacts::paidMessages, SyncChatRefusal::Paid },
		{ &SyncChatFacts::anonymousAdmin, SyncChatRefusal::SendAsOther },
		{ &SyncChatFacts::postsAsOther, SyncChatRefusal::SendAsOther },
		{ &SyncChatFacts::migrated, SyncChatRefusal::Migrated },
		{ &SyncChatFacts::slowMode, SyncChatRefusal::SlowMode },
	};
	for (const auto kind : {
		SyncChatKind::Self, SyncChatKind::Private, SyncChatKind::Bot,
		SyncChatKind::BasicGroup, SyncChatKind::Supergroup,
	}) {
		facts.kind = kind;
		CHECK(SyncChatEligibility(facts, unselected));
		for (const auto &test : cases) {
			auto bad = facts;
			bad.*test.field = true;
			CHECK(SyncChatEligibility(bad, unselected).refusal == test.refusal);
		}
		for (auto mask = 0; mask != (1 << cases.size()); ++mask) {
			auto combination = facts;
			for (auto i = size_t(0); i != cases.size(); ++i) {
				combination.*cases[i].field = (mask & (1 << i)) != 0;
			}
			CHECK(bool(SyncChatEligibility(combination, unselected)) == (mask == 0));
		}
	}
	facts.kind = SyncChatKind::Broadcast;
	for (const auto creator : { false, true }) {
		for (const auto soleAdmin : { false, true }) {
			facts.creator = creator;
			facts.soleAdmin = soleAdmin;
			CHECK(SyncChatEligibility(facts, unselected).refusal
				== SyncChatRefusal::BroadcastOwnershipUnselected);
			CHECK(SyncChatEligibility(facts,
				SyncChatBroadcastOwnership::OutgoingVerified));
			CHECK(bool(SyncChatEligibility(facts,
				SyncChatBroadcastOwnership::CreatorAndSoleAdmin))
				== (creator && soleAdmin));
		}
	}
	facts.kind = SyncChatKind::Self;
	facts.canSendFiles = false;
	CHECK(SyncChatEligibility(facts, unselected).refusal
		== SyncChatRefusal::CannotSendFiles);
	facts.canSendFiles = true;
	facts.slowMode = true;
	facts.admin = true;
	CHECK(SyncChatEligibility(facts, unselected));
	facts.slowMode = false;
	facts.kind = SyncChatKind::Secret;
	CHECK(SyncChatEligibility(facts, unselected).refusal == SyncChatRefusal::Secret);
	facts.kind = SyncChatKind::ChannelDirectMessages;
	CHECK(SyncChatEligibility(facts, unselected).refusal
		== SyncChatRefusal::TopicRouting);
	CHECK(Eligible(SyncChatKind::Private).readers.privateRecipient);
	CHECK(Eligible(SyncChatKind::Bot).readers.botOwner);
	CHECK(!Eligible().readers.warningRequired);
	CHECK(!Eligible(SyncChatKind::Supergroup).readers.warningRequired);
	facts.kind = SyncChatKind::Supergroup;
	facts.otherMembers.reset();
	CHECK(SyncChatEligibility(facts, unselected).readers.populationUnknown);
	CHECK(SyncChatEligibility(facts, unselected).readers.warningRequired);
	facts.otherMembers = 2;
	CHECK(SyncChatEligibility(facts, unselected).readers.otherMembers == 2);
	CHECK(SyncChatEligibility(facts, unselected).readers.warningRequired);
	facts.otherMembers = 0;
	facts.otherAdmins = 1;
	CHECK(SyncChatEligibility(facts, unselected).readers.warningRequired);
}

bool SameResolution(const SyncChatResolution &a, const SyncChatResolution &b) {
	return a.action == b.action && a.cache == b.cache && a.readChat == b.readChat
		&& a.winner == b.winner && a.source == b.source
		&& a.olderAlternative == b.olderAlternative && a.readers == b.readers
		&& a.needsReaderWarning == b.needsReaderWarning
		&& a.postingHeld == b.postingHeld && a.retryAfterSeconds == b.retryAfterSeconds;
}

void Resolution() {
	auto input = SyncChatResolveInput();
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Unset);
	for (const auto hint : {
		SyncChatSettingsHint::NotRun, SyncChatSettingsHint::Unknown,
		SyncChatSettingsHint::Absent,
	}) {
		input.settingsHint = hint;
		CHECK(ResolveSyncChat(input).action == SyncChatAction::Unset);
		CHECK(!ResolveSyncChat(input).readChat);
	}
	input.settingsHint = SyncChatSettingsHint::Found;
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Use);
	CHECK(ResolveSyncChat(input).cache == SyncChatCache());
	input.settingsHint = SyncChatSettingsHint::NotRun;
	input.settingsBound = true;
	input.cachedEligibility = Eligible();
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Use);
	CHECK(ResolveSyncChat(input).cache == SyncChatCache());
	input.marks = { Mark(u"channel:4"_q, 100) };
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Follow);
	CHECK(ResolveSyncChat(input).postingHeld);
	CHECK(ResolveSyncChat(input).readChat == PeerToken());
	input.marks = { Mark(u"self"_q, 100) };
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Use);
	CHECK(ResolveSyncChat(input).cache == input.marks.front().location);
	input.settingsBound = false;
	input.cache = Mark(u"channel:8"_q, 100).location;
	input.marks = { Mark(u"user:3"_q, 99), Mark(u"channel:9"_q, 98) };
	for (const auto read : { SyncChatDirectRead::Unknown, SyncChatDirectRead::Valid }) {
		input.cacheRead = read;
		CHECK(ResolveSyncChat(input).action == SyncChatAction::Use);
		CHECK(ResolveSyncChat(input).winner == input.cache);
		CHECK(ResolveSyncChat(input).postingHeld == (read == SyncChatDirectRead::Unknown));
	}
	for (const auto read : {
		SyncChatDirectRead::MarkGone, SyncChatDirectRead::ChatGone,
		SyncChatDirectRead::Migrated,
	}) {
		input.cacheRead = read;
		for (auto retry = 0; retry != 4; ++retry) {
			input.searchRetriesCompleted = retry;
			const auto result = ResolveSyncChat(input);
			CHECK(result.action == (retry == 3
				? SyncChatAction::Lost : SyncChatAction::Searching));
			CHECK(result.retryAfterSeconds == (std::array{ 30, 120, 600, 0 }[retry]));
			CHECK(result.olderAlternative == Peer(u"user:3"_q));
			CHECK(result.postingHeld);
			CHECK(result.cache == input.cache);
		}
	}
	input.searchRetriesCompleted = 0;
	input.marks.clear();
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Searching);
	input.marks = { Mark(u"channel:8"_q, 101) };
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Use);
	input.marks = { Mark(u"channel:7"_q, 101) };
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Follow);
	CHECK(ResolveSyncChat(input).postingHeld);
	input.marks.front().freshlyRead = false;
	CHECK(ResolveSyncChat(input).action == SyncChatAction::VerifyMark);
	CHECK(ResolveSyncChat(input).cache == input.cache);
	input.marks.front().freshlyRead = true;
	input.marks.front().status = SyncChatMarkStatus::NewerVersion;
	CHECK(ResolveSyncChat(input).action == SyncChatAction::NewerMark);
	CHECK(ResolveSyncChat(input).postingHeld);
	CHECK(ResolveSyncChat(input).readChat == input.cache->chat);
	input.marks.front().eligibility.refusal = SyncChatRefusal::Public;
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Searching);
	input.cache.reset();
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Unset);
	input.marks = { Mark(u"channel:10"_q, 101), Mark(u"chat:4"_q, 99) };
	input.marks.front().records = SyncChatRecordPresence::Empty;
	input.marks.back().records = SyncChatRecordPresence::Present;
	input.marks.back().readable = true;
	const auto fill = ResolveSyncChat(input);
	CHECK(fill.action == SyncChatAction::Fill);
	CHECK(fill.source == Peer(u"chat:4"_q));
	CHECK(fill.postingHeld);
	CHECK(!fill.cache);
	CHECK(!fill.readChat);
	for (const auto presence : {
		SyncChatRecordPresence::Unknown, SyncChatRecordPresence::Empty,
	}) {
		input.marks.back().records = presence;
		CHECK(ResolveSyncChat(input).action == SyncChatAction::Fill);
	}
	input.marks.back().readable = false;
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Use);
	input.settingsHint = SyncChatSettingsHint::Found;
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Fill);
	CHECK(ResolveSyncChat(input).source == PeerToken());
	input.marks.front().location.chat = PeerToken();
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Use);
	input.settingsHint = SyncChatSettingsHint::NotRun;
	input.marks = { Mark(u"user:2"_q, 100), Mark(u"user:10"_q, 100) };
	CHECK(ResolveSyncChat(input).winner == input.marks.front().location);
	input.marks = { Mark(u"chat:4"_q, 100, 2), Mark(u"chat:4"_q, 100, 3) };
	CHECK(ResolveSyncChat(input).winner == input.marks.back().location);
	input.marks.front().eligibility = Eligible(SyncChatKind::Private);
	input.marks.back().eligibility = Eligible(SyncChatKind::Private);
	CHECK(ResolveSyncChat(input).needsReaderWarning);
	CHECK(ResolveSyncChat(input).postingHeld);
	input.readerWarningAccepted = true;
	CHECK(!ResolveSyncChat(input).postingHeld);
	input.selfUserId = 4;
	input.marks = { Mark(u"user:4"_q, 100) };
	CHECK(ResolveSyncChat(input).readChat == PeerToken());
	input.marks.push_back(input.marks.front());
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Use);
	input.marks.back().location.markDate = 200;
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Invalid);
	input.marks = { Mark(u"self"_q, 100) };
	input.marks.front().location.markId = 0;
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Invalid);
	input.marks.clear();
	input.searchRetriesCompleted = -1;
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Invalid);
	input = SyncChatResolveInput();
	input.settingsChat = Peer(u"channel:999"_q);
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Unset);
	input.settingsBound = true;
	CHECK(ResolveSyncChat(input).cache == (SyncChatCache{ input.settingsChat, 0, 0 }));
	CHECK(ResolveSyncChat(input).readChat == input.settingsChat);
	input.cacheRead = SyncChatDirectRead::ChatGone;
	input.settingsHint = SyncChatSettingsHint::Found;
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Searching);
	CHECK(ResolveSyncChat(input).readChat == input.settingsChat);
	input.cacheRead = SyncChatDirectRead::Valid;
	input.cachedEligibility = Eligible(SyncChatKind::Private);
	CHECK(ResolveSyncChat(input).needsReaderWarning);
	CHECK(ResolveSyncChat(input).postingHeld);
	input.readerWarningAccepted = true;
	CHECK(!ResolveSyncChat(input).postingHeld);
	input.cachedEligibility.refusal = SyncChatRefusal::Protected;
	CHECK(ResolveSyncChat(input).postingHeld);
	input.cache = SyncChatCache{ PeerToken(), 0, 1 };
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Invalid);
	input.cache.reset();
	input.cacheRead = SyncChatDirectRead(99);
	CHECK(ResolveSyncChat(input).action == SyncChatAction::Invalid);
}

void WinnerInventoryReviewRegression() {
	auto input = SyncChatResolveInput();
	input.marks = { Mark(u"chat:20"_q, 200), Mark(u"chat:10"_q, 100) };
	input.marks[0].eligibility = Eligible(SyncChatKind::BasicGroup);
	input.marks[1].eligibility = Eligible(SyncChatKind::BasicGroup);
	input.marks[0].records = SyncChatRecordPresence::Unknown;
	input.marks[1].records = SyncChatRecordPresence::Present;
	input.marks[1].readable = true;
	const auto first = ResolveSyncChat(input);
	CHECK(first.action == SyncChatAction::ReadWinnerInventory);
	CHECK(first.postingHeld && !first.cache);
	CHECK(first.winner == input.marks[0].location);
	CHECK(first.source == input.marks[1].location.chat);
	CHECK(!first.readChat);
	input.cache = first.cache;
	input.cacheRead = SyncChatDirectRead::Valid;
	input.cachedEligibility = Eligible();
	input.marks[0].records = SyncChatRecordPresence::Empty;
	const auto second = ResolveSyncChat(input);
	CHECK(second.action == SyncChatAction::Fill);
	CHECK(second.postingHeld && !second.cache);
	CHECK(second.source == input.marks[1].location.chat);
	input.cache.reset();
	const auto fresh = ResolveSyncChat(input);
	CHECK(SameResolution(second, fresh));
}

void WinnerInventoryTransitions() {
	struct SourceCase {
		bool marked = false;
		SyncChatSettingsHint hint = SyncChatSettingsHint::NotRun;
	};
	for (const auto source : std::vector<SourceCase>{
		{ true, SyncChatSettingsHint::NotRun },
		{ false, SyncChatSettingsHint::Found },
		{ true, SyncChatSettingsHint::Found },
	}) {
		for (const auto sourceRecords : {
			SyncChatRecordPresence::Unknown,
			SyncChatRecordPresence::Empty,
			SyncChatRecordPresence::Present,
		}) {
			auto input = SyncChatResolveInput();
			input.marks = { Mark(u"chat:20"_q, 200) };
			input.marks[0].eligibility = Eligible(SyncChatKind::BasicGroup);
			if (source.marked) {
				auto mark = Mark(u"chat:10"_q, 100);
				mark.eligibility = Eligible(SyncChatKind::BasicGroup);
				mark.records = sourceRecords;
				mark.readable = true;
				input.marks.push_back(mark);
			}
			input.settingsHint = source.hint;
			input.readerWarningAccepted = true;
			const auto expectedSource = source.marked
				? Peer(u"chat:10"_q) : PeerToken();
			const auto first = ResolveSyncChat(input);
			CHECK(first.action == SyncChatAction::ReadWinnerInventory);
			CHECK(first.winner == input.marks[0].location);
			CHECK(first.source == expectedSource);
			CHECK(first.postingHeld);
			CHECK(!first.cache && !first.readChat);
			input.cache = first.cache;
			input.cacheRead = SyncChatDirectRead::Valid;
			input.cachedEligibility = Eligible(SyncChatKind::BasicGroup);
			CHECK(SameResolution(ResolveSyncChat(input), first));
			std::reverse(input.marks.begin(), input.marks.end());
			CHECK(SameResolution(ResolveSyncChat(input), first));
			std::reverse(input.marks.begin(), input.marks.end());
			for (const auto winnerRecords : {
				SyncChatRecordPresence::Empty, SyncChatRecordPresence::Present,
			}) {
				auto complete = input;
				complete.marks[0].records = winnerRecords;
				const auto resolved = ResolveSyncChat(complete);
				CHECK(resolved.winner == complete.marks[0].location);
				if (winnerRecords == SyncChatRecordPresence::Empty) {
					CHECK(resolved.action == SyncChatAction::Fill);
					CHECK(resolved.postingHeld);
					CHECK(!resolved.cache && !resolved.readChat);
					CHECK(resolved.source == expectedSource);
				} else {
					CHECK(resolved.action == SyncChatAction::Use);
					CHECK(!resolved.postingHeld);
					CHECK(resolved.cache == complete.marks[0].location);
					CHECK(resolved.readChat == complete.marks[0].location.chat);
					CHECK(!resolved.source);
					complete.cache = resolved.cache;
					CHECK(SameResolution(ResolveSyncChat(complete), resolved));
				}
			}
			input.marks[0].freshlyRead = false;
			CHECK(ResolveSyncChat(input).action == SyncChatAction::VerifyMark);
			CHECK(!ResolveSyncChat(input).cache);
			CHECK(ResolveSyncChat(input).postingHeld);
		}
	}
}

void WinnerWithoutFillSource() {
	for (const auto hint : {
		SyncChatSettingsHint::NotRun,
		SyncChatSettingsHint::Unknown,
		SyncChatSettingsHint::Absent,
	}) {
		for (auto sourceKind = 0; sourceKind != 4; ++sourceKind) {
			auto input = SyncChatResolveInput();
			input.marks = { Mark(u"chat:20"_q, 200) };
			input.marks[0].eligibility = Eligible(SyncChatKind::BasicGroup);
			input.settingsHint = hint;
			if (sourceKind) {
				auto older = Mark(u"chat:10"_q, 100);
				older.eligibility = Eligible(SyncChatKind::BasicGroup);
				older.records = SyncChatRecordPresence::Present;
				older.readable = true;
				if (sourceKind == 1) {
					older.location.chat = input.marks[0].location.chat;
					older.location.markId = 2;
				} else if (sourceKind == 2) {
					older.eligibility.refusal = SyncChatRefusal::Public;
				} else {
					older.readable = false;
				}
				input.marks.push_back(older);
			}
			for (const auto presence : {
				SyncChatRecordPresence::Unknown,
				SyncChatRecordPresence::Empty,
				SyncChatRecordPresence::Present,
			}) {
				input.marks[0].records = presence;
				const auto resolved = ResolveSyncChat(input);
				CHECK(resolved.action == SyncChatAction::Use);
				CHECK(!resolved.postingHeld);
				CHECK(resolved.cache == input.marks[0].location);
				CHECK(resolved.readChat == input.marks[0].location.chat);
				CHECK(!resolved.source);
			}
		}
	}
	auto self = SyncChatResolveInput();
	self.marks = { Mark(u"self"_q, 200) };
	self.settingsHint = SyncChatSettingsHint::Found;
	CHECK(ResolveSyncChat(self).action == SyncChatAction::Use);
	CHECK(!ResolveSyncChat(self).postingHeld);
}

void ResolutionProperties() {
	auto random = std::mt19937_64(0x5b4);
	for (auto trial = 0; trial != 1000; ++trial) {
		auto input = SyncChatResolveInput();
		for (auto i = 0; i != 8; ++i) {
			auto mark = Mark(u"channel:%1"_q.arg(i + 1), 1 + random() % 100, i + 1);
			mark.records = SyncChatRecordPresence(random() % 3);
			mark.readable = random() % 2;
			mark.freshlyRead = random() % 2;
			if (random() % 5 == 0) {
				mark.eligibility.refusal = SyncChatRefusal::Protected;
			}
			if (random() % 5 == 0) {
				mark.status = SyncChatMarkStatus::NewerVersion;
			}
			input.marks.push_back(mark);
		}
		if (random() % 2) {
			input.cache = Mark(u"channel:9"_q, 1 + random() % 100).location;
		}
		input.cacheRead = SyncChatDirectRead(random() % 5);
		input.settingsHint = SyncChatSettingsHint(random() % 4);
		input.searchRetriesCompleted = random() % 4;
		const auto expected = ResolveSyncChat(input);
		for (auto order = 0; order != 6; ++order) {
			std::shuffle(input.marks.begin(), input.marks.end(), random);
			CHECK(SameResolution(ResolveSyncChat(input), expected));
		}
		if (expected.action == SyncChatAction::Follow
			|| expected.action == SyncChatAction::Fill
			|| expected.action == SyncChatAction::NewerMark
			|| expected.action == SyncChatAction::ReadWinnerInventory) {
			CHECK(expected.postingHeld);
		}
		if (expected.action == SyncChatAction::Follow) {
			CHECK(expected.winner->markDate >= input.cache->markDate);
		}
		input.cache = Mark(u"user:999"_q, 101).location;
		input.cacheRead = SyncChatDirectRead::Unknown;
		for (auto &mark : input.marks) {
			mark.status = SyncChatMarkStatus::Supported;
		}
		const auto unknown = ResolveSyncChat(input);
		CHECK(unknown.action == SyncChatAction::Use);
		CHECK(unknown.winner == input.cache);
		CHECK(unknown.postingHeld);
	}
}

SyncLocalState LocalState() {
	auto state = SyncLocalState();
	state.install = *FormatSyncInstallId(QByteArray(16, 'i'));
	state.space = *FormatSyncSpaceId(QByteArray(16, 's'));
	state.createdDevice = u"test-device"_q;
	state.bindingToken = *FormatSyncBindingToken(QByteArray(16, 't'));
	state.preserved.insert(u"future"_q, QJsonArray{ 1, u"retained"_q });
	return state;
}

void StateAndMoves() {
	auto state = LocalState();
	const auto original = SerializeSyncLocalState(state);
	CHECK(original);
	CHECK(!QJsonDocument::fromJson(original.canonical).object().contains(u"chat"_q));
	const auto implicit = ParseSyncLocalState(original.canonical);
	CHECK(implicit);
	CHECK(implicit.state.version == 1);
	CHECK(implicit.state.chat == PeerToken());
	CHECK(SerializeSyncLocalState(implicit.state).canonical == original.canonical);
	const auto same = MoveSyncChat(implicit.state, PeerToken(), false);
	CHECK(same);
	CHECK(!same.changed);
	CHECK(same.state.version == 1);
	CHECK(SerializeSyncLocalState(same.state).canonical == original.canonical);
	const auto alias = MoveSyncChat(implicit.state, Peer(u"user:42"_q), false, 42);
	CHECK(alias);
	CHECK(!alias.changed);
	CHECK(alias.state.chat == PeerToken());
	CHECK(alias.state.version == 1);

	const auto record = BuildConfigRecord({
		.text = QByteArray("version = 1\n"),
		.space = state.space,
		.install = state.install,
		.device = state.createdDevice,
		.platform = u"test"_q,
		.app = u"Harness"_q,
		.seq = 1,
		.at = 1700000000,
	});
	CHECK(record);
	const auto reserved = ReserveSyncSeq(state, SyncLocalStream::Config, record.payloadHash);
	CHECK(reserved);
	const auto issued = AppendIssuedConfigRecord(reserved.state, record.canonical);
	CHECK(issued);
	const auto confirmed = ConfirmSyncReadBack(issued.state, state.createdDevice,
		SyncLocalStream::Config, { OwnRecordObservationKind::Present, 1, record.payloadHash });
	CHECK(confirmed.changed);
	state = RecordConfirmedOwnConfigMessage(confirmed.state, 77, record.canonical).state;
	CHECK(state.ownMessages.size() == 1);
	state.configData.base = record.version.key;
	state.configData.baseLineage = record.version.lineage;
	state.configData.seenSeq[state.install] = 1;
	const auto retained = SerializeSyncLocalState(state);
	CHECK(retained);
	state = ParseSyncLocalState(retained.canonical).state;
	const auto moved = MoveSyncChat(state, Peer(u"channel:42"_q), false);
	CHECK(moved);
	CHECK(moved.changed);
	CHECK(moved.state.version == 2);
	CHECK(moved.state.chat == Peer(u"channel:42"_q));
	CHECK(moved.state.ownMessages.empty());
	CHECK(!moved.state.preserved.contains(u"own_messages"_q));
	CHECK(moved.state.install == state.install);
	CHECK(moved.state.space == state.space);
	CHECK(moved.state.createdDevice == state.createdDevice);
	CHECK(moved.state.bindingToken == state.bindingToken);
	CHECK(moved.state.config.seq == 1);
	CHECK(moved.state.config.confirmedSeq == 1);
	CHECK(moved.state.config.issuedRecords.size() == 1);
	CHECK(moved.state.config.issuedRecords.front().recordHash
		== state.config.issuedRecords.front().recordHash);
	CHECK(moved.state.configData.base == state.configData.base);
	CHECK(moved.state.configData.baseLineage == state.configData.baseLineage);
	CHECK(moved.state.configData.seenSeq == state.configData.seenSeq);
	CHECK(moved.state.preserved.value(u"future"_q) == state.preserved.value(u"future"_q));
	CHECK(SerializeSyncLocalState(state).canonical == retained.canonical);
	const auto saved = SerializeSyncLocalState(moved.state);
	CHECK(saved);
	const auto parsed = ParseSyncLocalState(saved.canonical);
	CHECK(parsed);
	CHECK(parsed.state.version == 2);
	CHECK(parsed.state.chat == moved.state.chat);
	CHECK(parsed.state.ownMessages.empty());
	CHECK(SerializeSyncLocalState(parsed.state).canonical == saved.canonical);
	CHECK(CheckSyncAccountBinding(parsed.state, state.bindingToken.toLatin1())
		== SyncAccountBindingVerdict::Bound);
	const auto adopted = AdoptIssuedOwnConfigMessage(parsed.state, 99, record.canonical);
	CHECK(adopted);
	CHECK(adopted.state.ownMessages.size() == 1);
	CHECK(adopted.state.ownMessages.front().messageId == 99);
	CHECK(CheckOwnConfigMessageDeletion(adopted.state, 77, record.canonical).error
		== SyncOwnMessageError::NotFound);
	const auto back = MoveSyncChat(adopted.state, PeerToken(), false);
	CHECK(back);
	CHECK(back.state.version == 2);
	CHECK(back.state.chat == PeerToken());
	CHECK(back.state.ownMessages.empty());
	CHECK(ParseSyncLocalState(SerializeSyncLocalState(back.state).canonical).state.chat
		== PeerToken());
	CHECK(MoveSyncChat(state, Peer(u"channel:42"_q), true).error
		== SyncChatMoveError::PendingStage);
	CHECK(MoveSyncChat(reserved.state, Peer(u"channel:42"_q), false).error
		== SyncChatMoveError::PendingStage);
	CHECK(MoveSyncChat(ReserveSyncSeq(state, SyncLocalStream::Library,
		QString(64, u'a')).state, Peer(u"channel:42"_q), false).error
		== SyncChatMoveError::PendingStage);
	auto pendingKey = state;
	pendingKey.configData.pending = record.version.key;
	CHECK(MoveSyncChat(pendingKey, Peer(u"channel:42"_q), false).error
		== SyncChatMoveError::PendingStage);
	CHECK(MoveSyncChat(state, { PeerTokenKind::User, 0 }, false).error
		== SyncChatMoveError::InvalidChat);
	auto invalidState = state;
	invalidState.install.clear();
	CHECK(MoveSyncChat(invalidState, Peer(u"channel:42"_q), false).error
		== SyncChatMoveError::InvalidState);
	CHECK(SerializeSyncLocalState(MoveSyncChat(state, PeerToken(), true).state).canonical
		== retained.canonical);

	for (const auto &chat : { u"self"_q, u"user:2"_q, u"chat:2"_q, u"channel:2"_q }) {
		auto explicitState = state;
		explicitState.version = 2;
		explicitState.chat = Peer(chat);
		const auto bytes = SerializeSyncLocalState(explicitState);
		CHECK(bytes);
		const auto object = QJsonDocument::fromJson(bytes.canonical).object();
		CHECK(object.value(u"version"_q).toInt() == 2);
		CHECK(object.value(u"chat"_q).toString() == chat);
		CHECK(ParseSyncLocalState(bytes.canonical).state.chat == explicitState.chat);
		CHECK(SerializeSyncLocalState(ParseSyncLocalState(bytes.canonical).state).canonical
			== bytes.canonical);
		auto downgrade = object;
		downgrade.insert(u"version"_q, 1);
		CHECK(ParseSyncLocalState(QJsonDocument(downgrade).toJson()).error
			== SyncLocalError::InvalidChatToken);
	}
	auto object = QJsonDocument::fromJson(saved.canonical).object();
	for (const auto &value : std::vector<QJsonValue>{
		QJsonValue(), 0, true, QJsonArray(), QJsonObject(),
		u""_q, u"user:0"_q, u"user:01"_q, u"channel:-1"_q, u" self"_q,
	}) {
		auto invalid = object;
		invalid.insert(u"chat"_q, value);
		CHECK(ParseSyncLocalState(QJsonDocument(invalid).toJson()).error
			== SyncLocalError::InvalidChatToken);
	}
	object.remove(u"chat"_q);
	CHECK(ParseSyncLocalState(QJsonDocument(object).toJson()).error
		== SyncLocalError::InvalidChatToken);
	object.insert(u"version"_q, 3);
	CHECK(ParseSyncLocalState(QJsonDocument(object).toJson()).status
		== SyncLocalStatus::NewerVersion);
	auto future = state;
	future.version = 3;
	CHECK(SerializeSyncLocalState(future).status == SyncLocalStatus::NewerVersion);
	auto wrongV1 = state;
	wrongV1.chat = Peer(u"channel:42"_q);
	CHECK(SerializeSyncLocalState(wrongV1).error == SyncLocalError::InvalidChatToken);
	auto wrongV2 = parsed.state;
	wrongV2.chat = { PeerTokenKind::Chat, 0 };
	CHECK(SerializeSyncLocalState(wrongV2).error == SyncLocalError::InvalidChatToken);
	wrongV2 = parsed.state;
	wrongV2.version = 1;
	CHECK(SerializeSyncLocalState(wrongV2).error == SyncLocalError::InvalidChatToken);

	auto random = std::mt19937_64(0x6b4);
	auto current = state;
	for (auto move = 0; move != 1000; ++move) {
		const auto target = PeerToken{ PeerTokenKind(1 + random() % 3), 1 + random() % 9999 };
		const auto transition = MoveSyncChat(current, target, false);
		CHECK(transition);
		const auto serialized = SerializeSyncLocalState(transition.state);
		CHECK(serialized);
		const auto restored = ParseSyncLocalState(serialized.canonical);
		CHECK(restored);
		CHECK(restored.state.version == 2);
		CHECK(restored.state.chat == target);
		CHECK(restored.state.ownMessages.empty());
		CHECK(restored.state.config.seq == state.config.seq);
		CHECK(restored.state.bindingToken == state.bindingToken);
		CHECK(SerializeSyncLocalState(restored.state).canonical == serialized.canonical);
		current = restored.state;
	}
}

}

int main() {
	PeerTokens();
	MarkGrammar();
	Eligibility();
	Resolution();
	WinnerInventoryReviewRegression();
	WinnerInventoryTransitions();
	WinnerWithoutFillSource();
	ResolutionProperties();
	StateAndMoves();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
