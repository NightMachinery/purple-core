/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_local_choice.h"

#include <limits>

#define TOML_EXCEPTIONS 0
#include <toml.hpp>

namespace Purple {
namespace {

[[nodiscard]] QString Text(std::string_view value) {
	return QString::fromUtf8(value.data(), int(value.size()));
}

[[nodiscard]] OwnerRecordParseResult ReadOwner(const toml::table &root) {
	auto result = OwnerRecordParseResult();
	const auto node = root.get("root_file_account");
	if (!node) {
		return result;
	}
	const auto text = node->value<std::string_view>();
	result.account = text ? ParseAccountKey(Text(*text)) : std::nullopt;
	result.status = result.account
		? OwnerRecordStatus::Valid
		: OwnerRecordStatus::Invalid;
	if (!result.account) {
		result.error = u"root_file_account must be a canonical account key."_q;
	}
	return result;
}

[[nodiscard]] std::optional<AccountWorkMode> ParseWorkMode(
		const toml::node &node) {
	const auto value = node.value<std::string_view>();
	if (!value) {
		return std::nullopt;
	} else if (*value == "own") {
		return AccountWorkMode::Own;
	} else if (*value == "follow") {
		return AccountWorkMode::Follow;
	} else if (*value == "stock") {
		return AccountWorkMode::Stock;
	}
	return std::nullopt;
}

[[nodiscard]] QString WorkModeText(AccountWorkMode value) {
	switch (value) {
	case AccountWorkMode::Own: return u"own"_q;
	case AccountWorkMode::Follow: return u"follow"_q;
	case AccountWorkMode::Stock: return u"stock"_q;
	}
	return QString();
}

[[nodiscard]] QString NameComment(
		const AccountKey &key,
		const AccountDisplayName &displayName) {
	if (!displayName) {
		return QString();
	}
	auto result = QString();
	for (const auto ch : displayName(key)) {
		const auto category = ch.category();
		result += (ch.unicode() < 0x20
			|| ch.unicode() == 0x7f
			|| category == QChar::Separator_Line
			|| category == QChar::Separator_Paragraph)
			? QChar(' ')
			: ch;
	}
	return result.trimmed().isEmpty()
		? QString()
		: u"   # %1"_q.arg(result.trimmed());
}

} // namespace

std::optional<QString> AccountKeyText(int environment, int64 userId) {
	if ((environment != 0 && environment != 1) || userId <= 0) {
		return std::nullopt;
	}
	return u"%1-%2"_q.arg(environment).arg(userId);
}

std::optional<AccountKey> ParseAccountKey(const QString &text) {
	if (text.size() < 3
		|| (text[0] != QChar('0') && text[0] != QChar('1'))
		|| text[1] != QChar('-')
		|| text[2] < QChar('1')
		|| text[2] > QChar('9')) {
		return std::nullopt;
	}
	auto userId = int64(0);
	for (auto i = qsizetype(2); i < text.size(); ++i) {
		const auto ch = text[i];
		if (ch < QChar('0') || ch > QChar('9')) {
			return std::nullopt;
		}
		const auto digit = int64(ch.unicode() - '0');
		if (userId > (std::numeric_limits<int64>::max() - digit) / 10) {
			return std::nullopt;
		}
		userId = userId * 10 + digit;
	}
	return AccountKey{ text[0] == QChar('1') ? 1 : 0, userId };
}

LocalChoiceParseResult ParseLocalChoice(
		const QString &text,
		const QString &path) {
	auto result = LocalChoiceParseResult();
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		result.error = u"Could not parse local choices as TOML at %1:%2."_q
			.arg(parsed.error().source().begin.line)
			.arg(parsed.error().source().begin.column);
		return result;
	}
	const auto &root = parsed.table();
	const auto owner = ReadOwner(root);
	result.choice.rootFileAccount = owner.account;
	if (owner.status == OwnerRecordStatus::Invalid) {
		result.warnings.push_back(owner.error);
	}
	if (const auto node = root.get("global_account")) {
		const auto value = node->value<std::string_view>();
		result.choice.globalAccount = value
			? ParseAccountKey(Text(*value))
			: std::nullopt;
		if (!result.choice.globalAccount) {
			result.warnings.push_back(u"global_account must be a canonical "
				"account key; use the startup root-file owner instead."_q);
		}
	}
	if (const auto node = root.get("account_work_mode")) {
		const auto table = node->as_table();
		if (!table) {
			result.warnings.push_back(
				u"account_work_mode must be a table; using own."_q);
		} else {
			for (const auto &[name, value] : *table) {
				const auto key = Text(name.str());
				const auto account = ParseAccountKey(key);
				const auto mode = ParseWorkMode(value);
				if (!account || !mode) {
					result.warnings.push_back(u"account_work_mode contains an "
						"invalid account key or choice; "
						"using own for that entry."_q);
					continue;
				}
				result.choice.accountWorkMode.emplace(key, *mode);
			}
		}
	}
	if (result.choice.globalAccount) {
		const auto key = AccountKeyText(
			result.choice.globalAccount->environment,
			result.choice.globalAccount->userId);
		const auto i = result.choice.accountWorkMode.find(*key);
		if (i != result.choice.accountWorkMode.end()
			&& i->second == AccountWorkMode::Follow) {
			result.warnings.push_back(
				u"The global account already uses its own lists; "
				"follow is ignored."_q);
		}
	}
	return result;
}

OwnerRecordParseResult ParseOwnerRecord(
		const QString &text,
		const QString &path) {
	const auto utf8 = text.toUtf8();
	auto parsed = toml::parse(
		std::string_view(utf8.constData(), utf8.size()),
		path.toStdString());
	if (!parsed) {
		return { OwnerRecordStatus::Invalid, std::nullopt,
			u"Could not parse the root-file owner record as TOML at %1:%2."_q
				.arg(parsed.error().source().begin.line)
				.arg(parsed.error().source().begin.column) };
	}
	return ReadOwner(parsed.table());
}

std::optional<AccountKey> GlobalAccountFor(
		const LocalChoice &choice,
		const AccountKey &startupOwner) {
	if (choice.globalAccount) {
		const auto &key = *choice.globalAccount;
		if (AccountKeyText(key.environment, key.userId)) {
			return key;
		}
	}
	return AccountKeyText(startupOwner.environment, startupOwner.userId)
		? std::optional<AccountKey>(startupOwner)
		: std::nullopt;
}

AccountWorkMode AccountWorkModeFor(
		const LocalChoice &choice,
		const AccountKey &account,
		const AccountKey &globalAccount) {
	const auto key = AccountKeyText(account.environment, account.userId);
	if (!key) {
		return AccountWorkMode::Own;
	}
	const auto i = choice.accountWorkMode.find(*key);
	if (i == choice.accountWorkMode.end()
		|| WorkModeText(i->second).isEmpty()
		|| (i->second == AccountWorkMode::Follow && account == globalAccount)) {
		return AccountWorkMode::Own;
	}
	return i->second;
}

std::optional<QString> SerializeLocalChoice(
		const LocalChoice &choice,
		const AccountDisplayName &displayName,
		const std::vector<AccountKey> &accountsWithFiles) {
	if (!choice.rootFileAccount) {
		return std::nullopt;
	}
	const auto &owner = *choice.rootFileAccount;
	const auto ownerText = AccountKeyText(owner.environment, owner.userId);
	const auto global = choice.globalAccount.value_or(owner);
	const auto globalText = AccountKeyText(global.environment, global.userId);
	if (!ownerText || !globalText) {
		return std::nullopt;
	}
	auto result = u"# Purple Telegram: this device only. Never synced or sent.\n\n"
		"# The account supplying app-wide settings on this device.\n"
		"# You may change this line; the app switches when you save.\n"
		"global_account = \"%1\"%2\n"
		"# That account's file on this device: %3\n\n"
		"# Managed by Purple Telegram, read only at startup.\n"
		"# A hand change to the owner is put back at the next start.\n"
		"root_file_account = \"%4\"%5\n"_q
		.arg(*globalText,
			NameComment(global, displayName),
			(global == owner)
				? u"settings.toml"_q
				: u"accounts/%1/settings.toml"_q.arg(*globalText),
			*ownerText,
			NameComment(owner, displayName));
	if (!accountsWithFiles.empty()) {
		result += u"\n# Accounts with their own file on this device:\n"_q;
		for (const auto &account : accountsWithFiles) {
			const auto key = AccountKeyText(
				account.environment,
				account.userId);
			if (!key) {
				return std::nullopt;
			} else if (account == owner) {
				continue;
			}
			result += u"#   accounts/%1/settings.toml%2\n"_q
				.arg(*key, NameComment(account, displayName));
		}
	}
	if (!choice.accountWorkMode.empty()) {
		result += u"\n# Missing accounts use own members, else the global file's.\n"
			"# follow uses global lists here; stock disables Work Mode here.\n"
			"[account_work_mode]\n"_q;
		for (const auto &[key, mode] : choice.accountWorkMode) {
			const auto account = ParseAccountKey(key);
			const auto value = WorkModeText(mode);
			if (!account || value.isEmpty()) {
				return std::nullopt;
			}
			result += u"\"%1\" = \"%2\"%3\n"_q
				.arg(key, value, NameComment(*account, displayName));
		}
	}
	return result;
}

std::optional<QString> SerializeOwnerRecord(
		const AccountKey &owner,
		const AccountDisplayName &displayName) {
	const auto key = AccountKeyText(owner.environment, owner.userId);
	if (!key) {
		return std::nullopt;
	}
	return u"# Managed by Purple Telegram. Root-file ownership, read at startup.\n"
		"# Do not edit; the second copy is in local.toml.\n"
		"root_file_account = \"%1\"%2\n"_q
		.arg(*key, NameComment(owner, displayName));
}

} // namespace Purple
