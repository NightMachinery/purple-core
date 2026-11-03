/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_chat_magic.h"

#include "purple/purple_sync_json.h"
#include "purple/purple_types.h"

#include <QtCore/QJsonDocument>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace Purple {
namespace {

constexpr auto kBindingLimit = 16 * 1024;
constexpr auto kWaitLimit = 32;
constexpr auto kRejectLimit = 16;

const auto Problems = std::array<std::pair<SyncChatProblem, const char*>, 17>{ {
	{ SyncChatProblem::None, "none" },
	{ SyncChatProblem::ChatGone, "chat_gone" },
	{ SyncChatProblem::ChatLeft, "chat_left" },
	{ SyncChatProblem::NotMegagroup, "not_megagroup" },
	{ SyncChatProblem::NotCreator, "not_creator" },
	{ SyncChatProblem::ExtraMember, "extra_member" },
	{ SyncChatProblem::ExtraAdmin, "extra_admin" },
	{ SyncChatProblem::PublicUsername, "public_username" },
	{ SyncChatProblem::Restricted, "restricted" },
	{ SyncChatProblem::CannotPost, "cannot_post" },
	{ SyncChatProblem::ForeignRecord, "foreign_record" },
	{ SyncChatProblem::ForwardedRecord, "forwarded_record" },
	{ SyncChatProblem::ViaBotRecord, "via_bot_record" },
	{ SyncChatProblem::CreateLimit, "create_limit" },
	{ SyncChatProblem::MagicIneligible, "magic_ineligible" },
	{ SyncChatProblem::SpaceMismatch, "space_mismatch" },
	{ SyncChatProblem::RequestRefused, "request_refused" },
} };

const auto Modes = std::array<std::pair<SyncChatBindingMode, const char*>, 3>{ {
	{ SyncChatBindingMode::None, "none" },
	{ SyncChatBindingMode::Magic, "magic" },
	{ SyncChatBindingMode::Manual, "manual" },
} };

const auto CreateSteps = std::array<std::pair<SyncChatCreateStep, const char*>, 6>{ {
	{ SyncChatCreateStep::None, "none" },
	{ SyncChatCreateStep::Requested, "requested" },
	{ SyncChatCreateStep::Uncertain, "uncertain" },
	{ SyncChatCreateStep::Created, "created" },
	{ SyncChatCreateStep::Archived, "archived" },
	{ SyncChatCreateStep::Muted, "muted" },
} };

const auto Migrations = std::array<std::pair<SyncChatMigrationStep, const char*>, 5>{ {
	{ SyncChatMigrationStep::None, "none" },
	{ SyncChatMigrationStep::Moved, "moved" },
	{ SyncChatMigrationStep::Published, "published" },
	{ SyncChatMigrationStep::Joined, "joined" },
	{ SyncChatMigrationStep::Done, "done" },
} };

template <typename Value, size_t Size>
QString Code(const std::array<std::pair<Value, const char*>, Size> &table, Value value) {
	for (const auto &[known, code] : table) {
		if (known == value) { return QString::fromLatin1(code); }
	}
	return QString();
}

template <typename Value, size_t Size>
std::optional<Value> Parse(const std::array<std::pair<Value, const char*>, Size> &table, const QString &code) {
	for (const auto &[known, text] : table) {
		if (code == QLatin1String(text)) { return known; }
	}
	return std::nullopt;
}

std::optional<int64_t> Time(const QJsonValue &value) {
	if (!value.isString()) { return std::nullopt; }
	auto ok = false;
	const auto result = value.toString().toLongLong(&ok);
	return (ok && result >= 0 && QString::number(result) == value.toString())
		? std::make_optional(int64_t(result)) : std::nullopt;
}

std::optional<PeerToken> Token(const QJsonValue &value) {
	return value.isString() ? ParsePeerToken(value.toString()) : std::nullopt;
}

bool MethodName(const QString &method) {
	if (method.isEmpty() || method.size() > 64) { return false; }
	for (const auto ch : method) {
		if (!((ch >= u'a' && ch <= u'z') || (ch >= u'A' && ch <= u'Z')
			|| (ch >= u'0' && ch <= u'9') || ch == u'.' || ch == u'_')) { return false; }
	}
	return true;
}

PeerToken Channel(uint64_t id) {
	return PeerToken{ PeerTokenKind::Channel, id };
}

bool Usable(const MagicChatCandidate &chat) {
	return CheckMagicChat(chat).verdict != MagicChatVerdict::Ineligible;
}

SyncChatLookupPlan Problem(SyncChatProblem problem, bool show, std::optional<PeerToken> chat = std::nullopt) {
	auto result = SyncChatLookupPlan();
	result.action = SyncChatLookupAction::Problem;
	result.problem = problem;
	result.show = show;
	result.chat = std::move(chat);
	return result;
}

SyncChatLookupPlan Act(SyncChatLookupAction action, std::optional<PeerToken> chat = std::nullopt, bool rebind = false) {
	auto result = SyncChatLookupPlan();
	result.action = action;
	result.chat = std::move(chat);
	result.rebind = rebind;
	return result;
}

SyncChatLookupPlan WaitUntil(int64_t retryAt, bool telegram = false) {
	auto result = SyncChatLookupPlan();
	result.action = SyncChatLookupAction::Wait;
	result.retryAt = retryAt;
	result.telegram = telegram;
	return result;
}

SyncChatLookupPlan Manual(const SyncChatLookupInput &input) {
	const auto &chat = *input.binding.chat;
	if (chat.kind == PeerTokenKind::Self) { return Act(SyncChatLookupAction::Use, chat); }
	switch (input.boundState) {
	case SyncChatBoundState::Unknown: return Act(SyncChatLookupAction::Wait);
	case SyncChatBoundState::Gone: return Problem(SyncChatProblem::ChatGone, true, chat);
	case SyncChatBoundState::Present: break;
	}
	if (!input.bound.member) { return Problem(SyncChatProblem::ChatLeft, true, chat); }
	if (input.bound.facts.otherMembers.value_or(0) != 0) { return Problem(SyncChatProblem::ExtraMember, true, chat); }
	if (input.bound.facts.otherAdmins.value_or(0) != 0) { return Problem(SyncChatProblem::ExtraAdmin, true, chat); }
	const auto refusal = SyncChatEligibility(input.bound.facts, SyncChatBroadcastOwnership::CreatorAndSoleAdmin).refusal;
	if (refusal == SyncChatRefusal::CannotSendFiles) { return Problem(SyncChatProblem::CannotPost, true, chat); }
	if (refusal != SyncChatRefusal::None) { return Problem(SyncChatProblem::Restricted, true, chat); }
	return Act(SyncChatLookupAction::Use, chat);
}

std::vector<MagicChatCandidate> Admitted(
		const SyncChatBinding &binding,
		std::vector<MagicChatCandidate> chats) {
	chats.erase(std::remove_if(chats.begin(), chats.end(), [&](const MagicChatCandidate &chat) {
		return SyncChatCandidateRejected(binding, chat.channelId);
	}), chats.end());
	return chats;
}

SyncChatLookupPlan Magic(const SyncChatLookupInput &input) {
	const auto &binding = input.binding;
	const auto &chat = *binding.chat;
	if (chat.kind != PeerTokenKind::Channel) { return Problem(SyncChatProblem::NotMegagroup, true, chat); }
	switch (input.boundState) {
	case SyncChatBoundState::Unknown:
		return input.listsComplete ? Problem(SyncChatProblem::ChatGone, true, chat) : Act(SyncChatLookupAction::Wait);
	case SyncChatBoundState::Gone: return Problem(SyncChatProblem::ChatGone, true, chat);
	case SyncChatBoundState::Present: break;
	}
	const auto check = CheckMagicChat(input.bound);
	if (check.verdict == MagicChatVerdict::Ineligible) { return Problem(check.problem, true, chat); }
	const auto oldest = PickMagicChat(Admitted(binding, input.visible));
	if (oldest && *oldest < chat.id) { return Act(SyncChatLookupAction::Verify, Channel(*oldest), true); }
	const auto stale = binding.verifiedAt <= 0 || binding.verifiedAt > input.now
		|| input.now - binding.verifiedAt >= kSyncChatVerifySeconds;
	const auto due = stale && (input.fresh || check.verdict == MagicChatVerdict::Unverified);
	const auto waiting = SyncChatWaitUntil(binding, QString::fromLatin1(kSyncChatFullInfoMethod), input.now) != 0;
	return Act((due && !waiting) ? SyncChatLookupAction::Verify : SyncChatLookupAction::Use, chat);
}

SyncChatLookupPlan Unbound(const SyncChatLookupInput &input) {
	const auto &binding = input.binding;
	if (const auto pick = PickMagicChat(Admitted(binding, input.visible))) {
		return Act(SyncChatLookupAction::Verify, Channel(*pick), true);
	}
	if (!input.visible.empty()) { return Problem(SyncChatProblem::MagicIneligible, true); }
	if (!input.listsComplete) { return Act(SyncChatLookupAction::Wait); }
	if (binding.createStep != SyncChatCreateStep::None
		&& binding.createAt <= input.now
		&& input.now - binding.createAt < kSyncChatCreateUncertainSeconds) {
		return WaitUntil(binding.createAt + kSyncChatCreateUncertainSeconds);
	}
	if (const auto until = SyncChatWaitUntil(binding, QString::fromLatin1(kSyncChatCreateMethod), input.now)) {
		return WaitUntil(until, true);
	}
	return Act(SyncChatLookupAction::Create);
}

} // namespace

bool IsMagicSyncChatTitle(const QString &title) {
	return title == QLatin1String(kMagicSyncChatTitle);
}

QString SyncChatProblemCode(SyncChatProblem problem) {
	return Code(Problems, problem);
}

std::optional<SyncChatProblem> ParseSyncChatProblemCode(const QString &code) {
	return Parse(Problems, code);
}

MagicChatCheck CheckMagicChat(const MagicChatCandidate &chat) {
	const auto refuse = [](SyncChatProblem problem, SyncChatRefusal refusal = SyncChatRefusal::None) {
		return MagicChatCheck{ MagicChatVerdict::Ineligible, problem, refusal };
	};
	if (!chat.member) { return refuse(SyncChatProblem::ChatLeft); }
	if (!chat.megagroup || chat.gigagroup || chat.facts.kind != SyncChatKind::Supergroup) {
		return refuse(SyncChatProblem::NotMegagroup);
	}
	if (chat.facts.publicUsername) { return refuse(SyncChatProblem::PublicUsername); }
	if (!chat.facts.creator) { return refuse(SyncChatProblem::NotCreator); }
	if (chat.facts.otherMembers.value_or(0) != 0) { return refuse(SyncChatProblem::ExtraMember); }
	if (chat.facts.otherAdmins.value_or(0) != 0) { return refuse(SyncChatProblem::ExtraAdmin); }
	const auto refusal = SyncChatEligibility(chat.facts, SyncChatBroadcastOwnership::CreatorAndSoleAdmin).refusal;
	if (refusal == SyncChatRefusal::CannotSendFiles) { return refuse(SyncChatProblem::CannotPost, refusal); }
	if (refusal != SyncChatRefusal::None) { return refuse(SyncChatProblem::Restricted, refusal); }
	const auto known = chat.facts.otherMembers && chat.facts.otherAdmins;
	return MagicChatCheck{ known ? MagicChatVerdict::Eligible : MagicChatVerdict::Unverified };
}

std::optional<uint64_t> PickMagicChat(const std::vector<MagicChatCandidate> &chats) {
	auto result = std::optional<uint64_t>();
	for (const auto &chat : chats) {
		if (chat.channelId == 0 || !Usable(chat)) { continue; }
		if (!result || chat.channelId < *result) { result = chat.channelId; }
	}
	return result;
}

std::optional<QByteArray> SerializeSyncChatBinding(const SyncChatBinding &binding) {
	if (binding.version != 1 || (binding.mode != SyncChatBindingMode::None) != bool(binding.chat)
		|| binding.verifiedAt < 0 || binding.createAt < 0 || int(binding.waits.size()) > kWaitLimit
		|| int(binding.rejected.size()) > kRejectLimit
		|| (binding.problem == SyncChatProblem::None && binding.problemShown)
		|| SyncChatProblemCode(binding.problem).isEmpty()
		|| Code(Modes, binding.mode).isEmpty() || Code(CreateSteps, binding.createStep).isEmpty()
		|| Code(Migrations, binding.migration).isEmpty()) {
		return std::nullopt;
	}
	auto object = binding.preserved;
	object.insert(u"version"_q, binding.version);
	object.insert(u"mode"_q, Code(Modes, binding.mode));
	if (binding.chat) {
		const auto text = PeerTokenText(*binding.chat);
		if (!text) { return std::nullopt; }
		object.insert(u"chat"_q, *text);
	} else {
		object.remove(u"chat"_q);
	}
	object.insert(u"verifiedAt"_q, QString::number(binding.verifiedAt));
	auto waits = QJsonObject();
	for (const auto &[method, retryAt] : binding.waits) {
		if (!MethodName(method) || retryAt <= 0) { return std::nullopt; }
		waits.insert(method, QString::number(retryAt));
	}
	object.insert(u"waits"_q, waits);
	auto create = QJsonObject();
	create.insert(u"step"_q, Code(CreateSteps, binding.createStep));
	create.insert(u"at"_q, QString::number(binding.createAt));
	if (binding.created) {
		const auto text = PeerTokenText(*binding.created);
		if (!text) { return std::nullopt; }
		create.insert(u"chat"_q, *text);
	}
	object.insert(u"create"_q, create);
	object.insert(u"problem"_q, SyncChatProblemCode(binding.problem));
	object.insert(u"problemShown"_q, binding.problemShown);
	object.insert(u"migration"_q, Code(Migrations, binding.migration));
	object.insert(u"login"_q, binding.login);
	if (binding.listed) {
		object.insert(u"listed"_q, true);
	} else {
		object.remove(u"listed"_q);
	}
	if (binding.rejected.empty()) {
		object.remove(u"rejected"_q);
	} else {
		auto rejected = QJsonObject();
		for (const auto &[id, at] : binding.rejected) {
			if (id == 0 || at < 0) { return std::nullopt; }
			rejected.insert(QString::number(id), QString::number(at));
		}
		object.insert(u"rejected"_q, rejected);
	}
	const auto json = CanonicalizeSyncJson(QJsonDocument(object).toJson(QJsonDocument::Compact));
	return (json && json.canonical.size() <= kBindingLimit) ? std::make_optional(json.canonical) : std::nullopt;
}

std::optional<SyncChatBinding> ParseSyncChatBinding(const QByteArray &bytes) {
	if (bytes.size() > kBindingLimit) { return std::nullopt; }
	const auto canonical = CanonicalizeSyncJson(bytes);
	if (!canonical) { return std::nullopt; }
	const auto document = QJsonDocument::fromJson(canonical.canonical);
	if (!document.isObject()) { return std::nullopt; }
	auto object = document.object();
	auto result = SyncChatBinding();
	const auto version = object.take(u"version"_q);
	if (!version.isDouble() || version.toInteger(-1) != 1) { return std::nullopt; }
	const auto mode = object.take(u"mode"_q);
	const auto parsedMode = mode.isString() ? Parse(Modes, mode.toString()) : std::nullopt;
	if (!parsedMode) { return std::nullopt; }
	result.mode = *parsedMode;
	if (object.contains(u"chat"_q)) {
		result.chat = Token(object.take(u"chat"_q));
		if (!result.chat) { return std::nullopt; }
	}
	const auto verifiedAt = Time(object.take(u"verifiedAt"_q));
	if (!verifiedAt) { return std::nullopt; }
	result.verifiedAt = *verifiedAt;
	const auto waitsValue = object.take(u"waits"_q);
	const auto waits = waitsValue.toObject();
	if (!waitsValue.isObject() || waits.size() > kWaitLimit) { return std::nullopt; }
	for (auto i = waits.begin(); i != waits.end(); ++i) {
		const auto retryAt = Time(i.value());
		if (!MethodName(i.key()) || !retryAt || *retryAt <= 0) { return std::nullopt; }
		result.waits.emplace(i.key(), *retryAt);
	}
	const auto create = object.take(u"create"_q).toObject();
	const auto step = create.value(u"step"_q);
	const auto parsedStep = step.isString() ? Parse(CreateSteps, step.toString()) : std::nullopt;
	const auto createAt = Time(create.value(u"at"_q));
	if (!parsedStep || !createAt || create.size() > 3 || (create.size() == 3 && !create.contains(u"chat"_q))) {
		return std::nullopt;
	}
	result.createStep = *parsedStep;
	result.createAt = *createAt;
	if (create.contains(u"chat"_q)) {
		result.created = Token(create.value(u"chat"_q));
		if (!result.created) { return std::nullopt; }
	}
	const auto problem = object.take(u"problem"_q);
	const auto parsedProblem = problem.isString() ? ParseSyncChatProblemCode(problem.toString()) : std::nullopt;
	const auto shown = object.take(u"problemShown"_q);
	if (!parsedProblem || !shown.isBool()) { return std::nullopt; }
	result.problem = *parsedProblem;
	result.problemShown = shown.toBool();
	const auto migration = object.take(u"migration"_q);
	const auto parsedMigration = migration.isString() ? Parse(Migrations, migration.toString()) : std::nullopt;
	const auto login = object.take(u"login"_q);
	if (!parsedMigration || !login.isString()) { return std::nullopt; }
	result.migration = *parsedMigration;
	result.login = login.toString();
	if (object.contains(u"listed"_q)) {
		const auto listed = object.take(u"listed"_q);
		if (!listed.isBool() || !listed.toBool()) { return std::nullopt; }
		result.listed = true;
	}
	if (object.contains(u"rejected"_q)) {
		const auto value = object.take(u"rejected"_q);
		const auto rejected = value.toObject();
		if (!value.isObject() || rejected.isEmpty() || rejected.size() > kRejectLimit) { return std::nullopt; }
		for (auto i = rejected.begin(); i != rejected.end(); ++i) {
			auto ok = false;
			const auto id = i.key().toULongLong(&ok);
			const auto at = Time(i.value());
			if (!ok || id == 0 || QString::number(id) != i.key() || !at) { return std::nullopt; }
			result.rejected.emplace(id, *at);
		}
	}
	result.preserved = object;
	return SerializeSyncChatBinding(result) ? std::make_optional(result) : std::nullopt;
}

void RecordSyncChatWait(SyncChatBinding &binding, const QString &method, int64_t retryAt) {
	if (!MethodName(method) || retryAt <= 0) { return; }
	auto &value = binding.waits[method];
	value = std::max(value, retryAt);
}

int64_t SyncChatWaitUntil(const SyncChatBinding &binding, const QString &method, int64_t now) {
	const auto i = binding.waits.find(method);
	return (i != binding.waits.end() && i->second > now) ? i->second : 0;
}

void RaiseSyncChatProblem(SyncChatBinding &binding, SyncChatProblem problem) {
	if (problem == SyncChatProblem::None || binding.problem == problem) { return; }
	binding.problem = problem;
	binding.problemShown = false;
}

void ClearSyncChatProblem(SyncChatBinding &binding) {
	binding.problem = SyncChatProblem::None;
	binding.problemShown = false;
}

void ReadmitSyncChatCandidates(SyncChatBinding &binding) {
	binding.rejected.clear();
}

bool SyncChatCandidateRejected(const SyncChatBinding &binding, uint64_t channelId) {
	return binding.rejected.contains(channelId);
}

SyncChatLookupPlan PlanSyncChatLookup(const SyncChatLookupInput &input) {
	const auto &binding = input.binding;
	if (binding.problem != SyncChatProblem::None) {
		return Problem(binding.problem, !binding.problemShown, binding.chat);
	}
	if (binding.mode == SyncChatBindingMode::None || !binding.chat) { return Unbound(input); }
	return binding.mode == SyncChatBindingMode::Manual ? Manual(input) : Magic(input);
}

SyncChatError ClassifySyncChatError(int code, const QString &type, bool timeout) {
	for (const auto prefix : { "FLOOD_WAIT_", "FLOOD_PREMIUM_WAIT_", "SLOWMODE_WAIT_" }) {
		const auto name = QString::fromLatin1(prefix);
		if (!type.startsWith(name)) { continue; }
		auto ok = false;
		const auto seconds = type.mid(name.size()).toLongLong(&ok);
		if (ok && seconds >= 0 && QString::number(seconds) == type.mid(name.size())) {
			return { SyncChatErrorKind::Wait, std::min<int64_t>(seconds, 366 * 24 * 60 * 60) };
		}
	}
	if (timeout || code < 0 || code == 401 || (code >= 500 && code < 600)) { return { SyncChatErrorKind::Transient }; }
	return { SyncChatErrorKind::Refused };
}

SyncChatProblem SyncChatRefusalProblem(const QString &method, const QString &type) {
	if (method == QLatin1String(kSyncChatCreateMethod)
		&& (type.contains(u"TOO_MUCH"_q) || type.startsWith(u"USER_RESTRICTED"_q))) {
		return SyncChatProblem::CreateLimit;
	} else if (method == QLatin1String(kSyncChatFullInfoMethod)
		&& (type == u"CHANNEL_PRIVATE"_q || type == u"CHANNEL_INVALID"_q)) {
		return SyncChatProblem::ChatGone;
	}
	return SyncChatProblem::RequestRefused;
}

void RecordSyncChatVerifyProblem(SyncChatBinding &binding, uint64_t channelId, SyncChatProblem problem, int64_t now) {
	if (problem == SyncChatProblem::None) {
		return;
	} else if (binding.chat == Channel(channelId)) {
		RaiseSyncChatProblem(binding, problem);
		return;
	}
	// WHY: an older chat someone else joined must not pause sync in a working one;
	// the lookup skips it and picks again.
	if (int(binding.rejected.size()) >= kRejectLimit && !binding.rejected.contains(channelId)) {
		binding.rejected.erase(std::min_element(binding.rejected.begin(), binding.rejected.end(),
			[](const auto &a, const auto &b) { return a.second < b.second; }));
	}
	binding.rejected[channelId] = std::max<int64_t>(now, 0);
}

SyncChatProblem CompleteSyncChatVerify(SyncChatBinding &binding, const MagicChatCandidate &chat, int64_t now) {
	const auto check = CheckMagicChat(chat);
	const auto problem = check.verdict == MagicChatVerdict::Eligible ? SyncChatProblem::None
		: check.verdict == MagicChatVerdict::Unverified ? SyncChatProblem::MagicIneligible
		: check.problem;
	if (problem != SyncChatProblem::None) {
		RecordSyncChatVerifyProblem(binding, chat.channelId, problem, now);
		return problem;
	}
	binding.mode = SyncChatBindingMode::Magic;
	binding.chat = Channel(chat.channelId);
	binding.verifiedAt = now;
	binding.createStep = SyncChatCreateStep::None;
	binding.createAt = 0;
	binding.created.reset();
	ClearSyncChatProblem(binding);
	return SyncChatProblem::None;
}

SyncStateMoveResult MoveSyncStateToChat(
		const SyncLocalState &state,
		const PeerToken &chat,
		uint64_t selfUserId) {
	auto result = SyncStateMoveResult();
	const auto text = PeerTokenText(chat, selfUserId);
	if (text && ParsePeerToken(*text) == state.chat) {
		result.move.state = state;
		return result;
	}
	auto next = state;
	if (state.config.pendingSeq != 0) {
		// WHY: only a v2 state may record an abandoned stage; the move makes it v2.
		next.version = 2;
		next.config.abandonedSeq = state.config.pendingSeq;
		next.config.pendingSeq = 0;
		next.configData.pending.clear();
		result.abandonedSeq = next.config.abandonedSeq;
	}
	result.move = MoveSyncChat(next, chat, false, selfUserId);
	if (!result.move) {
		result.move.state = state;
		result.abandonedSeq = 0;
	}
	return result;
}

}
