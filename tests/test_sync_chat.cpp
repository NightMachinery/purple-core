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

SyncChatMessageFacts OwnMessage(SyncChatKind kind) {
	auto facts = SyncChatMessageFacts();
	facts.kind = kind;
	facts.regular = true;
	facts.forwarded = false;
	facts.viaBot = false;
	facts.scheduled = false;
	facts.hasTtl = false;
	facts.sentAsOther = false;
	facts.out = true;
	facts.sender = Peer(u"user:42"_q);
	facts.selfUserId = 42;
	facts.broadcastOwnership = SyncChatBroadcastOwnership::CreatorAndSoleAdmin;
	facts.creator = true;
	facts.soleAdmin = true;
	return facts;
}

void CheckOwnership(
		const SyncChatMessageFacts &facts,
		SyncChatMessageOwnership expected) {
	const auto ownership = GetSyncChatMessageOwnership(facts);
	CHECK(ownership == expected);
	CHECK(IsOwnSyncChatMessage(facts) == (expected == SyncChatMessageOwnership::Own));
}

void MessageOwnershipGuards() {
	using Ownership = SyncChatMessageOwnership;
	CheckOwnership({}, Ownership::Unknown);
	const auto kinds = std::array{
		SyncChatKind::Self, SyncChatKind::Private, SyncChatKind::Bot,
		SyncChatKind::BasicGroup, SyncChatKind::Supergroup, SyncChatKind::Broadcast,
	};
	struct Guard {
		std::optional<bool> SyncChatMessageFacts::*field;
		bool safe;
	};
	const auto guards = std::array{
		Guard{ &SyncChatMessageFacts::regular, true },
		Guard{ &SyncChatMessageFacts::forwarded, false },
		Guard{ &SyncChatMessageFacts::viaBot, false },
		Guard{ &SyncChatMessageFacts::scheduled, false },
		Guard{ &SyncChatMessageFacts::hasTtl, false },
		Guard{ &SyncChatMessageFacts::sentAsOther, false },
	};
	for (const auto kind : kinds) {
		const auto own = OwnMessage(kind);
		CheckOwnership(own, Ownership::Own);
		for (const auto &guard : guards) {
			auto refused = own;
			refused.*guard.field = !guard.safe;
			CheckOwnership(refused, Ownership::Other);
			refused.*guard.field = std::nullopt;
			CheckOwnership(refused, Ownership::Unknown);
			for (const auto &other : guards) {
				if (other.field == guard.field) {
					continue;
				}
				auto excluded = refused;
				excluded.*other.field = !other.safe;
				CheckOwnership(excluded, Ownership::Other);
			}
		}
		for (const auto out : std::array<std::optional<bool>, 3>{
			true, false, std::nullopt,
		}) {
			auto facts = own;
			facts.out = out;
			const auto expected = kind == SyncChatKind::Self || out == true
				? Ownership::Own : out == false ? Ownership::Other : Ownership::Unknown;
			CheckOwnership(facts, expected);
		}
	}
	for (const auto kind : { SyncChatKind::Secret, SyncChatKind::ChannelDirectMessages }) {
		CheckOwnership(OwnMessage(kind), Ownership::Other);
	}
	for (const auto kind : { SyncChatKind::Unknown, SyncChatKind(-1), SyncChatKind(99) }) {
		CheckOwnership(OwnMessage(kind), Ownership::Unknown);
	}
}

void MessageOwnershipNormalization() {
	using Ownership = SyncChatMessageOwnership;
	const auto senders = std::vector<std::optional<PeerToken>>{
		std::nullopt,
		PeerToken(),
		Peer(u"user:42"_q),
		Peer(u"user:43"_q),
		Peer(u"chat:42"_q),
		Peer(u"channel:42"_q),
	};
	for (const auto kind : { SyncChatKind::Self, SyncChatKind::Private, SyncChatKind::Bot }) {
		for (const auto &sender : senders) {
			auto facts = OwnMessage(kind);
			facts.sender = sender;
			facts.selfUserId = 0;
			facts.creator.reset();
			facts.soleAdmin.reset();
			facts.broadcastOwnership = SyncChatBroadcastOwnership::Unselected;
			CheckOwnership(facts, Ownership::Own);
		}
	}
	auto self = OwnMessage(SyncChatKind::Self);
	self.sender.reset();
	self.selfUserId = 0;
	self.out = false;
	CheckOwnership(self, Ownership::Own);
	self.out.reset();
	CheckOwnership(self, Ownership::Own);
	self.forwarded = true;
	CheckOwnership(self, Ownership::Other);
	for (const auto kind : { SyncChatKind::Private, SyncChatKind::Bot }) {
		auto facts = OwnMessage(kind);
		facts.out = false;
		CheckOwnership(facts, Ownership::Other);
		facts.out.reset();
		CheckOwnership(facts, Ownership::Unknown);
	}
	struct SenderCase {
		std::optional<PeerToken> sender;
		uint64_t selfUserId;
		SyncChatMessageOwnership ownership;
	};
	const auto maximumId = uint64_t(std::numeric_limits<int64_t>::max());
	const auto cases = std::vector<SenderCase>{
		{ std::nullopt, 42, Ownership::Unknown },
		{ PeerToken(), 42, Ownership::Own },
		{ PeerToken(), 0, Ownership::Own },
		{ Peer(u"user:42"_q), 42, Ownership::Own },
		{ Peer(u"user:43"_q), 42, Ownership::Other },
		{ Peer(u"user:42"_q), 0, Ownership::Unknown },
		{ Peer(u"chat:42"_q), 42, Ownership::Other },
		{ Peer(u"channel:42"_q), 42, Ownership::Other },
		{ Peer(u"chat:42"_q), 0, Ownership::Other },
		{ Peer(u"channel:42"_q), 0, Ownership::Other },
		{ PeerToken{ PeerTokenKind::User, maximumId }, maximumId, Ownership::Own },
		{ PeerToken{ PeerTokenKind::User, maximumId }, 42, Ownership::Other },
		{ PeerToken{ PeerTokenKind::Self, 42 }, 42, Ownership::Unknown },
		{ PeerToken{ PeerTokenKind::User, 0 }, 42, Ownership::Unknown },
		{ PeerToken{ PeerTokenKind::Chat, 0 }, 42, Ownership::Unknown },
		{ PeerToken{ PeerTokenKind::Channel, 0 }, 42, Ownership::Unknown },
		{ PeerToken{ PeerTokenKind::User, maximumId + 1 }, 42, Ownership::Unknown },
		{ PeerToken{ PeerTokenKind(-1), 42 }, 42, Ownership::Unknown },
		{ PeerToken{ PeerTokenKind(99), 42 }, 42, Ownership::Unknown },
		{ PeerToken(), maximumId + 1, Ownership::Unknown },
		{ Peer(u"user:42"_q), maximumId + 1, Ownership::Unknown },
	};
	for (const auto kind : { SyncChatKind::BasicGroup, SyncChatKind::Supergroup }) {
		for (const auto &test : cases) {
			auto facts = OwnMessage(kind);
			facts.sender = test.sender;
			facts.selfUserId = test.selfUserId;
			CheckOwnership(facts, test.ownership);
			if (test.ownership == Ownership::Own) {
				facts.out = false;
				CheckOwnership(facts, Ownership::Other);
				facts.out.reset();
				CheckOwnership(facts, Ownership::Unknown);
			}
		}
	}
}

void MessageOwnershipBroadcast() {
	using Ownership = SyncChatMessageOwnership;
	const auto options = std::array<std::optional<bool>, 3>{ true, false, std::nullopt };
	for (const auto mode : {
		SyncChatBroadcastOwnership::Unselected,
		SyncChatBroadcastOwnership::OutgoingVerified,
		SyncChatBroadcastOwnership::CreatorAndSoleAdmin,
		SyncChatBroadcastOwnership(-1), SyncChatBroadcastOwnership(99),
	}) {
		for (const auto out : options) {
			for (const auto creator : options) {
				for (const auto sole : options) {
					auto facts = OwnMessage(SyncChatKind::Broadcast);
					facts.broadcastOwnership = mode;
					facts.out = out;
					facts.creator = creator;
					facts.soleAdmin = sole;
					facts.sender.reset();
					facts.selfUserId = 0;
					const auto expected = out == false ? Ownership::Other
						: !out || mode != SyncChatBroadcastOwnership::CreatorAndSoleAdmin
						? Ownership::Unknown
						: creator == false || sole == false ? Ownership::Other
						: !creator || !sole ? Ownership::Unknown : Ownership::Own;
					CheckOwnership(facts, expected);
				}
			}
		}
	}
	auto facts = OwnMessage(SyncChatKind::Broadcast);
	facts.sender = Peer(u"channel:42"_q);
	CheckOwnership(facts, Ownership::Own);
	facts.sentAsOther = true;
	CheckOwnership(facts, Ownership::Other);
	facts = OwnMessage(SyncChatKind::Broadcast);
	facts.broadcastOwnership = SyncChatBroadcastOwnership::OutgoingVerified;
	CheckOwnership(facts, Ownership::Unknown);
	auto eligibility = SyncChatFacts();
	eligibility.kind = SyncChatKind::Broadcast;
	eligibility.canSendFiles = true;
	CHECK(SyncChatEligibility(eligibility, SyncChatBroadcastOwnership::OutgoingVerified));
	CHECK(!IsOwnSyncChatMessage(facts));
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
	// Who else can read the chat is the magic policy's call, not a refusal.
	facts.kind = SyncChatKind::Supergroup;
	facts.otherMembers.reset();
	CHECK(SyncChatEligibility(facts, unselected));
	facts.otherMembers = 2;
	facts.otherAdmins = 1;
	CHECK(SyncChatEligibility(facts, unselected));
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
	MessageOwnershipGuards();
	MessageOwnershipNormalization();
	MessageOwnershipBroadcast();
	Eligibility();
	StateAndMoves();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
