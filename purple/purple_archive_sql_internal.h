/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_archive_sql.h"

namespace Purple {

template <typename Callback>
auto CheckArchiveSqlDerivations(const ArchiveSqlSecurity &security, Callback callback) {
	auto failed = !security.hmacSha256;
	auto checked = security;
	checked.hmacSha256 = [&](const QByteArray &input) -> std::optional<QByteArray> {
		if (failed) { return {}; }
		auto value = security.hmacSha256(input);
		if (!value || value->size() != 32) { failed = true; return {}; }
		return value;
	};
	auto result = callback(checked);
	return failed ? decltype(result)() : std::move(result);
}

}
