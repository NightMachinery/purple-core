/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_playlists.h"

#include "purple/purple_playlists_detail.h"
#include "purple/purple_sync_json.h"
#include "purple/purple_types.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>

namespace Purple {
namespace {

constexpr auto kMaximumPeerId = PlaylistsDetail::kMaximumPeerId;
constexpr auto kPlaylistIdLength = qsizetype(19);
constexpr auto kMaximumSafeInteger = PlaylistsDetail::kMaximumSafeInteger;

[[nodiscard]] PlaylistLibraryValidationResult InvalidValidation(
		PlaylistLibraryError error) {
	return { PlaylistLibraryStatus::Invalid, error };
}

[[nodiscard]] PlaylistLibraryParseResult InvalidParse(
		PlaylistLibraryError error) {
	return { PlaylistLibraryStatus::Invalid, error, {} };
}

[[nodiscard]] std::optional<qsizetype> CountCodePoints(
		const QString &value,
		bool rejectControls) {
	auto count = qsizetype(0);
	for (auto i = qsizetype(0); i < value.size(); ++i) {
		const auto code = value[i].unicode();
		auto cp = uint32_t(code);
		if (code >= 0xDC00 && code <= 0xDFFF) {
			return std::nullopt;
		}
		if (code >= 0xD800 && code <= 0xDBFF) {
			if (++i == value.size()) {
				return std::nullopt;
			}
			const auto second = value[i].unicode();
			if (second < 0xDC00 || second > 0xDFFF) {
				return std::nullopt;
			}
			cp = 0x10000u
				+ ((uint32_t(code) - 0xD800u) << 10)
				+ (uint32_t(second) - 0xDC00u);
		}
		if (rejectControls
			&& (cp <= 0x1Fu
				|| (cp >= 0x7Fu && cp <= 0x9Fu)
				|| cp == 0x2028u
				|| cp == 0x2029u)) {
			return std::nullopt;
		}
		++count;
	}
	return count;
}

[[nodiscard]] PeerToken NormalizePeer(PeerToken peer, uint64_t selfUserId) {
	if (selfUserId > 0
		&& peer.kind == PeerTokenKind::User
		&& peer.id == selfUserId) {
		return PeerToken();
	}
	return peer;
}

[[nodiscard]] bool SameSourceRef(
		const PeerToken &aPeer,
		int32_t aMsg,
		const PeerToken &bPeer,
		int32_t bMsg,
		uint64_t selfUserId) {
	return aMsg == bMsg
		&& NormalizePeer(aPeer, selfUserId) == NormalizePeer(bPeer, selfUserId);
}

} // namespace

namespace PlaylistsDetail {

bool HasValidUtf16CodePoints(const QString &value) {
	return CountCodePoints(value, false).has_value();
}

std::optional<uint64_t> ReadBoundedInteger(
		const QJsonValue &value,
		uint64_t minValue,
		uint64_t maxValue) {
	if (!value.isDouble()) {
		return std::nullopt;
	}
	const auto number = value.toDouble();
	if (!std::isfinite(number)
		|| number < 0.0
		|| number > double(kMaximumSafeInteger)
		|| std::floor(number) != number) {
		return std::nullopt;
	}
	const auto integer = uint64_t(number);
	if (integer < minValue || integer > maxValue) {
		return std::nullopt;
	}
	return integer;
}

bool CheckAllowedKeys(
		const QJsonObject &object,
		std::initializer_list<QString> allowed) {
	for (auto it = object.begin(); it != object.end(); ++it) {
		const auto key = it.key();
		auto found = false;
		for (const auto &candidate : allowed) {
			if (key == candidate) {
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

void AppendUint64(QByteArray &out, uint64_t value) {
	char buffer[24];
	auto len = 0;
	do {
		buffer[len++] = char('0' + (value % 10));
		value /= 10;
	} while (value != 0);
	while (len > 0) {
		out.append(buffer[--len]);
	}
}

void AppendJsonString(QByteArray &out, const QString &value) {
	constexpr auto hex = "0123456789abcdef";
	out.append('"');
	for (auto i = qsizetype(0); i < value.size(); ++i) {
		const auto code = value[i].unicode();
		switch (code) {
		case '"': out.append("\\\""); break;
		case '\\': out.append("\\\\"); break;
		case '\b': out.append("\\b"); break;
		case '\f': out.append("\\f"); break;
		case '\n': out.append("\\n"); break;
		case '\r': out.append("\\r"); break;
		case '\t': out.append("\\t"); break;
		default:
			if (code < 0x20) {
				out.append("\\u00");
				out.append(hex[code >> 4]);
				out.append(hex[code & 15]);
			} else if (code < 0x80) {
				out.append(char(code));
			} else if (code < 0x800) {
				out.append(char(0xC0 | (code >> 6)));
				out.append(char(0x80 | (code & 0x3F)));
			} else if (code >= 0xD800 && code <= 0xDBFF && i + 1 < value.size()) {
				const auto lo = value[++i].unicode();
				const auto cp = uint32_t(0x10000u)
					+ ((uint32_t(code) - 0xD800u) << 10)
					+ (uint32_t(lo) - 0xDC00u);
				out.append(char(0xF0 | (cp >> 18)));
				out.append(char(0x80 | ((cp >> 12) & 0x3F)));
				out.append(char(0x80 | ((cp >> 6) & 0x3F)));
				out.append(char(0x80 | (cp & 0x3F)));
			} else {
				out.append(char(0xE0 | (code >> 12)));
				out.append(char(0x80 | ((code >> 6) & 0x3F)));
				out.append(char(0x80 | (code & 0x3F)));
			}
			break;
		}
	}
	out.append('"');
}

} // namespace PlaylistsDetail

namespace {

using PlaylistsDetail::AppendJsonString;
using PlaylistsDetail::AppendUint64;
using PlaylistsDetail::CheckAllowedKeys;
using PlaylistsDetail::ReadBoundedInteger;

[[nodiscard]] PlaylistLibraryError ValidateSource(
		const PlaylistSource &source,
		uint64_t registerStamp,
		uint64_t selfUserId) {
	if (!PeerTokenText(source.peer, selfUserId)) {
		return PlaylistLibraryError::InvalidPeerToken;
	}
	if (source.msg < 1 || source.msg > kMaximumPlaylistMessageId) {
		return PlaylistLibraryError::InvalidValue;
	}
	if (!source.username.isEmpty() && !IsValidPlaylistUsername(source.username)) {
		return PlaylistLibraryError::InvalidUsername;
	}
	if (!IsValidPlaylistStamp(source.since) || source.since > registerStamp) {
		return PlaylistLibraryError::InvalidSince;
	}
	if (!IsValidPlaylistMetaText(source.meta.title)
		|| !IsValidPlaylistMetaText(source.meta.performer)
		|| source.meta.duration < 0
		|| source.meta.duration > kMaximumPlaylistDurationSeconds) {
		return PlaylistLibraryError::InvalidMeta;
	}
	if (source.alt.size() > kMaximumPlaylistFallbacks) {
		return PlaylistLibraryError::InvalidAlt;
	}
	for (std::size_t i = 0; i < source.alt.size(); ++i) {
		const auto &entry = source.alt[i];
		if (!PeerTokenText(entry.peer, selfUserId)) {
			return PlaylistLibraryError::InvalidPeerToken;
		}
		if (entry.msg < 1 || entry.msg > kMaximumPlaylistMessageId) {
			return PlaylistLibraryError::InvalidValue;
		}
		if (!entry.username.isEmpty()
			&& !IsValidPlaylistUsername(entry.username)) {
			return PlaylistLibraryError::InvalidUsername;
		}
		if (SameSourceRef(
				entry.peer,
				entry.msg,
				source.peer,
				source.msg,
				selfUserId)) {
			return PlaylistLibraryError::InvalidAlt;
		}
		for (std::size_t j = 0; j < i; ++j) {
			if (SameSourceRef(
					entry.peer,
					entry.msg,
					source.alt[j].peer,
					source.alt[j].msg,
					selfUserId)) {
				return PlaylistLibraryError::InvalidAlt;
			}
		}
	}
	return PlaylistLibraryError::None;
}

void AppendOrderValue(QByteArray &out, const std::vector<uint64_t> &order) {
	out.append('[');
	for (std::size_t i = 0; i < order.size(); ++i) {
		if (i > 0) {
			out.append(',');
		}
		out.append('"');
		AppendUint64(out, order[i]);
		out.append('"');
	}
	out.append(']');
}

void AppendSourceRef(
		QByteArray &out,
		const PlaylistSourceRef &entry,
		uint64_t selfUserId) {
	out.append("{\"msg\":");
	AppendUint64(out, uint64_t(entry.msg));
	out.append(",\"peer\":");
	AppendJsonString(out, *PeerTokenText(entry.peer, selfUserId));
	if (!entry.username.isEmpty()) {
		out.append(",\"un\":");
		AppendJsonString(out, entry.username);
	}
	out.append('}');
}

void AppendSongValue(
		QByteArray &out,
		const PlaylistSongValue &value,
		uint64_t selfUserId) {
	if (!value.source) {
		out.append("false");
		return;
	}
	const auto &src = *value.source;
	out.append('{');
	auto first = true;
	const auto sep = [&]() {
		if (!first) {
			out.append(',');
		}
		first = false;
	};
	if (!src.alt.empty()) {
		sep();
		out.append("\"alt\":[");
		for (std::size_t i = 0; i < src.alt.size(); ++i) {
			if (i > 0) {
				out.append(',');
			}
			AppendSourceRef(out, src.alt[i], selfUserId);
		}
		out.append(']');
	}
	sep();
	out.append("\"meta\":{\"dur\":");
	AppendUint64(out, uint64_t(src.meta.duration));
	out.append(",\"perf\":");
	AppendJsonString(out, src.meta.performer);
	out.append(",\"title\":");
	AppendJsonString(out, src.meta.title);
	out.append('}');
	sep();
	out.append("\"msg\":");
	AppendUint64(out, uint64_t(src.msg));
	sep();
	out.append("\"peer\":");
	AppendJsonString(out, *PeerTokenText(src.peer, selfUserId));
	sep();
	out.append("\"since\":");
	AppendUint64(out, src.since);
	if (!src.username.isEmpty()) {
		sep();
		out.append("\"un\":");
		AppendJsonString(out, src.username);
	}
	out.append('}');
}

[[nodiscard]] bool CanonicalBytesGreater(
		const QByteArray &a,
		const QByteArray &b) {
	const auto common = std::min(a.size(), b.size());
	const auto cmp = std::memcmp(a.constData(), b.constData(), std::size_t(common));
	if (cmp != 0) {
		return cmp > 0;
	}
	return a.size() > b.size();
}

template <typename Value, typename SerializeFn>
[[nodiscard]] PlaylistRegister<Value> JoinRegister(
		const PlaylistRegister<Value> &a,
		const PlaylistRegister<Value> &b,
		SerializeFn &&serializeValue) {
	if (a.stamp != b.stamp) {
		return (a.stamp > b.stamp) ? a : b;
	}
	const auto bytesA = serializeValue(a.value);
	const auto bytesB = serializeValue(b.value);
	return CanonicalBytesGreater(bytesB, bytesA) ? b : a;
}

template <typename Reg, typename NormalizeFn, typename SerializeFn>
[[nodiscard]] std::optional<Reg> JoinOptionalRegister(
		const std::optional<Reg> &a,
		const std::optional<Reg> &b,
		NormalizeFn &&normalize,
		SerializeFn &&serializeValue) {
	if (!a && !b) {
		return std::nullopt;
	}
	if (!a) {
		return normalize(*b);
	}
	if (!b) {
		return normalize(*a);
	}
	return JoinRegister(
		normalize(*a),
		normalize(*b),
		std::forward<SerializeFn>(serializeValue));
}

[[nodiscard]] PlaylistSongRegister NormalizeSongRegister(
		PlaylistSongRegister reg,
		uint64_t selfUserId) {
	if (reg.value.source) {
		reg.value.source->peer = NormalizePeer(reg.value.source->peer, selfUserId);
		for (auto &alt : reg.value.source->alt) {
			alt.peer = NormalizePeer(alt.peer, selfUserId);
		}
	}
	return reg;
}

[[nodiscard]] Playlist JoinPlaylists(
		const Playlist &a,
		const Playlist &b,
		uint64_t selfUserId) {
	auto out = Playlist();
	const auto identity = [](const auto &reg) {
		return reg;
	};
	out.title = JoinOptionalRegister(a.title, b.title, identity, [](const QString &v) {
		auto buf = QByteArray();
		AppendJsonString(buf, v);
		return buf;
	});
	out.deleted = JoinOptionalRegister(a.deleted, b.deleted, identity, [](bool v) {
		return v ? QByteArray("true") : QByteArray("false");
	});
	out.order = JoinOptionalRegister(a.order, b.order, identity, [](const std::vector<uint64_t> &v) {
		auto buf = QByteArray();
		AppendOrderValue(buf, v);
		return buf;
	});
	if (a.songs || b.songs) {
		auto joinedSongs = std::map<uint64_t, PlaylistSongRegister>();
		if (a.songs) {
			for (const auto &[docId, reg] : *a.songs) {
				joinedSongs.emplace(
					docId,
					NormalizeSongRegister(reg, selfUserId));
			}
		}
		if (b.songs) {
			for (const auto &[docId, reg] : *b.songs) {
				auto normB = NormalizeSongRegister(reg, selfUserId);
				auto it = joinedSongs.find(docId);
				if (it == joinedSongs.end()) {
					joinedSongs.emplace(docId, std::move(normB));
				} else {
					it->second = JoinRegister(
						it->second,
						normB,
						[selfUserId](const PlaylistSongValue &v) {
							auto buf = QByteArray();
							AppendSongValue(buf, v, selfUserId);
							return buf;
						});
				}
			}
		}
		out.songs = std::move(joinedSongs);
	}
	return out;
}

[[nodiscard]] std::optional<PlaylistLibraryError> ParseSourceRefJson(
		const QJsonObject &obj,
		uint64_t selfUserId,
		PlaylistSourceRef &out) {
	if (!CheckAllowedKeys(obj, { u"msg"_q, u"peer"_q, u"un"_q })) {
		return PlaylistLibraryError::UnknownKey;
	}
	if (!obj.contains(u"msg"_q) || !obj.contains(u"peer"_q)) {
		return PlaylistLibraryError::MissingField;
	}
	const auto peerVal = obj.value(u"peer"_q);
	const auto msgVal = obj.value(u"msg"_q);
	if (!peerVal.isString() || !msgVal.isDouble()) {
		return PlaylistLibraryError::FieldType;
	}
	const auto peer = ParsePeerToken(peerVal.toString(), selfUserId);
	if (!peer) {
		return PlaylistLibraryError::InvalidPeerToken;
	}
	const auto msg = ReadBoundedInteger(
		msgVal,
		1,
		uint64_t(kMaximumPlaylistMessageId));
	if (!msg) {
		return PlaylistLibraryError::InvalidValue;
	}
	out.peer = *peer;
	out.msg = int32_t(*msg);
	if (obj.contains(u"un"_q)) {
		const auto unVal = obj.value(u"un"_q);
		if (!unVal.isString()) {
			return PlaylistLibraryError::FieldType;
		}
		out.username = unVal.toString();
		if (!IsValidPlaylistUsername(out.username)) {
			return PlaylistLibraryError::InvalidUsername;
		}
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<PlaylistLibraryError> ParseSourceJson(
		const QJsonObject &obj,
		uint64_t registerStamp,
		uint64_t selfUserId,
		PlaylistSource &out) {
	if (!CheckAllowedKeys(obj, {
		u"alt"_q, u"meta"_q, u"msg"_q, u"peer"_q, u"since"_q, u"un"_q,
	})) {
		return PlaylistLibraryError::UnknownKey;
	}
	for (const auto &required : {
		u"meta"_q, u"msg"_q, u"peer"_q, u"since"_q,
	}) {
		if (!obj.contains(required)) {
			return PlaylistLibraryError::MissingField;
		}
	}
	const auto peerVal = obj.value(u"peer"_q);
	const auto msgVal = obj.value(u"msg"_q);
	const auto metaVal = obj.value(u"meta"_q);
	const auto sinceVal = obj.value(u"since"_q);
	if (!peerVal.isString()
		|| !msgVal.isDouble()
		|| !metaVal.isObject()
		|| !sinceVal.isDouble()) {
		return PlaylistLibraryError::FieldType;
	}
	const auto peer = ParsePeerToken(peerVal.toString(), selfUserId);
	if (!peer) {
		return PlaylistLibraryError::InvalidPeerToken;
	}
	const auto msg = ReadBoundedInteger(
		msgVal,
		1,
		uint64_t(kMaximumPlaylistMessageId));
	if (!msg) {
		return PlaylistLibraryError::InvalidValue;
	}
	const auto since = ReadBoundedInteger(
		sinceVal,
		kMinimumPlaylistStamp,
		kMaximumPlaylistStamp);
	if (!since || *since > registerStamp) {
		return PlaylistLibraryError::InvalidSince;
	}
	out.peer = *peer;
	out.msg = int32_t(*msg);
	out.since = *since;
	if (obj.contains(u"un"_q)) {
		const auto unVal = obj.value(u"un"_q);
		if (!unVal.isString()) {
			return PlaylistLibraryError::FieldType;
		}
		out.username = unVal.toString();
		if (!IsValidPlaylistUsername(out.username)) {
			return PlaylistLibraryError::InvalidUsername;
		}
	}
	const auto metaObj = metaVal.toObject();
	if (!CheckAllowedKeys(metaObj, { u"dur"_q, u"perf"_q, u"title"_q })) {
		return PlaylistLibraryError::UnknownKey;
	}
	for (const auto &required : { u"dur"_q, u"perf"_q, u"title"_q }) {
		if (!metaObj.contains(required)) {
			return PlaylistLibraryError::MissingField;
		}
	}
	const auto durVal = metaObj.value(u"dur"_q);
	const auto perfVal = metaObj.value(u"perf"_q);
	const auto titleVal = metaObj.value(u"title"_q);
	if (!durVal.isDouble() || !perfVal.isString() || !titleVal.isString()) {
		return PlaylistLibraryError::FieldType;
	}
	const auto dur = ReadBoundedInteger(
		durVal,
		0,
		uint64_t(kMaximumPlaylistDurationSeconds));
	if (!dur
		|| !IsValidPlaylistMetaText(titleVal.toString())
		|| !IsValidPlaylistMetaText(perfVal.toString())) {
		return PlaylistLibraryError::InvalidMeta;
	}
	out.meta.title = titleVal.toString();
	out.meta.performer = perfVal.toString();
	out.meta.duration = int32_t(*dur);
	if (obj.contains(u"alt"_q)) {
		const auto altVal = obj.value(u"alt"_q);
		if (!altVal.isArray()) {
			return PlaylistLibraryError::FieldType;
		}
		const auto altArr = altVal.toArray();
		if (altArr.isEmpty() || std::size_t(altArr.size()) > kMaximumPlaylistFallbacks) {
			return PlaylistLibraryError::InvalidAlt;
		}
		out.alt.reserve(std::size_t(altArr.size()));
		for (const auto &item : altArr) {
			if (!item.isObject()) {
				return PlaylistLibraryError::FieldType;
			}
			auto ref = PlaylistSourceRef();
			if (const auto err = ParseSourceRefJson(
					item.toObject(),
					selfUserId,
					ref)) {
				return err;
			}
			out.alt.push_back(std::move(ref));
		}
	}
	const auto finalErr = ValidateSource(out, registerStamp, selfUserId);
	if (finalErr != PlaylistLibraryError::None) {
		return finalErr;
	}
	return std::nullopt;
}

} // namespace

namespace PlaylistsDetail {

PlaylistLibraryError ValidatePlaylist(
		const Playlist &playlist,
		uint64_t selfUserId) {
	if (!playlist.title
		&& !playlist.deleted
		&& !playlist.order
		&& !playlist.songs) {
		return PlaylistLibraryError::EmptyPlaylist;
	}
	if (playlist.title) {
		if (!IsValidPlaylistTitle(playlist.title->value)) {
			return PlaylistLibraryError::InvalidTitle;
		}
		if (!IsValidPlaylistStamp(playlist.title->stamp)) {
			return PlaylistLibraryError::InvalidStamp;
		}
	}
	if (playlist.deleted && !IsValidPlaylistStamp(playlist.deleted->stamp)) {
		return PlaylistLibraryError::InvalidStamp;
	}
	if (playlist.order) {
		if (!IsValidPlaylistStamp(playlist.order->stamp)) {
			return PlaylistLibraryError::InvalidStamp;
		}
		auto seen = std::unordered_set<uint64_t>();
		seen.reserve(playlist.order->value.size());
		for (const auto docId : playlist.order->value) {
			if (docId == 0) {
				return PlaylistLibraryError::InvalidDocumentId;
			}
			if (!seen.insert(docId).second) {
				return PlaylistLibraryError::InvalidOrder;
			}
		}
	}
	if (playlist.songs) {
		for (const auto &[docId, reg] : *playlist.songs) {
			if (docId == 0) {
				return PlaylistLibraryError::InvalidDocumentId;
			}
			if (!IsValidPlaylistStamp(reg.stamp)) {
				return PlaylistLibraryError::InvalidStamp;
			}
			if (reg.value.source) {
				const auto err = ValidateSource(
					*reg.value.source,
					reg.stamp,
					selfUserId);
				if (err != PlaylistLibraryError::None) {
					return err;
				}
			}
		}
	}
	return PlaylistLibraryError::None;
}

Playlist NormalizePlaylist(
		Playlist playlist,
		uint64_t selfUserId) {
	if (playlist.songs) {
		for (auto &entry : *playlist.songs) {
			entry.second = NormalizeSongRegister(
				std::move(entry.second), selfUserId);
		}
	}
	return playlist;
}

std::optional<PlaylistLibraryError> ParsePlaylistJson(
		const QJsonObject &obj,
		uint64_t selfUserId,
		Playlist &out) {
	if (!CheckAllowedKeys(obj, {
		u"deleted"_q, u"order"_q, u"songs"_q, u"title"_q,
	})) {
		return PlaylistLibraryError::UnknownKey;
	}
	if (obj.isEmpty()) {
		return PlaylistLibraryError::EmptyPlaylist;
	}
	if (obj.contains(u"title"_q)) {
		const auto regVal = obj.value(u"title"_q);
		if (!regVal.isArray()) {
			return PlaylistLibraryError::FieldType;
		}
		const auto arr = regVal.toArray();
		if (arr.size() != 2 || !arr[0].isString() || !arr[1].isDouble()) {
			return PlaylistLibraryError::FieldType;
		}
		const auto stamp = ReadBoundedInteger(
			arr[1],
			kMinimumPlaylistStamp,
			kMaximumPlaylistStamp);
		if (!stamp) {
			return PlaylistLibraryError::InvalidStamp;
		}
		const auto title = arr[0].toString();
		if (!IsValidPlaylistTitle(title)) {
			return PlaylistLibraryError::InvalidTitle;
		}
		out.title = PlaylistTitleRegister{ title, *stamp };
	}
	if (obj.contains(u"deleted"_q)) {
		const auto regVal = obj.value(u"deleted"_q);
		if (!regVal.isArray()) {
			return PlaylistLibraryError::FieldType;
		}
		const auto arr = regVal.toArray();
		if (arr.size() != 2 || !arr[0].isBool() || !arr[1].isDouble()) {
			return PlaylistLibraryError::FieldType;
		}
		const auto stamp = ReadBoundedInteger(
			arr[1],
			kMinimumPlaylistStamp,
			kMaximumPlaylistStamp);
		if (!stamp) {
			return PlaylistLibraryError::InvalidStamp;
		}
		out.deleted = PlaylistDeletedRegister{ arr[0].toBool(), *stamp };
	}
	if (obj.contains(u"order"_q)) {
		const auto regVal = obj.value(u"order"_q);
		if (!regVal.isArray()) {
			return PlaylistLibraryError::FieldType;
		}
		const auto arr = regVal.toArray();
		if (arr.size() != 2 || !arr[0].isArray() || !arr[1].isDouble()) {
			return PlaylistLibraryError::FieldType;
		}
		const auto stamp = ReadBoundedInteger(
			arr[1],
			kMinimumPlaylistStamp,
			kMaximumPlaylistStamp);
		if (!stamp) {
			return PlaylistLibraryError::InvalidStamp;
		}
		const auto idsArr = arr[0].toArray();
		auto order = std::vector<uint64_t>();
		order.reserve(std::size_t(idsArr.size()));
		auto seen = std::unordered_set<uint64_t>();
		seen.reserve(std::size_t(idsArr.size()));
		for (const auto &item : idsArr) {
			if (!item.isString()) {
				return PlaylistLibraryError::FieldType;
			}
			const auto docId = ParsePlaylistDocumentId(item.toString());
			if (!docId) {
				return PlaylistLibraryError::InvalidDocumentId;
			}
			if (!seen.insert(*docId).second) {
				return PlaylistLibraryError::InvalidOrder;
			}
			order.push_back(*docId);
		}
		out.order = PlaylistOrderRegister{ std::move(order), *stamp };
	}
	if (obj.contains(u"songs"_q)) {
		const auto songsVal = obj.value(u"songs"_q);
		if (!songsVal.isObject()) {
			return PlaylistLibraryError::FieldType;
		}
		const auto songsObj = songsVal.toObject();
		auto songs = std::map<uint64_t, PlaylistSongRegister>();
		for (auto it = songsObj.begin(); it != songsObj.end(); ++it) {
			const auto docId = ParsePlaylistDocumentId(it.key());
			if (!docId) {
				return PlaylistLibraryError::InvalidDocumentId;
			}
			if (!it.value().isArray()) {
				return PlaylistLibraryError::FieldType;
			}
			const auto arr = it.value().toArray();
			if (arr.size() != 2 || !arr[1].isDouble()) {
				return PlaylistLibraryError::FieldType;
			}
			const auto stamp = ReadBoundedInteger(
				arr[1],
				kMinimumPlaylistStamp,
				kMaximumPlaylistStamp);
			if (!stamp) {
				return PlaylistLibraryError::InvalidStamp;
			}
			auto songVal = PlaylistSongValue();
			if (arr[0].isBool()) {
				if (arr[0].toBool()) {
					return PlaylistLibraryError::InvalidValue;
				}
			} else if (arr[0].isObject()) {
				auto source = PlaylistSource();
				if (const auto err = ParseSourceJson(
						arr[0].toObject(),
						*stamp,
						selfUserId,
						source)) {
					return err;
				}
				songVal.source = std::move(source);
			} else {
				return PlaylistLibraryError::FieldType;
			}
			songs.emplace(*docId, PlaylistSongRegister{ std::move(songVal), *stamp });
		}
		out.songs = std::move(songs);
	}
	return std::nullopt;
}

void AppendPlaylistJson(
		QByteArray &out,
		const Playlist &playlist,
		uint64_t selfUserId) {
	out.append('{');
	auto firstField = true;
	const auto sepField = [&]() {
		if (!firstField) {
			out.append(',');
		}
		firstField = false;
	};
	if (playlist.deleted) {
		sepField();
		out.append("\"deleted\":[");
		out.append(playlist.deleted->value ? "true," : "false,");
		AppendUint64(out, playlist.deleted->stamp);
		out.append(']');
	}
	if (playlist.order) {
		sepField();
		out.append("\"order\":[");
		AppendOrderValue(out, playlist.order->value);
		out.append(',');
		AppendUint64(out, playlist.order->stamp);
		out.append(']');
	}
	if (playlist.songs) {
		sepField();
		out.append("\"songs\":{");
		// RFC 8785 sorts object keys lexicographically ("10" < "2"), unlike std::map<uint64_t>.
		auto ordered = std::vector<std::pair<QByteArray, const PlaylistSongRegister *>>();
		ordered.reserve(playlist.songs->size());
		for (const auto &[docId, reg] : *playlist.songs) {
			auto keyBytes = QByteArray();
			AppendUint64(keyBytes, docId);
			ordered.emplace_back(std::move(keyBytes), &reg);
		}
		std::sort(ordered.begin(), ordered.end(), [](const auto &lhs, const auto &rhs) {
			return lhs.first < rhs.first;
		});
		for (std::size_t i = 0; i < ordered.size(); ++i) {
			if (i > 0) {
				out.append(',');
			}
			out.append('"');
			out.append(ordered[i].first);
			out.append("\":[");
			AppendSongValue(out, ordered[i].second->value, selfUserId);
			out.append(',');
			AppendUint64(out, ordered[i].second->stamp);
			out.append(']');
		}
		out.append('}');
	}
	if (playlist.title) {
		sepField();
		out.append("\"title\":[");
		AppendJsonString(out, playlist.title->value);
		out.append(',');
		AppendUint64(out, playlist.title->stamp);
		out.append(']');
	}
	out.append('}');
}

} // namespace PlaylistsDetail

bool IsPlaylistId(const QString &value) {
	if (value.size() != kPlaylistIdLength || !value.startsWith(u"pl-"_q)) {
		return false;
	}
	for (auto i = qsizetype(3); i < kPlaylistIdLength; ++i) {
		const auto ch = value[i];
		if (!((ch >= u'a' && ch <= u'z') || (ch >= u'2' && ch <= u'7'))) {
			return false;
		}
	}
	return true;
}

std::optional<uint64_t> ParsePlaylistDocumentId(const QString &text) {
	if (text.isEmpty() || text.size() > 20) {
		return std::nullopt;
	}
	const auto first = text[0];
	if (first < u'1' || first > u'9') {
		return std::nullopt;
	}
	auto id = uint64_t(0);
	constexpr auto kMaxUint64 = std::numeric_limits<uint64_t>::max();
	for (const auto ch : text) {
		if (ch < u'0' || ch > u'9') {
			return std::nullopt;
		}
		const auto digit = uint64_t(ch.unicode() - u'0');
		if (id > (kMaxUint64 - digit) / 10) {
			return std::nullopt;
		}
		id = id * 10 + digit;
	}
	return id;
}

std::optional<QString> FormatPlaylistDocumentId(uint64_t id) {
	if (id == 0) {
		return std::nullopt;
	}
	return QString::number(qulonglong(id));
}

bool IsValidPlaylistUsername(const QString &value) {
	if (value.size() < kMinimumPlaylistUsernameLength
		|| value.size() > kMaximumPlaylistUsernameLength) {
		return false;
	}
	for (const auto ch : value) {
		if (!((ch >= u'A' && ch <= u'Z')
			|| (ch >= u'a' && ch <= u'z')
			|| (ch >= u'0' && ch <= u'9')
			|| ch == u'_')) {
			return false;
		}
	}
	return true;
}

bool IsValidPlaylistTitle(const QString &value) {
	const auto count = CountCodePoints(value, true);
	if (!count
		|| *count < 1
		|| *count > kMaximumPlaylistTitleCodePoints) {
		return false;
	}
	return value.trimmed() == value;
}

bool IsValidPlaylistMetaText(const QString &value) {
	const auto count = CountCodePoints(value, false);
	return count.has_value() && *count <= kMaximumPlaylistMetaCodePoints;
}

bool IsValidPlaylistStamp(uint64_t stamp) {
	return stamp >= kMinimumPlaylistStamp && stamp <= kMaximumPlaylistStamp;
}

PlaylistLibraryValidationResult ValidatePlaylistLibrary(
		const PlaylistLibrary &library,
		uint64_t selfUserId) {
	if (selfUserId > kMaximumPeerId) {
		return InvalidValidation(PlaylistLibraryError::InvalidSelfUserId);
	}
	if (library.format > kPlaylistFormatVersion) {
		return { PlaylistLibraryStatus::NewerFormat, PlaylistLibraryError::None };
	}
	if (library.format != kPlaylistFormatVersion) {
		return InvalidValidation(PlaylistLibraryError::InvalidFormat);
	}
	for (const auto &purgedId : library.purged) {
		if (!IsPlaylistId(purgedId)) {
			return InvalidValidation(PlaylistLibraryError::InvalidId);
		}
		if (library.playlists.contains(purgedId)) {
			return InvalidValidation(PlaylistLibraryError::PurgedPlaylistPresent);
		}
	}
	for (const auto &[plId, playlist] : library.playlists) {
		if (!IsPlaylistId(plId)) {
			return InvalidValidation(PlaylistLibraryError::InvalidId);
		}
		const auto err = PlaylistsDetail::ValidatePlaylist(playlist, selfUserId);
		if (err != PlaylistLibraryError::None) {
			return InvalidValidation(err);
		}
	}
	return { PlaylistLibraryStatus::Valid, PlaylistLibraryError::None };
}

PlaylistLibraryParseResult ParsePlaylistLibrary(
		const QByteArray &json,
		uint64_t selfUserId,
		std::optional<qsizetype> maximumBytes) {
	if (selfUserId > kMaximumPeerId) {
		return InvalidParse(PlaylistLibraryError::InvalidSelfUserId);
	}
	const auto canonical = CanonicalizeSyncJson(json, maximumBytes);
	if (!canonical) {
		const auto err = (canonical.error == SyncJsonErrorKind::SizeLimit)
			? PlaylistLibraryError::SizeLimit
			: PlaylistLibraryError::InvalidJson;
		return InvalidParse(err);
	}
	const auto parsed = QJsonDocument::fromJson(canonical.canonical);
	if (!parsed.isObject()) {
		return InvalidParse(PlaylistLibraryError::FieldType);
	}
	const auto root = parsed.object();
	if (!root.contains(u"format"_q)) {
		return InvalidParse(PlaylistLibraryError::MissingField);
	}
	const auto formatVal = root.value(u"format"_q);
	if (!formatVal.isDouble()) {
		return InvalidParse(PlaylistLibraryError::FieldType);
	}
	const auto formatNum = PlaylistsDetail::ReadBoundedInteger(
		formatVal,
		1,
		kMaximumSafeInteger);
	if (!formatNum) {
		return InvalidParse(PlaylistLibraryError::InvalidFormat);
	}
	if (*formatNum > uint64_t(kPlaylistFormatVersion)) {
		return { PlaylistLibraryStatus::NewerFormat, PlaylistLibraryError::None, {} };
	}
	if (!PlaylistsDetail::CheckAllowedKeys(root, { u"format"_q, u"playlists"_q, u"purged"_q })) {
		return InvalidParse(PlaylistLibraryError::UnknownKey);
	}
	if (!root.contains(u"playlists"_q) || !root.contains(u"purged"_q)) {
		return InvalidParse(PlaylistLibraryError::MissingField);
	}
	const auto playlistsVal = root.value(u"playlists"_q);
	const auto purgedVal = root.value(u"purged"_q);
	if (!playlistsVal.isObject() || !purgedVal.isObject()) {
		return InvalidParse(PlaylistLibraryError::FieldType);
	}
	auto library = PlaylistLibrary();
	const auto purgedObj = purgedVal.toObject();
	for (auto it = purgedObj.begin(); it != purgedObj.end(); ++it) {
		if (!IsPlaylistId(it.key())) {
			return InvalidParse(PlaylistLibraryError::InvalidId);
		}
		if (!it.value().isDouble()) {
			return InvalidParse(PlaylistLibraryError::FieldType);
		}
		if (it.value().toDouble() != 1.0) {
			return InvalidParse(PlaylistLibraryError::InvalidPurged);
		}
		library.purged.insert(it.key());
	}
	const auto playlistsObj = playlistsVal.toObject();
	for (auto it = playlistsObj.begin(); it != playlistsObj.end(); ++it) {
		if (!IsPlaylistId(it.key())) {
			return InvalidParse(PlaylistLibraryError::InvalidId);
		}
		if (library.purged.contains(it.key())) {
			return InvalidParse(PlaylistLibraryError::PurgedPlaylistPresent);
		}
		if (!it.value().isObject()) {
			return InvalidParse(PlaylistLibraryError::FieldType);
		}
		auto playlist = Playlist();
		if (const auto err = PlaylistsDetail::ParsePlaylistJson(
				it.value().toObject(),
				selfUserId,
				playlist)) {
			return InvalidParse(*err);
		}
		library.playlists.emplace(it.key(), std::move(playlist));
	}
	return { PlaylistLibraryStatus::Valid, PlaylistLibraryError::None, std::move(library) };
}

PlaylistLibraryWriteResult SerializePlaylistLibrary(
		const PlaylistLibrary &library,
		uint64_t selfUserId) {
	const auto validation = ValidatePlaylistLibrary(library, selfUserId);
	if (!validation) {
		return { {}, validation.status, validation.error };
	}
	auto out = QByteArray();
	out.append("{\"format\":1,\"playlists\":{");
	auto firstPlaylist = true;
	for (const auto &[plId, playlist] : library.playlists) {
		if (!firstPlaylist) {
			out.append(',');
		}
		firstPlaylist = false;
		PlaylistsDetail::AppendJsonString(out, plId);
		out.append(':');
		PlaylistsDetail::AppendPlaylistJson(out, playlist, selfUserId);
	}
	out.append("},\"purged\":{");
	auto firstPurged = true;
	for (const auto &purgedId : library.purged) {
		if (!firstPurged) {
			out.append(',');
		}
		firstPurged = false;
		PlaylistsDetail::AppendJsonString(out, purgedId);
		out.append(":1");
	}
	out.append("}}");
	return { std::move(out), PlaylistLibraryStatus::Valid, PlaylistLibraryError::None };
}

PlaylistLibraryJoinResult JoinPlaylistLibraries(
		const PlaylistLibrary &a,
		const PlaylistLibrary &b,
		uint64_t selfUserId) {
	const auto validA = ValidatePlaylistLibrary(a, selfUserId);
	if (!validA) {
		return { validA.status, validA.error, {} };
	}
	const auto validB = ValidatePlaylistLibrary(b, selfUserId);
	if (!validB) {
		return { validB.status, validB.error, {} };
	}
	auto result = PlaylistLibrary();
	result.format = kPlaylistFormatVersion;
	result.purged = a.purged;
	result.purged.insert(b.purged.begin(), b.purged.end());
	for (const auto &[plId, playlistA] : a.playlists) {
		if (result.purged.contains(plId)) {
			continue;
		}
		const auto itB = b.playlists.find(plId);
		if (itB == b.playlists.end()) {
			result.playlists.emplace(
				plId,
				PlaylistsDetail::NormalizePlaylist(playlistA, selfUserId));
		} else {
			result.playlists.emplace(
				plId,
				JoinPlaylists(playlistA, itB->second, selfUserId));
		}
	}
	for (const auto &[plId, playlistB] : b.playlists) {
		if (result.purged.contains(plId) || a.playlists.contains(plId)) {
			continue;
		}
		result.playlists.emplace(
			plId,
			PlaylistsDetail::NormalizePlaylist(playlistB, selfUserId));
	}
	return {
		PlaylistLibraryStatus::Valid,
		PlaylistLibraryError::None,
		std::move(result),
	};
}

PlaylistLibraryContainsResult PlaylistLibraryContains(
		const PlaylistLibrary &outer,
		const PlaylistLibrary &inner,
		uint64_t selfUserId) {
	const auto outerBytes = SerializePlaylistLibrary(outer, selfUserId);
	if (!outerBytes) {
		return { outerBytes.status, outerBytes.error, false };
	}
	const auto joined = JoinPlaylistLibraries(outer, inner, selfUserId);
	if (!joined) {
		return { joined.status, joined.error, false };
	}
	const auto joinedBytes = SerializePlaylistLibrary(joined.library, selfUserId);
	if (!joinedBytes) {
		return { joinedBytes.status, joinedBytes.error, false };
	}
	return {
		PlaylistLibraryStatus::Valid,
		PlaylistLibraryError::None,
		joinedBytes.canonical == outerBytes.canonical,
	};
}

} // namespace Purple
