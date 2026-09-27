/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include <QtCore/QString>

namespace Purple {

[[nodiscard]] QString PersianKeyboardToEnglish(QString candidate);

} // namespace Purple
