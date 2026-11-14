/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_peer_token.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace Purple {

inline constexpr auto kPlaylistFormatVersion = 1;
inline constexpr auto kMinimumPlaylistStamp = uint64_t(1);
inline constexpr auto kMaximumPlaylistStamp = uint64_t(1) << 52;
inline constexpr auto kMaximumPlaylistMessageId = int32_t(2147483647);
inline constexpr auto kMaximumPlaylistDurationSeconds = int32_t(2147483647);
inline constexpr auto kMaximumPlaylistTitleCodePoints = qsizetype(64);
inline constexpr auto kMaximumPlaylistMetaCodePoints = qsizetype(96);
inline constexpr auto kMinimumPlaylistUsernameLength = qsizetype(4);
inline constexpr auto kMaximumPlaylistUsernameLength = qsizetype(32);
inline constexpr auto kMaximumPlaylistFallbacks = std::size_t(3);

template <typename Value>
struct PlaylistRegister {
	Value value{};
	uint64_t stamp = 0;

	[[nodiscard]] bool operator==(const PlaylistRegister &) const = default;
};

struct PlaylistSourceRef {
	PeerToken peer;
	int32_t msg = 0;
	QString username;

	[[nodiscard]] bool operator==(const PlaylistSourceRef &) const = default;
};

struct PlaylistSongMeta {
	QString title;
	QString performer;
	int32_t duration = 0;

	[[nodiscard]] bool operator==(const PlaylistSongMeta &) const = default;
};

struct PlaylistSource {
	PeerToken peer;
	int32_t msg = 0;
	QString username;
	std::vector<PlaylistSourceRef> alt;
	PlaylistSongMeta meta;
	uint64_t since = 0;

	[[nodiscard]] bool operator==(const PlaylistSource &) const = default;
};

struct PlaylistSongValue {
	std::optional<PlaylistSource> source;

	[[nodiscard]] bool isRemoved() const {
		return !source.has_value();
	}

	[[nodiscard]] bool operator==(const PlaylistSongValue &) const = default;
};

using PlaylistTitleRegister = PlaylistRegister<QString>;
using PlaylistDeletedRegister = PlaylistRegister<bool>;
using PlaylistOrderRegister = PlaylistRegister<std::vector<uint64_t>>;
using PlaylistSongRegister = PlaylistRegister<PlaylistSongValue>;

struct Playlist {
	std::optional<PlaylistTitleRegister> title;
	std::optional<PlaylistDeletedRegister> deleted;
	std::optional<PlaylistOrderRegister> order;
	std::optional<std::map<uint64_t, PlaylistSongRegister>> songs;

	[[nodiscard]] bool isPartial() const {
		return !title || !deleted || !order || !songs;
	}

	[[nodiscard]] bool operator==(const Playlist &) const = default;
};

struct PlaylistLibrary {
	int format = kPlaylistFormatVersion;
	std::map<QString, Playlist> playlists;
	std::set<QString> purged;

	[[nodiscard]] bool operator==(const PlaylistLibrary &) const = default;
};

enum class PlaylistLibraryStatus {
	Valid,
	NewerFormat,
	Invalid,
};

enum class PlaylistLibraryError {
	None,
	InvalidJson,
	SizeLimit,
	MissingField,
	UnknownKey,
	FieldType,
	InvalidFormat,
	InvalidId,
	InvalidDocumentId,
	InvalidPeerToken,
	InvalidUsername,
	InvalidTitle,
	InvalidMeta,
	InvalidStamp,
	InvalidSince,
	InvalidOrder,
	InvalidAlt,
	InvalidPurged,
	InvalidValue,
	EmptyPlaylist,
	PurgedPlaylistPresent,
	InvalidSelfUserId,
};

struct PlaylistLibraryValidationResult {
	PlaylistLibraryStatus status = PlaylistLibraryStatus::Invalid;
	PlaylistLibraryError error = PlaylistLibraryError::None;

	[[nodiscard]] explicit operator bool() const {
		return status == PlaylistLibraryStatus::Valid;
	}
};

struct PlaylistLibraryParseResult {
	PlaylistLibraryStatus status = PlaylistLibraryStatus::Invalid;
	PlaylistLibraryError error = PlaylistLibraryError::None;
	PlaylistLibrary library;

	[[nodiscard]] explicit operator bool() const {
		return status == PlaylistLibraryStatus::Valid;
	}
};

struct PlaylistLibraryWriteResult {
	QByteArray canonical;
	PlaylistLibraryStatus status = PlaylistLibraryStatus::Invalid;
	PlaylistLibraryError error = PlaylistLibraryError::None;

	[[nodiscard]] explicit operator bool() const {
		return status == PlaylistLibraryStatus::Valid;
	}
};

struct PlaylistLibraryJoinResult {
	PlaylistLibraryStatus status = PlaylistLibraryStatus::Invalid;
	PlaylistLibraryError error = PlaylistLibraryError::None;
	PlaylistLibrary library;

	[[nodiscard]] explicit operator bool() const {
		return status == PlaylistLibraryStatus::Valid;
	}
};

struct PlaylistLibraryContainsResult {
	PlaylistLibraryStatus status = PlaylistLibraryStatus::Invalid;
	PlaylistLibraryError error = PlaylistLibraryError::None;
	bool contains = false;

	[[nodiscard]] bool valid() const {
		return status == PlaylistLibraryStatus::Valid;
	}
};

[[nodiscard]] bool IsPlaylistId(const QString &value);
[[nodiscard]] std::optional<uint64_t> ParsePlaylistDocumentId(
	const QString &text);
[[nodiscard]] std::optional<QString> FormatPlaylistDocumentId(uint64_t id);
[[nodiscard]] bool IsValidPlaylistUsername(const QString &value);
[[nodiscard]] bool IsValidPlaylistTitle(const QString &value);
[[nodiscard]] bool IsValidPlaylistMetaText(const QString &value);
[[nodiscard]] bool IsValidPlaylistStamp(uint64_t stamp);

[[nodiscard]] PlaylistLibraryValidationResult ValidatePlaylistLibrary(
	const PlaylistLibrary &library,
	uint64_t selfUserId = 0);
[[nodiscard]] PlaylistLibraryParseResult ParsePlaylistLibrary(
	const QByteArray &json,
	uint64_t selfUserId = 0,
	std::optional<qsizetype> maximumBytes = std::nullopt);
[[nodiscard]] PlaylistLibraryWriteResult SerializePlaylistLibrary(
	const PlaylistLibrary &library,
	uint64_t selfUserId = 0);
[[nodiscard]] PlaylistLibraryJoinResult JoinPlaylistLibraries(
	const PlaylistLibrary &a,
	const PlaylistLibrary &b,
	uint64_t selfUserId = 0);
[[nodiscard]] PlaylistLibraryContainsResult PlaylistLibraryContains(
	const PlaylistLibrary &outer,
	const PlaylistLibrary &inner,
	uint64_t selfUserId = 0);

} // namespace Purple
