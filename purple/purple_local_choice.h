/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_types.h"

#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace Purple {

struct AccountKey {
	int environment = 0;
	int64 userId = 0;

	friend bool operator==(const AccountKey &, const AccountKey &) = default;
};

[[nodiscard]] std::optional<QString> AccountKeyText(
	int environment,
	int64 userId);
[[nodiscard]] std::optional<AccountKey> ParseAccountKey(const QString &text);

enum class AccountWorkMode {
	Own,
	Follow,
	Stock,
};

enum class OwnerRecordStatus {
	Missing,
	Valid,
	Invalid,
};

struct OwnerRecordParseResult {
	OwnerRecordStatus status = OwnerRecordStatus::Missing;
	std::optional<AccountKey> account;
	QString error;

	[[nodiscard]] bool ok() const {
		return status == OwnerRecordStatus::Valid;
	}
};

struct LocalChoice {
	std::optional<AccountKey> globalAccount;
	std::optional<AccountKey> rootFileAccount;
	std::map<QString, AccountWorkMode> accountWorkMode;
};

struct LocalChoiceParseResult {
	LocalChoice choice;
	std::vector<QString> warnings;
	QString error;

	[[nodiscard]] bool ok() const {
		return error.isEmpty();
	}
};

using AccountDisplayName = std::function<QString(const AccountKey &)>;

[[nodiscard]] LocalChoiceParseResult ParseLocalChoice(
	const QString &text,
	const QString &path);
[[nodiscard]] OwnerRecordParseResult ParseOwnerRecord(
	const QString &text,
	const QString &path);
[[nodiscard]] std::optional<AccountKey> GlobalAccountFor(
	const LocalChoice &choice,
	const AccountKey &startupOwner);
[[nodiscard]] AccountWorkMode AccountWorkModeFor(
	const LocalChoice &choice,
	const AccountKey &account,
	const AccountKey &globalAccount);
[[nodiscard]] std::optional<QString> SerializeLocalChoice(
	const LocalChoice &choice,
	const AccountDisplayName &displayName = {},
	const std::vector<AccountKey> &accountsWithFiles = {});
[[nodiscard]] std::optional<QString> SerializeOwnerRecord(
	const AccountKey &owner,
	const AccountDisplayName &displayName = {});

} // namespace Purple
