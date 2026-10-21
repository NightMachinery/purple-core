/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_local_choice.h"

namespace Purple {

enum class AccountFileLayout { Desktop, Android };
enum class AccountFingerprintStorage { DeviceState, AccountSendState };

struct AccountFiles {
	QString directory;
	QString settings;
	QString good;
	QString backup;
	QString importBackup;
	QString syncDirectory;
	QString syncStore;
	QString history;
	QString fingerprintFile;
	AccountFingerprintStorage fingerprintStorage = AccountFingerprintStorage::DeviceState;
};

struct DeviceAccountFiles {
	QString state = u"state.toml"_q;
	QString localChoice = u"local.toml"_q;
	QString ownerRecord = u"accounts/root.toml"_q;
};

[[nodiscard]] std::optional<AccountFiles> ResolveAccountFiles(
	const AccountKey &account,
	const std::optional<AccountKey> &startupOwner,
	AccountFileLayout layout);

struct OwnerFileSnapshot {
	bool present = false;
	QString text;
};

struct StartupAccountCandidate {
	AccountKey account;
	bool hasAccountDirectory = false;
	bool rootBindingMatches = false;
};

struct AccountOwnerStartupInput {
	OwnerFileSnapshot localChoice;
	OwnerFileSnapshot ownerRecord;
	bool accountsDirectoryPresent = false;
	std::vector<StartupAccountCandidate> signedInAccounts;
	std::optional<AccountKey> chosenAccount;
};

enum class AccountOwnerStartupStatus {
	Owned,
	PendingOwnerWrites,
	WaitingForFirstSignIn,
	NeedsChoice,
	SingleFileError,
	InvalidInput,
};

enum class AccountOwnerWriteTarget { OwnerRecordCopy2, LocalChoiceCopy1 };

struct AccountOwnerWrite {
	AccountOwnerWriteTarget target = AccountOwnerWriteTarget::OwnerRecordCopy2;
	QString relativePath;
	QString text;
};

struct AccountOwnerPermissions {
	bool rootSettingsForAllAccounts = true;
	bool loadAccountFiles = false;
	bool settingsSync = false;
	bool launchOffers = false;
	bool messageImports = false;
	bool automaticSend = false;
	bool manualSend = true;
	bool manualShare = true;
};

struct AccountOwnerStartupPlan {
	AccountOwnerStartupStatus status = AccountOwnerStartupStatus::InvalidInput;
	std::optional<AccountKey> startupOwner;
	std::optional<AccountKey> pendingOwner;
	LocalChoice localChoice;
	std::optional<AccountKey> globalAccount;
	std::vector<AccountKey> eligibleChoices;
	std::vector<AccountOwnerWrite> writes;
	AccountOwnerPermissions permissions;
	bool restoredHandEditedOwnerNotice = false;
	QString error;
};

[[nodiscard]] AccountOwnerStartupPlan PlanAccountOwnerStartup(
	const AccountOwnerStartupInput &input);

}
