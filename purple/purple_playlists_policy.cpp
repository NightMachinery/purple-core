/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists_policy.h"

#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>

namespace Purple {
namespace {

[[nodiscard]] std::optional<QString> TruncateValidMetaText(
		const QString &value,
		qsizetype maxCodePoints) {
	auto count = qsizetype(0);
	auto cutIndex = qsizetype(0);
	for (auto i = qsizetype(0); i < value.size(); ++i) {
		const auto code = value[i].unicode();
		if (code >= 0xDC00 && code <= 0xDFFF) {
			return std::nullopt;
		}
		auto unitCount = qsizetype(1);
		if (code >= 0xD800 && code <= 0xDBFF) {
			if (i + 1 >= value.size()) {
				return std::nullopt;
			}
			const auto second = value[i + 1].unicode();
			if (second < 0xDC00 || second > 0xDFFF) {
				return std::nullopt;
			}
			++i;
			unitCount = 2;
		}
		++count;
		if (count <= maxCodePoints) {
			cutIndex += unitCount;
		}
	}
	auto result = value.left(cutIndex);
	if (!IsValidPlaylistMetaText(result)) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] bool IsPlaylistDueForAutoPurge(
		const Playlist &playlist,
		uint64_t nowMs) {
	if (!playlist.deleted || !playlist.deleted->value) {
		return false;
	}
	const auto stamp = playlist.deleted->stamp;
	return nowMs >= stamp && (nowMs - stamp) >= kAutoPurgeRetentionMs;
}

[[nodiscard]] bool IsDiscoveryComplete(
		const PlaylistAutoPurgeContext &context) {
	if (!context.listingPagedToEnd
		|| !context.missingMemoEntriesResolved
		|| !context.allCandidatesClassifiedOrRead
		|| !context.noTransientReadFailure
		|| !context.noUnknownOwnership) {
		return false;
	}
	if (context.vanishedMemoRecordCount > 0 && context.newValidRecordCount == 0) {
		return false;
	}
	return true;
}

[[nodiscard]] PlaylistAutoPurgeResult BlockedResult(
		PlaylistAutoPurgeStatus status,
		const PlaylistLibrary &library,
		const std::map<QString, PlaylistAutoPurgeDueSeen> &dueSeen) {
	return { status, library, dueSeen, {} };
}

} // namespace

SongQualificationResult QualifySong(
		const PlaylistSongCandidate &candidate,
		bool protectedContentAllowed) {
	if (candidate.documentId == 0) {
		return { SongQualificationReason::InvalidDocumentId };
	}
	if (candidate.messageId < 1 || candidate.messageId > kMaximumPlaylistMessageId) {
		return { SongQualificationReason::InvalidMessageId };
	}
	if (candidate.isVoiceMessage) {
		return { SongQualificationReason::VoiceMessage };
	}
	if (!candidate.hasAudioAttribute) {
		return { SongQualificationReason::MissingAudioAttribute };
	}
	if (candidate.isScheduled) {
		return { SongQualificationReason::ScheduledMessage };
	}
	if (candidate.isLocalUnsent) {
		return { SongQualificationReason::LocalUnsentMessage };
	}
	if (candidate.isSponsored) {
		return { SongQualificationReason::SponsoredMessage };
	}
	if (candidate.isStory) {
		return { SongQualificationReason::StoryMessage };
	}
	if (candidate.isProfileMusic) {
		return { SongQualificationReason::ProfileMusicItem };
	}
	if (candidate.isSecretChat) {
		return { SongQualificationReason::SecretChatMessage };
	}
	if (!candidate.isRegularServerMessage) {
		return { SongQualificationReason::NotRegularServerMessage };
	}
	if (candidate.isRichMediaSubtrack) {
		return { SongQualificationReason::RichMediaSubtrack };
	}
	if (!candidate.isMainMessageMedia) {
		return { SongQualificationReason::NotMainMessageMedia };
	}
	if (candidate.hasSelfDestructTimer) {
		return { SongQualificationReason::SelfDestructTimer };
	}
	if (candidate.isProtectedContent && !protectedContentAllowed) {
		return { SongQualificationReason::ProtectedContentRestricted };
	}
	return { SongQualificationReason::Qualified };
}

SongMetaPrepareResult PrepareSongMeta(const PlaylistRawSongMeta &raw) {
	if (raw.durationSeconds < 0
		|| raw.durationSeconds > int64_t(kMaximumPlaylistDurationSeconds)) {
		return { SongMetaPrepareError::InvalidDuration, {} };
	}
	const auto &rawTitle = raw.title.isEmpty() ? raw.fileName : raw.title;
	const auto title = TruncateValidMetaText(
		rawTitle,
		kMaximumPlaylistMetaCodePoints);
	if (!title) {
		return { SongMetaPrepareError::InvalidTitleUnicode, {} };
	}
	const auto performer = TruncateValidMetaText(
		raw.performer,
		kMaximumPlaylistMetaCodePoints);
	if (!performer) {
		return { SongMetaPrepareError::InvalidPerformerUnicode, {} };
	}
	return {
		SongMetaPrepareError::None,
		PlaylistSongMeta{
			*title,
			*performer,
			int32_t(raw.durationSeconds),
		},
	};
}

PlaylistChatOrderResult SortPlaylistChatOrder(
		const std::vector<PlaylistChatOrderItem> &items) {
	for (const auto &item : items) {
		if (item.documentId == 0) {
			return { PlaylistChatOrderError::InvalidDocumentId, {} };
		}
		if (item.messageId < 1 || item.messageId > kMaximumPlaylistMessageId) {
			return { PlaylistChatOrderError::InvalidMessageId, {} };
		}
	}
	auto sorted = items;
	std::sort(
		sorted.begin(),
		sorted.end(),
		[](const PlaylistChatOrderItem &a, const PlaylistChatOrderItem &b) {
			return std::tuple(!a.fromMigratedBasicGroup, a.messageId, a.documentId)
				< std::tuple(!b.fromMigratedBasicGroup, b.messageId, b.documentId);
		});
	return { PlaylistChatOrderError::None, std::move(sorted) };
}

PlaylistAutoPurgeResult AutoPurgePlaylists(
		const PlaylistLibrary &mergedLibrary,
		const std::map<QString, PlaylistAutoPurgeDueSeen> &dueSeen,
		const PlaylistAutoPurgeContext &context) {
	const auto valid = ValidatePlaylistLibrary(mergedLibrary, context.selfUserId);
	if (valid.status == PlaylistLibraryStatus::NewerFormat) {
		return BlockedResult(
			PlaylistAutoPurgeStatus::NewerFormatLibrary,
			mergedLibrary,
			dueSeen);
	}
	if (!valid) {
		return BlockedResult(
			PlaylistAutoPurgeStatus::InvalidLibrary,
			mergedLibrary,
			dueSeen);
	}
	if (!context.serverRequestSucceeded) {
		return BlockedResult(
			PlaylistAutoPurgeStatus::BlockedServerRequestRequired,
			mergedLibrary,
			dueSeen);
	}
	if (!context.syncEnabled) {
		auto nextLibrary = mergedLibrary;
		auto nextDueSeen = std::map<QString, PlaylistAutoPurgeDueSeen>();
		auto purgedIds = std::vector<QString>();
		for (const auto &[id, playlist] : mergedLibrary.playlists) {
			if (IsPlaylistDueForAutoPurge(playlist, context.nowMs)) {
				nextLibrary.playlists.erase(id);
				nextLibrary.purged.insert(id);
				purgedIds.push_back(id);
			}
		}
		return {
			PlaylistAutoPurgeStatus::Completed,
			std::move(nextLibrary),
			std::move(nextDueSeen),
			std::move(purgedIds),
		};
	}
	if (!IsDiscoveryComplete(context)) {
		return BlockedResult(
			PlaylistAutoPurgeStatus::BlockedIncompleteDiscovery,
			mergedLibrary,
			dueSeen);
	}
	if (!context.noPresentNewerFormatRecord) {
		// Unreadable newer-format records may contain a restore, so reset dueSeen conservatively.
		return {
			PlaylistAutoPurgeStatus::BlockedNewerFormatRecord,
			mergedLibrary,
			{},
			{},
		};
	}

	auto nextLibrary = mergedLibrary;
	auto nextDueSeen = std::map<QString, PlaylistAutoPurgeDueSeen>();
	auto purgedIds = std::vector<QString>();
	for (const auto &[id, playlist] : mergedLibrary.playlists) {
		if (!IsPlaylistDueForAutoPurge(playlist, context.nowMs)) {
			continue;
		}
		const auto deletionStamp = playlist.deleted->stamp;
		const auto existing = dueSeen.find(id);
		if (existing != dueSeen.end()
			&& existing->second.deletionStamp == deletionStamp
			&& IsPlaylistDueForAutoPurge(playlist, existing->second.firstSeenAtMs)) {
			const auto firstSeen = existing->second.firstSeenAtMs;
			if (context.nowMs >= firstSeen
				&& (context.nowMs - firstSeen) >= kAutoPurgeMinimumObservationMs) {
				nextLibrary.playlists.erase(id);
				nextLibrary.purged.insert(id);
				purgedIds.push_back(id);
			} else {
				nextDueSeen.emplace(id, existing->second);
			}
		} else {
			nextDueSeen.emplace(
				id,
				PlaylistAutoPurgeDueSeen{ deletionStamp, context.nowMs });
		}
	}
	return {
		PlaylistAutoPurgeStatus::Completed,
		std::move(nextLibrary),
		std::move(nextDueSeen),
		std::move(purgedIds),
	};
}

} // namespace Purple
