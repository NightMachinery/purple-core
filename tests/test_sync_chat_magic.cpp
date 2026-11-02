/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_chat_magic.h"
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
	binding.waits.emplace(QString::fromLatin1(kSyncChatAllChatsMethod), Now + 43200);
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
}

void TestWaits() {
	Section = "persisted waits";
	auto binding = SyncChatBinding();
	const auto method = QString::fromLatin1(kSyncChatAllChatsMethod);
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
	Section = "unbound: local lists first, one absence proof, then create";
	auto input = SyncChatLookupInput();
	input.now = Now;
	auto plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::ProveAbsent);
	input.visible = { Magic(9), Magic(6, false) };
	plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Verify && plan.chat == Channel(6) && plan.rebind);
	auto extra = Magic(4); extra.facts.otherMembers = 1;
	input.visible = { extra };
	plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Problem && plan.problem == SyncChatProblem::MagicIneligible && plan.show);
	input.visible.clear();
	RecordSyncChatWait(input.binding, QString::fromLatin1(kSyncChatAllChatsMethod), Now + 43200);
	plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Wait && plan.retryAt == Now + 43200);
	input.binding.waits.clear();
	input.allChats = std::vector<MagicChatCandidate>{ Magic(12) };
	plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Verify && plan.chat == Channel(12));
	input.allChats = std::vector<MagicChatCandidate>{ extra };
	CHECK(PlanSyncChatLookup(input).problem == SyncChatProblem::MagicIneligible);
	input.allChats = std::vector<MagicChatCandidate>();
	CHECK(PlanSyncChatLookup(input).action == SyncChatLookupAction::Create);
	input.binding.createStep = SyncChatCreateStep::Uncertain;
	input.binding.createAt = Now - 10;
	plan = PlanSyncChatLookup(input);
	CHECK(plan.action == SyncChatLookupAction::Wait && plan.retryAt == Now - 10 + kSyncChatCreateUncertainSeconds);
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
	input.visible = { Magic(1) };
	input.allChats = std::vector<MagicChatCandidate>{ Magic(2) };
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
	CHECK(binding.problem == SyncChatProblem::ExtraAdmin && binding.chat == Channel(40));
	ClearSyncChatProblem(binding);
	CHECK(CompleteSyncChatVerify(binding, Magic(42, false), Now) == SyncChatProblem::MagicIneligible);
	CHECK(SerializeSyncChatBinding(binding).has_value());
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
	TestManual();
	TestVerify();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
