/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_account_files.h"

#include <algorithm>
#include <set>

namespace Purple {
namespace {

bool ValidAccount(const AccountKey &account) {
	return AccountKeyText(account.environment, account.userId).has_value();
}

AccountOwnerStartupPlan Refused(QString error) {
	auto result = AccountOwnerStartupPlan();
	result.error = std::move(error);
	return result;
}

AccountOwnerPermissions OwnedPermissions() {
	auto result = AccountOwnerPermissions();
	result.rootSettingsForAllAccounts = false;
	result.loadAccountFiles = true;
	result.settingsSync = true;
	result.launchOffers = true;
	result.messageImports = true;
	result.automaticSend = true;
	return result;
}

void AppendCopy2(AccountOwnerStartupPlan &plan, const AccountKey &owner) {
	plan.writes.push_back({ AccountOwnerWriteTarget::OwnerRecordCopy2,
		u"accounts/root.toml"_q, *SerializeOwnerRecord(owner) });
}

void AppendCopy1(AccountOwnerStartupPlan &plan, const AccountKey &owner) {
	plan.localChoice.rootFileAccount = owner;
	plan.writes.push_back({ AccountOwnerWriteTarget::LocalChoiceCopy1,
		u"local.toml"_q, *SerializeLocalChoice(plan.localChoice) });
}

}

std::optional<AccountFiles> ResolveAccountFiles(
		const AccountKey &account,
		const std::optional<AccountKey> &startupOwner,
		AccountFileLayout layout) {
	const auto key = AccountKeyText(account.environment, account.userId);
	if (!key || !startupOwner || !ValidAccount(*startupOwner)
		|| (layout != AccountFileLayout::Desktop
			&& layout != AccountFileLayout::Android)) {
		return std::nullopt;
	}
	const auto root = (account == *startupOwner);
	const auto prefix = root ? QString() : u"accounts/%1/"_q.arg(*key);
	auto result = AccountFiles();
	result.directory = root ? QString() : u"accounts/%1"_q.arg(*key);
	result.settings = prefix + u"settings.toml"_q;
	result.good = prefix + u"settings.toml.good"_q;
	result.backup = prefix + u"settings.toml.bak"_q;
	result.importBackup = prefix + u"settings.toml.import.bak"_q;
	result.syncDirectory = prefix + u"sync"_q;
	result.syncStore = prefix + u"sync/state.json"_q;
	result.history = prefix + ((layout == AccountFileLayout::Desktop)
		? u"sync/history"_q : u"sync-history"_q);
	result.fingerprintFile = root ? u"state.toml"_q : prefix + u"send-state.toml"_q;
	result.fingerprintStorage = root ? AccountFingerprintStorage::DeviceState
		: AccountFingerprintStorage::AccountSendState;
	return result;
}

AccountOwnerStartupPlan PlanAccountOwnerStartup(
		const AccountOwnerStartupInput &input) {
	if ((!input.localChoice.present && !input.localChoice.text.isEmpty())
		|| (!input.ownerRecord.present && !input.ownerRecord.text.isEmpty())) {
		return Refused(u"Absent owner input contains text"_q);
	}
	auto identities = std::set<QString>();
	auto accountsEvidence = input.accountsDirectoryPresent || input.ownerRecord.present;
	for (const auto &candidate : input.signedInAccounts) {
		const auto key = AccountKeyText(candidate.account.environment, candidate.account.userId);
		if (!key || !identities.insert(*key).second) {
			return Refused(u"Invalid or duplicate signed-in account identity"_q);
		}
		if (candidate.hasAccountDirectory && !input.accountsDirectoryPresent) {
			return Refused(u"Account directory contradicts supplied parent presence"_q);
		}
		accountsEvidence = accountsEvidence || candidate.hasAccountDirectory;
	}
	if (input.chosenAccount && !ValidAccount(*input.chosenAccount)) {
		return Refused(u"Invalid chosen account identity"_q);
	}
	const auto local = ParseLocalChoice(input.localChoice.text, u"local.toml"_q);
	const auto copy1 = ParseOwnerRecord(input.localChoice.text, u"local.toml"_q);
	const auto copy2 = ParseOwnerRecord(input.ownerRecord.text, u"accounts/root.toml"_q);
	const auto first = input.localChoice.present && local.ok() && copy1.ok();
	const auto second = input.ownerRecord.present && copy2.ok();
	auto result = AccountOwnerStartupPlan();
	if (local.ok()) {
		result.localChoice = local.choice;
	}
	for (const auto &candidate : input.signedInAccounts) {
		if (!candidate.hasAccountDirectory) {
			result.eligibleChoices.push_back(candidate.account);
		}
	}
	if (first || second) {
		const auto owner = second ? *copy2.account : *copy1.account;
		if (input.chosenAccount) {
			return Refused(u"A confirmed startup owner cannot be reassigned"_q);
		}
		result.status = AccountOwnerStartupStatus::Owned;
		result.startupOwner = owner;
		result.localChoice.rootFileAccount = owner;
		result.globalAccount = GlobalAccountFor(result.localChoice, owner);
		result.permissions = OwnedPermissions();
		if (!second) {
			AppendCopy2(result, owner);
		}
		if (!first || (second && *copy1.account != owner)) {
			result.restoredHandEditedOwnerNotice = first && second;
			AppendCopy1(result, owner);
		}
		return result;
	}
	const auto migrated = input.localChoice.present || accountsEvidence;
	result.status = migrated ? AccountOwnerStartupStatus::SingleFileError
		: input.signedInAccounts.empty() ? AccountOwnerStartupStatus::WaitingForFirstSignIn
		: AccountOwnerStartupStatus::NeedsChoice;
	if (migrated) {
		result.error = u"settings.toml has no owner on this device; choose an eligible account"_q;
	}
	auto selected = input.chosenAccount;
	if (selected) {
		const auto eligible = std::find(result.eligibleChoices.begin(),
			result.eligibleChoices.end(), *selected) != result.eligibleChoices.end();
		if (!eligible) {
			return Refused(u"Chosen account is not signed in or already owns an account directory"_q);
		}
	} else if (!migrated) {
		if (input.signedInAccounts.size() == 1) {
			selected = input.signedInAccounts.front().account;
		} else {
			auto matches = 0;
			for (const auto &candidate : input.signedInAccounts) {
				if (candidate.rootBindingMatches) {
					++matches;
					selected = candidate.account;
				}
			}
			if (matches != 1 || selected->environment != 0) {
				selected.reset();
			}
		}
	}
	if (!selected) {
		return result;
	}
	result.status = AccountOwnerStartupStatus::PendingOwnerWrites;
	result.pendingOwner = selected;
	result.error.clear();
	result.localChoice.rootFileAccount = selected;
	result.localChoice.globalAccount = selected;
	result.globalAccount = selected;
	AppendCopy2(result, *selected);
	AppendCopy1(result, *selected);
	return result;
}

}
