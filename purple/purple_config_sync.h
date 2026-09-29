/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_types.h"

#include <QtCore/QString>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace Purple {

struct ConfigVersion {
	QString key;
	std::vector<QString> parents;
	std::vector<QString> lineage;
};

struct ConfigVersionKey {
	uint64_t generation = 0;
	QString fingerprint;
};

[[nodiscard]] std::optional<ConfigVersionKey> ParseConfigVersionKey(
	const QString &key);
[[nodiscard]] bool IsConfigVersionKey(const QString &key);
[[nodiscard]] std::optional<ConfigVersion> MakeConfigVersion(
	const QByteArray &text,
	const std::vector<ConfigVersion> &parents);

struct ConfigSyncState {
	QString space;
	QString install;
	QString base;
	std::vector<QString> baseLineage;
	std::vector<QString> equiv;
	QString pending;
	std::map<QString, uint64_t> seenSeq;
};

struct ConfigHead {
	QString space;
	QString install;
	uint64_t seq = 0;
	QString key;
	std::vector<QString> lineage;
};

enum class ConfigHeadKind {
	Invalid,
	Stale,
	Same,
	Ahead,
	Concurrent,
	Unrelated,
};

struct ConfigHeadOutcome {
	ConfigHead head;
	ConfigHeadKind kind = ConfigHeadKind::Invalid;
};

struct ConfigClassification {
	std::vector<ConfigHeadOutcome> heads;
	bool split = false;
	bool inputValid = true;
};

[[nodiscard]] ConfigClassification ClassifyConfig(
	const QString &localFp,
	const ConfigSyncState &state,
	const std::vector<ConfigHead> &remoteHeads);

} // namespace Purple
