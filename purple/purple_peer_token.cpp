/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_peer_token.h"

#include "purple/purple_types.h"

#include <limits>

namespace Purple {
namespace {

constexpr auto kMaximumPeerId = uint64_t(std::numeric_limits<int64_t>::max());

[[nodiscard]] QString Prefix(PeerTokenKind kind) {
	switch (kind) {
	case PeerTokenKind::User: return u"user:"_q;
	case PeerTokenKind::Chat: return u"chat:"_q;
	case PeerTokenKind::Channel: return u"channel:"_q;
	case PeerTokenKind::Self: break;
	}
	return QString();
}

}

std::optional<PeerToken> ParsePeerToken(
		const QString &text,
		uint64_t selfUserId) {
	if (selfUserId > kMaximumPeerId) {
		return std::nullopt;
	}
	if (text == u"self"_q) {
		return PeerToken();
	}
	for (const auto kind : {
		PeerTokenKind::User,
		PeerTokenKind::Chat,
		PeerTokenKind::Channel,
	}) {
		const auto prefix = Prefix(kind);
		if (!text.startsWith(prefix)) {
			continue;
		}
		const auto digits = QStringView(text).mid(prefix.size());
		if (digits.isEmpty() || digits.front() < u'1'
			|| digits.front() > u'9') {
			return std::nullopt;
		}
		auto id = uint64_t(0);
		for (const auto ch : digits) {
			if (ch < u'0' || ch > u'9') {
				return std::nullopt;
			}
			const auto digit = uint64_t(ch.unicode() - u'0');
			if (id > (kMaximumPeerId - digit) / 10) {
				return std::nullopt;
			}
			id = id * 10 + digit;
		}
		return (kind == PeerTokenKind::User && id == selfUserId)
			? PeerToken()
			: PeerToken{ kind, id };
	}
	return std::nullopt;
}

std::optional<QString> PeerTokenText(
		const PeerToken &peer,
		uint64_t selfUserId) {
	if (selfUserId > kMaximumPeerId) {
		return std::nullopt;
	}
	if (peer.kind == PeerTokenKind::Self) {
		return (peer.id == 0) ? std::optional(u"self"_q) : std::nullopt;
	}
	const auto prefix = Prefix(peer.kind);
	if (prefix.isEmpty() || !peer.id || peer.id > kMaximumPeerId) {
		return std::nullopt;
	}
	return (peer.kind == PeerTokenKind::User && peer.id == selfUserId)
		? u"self"_q
		: prefix + QString::number(qulonglong(peer.id));
}

std::optional<int> ComparePeerTokens(
		const PeerToken &a,
		const PeerToken &b,
		uint64_t selfUserId) {
	const auto first = PeerTokenText(a, selfUserId);
	const auto second = PeerTokenText(b, selfUserId);
	return (first && second)
		? std::optional(QString::compare(*first, *second, Qt::CaseSensitive))
		: std::nullopt;
}

}
