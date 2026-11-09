#pragma once

#include "purple/purple_splice.h"
#include <QtCore/QJsonValue>
#include <QtCore/QJsonObject>

namespace Purple {
[[nodiscard]] SpliceResult SetMediaReplyValue(
	const QString &text,
	const QString &path,
	const QString &row,
	const QString &key,
	const QJsonValue &value);
[[nodiscard]] QJsonObject MediaReplySettingsJson(const Settings &settings);
} // namespace Purple
