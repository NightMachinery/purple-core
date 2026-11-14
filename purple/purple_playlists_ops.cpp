/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists_ops.h"

#include "purple/purple_types.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace Purple {
namespace {

[[nodiscard]] PlaylistEditResult Refuse(
		const PlaylistLibrary &library,
		PlaylistEditError error,
		const QString &playlistId = QString()) {
	return { error, false, library, playlistId };
}

[[nodiscard]] PlaylistEditResult NoOp(
		const PlaylistLibrary &library,
		const QString &playlistId,
		std::size_t unchangedCount = 0) {
	return {
		PlaylistEditError::None,
		false,
		library,
		playlistId,
		{},
		0,
		0,
		0,
		unchangedCount,
	};
}

[[nodiscard]] PlaylistEditError MapLibraryError(
		const PlaylistLibraryValidationResult &validation) {
	if (validation.status == PlaylistLibraryStatus::NewerFormat) {
		return PlaylistEditError::NewerFormat;
	}
	if (validation.error == PlaylistLibraryError::InvalidSelfUserId) {
		return PlaylistEditError::InvalidSelfUserId;
	}
	return PlaylistEditError::InvalidLibrary;
}

[[nodiscard]] PlaylistEditError MapSourcePlaylistError(
		PlaylistLibraryError error) {
	switch (error) {
	case PlaylistLibraryError::InvalidSelfUserId:
		return PlaylistEditError::InvalidSelfUserId;
	case PlaylistLibraryError::InvalidTitle:
		return PlaylistEditError::InvalidTitle;
	case PlaylistLibraryError::InvalidStamp:
		return PlaylistEditError::InvalidStamp;
	case PlaylistLibraryError::InvalidDocumentId:
		return PlaylistEditError::InvalidDocumentId;
	case PlaylistLibraryError::InvalidOrder:
		return PlaylistEditError::InvalidOrder;
	case PlaylistLibraryError::InvalidPeerToken:
		return PlaylistEditError::InvalidPeerToken;
	case PlaylistLibraryError::InvalidUsername:
		return PlaylistEditError::InvalidUsername;
	case PlaylistLibraryError::InvalidMeta:
		return PlaylistEditError::InvalidMeta;
	case PlaylistLibraryError::InvalidValue:
	case PlaylistLibraryError::InvalidSince:
	case PlaylistLibraryError::InvalidAlt:
		return PlaylistEditError::InvalidValue;
	default:
		return PlaylistEditError::InvalidLibrary;
	}
}

[[nodiscard]] QString MakeCopiedPlaylistTitle(const QString &sourceTitle) {
	const auto suffix = u" (copy)"_q;
	const auto maxBaseCodePoints = kMaximumPlaylistTitleCodePoints - suffix.size();
	auto count = qsizetype(0);
	auto cutIndex = qsizetype(0);
	for (auto i = qsizetype(0); i < sourceTitle.size() && count < maxBaseCodePoints; ++i) {
		const auto code = sourceTitle[i].unicode();
		auto unitCount = qsizetype(1);
		if (code >= 0xD800 && code <= 0xDBFF && (i + 1) < sourceTitle.size()) {
			const auto second = sourceTitle[i + 1].unicode();
			if (second >= 0xDC00 && second <= 0xDFFF) {
				++i;
				unitCount = 2;
			}
		}
		++count;
		cutIndex += unitCount;
	}
	return sourceTitle.left(cutIndex) + suffix;
}

[[nodiscard]] PeerToken NormalizePeer(PeerToken peer, uint64_t selfUserId) {
	if (selfUserId > 0
		&& peer.kind == PeerTokenKind::User
		&& peer.id == selfUserId) {
		return PeerToken();
	}
	return peer;
}

[[nodiscard]] Playlist NormalizePlaylistPeers(
		Playlist playlist,
		uint64_t selfUserId) {
	if (playlist.songs) {
		for (auto &[docId, reg] : *playlist.songs) {
			if (!reg.value.source) {
				continue;
			}
			reg.value.source->peer = NormalizePeer(reg.value.source->peer, selfUserId);
			for (auto &alt : reg.value.source->alt) {
				alt.peer = NormalizePeer(alt.peer, selfUserId);
			}
		}
	}
	return playlist;
}

[[nodiscard]] bool SamePeerMsg(
		const PeerToken &aPeer,
		int32_t aMsg,
		const PeerToken &bPeer,
		int32_t bMsg,
		uint64_t selfUserId) {
	return aMsg == bMsg
		&& NormalizePeer(aPeer, selfUserId) == NormalizePeer(bPeer, selfUserId);
}

[[nodiscard]] uint64_t LargestPlaylistStamp(const Playlist &playlist) {
	auto largest = uint64_t(0);
	if (playlist.title) {
		largest = std::max(largest, playlist.title->stamp);
	}
	if (playlist.deleted) {
		largest = std::max(largest, playlist.deleted->stamp);
	}
	if (playlist.order) {
		largest = std::max(largest, playlist.order->stamp);
	}
	if (playlist.songs) {
		for (const auto &entry : *playlist.songs) {
			largest = std::max(largest, entry.second.stamp);
			if (entry.second.value.source) {
				largest = std::max(largest, entry.second.value.source->since);
			}
		}
	}
	return largest;
}

[[nodiscard]] std::optional<uint64_t> AllocateStamps(
		const Playlist &playlist,
		uint64_t nowMs,
		std::size_t count) {
	const auto largest = LargestPlaylistStamp(playlist);
	if (largest >= kMaximumPlaylistStamp) {
		return std::nullopt;
	}
	const auto base = std::max(nowMs, largest + 1);
	if (count == 0) {
		return (base <= kMaximumPlaylistStamp) ? std::optional(base) : std::nullopt;
	}
	const auto span = uint64_t(count - 1);
	if (base > kMaximumPlaylistStamp || span > kMaximumPlaylistStamp - base) {
		return std::nullopt;
	}
	return base;
}

[[nodiscard]] std::optional<PlaylistEditError> CheckTargetPlaylist(
		const PlaylistLibrary &library,
		const QString &playlistId,
		bool requireNotDeleted) {
	if (!IsPlaylistId(playlistId)) {
		return PlaylistEditError::InvalidId;
	}
	if (library.purged.contains(playlistId)) {
		return PlaylistEditError::PlaylistPurged;
	}
	const auto it = library.playlists.find(playlistId);
	if (it == library.playlists.end()) {
		return PlaylistEditError::PlaylistNotFound;
	}
	const auto &playlist = it->second;
	if (!playlist.title || !playlist.deleted) {
		return PlaylistEditError::PlaylistIncomplete;
	}
	if (requireNotDeleted && playlist.deleted->value) {
		return PlaylistEditError::PlaylistDeleted;
	}
	return std::nullopt;
}

[[nodiscard]] PlaylistEditResult FinalizeEdit(
		const PlaylistLibrary &original,
		PlaylistLibrary candidate,
		const QString &playlistId,
		PlaylistUndoRecord undo,
		uint64_t selfUserId,
		std::size_t addedCount = 0,
		std::size_t relinkedCount = 0,
		std::size_t removedCount = 0,
		std::size_t unchangedCount = 0) {
	const auto nextBytes = SerializePlaylistLibrary(candidate, selfUserId);
	if (!nextBytes) {
		return Refuse(original, MapLibraryError({ nextBytes.status, nextBytes.error }), playlistId);
	}
	if (nextBytes.canonical.size() > kPlaylistLocalEditMaximumBytes) {
		const auto oldBytes = SerializePlaylistLibrary(original, selfUserId);
		if (!oldBytes || nextBytes.canonical.size() >= oldBytes.canonical.size()) {
			return Refuse(original, PlaylistEditError::LibraryFull, playlistId);
		}
	}
	return {
		PlaylistEditError::None,
		true,
		std::move(candidate),
		playlistId,
		std::move(undo),
		addedCount,
		relinkedCount,
		removedCount,
		unchangedCount,
	};
}

[[nodiscard]] PlaylistSource BuildRelinkSource(
		const PlaylistSource &oldSource,
		const PlaylistAddSongInput &input,
		uint64_t selfUserId) {
	const auto incomingPeer = NormalizePeer(input.peer, selfUserId);
	auto next = PlaylistSource();
	next.peer = incomingPeer;
	next.msg = input.msg;
	next.username = input.username;
	next.meta = input.meta;
	next.since = oldSource.since;

	const auto pushFallback = [&](PeerToken peer, int32_t msg, const QString &username) {
		if (next.alt.size() >= kMaximumPlaylistFallbacks) {
			return;
		}
		const auto normalized = NormalizePeer(peer, selfUserId);
		if (SamePeerMsg(normalized, msg, incomingPeer, input.msg, selfUserId)) {
			return;
		}
		for (const auto &existing : next.alt) {
			if (SamePeerMsg(existing.peer, existing.msg, normalized, msg, selfUserId)) {
				return;
			}
		}
		next.alt.push_back({ normalized, msg, username });
	};
	pushFallback(oldSource.peer, oldSource.msg, oldSource.username);
	for (const auto &entry : oldSource.alt) {
		pushFallback(entry.peer, entry.msg, entry.username);
	}
	return next;
}

[[nodiscard]] std::optional<PlaylistEditError> ValidateAddInputs(
		const std::vector<PlaylistAddSongInput> &songs,
		uint64_t selfUserId) {
	auto seen = std::unordered_set<uint64_t>();
	seen.reserve(songs.size());
	for (const auto &item : songs) {
		if (item.documentId == 0) {
			return PlaylistEditError::InvalidDocumentId;
		}
		if (!seen.insert(item.documentId).second) {
			return PlaylistEditError::DuplicateDocumentId;
		}
		if (!PeerTokenText(item.peer, selfUserId)) {
			return PlaylistEditError::InvalidPeerToken;
		}
		if (item.msg < 1 || item.msg > kMaximumPlaylistMessageId) {
			return PlaylistEditError::InvalidValue;
		}
		if (!item.username.isEmpty() && !IsValidPlaylistUsername(item.username)) {
			return PlaylistEditError::InvalidUsername;
		}
		if (!IsValidPlaylistMetaText(item.meta.title)
			|| !IsValidPlaylistMetaText(item.meta.performer)
			|| item.meta.duration < 0
			|| item.meta.duration > kMaximumPlaylistDurationSeconds) {
			return PlaylistEditError::InvalidMeta;
		}
	}
	return std::nullopt;
}

[[nodiscard]] bool IsUntouchedCreatedPlaylist(
		const Playlist &playlist,
		const PlaylistUndoRecord &undo) {
	if (!playlist.title || !playlist.deleted || !playlist.order) {
		return false;
	}
	if (playlist.title->stamp != undo.createdStamp
		|| playlist.title->value != undo.createdTitle
		|| playlist.deleted->stamp != undo.createdStamp
		|| playlist.deleted->value
		|| playlist.order->stamp != undo.createdStamp
		|| !playlist.order->value.empty()) {
		return false;
	}
	return !playlist.songs || playlist.songs->empty();
}

} // namespace

std::optional<QString> FormatPlaylistId(const QByteArray &entropy) {
	if (entropy.size() != kPlaylistIdEntropyBytes) {
		return std::nullopt;
	}
	constexpr auto alphabet = "abcdefghijklmnopqrstuvwxyz234567";
	auto result = QByteArray("pl-");
	result.reserve(19);
	auto buffer = uint32_t(0);
	auto bits = 0;
	for (const auto byte : entropy) {
		buffer = (buffer << 8) | uint8_t(byte);
		bits += 8;
		while (bits >= 5) {
			bits -= 5;
			result.append(alphabet[(buffer >> bits) & 31]);
		}
		buffer &= (uint32_t(1) << bits) - 1;
	}
	return QString::fromLatin1(result);
}

PlaylistEditResult CreatePlaylist(
		const PlaylistLibrary &library,
		const QString &title,
		const QByteArray &entropy,
		uint64_t nowMs,
		uint64_t selfUserId) {
	const auto valid = ValidatePlaylistLibrary(library, selfUserId);
	if (!valid) {
		return Refuse(library, MapLibraryError(valid));
	}
	if (!IsValidPlaylistStamp(nowMs)) {
		return Refuse(library, PlaylistEditError::InvalidStamp);
	}
	if (!IsValidPlaylistTitle(title)) {
		return Refuse(library, PlaylistEditError::InvalidTitle);
	}
	const auto id = FormatPlaylistId(entropy);
	if (!id) {
		return Refuse(library, PlaylistEditError::InvalidEntropy);
	}
	if (library.playlists.contains(*id) || library.purged.contains(*id)) {
		return Refuse(library, PlaylistEditError::IdCollision, *id);
	}
	auto next = library;
	auto created = Playlist();
	created.title = PlaylistTitleRegister{ title, nowMs };
	created.deleted = PlaylistDeletedRegister{ false, nowMs };
	created.order = PlaylistOrderRegister{ {}, nowMs };
	created.songs = std::map<uint64_t, PlaylistSongRegister>{};
	next.playlists.emplace(*id, std::move(created));

	auto undo = PlaylistUndoRecord();
	undo.kind = PlaylistUndoKind::Create;
	undo.playlistId = *id;
	undo.createdTitle = title;
	undo.createdStamp = nowMs;
	return FinalizeEdit(library, std::move(next), *id, std::move(undo), selfUserId);
}

PlaylistEditResult CopyPlaylist(
		const PlaylistLibrary &library,
		const Playlist &source,
		const QByteArray &entropy,
		uint64_t nowMs,
		uint64_t selfUserId) {
	const auto valid = ValidatePlaylistLibrary(library, selfUserId);
	if (!valid) {
		return Refuse(library, MapLibraryError(valid));
	}
	if (!IsValidPlaylistStamp(nowMs)) {
		return Refuse(library, PlaylistEditError::InvalidStamp);
	}
	const auto id = FormatPlaylistId(entropy);
	if (!id) {
		return Refuse(library, PlaylistEditError::InvalidEntropy);
	}
	if (library.playlists.contains(*id) || library.purged.contains(*id)) {
		return Refuse(library, PlaylistEditError::IdCollision, *id);
	}
	if (!source.title || !source.deleted) {
		return Refuse(library, PlaylistEditError::PlaylistIncomplete, *id);
	}
	auto validationWrapper = PlaylistLibrary();
	validationWrapper.playlists.emplace(u"pl-aaaaaaaaaaaaaaaa"_q, source);
	const auto validSource = ValidatePlaylistLibrary(validationWrapper, selfUserId);
	if (!validSource) {
		return Refuse(library, MapSourcePlaylistError(validSource.error), *id);
	}
	const auto copiedTitle = MakeCopiedPlaylistTitle(source.title->value);
	if (!IsValidPlaylistTitle(copiedTitle)) {
		return Refuse(library, PlaylistEditError::InvalidTitle, *id);
	}
	const auto visibleIds = PlaylistVisibleSongIds(source);
	if (uint64_t(visibleIds.size()) > (kMaximumPlaylistStamp - nowMs)) {
		return Refuse(library, PlaylistEditError::StampOverflow, *id);
	}

	auto created = Playlist();
	created.title = PlaylistTitleRegister{ copiedTitle, nowMs };
	created.deleted = PlaylistDeletedRegister{ false, nowMs };
	created.order = PlaylistOrderRegister{ {}, nowMs };
	created.songs = std::map<uint64_t, PlaylistSongRegister>{};
	for (std::size_t i = 0; i < visibleIds.size(); ++i) {
		const auto docId = visibleIds[i];
		const auto &srcReg = source.songs->at(docId);
		const auto stamp = nowMs + uint64_t(i + 1);
		auto nextSource = *srcReg.value.source;
		nextSource.peer = NormalizePeer(nextSource.peer, selfUserId);
		for (auto &alt : nextSource.alt) {
			alt.peer = NormalizePeer(alt.peer, selfUserId);
		}
		nextSource.since = stamp;
		created.songs->emplace(
			docId,
			PlaylistSongRegister{
				PlaylistSongValue{ std::move(nextSource) },
				stamp,
			});
	}

	auto next = library;
	next.playlists.emplace(*id, created);

	// Capture the normalized copy snapshot so UndoCopy rejects any subsequent post-copy edit or merge.
	auto undo = PlaylistUndoRecord();
	undo.kind = PlaylistUndoKind::Copy;
	undo.playlistId = *id;
	undo.expectedCopiedPlaylist = std::move(created);
	return FinalizeEdit(
		library,
		std::move(next),
		*id,
		std::move(undo),
		selfUserId,
		visibleIds.size());
}

PlaylistEditResult RenamePlaylist(
		const PlaylistLibrary &library,
		const QString &playlistId,
		const QString &title,
		uint64_t nowMs,
		uint64_t selfUserId) {
	const auto valid = ValidatePlaylistLibrary(library, selfUserId);
	if (!valid) {
		return Refuse(library, MapLibraryError(valid), playlistId);
	}
	if (!IsValidPlaylistStamp(nowMs)) {
		return Refuse(library, PlaylistEditError::InvalidStamp, playlistId);
	}
	if (!IsValidPlaylistTitle(title)) {
		return Refuse(library, PlaylistEditError::InvalidTitle, playlistId);
	}
	if (const auto err = CheckTargetPlaylist(library, playlistId, true)) {
		return Refuse(library, *err, playlistId);
	}
	const auto &current = library.playlists.at(playlistId);
	if (current.title->value == title) {
		return NoOp(library, playlistId);
	}
	const auto stamp = AllocateStamps(current, nowMs, 1);
	if (!stamp) {
		return Refuse(library, PlaylistEditError::StampOverflow, playlistId);
	}
	auto undo = PlaylistUndoRecord();
	undo.kind = PlaylistUndoKind::RegisterWrites;
	undo.playlistId = playlistId;
	undo.hasTitleWrite = true;
	undo.priorTitle = current.title;

	auto next = library;
	next.playlists.at(playlistId).title = PlaylistTitleRegister{ title, *stamp };
	return FinalizeEdit(library, std::move(next), playlistId, std::move(undo), selfUserId);
}

PlaylistEditResult DeletePlaylist(
		const PlaylistLibrary &library,
		const QString &playlistId,
		uint64_t nowMs,
		uint64_t selfUserId) {
	const auto valid = ValidatePlaylistLibrary(library, selfUserId);
	if (!valid) {
		return Refuse(library, MapLibraryError(valid), playlistId);
	}
	if (!IsValidPlaylistStamp(nowMs)) {
		return Refuse(library, PlaylistEditError::InvalidStamp, playlistId);
	}
	if (const auto err = CheckTargetPlaylist(library, playlistId, false)) {
		return Refuse(library, *err, playlistId);
	}
	const auto &current = library.playlists.at(playlistId);
	if (current.deleted->value) {
		return NoOp(library, playlistId);
	}
	const auto stamp = AllocateStamps(current, nowMs, 1);
	if (!stamp) {
		return Refuse(library, PlaylistEditError::StampOverflow, playlistId);
	}
	auto undo = PlaylistUndoRecord();
	undo.kind = PlaylistUndoKind::RegisterWrites;
	undo.playlistId = playlistId;
	undo.hasDeletedWrite = true;
	undo.priorDeleted = current.deleted;

	auto next = library;
	next.playlists.at(playlistId).deleted = PlaylistDeletedRegister{ true, *stamp };
	return FinalizeEdit(library, std::move(next), playlistId, std::move(undo), selfUserId);
}

PlaylistEditResult RestorePlaylist(
		const PlaylistLibrary &library,
		const QString &playlistId,
		uint64_t nowMs,
		uint64_t selfUserId) {
	const auto valid = ValidatePlaylistLibrary(library, selfUserId);
	if (!valid) {
		return Refuse(library, MapLibraryError(valid), playlistId);
	}
	if (!IsValidPlaylistStamp(nowMs)) {
		return Refuse(library, PlaylistEditError::InvalidStamp, playlistId);
	}
	if (const auto err = CheckTargetPlaylist(library, playlistId, false)) {
		return Refuse(library, *err, playlistId);
	}
	const auto &current = library.playlists.at(playlistId);
	if (!current.deleted->value) {
		return NoOp(library, playlistId);
	}
	const auto stamp = AllocateStamps(current, nowMs, 1);
	if (!stamp) {
		return Refuse(library, PlaylistEditError::StampOverflow, playlistId);
	}
	auto undo = PlaylistUndoRecord();
	undo.kind = PlaylistUndoKind::RegisterWrites;
	undo.playlistId = playlistId;
	undo.hasDeletedWrite = true;
	undo.priorDeleted = current.deleted;

	auto next = library;
	next.playlists.at(playlistId).deleted = PlaylistDeletedRegister{ false, *stamp };
	return FinalizeEdit(library, std::move(next), playlistId, std::move(undo), selfUserId);
}

PlaylistEditResult DeleteForeverPlaylist(
		const PlaylistLibrary &library,
		const QString &playlistId,
		uint64_t selfUserId) {
	const auto valid = ValidatePlaylistLibrary(library, selfUserId);
	if (!valid) {
		return Refuse(library, MapLibraryError(valid), playlistId);
	}
	if (const auto err = CheckTargetPlaylist(library, playlistId, false)) {
		return Refuse(library, *err, playlistId);
	}
	const auto &current = library.playlists.at(playlistId);
	if (!current.deleted->value) {
		return Refuse(library, PlaylistEditError::PlaylistNotDeleted, playlistId);
	}
	auto next = library;
	next.playlists.erase(playlistId);
	next.purged.insert(playlistId);
	return FinalizeEdit(library, std::move(next), playlistId, {}, selfUserId);
}

PlaylistEditResult AddPlaylistSongs(
		const PlaylistLibrary &library,
		const QString &playlistId,
		const std::vector<PlaylistAddSongInput> &songs,
		uint64_t nowMs,
		uint64_t selfUserId) {
	const auto valid = ValidatePlaylistLibrary(library, selfUserId);
	if (!valid) {
		return Refuse(library, MapLibraryError(valid), playlistId);
	}
	if (!IsValidPlaylistStamp(nowMs)) {
		return Refuse(library, PlaylistEditError::InvalidStamp, playlistId);
	}
	if (const auto err = CheckTargetPlaylist(library, playlistId, true)) {
		return Refuse(library, *err, playlistId);
	}
	if (const auto err = ValidateAddInputs(songs, selfUserId)) {
		return Refuse(library, *err, playlistId);
	}
	const auto &current = library.playlists.at(playlistId);
	auto writesNeeded = std::size_t(0);
	for (const auto &item : songs) {
		if (!current.songs) {
			++writesNeeded;
			continue;
		}
		const auto it = current.songs->find(item.documentId);
		if (it == current.songs->end() || it->second.value.isRemoved()) {
			++writesNeeded;
		} else {
			const auto &oldSrc = *it->second.value.source;
			if (!SamePeerMsg(oldSrc.peer, oldSrc.msg, item.peer, item.msg, selfUserId)) {
				++writesNeeded;
			}
		}
	}
	if (writesNeeded == 0) {
		return NoOp(library, playlistId, songs.size());
	}
	const auto baseStamp = AllocateStamps(current, nowMs, writesNeeded);
	if (!baseStamp) {
		return Refuse(library, PlaylistEditError::StampOverflow, playlistId);
	}

	auto next = library;
	auto &target = next.playlists.at(playlistId);
	if (!target.songs) {
		target.songs = std::map<uint64_t, PlaylistSongRegister>{};
	}
	auto undo = PlaylistUndoRecord();
	undo.kind = PlaylistUndoKind::RegisterWrites;
	undo.playlistId = playlistId;
	auto added = std::size_t(0);
	auto relinked = std::size_t(0);
	auto unchanged = std::size_t(0);
	auto writeOffset = uint64_t(0);

	for (const auto &item : songs) {
		const auto it = target.songs->find(item.documentId);
		if (it == target.songs->end() || it->second.value.isRemoved()) {
			const auto stamp = *baseStamp + writeOffset++;
			undo.priorSongs.emplace(
				item.documentId,
				(it == target.songs->end())
					? std::nullopt
					: std::optional(it->second));
			auto src = PlaylistSource();
			src.peer = NormalizePeer(item.peer, selfUserId);
			src.msg = item.msg;
			src.username = item.username;
			src.meta = item.meta;
			src.since = stamp;
			(*target.songs)[item.documentId] = PlaylistSongRegister{
				PlaylistSongValue{ std::move(src) },
				stamp,
			};
			++added;
		} else {
			const auto &oldSrc = *it->second.value.source;
			if (SamePeerMsg(oldSrc.peer, oldSrc.msg, item.peer, item.msg, selfUserId)) {
				++unchanged;
				continue;
			}
			const auto stamp = *baseStamp + writeOffset++;
			undo.priorSongs.emplace(item.documentId, it->second);
			auto relinkedSrc = BuildRelinkSource(oldSrc, item, selfUserId);
			it->second = PlaylistSongRegister{
				PlaylistSongValue{ std::move(relinkedSrc) },
				stamp,
			};
			++relinked;
		}
	}
	return FinalizeEdit(
		library,
		std::move(next),
		playlistId,
		std::move(undo),
		selfUserId,
		added,
		relinked,
		0,
		unchanged);
}

PlaylistEditResult RemovePlaylistSongs(
		const PlaylistLibrary &library,
		const QString &playlistId,
		const std::vector<uint64_t> &documentIds,
		uint64_t nowMs,
		uint64_t selfUserId) {
	const auto valid = ValidatePlaylistLibrary(library, selfUserId);
	if (!valid) {
		return Refuse(library, MapLibraryError(valid), playlistId);
	}
	if (!IsValidPlaylistStamp(nowMs)) {
		return Refuse(library, PlaylistEditError::InvalidStamp, playlistId);
	}
	if (const auto err = CheckTargetPlaylist(library, playlistId, true)) {
		return Refuse(library, *err, playlistId);
	}
	auto uniqueIds = std::vector<uint64_t>();
	auto seen = std::unordered_set<uint64_t>();
	seen.reserve(documentIds.size());
	for (const auto docId : documentIds) {
		if (docId == 0) {
			return Refuse(library, PlaylistEditError::InvalidDocumentId, playlistId);
		}
		if (seen.insert(docId).second) {
			uniqueIds.push_back(docId);
		}
	}
	const auto &current = library.playlists.at(playlistId);
	auto toRemove = std::vector<uint64_t>();
	for (const auto docId : uniqueIds) {
		if (!current.songs) {
			continue;
		}
		const auto it = current.songs->find(docId);
		if (it != current.songs->end() && !it->second.value.isRemoved()) {
			toRemove.push_back(docId);
		}
	}
	const auto unchanged = uniqueIds.size() - toRemove.size();
	if (toRemove.empty()) {
		return NoOp(library, playlistId, unchanged);
	}
	const auto stamp = AllocateStamps(current, nowMs, 1);
	if (!stamp) {
		return Refuse(library, PlaylistEditError::StampOverflow, playlistId);
	}
	auto next = library;
	auto &songsMap = *next.playlists.at(playlistId).songs;
	auto undo = PlaylistUndoRecord();
	undo.kind = PlaylistUndoKind::RegisterWrites;
	undo.playlistId = playlistId;
	for (const auto docId : toRemove) {
		auto &reg = songsMap.at(docId);
		undo.priorSongs.emplace(docId, reg);
		reg = PlaylistSongRegister{ PlaylistSongValue{ std::nullopt }, *stamp };
	}
	return FinalizeEdit(
		library,
		std::move(next),
		playlistId,
		std::move(undo),
		selfUserId,
		0,
		0,
		toRemove.size(),
		unchanged);
}

PlaylistEditResult ReorderPlaylistSongs(
		const PlaylistLibrary &library,
		const QString &playlistId,
		const std::vector<uint64_t> &documentIds,
		uint64_t nowMs,
		uint64_t selfUserId) {
	const auto valid = ValidatePlaylistLibrary(library, selfUserId);
	if (!valid) {
		return Refuse(library, MapLibraryError(valid), playlistId);
	}
	if (!IsValidPlaylistStamp(nowMs)) {
		return Refuse(library, PlaylistEditError::InvalidStamp, playlistId);
	}
	if (const auto err = CheckTargetPlaylist(library, playlistId, true)) {
		return Refuse(library, *err, playlistId);
	}
	const auto &current = library.playlists.at(playlistId);
	auto visible = std::unordered_set<uint64_t>();
	if (current.songs) {
		for (const auto &entry : *current.songs) {
			if (!entry.second.value.isRemoved()) {
				visible.insert(entry.first);
			}
		}
	}
	if (documentIds.size() != visible.size()) {
		return Refuse(library, PlaylistEditError::InvalidOrder, playlistId);
	}
	auto seen = std::unordered_set<uint64_t>();
	seen.reserve(documentIds.size());
	for (const auto docId : documentIds) {
		if (docId == 0) {
			return Refuse(library, PlaylistEditError::InvalidDocumentId, playlistId);
		}
		if (!visible.contains(docId) || !seen.insert(docId).second) {
			return Refuse(library, PlaylistEditError::InvalidOrder, playlistId);
		}
	}
	if (current.order
		&& current.order->value == documentIds
		&& PlaylistVisibleSongIds(current) == documentIds) {
		return NoOp(library, playlistId);
	}
	const auto stamp = AllocateStamps(current, nowMs, 1);
	if (!stamp) {
		return Refuse(library, PlaylistEditError::StampOverflow, playlistId);
	}
	auto undo = PlaylistUndoRecord();
	undo.kind = PlaylistUndoKind::RegisterWrites;
	undo.playlistId = playlistId;
	undo.hasOrderWrite = true;
	undo.priorOrder = current.order;

	auto next = library;
	next.playlists.at(playlistId).order = PlaylistOrderRegister{ documentIds, *stamp };
	return FinalizeEdit(library, std::move(next), playlistId, std::move(undo), selfUserId);
}

PlaylistEditResult UndoPlaylistOperation(
		const PlaylistLibrary &library,
		const PlaylistUndoRecord &undo,
		uint64_t nowMs,
		uint64_t selfUserId) {
	const auto valid = ValidatePlaylistLibrary(library, selfUserId);
	if (!valid) {
		return Refuse(library, MapLibraryError(valid), undo.playlistId);
	}
	if (!undo.hasUndo()) {
		return Refuse(library, PlaylistEditError::NothingToUndo, undo.playlistId);
	}
	if (!IsPlaylistId(undo.playlistId)) {
		return Refuse(library, PlaylistEditError::InvalidId, undo.playlistId);
	}
	if (library.purged.contains(undo.playlistId)) {
		return Refuse(library, PlaylistEditError::PlaylistPurged, undo.playlistId);
	}
	const auto it = library.playlists.find(undo.playlistId);
	if (it == library.playlists.end()) {
		return Refuse(library, PlaylistEditError::PlaylistNotFound, undo.playlistId);
	}
	const auto &current = it->second;
	if (!current.title || !current.deleted) {
		return Refuse(library, PlaylistEditError::PlaylistIncomplete, undo.playlistId);
	}
	if (undo.kind == PlaylistUndoKind::Create) {
		if (!IsUntouchedCreatedPlaylist(current, undo)) {
			return Refuse(library, PlaylistEditError::UndoConflict, undo.playlistId);
		}
		auto next = library;
		next.playlists.erase(undo.playlistId);
		next.purged.insert(undo.playlistId);
		return FinalizeEdit(library, std::move(next), undo.playlistId, {}, selfUserId);
	}
	if (undo.kind == PlaylistUndoKind::Copy) {
		if (!undo.expectedCopiedPlaylist
			|| NormalizePlaylistPeers(current, selfUserId)
				!= NormalizePlaylistPeers(*undo.expectedCopiedPlaylist, selfUserId)) {
			return Refuse(library, PlaylistEditError::UndoConflict, undo.playlistId);
		}
		auto next = library;
		next.playlists.erase(undo.playlistId);
		next.purged.insert(undo.playlistId);
		return FinalizeEdit(library, std::move(next), undo.playlistId, {}, selfUserId);
	}
	if (!IsValidPlaylistStamp(nowMs)) {
		return Refuse(library, PlaylistEditError::InvalidStamp, undo.playlistId);
	}
	if (!undo.hasTitleWrite
		&& !undo.hasDeletedWrite
		&& !undo.hasOrderWrite
		&& undo.priorSongs.empty()) {
		return Refuse(library, PlaylistEditError::NothingToUndo, undo.playlistId);
	}
	// Refuse undoing non-delete edits onto a playlist deleted concurrently or on another device.
	if (!undo.hasDeletedWrite && current.deleted->value) {
		return Refuse(library, PlaylistEditError::PlaylistDeleted, undo.playlistId);
	}
	const auto stamp = AllocateStamps(current, nowMs, 1);
	if (!stamp) {
		return Refuse(library, PlaylistEditError::StampOverflow, undo.playlistId);
	}
	auto next = library;
	auto &target = next.playlists.at(undo.playlistId);
	auto redo = PlaylistUndoRecord();
	redo.kind = PlaylistUndoKind::RegisterWrites;
	redo.playlistId = undo.playlistId;

	if (undo.hasTitleWrite) {
		if (!undo.priorTitle) {
			return Refuse(library, PlaylistEditError::UndoConflict, undo.playlistId);
		}
		redo.hasTitleWrite = true;
		redo.priorTitle = target.title;
		target.title = PlaylistTitleRegister{ undo.priorTitle->value, *stamp };
	}
	if (undo.hasDeletedWrite) {
		if (!undo.priorDeleted) {
			return Refuse(library, PlaylistEditError::UndoConflict, undo.playlistId);
		}
		redo.hasDeletedWrite = true;
		redo.priorDeleted = target.deleted;
		target.deleted = PlaylistDeletedRegister{ undo.priorDeleted->value, *stamp };
	}
	if (undo.hasOrderWrite) {
		redo.hasOrderWrite = true;
		redo.priorOrder = target.order;
		if (undo.priorOrder) {
			target.order = PlaylistOrderRegister{ undo.priorOrder->value, *stamp };
		} else {
			target.order = PlaylistOrderRegister{ {}, *stamp };
		}
	}
	if (!undo.priorSongs.empty()) {
		if (!target.songs) {
			target.songs = std::map<uint64_t, PlaylistSongRegister>{};
		}
		for (const auto &[docId, priorReg] : undo.priorSongs) {
			if (docId == 0) {
				return Refuse(library, PlaylistEditError::InvalidDocumentId, undo.playlistId);
			}
			const auto existingIt = target.songs->find(docId);
			redo.priorSongs.emplace(
				docId,
				(existingIt == target.songs->end())
					? std::nullopt
					: std::optional(existingIt->second));
			auto restoredValue = (priorReg && priorReg->value.source)
				? priorReg->value
				: PlaylistSongValue{ std::nullopt };
			(*target.songs)[docId] = PlaylistSongRegister{
				std::move(restoredValue),
				*stamp,
			};
		}
	}
	return FinalizeEdit(library, std::move(next), undo.playlistId, std::move(redo), selfUserId);
}

std::vector<uint64_t> PlaylistVisibleSongIds(const Playlist &playlist) {
	if (!playlist.songs) {
		return {};
	}
	auto result = std::vector<uint64_t>();
	result.reserve(playlist.songs->size());
	auto placed = std::unordered_set<uint64_t>();
	placed.reserve(playlist.songs->size());
	if (playlist.order) {
		const auto orderStamp = playlist.order->stamp;
		for (const auto docId : playlist.order->value) {
			const auto it = playlist.songs->find(docId);
			if (it == playlist.songs->end() || !it->second.value.source) {
				continue;
			}
			if (it->second.value.source->since <= orderStamp
				&& placed.insert(docId).second) {
				result.push_back(docId);
			}
		}
	}
	auto tail = std::vector<std::pair<uint64_t, uint64_t>>();
	for (const auto &[docId, reg] : *playlist.songs) {
		if (!reg.value.source || placed.contains(docId)) {
			continue;
		}
		tail.emplace_back(reg.value.source->since, docId);
	}
	std::sort(tail.begin(), tail.end());
	for (const auto &entry : tail) {
		result.push_back(entry.second);
	}
	return result;
}

std::vector<PlaylistListEntry> ListVisiblePlaylists(
		const PlaylistLibrary &library,
		const std::map<QString, uint64_t> &recentUseMs) {
	auto entries = std::vector<PlaylistListEntry>();
	for (const auto &[id, playlist] : library.playlists) {
		if (library.purged.contains(id)
			|| !playlist.title
			|| !playlist.deleted
			|| playlist.deleted->value) {
			continue;
		}
		const auto recentIt = recentUseMs.find(id);
		const auto recent = (recentIt == recentUseMs.end()) ? uint64_t(0) : recentIt->second;
		entries.push_back({
			id,
			playlist.title->value,
			recent,
			PlaylistVisibleSongIds(playlist).size(),
		});
	}
	std::sort(entries.begin(), entries.end(), [](
			const PlaylistListEntry &a,
			const PlaylistListEntry &b) {
		if (a.recentUseMs != b.recentUseMs) {
			return a.recentUseMs > b.recentUseMs;
		}
		const auto folded = QString::compare(a.title, b.title, Qt::CaseInsensitive);
		if (folded != 0) {
			return folded < 0;
		}
		return a.id < b.id;
	});
	return entries;
}

} // namespace Purple
