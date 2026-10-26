/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_chat.h"

#include "purple/purple_types.h"

#include <QtCore/QStringList>

#include <algorithm>
#include <array>
#include <map>
#include <utility>

namespace Purple {
namespace {

[[nodiscard]] bool Newer(
		const SyncChatCache &a,
		const SyncChatCache &b) {
	if (a.markDate != b.markDate) {
		return a.markDate > b.markDate;
	}
	const auto order = *ComparePeerTokens(a.chat, b.chat);
	return order ? (order > 0) : (a.markId > b.markId);
}

[[nodiscard]] std::optional<SyncChatCache> NormalizeCache(
		SyncChatCache cache,
		uint64_t selfUserId) {
	const auto text = PeerTokenText(cache.chat, selfUserId);
	if (!text || cache.markId < 0 || cache.markDate < 0
		|| ((cache.markId == 0) != (cache.markDate == 0))) {
		return std::nullopt;
	}
	cache.chat = *ParsePeerToken(*text);
	return cache;
}

[[nodiscard]] bool SameMarkFacts(
		const SyncChatMark &a,
		const SyncChatMark &b) {
	return a.location == b.location
		&& a.status == b.status
		&& a.eligibility.refusal == b.eligibility.refusal
		&& a.eligibility.readers == b.eligibility.readers
		&& a.records == b.records
		&& a.readable == b.readable
		&& a.freshlyRead == b.freshlyRead;
}

void Searching(
		SyncChatResolution &result,
		int retriesCompleted) {
	constexpr auto delays = std::array{ 30, 120, 600 };
	result.action = (retriesCompleted < int(delays.size()))
		? SyncChatAction::Searching : SyncChatAction::Lost;
	if (result.action == SyncChatAction::Searching) {
		result.retryAfterSeconds = delays[retriesCompleted];
	}
}

void Readers(
		SyncChatResolution &result,
		const SyncChatEligibilityResult &eligibility,
		bool accepted) {
	result.readers = eligibility.readers;
	result.needsReaderWarning = eligibility.readers.warningRequired
		&& !accepted;
}

}

bool SyncChatMarkOutranks(
		const SyncChatCache &a,
		const SyncChatCache &b) {
	const auto first = NormalizeCache(a, 0);
	const auto second = NormalizeCache(b, 0);
	return first && second && Newer(*first, *second);
}

SyncChatMessageOwnership GetSyncChatMessageOwnership(
		const SyncChatMessageFacts &facts) {
	using Ownership = SyncChatMessageOwnership;
	switch (facts.kind) {
	case SyncChatKind::Self:
	case SyncChatKind::Private:
	case SyncChatKind::Bot:
	case SyncChatKind::BasicGroup:
	case SyncChatKind::Supergroup:
	case SyncChatKind::Broadcast: break;
	case SyncChatKind::Secret:
	case SyncChatKind::ChannelDirectMessages: return Ownership::Other;
	default: return Ownership::Unknown;
	}
	if (facts.regular == false || facts.forwarded == true
		|| facts.viaBot == true || facts.scheduled == true
		|| facts.hasTtl == true || facts.sentAsOther == true) {
		return Ownership::Other;
	}
	if (!facts.regular || !facts.forwarded || !facts.viaBot
		|| !facts.scheduled || !facts.hasTtl || !facts.sentAsOther) {
		return Ownership::Unknown;
	}
	if (facts.kind == SyncChatKind::Self) {
		return Ownership::Own;
	}
	if (!facts.out) {
		return Ownership::Unknown;
	} else if (!*facts.out) {
		return Ownership::Other;
	}
	if (facts.kind == SyncChatKind::Private || facts.kind == SyncChatKind::Bot) {
		return Ownership::Own;
	} else if (facts.kind == SyncChatKind::Broadcast) {
		if (facts.broadcastOwnership != SyncChatBroadcastOwnership::CreatorAndSoleAdmin) {
			return Ownership::Unknown;
		} else if (facts.creator == false || facts.soleAdmin == false) {
			return Ownership::Other;
		} else if (!facts.creator || !facts.soleAdmin) {
			return Ownership::Unknown;
		}
		return Ownership::Own;
	}
	if (!facts.sender) {
		return Ownership::Unknown;
	}
	const auto sender = PeerTokenText(*facts.sender, facts.selfUserId);
	if (!sender) {
		return Ownership::Unknown;
	} else if (facts.sender->kind == PeerTokenKind::User && !facts.selfUserId) {
		return Ownership::Unknown;
	}
	return (*sender == u"self"_q) ? Ownership::Own : Ownership::Other;
}

bool IsOwnSyncChatMessage(const SyncChatMessageFacts &facts) {
	return GetSyncChatMessageOwnership(facts) == SyncChatMessageOwnership::Own;
}

SyncChatMarkParseResult ParseSyncChatMark(
		const QString &text,
		bool hasMedia,
		SyncChatMessageOwnership ownership) {
	if (hasMedia || ownership != SyncChatMessageOwnership::Own) {
		return {};
	}
	auto line = QString();
	const auto lines = text.split(u'\n');
	for (auto i = lines.crbegin(); i != lines.crend(); ++i) {
		line = i->trimmed().toCaseFolded();
		if (!line.isEmpty()) {
			break;
		}
	}
	const auto tag = u"#purplechatsync"_q;
	if (line == tag) {
		return { SyncChatMarkStatus::Supported, u"1"_q };
	}
	if (!line.startsWith(tag + u" v"_q)) {
		return {};
	}
	auto digits = line.mid(tag.size() + 2);
	if (digits.isEmpty()) {
		return {};
	}
	for (const auto ch : digits) {
		if (ch < u'0' || ch > u'9') {
			return {};
		}
	}
	auto first = qsizetype(0);
	while (first < digits.size() && digits[first] == u'0') {
		++first;
	}
	if (first == digits.size()) {
		return {};
	}
	digits = digits.mid(first);
	return {
		(digits == u"1"_q)
			? SyncChatMarkStatus::Supported : SyncChatMarkStatus::NewerVersion,
		std::move(digits),
	};
}

SyncChatEligibilityResult SyncChatEligibility(
		const SyncChatFacts &facts,
		SyncChatBroadcastOwnership broadcastOwnership) {
	auto readers = SyncChatReaderFacts();
	if (facts.kind == SyncChatKind::Private) {
		readers.privateRecipient = true;
		readers.warningRequired = true;
	} else if (facts.kind == SyncChatKind::Bot) {
		readers.botOwner = true;
		readers.warningRequired = true;
	} else if (facts.kind == SyncChatKind::BasicGroup
		|| facts.kind == SyncChatKind::Supergroup
		|| facts.kind == SyncChatKind::Broadcast) {
		readers.otherMembers = facts.otherMembers;
		readers.otherAdmins = facts.otherAdmins;
		readers.populationUnknown = !facts.otherMembers || !facts.otherAdmins;
		readers.warningRequired = readers.populationUnknown
			|| facts.otherMembers.value_or(0) != 0
			|| facts.otherAdmins.value_or(0) != 0;
	}
	const auto refuse = [&](SyncChatRefusal reason) {
		return SyncChatEligibilityResult{ reason, readers };
	};
	switch (facts.kind) {
	case SyncChatKind::Self:
	case SyncChatKind::Private:
	case SyncChatKind::Bot:
	case SyncChatKind::BasicGroup:
	case SyncChatKind::Supergroup:
	case SyncChatKind::Broadcast: break;
	case SyncChatKind::Secret: return refuse(SyncChatRefusal::Secret);
	case SyncChatKind::ChannelDirectMessages:
		return refuse(SyncChatRefusal::TopicRouting);
	default: return refuse(SyncChatRefusal::UnknownKind);
	}
	if (facts.migrated) {
		return refuse(SyncChatRefusal::Migrated);
	} else if (facts.forum) {
		return refuse(SyncChatRefusal::TopicRouting);
	} else if (facts.publicUsername) {
		return refuse(SyncChatRefusal::Public);
	} else if (facts.protectedContent) {
		return refuse(SyncChatRefusal::Protected);
	} else if (facts.linkedDiscussion) {
		return refuse(SyncChatRefusal::Discussion);
	} else if (facts.autoDelete) {
		return refuse(SyncChatRefusal::AutoDelete);
	} else if (facts.paidMessages) {
		return refuse(SyncChatRefusal::Paid);
	} else if (!facts.canSendFiles) {
		return refuse(SyncChatRefusal::CannotSendFiles);
	} else if (facts.anonymousAdmin || facts.postsAsOther) {
		return refuse(SyncChatRefusal::SendAsOther);
	} else if (facts.slowMode && !facts.admin) {
		return refuse(SyncChatRefusal::SlowMode);
	} else if (facts.kind == SyncChatKind::Broadcast) {
		switch (broadcastOwnership) {
		case SyncChatBroadcastOwnership::OutgoingVerified: break;
		case SyncChatBroadcastOwnership::CreatorAndSoleAdmin:
			if (!facts.creator || !facts.soleAdmin) {
				return refuse(SyncChatRefusal::BroadcastNeedsCreatorAndSoleAdmin);
			}
			break;
		default:
			return refuse(SyncChatRefusal::BroadcastOwnershipUnselected);
		}
	}
	return refuse(SyncChatRefusal::None);
}

SyncChatResolution ResolveSyncChat(const SyncChatResolveInput &input) {
	auto result = SyncChatResolution();
	if (input.searchRetriesCompleted < 0
		|| !PeerTokenText(PeerToken(), input.selfUserId)) {
		return result;
	}
	if (input.cache) {
		result.cache = NormalizeCache(*input.cache, input.selfUserId);
		if (!result.cache) {
			return result;
		}
	} else if (input.settingsBound) {
		result.cache = NormalizeCache(
			{ input.settingsChat, 0, 0 }, input.selfUserId);
		if (!result.cache) {
			return result;
		}
	}
	if (result.cache) {
		result.readChat = result.cache->chat;
	}
	if (input.cacheRead < SyncChatDirectRead::Unknown
		|| input.cacheRead > SyncChatDirectRead::Migrated
		|| input.settingsHint < SyncChatSettingsHint::NotRun
		|| input.settingsHint > SyncChatSettingsHint::Found) {
		return result;
	}
	auto marks = input.marks;
	auto identities = std::map<std::pair<QString, int32_t>, SyncChatMark>();
	for (auto &mark : marks) {
		const auto location = NormalizeCache(mark.location, input.selfUserId);
		if (!location || location->markId == 0
			|| mark.records < SyncChatRecordPresence::Unknown
			|| mark.records > SyncChatRecordPresence::Present
			|| (mark.status != SyncChatMarkStatus::Supported
				&& mark.status != SyncChatMarkStatus::NewerVersion)) {
			return result;
		}
		mark.location = *location;
		const auto identity = std::pair(
			*PeerTokenText(mark.location.chat), mark.location.markId);
		const auto [i, inserted] = identities.emplace(identity, mark);
		if (!inserted && !SameMarkFacts(i->second, mark)) {
			return result;
		}
	}
	std::sort(marks.begin(), marks.end(), [](const auto &a, const auto &b) {
		return SyncChatMarkOutranks(a.location, b.location);
	});
	const auto newest = std::find_if(marks.begin(), marks.end(),
		[](const auto &mark) { return bool(mark.eligibility); });
	if (newest != marks.end()
		&& newest->status == SyncChatMarkStatus::NewerVersion) {
		result.action = newest->freshlyRead
			? SyncChatAction::NewerMark : SyncChatAction::VerifyMark;
		result.winner = newest->location;
		return result;
	}
	const auto cacheUsable = result.cache
		&& (input.cacheRead == SyncChatDirectRead::Valid
			|| input.cacheRead == SyncChatDirectRead::Unknown);
	const auto useCache = cacheUsable
		&& (newest == marks.end()
			|| !SyncChatMarkOutranks(newest->location, *result.cache));
	if (useCache) {
		result.action = SyncChatAction::Use;
		result.winner = result.cache;
		Readers(result, input.cachedEligibility, input.readerWarningAccepted);
		result.postingHeld = !input.cachedEligibility
			|| result.needsReaderWarning
			|| input.cacheRead == SyncChatDirectRead::Unknown;
		return result;
	}
	if (newest == marks.end()) {
		if (result.cache) {
			Searching(result, input.searchRetriesCompleted);
		} else if (input.settingsHint == SyncChatSettingsHint::Found) {
			result.action = SyncChatAction::Use;
			result.cache = SyncChatCache();
			result.winner = result.cache;
			result.readChat = PeerToken();
			result.postingHeld = false;
		} else {
			result.action = SyncChatAction::Unset;
		}
		return result;
	}
	result.winner = newest->location;
	Readers(result, newest->eligibility, input.readerWarningAccepted);
	if (result.cache
		&& result.cache->chat != newest->location.chat
		&& !SyncChatMarkOutranks(newest->location, *result.cache)) {
		result.olderAlternative = newest->location.chat;
		Searching(result, input.searchRetriesCompleted);
		return result;
	}
	if (!newest->freshlyRead) {
		result.action = SyncChatAction::VerifyMark;
		return result;
	}
	if (result.cache && result.cache->chat != newest->location.chat) {
		result.action = SyncChatAction::Follow;
		result.source = result.cache->chat;
		return result;
	}
	if (!result.cache && newest->records != SyncChatRecordPresence::Present) {
		const auto source = std::find_if(marks.begin(), marks.end(),
			[&](const auto &mark) {
				return mark.eligibility && mark.readable
					&& mark.location.chat != newest->location.chat
					&& SyncChatMarkOutranks(newest->location, mark.location);
			});
		if (source != marks.end()) {
			result.source = source->location.chat;
		} else if (input.settingsHint == SyncChatSettingsHint::Found
			&& newest->location.chat != PeerToken()) {
			result.source = PeerToken();
		}
		if (result.source) {
			result.action = (newest->records == SyncChatRecordPresence::Unknown)
				? SyncChatAction::ReadWinnerInventory : SyncChatAction::Fill;
			return result;
		}
	}
	result.action = SyncChatAction::Use;
	result.cache = newest->location;
	result.readChat = newest->location.chat;
	result.postingHeld = result.needsReaderWarning;
	return result;
}

SyncChatMoveResult MoveSyncChat(
		const SyncLocalState &state,
		const PeerToken &newChat,
		bool stagePresent,
		uint64_t selfUserId) {
	if (!SerializeSyncLocalState(state)) {
		return { state, SyncChatMoveError::InvalidState };
	}
	const auto text = PeerTokenText(newChat, selfUserId);
	if (!text) {
		return { state, SyncChatMoveError::InvalidChat };
	}
	if (stagePresent || state.config.pendingSeq || state.library.pendingSeq
		|| !state.configData.pending.isEmpty()) {
		return { state, SyncChatMoveError::PendingStage };
	}
	const auto chat = *ParsePeerToken(*text);
	if (state.chat == chat) {
		return { state };
	}
	auto next = state;
	next.version = 2;
	next.chat = chat;
	next.ownMessages.clear();
	next.preserved.remove(u"own_messages"_q);
	if (!SerializeSyncLocalState(next)) {
		return { state, SyncChatMoveError::InvalidState };
	}
	return { std::move(next), SyncChatMoveError::None, true };
}

}
