/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_sync_chat.h"

#include "purple/purple_types.h"

#include <utility>

namespace Purple {

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

SyncChatEligibilityResult SyncChatEligibility(
		const SyncChatFacts &facts,
		SyncChatBroadcastOwnership broadcastOwnership) {
	const auto refuse = [](SyncChatRefusal reason) {
		return SyncChatEligibilityResult{ reason };
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
