/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

// The handful of names the core borrowed from tdesktop's base/basic_types.h
// before it was extracted. Nothing here is Purple's own vocabulary - it exists
// so the same sources compile inside the tdesktop fork, inside an Android fork
// built with the NDK, and inside the standalone test harness, without any of
// the three needing the other two.
//
// When these files are compiled INSIDE tdesktop, base/basic_types.h is on the
// include path and declares all of this itself. Repeating the two using-aliases
// there is harmless - a using-alias redeclared to the exact same type is legal
// C++, which is why they must be spelled `long long` and `unsigned char`, the
// types qint64 and Qt's uchar resolve to on every platform we target. The `_q`
// literal operator is a different matter: a second definition of it in the same
// translation unit is a redefinition error, not a redeclaration. So we defer to
// tdesktop's copy whenever it is reachable, and define our own only when it is
// not. Standalone builds see no base/ directory and take the second branch.

#if defined(__has_include)
#if __has_include("base/basic_types.h")
#define PURPLE_HAS_TDESKTOP_BASIC_TYPES
#endif // __has_include("base/basic_types.h")
#endif // defined(__has_include)

#ifdef PURPLE_HAS_TDESKTOP_BASIC_TYPES

#include "base/basic_types.h"

#else // PURPLE_HAS_TDESKTOP_BASIC_TYPES

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <cstddef>
#include <cstdint>

// `long long` rather than std::int64_t: on a platform where int64_t is `long`,
// an alias to int64_t would be a different type from Qt's qint64 and from the
// int64 tdesktop declares, and redeclaring it there would stop compiling.
using int64 = long long;
using uchar = unsigned char;

// Byte-for-byte tdesktop's own definitions (the non-MSVC branch of
// base/basic_types.h), so a literal means the same thing in both builds:
// a QString or QByteArray viewing the literal's static storage, with no
// allocation and no copy.
[[nodiscard]] inline QByteArray operator""_q(
		const char *data,
		std::size_t size) {
	return QByteArray::fromRawData(data, size);
}

[[nodiscard]] inline QString operator""_q(
		const char16_t *data,
		std::size_t size) {
	return QString::fromRawData(
		reinterpret_cast<const QChar*>(data),
		size);
}

#endif // PURPLE_HAS_TDESKTOP_BASIC_TYPES
