/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#include "purple/purple_account_files.h"
#include "purple/purple_settings.h"

#include <QString>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <vector>

using namespace Purple;

namespace {

auto Checks = 0;
auto Failures = 0;

void Check(bool good, const char *condition, int line) {
	++Checks;
	if (!good) {
		++Failures;
		std::printf("FAIL line %d: %s\n", line, condition);
	}
}

#define CHECK(...) Check(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __LINE__)

bool ContainsRoot(const std::vector<QString> &roots, const QString &name) {
	return std::find(roots.begin(), roots.end(), name) != roots.end();
}

const SettingsLayerKey *FindApplied(
		const LayerSettingsResult &layered,
		const std::vector<QString> &table,
		const QString &key) {
	for (const auto &entry : layered.applied) {
		if (entry.table == table && entry.key == key) {
			return &entry;
		}
	}
	return nullptr;
}

const SettingsInactiveKey *FindInactive(
		const LayerSettingsResult &layered,
		const std::vector<QString> &table,
		const QString &key) {
	for (const auto &entry : layered.inactive) {
		if (entry.source.table == table && entry.source.key == key) {
			return &entry;
		}
	}
	return nullptr;
}

void TestAccountFilesPlaylists() {
	const auto prodOwner = AccountKey{ 0, 42 };
	const auto testOwner = AccountKey{ 1, 100 };
	const auto prodOther = AccountKey{ 0, 77 };
	const auto testOther = AccountKey{ 1, 99 };

	for (const auto layout : {
			AccountFileLayout::Desktop,
			AccountFileLayout::Android }) {
		const auto rootFiles = ResolveAccountFiles(prodOwner, prodOwner, layout);
		CHECK(rootFiles.has_value());
		if (rootFiles) {
			CHECK(rootFiles->directory.isEmpty());
			CHECK(rootFiles->settings == u"settings.toml"_q);
			CHECK(rootFiles->syncDirectory == u"sync"_q);
			CHECK(rootFiles->syncStore == u"sync/state.json"_q);
			CHECK(rootFiles->playlists == u"playlists"_q);
			CHECK(rootFiles->fingerprintFile == u"state.toml"_q);
			CHECK(rootFiles->fingerprintStorage
				== AccountFingerprintStorage::DeviceState);
			CHECK(rootFiles->history == (layout == AccountFileLayout::Desktop
				? u"sync/history"_q : u"sync-history"_q));
		}

		const auto prodSecondary = ResolveAccountFiles(
			prodOther,
			prodOwner,
			layout);
		CHECK(prodSecondary.has_value());
		if (prodSecondary) {
			CHECK(prodSecondary->directory == u"accounts/0-77"_q);
			CHECK(prodSecondary->settings == u"accounts/0-77/settings.toml"_q);
			CHECK(prodSecondary->syncDirectory == u"accounts/0-77/sync"_q);
			CHECK(prodSecondary->syncStore == u"accounts/0-77/sync/state.json"_q);
			CHECK(prodSecondary->playlists == u"accounts/0-77/playlists"_q);
			CHECK(prodSecondary->fingerprintFile
				== u"accounts/0-77/send-state.toml"_q);
			CHECK(prodSecondary->fingerprintStorage
				== AccountFingerprintStorage::AccountSendState);
			CHECK(prodSecondary->history == (layout == AccountFileLayout::Desktop
				? u"accounts/0-77/sync/history"_q
				: u"accounts/0-77/sync-history"_q));
		}

		const auto testSecondary = ResolveAccountFiles(
			testOther,
			prodOwner,
			layout);
		CHECK(testSecondary.has_value());
		if (testSecondary) {
			CHECK(testSecondary->directory == u"accounts/1-99"_q);
			CHECK(testSecondary->syncDirectory == u"accounts/1-99/sync"_q);
			CHECK(testSecondary->playlists == u"accounts/1-99/playlists"_q);
		}

		const auto testRoot = ResolveAccountFiles(testOwner, testOwner, layout);
		CHECK(testRoot.has_value());
		if (testRoot) {
			CHECK(testRoot->directory.isEmpty());
			CHECK(testRoot->playlists == u"playlists"_q);
		}

		const auto prodUnderTest = ResolveAccountFiles(
			AccountKey{ 0, 5 },
			testOwner,
			layout);
		CHECK(prodUnderTest.has_value());
		if (prodUnderTest) {
			CHECK(prodUnderTest->playlists == u"accounts/0-5/playlists"_q);
		}

		const auto testUnderTest = ResolveAccountFiles(
			AccountKey{ 1, 200 },
			testOwner,
			layout);
		CHECK(testUnderTest.has_value());
		if (testUnderTest) {
			CHECK(testUnderTest->playlists == u"accounts/1-200/playlists"_q);
		}

		CHECK(!ResolveAccountFiles(prodOwner, std::nullopt, layout));
		CHECK(!ResolveAccountFiles(AccountKey{ 0, 0 }, prodOwner, layout));
		CHECK(!ResolveAccountFiles(AccountKey{ 0, -7 }, prodOwner, layout));
		CHECK(!ResolveAccountFiles(AccountKey{ -1, 42 }, prodOwner, layout));
		CHECK(!ResolveAccountFiles(AccountKey{ 9, 42 }, prodOwner, layout));
		CHECK(!ResolveAccountFiles(prodOwner, AccountKey{ 0, 0 }, layout));
		CHECK(!ResolveAccountFiles(prodOwner, AccountKey{ 9, 42 }, layout));
	}

	CHECK(!ResolveAccountFiles(
		prodOwner,
		prodOwner,
		static_cast<AccountFileLayout>(9)));
}

void TestSettingsPlaybackAndPlaylistsParsing() {
	const auto emptyParsed = ParseSettings(QString(), u"empty.toml"_q);
	{
		CHECK(emptyParsed.ok());
		CHECK(emptyParsed.warnings.empty());
		CHECK(emptyParsed.settings.playback == Playback());
		CHECK(emptyParsed.settings.playlists == Playlists());
		CHECK(emptyParsed.settings.playback.privateListening == true);
		CHECK(emptyParsed.settings.playback.privateChatQueue == true);
		CHECK(emptyParsed.settings.playback.afterVoice == VoiceContinuation::Unheard);
		CHECK(emptyParsed.settings.playback.resumeMusicAfterVoice == true);
		CHECK(emptyParsed.settings.playback.seekStepSeconds == 10);
		CHECK(emptyParsed.settings.playlists.autoSync == true);
		CHECK(emptyParsed.settings.playlists.menuItem == true);
		CHECK(emptyParsed.settings.playlists.keepDownloaded == true);
		CHECK(ContainsRoot(emptyParsed.readRoots, u"playback"_q));
		CHECK(ContainsRoot(emptyParsed.readRoots, u"playlists"_q));
		CHECK(!ContainsRoot(emptyParsed.readRoots, u"music"_q));
	}

	{
		const auto parsed = ParseSettings(
			u"[playback]\n[playlists]\n"_q,
			u"empty-tables.toml"_q);
		CHECK(parsed.ok());
		CHECK(parsed.warnings.empty());
		CHECK(parsed.settings == emptyParsed.settings);
		CHECK(parsed.settings.playback.privateListening == true);
		CHECK(parsed.settings.playlists == Playlists{ true, true, true });
	}

	{
		const auto parsed = ParseSettings(
			u"[playback]\n"
			"private_listening_p = false\n"
			"[playlists]\n"
			"auto_sync_p = false\n"
			"menu_item_p = false\n"
			"keep_downloaded_p = false\n"_q,
			u"all-false.toml"_q);
		CHECK(parsed.ok());
		CHECK(parsed.warnings.empty());
		CHECK(parsed.settings.playback.privateListening == false);
		CHECK(parsed.settings.playback.privateChatQueue == true);
		CHECK(parsed.settings.playback.afterVoice == VoiceContinuation::Unheard);
		CHECK(parsed.settings.playback.resumeMusicAfterVoice == true);
		CHECK(parsed.settings.playback.seekStepSeconds == 10);
		CHECK(parsed.settings.playlists.autoSync == false);
		CHECK(parsed.settings.playlists.menuItem == false);
		CHECK(parsed.settings.playlists.keepDownloaded == false);
		CHECK(parsed.settings != emptyParsed.settings);
	}

	{
		auto a = Settings();
		auto b = Settings();
		CHECK(a == b);
		b.playback.privateListening = false;
		CHECK(a != b);
		b = Settings();
		b.playlists.autoSync = false;
		CHECK(a != b);
		b = Settings();
		b.playlists.menuItem = false;
		CHECK(a != b);
		b = Settings();
		b.playlists.keepDownloaded = false;
		CHECK(a != b);
	}

	{
		const auto parsed = ParseSettings(
			u"[playback]\n"
			"private_chat_queue_p = false\n"
			"after_voice = 'resume_music'\n"
			"resume_music_after_voice_p = false\n"
			"seek_step = '30s'\n"
			"private_listening_p = false\n"
			"[playlists]\n"
			"auto_sync_p = true\n"
			"menu_item_p = false\n"
			"keep_downloaded_p = true\n"_q,
			u"mixed.toml"_q);
		CHECK(parsed.ok());
		CHECK(parsed.warnings.empty());
		CHECK(parsed.settings.playback.privateChatQueue == false);
		CHECK(parsed.settings.playback.afterVoice
			== VoiceContinuation::ResumeMusic);
		CHECK(parsed.settings.playback.resumeMusicAfterVoice == false);
		CHECK(parsed.settings.playback.seekStepSeconds == 30);
		CHECK(parsed.settings.playback.privateListening == false);
		CHECK(parsed.settings.playlists.autoSync == true);
		CHECK(parsed.settings.playlists.menuItem == false);
		CHECK(parsed.settings.playlists.keepDownloaded == true);
	}

	{
		const auto parsed = ParseSettings(
			u"playback = 'bad'\nplaylists = 7\n"_q,
			u"non-table.toml"_q);
		CHECK(parsed.ok());
		CHECK(parsed.settings.playback.privateListening == true);
		CHECK(parsed.settings.playlists == Playlists{ true, true, true });
		CHECK(parsed.warnings == (std::vector<QString>{
			u"'playback' should be a table (line 1)."_q,
			u"'playlists' should be a table (line 2)."_q,
		}));
	}

	{
		const auto parsed = ParseSettings(
			u"playlists = [true, false]\n"_q,
			u"array-root.toml"_q);
		CHECK(parsed.ok());
		CHECK(parsed.settings.playlists == Playlists{ true, true, true });
		CHECK(parsed.warnings == (std::vector<QString>{
			u"'playlists' should be a table (line 1)."_q,
		}));
	}

	{
		const auto parsed = ParseSettings(
			u"[playback]\n"
			"seek_step = '15s'\n"
			"private_listening_p = 0\n"
			"[playlists]\n"
			"auto_sync_p = 'false'\n"
			"menu_item_p = 1\n"
			"keep_downloaded_p = { bad = true }\n"_q,
			u"bad-values.toml"_q);
		CHECK(parsed.ok());
		CHECK(parsed.settings.playback.seekStepSeconds == 15);
		CHECK(parsed.settings.playback.privateListening == true);
		CHECK(parsed.settings.playlists == Playlists{ true, true, true });
		CHECK(parsed.warnings == (std::vector<QString>{
			u"playback: 'private_listening_p' should be true or false (line 3), ignoring it."_q,
			u"playlists: 'auto_sync_p' should be true or false (line 5), ignoring it."_q,
			u"playlists: 'menu_item_p' should be true or false (line 6), ignoring it."_q,
			u"playlists: 'keep_downloaded_p' should be true or false (line 7), ignoring it."_q,
		}));
	}

	{
		const auto parsed = ParseSettings(
			u"[playlists]\n"
			"auto_sync_p = false\n"
			"menu_item_p = 'yes'\n"
			"keep_downloaded_p = false\n"_q,
			u"partial-bad.toml"_q);
		CHECK(parsed.ok());
		CHECK(parsed.settings.playlists.autoSync == false);
		CHECK(parsed.settings.playlists.menuItem == true);
		CHECK(parsed.settings.playlists.keepDownloaded == false);
		CHECK(parsed.warnings == (std::vector<QString>{
			u"playlists: 'menu_item_p' should be true or false (line 3), ignoring it."_q,
		}));
	}

	{
		// Sync switch lives in local.json, never settings.toml; unknown keys
		// and unregistered subtables under [playlists] produce no warnings.
		const auto parsed = ParseSettings(
			u"[playlists]\n"
			"sync = false\n"
			"sync_p = false\n"
			"future_flag = 'ignored'\n"
			"[playlists.nested]\n"
			"auto_sync_p = false\n"_q,
			u"unknown-keys.toml"_q);
		CHECK(parsed.ok());
		CHECK(parsed.warnings.empty());
		CHECK(parsed.settings.playlists == Playlists{ true, true, true });
	}
}

void TestPlaylistAndPlaybackScopeRegistry() {
	const auto emptyParsed = ParseSettings(QString(), u"scope.toml"_q);
	CHECK(emptyParsed.ok());
	const auto &registry = SettingsScopeRegistry();
	for (const auto &root : emptyParsed.readRoots) {
		CHECK(std::any_of(registry.begin(), registry.end(), [&](const auto &entry) {
			return entry.name == root && !entry.future;
		}));
		CHECK(SettingsScope(QString(), root) != SettingScope::Unknown);
	}

	const auto playlistsEntry = std::find_if(
		registry.begin(),
		registry.end(),
		[](const auto &entry) { return entry.name == u"playlists"_q; });
	CHECK(playlistsEntry != registry.end());
	if (playlistsEntry != registry.end()) {
		CHECK(!playlistsEntry->future);
		CHECK(playlistsEntry->scope == SettingScope::PerAccount);
		CHECK(!playlistsEntry->rootKey);
	}

	const auto musicEntry = std::find_if(
		registry.begin(),
		registry.end(),
		[](const auto &entry) { return entry.name == u"music"_q; });
	CHECK(musicEntry != registry.end());
	if (musicEntry != registry.end()) {
		CHECK(musicEntry->future);
	}

	// Baseline negative control: marking `playlists` future = true breaks the
	// registry completeness check against actual readRoots.
	auto staleRegistry = registry;
	for (auto &entry : staleRegistry) {
		if (entry.name == u"playlists"_q) {
			entry.future = true;
		}
	}
	auto unreadyRoots = std::vector<QString>();
	for (const auto &root : emptyParsed.readRoots) {
		const auto active = std::any_of(
			staleRegistry.begin(),
			staleRegistry.end(),
			[&](const auto &entry) {
				return entry.name == root && !entry.future;
			});
		if (!active) {
			unreadyRoots.push_back(root);
		}
	}
	CHECK(unreadyRoots == std::vector<QString>{ u"playlists"_q });

	CHECK(SettingsScope(u"playlists"_q, u"auto_sync_p"_q)
		== SettingScope::PerAccount);
	CHECK(!SettingsScopeOverride({ u"playlists"_q }, u"auto_sync_p"_q));
	CHECK(SettingsScope(u"playlists"_q, u"future_unknown"_q)
		== SettingScope::PerAccount);
	CHECK(SettingsScope(u"playlists"_q, u"menu_item_p"_q)
		== SettingScope::AppWide);
	CHECK(SettingsScopeOverride({ u"playlists"_q }, u"menu_item_p"_q)
		== SettingScope::AppWide);
	CHECK(SettingsScope(u"playlists"_q, u"keep_downloaded_p"_q)
		== SettingScope::AppWide);
	CHECK(SettingsScopeOverride({ u"playlists"_q }, u"keep_downloaded_p"_q)
		== SettingScope::AppWide);
	CHECK(SettingsScope(u"playlists.nested"_q, u"auto_sync_p"_q)
		== SettingScope::Unknown);
	CHECK(SettingsScope(u"playlists.nested"_q, u"menu_item_p"_q)
		== SettingScope::Unknown);
	CHECK(SettingsScope(u"\"playlists.nested\""_q, u"auto_sync_p"_q)
		== SettingScope::Unknown);
	CHECK(SettingsScope(u"playback"_q, u"private_listening_p"_q)
		== SettingScope::AppWide);
	CHECK(SettingsScope(u"playback.nested"_q, u"private_listening_p"_q)
		== SettingScope::AppWide);
	CHECK(!SettingsScopeOverride(
		{ u"playback"_q },
		u"private_listening_p"_q));
}

void TestPlaylistAndPlaybackLayering() {
	{
		const auto globalText = u""
			"[playback]\n"
			"seek_step = '15s'\n"
			"private_listening_p = false\n"
			"[playlists]\n"
			"auto_sync_p = false\n"
			"menu_item_p = false\n"
			"keep_downloaded_p = false\n"_q;
		const auto accountText = u""
			"[playback]\n"
			"private_listening_p = true\n"
			"[playlists]\n"
			"auto_sync_p = true\n"
			"menu_item_p = true\n"
			"keep_downloaded_p = true\n"_q;
		const auto layered = LayerSettings(
			globalText,
			u"global.toml"_q,
			accountText,
			u"account.toml"_q);
		CHECK(layered.effective.ok());
		CHECK(layered.effective.warnings.empty());
		CHECK(ContainsRoot(layered.effective.readRoots, u"playback"_q));
		CHECK(ContainsRoot(layered.effective.readRoots, u"playlists"_q));
		CHECK(layered.effective.settings.playback.seekStepSeconds == 15);
		CHECK(layered.effective.settings.playback.privateListening == false);
		CHECK(layered.effective.settings.playlists.autoSync == true);
		CHECK(layered.effective.settings.playlists.menuItem == false);
		CHECK(layered.effective.settings.playlists.keepDownloaded == false);

		CHECK(layered.applied.size() == 1);
		const auto *appliedAutoSync = FindApplied(
			layered,
			{ u"playlists"_q },
			u"auto_sync_p"_q);
		CHECK(appliedAutoSync != nullptr);
		if (appliedAutoSync) {
			CHECK(appliedAutoSync->path == u"account.toml"_q);
			CHECK(appliedAutoSync->line == 4);
		}

		CHECK(layered.inactive.size() == 3);
		const auto *inactivePrivate = FindInactive(
			layered,
			{ u"playback"_q },
			u"private_listening_p"_q);
		CHECK(inactivePrivate != nullptr);
		if (inactivePrivate) {
			CHECK(inactivePrivate->reason == SettingsInactiveReason::AppWide);
			CHECK(inactivePrivate->source.path == u"account.toml"_q);
			CHECK(inactivePrivate->source.line == 2);
		}
		const auto *inactiveMenu = FindInactive(
			layered,
			{ u"playlists"_q },
			u"menu_item_p"_q);
		CHECK(inactiveMenu != nullptr);
		if (inactiveMenu) {
			CHECK(inactiveMenu->reason == SettingsInactiveReason::AppWide);
			CHECK(inactiveMenu->source.path == u"account.toml"_q);
			CHECK(inactiveMenu->source.line == 5);
		}
		const auto *inactiveKeep = FindInactive(
			layered,
			{ u"playlists"_q },
			u"keep_downloaded_p"_q);
		CHECK(inactiveKeep != nullptr);
		if (inactiveKeep) {
			CHECK(inactiveKeep->reason == SettingsInactiveReason::AppWide);
			CHECK(inactiveKeep->source.path == u"account.toml"_q);
			CHECK(inactiveKeep->source.line == 6);
		}
	}

	{
		const auto accountText = u""
			"[playback]\n"
			"private_listening_p = false\n"
			"[playlists]\n"
			"auto_sync_p = false\n"
			"menu_item_p = false\n"
			"keep_downloaded_p = false\n"_q;
		const auto layered = LayerSettings(
			QString(),
			u"global.toml"_q,
			accountText,
			u"account.toml"_q);
		CHECK(layered.effective.ok());
		CHECK(layered.effective.settings.playback.privateListening == true);
		CHECK(layered.effective.settings.playlists.autoSync == false);
		CHECK(layered.effective.settings.playlists.menuItem == true);
		CHECK(layered.effective.settings.playlists.keepDownloaded == true);
	}

	{
		auto options = SettingsLayerOptions();
		options.accountPresent = false;
		const auto globalText = u""
			"[playback]\n"
			"private_listening_p = false\n"
			"[playlists]\n"
			"auto_sync_p = false\n"
			"menu_item_p = false\n"
			"keep_downloaded_p = false\n"_q;
		const auto layered = LayerSettings(
			globalText,
			u"global.toml"_q,
			QString(),
			u"account.toml"_q,
			options);
		CHECK(layered.effective.ok());
		CHECK(layered.effective.settings.playback.privateListening == false);
		CHECK(layered.effective.settings.playlists
			== Playlists{ false, false, false });
		CHECK(layered.applied.empty());
		CHECK(layered.inactive.empty());
	}

	{
		const auto globalText = u""
			"[playback]\n"
			"private_listening_p = false\n"
			"[playlists]\n"
			"auto_sync_p = false\n"
			"menu_item_p = false\n"
			"keep_downloaded_p = true\n"_q;
		const auto accountAText = u"[playlists]\nauto_sync_p = false\n"_q;
		const auto accountBText = u"[playlists]\nauto_sync_p = true\n"_q;
		auto options = SettingsLayerOptions();
		options.followListMembers = true;

		const auto layeredA = LayerSettings(
			globalText,
			u"global.toml"_q,
			accountAText,
			u"account-a.toml"_q,
			options);
		const auto layeredB = LayerSettings(
			globalText,
			u"global.toml"_q,
			accountBText,
			u"account-b.toml"_q,
			options);
		CHECK(layeredA.effective.settings.playlists.autoSync == false);
		CHECK(layeredB.effective.settings.playlists.autoSync == true);
		CHECK(layeredA.effective.settings.playlists.menuItem == false);
		CHECK(layeredB.effective.settings.playlists.menuItem == false);
		CHECK(layeredA.effective.settings.playlists.keepDownloaded == true);
		CHECK(layeredB.effective.settings.playlists.keepDownloaded == true);
		CHECK(layeredA.effective.settings.playback.privateListening == false);
		CHECK(layeredB.effective.settings.playback.privateListening == false);
	}

	{
		const auto globalText = u""
			"[playback]\n"
			"private_listening_p = false\n"
			"[playlists]\n"
			"auto_sync_p = false\n"
			"menu_item_p = false\n"
			"keep_downloaded_p = false\n"_q;
		const auto badAccountTable = u"playlists = 'oops'\n"_q;
		const auto layered = LayerSettings(
			globalText,
			u"global.toml"_q,
			badAccountTable,
			u"bad-account.toml"_q);
		CHECK(layered.effective.ok());
		CHECK(layered.effective.warnings.empty());
		CHECK(layered.effective.settings.playlists
			== Playlists{ false, false, false });
		CHECK(layered.account.current.parsed.warnings == (std::vector<QString>{
			u"'playlists' should be a table (line 1)."_q,
		}));
	}

	{
		const auto globalText = u""
			"[playback]\n"
			"private_listening_p = false\n"
			"[playlists]\n"
			"auto_sync_p = false\n"
			"menu_item_p = false\n"_q;
		const auto badLeafAccount = u""
			"[playback]\n"
			"private_listening_p = 1\n"
			"[playlists]\n"
			"auto_sync_p = 1\n"_q;
		const auto layered = LayerSettings(
			globalText,
			u"global.toml"_q,
			badLeafAccount,
			u"bad-leaf.toml"_q);
		CHECK(layered.effective.ok());
		CHECK(layered.effective.warnings.empty());
		CHECK(layered.effective.settings.playback.privateListening == false);
		CHECK(layered.effective.settings.playlists.autoSync == true);
		CHECK(layered.effective.settings.playlists.menuItem == false);
		CHECK(layered.effective.settings.playlists.keepDownloaded == true);
		CHECK(FindInactive(
			layered,
			{ u"playback"_q },
			u"private_listening_p"_q) != nullptr);
		CHECK(layered.account.current.parsed.warnings == (std::vector<QString>{
			u"playback: 'private_listening_p' should be true or false (line 2), ignoring it."_q,
			u"playlists: 'auto_sync_p' should be true or false (line 4), ignoring it."_q,
		}));
	}

	{
		const auto globalText = u""
			"[playlists]\n"
			"auto_sync_p = false\n"
			"menu_item_p = false\n"_q;
		auto options = SettingsLayerOptions();
		options.accountGood = SettingsLayerText{
			u"[playlists]\nauto_sync_p = true\n"_q,
			u"account.toml.good"_q,
		};
		const auto layered = LayerSettings(
			globalText,
			u"global.toml"_q,
			u"[playlists\nauto_sync_p = false\n"_q,
			u"account.toml"_q,
			options);
		CHECK(layered.effective.ok());
		CHECK(!layered.account.current.parsed.ok());
		CHECK(layered.account.usedGood);
		CHECK(layered.effective.settings.playlists.autoSync == true);
		CHECK(layered.effective.settings.playlists.menuItem == false);
		CHECK(layered.effective.settings.playlists.keepDownloaded == true);
	}
}

} // namespace

int main() {
	TestAccountFilesPlaylists();
	TestSettingsPlaybackAndPlaylistsParsing();
	TestPlaylistAndPlaybackScopeRegistry();
	TestPlaylistAndPlaybackLayering();
	std::printf("Checks: %d, Failures: %d\n", Checks, Failures);
	return Failures ? 1 : 0;
}
