/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_passcode.h"

#include <iterator>

namespace Purple {

QString PersianKeyboardToEnglish(QString candidate) {
	constexpr char16_t kPersian[] = u"ضصثقفغعهخحجچشسیبلاتنمکگظطزرذدپو.ًٌٍَُِّْ][}{|ؤئيإأآة»«:؛كٓژٰ‌ٔء<>؟٬٫﷼٪×،)(ـ۱۲۳۴۵۶۷۸۹۰";
	constexpr char16_t kEnglish[] = u"qwertyuiop[]asdfghjkl;'zxcvbnm,.QWERTYUIOP{}|ASDFGHJKL:\"ZXCVBNM<>?@#$%^&()_1234567890";
	static_assert(std::size(kPersian) == std::size(kEnglish));

	const auto source = QString::fromUtf16(kPersian);
	for (auto i = 0; i != candidate.size(); ++i) {
		const auto index = source.indexOf(candidate.at(i));
		if (index >= 0) {
			candidate[i] = kEnglish[index];
		}
	}
	return candidate;
}

} // namespace Purple
