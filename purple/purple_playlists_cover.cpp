/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists_cover.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace Purple {
namespace {

[[nodiscard]] PeerToken NormalizeChat(PeerToken chat, uint64_t selfUserId) {
	if (chat.kind == PeerTokenKind::User && selfUserId && chat.id == selfUserId) {
		return PeerToken{ PeerTokenKind::Self, 0 };
	}
	return chat;
}

[[nodiscard]] bool ValidChat(const PeerToken &chat, uint64_t selfUserId) {
	PlaylistLocalState state;
	state.chat = chat;
	return ValidatePlaylistLocalState(state, selfUserId).status
		== PlaylistLocalStatus::Valid;
}

[[nodiscard]] bool ValidHash(const std::optional<QString> &hash) {
	return hash && hash->size() == 64
		&& std::ranges::all_of(*hash, [](QChar ch) {
			return (ch >= u'0' && ch <= u'9') || (ch >= u'a' && ch <= u'f');
		});
}

[[nodiscard]] PlaylistRecordPruneResult Invalid(PlaylistRecordPruneError error) {
	PlaylistRecordPruneResult result;
	result.error = error;
	return result;
}

[[nodiscard]] std::optional<PlaylistRecordUnusableReason> ProofFailure(
		const PlaylistRecordPayloadProof &proof) {
	switch (proof.origin) {
	case PlaylistPayloadOrigin::DownloadedThisStep:
		if (!proof.readSucceeded) {
			return PlaylistRecordUnusableReason::ReadFailed;
		}
		return std::nullopt;
	case PlaylistPayloadOrigin::Cached:
		if (!ValidHash(proof.expectedCanonicalHash)) {
			return PlaylistRecordUnusableReason::InvalidCacheHash;
		}
		return std::nullopt;
	case PlaylistPayloadOrigin::PublishedThisStep:
		if (!proof.publicationAcknowledged) {
			return PlaylistRecordUnusableReason::PublicationUnacknowledged;
		}
		return std::nullopt;
	case PlaylistPayloadOrigin::Unproven:
		return PlaylistRecordUnusableReason::UnprovenPayload;
	}
	return PlaylistRecordUnusableReason::UnprovenPayload;
}

struct VerifiedRecord {
	PlaylistRecordIdentity identity;
	PlaylistLibrary library;
	QByteArray canonical;
	QString hash;
};

[[nodiscard]] std::optional<bool> Covers(
		const VerifiedRecord &outer,
		const VerifiedRecord &inner,
		uint64_t selfUserId) {
	if (outer.canonical == inner.canonical
			&& outer.identity.messageId <= inner.identity.messageId) {
		return false;
	}
	const auto contains = PlaylistLibraryContains(outer.library, inner.library, selfUserId);
	return contains.valid() ? std::optional<bool>(contains.contains) : std::nullopt;
}

} // namespace

PlaylistRecordPruneResult PlanPlaylistRecordPrunes(
		const PlaylistRecordPruneInput &input) {
	if (!input.presentSetChanged && !input.publicationSucceeded) {
		PlaylistRecordPruneResult result;
		result.status = PlaylistRecordPruneStatus::Skipped;
		return result;
	}
	if (!input.currentChat || !ValidChat(*input.currentChat, input.selfUserId)) {
		return Invalid(PlaylistRecordPruneError::InvalidChat);
	}
	if (input.inventory.status != PlaylistInventoryStatus::Valid
			|| !input.inventory.normalizedCurrentChat
			|| !ValidChat(*input.inventory.normalizedCurrentChat, input.selfUserId)) {
		return Invalid(PlaylistRecordPruneError::InvalidInventory);
	}
	const auto chat = NormalizeChat(*input.currentChat, input.selfUserId);
	if (NormalizeChat(*input.inventory.normalizedCurrentChat, input.selfUserId) != chat) {
		return Invalid(PlaylistRecordPruneError::BindingMismatch);
	}
	std::map<int32_t, PlaylistInventoryPresentCandidate> present;
	for (const auto &candidate : input.inventory.presentCandidates) {
		if (candidate.messageId <= 0 || candidate.documentId == 0) {
			return Invalid(PlaylistRecordPruneError::InvalidIdentity);
		}
		const auto [it, added] = present.emplace(candidate.messageId, candidate);
		if (!added && (it->second.documentId != candidate.documentId
				|| it->second.meta != candidate.meta)) {
			return Invalid(PlaylistRecordPruneError::ConflictingPresence);
		}
	}
	std::set<int32_t> excluded(input.inventory.unknownOwnershipIds.begin(),
		input.inventory.unknownOwnershipIds.end());
	excluded.insert(input.inventory.conflictingIds.begin(), input.inventory.conflictingIds.end());
	std::map<int32_t, std::vector<PlaylistRecordPayloadProof>> proofs;
	for (const auto &proof : input.payloads) {
		if (proof.messageId > 0
				&& proof.messageId <= std::numeric_limits<int32_t>::max()
				&& present.contains(int32_t(proof.messageId))) {
			auto normalized = proof;
			if (normalized.chat) {
				normalized.chat = NormalizeChat(*normalized.chat, input.selfUserId);
			}
			proofs[int32_t(proof.messageId)].push_back(std::move(normalized));
		}
	}
	PlaylistRecordPruneResult result;
	result.status = PlaylistRecordPruneStatus::Planned;
	std::vector<VerifiedRecord> verified;
	for (const auto &[id, candidate] : present) {
		const auto reject = [&](PlaylistRecordUnusableReason reason) {
			result.unusable.push_back(PlaylistRecordUnusable{ id, reason });
		};
		if (excluded.contains(id)) {
			reject(PlaylistRecordUnusableReason::ExcludedPresence);
			continue;
		}
		if (!IsPlaylistRecordCandidate(candidate.meta, SyncChatMessageOwnership::Own)) {
			reject(PlaylistRecordUnusableReason::UnrecognizedRecord);
			continue;
		}
		const auto it = proofs.find(id);
		if (it == proofs.end()) {
			reject(PlaylistRecordUnusableReason::MissingPayload);
			continue;
		}
		const auto &proof = it->second.front();
		if (std::ranges::any_of(it->second, [&](const auto &other) { return other != proof; })) {
			reject(PlaylistRecordUnusableReason::ConflictingPayload);
			continue;
		}
		if (!proof.chat || !ValidChat(*proof.chat, input.selfUserId)
				|| *proof.chat != chat || proof.documentId != candidate.documentId) {
			reject(PlaylistRecordUnusableReason::IdentityMismatch);
			continue;
		}
		if (const auto failure = ProofFailure(proof)) {
			reject(*failure);
			continue;
		}
		auto classified = ClassifyPlaylistRecord(PlaylistRecordInput{
			.messageId = id,
			.documentId = candidate.documentId,
			.ownership = SyncChatMessageOwnership::Own,
			.meta = candidate.meta,
			.readOutcome = PlaylistRecordReadOutcome::Succeeded,
			.compressedBytes = proof.compressedBytes,
			.selfUserId = input.selfUserId,
		});
		if (!classified) {
			reject(classified.status == PlaylistRecordStatus::NewerFormat
				? PlaylistRecordUnusableReason::NewerFormat
				: classified.status == PlaylistRecordStatus::Oversized
				? PlaylistRecordUnusableReason::Oversized
				: PlaylistRecordUnusableReason::InvalidPayload);
			continue;
		}
		if (proof.origin == PlaylistPayloadOrigin::Cached
				&& classified.canonicalHash != proof.expectedCanonicalHash) {
			reject(PlaylistRecordUnusableReason::CacheHashMismatch);
			continue;
		}
		verified.push_back(VerifiedRecord{
			.identity = { id, candidate.documentId },
			.library = std::move(*classified.library),
			.canonical = std::move(*classified.canonicalBytes),
			.hash = std::move(*classified.canonicalHash),
		});
	}
	std::vector<bool> maximal(verified.size(), true);
	for (auto i = std::size_t(0); i != verified.size(); ++i) {
		for (auto j = std::size_t(0); j != verified.size(); ++j) {
			if (i == j) {
				continue;
			}
			const auto covers = Covers(verified[j], verified[i], input.selfUserId);
			if (!covers) {
				return Invalid(PlaylistRecordPruneError::ContainmentFailed);
			}
			if (*covers) {
				maximal[i] = false;
				break;
			}
		}
	}
	for (auto i = std::size_t(0); i != verified.size(); ++i) {
		if (maximal[i]) {
			result.survivors.push_back(verified[i].identity);
			continue;
		}
		auto witnessed = false;
		// WHY: A direct maximal witness survives this plan, so execution order
		// never makes a deletion depend on another planned deletion.
		for (auto j = verified.size(); j != 0; --j) {
			if (!maximal[j - 1]) {
				continue;
			}
			const auto &witness = verified[j - 1];
			const auto covers = Covers(witness, verified[i], input.selfUserId);
			if (!covers) {
				return Invalid(PlaylistRecordPruneError::ContainmentFailed);
			}
			if (*covers) {
				result.prune.push_back(PlaylistRecordPrune{
					.target = verified[i].identity,
					.witness = witness.identity,
					.witnessCanonicalHash = witness.hash,
				});
				witnessed = true;
				break;
			}
		}
		if (!witnessed) {
			return Invalid(PlaylistRecordPruneError::ContainmentFailed);
		}
	}
	return result;
}

} // namespace Purple
