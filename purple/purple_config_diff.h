/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_types.h"
#include "purple/purple_settings.h"
#include "purple/purple_local_choice.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <optional>
#include <vector>

namespace Purple {

inline constexpr auto kConfigDiffEditLimit = 1000;

enum class ConfigDiffLineKind {
	Context,
	Removed,
	Added,
};

struct ConfigDiffLine {
	ConfigDiffLineKind kind = ConfigDiffLineKind::Context;
	int oldLine = 0;
	int newLine = 0;
	QString text;
};

struct ConfigDiffHunk {
	int oldStart = 0;
	int oldCount = 0;
	int newStart = 0;
	int newCount = 0;
	std::vector<ConfigDiffLine> lines;
};

struct ConfigTextDiff {
	std::vector<ConfigDiffHunk> hunks;
	int added = 0;
	int removed = 0;
	bool identical = false;
	bool truncated = false;
};

[[nodiscard]] ConfigTextDiff DiffConfigText(
	const QByteArray &before,
	const QByteArray &after,
	int context = 3);

enum class ConfigChangeKind {
	Added,
	Removed,
	Changed,
};

enum class ConfigChangeAddressKind {
	Key,
	Unit,
	ListDefinition,
};

struct ConfigKeyChange {
	ConfigChangeKind kind = ConfigChangeKind::Changed;
	QString table;
	QString key;
	SettingScope scope = SettingScope::Unknown;
	ConfigChangeAddressKind addressKind = ConfigChangeAddressKind::Key;
	std::optional<QString> beforeValue;
	std::optional<QString> afterValue;
};

struct ConfigChangeEntry {
	ConfigChangeKind kind = ConfigChangeKind::Changed;
	QString table;
	QString label;
	SettingScope scope = SettingScope::Unknown;
	bool mixedScopes = false;
	std::vector<ConfigKeyChange> keys;
};

struct ConfigChangeSummary {
	std::vector<ConfigChangeEntry> entries;
	bool parsed = false;
};

[[nodiscard]] ConfigChangeSummary SummarizeConfigChange(
	const QByteArray &before,
	const QByteArray &after);

enum class ConfigImpactFileRole {
	Global,
	Layered,
};

struct ConfigImpactText {
	QByteArray text;
	bool present = true;
};

struct ConfigImpactAccount {
	QString account;
	ConfigImpactText file;
	AccountWorkMode workMode = AccountWorkMode::Own;
};

struct ConfigImpactInput {
	ConfigImpactText current;
	ConfigImpactText proposed;
	ConfigImpactFileRole role = ConfigImpactFileRole::Global;
	AccountWorkMode workMode = AccountWorkMode::Own;
	ConfigImpactText global = { {}, false };
	std::vector<ConfigImpactAccount> accounts;
};

enum class ConfigImpactStatus {
	Ready,
	InvalidCurrent,
	InvalidProposed,
	InvalidGlobal,
	InvalidAccount,
	NewerSchema,
	InvalidContext,
};

enum class ConfigImpactUse {
	Absent,
	Used,
	AppWideElsewhere,
	Follow,
	Stock,
	UndefinedList,
	Unknown,
};

enum class ConfigImpactKind {
	ValueChanged,
	Pinned,
	Unpinned,
	AppWideLoss,
	ListLoss,
	PresetLoss,
	ScheduleLoss,
	DefinedListEmptied,
	StrandedMembers,
	UnknownRemoval,
};

struct ConfigImpactFinding {
	ConfigImpactKind kind = ConfigImpactKind::ValueChanged;
	ConfigKeyChange change;
	QString label;
	QString account;
	ConfigImpactUse beforeUse = ConfigImpactUse::Absent;
	ConfigImpactUse afterUse = ConfigImpactUse::Absent;
	int beforeCount = -1;
	int afterCount = -1;
	bool dangerous = false;
};

struct ConfigChangeImpact {
	ConfigImpactStatus status = ConfigImpactStatus::InvalidContext;
	QString account;
	QByteArray proposedText;
	bool proposedPresent = true;
	ConfigChangeSummary summary;
	std::vector<ConfigImpactFinding> findings;
	bool dangerous = false;
};

[[nodiscard]] ConfigChangeImpact AnalyzeConfigChange(
	const ConfigImpactInput &input);

} // namespace Purple
