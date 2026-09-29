/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include <QtCore/QByteArray>

namespace Purple {

enum class SyncJsonErrorKind {
	None,
	Syntax,
	InvalidUtf8,
	InvalidUnicode,
	DuplicateKey,
	NonInteger,
	NumberRange,
	DepthLimit,
	SizeLimit,
};

struct SyncJsonResult {
	QByteArray canonical;
	SyncJsonErrorKind error = SyncJsonErrorKind::None;
	qsizetype offset = 0;

	[[nodiscard]] explicit operator bool() const {
		return error == SyncJsonErrorKind::None;
	}
};

[[nodiscard]] SyncJsonResult CanonicalizeSyncJson(const QByteArray &json);

} // namespace Purple
