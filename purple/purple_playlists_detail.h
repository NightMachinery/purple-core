/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_playlists.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QString>

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>

namespace Purple::PlaylistsDetail {

inline constexpr auto kMaximumPeerId = uint64_t(std::numeric_limits<int64_t>::max());
inline constexpr auto kMaximumSafeInteger = uint64_t(9007199254740991ULL);

[[nodiscard]] bool HasValidUtf16CodePoints(const QString &value);
[[nodiscard]] std::optional<uint64_t> ReadBoundedInteger(
	const QJsonValue &value,
	uint64_t minValue,
	uint64_t maxValue);
[[nodiscard]] bool CheckAllowedKeys(
	const QJsonObject &object,
	std::initializer_list<QString> allowed);
[[nodiscard]] PlaylistLibraryError ValidatePlaylist(
	const Playlist &playlist,
	uint64_t selfUserId);
[[nodiscard]] Playlist NormalizePlaylist(
	Playlist playlist,
	uint64_t selfUserId);
[[nodiscard]] std::optional<PlaylistLibraryError> ParsePlaylistJson(
	const QJsonObject &obj,
	uint64_t selfUserId,
	Playlist &out);
void AppendUint64(QByteArray &out, uint64_t value);
void AppendJsonString(QByteArray &out, const QString &value);
void AppendPlaylistJson(
	QByteArray &out,
	const Playlist &playlist,
	uint64_t selfUserId);

} // namespace Purple::PlaylistsDetail
