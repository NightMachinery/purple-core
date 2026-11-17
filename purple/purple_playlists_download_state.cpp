#include "purple/purple_types.h"
#include "purple/purple_playlists_download_state.h"

#include <QtCore/QJsonArray>
#include <algorithm>
#include <cmath>
#include <limits>

namespace Purple {
namespace {

QString Key(PlaylistDownloadClient client) {
	switch (client) {
	case PlaylistDownloadClient::Desktop: return u"desktop_playlist_downloads"_q;
	case PlaylistDownloadClient::Android: return u"android_playlist_downloads"_q;
	}
	return {};
}

std::optional<uint64_t> Decimal(const QJsonValue &value) {
	if (!value.isString()) return std::nullopt;
	const auto text = value.toString();
	auto ok = false;
	const auto result = text.toULongLong(&ok);
	return ok && result && QString::number(result) == text
		&& result <= uint64_t(std::numeric_limits<qint64>::max())
		? std::optional<uint64_t>(result) : std::nullopt;
}

bool FileIdentity(const QByteArray &value) {
	if (value.isEmpty() || value.size() > 256 || value.front() == ':'
		|| value.back() == ':' || value.contains("::")) return false;
	const auto separators = value.count(':');
	return (separators == 1 || separators == 2)
		&& std::all_of(value.cbegin(), value.cend(), [](char ch) {
			return ch == ':' || (ch >= '0' && ch <= '9');
		});
}

bool Digest(const QByteArray &value) {
	return value.size() == 64 && std::all_of(value.cbegin(), value.cend(), [](char ch) {
		return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
	});
}

} // namespace

bool PlaylistDownloadTokenSafe(const QString &token) {
	return token.size() == 32
		&& std::all_of(token.cbegin(), token.cend(), [](QChar ch) {
			return (ch >= u'0' && ch <= u'9') || (ch >= u'a' && ch <= u'f');
		});
}

bool PlaylistDownloadExtensionSafe(const QString &extension) {
	return !extension.isEmpty() && extension.size() <= 12
		&& std::all_of(extension.cbegin(), extension.cend(), [](QChar ch) {
			return (ch >= u'a' && ch <= u'z') || (ch >= u'0' && ch <= u'9');
		});
}

PlaylistDownloadCleanup ReadPlaylistDownloadCleanup(const PlaylistDownloadState &state) {
	const auto raw = state.preserved.value(u"cleanup"_q);
	if (raw.isUndefined()) return { PlaylistDownloadStatus::Ready };
	if (!raw.isObject()) return {};
	const auto object = raw.toObject();
	const auto version = object.value(u"format"_q);
	if (!version.isDouble() || version.toDouble() < 1
		|| std::floor(version.toDouble()) != version.toDouble()) return {};
	if (version.toDouble() > 1) return { PlaylistDownloadStatus::Newer };
	const auto prune = object.value(u"prune"_q);
	const auto playlists = object.value(u"playlists"_q);
	if (!prune.isBool() || !playlists.isArray()) return {};
	auto result = PlaylistDownloadCleanup{ PlaylistDownloadStatus::Ready, prune.toBool() };
	for (const auto value : playlists.toArray()) {
		if (!value.isString() || !IsPlaylistId(value.toString())
			|| !result.playlists.insert(value.toString()).second) return {};
	}
	return result;
}

PlaylistDownloadRead ReadPlaylistDownloadState(
		const PlaylistLocalState &local,
		PlaylistDownloadClient client) {
	const auto key = Key(client);
	if (key.isEmpty()) return {};
	const auto value = local.preserved.value(key);
	if (value.isUndefined()) return { PlaylistDownloadStatus::Ready, {} };
	if (!value.isObject()) return {};
	const auto object = value.toObject();
	const auto version = object.value(u"format"_q);
	if (!version.isDouble() || version.toDouble() < 1
		|| std::floor(version.toDouble()) != version.toDouble()) return {};
	if (version.toDouble() > 1) return { PlaylistDownloadStatus::Newer, {} };
	const auto prefs = object.value(u"preferences"_q);
	const auto files = object.value(u"files"_q);
	if (!prefs.isObject() || !files.isObject()) return {};
	auto result = PlaylistDownloadRead{ PlaylistDownloadStatus::Ready, {} };
	result.state.preserved = object;
	result.state.preserved.remove(u"format"_q);
	result.state.preserved.remove(u"preferences"_q);
	result.state.preserved.remove(u"files"_q);
	result.state.preserved.remove(u"pending"_q);
	const auto preferences = prefs.toObject();
	for (auto i = preferences.begin(); i != preferences.end(); ++i) {
		if (!IsPlaylistId(i.key()) || !i.value().isString()) return {};
		const auto text = i.value().toString();
		if (text != u"on"_q && text != u"off"_q) return {};
		result.state.preferences.emplace(i.key(), text == u"on"_q
			? PlaylistKeepDownloaded::On : PlaylistKeepDownloaded::Off);
	}
	const auto inventory = files.toObject();
	for (auto i = inventory.begin(); i != inventory.end(); ++i) {
		const auto id = ParsePlaylistDocumentId(i.key());
		if (!id || !i.value().isObject()) return {};
		const auto entry = i.value().toObject();
		const auto size = Decimal(entry.value(u"size"_q));
		const auto extension = entry.value(u"extension"_q).toString();
		const auto identity = entry.value(u"identity"_q).toString().toLatin1();
		const auto hash = entry.value(u"sha256"_q).toString().toLatin1();
		if (!size || !PlaylistDownloadExtensionSafe(extension)
			|| !FileIdentity(identity)
			|| !Digest(hash)) return {};
		auto preserved = entry;
		for (const auto key : { u"extension"_q, u"size"_q, u"identity"_q, u"sha256"_q }) {
			preserved.remove(key);
		}
		result.state.files.emplace(*id, PlaylistKeptFile{
			extension, *size, identity, hash, preserved });
	}
	const auto pending = object.value(u"pending"_q);
	if (!pending.isUndefined() && !pending.isObject()) return {};
	const auto intents = pending.toObject();
	auto pendingDocuments = std::set<uint64_t>();
	for (auto i = intents.begin(); i != intents.end(); ++i) {
		if (!PlaylistDownloadTokenSafe(i.key()) || !i.value().isObject()) return {};
		const auto entry = i.value().toObject();
		const auto playlistId = entry.value(u"playlist"_q).toString();
		const auto id = ParsePlaylistDocumentId(entry.value(u"document"_q).toString());
		const auto extension = entry.value(u"extension"_q).toString();
		const auto size = Decimal(entry.value(u"size"_q));
		const auto identityValue = entry.value(u"identity"_q);
		const auto identity = identityValue.toString().toLatin1();
		const auto digestValue = entry.value(u"sha256"_q);
		const auto digest = digestValue.toString().toLatin1();
		if (!IsPlaylistId(playlistId) || !id || !size
			|| !PlaylistDownloadExtensionSafe(extension) || !identityValue.isString()
			|| (!identity.isEmpty() && !FileIdentity(identity))
			|| !digestValue.isString() || (!digest.isEmpty() && !Digest(digest))
			|| (identity.isEmpty() && !digest.isEmpty())
			|| !pendingDocuments.insert(*id).second) return {};
		auto preserved = entry;
		for (const auto key : { u"playlist"_q, u"document"_q, u"extension"_q,
			u"size"_q, u"identity"_q, u"sha256"_q }) preserved.remove(key);
		result.state.pending.emplace(i.key(), PlaylistOwnedDownloadIntent{
			playlistId, *id, extension, *size, identity, digest, preserved });
	}
	const auto cleanup = ReadPlaylistDownloadCleanup(result.state);
	if (cleanup.status != PlaylistDownloadStatus::Ready) return { cleanup.status, {} };
	return result;
}

PlaylistDownloadStatus SetPlaylistDownloadState(
		PlaylistLocalState &local,
		const PlaylistDownloadState &state,
		PlaylistDownloadClient client) {
	const auto old = ReadPlaylistDownloadState(local, client);
	if (!old) return old.status;
	auto object = state.preserved;
	object.insert(u"format"_q, 1);
	auto preferences = QJsonObject();
	for (const auto &[id, preference] : state.preferences) {
		if (!IsPlaylistId(id)) return PlaylistDownloadStatus::Invalid;
		switch (preference) {
		case PlaylistKeepDownloaded::Inherit: break;
		case PlaylistKeepDownloaded::On: preferences.insert(id, u"on"_q); break;
		case PlaylistKeepDownloaded::Off: preferences.insert(id, u"off"_q); break;
		default: return PlaylistDownloadStatus::Invalid;
		}
	}
	object.insert(u"preferences"_q, preferences);
	auto files = QJsonObject();
	for (const auto &[id, file] : state.files) {
		const auto key = FormatPlaylistDocumentId(id);
		if (!key) return PlaylistDownloadStatus::Invalid;
		auto entry = file.preserved;
		entry.insert(u"extension"_q, file.extension);
		entry.insert(u"size"_q, QString::number(file.size));
		entry.insert(u"identity"_q, QString::fromLatin1(file.identity));
		entry.insert(u"sha256"_q, QString::fromLatin1(file.sha256));
		files.insert(*key, entry);
	}
	object.insert(u"files"_q, files);
	auto pending = QJsonObject();
	for (const auto &[token, intent] : state.pending) {
		auto entry = intent.preserved;
		entry.insert(u"playlist"_q, intent.playlistId);
		entry.insert(u"document"_q, QString::number(intent.documentId));
		entry.insert(u"extension"_q, intent.extension);
		entry.insert(u"size"_q, QString::number(intent.size));
		entry.insert(u"identity"_q, QString::fromLatin1(intent.identity));
		entry.insert(u"sha256"_q, QString::fromLatin1(intent.sha256));
		pending.insert(token, entry);
	}
	if (!pending.isEmpty()) object.insert(u"pending"_q, pending);
	else object.remove(u"pending"_q);
	auto candidate = local;
	candidate.preserved.insert(Key(client), object);
	if (!ReadPlaylistDownloadState(candidate, client)) return PlaylistDownloadStatus::Invalid;
	local = std::move(candidate);
	return PlaylistDownloadStatus::Ready;
}

bool PlaylistKeepsDownloads(
		const PlaylistDownloadState &state,
		const QString &playlistId,
		bool appWideDefault) {
	const auto i = state.preferences.find(playlistId);
	return i == end(state.preferences) || i->second == PlaylistKeepDownloaded::Inherit
		? appWideDefault : i->second == PlaylistKeepDownloaded::On;
}

std::set<uint64_t> PlaylistDownloadReferences(
		const PlaylistLibrary &library,
		const QString &excluding) {
	auto result = std::set<uint64_t>();
	for (const auto &[id, playlist] : library.playlists) {
		if (id == excluding || library.purged.contains(id) || !playlist.songs) continue;
		for (const auto &[documentId, song] : *playlist.songs) {
			if (!song.value.isRemoved()) result.insert(documentId);
		}
	}
	return result;
}

std::vector<QString> PlaylistDownloadObsoleteCleanupTargets(
		const PlaylistLibrary &library,
		const PlaylistDownloadState &state) {
	const auto cleanup = ReadPlaylistDownloadCleanup(state);
	auto result = std::vector<QString>();
	if (cleanup.status != PlaylistDownloadStatus::Ready) return result;
	for (const auto &id : cleanup.playlists) {
		if (library.purged.contains(id)) result.push_back(id);
	}
	return result;
}

std::vector<QString> PlaylistDownloadRemovalIntents(
		const PlaylistLibrary &library,
		const PlaylistDownloadState &state,
		const std::optional<QString> &removeFromPlaylist) {
	for (const auto &[id, playlist] : library.playlists) {
		if (!library.purged.contains(id) && !playlist.songs) return {};
	}
	const auto references = PlaylistDownloadReferences(library);
	auto result = std::vector<QString>();
	for (const auto &[token, intent] : state.pending) {
		if (removeFromPlaylist
			? intent.playlistId == *removeFromPlaylist
			: !references.contains(intent.documentId)) result.push_back(token);
	}
	return result;
}

std::vector<uint64_t> PlaylistDownloadRemovalCandidates(
		const PlaylistLibrary &library,
		const PlaylistDownloadState &state,
		const std::optional<QString> &removeFromPlaylist) {
	for (const auto &[id, playlist] : library.playlists) {
		if (!library.purged.contains(id) && !playlist.songs) return {};
	}
	const auto protectedIds = PlaylistDownloadReferences(
		library, removeFromPlaylist.value_or(QString()));
	auto targets = std::set<uint64_t>();
	if (removeFromPlaylist) {
		const auto i = library.playlists.find(*removeFromPlaylist);
		if (i == end(library.playlists) || !i->second.songs) return {};
		for (const auto &[id, song] : *i->second.songs) {
			if (!song.value.isRemoved()) targets.insert(id);
		}
	}
	auto result = std::vector<uint64_t>();
	for (const auto &[id, file] : state.files) {
		if (!protectedIds.contains(id)
			&& (!removeFromPlaylist || targets.contains(id))) result.push_back(id);
	}
	return result;
}

namespace {

PlaylistDownloadStatus Mutate(PlaylistDownloadState &state, PlaylistDownloadChange change,
		const QString &playlist, const QString &token, const PlaylistOwnedDownloadIntent &intent) {
	const auto operation = int(change);
	if (operation == 0) return PlaylistDownloadStatus::Ready;
	if (operation >= 1 && operation <= 3) {
		if (!IsPlaylistId(playlist)) return PlaylistDownloadStatus::Invalid;
		if (operation == 1) state.preferences.erase(playlist);
		else state.preferences[playlist] = operation == 2
			? PlaylistKeepDownloaded::On : PlaylistKeepDownloaded::Off;
		return PlaylistDownloadStatus::Ready;
	}
	if (operation >= 9 && operation <= 12) {
		auto cleanup = ReadPlaylistDownloadCleanup(state);
		if (cleanup.status != PlaylistDownloadStatus::Ready) return cleanup.status;
		if (operation <= 10) cleanup.prune = operation == 9;
		else {
			if (!IsPlaylistId(playlist)) return PlaylistDownloadStatus::Invalid;
			if (operation == 11) cleanup.playlists.insert(playlist);
			else cleanup.playlists.erase(playlist);
		}
		auto object = state.preserved.value(u"cleanup"_q).toObject();
		object.insert(u"format"_q, 1);
		object.insert(u"prune"_q, cleanup.prune);
		auto ids = QJsonArray();
		for (const auto &id : cleanup.playlists) ids.push_back(id);
		object.insert(u"playlists"_q, ids);
		if (!cleanup.prune && cleanup.playlists.empty() && object.size() == 3) state.preserved.remove(u"cleanup"_q);
		else state.preserved.insert(u"cleanup"_q, object);
		return PlaylistDownloadStatus::Ready;
	}
	if (operation < 4 || operation > 8) return PlaylistDownloadStatus::Invalid;
	if (operation == 8) {
		const auto i = state.files.find(intent.documentId);
		if (i == state.files.end() || i->second.extension != intent.extension
				|| i->second.size != intent.size || i->second.identity != intent.identity
				|| i->second.sha256 != intent.sha256) return PlaylistDownloadStatus::Changed;
		state.files.erase(i);
		return PlaylistDownloadStatus::Ready;
	}
	if (!PlaylistDownloadTokenSafe(token)) return PlaylistDownloadStatus::Invalid;
	if (operation == 4) {
		if (!intent.identity.isEmpty() || !intent.sha256.isEmpty()) return PlaylistDownloadStatus::Invalid;
		if (state.pending.contains(token) || state.files.contains(intent.documentId)) return PlaylistDownloadStatus::Busy;
		for (const auto &[key, existing] : state.pending) {
			if (existing.documentId == intent.documentId) return PlaylistDownloadStatus::Busy;
		}
		state.pending.emplace(token, intent);
		return PlaylistDownloadStatus::Ready;
	}
	const auto i = state.pending.find(token);
	if (i == state.pending.end()) return PlaylistDownloadStatus::Changed;
	auto &old = i->second;
	if (old.playlistId != intent.playlistId || old.documentId != intent.documentId
			|| old.extension != intent.extension || old.size != intent.size) return PlaylistDownloadStatus::Changed;
	if (operation == 5) {
		if (intent.identity.isEmpty() || (!old.identity.isEmpty() && old.identity != intent.identity)
				|| (!old.sha256.isEmpty() && old.sha256 != intent.sha256)) return PlaylistDownloadStatus::Changed;
		old.identity = intent.identity;
		old.sha256 = intent.sha256;
		return PlaylistDownloadStatus::Ready;
	}
	if (old.identity != intent.identity || old.sha256 != intent.sha256) return PlaylistDownloadStatus::Changed;
	if (operation == 6) {
		if (old.identity.isEmpty() || old.sha256.isEmpty()) return PlaylistDownloadStatus::Invalid;
		if (state.files.contains(old.documentId)) return PlaylistDownloadStatus::Busy;
		state.files.emplace(old.documentId, PlaylistKeptFile{
			old.extension, old.size, old.identity, old.sha256, old.preserved });
	}
	state.pending.erase(i);
	return PlaylistDownloadStatus::Ready;
}


} // namespace

PlaylistDownloadStatus ApplyPlaylistDownloadChange(
		PlaylistDownloadState &state,
		PlaylistDownloadChange change,
		const QString &playlist,
		const QString &token,
		const PlaylistOwnedDownloadIntent &intent) {
	auto validation = PlaylistLocalState();
	if (SetPlaylistDownloadState(validation, state) != PlaylistDownloadStatus::Ready) {
		return PlaylistDownloadStatus::Invalid;
	}
	auto candidate = state;
	const auto status = Mutate(candidate, change, playlist, token, intent);
	if (status != PlaylistDownloadStatus::Ready) return status;
	if (SetPlaylistDownloadState(validation, candidate) != PlaylistDownloadStatus::Ready) {
		return PlaylistDownloadStatus::Invalid;
	}
	state = std::move(candidate);
	return PlaylistDownloadStatus::Ready;
}

} // namespace Purple
