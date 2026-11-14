/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists_local.h"

#include "purple/purple_playlists_detail.h"
#include "purple/purple_sync_json.h"
#include "purple/purple_types.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonValue>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace Purple {
namespace {

constexpr auto kMaximumPreservedJsonDepth = 128;

[[nodiscard]] bool IsKnownRootKey(const QString &key) {
	return key == u"due_seen"_q
		|| key == u"format"_q
		|| key == u"hidden"_q
		|| key == u"recent_use"_q
		|| key == u"sync"_q;
}

[[nodiscard]] bool IsKnownDueSeenKey(const QString &key) {
	return key == u"deletion_stamp"_q || key == u"first_seen_at_ms"_q;
}

[[nodiscard]] bool IsKnownHiddenKey(const QString &key) {
	return key == u"playlist"_q || key == u"reason"_q;
}

[[nodiscard]] PlaylistLocalValidationResult InvalidValidation(
		PlaylistLocalError error,
		PlaylistLibraryError playlistError = PlaylistLibraryError::None) {
	return { PlaylistLocalStatus::Invalid, error, playlistError };
}

[[nodiscard]] PlaylistLocalParseResult InvalidParse(
		PlaylistLocalError error,
		PlaylistLibraryError playlistError = PlaylistLibraryError::None) {
	return { PlaylistLocalStatus::Invalid, error, playlistError, {} };
}

[[nodiscard]] std::optional<PlaylistNoticeReason> ParseReason(
		const QString &value) {
	if (value == u"deleted"_q) {
		return PlaylistNoticeReason::DeletedElsewhere;
	}
	if (value == u"purged"_q) {
		return PlaylistNoticeReason::PurgedElsewhere;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<QByteArray> SerializeReason(
		PlaylistNoticeReason reason) {
	switch (reason) {
	case PlaylistNoticeReason::DeletedElsewhere:
		return QByteArray("\"deleted\"");
	case PlaylistNoticeReason::PurgedElsewhere:
		return QByteArray("\"purged\"");
	}
	return std::nullopt;
}

[[nodiscard]] bool LessUtf16(const QString &a, const QString &b) {
	const auto *left = a.utf16();
	const auto *right = b.utf16();
	return std::lexicographical_compare(
		left,
		left + a.size(),
		right,
		right + b.size());
}

void AppendSignedSafeInt64(QByteArray &out, int64_t value) {
	if (value < 0) {
		out.append('-');
		PlaylistsDetail::AppendUint64(out, uint64_t(-(value + 1)) + 1ULL);
	} else {
		PlaylistsDetail::AppendUint64(out, uint64_t(value));
	}
}

[[nodiscard]] bool TraverseCanonicalQJsonValue(
		const QJsonValue &value,
		int depth,
		QByteArray *out) {
	if (depth > kMaximumPreservedJsonDepth) {
		return false;
	}
	switch (value.type()) {
	case QJsonValue::Null:
		if (out) {
			out->append("null");
		}
		return true;
	case QJsonValue::Bool:
		if (out) {
			out->append(value.toBool() ? "true" : "false");
		}
		return true;
	case QJsonValue::Double: {
		const auto d = value.toDouble();
		if (!std::isfinite(d)
			|| std::trunc(d) != d
			|| d < -double(PlaylistsDetail::kMaximumSafeInteger)
			|| d > double(PlaylistsDetail::kMaximumSafeInteger)) {
			return false;
		}
		if (out) {
			AppendSignedSafeInt64(*out, int64_t(d));
		}
		return true;
	}
	case QJsonValue::String: {
		const auto s = value.toString();
		if (!PlaylistsDetail::HasValidUtf16CodePoints(s)) {
			return false;
		}
		if (out) {
			PlaylistsDetail::AppendJsonString(*out, s);
		}
		return true;
	}
	case QJsonValue::Array: {
		const auto arr = value.toArray();
		if (out) {
			out->append('[');
		}
		for (auto i = qsizetype(0); i < arr.size(); ++i) {
			if (i > 0 && out) {
				out->append(',');
			}
			if (!TraverseCanonicalQJsonValue(arr[i], depth + 1, out)) {
				return false;
			}
		}
		if (out) {
			out->append(']');
		}
		return true;
	}
	case QJsonValue::Object: {
		const auto obj = value.toObject();
		auto keys = std::vector<QString>();
		keys.reserve(std::size_t(obj.size()));
		for (auto it = obj.begin(); it != obj.end(); ++it) {
			if (!PlaylistsDetail::HasValidUtf16CodePoints(it.key())) {
				return false;
			}
			keys.push_back(it.key());
		}
		if (out) {
			std::sort(keys.begin(), keys.end(), LessUtf16);
			out->append('{');
		}
		for (std::size_t i = 0; i < keys.size(); ++i) {
			if (i > 0 && out) {
				out->append(',');
			}
			if (out) {
				PlaylistsDetail::AppendJsonString(*out, keys[i]);
				out->append(':');
			}
			if (!TraverseCanonicalQJsonValue(obj.value(keys[i]), depth + 1, out)) {
				return false;
			}
		}
		if (out) {
			out->append('}');
		}
		return true;
	}
	case QJsonValue::Undefined:
		break;
	}
	return false;
}

template <typename Pred>
[[nodiscard]] bool ValidatePreservedObject(
		const QJsonObject &obj,
		int valueDepth,
		Pred isExcluded) {
	for (auto it = obj.begin(); it != obj.end(); ++it) {
		if (isExcluded(it.key())) {
			continue;
		}
		if (!PlaylistsDetail::HasValidUtf16CodePoints(it.key())) {
			return false;
		}
		if (!TraverseCanonicalQJsonValue(it.value(), valueDepth, nullptr)) {
			return false;
		}
	}
	return true;
}

template <typename Pred>
[[nodiscard]] QJsonObject CopyPreservedExcluding(
		const QJsonObject &obj,
		Pred isExcluded) {
	auto out = QJsonObject();
	for (auto it = obj.begin(); it != obj.end(); ++it) {
		if (!isExcluded(it.key())) {
			out.insert(it.key(), it.value());
		}
	}
	return out;
}

template <typename Pred, typename EmitTypedFn>
void AppendObjectWithPreserved(
		QByteArray &out,
		std::vector<QString> typedKeys,
		const QJsonObject &preserved,
		int valueDepth,
		Pred isExcluded,
		EmitTypedFn &&emitTyped) {
	auto keys = std::move(typedKeys);
	keys.reserve(keys.size() + std::size_t(preserved.size()));
	for (auto it = preserved.begin(); it != preserved.end(); ++it) {
		if (!isExcluded(it.key())) {
			keys.push_back(it.key());
		}
	}
	std::sort(keys.begin(), keys.end(), LessUtf16);
	out.append('{');
	for (std::size_t i = 0; i < keys.size(); ++i) {
		if (i > 0) {
			out.append(',');
		}
		const auto &key = keys[i];
		PlaylistsDetail::AppendJsonString(out, key);
		out.append(':');
		if (!emitTyped(key)) {
			[[maybe_unused]] const auto ok = TraverseCanonicalQJsonValue(
				preserved.value(key),
				valueDepth,
				&out);
		}
	}
	out.append('}');
}

void AppendDueSeenEntry(
		QByteArray &out,
		const PlaylistLocalDueSeenEntry &entry) {
	auto typed = std::vector<QString>{
		u"deletion_stamp"_q,
		u"first_seen_at_ms"_q,
	};
	AppendObjectWithPreserved(
		out,
		std::move(typed),
		entry.preserved,
		3,
		IsKnownDueSeenKey,
		[&](const QString &key) {
			if (key == u"deletion_stamp"_q) {
				PlaylistsDetail::AppendUint64(out, entry.value.deletionStamp);
				return true;
			}
			if (key == u"first_seen_at_ms"_q) {
				PlaylistsDetail::AppendUint64(out, entry.value.firstSeenAtMs);
				return true;
			}
			return false;
		});
}

void AppendHiddenEntry(
		QByteArray &out,
		const PlaylistLocalHiddenEntry &entry,
		uint64_t selfUserId) {
	auto typed = std::vector<QString>{
		u"playlist"_q,
		u"reason"_q,
	};
	AppendObjectWithPreserved(
		out,
		std::move(typed),
		entry.preserved,
		3,
		IsKnownHiddenKey,
		[&](const QString &key) {
			if (key == u"playlist"_q) {
				PlaylistsDetail::AppendPlaylistJson(
					out,
					entry.playlist,
					selfUserId);
				return true;
			}
			if (key == u"reason"_q) {
				out.append(*SerializeReason(entry.reason));
				return true;
			}
			return false;
		});
}

} // namespace

std::map<QString, PlaylistAutoPurgeDueSeen> PlaylistLocalDueSeenValues(
		const PlaylistLocalState &state) {
	auto out = std::map<QString, PlaylistAutoPurgeDueSeen>();
	for (const auto &[plId, entry] : state.dueSeen) {
		out.emplace(plId, entry.value);
	}
	return out;
}

PlaylistLocalValidationResult ValidatePlaylistLocalState(
		const PlaylistLocalState &state,
		uint64_t selfUserId) {
	if (selfUserId > PlaylistsDetail::kMaximumPeerId) {
		return InvalidValidation(PlaylistLocalError::InvalidSelfUserId);
	}
	if (state.format > kPlaylistFormatVersion) {
		return { PlaylistLocalStatus::NewerFormat, PlaylistLocalError::None };
	}
	if (state.format != kPlaylistFormatVersion) {
		return InvalidValidation(PlaylistLocalError::InvalidFormat);
	}
	if (!ValidatePreservedObject(state.preserved, 1, IsKnownRootKey)) {
		return InvalidValidation(PlaylistLocalError::InvalidPreservedJson);
	}
	for (const auto &[plId, entry] : state.dueSeen) {
		if (!IsPlaylistId(plId)) {
			return InvalidValidation(PlaylistLocalError::InvalidId);
		}
		if (!IsValidPlaylistStamp(entry.value.deletionStamp)
			|| !IsValidPlaylistStamp(entry.value.firstSeenAtMs)) {
			return InvalidValidation(PlaylistLocalError::InvalidStamp);
		}
		if (!ValidatePreservedObject(entry.preserved, 3, IsKnownDueSeenKey)) {
			return InvalidValidation(PlaylistLocalError::InvalidPreservedJson);
		}
	}
	for (const auto &[plId, lastUseMs] : state.recentUse) {
		if (!IsPlaylistId(plId)) {
			return InvalidValidation(PlaylistLocalError::InvalidId);
		}
		if (lastUseMs > kMaximumPlaylistStamp) {
			return InvalidValidation(PlaylistLocalError::InvalidRecentUse);
		}
	}
	for (const auto &[plId, entry] : state.hidden) {
		if (!IsPlaylistId(plId)) {
			return InvalidValidation(PlaylistLocalError::InvalidId);
		}
		if (!SerializeReason(entry.reason)) {
			return InvalidValidation(PlaylistLocalError::InvalidReason);
		}
		const auto playlistErr = PlaylistsDetail::ValidatePlaylist(
			entry.playlist,
			selfUserId);
		if (playlistErr != PlaylistLibraryError::None) {
			return InvalidValidation(
				PlaylistLocalError::InvalidPlaylist,
				playlistErr);
		}
		if (!entry.playlist.title
			|| !entry.playlist.deleted
			|| entry.playlist.deleted->value) {
			return InvalidValidation(PlaylistLocalError::InvalidSnapshot);
		}
		if (!ValidatePreservedObject(entry.preserved, 3, IsKnownHiddenKey)) {
			return InvalidValidation(PlaylistLocalError::InvalidPreservedJson);
		}
	}
	return { PlaylistLocalStatus::Valid, PlaylistLocalError::None };
}

PlaylistLocalParseResult ParsePlaylistLocalState(
		const QByteArray &json,
		uint64_t selfUserId,
		std::optional<qsizetype> maximumBytes) {
	if (selfUserId > PlaylistsDetail::kMaximumPeerId) {
		return InvalidParse(PlaylistLocalError::InvalidSelfUserId);
	}
	const auto canonical = CanonicalizeSyncJson(json, maximumBytes);
	if (!canonical) {
		const auto err = (canonical.error == SyncJsonErrorKind::SizeLimit)
			? PlaylistLocalError::SizeLimit
			: PlaylistLocalError::InvalidJson;
		return InvalidParse(err);
	}
	const auto parsed = QJsonDocument::fromJson(canonical.canonical);
	if (!parsed.isObject()) {
		return InvalidParse(PlaylistLocalError::FieldType);
	}
	const auto root = parsed.object();
	if (root.contains(u"format"_q)) {
		const auto formatVal = root.value(u"format"_q);
		if (!formatVal.isDouble()) {
			return InvalidParse(PlaylistLocalError::FieldType);
		}
		const auto formatNum = PlaylistsDetail::ReadBoundedInteger(
			formatVal,
			1,
			PlaylistsDetail::kMaximumSafeInteger);
		if (!formatNum) {
			return InvalidParse(PlaylistLocalError::InvalidFormat);
		}
		if (*formatNum > uint64_t(kPlaylistFormatVersion)) {
			return {
				PlaylistLocalStatus::NewerFormat,
				PlaylistLocalError::None,
				PlaylistLibraryError::None,
				{},
			};
		}
	}

	auto state = DefaultPlaylistLocalState();
	if (root.contains(u"sync"_q)) {
		const auto syncVal = root.value(u"sync"_q);
		if (!syncVal.isBool()) {
			return InvalidParse(PlaylistLocalError::FieldType);
		}
		state.sync = syncVal.toBool();
	}
	if (root.contains(u"due_seen"_q)) {
		const auto dueVal = root.value(u"due_seen"_q);
		if (!dueVal.isObject()) {
			return InvalidParse(PlaylistLocalError::FieldType);
		}
		const auto dueObj = dueVal.toObject();
		for (auto it = dueObj.begin(); it != dueObj.end(); ++it) {
			if (!IsPlaylistId(it.key())) {
				return InvalidParse(PlaylistLocalError::InvalidId);
			}
			if (!it.value().isObject()) {
				return InvalidParse(PlaylistLocalError::FieldType);
			}
			const auto entryObj = it.value().toObject();
			if (!entryObj.contains(u"deletion_stamp"_q)
				|| !entryObj.contains(u"first_seen_at_ms"_q)) {
				return InvalidParse(PlaylistLocalError::MissingField);
			}
			const auto deletionVal = entryObj.value(u"deletion_stamp"_q);
			const auto firstSeenVal = entryObj.value(u"first_seen_at_ms"_q);
			if (!deletionVal.isDouble() || !firstSeenVal.isDouble()) {
				return InvalidParse(PlaylistLocalError::FieldType);
			}
			const auto deletionStamp = PlaylistsDetail::ReadBoundedInteger(
				deletionVal,
				kMinimumPlaylistStamp,
				kMaximumPlaylistStamp);
			const auto firstSeenAtMs = PlaylistsDetail::ReadBoundedInteger(
				firstSeenVal,
				kMinimumPlaylistStamp,
				kMaximumPlaylistStamp);
			if (!deletionStamp || !firstSeenAtMs) {
				return InvalidParse(PlaylistLocalError::InvalidStamp);
			}
			auto entry = PlaylistLocalDueSeenEntry{
				PlaylistAutoPurgeDueSeen{ *deletionStamp, *firstSeenAtMs },
				CopyPreservedExcluding(entryObj, IsKnownDueSeenKey),
			};
			state.dueSeen.emplace(it.key(), std::move(entry));
		}
	}
	if (root.contains(u"recent_use"_q)) {
		const auto recentVal = root.value(u"recent_use"_q);
		if (!recentVal.isObject()) {
			return InvalidParse(PlaylistLocalError::FieldType);
		}
		const auto recentObj = recentVal.toObject();
		for (auto it = recentObj.begin(); it != recentObj.end(); ++it) {
			if (!IsPlaylistId(it.key())) {
				return InvalidParse(PlaylistLocalError::InvalidId);
			}
			if (!it.value().isDouble()) {
				return InvalidParse(PlaylistLocalError::FieldType);
			}
			const auto lastUseMs = PlaylistsDetail::ReadBoundedInteger(
				it.value(),
				0,
				kMaximumPlaylistStamp);
			if (!lastUseMs) {
				return InvalidParse(PlaylistLocalError::InvalidRecentUse);
			}
			state.recentUse.emplace(it.key(), *lastUseMs);
		}
	}
	if (root.contains(u"hidden"_q)) {
		const auto hiddenVal = root.value(u"hidden"_q);
		if (!hiddenVal.isObject()) {
			return InvalidParse(PlaylistLocalError::FieldType);
		}
		const auto hiddenObj = hiddenVal.toObject();
		for (auto it = hiddenObj.begin(); it != hiddenObj.end(); ++it) {
			if (!IsPlaylistId(it.key())) {
				return InvalidParse(PlaylistLocalError::InvalidId);
			}
			if (!it.value().isObject()) {
				return InvalidParse(PlaylistLocalError::FieldType);
			}
			const auto entryObj = it.value().toObject();
			if (!entryObj.contains(u"reason"_q)
				|| !entryObj.contains(u"playlist"_q)) {
				return InvalidParse(PlaylistLocalError::MissingField);
			}
			const auto reasonVal = entryObj.value(u"reason"_q);
			const auto playlistVal = entryObj.value(u"playlist"_q);
			if (!reasonVal.isString() || !playlistVal.isObject()) {
				return InvalidParse(PlaylistLocalError::FieldType);
			}
			const auto reason = ParseReason(reasonVal.toString());
			if (!reason) {
				return InvalidParse(PlaylistLocalError::InvalidReason);
			}
			auto playlist = Playlist();
			if (const auto err = PlaylistsDetail::ParsePlaylistJson(
					playlistVal.toObject(),
					selfUserId,
					playlist)) {
				return InvalidParse(PlaylistLocalError::InvalidPlaylist, *err);
			}
			playlist = PlaylistsDetail::NormalizePlaylist(
				std::move(playlist),
				selfUserId);
			if (!playlist.title
				|| !playlist.deleted
				|| playlist.deleted->value) {
				return InvalidParse(PlaylistLocalError::InvalidSnapshot);
			}
			auto entry = PlaylistLocalHiddenEntry{
				*reason,
				std::move(playlist),
				CopyPreservedExcluding(entryObj, IsKnownHiddenKey),
			};
			state.hidden.emplace(it.key(), std::move(entry));
		}
	}
	state.preserved = CopyPreservedExcluding(root, IsKnownRootKey);
	const auto validation = ValidatePlaylistLocalState(state, selfUserId);
	if (!validation) {
		return InvalidParse(validation.error, validation.playlistError);
	}
	return {
		PlaylistLocalStatus::Valid,
		PlaylistLocalError::None,
		PlaylistLibraryError::None,
		std::move(state),
	};
}

PlaylistLocalWriteResult SerializePlaylistLocalState(
		const PlaylistLocalState &state,
		uint64_t selfUserId) {
	const auto validation = ValidatePlaylistLocalState(state, selfUserId);
	if (!validation) {
		return {
			{},
			validation.status,
			validation.error,
			validation.playlistError,
		};
	}
	auto out = QByteArray();
	auto typedRoot = std::vector<QString>{
		u"format"_q,
		u"sync"_q,
	};
	if (!state.dueSeen.empty()) {
		typedRoot.push_back(u"due_seen"_q);
	}
	if (!state.hidden.empty()) {
		typedRoot.push_back(u"hidden"_q);
	}
	if (!state.recentUse.empty()) {
		typedRoot.push_back(u"recent_use"_q);
	}
	AppendObjectWithPreserved(
		out,
		std::move(typedRoot),
		state.preserved,
		1,
		IsKnownRootKey,
		[&](const QString &key) {
			if (key == u"format"_q) {
				out.append('1');
				return true;
			}
			if (key == u"sync"_q) {
				out.append(state.sync ? "true" : "false");
				return true;
			}
			if (key == u"due_seen"_q) {
				out.append('{');
				auto first = true;
				for (const auto &[plId, entry] : state.dueSeen) {
					if (!first) {
						out.append(',');
					}
					first = false;
					PlaylistsDetail::AppendJsonString(out, plId);
					out.append(':');
					AppendDueSeenEntry(out, entry);
				}
				out.append('}');
				return true;
			}
			if (key == u"recent_use"_q) {
				out.append('{');
				auto first = true;
				for (const auto &[plId, lastUseMs] : state.recentUse) {
					if (!first) {
						out.append(',');
					}
					first = false;
					PlaylistsDetail::AppendJsonString(out, plId);
					out.append(':');
					PlaylistsDetail::AppendUint64(out, lastUseMs);
				}
				out.append('}');
				return true;
			}
			if (key == u"hidden"_q) {
				out.append('{');
				auto first = true;
				for (const auto &[plId, entry] : state.hidden) {
					if (!first) {
						out.append(',');
					}
					first = false;
					PlaylistsDetail::AppendJsonString(out, plId);
					out.append(':');
					AppendHiddenEntry(out, entry, selfUserId);
				}
				out.append('}');
				return true;
			}
			return false;
		});
	return {
		std::move(out),
		PlaylistLocalStatus::Valid,
		PlaylistLocalError::None,
		PlaylistLibraryError::None,
	};
}

} // namespace Purple
