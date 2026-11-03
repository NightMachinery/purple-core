/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_chat_magic.h"
#include "purple/purple_config_payload.h"
#include "purple/purple_sync_local_state.h"
#include "purple/purple_types.h"

#include <cstdio>

using namespace Purple;

namespace {
auto Checks = 0;
auto Failures = 0;
const char *Section = "";
void Report(bool condition, const char *expression, int line) {
	++Checks;
	if (!condition) { ++Failures; std::printf("FAIL %s:%d %s\n", Section, line, expression); std::fflush(stdout); }
}
#define CHECK(condition) Report(bool(condition), #condition, __LINE__)

constexpr auto Now = int64_t(1800000000);

MagicChatCandidate Magic(uint64_t id, bool counted = true) {
	auto result = MagicChatCandidate();
	result.channelId = id;
	result.title = QString::fromLatin1(kMagicSyncChatTitle);
	result.megagroup = true;
	result.facts.kind = SyncChatKind::Supergroup;
	result.facts.canSendFiles = true;
	result.facts.creator = true;
	result.facts.admin = true;
	result.facts.soleAdmin = true;
	if (counted) {
		result.facts.otherMembers = 0;
		result.facts.otherAdmins = 0;
	}
	return result;
}

PeerToken Channel(uint64_t id) {
	return PeerToken{ PeerTokenKind::Channel, id };
}

SyncChatBinding Bound(uint64_t id, int64_t verifiedAt = Now - 60) {
	auto result = SyncChatBinding();
	result.mode = SyncChatBindingMode::Magic;
	result.chat = Channel(id);
	result.verifiedAt = verifiedAt;
	return result;
}

void TestTitle() {
	Section = "exact title";
	CHECK(IsMagicSyncChatTitle(u"Magic Purple Sync"_q));
	CHECK(!IsMagicSyncChatTitle(u"magic purple sync"_q));
	CHECK(!IsMagicSyncChatTitle(u"Magic Purple Sync "_q));
	CHECK(!IsMagicSyncChatTitle(u"Purple sync"_q));
}

void TestCheck() {
	Section = "eligibility";
	CHECK(CheckMagicChat(Magic(5)).verdict == MagicChatVerdict::Eligible);
	CHECK(CheckMagicChat(Magic(5, false)).verdict == MagicChatVerdict::Unverified);
	const auto expect = [](MagicChatCandidate chat, SyncChatProblem problem) {
		const auto check = CheckMagicChat(chat);
		return check.verdict == MagicChatVerdict::Ineligible && check.problem == problem;
	};
	auto chat = Magic(5); chat.member = false; CHECK(expect(chat, SyncChatProblem::ChatLeft));
	chat = Magic(5); chat.megagroup = false; CHECK(expect(chat, SyncChatProblem::NotMegagroup));
	chat = Magic(5); chat.gigagroup = true; CHECK(expect(chat, SyncChatProblem::NotMegagroup));
	chat = Magic(5); chat.facts.kind = SyncChatKind::Broadcast; CHECK(expect(chat, SyncChatProblem::NotMegagroup));
	chat = Magic(5); chat.facts.publicUsername = true; CHECK(expect(chat, SyncChatProblem::PublicUsername));
	chat = Magic(5); chat.facts.creator = false; CHECK(expect(chat, SyncChatProblem::NotCreator));
	chat = Magic(5); chat.facts.otherMembers = 1; CHECK(expect(chat, SyncChatProblem::ExtraMember));
	chat = Magic(5); chat.facts.otherAdmins = 1; CHECK(expect(chat, SyncChatProblem::ExtraAdmin));
	chat = Magic(5, false); chat.facts.otherMembers = 2; CHECK(expect(chat, SyncChatProblem::ExtraMember));
	chat = Magic(5); chat.facts.canSendFiles = false; CHECK(expect(chat, SyncChatProblem::CannotPost));
	chat = Magic(5); chat.facts.protectedContent = true; CHECK(expect(chat, SyncChatProblem::Restricted));
	CHECK(CheckMagicChat(chat).refusal == SyncChatRefusal::Protected);
	chat = Magic(5); chat.facts.forum = true; CHECK(expect(chat, SyncChatProblem::Restricted));
	chat = Magic(5); chat.facts.autoDelete = true; CHECK(expect(chat, SyncChatProblem::Restricted));
	chat = Magic(5); chat.facts.linkedDiscussion = true; CHECK(expect(chat, SyncChatProblem::Restricted));
	chat = Magic(5); chat.facts.migrated = true; CHECK(expect(chat, SyncChatProblem::Restricted));
	chat = Magic(5); chat.facts.anonymousAdmin = true; CHECK(expect(chat, SyncChatProblem::Restricted));
	chat = Magic(5); chat.title = u"Renamed"_q; CHECK(CheckMagicChat(chat).verdict == MagicChatVerdict::Eligible);
}

void TestPick() {
	Section = "oldest eligible pick";
	CHECK(!PickMagicChat({}));
	CHECK(PickMagicChat({ Magic(9), Magic(7), Magic(8) }) == 7);
	auto extra = Magic(3); extra.facts.otherMembers = 1;
	CHECK(PickMagicChat({ Magic(9), extra }) == 9);
	CHECK(PickMagicChat({ Magic(9), Magic(4, false) }) == 4);
	CHECK(PickMagicChat({ Magic(9), Magic(0) }) == 9);
	CHECK(PickMagicChat({ extra }) == std::nullopt);
	CHECK(PickMagicChat({ Magic(9), Magic(7) }) == PickMagicChat({ Magic(7), Magic(9) }));
}

void TestCodec() {
	Section = "binding codec";
	auto binding = Bound(77);
	binding.waits.emplace(QString::fromLatin1(kSyncChatCreateMethod), Now + 43200);
	binding.createStep = SyncChatCreateStep::Archived;
	binding.createAt = Now - 5;
	binding.created = Channel(77);
	binding.problem = SyncChatProblem::ForeignRecord;
	binding.problemShown = true;
	binding.migration = SyncChatMigrationStep::Published;
	binding.login = u"opaque"_q;
	binding.preserved.insert(u"future"_q, 3);
	const auto bytes = SerializeSyncChatBinding(binding);
	CHECK(bytes.has_value());
	const auto parsed = bytes ? ParseSyncChatBinding(*bytes) : std::nullopt;
	CHECK(parsed && *parsed == binding);
	CHECK(parsed && parsed->preserved.value(u"future"_q).toInt() == 3);
	CHECK(SerializeSyncChatBinding(SyncChatBinding()).has_value());
	auto bad = binding; bad.chat.reset(); CHECK(!SerializeSyncChatBinding(bad));
	bad = SyncChatBinding(); bad.chat = Channel(1); CHECK(!SerializeSyncChatBinding(bad));
	bad = binding; bad.problem = SyncChatProblem::None; CHECK(!SerializeSyncChatBinding(bad));
	bad = binding; bad.waits.emplace(u"bad method"_q, Now); CHECK(!SerializeSyncChatBinding(bad));
	CHECK(!ParseSyncChatBinding("{}"));
	CHECK(!ParseSyncChatBinding("not json"));
	CHECK(!ParseSyncChatBinding(QByteArray(20000, ' ')));
	const auto newer = QByteArray(*bytes).replace("\"version\":1", "\"version\":2");
	CHECK(!ParseSyncChatBinding(newer));
	const auto wrongTime = QByteArray(*bytes).replace("\"verifiedAt\":\"", "\"verifiedAt\":\"-");
	CHECK(!ParseSyncChatBinding(wrongTime));
	for (auto problem = int(SyncChatProblem::None); problem <= int(SyncChatProblem::SpaceMismatch); ++problem) {
		const auto code = SyncChatProblemCode(SyncChatProblem(problem));
		CHECK(!code.isEmpty() && ParseSyncChatProblemCode(code) == SyncChatProblem(problem));
	}
	CHECK(!ParseSyncChatProblemCode(u"Chat_Gone"_q));

	// The list choice is written only when set, so older files read unchanged.
	CHECK(!bytes->contains("listed"));
	auto listed = binding;
	listed.listed = true;
	const auto listedBytes = SerializeSyncChatBinding(listed);
	CHECK(listedBytes && listedBytes->contains("\"listed\":true"));
	const auto listedParsed = listedBytes ? ParseSyncChatBinding(*listedBytes) : std::nullopt;
	CHECK(listedParsed && listedParsed->listed && *listedParsed == listed);
	CHECK(!ParseSyncChatBinding(QByteArray(*listedBytes).replace("\"listed\":true", "\"listed\":false")));
	CHECK(!ParseSyncChatBinding(QByteArray(*listedBytes).replace("\"listed\":true", "\"listed\":1")));
}

void TestWaits() {
	Section = "persisted waits";
	auto binding = SyncChatBinding();
	const auto method = QString::fromLatin1(kSyncChatFullInfoMethod);
	CHECK(SyncChatWaitUntil(binding, method, Now) == 0);
	RecordSyncChatWait(binding, method, Now + 100);
	RecordSyncChatWait(binding, method, Now + 50);
	CHECK(SyncChatWaitUntil(binding, method, Now) == Now + 100);
	CHECK(SyncChatWaitUntil(binding, method, Now + 100) == 0);
	RecordSyncChatWait(binding, method, Now + 43200);
	const auto restored = ParseSyncChatBinding(*SerializeSyncChatBinding(binding));
	CHECK(restored && SyncChatWaitUntil(*restored, method, Now + 3600) == Now + 43200);
	CHECK(SyncChatWaitUntil(*restored, QString::fromLatin1(kSyncChatCreateMethod), Now) == 0);
	RecordSyncChatWait(binding, u"bad method"_q, Now + 5);
	CHECK(binding.waits.size() == 1);
}

void TestProblems() {
	Section = "problems are shown once and cleared by the user";
	auto binding = Bound(5);
	RaiseSyncChatProblem(binding, SyncChatProblem::ExtraMember);
	auto input = SyncChatLookupInput();
	input.binding = binding;
	input.now = Now;
	auto plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Problem && plan.problem == SyncChatProblem::ExtraMember && plan.show);
	binding.problemShown = true;
	input.binding = binding;
	plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Problem && !plan.show);
	RaiseSyncChatProblem(binding, SyncChatProblem::ExtraMember);
	CHECK(binding.problemShown);
	RaiseSyncChatProblem(binding, SyncChatProblem::ForeignRecord);
	CHECK(binding.problem == SyncChatProblem::ForeignRecord && !binding.problemShown);
	ClearSyncChatProblem(binding);
	input.binding = binding;
	input.boundState = SyncChatBoundState::Present;
	input.bound = Magic(5);
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Use);
}

void TestUnbound() {
	Section = "unbound: local lists first, complete lists prove absence, then create";
	auto input = SyncChatLookupInput();
	input.now = Now;
	auto plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Wait && plan.retryAt == 0 && !plan.telegram);
	input.visible = { Magic(9), Magic(6, false) };
	plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Verify && plan.chat == Channel(6) && plan.rebind);
	auto extra = Magic(4); extra.facts.otherMembers = 1;
	input.visible = { extra };
	plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Wait && plan.retryAt == 0 && !plan.telegram);
	input.binding.rejected[9] = Now;
	input.visible = { Magic(9) };
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Wait);
	input.binding.rejected.clear();
	input.visible = { extra };
	input.listsComplete = true;
	plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Problem && plan.problem == SyncChatProblem::MagicIneligible && plan.show);
	input.visible.clear();
	input.listsComplete = false;
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Wait);
	input.listsComplete = true;
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Create);
	RecordSyncChatWait(input.binding, QString::fromLatin1(kSyncChatCreateMethod), Now + 600);
	plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Wait && plan.retryAt == Now + 600 && plan.telegram);
	input.binding.waits.clear();
	input.binding.createStep = SyncChatCreateStep::Uncertain;
	input.binding.createAt = Now - 10;
	plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Wait && plan.retryAt == Now - 10 + kSyncChatCreateUncertainSeconds
		&& !plan.telegram);
	input.now = Now - 10 + kSyncChatCreateUncertainSeconds;
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Create);
	input.now = Now;
	input.binding.createStep = SyncChatCreateStep::None;
	RecordSyncChatWait(input.binding, QString::fromLatin1(kSyncChatCreateMethod), Now + 30);
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Wait);
}

void TestMagic() {
	Section = "magic binding";
	auto input = SyncChatLookupInput();
	input.binding = Bound(20);
	input.now = Now;
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Wait);
	input.listsComplete = true;
	auto gone = PlanSyncChatLookup(input);
	CHECK(gone.action == SyncChatLookupAction::Problem && gone.problem == SyncChatProblem::ChatGone && gone.show);
	input.listsComplete = false;
	input.boundState = SyncChatBoundState::Gone;
	auto plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Problem && plan.problem == SyncChatProblem::ChatGone);
	input.boundState = SyncChatBoundState::Present;
	input.bound = Magic(20);
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Use);
	input.bound.title = u"Renamed"_q;
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Use);
	input.bound = Magic(20);
	input.bound.facts.otherMembers = 1;
	CHECK(PlanSyncChatLookup(input).problem == SyncChatProblem::ExtraMember);
	input.bound = Magic(20);
	input.visible = { Magic(25), Magic(20) };
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Use);
	input.visible = { Magic(15), Magic(20) };
	plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Verify && plan.chat == Channel(15) && plan.rebind);
	input.visible.clear();
	input.fresh = true;
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Use);
	input.binding.verifiedAt = Now - kSyncChatVerifySeconds;
	plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Verify && plan.chat == Channel(20) && !plan.rebind);
	RecordSyncChatWait(input.binding, QString::fromLatin1(kSyncChatFullInfoMethod), Now + 60);
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Use);
	input.fresh = false;
	input.binding.waits.clear();
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Use);
	input.bound = Magic(20, false);
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Verify);
	RecordSyncChatWait(input.binding, QString::fromLatin1(kSyncChatFullInfoMethod), Now + 60);
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Use);
	input.binding.waits.clear();
	input.binding.verifiedAt = Now - 60;
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Use);
	input.binding.verifiedAt = Now - kSyncChatVerifySeconds;
	input.binding.verifiedAt = Now + 1000;
	input.binding.waits.clear();
	input.bound = Magic(20);
	input.fresh = true;
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Verify);
}

void TestErrors() {
	Section = "only Telegram's waits are waits; refusals are problems";
	const auto kind = [](int code, const char *type, bool timeout = false) {
		return ClassifySyncChatError(code, QString::fromLatin1(type), timeout);
	};
	CHECK(kind(420, "FLOOD_WAIT_30").kind == SyncChatErrorKind::Wait && kind(420, "FLOOD_WAIT_30").seconds == 30);
	CHECK(kind(420, "FLOOD_PREMIUM_WAIT_5").seconds == 5);
	CHECK(kind(400, "SLOWMODE_WAIT_12").kind == SyncChatErrorKind::Wait);
	CHECK(kind(420, "FLOOD_WAIT_").kind == SyncChatErrorKind::Refused);
	CHECK(kind(420, "FLOOD_WAIT_-3").kind == SyncChatErrorKind::Refused);
	CHECK(kind(400, "INPUT_METHOD_INVALID_1_2").kind == SyncChatErrorKind::Refused);
	CHECK(kind(400, "PEER_FLOOD").kind == SyncChatErrorKind::Refused);
	CHECK(kind(400, "CHAT_ID_INVALID").kind == SyncChatErrorKind::Refused);
	CHECK(kind(403, "CHAT_WRITE_FORBIDDEN").kind == SyncChatErrorKind::Refused);
	CHECK(kind(406, "CHANNEL_PRIVATE").kind == SyncChatErrorKind::Refused);
	CHECK(kind(0, "", true).kind == SyncChatErrorKind::Transient);
	CHECK(kind(-1, "").kind == SyncChatErrorKind::Transient);
	CHECK(kind(-503, "Timedout").kind == SyncChatErrorKind::Transient);
	CHECK(kind(500, "INTERNAL").kind == SyncChatErrorKind::Transient);
	CHECK(kind(401, "AUTH_KEY_UNREGISTERED").kind == SyncChatErrorKind::Transient);
	const auto create = QString::fromLatin1(kSyncChatCreateMethod);
	const auto full = QString::fromLatin1(kSyncChatFullInfoMethod);
	CHECK(SyncChatRefusalProblem(create, u"CHANNELS_TOO_MUCH"_q) == SyncChatProblem::CreateLimit);
	CHECK(SyncChatRefusalProblem(create, u"USER_RESTRICTED"_q) == SyncChatProblem::CreateLimit);
	CHECK(SyncChatRefusalProblem(create, u"PEER_FLOOD"_q) == SyncChatProblem::RequestRefused);
	CHECK(SyncChatRefusalProblem(full, u"CHANNEL_PRIVATE"_q) == SyncChatProblem::ChatGone);
	CHECK(SyncChatRefusalProblem(full, u"INPUT_METHOD_INVALID"_q) == SyncChatProblem::RequestRefused);
	CHECK(SyncChatProblemCode(SyncChatProblem::RequestRefused) == u"request_refused"_q);
	CHECK(ParseSyncChatProblemCode(u"request_refused"_q) == SyncChatProblem::RequestRefused);
	auto binding = Bound(20);
	RecordSyncChatVerifyProblem(binding, 20, SyncChatProblem::RequestRefused, Now);
	CHECK(binding.problem == SyncChatProblem::RequestRefused && !binding.problemShown && binding.rejected.empty());
	binding = Bound(20);
	RecordSyncChatVerifyProblem(binding, 15, SyncChatProblem::ChatGone, Now);
	CHECK(binding.problem == SyncChatProblem::None && SyncChatCandidateRejected(binding, 15));
}

void TestManual() {
	Section = "manual binding";
	auto input = SyncChatLookupInput();
	input.binding.mode = SyncChatBindingMode::Manual;
	input.binding.chat = PeerToken();
	input.now = Now;
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Use);
	input.binding.chat = PeerToken{ PeerTokenKind::Chat, 31 };
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Wait);
	input.boundState = SyncChatBoundState::Present;
	input.bound.facts.kind = SyncChatKind::BasicGroup;
	input.bound.facts.canSendFiles = true;
	input.bound.facts.otherMembers = 0;
	input.bound.facts.otherAdmins = 0;
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Use);
	input.visible = { Magic(1), Magic(2) };
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Use);
	input.bound.facts.otherMembers = 1;
	CHECK(PlanSyncChatLookup(input).problem == SyncChatProblem::ExtraMember);
	input.bound.facts.otherMembers = 0;
	input.bound.member = false;
	CHECK(PlanSyncChatLookup(input).problem == SyncChatProblem::ChatLeft);
	input.bound.member = true;
	input.bound.facts.canSendFiles = false;
	CHECK(PlanSyncChatLookup(input).problem == SyncChatProblem::CannotPost);
	input.boundState = SyncChatBoundState::Gone;
	CHECK(PlanSyncChatLookup(input).problem == SyncChatProblem::ChatGone);
}

void TestVerify() {
	Section = "verify binds or raises";
	auto binding = SyncChatBinding();
	binding.createStep = SyncChatCreateStep::Muted;
	binding.createAt = Now - 3;
	binding.created = Channel(40);
	CHECK(CompleteSyncChatVerify(binding, Magic(40), Now) == SyncChatProblem::None);
	CHECK(binding.mode == SyncChatBindingMode::Magic && binding.chat == Channel(40) && binding.verifiedAt == Now);
	CHECK(binding.createStep == SyncChatCreateStep::None && !binding.created);
	auto extra = Magic(41); extra.facts.otherAdmins = 1;
	CHECK(CompleteSyncChatVerify(binding, extra, Now) == SyncChatProblem::ExtraAdmin);
	CHECK(binding.problem == SyncChatProblem::None && binding.chat == Channel(40));
	CHECK(SyncChatCandidateRejected(binding, 41) && !SyncChatCandidateRejected(binding, 40));
	CHECK(CompleteSyncChatVerify(binding, Magic(42, false), Now) == SyncChatProblem::MagicIneligible);
	CHECK(binding.problem == SyncChatProblem::None && SyncChatCandidateRejected(binding, 42));
	auto bound = Magic(40); bound.facts.otherMembers = 1;
	CHECK(CompleteSyncChatVerify(binding, bound, Now) == SyncChatProblem::ExtraMember);
	CHECK(binding.problem == SyncChatProblem::ExtraMember && !SyncChatCandidateRejected(binding, 40));
	CHECK(SerializeSyncChatBinding(binding).has_value());
}

void TestRejected() {
	Section = "a candidate failing verification is skipped, not raised";
	auto input = SyncChatLookupInput();
	input.binding = Bound(20);
	input.now = Now;
	input.boundState = SyncChatBoundState::Present;
	input.bound = Magic(20);
	input.visible = { Magic(15), Magic(20) };
	auto plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Verify && plan.chat == Channel(15) && plan.rebind);
	auto older = Magic(15); older.facts.otherMembers = 1;
	CHECK(CompleteSyncChatVerify(input.binding, older, Now) == SyncChatProblem::ExtraMember);
	CHECK(input.binding.problem == SyncChatProblem::None && input.binding.chat == Channel(20));
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Use);
	input.visible = { Magic(15), Magic(12), Magic(20) };
	plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Verify && plan.chat == Channel(12));
	const auto bytes = SerializeSyncChatBinding(input.binding);
	CHECK(bytes && bytes->contains("\"rejected\":{\"15\":\""));
	const auto parsed = bytes ? ParseSyncChatBinding(*bytes) : std::nullopt;
	CHECK(parsed && *parsed == input.binding);
	ReadmitSyncChatCandidates(input.binding);
	CHECK(!SyncChatCandidateRejected(input.binding, 15));
	CHECK(!SerializeSyncChatBinding(input.binding)->contains("rejected"));
	CHECK(!ParseSyncChatBinding(QByteArray(R"({"create":{"at":"0","step":"none"},"login":"","migration":"none","mode":"none","problem":"none","problemShown":false,"rejected":{},"verifiedAt":"0","version":1,"waits":{}})")));
	CHECK(!ParseSyncChatBinding(QByteArray(R"({"create":{"at":"0","step":"none"},"login":"","migration":"none","mode":"none","problem":"none","problemShown":false,"rejected":{"07":"1"},"verifiedAt":"0","version":1,"waits":{}})")));

	Section = "unbound: the next candidate, and magic_ineligible only when none is left";
	auto unbound = SyncChatLookupInput();
	unbound.now = Now;
	unbound.visible = { Magic(6), Magic(9) };
	auto bad = Magic(6); bad.facts.otherAdmins = 1;
	CHECK(CompleteSyncChatVerify(unbound.binding, bad, Now) == SyncChatProblem::ExtraAdmin);
	CHECK(unbound.binding.problem == SyncChatProblem::None && !unbound.binding.chat);
	plan = PlanSyncChatLookup(unbound);
	CHECK(plan.action == SyncChatLookupAction::Verify && plan.chat == Channel(9));
	CHECK(CompleteSyncChatVerify(unbound.binding, Magic(9, false), Now) == SyncChatProblem::MagicIneligible);
	CHECK(PlanSyncChatLookup(unbound).action == SyncChatLookupAction::Wait);
	unbound.listsComplete = true;
	plan = PlanSyncChatLookup(unbound);
	CHECK(plan.action == SyncChatLookupAction::Problem && plan.problem == SyncChatProblem::MagicIneligible);

	Section = "the rejection list stays bounded";
	auto full = SyncChatBinding();
	for (auto id = 100; id < 120; ++id) {
		CHECK(CompleteSyncChatVerify(full, Magic(id, false), Now + id) == SyncChatProblem::MagicIneligible);
	}
	CHECK(full.rejected.size() == 16 && !SyncChatCandidateRejected(full, 100) && SyncChatCandidateRejected(full, 119));
	CHECK(SerializeSyncChatBinding(full).has_value());
}

SyncLocalState Staged(bool confirmFirst) {
	auto state = SyncLocalState();
	state.install = *FormatSyncInstallId(QByteArray(16, 'a'));
	state.createdDevice = u"android:"_q + state.install;
	state.space = *FormatSyncSpaceId(QByteArray(16, 's'));
	state.bindingToken = *FormatSyncBindingToken(QByteArray(16, 't'));
	const auto stage = [&](const QByteArray &text) {
		const auto built = BuildConfigRecord({ text, {}, std::nullopt, state.space, state.install,
			state.createdDevice, u"Android"_q, u"test"_q, state.config.seq + 1, uint64_t(Now) });
		CHECK(built);
		const auto reserved = ReserveSyncSeq(state, SyncLocalStream::Config, built.payloadHash);
		CHECK(reserved);
		auto next = reserved.state;
		next.configData.pending = built.version.key;
		const auto issued = AppendIssuedConfigRecord(next, built.canonical);
		CHECK(issued);
		return std::make_pair(issued.state, built);
	};
	if (confirmFirst) {
		const auto [issued, built] = stage("version = 1\n# first\n");
		const auto confirmed = ConfirmConfigReadBack(issued, state.createdDevice,
			{ OwnRecordObservationKind::Present, issued.config.pendingSeq, built.payloadHash }, built.version);
		CHECK(confirmed.changed);
		state = confirmed.state;
	}
	state = stage("version = 1\n# second\n").first;
	CHECK(SerializeSyncLocalState(state));
	return state;
}

QByteArray Canonical(const SyncLocalState &state) {
	return SerializeSyncLocalState(state).canonical;
}

void TestMoveState() {
	Section = "moving abandons a stage left in the old chat";
	const auto magic = PeerToken{ PeerTokenKind::Channel, 42 };
	for (const auto confirmFirst : { false, true }) {
		const auto state = Staged(confirmFirst);
		CHECK(state.config.pendingSeq != 0);
		CHECK(!MoveSyncChat(state, magic, false));
		const auto moved = MoveSyncStateToChat(state, magic);
		CHECK(moved.move && moved.move.changed);
		CHECK(moved.abandonedSeq == state.config.pendingSeq);
		CHECK(moved.move.state.chat == magic && moved.move.state.version == 2);
		CHECK(moved.move.state.config.pendingSeq == 0 && moved.move.state.configData.pending.isEmpty());
		CHECK(moved.move.state.config.abandonedSeq == state.config.pendingSeq);
		CHECK(moved.move.state.config.seq == state.config.seq);
		CHECK(moved.move.state.config.confirmedSeq == state.config.confirmedSeq);
		CHECK(SerializeSyncLocalState(moved.move.state));
		const auto again = MoveSyncStateToChat(moved.move.state, magic);
		CHECK(again.move && !again.move.changed && again.abandonedSeq == 0);
		CHECK(Canonical(again.move.state) == Canonical(moved.move.state));
	}

	Section = "moving to the same chat keeps a stage";
	auto self = Staged(true);
	const auto same = MoveSyncStateToChat(self, self.chat);
	CHECK(same.move && !same.move.changed && same.abandonedSeq == 0);
	CHECK(Canonical(same.move.state) == Canonical(self));

	Section = "a refused move changes nothing";
	auto library = Staged(true);
	library.library.pendingSeq = 1;
	const auto refused = MoveSyncStateToChat(library, magic);
	CHECK(!refused.move && refused.abandonedSeq == 0 && Canonical(refused.move.state) == Canonical(library));
	const auto invalid = MoveSyncStateToChat(Staged(true), PeerToken{ PeerTokenKind::Channel, 0 });
	CHECK(!invalid.move && invalid.abandonedSeq == 0);
}

} // namespace

int main() {
	TestTitle();
	TestCheck();
	TestPick();
	TestCodec();
	TestWaits();
	TestProblems();
	TestUnbound();
	TestMagic();
	TestErrors();
	TestManual();
	TestVerify();
	TestRejected();
	TestMoveState();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
