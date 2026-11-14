/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists_sync.h"

#include "purple/purple_playlists_detail.h"
#include "purple/purple_types.h"

#include <QtCore/QCryptographicHash>

#include <algorithm>
#include <limits>
#include <utility>

namespace Purple {
namespace {

inline constexpr int64_t kMaximumRecordMessageId =
	std::numeric_limits<int32_t>::max();

[[nodiscard]] bool MatchesSettingsExclusion(
		const PlaylistRecordMessageMeta &meta) {
	if (meta.caption.contains(u"#purplesync"_q)) {
		return true;
	}
	return std::ranges::any_of(meta.fileNames, [](const QString &name) {
		return name == u"Purple settings sync.json"_q
			|| name == u"Purple playlists sync.json"_q;
	});
}

[[nodiscard]] bool HasAnyPlaylistMarker(
		const PlaylistRecordMessageMeta &meta) {
	if (meta.caption.contains(PlaylistRecordCaptionTag())) {
		return true;
	}
	const auto expectedFile = PlaylistRecordFileName();
	return std::ranges::any_of(meta.fileNames, [&](const QString &name) {
		return name == expectedFile;
	});
}

[[nodiscard]] bool MatchesStrictPlaylistShape(
		const PlaylistRecordMessageMeta &meta) {
	if (meta.fileNames.empty()) {
		return false;
	}
	const auto expectedFile = PlaylistRecordFileName();
	return std::ranges::all_of(meta.fileNames, [&](const QString &name) {
		return name == expectedFile;
	});
}

[[nodiscard]] bool IsLowercaseSha256Hex(const QString &value) {
	if (value.size() != 64) {
		return false;
	}
	for (const auto ch : value) {
		const bool isDigit = (ch >= u'0' && ch <= u'9');
		const bool isLowerHex = (ch >= u'a' && ch <= u'f');
		if (!isDigit && !isLowerHex) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] PlaylistRecordClassification FinalizeClassification(
		PlaylistRecordClassification result) {
	if (result.status != PlaylistRecordStatus::Valid) {
		result.library.reset();
		result.canonicalBytes.reset();
		result.compressedBytes.reset();
		result.canonicalHash.reset();
	}
	result.memoEntry = PlaylistRecordMemoEntry(result);
	return result;
}

[[nodiscard]] PlaylistRecordError ErrorForReadOutcome(
		PlaylistRecordReadOutcome outcome) {
	switch (outcome) {
	case PlaylistRecordReadOutcome::Failed:
		return PlaylistRecordError::ReadFailed;
	case PlaylistRecordReadOutcome::Vanished:
		return PlaylistRecordError::ReadVanished;
	case PlaylistRecordReadOutcome::Inaccessible:
		return PlaylistRecordError::ReadInaccessible;
	case PlaylistRecordReadOutcome::Cancelled:
		return PlaylistRecordError::ReadCancelled;
	case PlaylistRecordReadOutcome::Succeeded:
		return PlaylistRecordError::None;
	}
	return PlaylistRecordError::ReadFailed;
}

} // namespace

QString PlaylistRecordFileName() {
	return u"Purple playlists.json.gz"_q;
}

QString PlaylistRecordMimeType() {
	return u"application/gzip"_q;
}

QString PlaylistRecordCaption() {
	return u"Purple playlists sync data #purpleplaylists"_q;
}

QString PlaylistRecordCaptionTag() {
	return u"#purpleplaylists"_q;
}

PlaylistRecordCandidateVerdict InspectPlaylistRecordMetadata(
		const PlaylistRecordMessageMeta &meta,
		SyncChatMessageOwnership ownership) {
	// WHY: Settings records hold exact claim on #purplesync and the two legacy
	// .json names; colliding markers must fail closed as ambiguous rather than
	// becoming candidates or leaking into unknown-ownership incomplete state.
	if (!meta.isRegularMessage || meta.forwarded || !meta.isDocument) {
		return PlaylistRecordCandidateVerdict::Ignored;
	}
	const bool settingsExcluded = MatchesSettingsExclusion(meta);
	if (settingsExcluded && HasAnyPlaylistMarker(meta)) {
		return PlaylistRecordCandidateVerdict::AmbiguousSettings;
	}
	if (settingsExcluded || !MatchesStrictPlaylistShape(meta)) {
		return PlaylistRecordCandidateVerdict::Ignored;
	}
	switch (ownership) {
	case SyncChatMessageOwnership::Own:
		return PlaylistRecordCandidateVerdict::Candidate;
	case SyncChatMessageOwnership::Unknown:
		return PlaylistRecordCandidateVerdict::UnknownOwnership;
	case SyncChatMessageOwnership::Other:
		return PlaylistRecordCandidateVerdict::Ignored;
	}
	return PlaylistRecordCandidateVerdict::Ignored;
}

bool IsPlaylistRecordCandidate(
		const PlaylistRecordMessageMeta &meta,
		SyncChatMessageOwnership ownership) {
	return InspectPlaylistRecordMetadata(meta, ownership)
		== PlaylistRecordCandidateVerdict::Candidate;
}

PlaylistRecordClassification ClassifyPlaylistRecord(
		const PlaylistRecordInput &input) {
	PlaylistRecordClassification result;
	result.messageId = input.messageId;
	result.documentId = input.documentId;
	result.classifierVersion = kPlaylistRecordClassifierVersion;

	if (input.messageId <= 0 || input.messageId > kMaximumRecordMessageId) {
		result.status = PlaylistRecordStatus::InvalidInput;
		result.error = PlaylistRecordError::InvalidMessageId;
		return FinalizeClassification(std::move(result));
	}
	if (input.documentId == 0) {
		result.status = PlaylistRecordStatus::InvalidInput;
		result.error = PlaylistRecordError::InvalidDocumentId;
		return FinalizeClassification(std::move(result));
	}
	if (input.selfUserId > PlaylistsDetail::kMaximumPeerId) {
		result.status = PlaylistRecordStatus::InvalidInput;
		result.error = PlaylistRecordError::InvalidSelfUserId;
		return FinalizeClassification(std::move(result));
	}

	const auto verdict = InspectPlaylistRecordMetadata(
		input.meta,
		input.ownership);
	switch (verdict) {
	case PlaylistRecordCandidateVerdict::Candidate:
		break;
	case PlaylistRecordCandidateVerdict::UnknownOwnership:
		result.status = PlaylistRecordStatus::Incomplete;
		result.error = PlaylistRecordError::UnknownOwnership;
		return FinalizeClassification(std::move(result));
	case PlaylistRecordCandidateVerdict::AmbiguousSettings:
		result.status = PlaylistRecordStatus::Ignored;
		result.error = PlaylistRecordError::AmbiguousSettingsConflict;
		return FinalizeClassification(std::move(result));
	case PlaylistRecordCandidateVerdict::Ignored:
		result.status = PlaylistRecordStatus::Ignored;
		result.error = (InspectPlaylistRecordMetadata(
				input.meta,
				SyncChatMessageOwnership::Own)
				== PlaylistRecordCandidateVerdict::Candidate)
			? PlaylistRecordError::NotOwnRecord
			: PlaylistRecordError::UnrecognizedRecord;
		return FinalizeClassification(std::move(result));
	}

	if (input.readOutcome != PlaylistRecordReadOutcome::Succeeded) {
		result.status = PlaylistRecordStatus::Incomplete;
		result.error = ErrorForReadOutcome(input.readOutcome);
		return FinalizeClassification(std::move(result));
	}

	const auto decoded = DecodePlaylistBytes(
		input.compressedBytes,
		kPlaylistRecordCodecCaps);
	if (decoded.status == PlaylistCodecStatus::Oversized) {
		result.status = PlaylistRecordStatus::Oversized;
		result.error = PlaylistRecordError::CodecError;
		result.codecError = decoded.error;
		return FinalizeClassification(std::move(result));
	}
	if (decoded.status != PlaylistCodecStatus::Valid) {
		result.status = PlaylistRecordStatus::Invalid;
		result.error = PlaylistRecordError::CodecError;
		result.codecError = decoded.error;
		return FinalizeClassification(std::move(result));
	}

	auto parsed = ParsePlaylistLibrary(
		decoded.bytes,
		input.selfUserId,
		std::nullopt);
	if (parsed.status == PlaylistLibraryStatus::NewerFormat) {
		result.status = PlaylistRecordStatus::NewerFormat;
		result.error = PlaylistRecordError::SchemaError;
		result.libraryError = parsed.error;
		return FinalizeClassification(std::move(result));
	}
	if (parsed.status != PlaylistLibraryStatus::Valid) {
		if (parsed.error == PlaylistLibraryError::InvalidSelfUserId) {
			result.status = PlaylistRecordStatus::InvalidInput;
			result.error = PlaylistRecordError::InvalidSelfUserId;
		} else {
			result.status = PlaylistRecordStatus::Invalid;
			result.error = PlaylistRecordError::SchemaError;
			result.libraryError = parsed.error;
		}
		return FinalizeClassification(std::move(result));
	}

	auto serialized = SerializePlaylistLibrary(parsed.library, input.selfUserId);
	if (serialized.status != PlaylistLibraryStatus::Valid
			|| serialized.canonical.isEmpty()) {
		if (serialized.error == PlaylistLibraryError::InvalidSelfUserId) {
			result.status = PlaylistRecordStatus::InvalidInput;
			result.error = PlaylistRecordError::InvalidSelfUserId;
		} else {
			result.status = PlaylistRecordStatus::Invalid;
			result.error = PlaylistRecordError::SerializeFailed;
			result.libraryError = serialized.error;
		}
		return FinalizeClassification(std::move(result));
	}

	const auto sha256Hex = QString::fromLatin1(
		QCryptographicHash::hash(serialized.canonical, QCryptographicHash::Sha256)
			.toHex());
	if (!IsLowercaseSha256Hex(sha256Hex)) {
		result.status = PlaylistRecordStatus::Invalid;
		result.error = PlaylistRecordError::SerializeFailed;
		return FinalizeClassification(std::move(result));
	}

	result.status = PlaylistRecordStatus::Valid;
	result.error = PlaylistRecordError::None;
	result.library = std::move(parsed.library);
	result.canonicalBytes = std::move(serialized.canonical);
	result.compressedBytes = input.compressedBytes;
	result.canonicalHash = sha256Hex;
	return FinalizeClassification(std::move(result));
}

std::optional<PlaylistLocalMemoEntry> PlaylistRecordMemoEntry(
		const PlaylistRecordClassification &classification) {
	if (classification.messageId <= 0
			|| classification.messageId > kMaximumRecordMessageId
			|| classification.documentId == 0
			|| classification.classifierVersion <= 0) {
		return std::nullopt;
	}
	switch (classification.status) {
	case PlaylistRecordStatus::Valid:
		if (!classification.canonicalHash
				|| !IsLowercaseSha256Hex(*classification.canonicalHash)) {
			return std::nullopt;
		}
		return PlaylistLocalMemoEntry{
			.documentId = classification.documentId,
			.state = PlaylistLocalMemoState::Valid,
			.classifier = classification.classifierVersion,
			.canonicalHash = classification.canonicalHash,
			.preserved = {},
		};
	case PlaylistRecordStatus::Invalid:
		return PlaylistLocalMemoEntry{
			.documentId = classification.documentId,
			.state = PlaylistLocalMemoState::Invalid,
			.classifier = classification.classifierVersion,
			.canonicalHash = std::nullopt,
			.preserved = {},
		};
	case PlaylistRecordStatus::NewerFormat:
		return PlaylistLocalMemoEntry{
			.documentId = classification.documentId,
			.state = PlaylistLocalMemoState::NewerFormat,
			.classifier = classification.classifierVersion,
			.canonicalHash = std::nullopt,
			.preserved = {},
		};
	case PlaylistRecordStatus::Oversized:
		return PlaylistLocalMemoEntry{
			.documentId = classification.documentId,
			.state = PlaylistLocalMemoState::Oversized,
			.classifier = classification.classifierVersion,
			.canonicalHash = std::nullopt,
			.preserved = {},
		};
	case PlaylistRecordStatus::Incomplete:
	case PlaylistRecordStatus::Ignored:
	case PlaylistRecordStatus::InvalidInput:
		return std::nullopt;
	}
	return std::nullopt;
}

} // namespace Purple
