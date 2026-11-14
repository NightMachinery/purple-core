/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists_step.h"

#include <QtCore/QCryptographicHash>

#include <algorithm>
#include <map>
#include <utility>

namespace Purple {
namespace {

[[nodiscard]] PlaylistStepResult Blocked(PlaylistStepError error) {
	PlaylistStepResult result;
	result.error = error;
	return result;
}

[[nodiscard]] PeerToken NormalizeChat(PeerToken chat, uint64_t selfUserId) {
	if (chat.kind == PeerTokenKind::User && chat.id == selfUserId) {
		return { PeerTokenKind::Self, 0 };
	}
	return chat;
}

[[nodiscard]] bool ValidHash(const std::optional<QString> &hash) {
	return hash && hash->size() == 64
		&& std::ranges::all_of(*hash, [](QChar ch) {
			return (ch >= u'0' && ch <= u'9') || (ch >= u'a' && ch <= u'f');
		});
}

[[nodiscard]] bool ValidProof(const PlaylistRecordPayloadProof &proof) {
	switch (proof.origin) {
	case PlaylistPayloadOrigin::DownloadedThisStep:
		return proof.readSucceeded;
	case PlaylistPayloadOrigin::Cached:
		return ValidHash(proof.expectedCanonicalHash);
	case PlaylistPayloadOrigin::PublishedThisStep:
		return proof.publicationAcknowledged;
	case PlaylistPayloadOrigin::Unproven:
		return false;
	}
	return false;
}

[[nodiscard]] bool ClockSucceeded(
		const PlaylistStepInput &input,
		const std::optional<PeerToken> &chat) {
	return input.clock.requestSucceeded
		&& input.clock.selfUserId == input.selfUserId
		&& input.clock.nowMs >= kMinimumPlaylistStamp
		&& input.clock.nowMs <= kMaximumPlaylistStamp
		&& (!chat || (input.clock.chat
			&& NormalizeChat(*input.clock.chat, input.selfUserId) == *chat));
}

void CountMemo(PlaylistStepRecordCounts &counts, PlaylistLocalMemoState state) {
	switch (state) {
	case PlaylistLocalMemoState::Valid: ++counts.valid; break;
	case PlaylistLocalMemoState::Invalid: ++counts.invalid; break;
	case PlaylistLocalMemoState::NewerFormat: ++counts.newerFormat; break;
	case PlaylistLocalMemoState::Oversized: ++counts.oversized; break;
	}
}

[[nodiscard]] bool SamePresentSet(
		const PlaylistInventoryResult &inventory,
		const std::map<int32_t, PlaylistLocalMemoEntry> &memo) {
	if (inventory.bindingChanged || inventory.presentCandidates.size() != memo.size()) {
		return false;
	}
	return std::ranges::all_of(inventory.presentCandidates, [&](const auto &candidate) {
		const auto it = memo.find(candidate.messageId);
		return it != memo.end() && it->second.documentId == candidate.documentId;
	});
}

void UpdateDueSeen(PlaylistLocalState &state,
		const std::map<QString, PlaylistAutoPurgeDueSeen> &values) {
	for (auto it = state.dueSeen.begin(); it != state.dueSeen.end();) {
		if (!values.contains(it->first)) {
			it = state.dueSeen.erase(it);
		} else {
			++it;
		}
	}
	for (const auto &[id, value] : values) {
		state.dueSeen[id].value = value;
	}
}

void PlanPublication(PlaylistStepResult &result, bool localOnly,
		bool listingSucceeded, bool anyRecords, bool equalRecord) {
	if (localOnly) {
		result.publication = PlaylistStepPublication::SyncOff;
	} else if (!listingSucceeded) {
		result.publication = PlaylistStepPublication::DiscoveryUnavailable;
	} else if (equalRecord) {
		result.publication = PlaylistStepPublication::EqualRecord;
	} else if (!anyRecords && result.library->playlists.empty()
			&& result.library->purged.empty()) {
		result.publication = PlaylistStepPublication::InitialEmpty;
	} else if (result.canonicalBytes.size() > kPlaylistRecordCodecCaps.maximumUncompressedBytes) {
		result.publication = PlaylistStepPublication::ExpandedLimit;
	} else {
		auto encoded = EncodePlaylistBytes(result.canonicalBytes, kPlaylistRecordCodecCaps);
		if (encoded) {
			result.publication = PlaylistStepPublication::Required;
			result.publicationBytes = std::move(encoded.bytes);
		} else {
			result.publication = encoded.error == PlaylistCodecError::CompressedSizeLimit
				? PlaylistStepPublication::CompressedLimit
				: PlaylistStepPublication::EncodingFailed;
		}
	}
}

} // namespace

PlaylistStepResult PlanPlaylistStep(const PlaylistStepInput &input) {
	if (!input.accountAuthorized) {
		return Blocked(PlaylistStepError::Unauthorized);
	}
	if (!input.selfUserId || !ValidatePlaylistLocalState(PlaylistLocalState(), input.selfUserId)) {
		return Blocked(PlaylistStepError::InvalidSelfUserId);
	}
	if (input.libraryRead != PlaylistStepLocalRead::Missing
			&& input.libraryRead != PlaylistStepLocalRead::Succeeded) {
		return Blocked(PlaylistStepError::LibraryReadFailed);
	}
	const auto before = input.libraryRead == PlaylistStepLocalRead::Missing
		? PlaylistLibrary() : input.library;
	const auto beforeBytes = SerializePlaylistLibrary(before, input.selfUserId);
	if (!beforeBytes) {
		return Blocked(beforeBytes.status == PlaylistLibraryStatus::NewerFormat
			? PlaylistStepError::NewerLibrary : PlaylistStepError::InvalidLibrary);
	}
	if (input.stateRead != PlaylistStepLocalRead::Missing
			&& input.stateRead != PlaylistStepLocalRead::Succeeded) {
		return Blocked(PlaylistStepError::StateReadFailed);
	}
	const auto originalState = input.stateRead == PlaylistStepLocalRead::Missing
		? PlaylistLocalState() : input.state;
	const auto validatedState = ValidatePlaylistLocalState(originalState, input.selfUserId);
	if (!validatedState) {
		return Blocked(validatedState.status == PlaylistLocalStatus::NewerFormat
			? PlaylistStepError::NewerState : PlaylistStepError::InvalidState);
	}
	auto localOnly = false;
	switch (input.trigger) {
	case PlaylistStepTrigger::Automatic:
		if (!originalState.sync) {
			PlaylistStepResult result;
			result.status = PlaylistStepStatus::Skipped;
			return result;
		}
		break;
	case PlaylistStepTrigger::Manual:
	case PlaylistStepTrigger::Logout:
		break;
	case PlaylistStepTrigger::LocalPurge:
		if (originalState.sync) {
			PlaylistStepResult result;
			result.status = PlaylistStepStatus::Skipped;
			return result;
		}
		localOnly = true;
		break;
	default:
		return Blocked(PlaylistStepError::InvalidTrigger);
	}

	PlaylistStepResult result;
	result.status = PlaylistStepStatus::Planned;
	auto state = originalState;
	auto merged = before;
	auto newValid = std::size_t(0);
	auto equalHashes = std::vector<QString>();
	auto publicationAcknowledged = false;
	auto presentSetChanged = false;
	auto boundPayloads = std::vector<PlaylistRecordPayloadProof>();
	if (!localOnly) {
		if (input.discovery.selfUserId != input.selfUserId) {
			return Blocked(PlaylistStepError::AccountMismatch);
		}
		auto discovery = input.discovery;
		discovery.priorChat = state.chat;
		discovery.priorMemo = input.libraryRead == PlaylistStepLocalRead::Missing
			? std::map<int32_t, PlaylistLocalMemoEntry>() : state.memo;
		result.inventory = ReconcilePlaylistInventory(discovery);
		if (!result.inventory) {
			return Blocked(PlaylistStepError::InvalidInventory);
		}
		presentSetChanged = !SamePresentSet(result.inventory, discovery.priorMemo)
			|| !result.inventory.readCandidates.empty();
		if (result.inventory.bindingChanged) {
			result.retiredMemo = originalState.memo;
			state.dueSeen.clear();
		}
		state.chat = result.inventory.normalizedCurrentChat;
		state.memo = result.inventory.unresolvedPriorMemo;
		result.records.unreadable = result.inventory.unknownOwnershipIds.size()
			+ result.inventory.conflictingIds.size()
			+ result.inventory.pendingByIdIds.size();
		for (const auto &entry : result.inventory.reusableMemo) {
			state.memo[entry.messageId] = entry.entry;
			CountMemo(result.records, entry.entry.state);
			if (entry.entry.state == PlaylistLocalMemoState::Valid) {
				equalHashes.push_back(*entry.entry.canonicalHash);
			}
		}
		std::map<int64_t, std::vector<PlaylistRecordPayloadProof>> proofs;
		for (auto proof : input.payloads) {
			if (proof.chat) {
				proof.chat = NormalizeChat(*proof.chat, input.selfUserId);
			}
			if (proof.origin == PlaylistPayloadOrigin::Cached) {
				const auto memo = (proof.messageId > 0 && proof.messageId <= kMaximumPlaylistMessageId)
					? discovery.priorMemo.find(int32_t(proof.messageId))
					: discovery.priorMemo.end();
				if (result.inventory.bindingChanged || proof.chat != state.chat
						|| memo == discovery.priorMemo.end()
						|| memo->second.state != PlaylistLocalMemoState::Valid
						|| memo->second.documentId != proof.documentId
						|| memo->second.canonicalHash != proof.expectedCanonicalHash) {
					proof.origin = PlaylistPayloadOrigin::Unproven;
				}
			}
			proofs[proof.messageId].push_back(proof);
			boundPayloads.push_back(std::move(proof));
		}
		for (const auto &candidate : result.inventory.readCandidates) {
			const auto retry = [&] {
				++result.records.unreadable;
				result.retryReads.push_back({ candidate.messageId, candidate.documentId });
				if (!result.inventory.bindingChanged && discovery.priorMemo.contains(candidate.messageId)) {
					state.memo[candidate.messageId] = discovery.priorMemo.at(candidate.messageId);
				}
			};
			const auto found = proofs.find(candidate.messageId);
			if (found == proofs.end()) {
				retry();
				continue;
			}
			const auto &proof = found->second.front();
			if (proof.chat != state.chat || proof.documentId != candidate.documentId
					|| !ValidProof(proof)
					|| std::ranges::any_of(found->second, [&](const auto &other) {
						return other != proof;
					})) {
				retry();
				continue;
			}
			auto classified = ClassifyPlaylistRecord(PlaylistRecordInput{
				.messageId = candidate.messageId,
				.documentId = candidate.documentId,
				.ownership = SyncChatMessageOwnership::Own,
				.meta = candidate.meta,
				.readOutcome = PlaylistRecordReadOutcome::Succeeded,
				.compressedBytes = proof.compressedBytes,
				.selfUserId = input.selfUserId,
			});
			if (proof.origin == PlaylistPayloadOrigin::Cached
					&& classified.canonicalHash != proof.expectedCanonicalHash) {
				retry();
				continue;
			}
			const auto memo = PlaylistRecordMemoEntry(classified);
			if (!memo) {
				retry();
				continue;
			}
			state.memo[candidate.messageId] = *memo;
			CountMemo(result.records, memo->state);
			if (classified) {
				auto joined = JoinPlaylistLibraries(merged, *classified.library, input.selfUserId);
				if (!joined) {
					return Blocked(PlaylistStepError::MergeFailed);
				}
				merged = std::move(joined.library);
				++newValid;
				++result.newlyMergedRecords;
				equalHashes.push_back(*classified.canonicalHash);
				publicationAcknowledged |= proof.origin == PlaylistPayloadOrigin::PublishedThisStep;
			}
		}
		for (const auto &candidate : result.inventory.presentCandidates) {
			publicationAcknowledged |= std::ranges::any_of(boundPayloads, [&](const auto &proof) {
				return proof.origin == PlaylistPayloadOrigin::PublishedThisStep
					&& proof.publicationAcknowledged && proof.chat
					&& NormalizeChat(*proof.chat, input.selfUserId) == *state.chat
					&& proof.messageId == candidate.messageId
					&& proof.documentId == candidate.documentId;
			});
		}
		result.discoveryComplete = result.inventory.metadataComplete
			&& result.records.unreadable == 0;
	}

	const auto clockOk = ClockSucceeded(input,
		localOnly ? std::nullopt : result.inventory.normalizedCurrentChat);
	auto purge = AutoPurgePlaylists(merged, PlaylistLocalDueSeenValues(state),
		PlaylistAutoPurgeContext{
			.syncEnabled = originalState.sync,
			.nowMs = input.clock.nowMs,
			.selfUserId = input.selfUserId,
			.serverRequestSucceeded = clockOk,
			.listingPagedToEnd = !localOnly && input.discovery.listingSucceeded
				&& input.discovery.listingExhausted,
			.missingMemoEntriesResolved = !localOnly && result.inventory.pendingByIdIds.empty()
				&& result.inventory.conflictingIds.empty(),
			.allCandidatesClassifiedOrRead = result.records.unreadable == 0,
			.noTransientReadFailure = result.records.unreadable == 0,
			.noUnknownOwnership = !localOnly && result.inventory.unknownOwnershipIds.empty(),
			.vanishedMemoRecordCount = result.inventory.vanishedMemoRecordCount,
			.newValidRecordCount = newValid,
			.noPresentNewerFormatRecord = result.records.newerFormat == 0,
		});
	result.purgeStatus = purge.status;
	result.purgedIds = std::move(purge.purgedIds);
	UpdateDueSeen(state, purge.dueSeen);
	merged = std::move(purge.library);
	const auto finalBytes = SerializePlaylistLibrary(merged, input.selfUserId);
	if (!finalBytes) {
		return Blocked(PlaylistStepError::MergeFailed);
	}
	result.diff = DiffPlaylistLibraries(before, merged, input.selfUserId);
	if (!result.diff) {
		return Blocked(PlaylistStepError::DiffFailed);
	}
	for (const auto &notice : result.diff.notices) {
		auto &hidden = state.hidden[notice.playlistId];
		hidden.reason = notice.reason;
		hidden.playlist = notice.preMergePlaylist;
	}
	result.writeLibrary = finalBytes.canonical != beforeBytes.canonical
		|| (input.libraryRead == PlaylistStepLocalRead::Missing && newValid > 0);
	result.writeState = state != originalState;
	result.library = std::move(merged);
	result.state = std::move(state);
	result.canonicalBytes = finalBytes.canonical;
	result.canonicalHash = QString::fromLatin1(QCryptographicHash::hash(
		result.canonicalBytes, QCryptographicHash::Sha256).toHex());
	const auto equalRecord = std::ranges::find(equalHashes, result.canonicalHash)
		!= equalHashes.end();
	const auto anyRecords = !result.inventory.presentCandidates.empty()
		|| !result.inventory.unknownOwnershipIds.empty() || !result.inventory.conflictingIds.empty();
	PlanPublication(result, localOnly, input.discovery.listingSucceeded, anyRecords, equalRecord);
	if (!localOnly) {
		result.prune = PlanPlaylistRecordPrunes(PlaylistRecordPruneInput{
			.selfUserId = input.selfUserId,
			.currentChat = result.inventory.normalizedCurrentChat,
			.inventory = result.inventory,
			.presentSetChanged = presentSetChanged || result.publish(),
			.publicationSucceeded = publicationAcknowledged,
			.payloads = std::move(boundPayloads),
		});
	} else {
		result.prune.status = PlaylistRecordPruneStatus::Skipped;
	}
	result.steadyState = !localOnly && !presentSetChanged
		&& result.inventory.readCandidates.empty() && result.discoveryComplete
		&& !result.writeLibrary && !result.writeState && equalRecord
		&& result.prune.status == PlaylistRecordPruneStatus::Skipped;
	return result;
}

} // namespace Purple
