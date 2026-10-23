/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include <QtCore/QString>

#include <cstdint>
#include <optional>

namespace Purple {

enum class PeerTokenKind {
	Self,
	User,
	Chat,
	Channel,
};

struct PeerToken {
	PeerTokenKind kind = PeerTokenKind::Self;
	uint64_t id = 0;

	[[nodiscard]] bool operator==(const PeerToken &) const = default;
};

[[nodiscard]] std::optional<PeerToken> ParsePeerToken(
	const QString &text,
	uint64_t selfUserId = 0);
[[nodiscard]] std::optional<QString> PeerTokenText(
	const PeerToken &peer,
	uint64_t selfUserId = 0);
[[nodiscard]] std::optional<int> ComparePeerTokens(
	const PeerToken &a,
	const PeerToken &b,
	uint64_t selfUserId = 0);

}
