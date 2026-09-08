/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/

// Standalone tests for the Purple config core. tdesktop has no unit test
// framework in the app target - cmake/tests.cmake builds visual GUI test apps
// behind DESKTOP_APP_TEST_APPS - and the parser and splicer are deliberately
// free of tdesktop dependencies, so they compile straight into this harness.
// Run with purple/test_config.sh.

#include "purple/purple_engine.h"
#include "purple/purple_screentime.h"
#include "purple/purple_settings.h"
#include "purple/purple_splice.h"
#include "purple/purple_state.h"

#include <QtCore/QDateTime>
#include <QtCore/QStringList>
#include <QtCore/QTimeZone>

#include <cstdio>
#include <type_traits>

namespace {

auto Checks = 0;
auto Failures = 0;
const char *Section = "";

void Report(bool ok, const QString &what, int line) {
	++Checks;
	if (!ok) {
		++Failures;
		std::printf("  FAIL  %s:%d  %s\n", Section, line, qPrintable(what));
	}
}

#define CHECK(cond) Report((cond), u"" #cond ""_q, __LINE__)
#define CHECK_EQ(a, b) Report( \
	(a) == (b), \
	u"%1 == %2, got '%3' vs '%4'"_q.arg(u"" #a ""_q, u"" #b ""_q) \
		.arg(Describe(a), Describe(b)), \
	__LINE__)

[[nodiscard]] QString Describe(const QString &value) {
	return value;
}

template <typename T>
	requires std::is_integral_v<T>
[[nodiscard]] QString Describe(T value) {
	if constexpr (std::is_same_v<T, bool>) {
		return value ? u"true"_q : u"false"_q;
	} else {
		return QString::number(qlonglong(value));
	}
}

[[nodiscard]] QString Describe(const std::vector<Purple::PeerIdValue> &value) {
	auto parts = QStringList();
	for (const auto id : value) {
		parts.push_back(QString::number(id));
	}
	return u"["_q + parts.join(u", "_q) + u"]"_q;
}

void Begin(const char *name) {
	Section = name;
	if (qEnvironmentVariableIsSet("PURPLE_TEST_TRACE")) {
		std::printf("== %s\n", name);
		std::fflush(stdout);
	}
}

[[nodiscard]] QString Path() {
	return u"settings.toml"_q;
}

[[nodiscard]] Purple::ParseResult Parse(const QString &text) {
	return Purple::ParseSettings(text, Path());
}

[[nodiscard]] bool WarnsAbout(
		const Purple::ParseResult &result,
		const QString &fragment) {
	for (const auto &warning : result.warnings) {
		if (warning.contains(fragment)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] QStringList ListNames(const Purple::Settings &settings) {
	auto result = QStringList();
	for (const auto &list : settings.lists) {
		result.push_back(list.name);
	}
	return result;
}

// -1 for "said nothing", which is a distinct and load-bearing answer: it is
// what makes an entry take the default for whatever kind the chat turns out
// to be.
[[nodiscard]] int Mode(const std::optional<Purple::ShowMode> &value) {
	return value ? int(*value) : -1;
}

[[nodiscard]] int Mode(Purple::ShowMode value) {
	return int(value);
}

[[nodiscard]] int Mode(Purple::ShowMode value, bool) = delete;

[[nodiscard]] Purple::MemberTitle Titles() {
	return [](Purple::PeerIdValue id) {
		return (id == 111222333)
			? u"Maman"_q
			: (id == 123456789)
			? u"Ali Rezaei"_q
			: (id == 987654321)
			? u"Backend Team"_q
			: QString();
	};
}

// The file the docs describe, used as the baseline for most checks.
[[nodiscard]] QString Example() {
	return uR"(# my settings

[lists.os]
title  = "OS"
members = [
  1234567890,   # My Todo Channel
]

[lists.emergency]
title  = "Emergency"
members = [
  111222333,    # Maman
]

[lists.colleagues]
title = "Colleagues"
# who I actually work with
members = [
  123456789,    # Ali Rezaei
  987654321,    # Backend Team
]

[lists.people]
title = "Everyone else"
kinds = ["private"]

[lists.noise]
kinds = ["channels", "bots"]

[list_sets.core]
list_order = [
  { list = "os",        show_mode = "always", notify_p = true },
  { list = "emergency", show_mode = "always", notify_p = true },
]

[folder_sets.work_folders]
folders = [
  { name = "Music", notify_p = false },
]

[presets.work]
list_order = [
  "*core",
  { list = "colleagues", show_mode = "mention",  notify_p = true },
  { list = "people",     show_mode = "always",  notify_p = false },
  { list = "noise",      show_mode = "never", notify_p = false },
]
folders = [ "*work_folders" ]

[presets.strict]
list_order = [ "*core" ]
folders = []
)"_q;
}

void TestLists() {
	Begin("lists");
	const auto result = Parse(Example());
	CHECK(result.ok());
	CHECK(result.warnings.empty());

	// Definitions only, in file order. There is no priority here any more -
	// that belongs to whichever preset names them.
	CHECK_EQ(ListNames(result.settings).join(u","_q),
		u"os,emergency,colleagues,people,noise"_q);

	const auto noise = result.settings.list(u"noise"_q);
	CHECK(noise != nullptr);
	CHECK_EQ(noise->title, u"noise"_q);
	CHECK(noise->members.empty());
	CHECK_EQ(int(noise->kinds.size()), 2);
	CHECK(noise->kinds[0] == Purple::ChatKind::Channel);
	CHECK(noise->kinds[1] == Purple::ChatKind::Bot);

	// A list carrying what a preset used to override is a file written against
	// the old model; saying so beats behaving differently in silence.
	const auto stale = Parse(uR"(
[lists.a]
show   = true
notify = false
locked = true
)"_q);
	CHECK(stale.ok());
	CHECK(WarnsAbout(stale, u"'show' is no longer a setting"_q));
	CHECK(WarnsAbout(stale, u"'notify' is no longer a setting"_q));
	CHECK(WarnsAbout(stale, u"'locked' is no longer a setting"_q));
	CHECK(WarnsAbout(stale, u"only reach a list it names"_q));
}

void TestKinds() {
	Begin("kinds");

	const auto result = Parse(uR"(
[lists.a]
kinds = ["private", "bots", "bots", "wombats"]

[lists.b]
kinds = "bots"
)"_q);
	CHECK(result.ok());
	CHECK(WarnsAbout(result, u"'wombats' is not one of"_q));
	CHECK(WarnsAbout(result, u"'kinds' should be an array"_q));

	// Deduplicated, order kept, the bad one dropped rather than the whole key.
	const auto a = result.settings.list(u"a"_q);
	CHECK_EQ(int(a->kinds.size()), 2);
	CHECK(a->kinds[0] == Purple::ChatKind::Private);
	CHECK(a->kinds[1] == Purple::ChatKind::Bot);
	CHECK(result.settings.list(u"b"_q)->kinds.empty());

	CHECK(Purple::ParseChatKind(u"Channels"_q) == Purple::ChatKind::Channel);
	CHECK(!Purple::ParseChatKind(u"channel"_q).has_value());
	CHECK_EQ(Purple::ChatKindName(Purple::ChatKind::Group), u"groups"_q);
}

void TestPresets() {
	Begin("presets");
	const auto result = Parse(Example());
	CHECK(result.ok());
	CHECK_EQ(result.settings.presets.size(), size_t(2));

	// File order, not alphabetical: the popover lists presets as written.
	CHECK_EQ(result.settings.presets[0].name, u"work"_q);
	CHECK_EQ(result.settings.presets[1].name, u"strict"_q);

	// "*core" spliced in at the front, then the three written inline.
	const auto work = result.settings.preset(u"work"_q);
	CHECK(work != nullptr);
	CHECK_EQ(int(work->listOrder.size()), 5);
	CHECK_EQ(work->listOrder[0].list, u"os"_q);
	CHECK_EQ(work->listOrder[1].list, u"emergency"_q);
	CHECK_EQ(work->listOrder[2].list, u"colleagues"_q);
	CHECK_EQ(Mode(work->listOrder[2].show), Mode(Purple::ShowMode::Mention));
	CHECK_EQ(work->listOrder[3].list, u"people"_q);
	CHECK(!work->listOrder[3].notify.value_or(true));
	CHECK_EQ(work->listOrder[4].list, u"noise"_q);
	CHECK_EQ(Mode(work->listOrder[4].show), Mode(Purple::ShowMode::Never));

	// Absent flags stay absent, so a warning can tell "false" from "unsaid".
	CHECK_EQ(Mode(work->listOrder[0].show), Mode(Purple::ShowMode::Always));

	CHECK_EQ(int(work->folders.size()), 1);
	CHECK_EQ(work->folders.front().name, u"Music"_q);
	CHECK(!work->folders.front().notify.value_or(true));
	CHECK(!work->folders.front().show.has_value());

	// strict shares the same set and asks for no folder tabs at all.
	const auto strict = result.settings.preset(u"strict"_q);
	CHECK(strict != nullptr);
	CHECK_EQ(int(strict->listOrder.size()), 2);
	CHECK_EQ(strict->listOrder[0].list, u"os"_q);
	CHECK(strict->folders.empty());
	CHECK(strict->views.empty());
}

void TestPresetPolicies() {
	Begin("preset policies");

	const auto reserved = Parse(uR"(
[presets.Normal]
list_order = [ { list = "a" } ]

[presets.keep]
list_order = [ { list = "a" } ]

[lists.a]
kinds = ["bots"]
)"_q);
	CHECK(reserved.ok());
	CHECK(WarnsAbout(reserved, u"reserved"_q));
	CHECK_EQ(reserved.settings.presets.size(), size_t(1));
	CHECK_EQ(reserved.settings.presets[0].name, u"keep"_q);

	// "default" was the implicit root of the old inheritance chain. There is no
	// chain now, so it is an ordinary name.
	const auto plain = Parse(uR"(
[presets.default]
list_order = [ { list = "a" } ]

[lists.a]
kinds = ["bots"]
)"_q);
	CHECK(plain.ok());
	CHECK_EQ(plain.settings.presets.size(), size_t(1));

	// Naming a list that is not defined claims nothing, which is worth saying
	// out loud - it looks exactly like a preset that is working.
	const auto ghost = Parse(uR"(
[presets.work]
list_order = [ { list = "ghosts" } ]
)"_q);
	CHECK(ghost.ok());
	CHECK(WarnsAbout(ghost, u"has no [lists.ghosts] table"_q));

	// An empty preset is legal and hides everything; silence about that would
	// be the single most confusing thing this parser could do.
	const auto empty = Parse(u"[presets.work]\n"_q);
	CHECK(empty.ok());
	CHECK(WarnsAbout(empty, u"hides and silences everything"_q));

	// Everything the old model spelled differently.
	const auto stale = Parse(uR"(
[presets.work]
inherit = "default"
groups_require_mention = true
hide_everywhere = true

[presets.work.overrides.a]
show = false
)"_q);
	CHECK(stale.ok());
	CHECK(WarnsAbout(stale, u"'inherit' is no longer a setting"_q));
	CHECK(WarnsAbout(stale, u"'overrides' is no longer a setting"_q));
	CHECK(WarnsAbout(stale, u"'groups_require_mention' is no longer"_q));
	CHECK(WarnsAbout(stale, u"spelled 'hide_everywhere_p' now"_q));
	CHECK(!stale.settings.preset(u"work"_q)->hideEverywhere.has_value());
}

void TestSpread() {
	Begin("spread");

	// Nested sets, a duplicate the second mention of which never decides
	// anything, and a name that is not a set at all.
	const auto result = Parse(uR"(
[lists.a]
kinds = ["bots"]
[lists.b]
kinds = ["channels"]
[lists.c]
kinds = ["groups"]

[list_sets.inner]
list_order = [ { list = "a", show_mode = "never" } ]

[list_sets.outer]
list_order = [ "*inner", { list = "b" } ]

[presets.work]
list_order = [ "*outer", { list = "a", show_mode = "always" }, "*ghosts", { list = "c" } ]
)"_q);
	CHECK(result.ok());
	CHECK(WarnsAbout(result, u"there is no list_set called 'ghosts'"_q));
	CHECK(WarnsAbout(result, u"already claimed further up"_q));

	const auto work = result.settings.preset(u"work"_q);
	CHECK_EQ(int(work->listOrder.size()), 3);
	CHECK_EQ(work->listOrder[0].list, u"a"_q);
	CHECK_EQ(work->listOrder[1].list, u"b"_q);
	CHECK_EQ(work->listOrder[2].list, u"c"_q);

	// First mention wins, so the "a" inside the set keeps its show_mode = "never"
	// and the later one is ignored rather than overriding it.
	CHECK_EQ(Mode(work->listOrder[0].show), Mode(Purple::ShowMode::Never));

	// The other way round is the idiom the spread exists for - override one
	// entry, then splice in the defaults - so it is silent.
	const auto overriding = Parse(uR"(
[lists.a]
kinds = ["bots"]
[lists.b]
kinds = ["channels"]

[list_sets.defaults]
list_order = [ { list = "a", show_mode = "always" }, { list = "b", show_mode = "always" } ]

[presets.work]
list_order = [ { list = "a", show_mode = "never" }, "*defaults" ]
)"_q);
	CHECK(overriding.ok());
	CHECK(!WarnsAbout(overriding, u"already claimed"_q));
	const auto tuned = overriding.settings.preset(u"work"_q);
	CHECK_EQ(int(tuned->listOrder.size()), 2);
	CHECK_EQ(tuned->listOrder[0].list, u"a"_q);
	CHECK_EQ(Mode(tuned->listOrder[0].show), Mode(Purple::ShowMode::Never));
	CHECK_EQ(tuned->listOrder[1].list, u"b"_q);

	// A set referring to itself is a typo, not a feature.
	const auto loop = Parse(uR"(
[lists.a]
kinds = ["bots"]

[list_sets.one]
list_order = [ "*two" ]

[list_sets.two]
list_order = [ "*one", { list = "a" } ]

[presets.work]
list_order = [ "*one" ]
)"_q);
	CHECK(loop.ok());
	CHECK(WarnsAbout(loop, u"refers back into itself"_q));
	CHECK_EQ(int(loop.settings.preset(u"work"_q)->listOrder.size()), 1);

	// A bare string that is not a reference is a mistake worth naming.
	const auto bare = Parse(uR"(
[presets.work]
list_order = [ "colleagues" ]
)"_q);
	CHECK(bare.ok());
	CHECK(WarnsAbout(bare, u"neither a table nor a \"*set\" reference"_q));
}

void TestFolderSelection() {
	Begin("folders");

	const auto result = Parse(uR"(
[folder_sets.mine]
folders = [ { name = "B", include_in_main_view = "all" } ]

[presets.work]
folders = [ "*mine", "*ALL", { name = "B", show_mode = "never" } ]

[presets.none]
list_order = []

[presets.every]
folders = [ "*ALL" ]
)"_q);
	CHECK(result.ok());
	CHECK(WarnsAbout(result, u"named more than once"_q));

	// The set first, then the marker; the third entry is B again and is
	// dropped, so B keeps the flags its first mention gave it.
	const auto work = result.settings.preset(u"work"_q);
	CHECK_EQ(int(work->folders.size()), 2);
	CHECK_EQ(work->folders[0].name, u"B"_q);
	CHECK_EQ(int(work->folders[0].include.value_or(
		Purple::FolderInclude::None)), int(Purple::FolderInclude::All));
	CHECK(Purple::IsAllFolders(work->folders[1]));

	// Saying nothing about folders is saying none, the same rule the lists
	// follow. "*ALL" is how you ask for the strip back.
	CHECK(result.settings.preset(u"none"_q)->folders.empty());
	const auto every = result.settings.preset(u"every"_q);
	CHECK_EQ(int(every->folders.size()), 1);
	CHECK(Purple::IsAllFolders(every->folders[0]));

	// The old spelling, and an attempt to redefine the built-in set.
	const auto stale = Parse(uR"(
[folder_sets.ALL]
folders = [ { name = "B" } ]

[presets.work]
folders = [ { name = "B", filtered = false } ]
)"_q);
	CHECK(stale.ok());
	CHECK(WarnsAbout(stale, u"cannot be redefined"_q));
	CHECK(WarnsAbout(stale, u"include_in_main_view = \"all\""_q));

	// The enum, and the two spellings it replaced. Both of those are retired
	// rather than quietly accepted: a folder still saying include_in_main_view_p
	// would otherwise include nothing and look exactly like one that meant to.
	const auto modes = Parse(uR"(
[presets.work]
folders = [
  { name = "Music", include_in_main_view = "pinned" },
  { name = "B",     include_in_main_view = "all" },
  { name = "Quiet", include_in_main_view = "none" },
  { name = "Plain" },
  { name = "Old",   include_in_main_view_p = true },
  { name = "Older", pinned_only_p = true },
  { name = "Typo",  include_in_main_view = "some" },
]
)"_q);
	CHECK(modes.ok());
	const auto shaped = modes.settings.preset(u"work"_q);
	const auto mode = [&](int index) {
		return int(shaped->folders[index].include.value_or(
			Purple::FolderInclude::None));
	};
	CHECK_EQ(mode(0), int(Purple::FolderInclude::Pinned));
	CHECK_EQ(mode(1), int(Purple::FolderInclude::All));
	CHECK_EQ(mode(2), int(Purple::FolderInclude::None));
	CHECK(!shaped->folders[3].include.has_value());
	CHECK(WarnsAbout(modes, u"no longer a yes-or-no"_q));
	CHECK(WarnsAbout(modes, u"write include_in_main_view = \"pinned\""_q));
	CHECK(WarnsAbout(modes, u"one of none, pinned, all"_q));

	// A misspelt value is ignored rather than guessed at, so the folder falls
	// back to contributing nothing.
	CHECK(!shaped->folders[6].include.has_value());

	CHECK_EQ(
		Purple::FolderIncludeName(Purple::FolderInclude::Pinned),
		u"pinned"_q);
	CHECK(!Purple::ParseFolderInclude(u"some"_q).has_value());
	CHECK_EQ(
		int(*Purple::ParseFolderInclude(u"  ALL "_q)),
		int(Purple::FolderInclude::All));
}

void TestShowModes() {
	Begin("show modes");

	const auto parsed = Parse(uR"(
[lists.everything]
kinds = ["private", "groups", "channels", "bots"]

[lists.loud]
kinds = ["channels"]

[presets.plain]
list_order = [ { list = "everything" } ]

[presets.spelled]
list_order = [
  { list = "loud",       show_mode = "never" },
  { list = "everything", show_mode = "message_or_reaction" },
]

[presets.stale]
list_order = [
  { list = "everything", show_p = true, groups_require_mention_p = true },
]

[presets.typo]
list_order = [ { list = "everything", show_mode = "sometimes" } ]
)"_q);
	CHECK(parsed.ok());

	// Saying nothing is the interesting case: the entry keeps no mode at all,
	// and the chat kind supplies one when the chat is finally in hand.
	const auto plain = parsed.settings.preset(u"plain"_q);
	CHECK(!plain->listOrder[0].show.has_value());

	const auto resolved = Purple::Resolve(parsed.settings, u"plain"_q);
	const auto modeOf = [&](Purple::ChatKind kind) {
		return Mode(Purple::Visible(parsed.settings, *resolved, 1, kind).show);
	};
	CHECK_EQ(modeOf(Purple::ChatKind::Channel),
		Mode(Purple::ShowMode::Always));
	CHECK_EQ(modeOf(Purple::ChatKind::Group),
		Mode(Purple::ShowMode::Mention));
	CHECK_EQ(modeOf(Purple::ChatKind::Private),
		Mode(Purple::ShowMode::Message));
	CHECK_EQ(modeOf(Purple::ChatKind::Bot),
		Mode(Purple::ShowMode::Always));

	// The same four, straight off DefaultShowMode, so the table above is not
	// only being read through one path.
	CHECK_EQ(Mode(Purple::DefaultShowMode(Purple::ChatKind::Channel)),
		Mode(Purple::ShowMode::Always));
	CHECK_EQ(Mode(Purple::DefaultShowMode(Purple::ChatKind::Group)),
		Mode(Purple::ShowMode::Mention));
	CHECK_EQ(Mode(Purple::DefaultShowMode(Purple::ChatKind::Bot)),
		Mode(Purple::ShowMode::Always));
	CHECK_EQ(Mode(Purple::DefaultShowMode(Purple::ChatKind::Private)),
		Mode(Purple::ShowMode::Message));

	// An explicit mode wins over the kind default, in both directions.
	const auto spelled = Purple::Resolve(parsed.settings, u"spelled"_q);
	CHECK_EQ(
		Mode(Purple::Visible(
			parsed.settings,
			*spelled,
			1,
			Purple::ChatKind::Channel).show),
		Mode(Purple::ShowMode::Never));
	CHECK_EQ(
		Mode(Purple::Visible(
			parsed.settings,
			*spelled,
			1,
			Purple::ChatKind::Group).show),
		Mode(Purple::ShowMode::MessageOrReaction));

	// Both retired spellings are reported rather than silently ignored, and
	// the entry falls back to the defaults.
	CHECK(WarnsAbout(parsed, u"no longer a yes-or-no"_q));
	CHECK(WarnsAbout(parsed, u"show_mode = \"mention\""_q));
	CHECK(!parsed.settings.preset(u"stale"_q)->listOrder[0].show.has_value());

	// A value nobody can spell is ignored, which leaves the default rather
	// than guessing at what was meant.
	CHECK(WarnsAbout(parsed, u"one of always, message, message_or_reaction"_q));
	CHECK(!parsed.settings.preset(u"typo"_q)->listOrder[0].show.has_value());

	// Fall-through is Never, which is the whole model in one line: a preset
	// names what gets through.
	const auto nothing = Purple::Resolve(parsed.settings, u"spelled"_q);
	CHECK_EQ(
		Mode(Purple::Visible(
			parsed.settings,
			*nothing,
			1,
			Purple::ChatKind::Bot).show),
		Mode(Purple::ShowMode::MessageOrReaction));

	// Rank is what makes "the most permissive of these two folders wins" a
	// comparison. It deliberately does not follow declaration order.
	CHECK(Purple::ShowModeRank(Purple::ShowMode::Always)
		> Purple::ShowModeRank(Purple::ShowMode::MessageOrReaction));
	CHECK(Purple::ShowModeRank(Purple::ShowMode::MessageOrReaction)
		> Purple::ShowModeRank(Purple::ShowMode::Message));
	CHECK(Purple::ShowModeRank(Purple::ShowMode::Message)
		> Purple::ShowModeRank(Purple::ShowMode::Mention));
	CHECK(Purple::ShowModeRank(Purple::ShowMode::Mention)
		> Purple::ShowModeRank(Purple::ShowMode::Never));

	// Only these two answer without the chat's unread state, which is what
	// keeps every other preset off the re-check path.
	CHECK(!Purple::ShowModeWatchesUnread(Purple::ShowMode::Always));
	CHECK(!Purple::ShowModeWatchesUnread(Purple::ShowMode::Never));
	CHECK(Purple::ShowModeWatchesUnread(Purple::ShowMode::Message));
	CHECK(Purple::ShowModeWatchesUnread(Purple::ShowMode::Mention));

	CHECK_EQ(Purple::ShowModeName(Purple::ShowMode::MessageOrReaction),
		u"message_or_reaction"_q);
	CHECK(!Purple::ParseShowMode(u"sometimes"_q).has_value());
	CHECK_EQ(Mode(*Purple::ParseShowMode(u"  MENTION "_q)),
		Mode(Purple::ShowMode::Mention));

	// A folder carries one too, for the chats it contributes - and badge_p
	// alongside it, which is a different question again.
	const auto folders = Parse(uR"(
[presets.work]
folders = [
  { name = "Music", include_in_main_view = "pinned", show_mode = "message", badge_p = false },
  { name = "B" },
]
)"_q);
	CHECK(folders.ok());
	const auto shaped = folders.settings.preset(u"work"_q);
	CHECK(shaped != nullptr);
	CHECK_EQ(int(shaped->folders.size()), 2);
	CHECK_EQ(Mode(shaped->folders[0].showMode), Mode(Purple::ShowMode::Message));
	CHECK(!shaped->folders[0].badge.value_or(true));

	// Saying nothing leaves a folder counted, which is what almost every
	// folder wants and nobody should have to write.
	CHECK(!shaped->folders[1].badge.has_value());
	CHECK(shaped->folders[1].badge.value_or(true));

	// enabled_p survives the parse as written, and the resolution is what
	// drops it - so a disabled entry keeps whatever was configured on it and
	// simply does nothing.
	const auto off = Parse(uR"(
[lists.all]
kinds = ["private"]

[presets.work]
list_order = [ { list = "all" } ]
folders = [
  { name = "XP", enabled_p = false, show_mode = "always", notify_p = false },
  { name = "Uni", enabled_p = false, include_in_main_view = "all" },
  { name = "B" },
]
)"_q);
	CHECK(off.ok());
	const auto written = off.settings.preset(u"work"_q);
	CHECK(written != nullptr);
	CHECK_EQ(int(written->folders.size()), 3);
	CHECK(!written->folders[0].enabled.value_or(true));
	CHECK_EQ(
		Mode(written->folders[0].showMode),
		Mode(Purple::ShowMode::Always));
	CHECK(!written->folders[2].enabled.has_value());

	// A disabled entry stays in the resolution, claimed and inert. It has to:
	// "*ALL" is expanded later against the account's real folders and skips
	// whatever the selection already names, so filtering the entry out here
	// would hand the folder straight back to a preset that also asked for all
	// of them.
	const auto only = Purple::Resolve(off.settings, u"work"_q);
	CHECK(only.has_value());
	CHECK_EQ(int(only->folders.size()), 3);
	CHECK(!Purple::FolderEnabled(only->folders[0]));
	CHECK(!Purple::FolderEnabled(only->folders[1]));
	CHECK(Purple::FolderEnabled(only->folders[2]));

	// What it does not do is act. All three derivations skip it, so a disabled
	// folder cannot silence, exempt or quieten anything from the grave - even
	// though XP said notify_p = false and Uni asked to be pulled in.
	CHECK(only->silencedFolders.empty());
	CHECK(only->exemptFolders.empty());
	CHECK(only->quietFolders.empty());

	// And the derivations still see the enabled ones beside it.
	auto mixed = *written;
	mixed.folders[2].notify = false;
	CHECK_EQ(
		Purple::SilencedFolderNames(mixed.folders).size(),
		size_t(1));
	CHECK_EQ(Purple::SilencedFolderNames(mixed.folders).front(), u"B"_q);
}

void TestViews() {
	Begin("views");

	const auto result = Parse(uR"(
[lists.a]
kinds = ["private"]
[lists.b]
kinds = ["bots"]

[presets.work]
list_order = [ { list = "a" } ]

[[presets.work.views]]
name   = "Focus"
pinned = [ 5, 6, 5 ]
list_order = [ { list = "a", show_mode = "always", notify_p = false } ]

[[presets.work.views]]
name = "Focus"
list_order = [ { list = "b" } ]

[[presets.work.views]]
name = "Empty"

[[presets.work.views]]
list_order = [ { list = "b" } ]
)"_q);
	CHECK(result.ok());
	CHECK(WarnsAbout(result, u"already a view called 'Focus'"_q));
	CHECK(WarnsAbout(result, u"names no list"_q));
	CHECK(WarnsAbout(result, u"a view needs 'name'"_q));

	// notify_p inside a view cannot mean anything: a chat has one mute state
	// however many tabs are showing it.
	CHECK(WarnsAbout(result, u"'notify_p' means nothing inside a view"_q));

	// But only when the view wrote it. A list_set exists to be reused, and it
	// was written for a preset's own order where notify_p is exactly what it
	// should say - warning about it every time the set is spread onto a tab
	// would make the idiom unusable while saying nothing to act on.
	const auto spread = Parse(uR"(
[lists.a]
kinds = ["private"]

[list_sets.core]
list_order = [ { list = "a", show_mode = "always", notify_p = true } ]

[presets.work]
list_order = [ "*core" ]

[[presets.work.views]]
name = "Focus"
list_order = [ "*core" ]
)"_q);
	CHECK(spread.ok());
	CHECK(!WarnsAbout(spread, u"'notify_p' means nothing inside a view"_q));
	CHECK_EQ(int(spread.settings.preset(u"work"_q)->views.size()), 1);

	const auto work = result.settings.preset(u"work"_q);
	CHECK_EQ(int(work->views.size()), 1);
	CHECK_EQ(work->views[0].name, u"Focus"_q);
	CHECK_EQ(work->views[0].pinned,
		(std::vector<Purple::PeerIdValue>{ 5, 6 }));
	CHECK_EQ(int(work->views[0].listOrder.size()), 1);

	// A view showing a chat the preset has taken out of the app entirely is not
	// a preference, it is an assertion failure waiting to happen.
	const auto gone = Parse(uR"(
[lists.a]
kinds = ["private"]

[presets.work]
hide_everywhere_p = true
list_order = [ { list = "a" } ]

[[presets.work.views]]
name = "Focus"
list_order = [ { list = "a" } ]
)"_q);
	CHECK(gone.ok());
	CHECK(WarnsAbout(gone, u"leaves nothing for an extra view to show"_q));
	CHECK(gone.settings.preset(u"work"_q)->views.empty());
}

void TestMembers() {
	Begin("members");

	const auto result = Parse(uR"(
[lists.a]
members = [ 1, 2, 2, 3, 1 ]

[lists.b]
members = [ "nope", 4 ]

[lists."*sneaky"]
members = [ 9 ]
)"_q);
	CHECK(result.ok());
	CHECK_EQ(result.settings.list(u"a"_q)->members,
		(std::vector<Purple::PeerIdValue>{ 1, 2, 3 }));
	CHECK(WarnsAbout(result, u"should be peer ids"_q));
	CHECK_EQ(result.settings.list(u"b"_q)->members,
		(std::vector<Purple::PeerIdValue>{ 4 }));

	// A list whose name starts with '*' could never be told apart from a set
	// reference by anyone reading the file.
	CHECK(WarnsAbout(result, u"reserved for set references"_q));
	CHECK(result.settings.list(u"*sneaky"_q) == nullptr);
}

void TestScheduleAndFocus() {
	Begin("schedule and focus");

	const auto result = Parse(uR"(
[presets.work]
list_order = []

[schedule]
enabled_p = true

[[schedule.rules]]
days   = ["mon", "tue"]
from   = "09:00"
to     = "17:00"
preset = "work"

[[schedule.rules]]
from   = "09:00"
to     = "17:00"
preset = "ghost"

[[schedule.rules]]
from   = "09:00"
to     = "09:00"
preset = "work"

[focus_sync]
enabled_p    = true
enter_preset = "work"
exit_preset  = "previous"

[peek]
hotkey   = "Ctrl+Alt+K"
auto_off = "90s"
)"_q);
	CHECK(result.ok());
	CHECK(result.settings.schedule.enabled);

	// Only the first rule is usable: the second names a missing preset, the
	// third is a zero-length window.
	CHECK_EQ(result.settings.schedule.rules.size(), size_t(1));
	CHECK(WarnsAbout(result, u"'ghost' does not exist"_q));
	CHECK(WarnsAbout(result, u"the same time"_q));

	const auto &rule = result.settings.schedule.rules[0];
	CHECK(rule.enabled);
	CHECK_EQ(rule.from, 9 * 60);
	CHECK_EQ(rule.till, 17 * 60);
	CHECK_EQ(int(rule.days.size()), 2);

	CHECK(result.settings.focusSync.enabled);
	CHECK_EQ(result.settings.focusSync.enterPreset, u"work"_q);
	CHECK_EQ(result.settings.peek.hotkey, u"Ctrl+Alt+K"_q);
	CHECK_EQ(result.settings.peek.autoOffSeconds, 90);

	// A DISABLED rule aimed at a preset that does not exist is kept and quiet:
	// that is the normal state of the example in the starter file, and a fresh
	// install must not complain on every start.
	const auto sleeping = Parse(uR"(
[[schedule.rules]]
enabled_p = false
days      = ["mon"]
from      = "09:00"
to        = "17:00"
preset    = "ghost"
)"_q);
	CHECK(sleeping.ok());
	CHECK(sleeping.warnings.empty());
	CHECK_EQ(sleeping.settings.schedule.rules.size(), size_t(1));
	CHECK(!sleeping.settings.schedule.rules[0].enabled);

	// Focus sync pointing at nothing turns itself off rather than half-working.
	const auto broken = Parse(uR"(
[focus_sync]
enabled_p    = true
enter_preset = "ghost"
)"_q);
	CHECK(broken.ok());
	CHECK(!broken.settings.focusSync.enabled);
	CHECK(WarnsAbout(broken, u"turning focus sync off"_q));
}

void TestScalarParsers() {
	Begin("scalar parsers");

	CHECK_EQ(Purple::ParseDuration(u"2m"_q).value_or(-1), 120);
	CHECK_EQ(Purple::ParseDuration(u"90s"_q).value_or(-1), 90);
	CHECK_EQ(Purple::ParseDuration(u"1h"_q).value_or(-1), 3600);
	CHECK_EQ(Purple::ParseDuration(u"0"_q).value_or(-1), 0);
	CHECK_EQ(Purple::ParseDuration(u"off"_q).value_or(-1), 0);
	CHECK(!Purple::ParseDuration(u"soon"_q).has_value());
	CHECK(!Purple::ParseDuration(u"-5m"_q).has_value());

	CHECK_EQ(Purple::ParseTimeOfDay(u"09:00"_q).value_or(-1), 540);
	CHECK_EQ(Purple::ParseTimeOfDay(u"23:59"_q).value_or(-1), 1439);
	CHECK(!Purple::ParseTimeOfDay(u"24:00"_q).has_value());
	CHECK(!Purple::ParseTimeOfDay(u"9am"_q).has_value());

	CHECK_EQ(Purple::ParseWeekday(u"mon"_q).value_or(-1), 1);
	CHECK_EQ(Purple::ParseWeekday(u"Sunday"_q).value_or(-1), 7);
	CHECK(!Purple::ParseWeekday(u"caturday"_q).has_value());
}

void TestPremiumStillParses() {
	Begin("premium");

	const auto result = Parse(uR"([premium]
# Unlock the client-side features.
enabled_p = true
)"_q);
	CHECK(result.ok());
	CHECK(result.settings.premium.enabled);

	// A file with nothing but the Premium toggle is complete, not
	// half-configured: it must not log a warning on every single start.
	CHECK(result.warnings.empty());
	CHECK(result.settings.lists.empty());
	CHECK(result.settings.presets.empty());
	CHECK(!result.settings.focusSync.enabled);

	const auto off = Parse(u"[premium]\nenabled_p = false\n"_q);
	CHECK(off.ok());
	CHECK(!off.settings.premium.enabled);

	// The old spelling is gone, and gone loudly - silently defaulting to true
	// would turn the toggle off-looking and on-behaving.
	const auto stale = Parse(u"[premium]\nenabled = false\n"_q);
	CHECK(stale.ok());
	CHECK(stale.settings.premium.enabled);
	CHECK(WarnsAbout(stale, u"spelled 'enabled_p' now"_q));

	// No file content at all is still a usable configuration.
	const auto empty = Parse(QString());
	CHECK(empty.ok());
	CHECK(empty.settings.premium.enabled);
	CHECK(empty.settings.lists.empty());
	CHECK(empty.warnings.empty());

	// A top-level list_order is where priority used to live.
	const auto order = Parse(u"list_order = [\"a\"]\n"_q);
	CHECK(order.ok());
	CHECK(WarnsAbout(order, u"each preset writes its own"_q));
}

void TestVersion() {
	Begin("version");

	// Absent is the ordinary case - every file written before the key existed
	// says nothing - so it must be silent. A file that starts nagging the day
	// it is read by a newer build would teach the banner to be ignored.
	const auto absent = Parse(Example());
	CHECK(absent.ok());
	CHECK_EQ(absent.settings.version, 1);
	CHECK(absent.warnings.empty());

	const auto current = Parse(u"version = 1\n"_q + Example());
	CHECK(current.ok());
	CHECK_EQ(current.settings.version, 1);
	CHECK(current.warnings.empty());

	// A file from a newer build parses as far as this one understands it, and
	// says so. Everything this build knows about is still exactly where it was.
	const auto newer = Parse(u"version = 2\n"_q + Example());
	CHECK(newer.ok());
	CHECK_EQ(newer.settings.version, 2);
	CHECK(WarnsAbout(newer, u"version 2"_q));
	CHECK_EQ(ListNames(newer.settings).join(u","_q),
		u"os,emergency,colleagues,people,noise"_q);
	CHECK_EQ(int(newer.settings.presets.size()), 2);

	const auto text = Parse(u"version = \"abc\"\n"_q + Example());
	CHECK(text.ok());
	CHECK_EQ(text.settings.version, 1);
	CHECK(WarnsAbout(text, u"should be a whole number"_q));

	const auto zero = Parse(u"version = 0\n"_q + Example());
	CHECK(zero.ok());
	CHECK_EQ(zero.settings.version, 1);
	CHECK(WarnsAbout(zero, u"should be 1 or more"_q));
}

void TestBrokenFile() {
	Begin("broken file");

	const auto result = Parse(u"[lists.a\nmembers = ["_q);
	CHECK(!result.ok());
	CHECK(!result.error.isEmpty());

	// And the splicer refuses to touch it rather than appending blindly.
	const auto spliced = Purple::AddListMember(
		u"[lists.a\nmembers = ["_q,
		Path(),
		u"a"_q,
		42,
		Titles());
	CHECK(!spliced.ok());
	CHECK(!spliced.changed);
	CHECK_EQ(spliced.text, u"[lists.a\nmembers = ["_q);
}

void TestSpliceAdd() {
	Begin("splice add");

	const auto before = Example();
	const auto result = Purple::AddListMember(
		before,
		Path(),
		u"colleagues"_q,
		555000111,
		[](Purple::PeerIdValue) { return u"New Person"_q; });
	CHECK(result.ok());
	CHECK(result.changed);
	CHECK(result.text.contains(u"555000111,   # New Person"_q)
		|| result.text.contains(u"555000111, # New Person"_q));

	// Every comment in the file survives, including the one inside the array.
	CHECK(result.text.contains(u"# my settings"_q));
	CHECK(result.text.contains(u"# who I actually work with"_q));
	CHECK(result.text.contains(u"# Ali Rezaei"_q));
	CHECK(result.text.contains(u"# My Todo Channel"_q));

	// The new id joins the end of the right list and nothing else moves.
	CHECK_EQ(Purple::ListMembers(result.text, Path(), u"colleagues"_q),
		(std::vector<Purple::PeerIdValue>{ 123456789, 987654321, 555000111 }));
	CHECK_EQ(Purple::ListMembers(result.text, Path(), u"os"_q),
		(std::vector<Purple::PeerIdValue>{ 1234567890 }));

	// The indentation of existing members is matched, not invented.
	CHECK(result.text.contains(u"\n  555000111,"_q));

	// Adding what is already there changes nothing at all.
	const auto again = Purple::AddListMember(
		result.text,
		Path(),
		u"colleagues"_q,
		555000111,
		Titles());
	CHECK(again.ok());
	CHECK(!again.changed);
	CHECK_EQ(again.text, result.text);
}

void TestSpliceRemove() {
	Begin("splice remove");

	const auto before = Example();
	const auto result = Purple::RemoveListMember(
		before,
		Path(),
		u"colleagues"_q,
		123456789,
		Titles());
	CHECK(result.ok());
	CHECK(result.changed);

	// Exactly one line goes, and it is the right one.
	CHECK_EQ(result.text.count('\n'), before.count('\n') - 1);
	CHECK(!result.text.contains(u"# Ali Rezaei"_q));
	CHECK(!result.text.contains(u"  123456789,"_q));
	CHECK(result.text.contains(u"987654321,    # Backend Team"_q));

	// The similar-looking id in another list is untouched.
	CHECK(result.text.contains(u"1234567890,   # My Todo Channel"_q));
	CHECK(result.text.contains(u"# who I actually work with"_q));
	CHECK_EQ(Purple::ListMembers(result.text, Path(), u"colleagues"_q),
		(std::vector<Purple::PeerIdValue>{ 987654321 }));

	// Removing what is not there is a no-op, not an error.
	const auto absent = Purple::RemoveListMember(
		before,
		Path(),
		u"colleagues"_q,
		42,
		Titles());
	CHECK(absent.ok());
	CHECK(!absent.changed);
	CHECK_EQ(absent.text, before);

	// Emptying a list leaves a well-formed empty array.
	auto emptied = Purple::RemoveListMember(
		before,
		Path(),
		u"emergency"_q,
		111222333,
		Titles());
	CHECK(emptied.ok());
	CHECK(Purple::ListMembers(emptied.text, Path(), u"emergency"_q).empty());
	CHECK(Parse(emptied.text).ok());
}

void TestSpliceCanonicalises() {
	Begin("splice canonicalises");

	const auto squashed = uR"(# head comment
[lists.a]
members = [ 1, 2, 3 ]   # inline note
[lists.b]
members = []
)"_q;
	const auto result = Purple::AddListMember(
		squashed,
		Path(),
		u"a"_q,
		4,
		[](Purple::PeerIdValue id) {
			return u"Name %1"_q.arg(id);
		});
	CHECK(result.ok());
	CHECK(result.changed);

	// Ids are preserved in order and the array becomes one-per-line.
	CHECK_EQ(Purple::ListMembers(result.text, Path(), u"a"_q),
		(std::vector<Purple::PeerIdValue>{ 1, 2, 3, 4 }));
	CHECK(result.text.contains(u"\n  1, # Name 1"_q));
	CHECK(result.text.contains(u"\n  4, # Name 4"_q));

	// The blast radius stops at the array: text before and after is untouched,
	// including the trailing comment that followed the closing bracket.
	CHECK(result.text.startsWith(u"# head comment\n[lists.a]\n"_q));
	CHECK(result.text.contains(u"]   # inline note"_q));
	CHECK(result.text.contains(u"[lists.b]"_q));
	CHECK(Parse(result.text).ok());

	// An empty array is canonicalised the same way.
	const auto filled = Purple::AddListMember(
		result.text,
		Path(),
		u"b"_q,
		9,
		Titles());
	CHECK(filled.ok());
	CHECK_EQ(Purple::ListMembers(filled.text, Path(), u"b"_q),
		(std::vector<Purple::PeerIdValue>{ 9 }));
}

void TestSpliceMissingArray() {
	Begin("splice missing array");

	const auto text = uR"([lists.a]
title = "A"

[lists.b]
title = "B"
)"_q;
	const auto result = Purple::AddListMember(
		text,
		Path(),
		u"a"_q,
		77,
		Titles());
	CHECK(result.ok());
	CHECK(result.changed);
	CHECK(Parse(result.text).ok());
	CHECK_EQ(Purple::ListMembers(result.text, Path(), u"a"_q),
		(std::vector<Purple::PeerIdValue>{ 77 }));
	CHECK(result.text.contains(u"title = \"A\""_q));
	CHECK(result.text.contains(u"[lists.b]"_q));

	// A list that is not in the file is refused, not created.
	const auto missing = Purple::AddListMember(
		text,
		Path(),
		u"ghosts"_q,
		1,
		Titles());
	CHECK(!missing.ok());
	CHECK_EQ(missing.text, text);
	CHECK(missing.error.contains(u"[lists.ghosts]"_q));
}

void TestSpliceStability() {
	Begin("splice stability");

	// 100 add/remove cycles, as the spec's acceptance test asks for. The file
	// has to come back byte-identical and every comment has to still be there.
	auto text = Example();
	const auto original = text;
	for (auto i = 0; i != 100; ++i) {
		const auto id = Purple::PeerIdValue(500000 + i);
		auto added = Purple::AddListMember(
			text,
			Path(),
			u"colleagues"_q,
			id,
			Titles());
		if (!added.ok()) {
			CHECK(added.ok());
			return;
		}
		auto removed = Purple::RemoveListMember(
			added.text,
			Path(),
			u"colleagues"_q,
			id,
			Titles());
		if (!removed.ok()) {
			CHECK(removed.ok());
			return;
		}
		text = removed.text;
	}
	CHECK_EQ(text, original);

	// And a long run of adds keeps the file parseable and ordered throughout.
	auto grown = Example();
	auto expected = std::vector<Purple::PeerIdValue>{ 123456789, 987654321 };
	for (auto i = 0; i != 50; ++i) {
		const auto id = Purple::PeerIdValue(600000 + i);
		auto added = Purple::AddListMember(
			grown,
			Path(),
			u"colleagues"_q,
			id,
			Titles());
		if (!added.ok()) {
			CHECK(added.ok());
			return;
		}
		grown = added.text;
		expected.push_back(id);
	}
	CHECK_EQ(Purple::ListMembers(grown, Path(), u"colleagues"_q), expected);
	CHECK(grown.contains(u"# who I actually work with"_q));
	CHECK(Parse(grown).ok());
}

void TestSpliceOddFormatting() {
	Begin("splice odd formatting");

	// Comments and blank lines between members must survive a removal of a
	// neighbour, which is the whole reason removal is line-based.
	const auto text = uR"([lists.a]
members = [
  1,  # first

  # a note about the next one
  2,
  3,
]
)"_q;
	const auto result = Purple::RemoveListMember(
		text,
		Path(),
		u"a"_q,
		2,
		Titles());
	CHECK(result.ok());
	CHECK(result.text.contains(u"# a note about the next one"_q));
	CHECK(result.text.contains(u"1,  # first"_q));
	CHECK_EQ(Purple::ListMembers(result.text, Path(), u"a"_q),
		(std::vector<Purple::PeerIdValue>{ 1, 3 }));

	// Tabs are indentation too.
	const auto tabbed = u"[lists.a]\nmembers = [\n\t1,\n]\n"_q;
	const auto added = Purple::AddListMember(
		tabbed,
		Path(),
		u"a"_q,
		2,
		Titles());
	CHECK(added.ok());
	CHECK(added.text.contains(u"\n\t2,"_q));
}

void TestSpliceCrlf() {
	Begin("splice CRLF");

	const auto text = u"[lists.a]\r\nmembers = [\r\n  1,\r\n]\r\n"_q;
	const auto added = Purple::AddListMember(
		text,
		Path(),
		u"a"_q,
		2,
		Titles());
	CHECK(added.ok());
	CHECK(Parse(added.text).ok());
	CHECK_EQ(Purple::ListMembers(added.text, Path(), u"a"_q),
		(std::vector<Purple::PeerIdValue>{ 1, 2 }));

	// No stray lone-LF line is introduced into a CRLF file.
	CHECK(!added.text.contains(u"\n"_q)
		|| added.text.count(u"\r\n"_q) == added.text.count('\n'));
}

void TestNameSanitising() {
	Begin("name sanitising");

	// A display name is user-controlled text landing in a TOML file; a newline
	// in it would otherwise write a second line into the array.
	const auto result = Purple::AddListMember(
		u"[lists.a]\nmembers = [\n]\n"_q,
		Path(),
		u"a"_q,
		5,
		[](Purple::PeerIdValue) {
			return u"evil\n999999, # injected"_q;
		});
	CHECK(result.ok());
	CHECK_EQ(Purple::ListMembers(result.text, Path(), u"a"_q),
		(std::vector<Purple::PeerIdValue>{ 5 }));
	CHECK(Parse(result.text).ok());
}

// A preset with two views, the first of them already pinning somebody. The
// pinned array is written the way the splice writes one, so a round trip can be
// checked byte for byte rather than "near enough".
[[nodiscard]] QString Viewed() {
	return uR"(# my settings

[lists.a]
kinds = ["private"]

[presets.work]
list_order = [ { list = "a" } ]

# the tab I actually watch
[[presets.work.views]]
name = "Focus"
pinned = [
  123456789, # Ali Rezaei
]
list_order = [ { list = "a" } ]

[[presets.work.views]]
name = "Rest"
list_order = [ { list = "a" } ]
)"_q;
}

void TestSpliceAddList() {
	Begin("splice add list");

	const auto before = uR"(# Mine.
[lists.os]
title = "OS"
members = [
  1, # One
]

[lists.people]
members = [
]

[presets.work]
list_order = [ { list = "os" } ]
)"_q;

	const auto added = Purple::AddList(
		before,
		Path(),
		u"reading"_q,
		u"Reading"_q);
	CHECK(added.ok());
	CHECK(added.changed);

	// Lands after the last list, not at the end of the file, so [presets.work]
	// is still below every [lists.*].
	CHECK(added.text.indexOf(u"[lists.reading]"_q)
		> added.text.indexOf(u"[lists.people]"_q));
	CHECK(added.text.indexOf(u"[lists.reading]"_q)
		< added.text.indexOf(u"[presets.work]"_q));

	// It parses, it is empty, and the comment above the first list survived.
	const auto parsed = Parse(added.text);
	CHECK(parsed.ok());
	const auto list = parsed.settings.list(u"reading"_q);
	CHECK(list != nullptr);
	CHECK(list->members.empty());
	CHECK_EQ(list->title, u"Reading"_q);
	CHECK(added.text.startsWith(u"# Mine."_q));

	// And the members array it wrote is the canonical shape, so the very next
	// tick from the chat menu splices one line in rather than rewriting it.
	const auto ticked = Purple::AddListMember(
		added.text,
		Path(),
		u"reading"_q,
		42,
		[](Purple::PeerIdValue) { return u"Someone"_q; });
	CHECK(ticked.ok());
	CHECK_EQ(
		Purple::ListMembers(ticked.text, Path(), u"reading"_q),
		(std::vector<Purple::PeerIdValue>{ 42 }));

	// A title that only repeats the name is not written - it would be noise in
	// a file somebody reads.
	const auto plain = Purple::AddList(before, Path(), u"inbox"_q, u"inbox"_q);
	CHECK(plain.ok());
	CHECK(!plain.text.contains(u"title = \"inbox\""_q));

	// Names the parser would refuse are refused here, before anything is
	// written, and so are duplicates.
	for (const auto &bad : { u""_q, u"   "_q, u"*core"_q, u"os"_q }) {
		const auto refused = Purple::AddList(before, Path(), bad, QString());
		CHECK(!refused.ok());
		CHECK_EQ(refused.text, before);
	}

	// A name TOML cannot hold bare is quoted rather than rejected.
	const auto odd = Purple::AddList(
		before,
		Path(),
		u"day one"_q,
		u"Day one"_q);
	CHECK(odd.ok());
	CHECK(odd.text.contains(u"[lists.\"day one\"]"_q));
	CHECK(Parse(odd.text).settings.list(u"day one"_q) != nullptr);

	// A file with no lists at all gets one at the end.
	const auto bare = Purple::AddList(
		u"[presets.work]\nlist_order = []\n"_q,
		Path(),
		u"first"_q,
		QString());
	CHECK(bare.ok());
	CHECK(Parse(bare.text).settings.list(u"first"_q) != nullptr);

	// A file that does not parse is never written to.
	const auto broken = Purple::AddList(
		u"[lists.a\n"_q,
		Path(),
		u"b"_q,
		QString());
	CHECK(!broken.ok());
}

void TestSplicePresetPinned() {
	Begin("splice preset pinned");

	const auto before = uR"([lists.a]
kinds = ["private"]

[presets.work]
list_order = [ { list = "a" } ]

[[presets.work.views]]
name = "Focus"
)"_q;
	CHECK(Purple::PresetPinned(before, Path(), u"work"_q).empty());

	// The array is written straight under the header, since a preset has no
	// `name' key to sit below.
	const auto added = Purple::SetPresetPinned(
		before,
		Path(),
		u"work"_q,
		{ 7, 8 },
		[](Purple::PeerIdValue id) {
			return (id == 7) ? u"Seven"_q : u"Eight"_q;
		});
	CHECK(added.ok());
	CHECK(added.changed);
	CHECK_EQ(
		Purple::PresetPinned(added.text, Path(), u"work"_q),
		(std::vector<Purple::PeerIdValue>{ 7, 8 }));
	CHECK(added.text.contains(u"# Seven"_q));

	// The view's own pins are a different array and are left alone, which is
	// the whole reason the two functions locate different tables.
	CHECK(Purple::ViewPinned(added.text, Path(), u"work"_q, u"Focus"_q).empty());

	// Past five, which is the point: the ceiling was the server's limit on the
	// account order the main view used to mirror, and this order is ours.
	const auto many = Purple::SetPresetPinned(
		added.text,
		Path(),
		u"work"_q,
		{ 1, 2, 3, 4, 5, 6, 7 },
		[](Purple::PeerIdValue id) { return QString::number(id); });
	CHECK(many.ok());
	CHECK_EQ(
		int(Purple::PresetPinned(many.text, Path(), u"work"_q).size()),
		7);

	// Writing the same order back is not a write at all, so the file's mtime
	// does not move and the watcher does not wake.
	const auto same = Purple::SetPresetPinned(
		many.text,
		Path(),
		u"work"_q,
		{ 1, 2, 3, 4, 5, 6, 7 },
		[](Purple::PeerIdValue id) { return QString::number(id); });
	CHECK(same.ok());
	CHECK(!same.changed);

	// Clearing it puts the preset back to mirroring the account.
	const auto cleared = Purple::SetPresetPinned(
		many.text,
		Path(),
		u"work"_q,
		{},
		[](Purple::PeerIdValue id) { return QString::number(id); });
	CHECK(cleared.ok());
	CHECK(Purple::PresetPinned(cleared.text, Path(), u"work"_q).empty());

	// A preset that is not there is refused rather than created.
	const auto missing = Purple::SetPresetPinned(
		before,
		Path(),
		u"nope"_q,
		{ 1 },
		[](Purple::PeerIdValue id) { return QString::number(id); });
	CHECK(!missing.ok());
	CHECK_EQ(missing.text, before);

	// And the parser reads back what the splice wrote.
	const auto parsed = Parse(added.text);
	CHECK(parsed.ok());
	CHECK_EQ(
		parsed.settings.preset(u"work"_q)->pinned,
		(std::vector<Purple::PeerIdValue>{ 7, 8 }));

	// Saying nothing leaves it empty, which is what keeps the main view
	// mirroring for every preset that never asked.
	const auto quiet = Parse(u"[presets.b]\nlist_order = []\n"_q);
	CHECK(quiet.ok());
	CHECK(quiet.settings.preset(u"b"_q)->pinned.empty());
}

void TestSpliceViewPinned() {
	Begin("splice view pins");

	using Ids = std::vector<Purple::PeerIdValue>;
	const auto before = Viewed();
	CHECK_EQ(Purple::ViewPinned(before, Path(), u"work"_q, u"Focus"_q),
		(Ids{ 123456789 }));
	CHECK(Purple::ViewPinned(before, Path(), u"work"_q, u"Rest"_q).empty());

	// Reordering rewrites the array and nothing else in the file.
	const auto moved = Purple::SetViewPinned(
		before,
		Path(),
		u"work"_q,
		u"Focus"_q,
		Ids{ 987654321, 123456789 },
		Titles());
	CHECK(moved.ok());
	CHECK(moved.changed);
	CHECK_EQ(Purple::ViewPinned(moved.text, Path(), u"work"_q, u"Focus"_q),
		(Ids{ 987654321, 123456789 }));
	CHECK(moved.text.contains(u"# my settings"_q));
	CHECK(moved.text.contains(u"# the tab I actually watch"_q));
	CHECK(moved.text.contains(u"\n  987654321, # Backend Team\n"_q));

	// The other view is not the one that moved, and stays without a key.
	CHECK(Purple::ViewPinned(moved.text, Path(), u"work"_q, u"Rest"_q).empty());
	CHECK_EQ(moved.text.count(u"pinned = ["_q), 1);

	// And back again, byte for byte. This is the property that matters: the
	// file is the user's, and the app has to be able to touch one array in it
	// without leaving a trace anywhere else.
	const auto back = Purple::SetViewPinned(
		moved.text,
		Path(),
		u"work"_q,
		u"Focus"_q,
		Ids{ 123456789 },
		Titles());
	CHECK(back.ok());
	CHECK(back.changed);
	CHECK_EQ(back.text, before);

	// Writing what is already there writes nothing.
	const auto same = Purple::SetViewPinned(
		before,
		Path(),
		u"work"_q,
		u"Focus"_q,
		Ids{ 123456789 },
		Titles());
	CHECK(same.ok());
	CHECK(!same.changed);
	CHECK_EQ(same.text, before);

	// A view with no array yet gets one, under its own name rather than at the
	// top of the block, and matched case-insensitively the way the parser
	// decides two views are the same one.
	const auto added = Purple::SetViewPinned(
		before,
		Path(),
		u"work"_q,
		u"rest"_q,
		Ids{ 111222333 },
		Titles());
	CHECK(added.ok());
	CHECK(added.changed);
	CHECK_EQ(Purple::ViewPinned(added.text, Path(), u"work"_q, u"Rest"_q),
		(Ids{ 111222333 }));
	CHECK(added.text.contains(
		u"name = \"Rest\"\npinned = [\n  111222333, # Maman\n]\n"_q));

	// Emptying leaves the array rather than the key, which is the shape the
	// next pin will write into.
	const auto emptied = Purple::SetViewPinned(
		before,
		Path(),
		u"work"_q,
		u"Focus"_q,
		Ids(),
		Titles());
	CHECK(emptied.ok());
	CHECK(emptied.changed);
	CHECK(Purple::ViewPinned(emptied.text, Path(), u"work"_q, u"Focus"_q)
		.empty());
	CHECK(emptied.text.contains(u"pinned = [\n]\n"_q));

	// Nothing is written for a preset or a view that is not there.
	for (const auto &[preset, view] : { std::pair{ u"work"_q, u"Gone"_q },
			std::pair{ u"missing"_q, u"Focus"_q } }) {
		const auto refused = Purple::SetViewPinned(
			before,
			Path(),
			preset,
			view,
			Ids{ 123456789 },
			Titles());
		CHECK(!refused.ok());
		CHECK(!refused.changed);
		CHECK_EQ(refused.text, before);
	}

	// An inline view is refused rather than mangled: there is no line to edit.
	const auto inlined = uR"([presets.work]
list_order = [ { list = "a" } ]
views = [ { name = "Focus", list_order = [ { list = "a" } ] } ]
)"_q;
	const auto squashed = Purple::SetViewPinned(
		inlined,
		Path(),
		u"work"_q,
		u"Focus"_q,
		Ids{ 123456789 },
		Titles());
	CHECK(!squashed.ok());
	CHECK_EQ(squashed.text, inlined);

	// A file that does not parse is never written to, same as every other
	// splice: the user may be halfway through an edit of their own.
	const auto broken = Purple::SetViewPinned(
		u"[presets.work"_q,
		Path(),
		u"work"_q,
		u"Focus"_q,
		Ids{ 123456789 },
		Titles());
	CHECK(!broken.ok());
	CHECK(!broken.changed);
}

void TestSetTableBool() {
	Begin("set table bool");

	// The Premium toggle writes into a file the user owns, so it may change
	// nothing but the one token it is responsible for.
	const auto text = uR"(# my settings
[premium]
# keep the ads away
enabled   =    true   # this comment matters

[other]
untouched = 1
)"_q;
	const auto off = Purple::SetTableBool(
		text,
		Path(),
		u"premium"_q,
		u"enabled"_q,
		false);
	CHECK(off.ok());
	CHECK(off.changed);
	CHECK(off.text.contains(u"enabled   =    false   # this comment matters"_q));
	CHECK(off.text.contains(u"# keep the ads away"_q));
	CHECK(off.text.contains(u"# my settings"_q));
	CHECK(off.text.contains(u"untouched = 1"_q));
	CHECK_EQ(off.text.count('\n'), text.count('\n'));

	// Setting what is already set writes nothing at all.
	const auto same = Purple::SetTableBool(
		text,
		Path(),
		u"premium"_q,
		u"enabled"_q,
		true);
	CHECK(same.ok());
	CHECK(!same.changed);
	CHECK_EQ(same.text, text);

	// A missing key joins the existing table rather than starting a new one.
	const auto added = Purple::SetTableBool(
		u"[premium]\n# a note\n\n[other]\nx = 1\n"_q,
		Path(),
		u"premium"_q,
		u"enabled"_q,
		true);
	CHECK(added.ok());
	CHECK(added.text.contains(u"enabled = true"_q));
	CHECK(added.text.contains(u"# a note"_q));
	CHECK_EQ(Parse(added.text).settings.premium.enabled, true);
	CHECK(Parse(added.text).ok());

	// A missing table is appended, and an empty file gains no leading blank.
	const auto fresh = Purple::SetTableBool(
		QString(),
		Path(),
		u"premium"_q,
		u"enabled"_q,
		false);
	CHECK(fresh.ok());
	CHECK_EQ(fresh.text, u"[premium]\nenabled = false\n"_q);

	const auto appended = Purple::SetTableBool(
		u"[other]\nx = 1\n"_q,
		Path(),
		u"premium"_q,
		u"enabled"_q,
		false);
	CHECK(appended.ok());
	CHECK(appended.text.startsWith(u"[other]\nx = 1\n"_q));
	CHECK(appended.text.contains(u"[premium]\nenabled = false"_q));
	CHECK(Parse(appended.text).ok());

	// A file mid-edit is left exactly as it is.
	const auto broken = Purple::SetTableBool(
		u"[premium\nenabled = true"_q,
		Path(),
		u"premium"_q,
		u"enabled"_q,
		false);
	CHECK(!broken.ok());
	CHECK(!broken.changed);
	CHECK_EQ(broken.text, u"[premium\nenabled = true"_q);
}

void TestSetTableString() {
	Begin("set table string");

	// The same contract as the boolean: one token changes and the rest of the
	// user's file - spacing, comments, other tables - is left exactly as it is.
	const auto text = uR"(# my settings
[schedule]
# what runs when no window does
outside   =    "normal"   # this comment matters

[other]
untouched = 1
)"_q;
	const auto home = Purple::SetTableString(
		text,
		Path(),
		u"schedule"_q,
		u"outside"_q,
		u"home"_q);
	CHECK(home.ok());
	CHECK(home.changed);
	CHECK(home.text.contains(
		u"outside   =    \"home\"   # this comment matters"_q));
	CHECK(home.text.contains(u"# what runs when no window does"_q));
	CHECK(home.text.contains(u"# my settings"_q));
	CHECK(home.text.contains(u"untouched = 1"_q));
	CHECK_EQ(home.text.count('\n'), text.count('\n'));

	// Setting what is already set writes nothing at all.
	const auto same = Purple::SetTableString(
		text,
		Path(),
		u"schedule"_q,
		u"outside"_q,
		u"normal"_q);
	CHECK(same.ok());
	CHECK(!same.changed);
	CHECK_EQ(same.text, text);

	// A missing key joins the existing table rather than starting a new one.
	const auto added = Purple::SetTableString(
		u"[schedule]\n# a note\n\n[other]\nx = 1\n"_q,
		Path(),
		u"schedule"_q,
		u"outside"_q,
		u"normal"_q);
	CHECK(added.ok());
	CHECK(added.text.contains(u"outside = \"normal\""_q));
	CHECK(added.text.contains(u"# a note"_q));
	CHECK(Parse(added.text).ok());
	CHECK_EQ(Parse(added.text).settings.schedule.outside, u"normal"_q);

	// A missing table is appended, and an empty file gains no leading blank.
	const auto fresh = Purple::SetTableString(
		QString(),
		Path(),
		u"schedule"_q,
		u"outside"_q,
		u"normal"_q);
	CHECK(fresh.ok());
	CHECK_EQ(fresh.text, u"[schedule]\noutside = \"normal\"\n"_q);

	const auto appended = Purple::SetTableString(
		u"[other]\nx = 1\n"_q,
		Path(),
		u"schedule"_q,
		u"outside"_q,
		u"normal"_q);
	CHECK(appended.ok());
	CHECK(appended.text.startsWith(u"[other]\nx = 1\n"_q));
	CHECK(appended.text.contains(u"[schedule]\noutside = \"normal\""_q));
	CHECK(Parse(appended.text).ok());

	// A file mid-edit is left exactly as it is.
	const auto broken = Purple::SetTableString(
		u"[schedule\noutside = \"normal\""_q,
		Path(),
		u"schedule"_q,
		u"outside"_q,
		u"home"_q);
	CHECK(!broken.ok());
	CHECK(!broken.changed);
	CHECK_EQ(broken.text, u"[schedule\noutside = \"normal\""_q);

	// A quote in the value is escaped rather than closing the string early,
	// and so is the backslash that escapes it. (`table' is one top-level key,
	// the same as the boolean's, so this uses a real string setting rather
	// than a dotted path the op does not take.)
	const auto quotes = Purple::SetTableString(
		u"[peek]\nhotkey = \"Ctrl+Alt+K\"\n"_q,
		Path(),
		u"peek"_q,
		u"hotkey"_q,
		uR"(say "hi" \ ok)"_q);
	CHECK(quotes.ok());
	CHECK(quotes.text.contains(uR"(hotkey = "say \"hi\" \\ ok")"_q));
	const auto backQuotes = Parse(quotes.text);
	CHECK(backQuotes.ok());
	CHECK_EQ(backQuotes.settings.peek.hotkey, uR"(say "hi" \ ok)"_q);

	// The value it reads back is the value it wrote, so a second call with the
	// same text is a no-op rather than a rewrite.
	const auto again = Purple::SetTableString(
		quotes.text,
		Path(),
		u"peek"_q,
		u"hotkey"_q,
		uR"(say "hi" \ ok)"_q);
	CHECK(again.ok());
	CHECK(!again.changed);

	// A '#' inside the value is part of the value, not the start of a comment:
	// the replaced span is the one toml++ read, so nothing here scans for it.
	const auto hash = Purple::SetTableString(
		u"[schedule]\noutside = \"a # b\"  # real comment\n"_q,
		Path(),
		u"schedule"_q,
		u"outside"_q,
		u"c # d"_q);
	CHECK(hash.ok());
	CHECK(hash.text.contains(u"outside = \"c # d\"  # real comment"_q));

	// A [schedule] with no header of its own gains one, exactly as the boolean
	// does - this is the file the schedule screen actually meets.
	const auto implicit = Purple::SetTableString(
		u"# rules only\n\n[presets.home]\nlist_order = []\n\n"
		"[[schedule.rules]]\ndays   = [\"mon\"]\nfrom   = \"09:00\"\n"
		"to     = \"17:00\"\npreset = \"home\"\n"_q,
		Path(),
		u"schedule"_q,
		u"outside"_q,
		u"home"_q);
	CHECK(implicit.ok());
	CHECK(implicit.text.contains(u"[schedule]\noutside = \"home\"\n\n"
		"[[schedule.rules]]"_q));
	const auto backImplicit = Parse(implicit.text);
	CHECK(backImplicit.ok());
	CHECK_EQ(backImplicit.settings.schedule.outside, u"home"_q);
	CHECK_EQ(backImplicit.settings.schedule.rules.size(), size_t(1));

	// A table written as dotted keys has no header to insert under, and one
	// inserted above would swallow it. So it says what to do instead.
	const auto dotted = Purple::SetTableString(
		u"schedule.rules = []\n"_q,
		Path(),
		u"schedule"_q,
		u"outside"_q,
		u"home"_q);
	CHECK(!dotted.ok());
	CHECK(dotted.error.contains(u"dotted keys"_q));

	// An inline table is refused by name rather than by a parse failure the
	// user cannot act on.
	const auto inlined = Purple::SetTableString(
		u"schedule = { enabled_p = true }\n"_q,
		Path(),
		u"schedule"_q,
		u"outside"_q,
		u"home"_q);
	CHECK(!inlined.ok());
	CHECK(inlined.error.contains(u"inline"_q));
}

// A schedule with everything the splicer has to survive: comments above a
// block and beside a value, a `days' array over four lines, and a rule the
// parser throws away sitting in the middle of the ones it keeps.
[[nodiscard]] QString ScheduleExample() {
	return uR"(# my settings

[presets.work]
list_order = []

[presets.play]
list_order = []

[schedule]
# when work happens
enabled_p = true

# the weekday window
[[schedule.rules]]
days   = [
  "mon",
  "tue",
]
from   = "09:00"
to     = "17:00"
preset = "work"   # the important one

[[schedule.rules]]
from   = "10:00"
preset = "play"

[[schedule.rules]]
enabled_p = false
days      = ["sat"]
from      = "20:00"
to        = "23:00"
preset    = "play"

[peek]
hotkey = "Ctrl+Alt+K"
)"_q;
}

[[nodiscard]] Purple::ScheduleRule Rule(
		bool enabled,
		std::vector<int> days,
		int from,
		int till,
		const QString &preset) {
	auto result = Purple::ScheduleRule();
	result.enabled = enabled;
	result.days = std::move(days);
	result.from = from;
	result.till = till;
	result.preset = preset;
	return result;
}

void TestScheduleRuleIdentity() {
	Begin("schedule rule identity");

	const auto text = ScheduleExample();
	const auto parsed = Parse(text);
	CHECK(parsed.ok());
	const auto &rules = parsed.settings.schedule.rules;

	// The middle rule has no 'to', so the parser drops it - and the rule after
	// it keeps the index it has in the file rather than moving up one. That is
	// the whole reason the index is recorded: an edit made from a screen has to
	// land on the rule the user was looking at.
	CHECK_EQ(rules.size(), size_t(2));
	CHECK_EQ(rules[0].sourceIndex, 0);
	CHECK_EQ(rules[1].sourceIndex, 2);
	CHECK_EQ(rules[1].from, 20 * 60);

	const auto lines = text.split('\n');
	CHECK_EQ(lines[rules[0].sourceLine - 1].trimmed(), u"[[schedule.rules]]"_q);
	CHECK_EQ(lines[rules[1].sourceLine - 1].trimmed(), u"[[schedule.rules]]"_q);
	CHECK(rules[1].sourceLine > rules[0].sourceLine);

	// A rule that came from nowhere says so.
	CHECK_EQ(Purple::ScheduleRule().sourceIndex, -1);
	CHECK_EQ(Purple::ScheduleRule().sourceLine, 0);
}

void TestSpliceScheduleSet() {
	Begin("splice schedule set");

	const auto text = ScheduleExample();
	const auto edited = Purple::SetScheduleRule(
		text,
		Path(),
		QString(),
		0,
		{ 9 * 60, 17 * 60, u"work"_q },
		Rule(true, { 1, 5 }, 8 * 60 + 30, 16 * 60, u"play"_q));
	CHECK(edited.ok());
	CHECK(edited.changed);

	// Every value is rewritten where it stands, so the spacing and the comment
	// beside one survive; the multi-line array comes back as the one line it
	// now needs.
	CHECK(edited.text.contains(u"days   = [\"mon\", \"fri\"]"_q));
	CHECK(edited.text.contains(u"from   = \"08:30\""_q));
	CHECK(edited.text.contains(u"to     = \"16:00\""_q));
	CHECK(edited.text.contains(u"preset = \"play\"   # the important one"_q));
	CHECK(edited.text.contains(u"# my settings"_q));
	CHECK(edited.text.contains(u"# when work happens"_q));
	CHECK(edited.text.contains(u"# the weekday window"_q));
	CHECK(edited.text.contains(u"[peek]"_q));

	// 'enabled_p' was not in that block at all, so it joins the end of it.
	CHECK(edited.text.contains(u"preset = \"play\"   # the important one\n"
		"enabled_p = true"_q));

	const auto back = Parse(edited.text);
	CHECK(back.ok());
	CHECK_EQ(back.settings.schedule.rules.size(), size_t(2));
	CHECK_EQ(back.settings.schedule.rules[0].from, 8 * 60 + 30);
	CHECK_EQ(back.settings.schedule.rules[0].till, 16 * 60);
	CHECK_EQ(back.settings.schedule.rules[0].preset, u"play"_q);
	CHECK_EQ(int(back.settings.schedule.rules[0].days.size()), 2);
	CHECK_EQ(back.settings.schedule.rules[0].days[1], 5);
	CHECK_EQ(back.settings.schedule.rules[1].from, 20 * 60);
	CHECK_EQ(back.settings.schedule.rules[1].sourceIndex, 2);

	// Writing what is already there writes nothing.
	const auto same = Purple::SetScheduleRule(
		text,
		Path(),
		QString(),
		2,
		{ 20 * 60, 23 * 60, u"play"_q },
		Rule(false, { 6 }, 20 * 60, 23 * 60, u"play"_q));
	CHECK(same.ok());
	CHECK(!same.changed);
	CHECK_EQ(same.text, text);

	// The rule the screen read is not the rule in the file any more, so the
	// edit is refused rather than landing on whatever is there now.
	const auto stale = Purple::SetScheduleRule(
		text,
		Path(),
		QString(),
		0,
		{ 8 * 60, 17 * 60, u"work"_q },
		Rule(true, { 1 }, 9 * 60, 10 * 60, u"work"_q));
	CHECK(!stale.ok());
	CHECK(!stale.changed);
	CHECK_EQ(stale.text, text);
	CHECK(stale.error.contains(u"changed underneath"_q));

	const auto gone = Purple::SetScheduleRule(
		text,
		Path(),
		QString(),
		7,
		{ 9 * 60, 17 * 60, u"work"_q },
		Rule(true, { 1 }, 9 * 60, 10 * 60, u"work"_q));
	CHECK(!gone.ok());
	CHECK_EQ(gone.text, text);

	// The broken rule in the middle is addressable like any other, and fixing
	// it leaves the rules on either side of it alone.
	const auto repaired = Purple::SetScheduleRule(
		text,
		Path(),
		QString(),
		1,
		{ 10 * 60, -1, u"play"_q },
		Rule(true, { 3 }, 10 * 60, 11 * 60, u"play"_q));
	CHECK(repaired.ok());
	CHECK(repaired.changed);
	CHECK(repaired.text.contains(u"to = \"11:00\""_q));
	const auto whole = Parse(repaired.text);
	CHECK(whole.ok());
	CHECK_EQ(whole.settings.schedule.rules.size(), size_t(3));
	CHECK_EQ(whole.settings.schedule.rules[0].from, 9 * 60);
	CHECK_EQ(whole.settings.schedule.rules[1].sourceIndex, 1);
	CHECK_EQ(whole.settings.schedule.rules[1].till, 11 * 60);
	CHECK_EQ(whole.settings.schedule.rules[2].sourceIndex, 2);
	CHECK_EQ(whole.settings.schedule.rules[2].from, 20 * 60);

	// A rule written inline is a table the app would have to re-serialise, so
	// it says so, with the line to go and look at.
	const auto inlined = u"[presets.work]\nlist_order = []\n\n[schedule]\n"
		"rules = [{ from = \"09:00\", to = \"10:00\", preset = \"work\" }]\n"_q;
	const auto refused = Purple::SetScheduleRule(
		inlined,
		Path(),
		QString(),
		0,
		{ 9 * 60, 10 * 60, u"work"_q },
		Rule(true, { 1 }, 9 * 60, 11 * 60, u"work"_q));
	CHECK(!refused.ok());
	CHECK(refused.error.contains(u"inline"_q));
	CHECK(refused.error.contains(u"line 5"_q));
	CHECK_EQ(refused.text, inlined);

	// And a rule the parser would throw away is refused before it is written,
	// rather than being saved into a file that then drops it.
	const auto empty = Purple::SetScheduleRule(
		text,
		Path(),
		QString(),
		0,
		{ 9 * 60, 17 * 60, u"work"_q },
		Rule(true, { 1 }, 9 * 60, 9 * 60, u"work"_q));
	CHECK(!empty.ok());
	CHECK(empty.error.contains(u"same time"_q));
	CHECK_EQ(empty.text, text);

	// A rewritten value keeps the carriage return that ends its line, which is
	// not part of the value however much it looks like one.
	const auto windows = u"[presets.work]\r\nlist_order = []\r\n\r\n"
		"[schedule]\r\n[[schedule.rules]]\r\ndays = [\"mon\"]\r\n"
		"from = \"09:00\"\r\nto = \"17:00\"\r\npreset = \"work\"\r\n"_q;
	const auto rewritten = Purple::SetScheduleRule(
		windows,
		Path(),
		QString(),
		0,
		{ 9 * 60, 17 * 60, u"work"_q },
		Rule(true, { 2 }, 10 * 60, 18 * 60, u"work"_q));
	CHECK(rewritten.ok());
	CHECK(rewritten.text.contains(u"from = \"10:00\"\r\n"_q));
	CHECK(rewritten.text.contains(u"days = [\"tue\"]\r\n"_q));
	CHECK(rewritten.text.contains(u"enabled_p = true\r\n"_q));
	CHECK(!rewritten.text.contains(u"\n\n"_q));
	CHECK_EQ(Parse(rewritten.text).settings.schedule.rules.size(), size_t(1));
	CHECK_EQ(Parse(rewritten.text).settings.schedule.rules[0].till, 18 * 60);

	// A file mid-edit is left exactly as it is.
	const auto broken = Purple::SetScheduleRule(
		u"[schedule\nenabled_p = true"_q,
		Path(),
		QString(),
		0,
		{ 9 * 60, 17 * 60, u"work"_q },
		Rule(true, { 1 }, 9 * 60, 10 * 60, u"work"_q));
	CHECK(!broken.ok());
	CHECK_EQ(broken.text, u"[schedule\nenabled_p = true"_q);
}

void TestSpliceScheduleAppend() {
	Begin("splice schedule append");

	const auto text = ScheduleExample();
	const auto added = Purple::AppendScheduleRule(
		text,
		Path(),
		QString(),
		Rule(true, { 7 }, 18 * 60, 20 * 60, u"work"_q));
	CHECK(added.ok());
	CHECK(added.changed);
	CHECK(added.text.contains(u"[[schedule.rules]]\nenabled_p = true\n"
		"days      = [\"sun\"]\nfrom      = \"18:00\"\nto        = \"20:00\"\n"
		"preset    = \"work\"\n"_q));

	// It goes after the last rule and before whatever section came next, which
	// is where a reader of the schedule will find it.
	CHECK(added.text.indexOf(u"18:00"_q) > added.text.indexOf(u"20:00\"\n"
		"preset    = \"play\""_q));
	CHECK(added.text.indexOf(u"[peek]"_q) > added.text.indexOf(u"18:00"_q));
	CHECK(added.text.contains(u"# the weekday window"_q));
	CHECK(added.text.contains(u"# the important one"_q));

	const auto back = Parse(added.text);
	CHECK(back.ok());
	CHECK_EQ(back.settings.schedule.rules.size(), size_t(3));
	CHECK_EQ(back.settings.schedule.rules[2].sourceIndex, 3);
	CHECK_EQ(back.settings.schedule.rules[2].from, 18 * 60);
	CHECK_EQ(back.settings.schedule.rules[2].preset, u"work"_q);

	// A [schedule] with no rules yet takes the first one under its own keys.
	const auto bare = u"[presets.work]\nlist_order = []\n\n[schedule]\n"
		"# off for now\nenabled_p = false\n\n[peek]\nhotkey = \"Ctrl+K\"\n"_q;
	const auto first = Purple::AppendScheduleRule(
		bare,
		Path(),
		QString(),
		Rule(true, { 1 }, 9 * 60, 17 * 60, u"work"_q));
	CHECK(first.ok());
	CHECK(first.text.contains(u"# off for now\nenabled_p = false\n\n"
		"[[schedule.rules]]"_q));
	CHECK(first.text.contains(u"\n\n[peek]"_q));
	CHECK(Parse(first.text).ok());
	CHECK_EQ(Parse(first.text).settings.schedule.rules.size(), size_t(1));

	// No schedule at all: the section goes in with the rule.
	const auto none = u"[presets.work]\nlist_order = []\n"_q;
	const auto started = Purple::AppendScheduleRule(
		none,
		Path(),
		QString(),
		Rule(true, { 1 }, 9 * 60, 17 * 60, u"work"_q));
	CHECK(started.ok());
	CHECK(started.text.startsWith(none));
	CHECK(started.text.contains(u"\n[schedule]\n[[schedule.rules]]\n"_q));
	CHECK(Parse(started.text).ok());
	CHECK_EQ(Parse(started.text).settings.schedule.rules.size(), size_t(1));

	// An empty file gains no leading blank line.
	const auto fresh = Purple::AppendScheduleRule(
		QString(),
		Path(),
		QString(),
		Rule(true, { 1 }, 9 * 60, 17 * 60, u"work"_q));
	CHECK(fresh.ok());
	CHECK(fresh.text.startsWith(u"[schedule]\n[[schedule.rules]]\n"_q));
	CHECK(fresh.text.endsWith(u"preset    = \"work\"\n"_q));

	// The refusals: an inline [schedule], and a rule the parser would drop.
	const auto inlined = u"schedule = { enabled_p = true }\n"_q;
	const auto refused = Purple::AppendScheduleRule(
		inlined,
		Path(),
		QString(),
		Rule(true, { 1 }, 9 * 60, 17 * 60, u"work"_q));
	CHECK(!refused.ok());
	CHECK(refused.error.contains(u"inline"_q));
	CHECK(refused.error.contains(u"line 1"_q));
	CHECK_EQ(refused.text, inlined);

	// A file with Windows endings keeps them, including on the lines we write.
	const auto crlf = u"[presets.work]\r\nlist_order = []\r\n\r\n"
		"[schedule]\r\nenabled_p = true\r\n"_q;
	const auto kept = Purple::AppendScheduleRule(
		crlf,
		Path(),
		QString(),
		Rule(true, { 1 }, 9 * 60, 17 * 60, u"work"_q));
	CHECK(kept.ok());
	CHECK(kept.text.contains(u"[[schedule.rules]]\r\nenabled_p = true\r\n"_q));
	CHECK(kept.text.contains(u"preset    = \"work\"\r\n"_q));
	CHECK(!kept.text.contains(u"\n\n"_q));
	CHECK_EQ(Parse(kept.text).settings.schedule.rules.size(), size_t(1));

	const auto nameless = Purple::AppendScheduleRule(
		text,
		Path(),
		QString(),
		Rule(true, { 1 }, 9 * 60, 17 * 60, QString()));
	CHECK(!nameless.ok());
	CHECK(nameless.error.contains(u"needs a preset"_q));
	CHECK_EQ(nameless.text, text);
}

void TestSpliceScheduleRemove() {
	Begin("splice schedule remove");

	const auto text = ScheduleExample();
	const auto first = Purple::RemoveScheduleRule(
		text,
		Path(),
		QString(),
		0,
		{ 9 * 60, 17 * 60, u"work"_q });
	CHECK(first.ok());
	CHECK(first.changed);
	CHECK(!first.text.contains(u"17:00"_q));

	// The comment above the block it took out stays - it is the user's, and
	// nothing here can tell whether it was about the rule or about the section.
	// So does the blank line above the block that follows.
	CHECK(first.text.contains(u"# the weekday window\n\n[[schedule.rules]]\n"
		"from   = \"10:00\""_q));
	CHECK(first.text.contains(u"# when work happens"_q));
	CHECK(first.text.contains(u"[peek]"_q));

	const auto back = Parse(first.text);
	CHECK(back.ok());
	CHECK_EQ(back.settings.schedule.rules.size(), size_t(1));
	CHECK_EQ(back.settings.schedule.rules[0].sourceIndex, 1);
	CHECK_EQ(back.settings.schedule.rules[0].from, 20 * 60);

	// The last rule leaves the section that follows it where it was.
	const auto last = Purple::RemoveScheduleRule(
		text,
		Path(),
		QString(),
		2,
		{ 20 * 60, 23 * 60, u"play"_q });
	CHECK(last.ok());
	CHECK(last.text.contains(u"preset = \"play\"\n\n[peek]"_q));
	CHECK(!last.text.contains(u"23:00"_q));
	CHECK_EQ(Parse(last.text).settings.schedule.rules.size(), size_t(1));

	// The only rule there was leaves the section itself standing.
	const auto single = u"[presets.work]\nlist_order = []\n\n[schedule]\n"
		"[[schedule.rules]]\nfrom = \"09:00\"\nto = \"10:00\"\n"
		"preset = \"work\"\n"_q;
	const auto emptied = Purple::RemoveScheduleRule(
		single,
		Path(),
		QString(),
		0,
		{ 9 * 60, 10 * 60, u"work"_q });
	CHECK(emptied.ok());
	CHECK_EQ(
		emptied.text,
		u"[presets.work]\nlist_order = []\n\n[schedule]\n"_q);
	CHECK(Parse(emptied.text).settings.schedule.rules.empty());
	CHECK(Parse(emptied.text).settings.schedule.enabled);

	// The same two refusals as the other ops.
	const auto stale = Purple::RemoveScheduleRule(
		text,
		Path(),
		QString(),
		2,
		{ 21 * 60, 23 * 60, u"play"_q });
	CHECK(!stale.ok());
	CHECK(stale.error.contains(u"changed underneath"_q));
	CHECK_EQ(stale.text, text);

	const auto gone = Purple::RemoveScheduleRule(
		text,
		Path(),
		QString(),
		9,
		{ 20 * 60, 23 * 60, u"play"_q });
	CHECK(!gone.ok());
	CHECK_EQ(gone.text, text);

	const auto inlined = u"[schedule]\nrules = [{ from = \"09:00\", "
		"to = \"10:00\", preset = \"work\" }]\n"_q;
	const auto refused = Purple::RemoveScheduleRule(
		inlined,
		Path(),
		QString(),
		0,
		{ 9 * 60, 10 * 60, u"work"_q });
	CHECK(!refused.ok());
	CHECK(refused.error.contains(u"inline"_q));
	CHECK_EQ(refused.text, inlined);
}

// One file describing three devices: a flat array that predates rulesets, a
// ruleset for phones, a ruleset that runs everywhere alongside whatever wins,
// and one for a laptop nobody in these tests is holding.
[[nodiscard]] QString RulesetsExample() {
	return uR"(# my settings

[presets.work]
list_order = []

[presets.home]
list_order = []

[schedule]
# when work happens
enabled_p = true

# the rules everybody had before rulesets existed
[[schedule.rules]]
days   = ["mon"]
from   = "09:00"
to     = "17:00"
preset = "work"   # the important one

# the phone works longer hours
[[schedule.rulesets]]
name    = "phone"
device  = "mobile"
outside = "home"

[[schedule.rulesets.rules]]
days   = ["mon"]
from   = "08:00"
to     = "18:00"
preset = "work"

[[schedule.rulesets]]
name = "quiet"
mode = "always"

[[schedule.rulesets.rules]]
days   = ["mon"]
from   = "22:00"
to     = "07:00"
preset = "home"

[[schedule.rulesets]]
name   = "the laptop"
device = "mac-3f9a"

[[schedule.rulesets.rules]]
days   = ["mon"]
from   = "10:00"
to     = "11:00"
preset = "home"

[peek]
hotkey = "Ctrl+Alt+K"
)"_q;
}

[[nodiscard]] Purple::DeviceIdentity Device(
		const QString &id,
		const QString &platform,
		const QString &cls) {
	auto result = Purple::DeviceIdentity();
	result.id = id;
	result.platform = platform;
	result.cls = cls;
	return result;
}

// Budgets with everything the splicer has to survive: a comment above a block
// and beside a value, keys set away from their defaults, and a budget the
// parser throws away sitting in the middle of the ones it keeps.
[[nodiscard]] QString BudgetExample() {
	return uR"(# my settings

[presets.work]
list_order = []

[screen_time]
# the log is on
enabled_p = true

# the noisy one
[[screen_time.budgets]]
target  = "chat:7"
per_day = "30m"
mode    = "hard"   # behind a cover
snooze  = "10m"

[[screen_time.budgets]]
target  = "kind:bananas"
per_day = "1h"

[[screen_time.budgets]]
target          = "preset:work"
per_day         = "2h"
snoozes_per_day = 0

[peek]
hotkey = "Ctrl+Alt+K"
)"_q;
}

[[nodiscard]] Purple::ScreenTimeBudget Budget(
		const QString &target,
		int perDaySeconds) {
	auto result = Purple::ScreenTimeBudget();
	result.target = target;
	result.perDaySeconds = perDaySeconds;
	return result;
}

void TestBudgetIdentity() {
	Begin("budget identity");

	const auto parsed = Parse(BudgetExample());
	CHECK(parsed.ok());
	const auto &budgets = parsed.settings.screenTime.budgets;

	// The middle budget names a kind that is not one, so the parser drops it -
	// and the budget after it keeps the index it has in the file rather than
	// moving up one. That is the whole reason the index is recorded: an edit
	// made from a screen has to land on the budget the user was looking at.
	CHECK_EQ(budgets.size(), size_t(2));
	CHECK_EQ(budgets[0].sourceIndex, 0);
	CHECK_EQ(budgets[1].sourceIndex, 2);
	CHECK_EQ(budgets[1].preset, u"work"_q);
	CHECK_EQ(budgets[0].snoozeSeconds, 10 * 60);
	CHECK_EQ(budgets[0].snoozesPerDay, 2);
	CHECK_EQ(budgets[1].snoozesPerDay, 0);

	// A budget that came from nowhere says so.
	CHECK_EQ(Purple::ScreenTimeBudget().sourceIndex, -1);
}

void TestSpliceBudgetAppend() {
	Begin("splice budget append");

	const auto text = BudgetExample();
	auto added = Budget(u"kind:groups"_q, 45 * 60);
	const auto first = Purple::AppendBudget(text, Path(), added);
	CHECK(first.ok());
	CHECK(first.changed);

	// Everything at its default is left out of the file: a budget that spells
	// out what it would have meant anyway is harder to read for no gain.
	CHECK(first.text.contains(u"[[screen_time.budgets]]\n"
		"target  = \"kind:groups\"\nper_day = \"45m\"\n"_q));
	CHECK(!first.text.contains(u"soft"_q));

	// It goes after the last budget and before whatever section came next.
	CHECK(first.text.indexOf(u"kind:groups"_q)
		> first.text.indexOf(u"snoozes_per_day = 0"_q));
	CHECK(first.text.indexOf(u"[peek]"_q)
		> first.text.indexOf(u"kind:groups"_q));
	CHECK(first.text.contains(u"# the noisy one"_q));
	CHECK(first.text.contains(u"# behind a cover"_q));
	CHECK(first.text.contains(u"# the log is on"_q));

	const auto back = Parse(first.text);
	CHECK(back.ok());
	CHECK_EQ(back.settings.screenTime.budgets.size(), size_t(3));
	CHECK_EQ(back.settings.screenTime.budgets[2].sourceIndex, 3);
	CHECK_EQ(back.settings.screenTime.budgets[2].perDaySeconds, 45 * 60);
	CHECK_EQ(back.settings.screenTime.budgets[2].snoozesPerDay, 2);
	CHECK(back.settings.screenTime.budgets[2].mode
		== Purple::BudgetMode::Soft);

	// A key away from its default is written, and the keys line up on the
	// widest one that is actually there.
	auto strict = Budget(u"all"_q, 2 * 60 * 60);
	strict.mode = Purple::BudgetMode::Hard;
	strict.snoozeSeconds = 90;
	strict.snoozesPerDay = 0;
	const auto full = Purple::AppendBudget(text, Path(), strict);
	CHECK(full.ok());
	CHECK(full.text.contains(u"[[screen_time.budgets]]\n"
		"target          = \"all\"\nper_day         = \"2h\"\n"
		"mode            = \"hard\"\nsnooze          = \"90s\"\n"
		"snoozes_per_day = 0\n"_q));
	CHECK_EQ(Parse(full.text).settings.screenTime.budgets[2].snoozeSeconds, 90);

	// A [screen_time] with no budgets yet takes the first one under its own
	// keys, which is where somebody reading it looks.
	const auto bare = u"[presets.work]\nlist_order = []\n\n[screen_time]\n"
		"# off for now\nenabled_p = false\n\n[peek]\nhotkey = \"Ctrl+K\"\n"_q;
	const auto under = Purple::AppendBudget(
		bare,
		Path(),
		Budget(u"all"_q, 60 * 60));
	CHECK(under.ok());
	CHECK(under.text.contains(u"# off for now\nenabled_p = false\n\n"
		"[[screen_time.budgets]]"_q));
	CHECK(under.text.contains(u"\n\n[peek]"_q));
	CHECK_EQ(Parse(under.text).settings.screenTime.budgets.size(), size_t(1));

	// No screen time at all: the section goes in with the budget.
	const auto none = u"[presets.work]\nlist_order = []\n"_q;
	const auto started = Purple::AppendBudget(
		none,
		Path(),
		Budget(u"chat:5"_q, 15 * 60));
	CHECK(started.ok());
	CHECK(started.text.startsWith(none));
	CHECK(started.text.contains(
		u"\n[screen_time]\n[[screen_time.budgets]]\n"_q));
	CHECK(Parse(started.text).ok());
	CHECK_EQ(Parse(started.text).settings.screenTime.budgets.size(), size_t(1));
	CHECK_EQ(Parse(started.text).settings.screenTime.budgets[0].chat, 5);

	// An empty file gains no leading blank line.
	const auto fresh = Purple::AppendBudget(
		QString(),
		Path(),
		Budget(u"all"_q, 30 * 60));
	CHECK(fresh.ok());
	CHECK(fresh.text.startsWith(
		u"[screen_time]\n[[screen_time.budgets]]\n"_q));
	CHECK(fresh.text.endsWith(u"per_day = \"30m\"\n"_q));

	// A file with Windows endings keeps them, including on the lines we write.
	const auto crlf = u"[presets.work]\r\nlist_order = []\r\n\r\n"
		"[screen_time]\r\nenabled_p = true\r\n"_q;
	const auto kept = Purple::AppendBudget(
		crlf,
		Path(),
		Budget(u"all"_q, 60 * 60));
	CHECK(kept.ok());
	CHECK(kept.text.contains(u"[[screen_time.budgets]]\r\n"
		"target  = \"all\"\r\nper_day = \"1h\"\r\n"_q));
	CHECK(!kept.text.contains(u"\n\n"_q));
	CHECK_EQ(Parse(kept.text).settings.screenTime.budgets.size(), size_t(1));

	// The refusals: an inline [screen_time], a budgets array the file wrote out
	// in full, a target with nothing in it, and one the parser would drop.
	const auto inlined = u"screen_time = { enabled_p = true }\n"_q;
	const auto refused = Purple::AppendBudget(
		inlined,
		Path(),
		Budget(u"all"_q, 60 * 60));
	CHECK(!refused.ok());
	CHECK(refused.error.contains(u"inline"_q));
	CHECK(refused.error.contains(u"line 1"_q));
	CHECK_EQ(refused.text, inlined);

	const auto literal = u"[screen_time]\nbudgets = []\n"_q;
	const auto array = Purple::AppendBudget(
		literal,
		Path(),
		Budget(u"all"_q, 60 * 60));
	CHECK(!array.ok());
	CHECK(array.error.contains(u"empty array"_q));
	CHECK_EQ(array.text, literal);

	const auto nameless = Purple::AppendBudget(
		text,
		Path(),
		Budget(u"   "_q, 60 * 60));
	CHECK(!nameless.ok());
	CHECK(nameless.error.contains(u"needs a target"_q));
	CHECK_EQ(nameless.text, text);

	const auto nonsense = Purple::AppendBudget(
		text,
		Path(),
		Budget(u"chat:banana"_q, 60 * 60));
	CHECK(!nonsense.ok());
	CHECK(nonsense.error.contains(u"would not read back"_q));
	CHECK_EQ(nonsense.text, text);

	// A file mid-edit is left exactly as it is.
	const auto broken = Purple::AppendBudget(
		u"[screen_time\nenabled_p = true"_q,
		Path(),
		Budget(u"all"_q, 60 * 60));
	CHECK(!broken.ok());
	CHECK_EQ(broken.text, u"[screen_time\nenabled_p = true"_q);
}

void TestSpliceBudgetSet() {
	Begin("splice budget set");

	const auto text = BudgetExample();
	auto edited = Budget(u"chat:7"_q, 20 * 60);
	edited.mode = Purple::BudgetMode::Hard;
	edited.snoozeSeconds = 10 * 60;
	const auto changed = Purple::SetBudget(
		text,
		Path(),
		0,
		u"chat:7"_q,
		edited);
	CHECK(changed.ok());
	CHECK(changed.changed);

	// The value is rewritten where it stands, so the spacing and the comment
	// beside one survive.
	CHECK(changed.text.contains(u"per_day = \"20m\""_q));
	CHECK(changed.text.contains(u"mode    = \"hard\"   # behind a cover"_q));
	CHECK(changed.text.contains(u"# the noisy one"_q));
	CHECK(changed.text.contains(u"[peek]"_q));
	CHECK(changed.text.contains(u"snoozes_per_day = 0"_q));
	CHECK_EQ(Parse(changed.text).settings.screenTime.budgets[0].perDaySeconds,
		20 * 60);

	// Writing what is already there writes nothing.
	auto same = Budget(u"preset:work"_q, 2 * 60 * 60);
	same.snoozesPerDay = 0;
	const auto quiet = Purple::SetBudget(
		text,
		Path(),
		2,
		u"preset:work"_q,
		same);
	CHECK(quiet.ok());
	CHECK(!quiet.changed);
	CHECK_EQ(quiet.text, text);

	// Everything back to its default takes those keys out of the file: a
	// screen says "the default" by not writing it down.
	const auto plain = Purple::SetBudget(
		text,
		Path(),
		0,
		u"chat:7"_q,
		Budget(u"chat:7"_q, 30 * 60));
	CHECK(plain.ok());
	CHECK(plain.changed);
	CHECK(!plain.text.contains(u"mode"_q));
	CHECK(!plain.text.contains(u"# behind a cover"_q));
	CHECK(!plain.text.contains(u"snooze  ="_q));
	CHECK(plain.text.contains(u"# the noisy one\n[[screen_time.budgets]]\n"
		"target  = \"chat:7\"\nper_day = \"30m\"\n\n"_q));
	const auto stripped = Parse(plain.text);
	CHECK(stripped.ok());
	CHECK_EQ(stripped.settings.screenTime.budgets.size(), size_t(2));
	CHECK(stripped.settings.screenTime.budgets[0].mode
		== Purple::BudgetMode::Soft);
	CHECK_EQ(stripped.settings.screenTime.budgets[0].snoozeSeconds, 5 * 60);
	CHECK_EQ(stripped.settings.screenTime.budgets[0].snoozesPerDay, 2);

	// And back again: a key the block no longer has joins the end of it.
	auto restored = Budget(u"chat:7"_q, 30 * 60);
	restored.mode = Purple::BudgetMode::Hard;
	restored.snoozeSeconds = 60;
	restored.snoozesPerDay = 1;
	const auto again = Purple::SetBudget(
		plain.text,
		Path(),
		0,
		u"chat:7"_q,
		restored);
	CHECK(again.ok());
	CHECK(again.text.contains(u"target  = \"chat:7\"\nper_day = \"30m\"\n"
		"mode = \"hard\"\nsnooze = \"1m\"\nsnoozes_per_day = 1\n"_q));
	const auto whole = Parse(again.text);
	CHECK(whole.ok());
	CHECK_EQ(whole.settings.screenTime.budgets.size(), size_t(2));
	CHECK(whole.settings.screenTime.budgets[0].mode
		== Purple::BudgetMode::Hard);
	CHECK_EQ(whole.settings.screenTime.budgets[0].snoozeSeconds, 60);
	CHECK_EQ(whole.settings.screenTime.budgets[0].snoozesPerDay, 1);

	// The broken budget in the middle is addressable like any other, and fixing
	// it leaves the budgets on either side of it alone.
	const auto repaired = Purple::SetBudget(
		text,
		Path(),
		1,
		u"kind:bananas"_q,
		Budget(u"kind:channels"_q, 60 * 60));
	CHECK(repaired.ok());
	CHECK(repaired.text.contains(u"target  = \"kind:channels\""_q));
	const auto three = Parse(repaired.text);
	CHECK(three.ok());
	CHECK_EQ(three.settings.screenTime.budgets.size(), size_t(3));
	CHECK_EQ(three.settings.screenTime.budgets[0].sourceIndex, 0);
	CHECK_EQ(three.settings.screenTime.budgets[1].sourceIndex, 1);
	CHECK_EQ(three.settings.screenTime.budgets[2].sourceIndex, 2);
	CHECK_EQ(three.settings.screenTime.budgets[0].chat, 7);
	CHECK_EQ(three.settings.screenTime.budgets[2].preset, u"work"_q);

	// The budget the screen read is not the budget in the file any more, so the
	// edit is refused rather than landing on whatever is there now.
	const auto stale = Purple::SetBudget(
		text,
		Path(),
		2,
		u"chat:7"_q,
		Budget(u"chat:7"_q, 60 * 60));
	CHECK(!stale.ok());
	CHECK(!stale.changed);
	CHECK_EQ(stale.text, text);
	CHECK(stale.error.contains(u"changed underneath"_q));

	const auto gone = Purple::SetBudget(
		text,
		Path(),
		9,
		u"chat:7"_q,
		Budget(u"chat:7"_q, 60 * 60));
	CHECK(!gone.ok());
	CHECK_EQ(gone.text, text);

	// A budget written inline is a table the app would have to re-serialise, so
	// it says so, with the line to go and look at.
	const auto inlined = u"[screen_time]\nbudgets = [{ target = \"all\", "
		"per_day = \"1h\" }]\n"_q;
	const auto refused = Purple::SetBudget(
		inlined,
		Path(),
		0,
		u"all"_q,
		Budget(u"all"_q, 30 * 60));
	CHECK(!refused.ok());
	CHECK(refused.error.contains(u"inline"_q));
	CHECK(refused.error.contains(u"line 2"_q));
	CHECK_EQ(refused.text, inlined);

	// And a budget the parser would throw away is refused before it is written,
	// rather than being saved into a file that then drops it.
	const auto nonsense = Purple::SetBudget(
		text,
		Path(),
		0,
		u"chat:7"_q,
		Budget(u"kind:bananas"_q, 30 * 60));
	CHECK(!nonsense.ok());
	CHECK(nonsense.error.contains(u"would not read back"_q));
	CHECK_EQ(nonsense.text, text);

	// A rewritten value keeps the carriage return that ends its line.
	const auto crlf = u"[screen_time]\r\n[[screen_time.budgets]]\r\n"
		"target = \"all\"\r\nper_day = \"1h\"\r\n"_q;
	const auto windows = Purple::SetBudget(
		crlf,
		Path(),
		0,
		u"all"_q,
		Budget(u"all"_q, 30 * 60));
	CHECK(windows.ok());
	CHECK(windows.text.contains(u"per_day = \"30m\"\r\n"_q));
	CHECK(!windows.text.contains(u"\n\n"_q));
	CHECK_EQ(
		Parse(windows.text).settings.screenTime.budgets[0].perDaySeconds,
		30 * 60);
}

void TestSpliceBudgetRemove() {
	Begin("splice budget remove");

	const auto text = BudgetExample();
	const auto first = Purple::RemoveBudget(text, Path(), 0, u"chat:7"_q);
	CHECK(first.ok());
	CHECK(first.changed);
	CHECK(!first.text.contains(u"chat:7"_q));
	CHECK(!first.text.contains(u"# behind a cover"_q));

	// The comment above the block it took out stays - it is the user's, and
	// nothing here can tell whether it was about the budget or about the
	// section. So does the blank line above the block that follows.
	CHECK(first.text.contains(u"# the noisy one\n\n[[screen_time.budgets]]\n"
		"target  = \"kind:bananas\""_q));
	CHECK(first.text.contains(u"# the log is on"_q));
	CHECK(first.text.contains(u"[peek]"_q));

	const auto back = Parse(first.text);
	CHECK(back.ok());
	CHECK_EQ(back.settings.screenTime.budgets.size(), size_t(1));
	CHECK_EQ(back.settings.screenTime.budgets[0].sourceIndex, 1);
	CHECK_EQ(back.settings.screenTime.budgets[0].preset, u"work"_q);

	// The broken one in the middle goes by the same address as any other, and
	// the two it sat between keep theirs.
	const auto middle = Purple::RemoveBudget(
		text,
		Path(),
		1,
		u"kind:bananas"_q);
	CHECK(middle.ok());
	CHECK(!middle.text.contains(u"bananas"_q));
	const auto two = Parse(middle.text);
	CHECK(two.ok());
	CHECK_EQ(two.settings.screenTime.budgets.size(), size_t(2));
	CHECK_EQ(two.settings.screenTime.budgets[0].sourceIndex, 0);
	CHECK_EQ(two.settings.screenTime.budgets[1].sourceIndex, 1);
	CHECK_EQ(two.settings.screenTime.budgets[1].preset, u"work"_q);

	// The last budget leaves the section that follows it where it was.
	const auto last = Purple::RemoveBudget(
		text,
		Path(),
		2,
		u"preset:work"_q);
	CHECK(last.ok());
	CHECK(last.text.contains(u"per_day = \"1h\"\n\n[peek]"_q));
	CHECK(!last.text.contains(u"preset:work"_q));
	CHECK_EQ(Parse(last.text).settings.screenTime.budgets.size(), size_t(1));

	// The only budget there was leaves the section itself standing.
	const auto single = u"[presets.work]\nlist_order = []\n\n[screen_time]\n"
		"enabled_p = true\n\n[[screen_time.budgets]]\ntarget = \"all\"\n"
		"per_day = \"1h\"\n"_q;
	const auto emptied = Purple::RemoveBudget(single, Path(), 0, u"all"_q);
	CHECK(emptied.ok());
	CHECK_EQ(
		emptied.text,
		u"[presets.work]\nlist_order = []\n\n[screen_time]\n"
			"enabled_p = true\n"_q);
	CHECK(Parse(emptied.text).settings.screenTime.budgets.empty());
	CHECK(Parse(emptied.text).settings.screenTime.enabled);

	// The same refusals as the other ops.
	const auto stale = Purple::RemoveBudget(text, Path(), 2, u"chat:7"_q);
	CHECK(!stale.ok());
	CHECK(stale.error.contains(u"changed underneath"_q));
	CHECK_EQ(stale.text, text);

	const auto gone = Purple::RemoveBudget(text, Path(), 9, u"chat:7"_q);
	CHECK(!gone.ok());
	CHECK_EQ(gone.text, text);

	const auto missing = Purple::RemoveBudget(
		u"[screen_time]\nenabled_p = true\n"_q,
		Path(),
		0,
		u"all"_q);
	CHECK(!missing.ok());
	CHECK(missing.error.contains(u"no [[screen_time.budgets]]"_q));

	const auto inlined = u"[screen_time]\nbudgets = [{ target = \"all\", "
		"per_day = \"1h\" }]\n"_q;
	const auto refused = Purple::RemoveBudget(inlined, Path(), 0, u"all"_q);
	CHECK(!refused.ok());
	CHECK(refused.error.contains(u"inline"_q));
	CHECK_EQ(refused.text, inlined);
}

void TestSpliceRulesets() {
	Begin("splice rulesets");

	const auto text = RulesetsExample();

	// A new ruleset lands after everything the schedule already holds, so the
	// file keeps its schedule in one piece rather than growing a second one at
	// the bottom. Only what differs from the default is written down.
	const auto added = Purple::AddRuleset(
		text,
		Path(),
		u"tablet"_q,
		u"android"_q,
		Purple::RulesetMode::Always);
	CHECK(added.ok());
	CHECK(added.changed);
	CHECK(added.text.contains(u"[[schedule.rulesets]]\nname    = \"tablet\"\n"
		"device  = \"android\"\nmode    = \"always\"\n\n[peek]"_q));
	CHECK(added.text.contains(u"# my settings"_q));
	CHECK(added.text.contains(u"# the phone works longer hours"_q));
	const auto backAdded = Parse(added.text);
	CHECK(backAdded.ok());
	CHECK_EQ(backAdded.settings.schedule.rulesets.size(), size_t(5));
	const auto &tablet = backAdded.settings.schedule.rulesets[4];
	CHECK_EQ(tablet.name, u"tablet"_q);
	CHECK_EQ(tablet.device, u"android"_q);
	CHECK(tablet.mode == Purple::RulesetMode::Always);
	CHECK(tablet.rules.empty());

	// The defaults are left unwritten, because a file that spells out what it
	// would have meant anyway is harder to read for no gain.
	const auto plain = Purple::AddRuleset(
		text,
		Path(),
		u"tablet"_q,
		u"any"_q,
		Purple::RulesetMode::Enabled);
	CHECK(plain.ok());
	CHECK(plain.text.contains(u"[[schedule.rulesets]]\nname    = \"tablet\"\n"_q));
	CHECK(!plain.text.contains(u"mode    = \"enabled\""_q));

	// The name is the address every later edit goes through, so a second
	// ruleset cannot take one that is already in use.
	const auto taken = Purple::AddRuleset(
		text,
		Path(),
		u"PHONE"_q,
		QString(),
		Purple::RulesetMode::Enabled);
	CHECK(!taken.ok());
	CHECK(!taken.changed);
	CHECK(taken.error.contains(u"already a schedule ruleset"_q));

	const auto nameless = Purple::AddRuleset(
		text,
		Path(),
		u"   "_q,
		QString(),
		Purple::RulesetMode::Enabled);
	CHECK(!nameless.ok());
	CHECK(nameless.error.contains(u"needs a name"_q));

	// A file with no schedule at all gains the section along with the ruleset.
	const auto fresh = Purple::AddRuleset(
		u"[presets.work]\nlist_order = []\n"_q,
		Path(),
		u"phone"_q,
		u"mobile"_q,
		Purple::RulesetMode::Enabled);
	CHECK(fresh.ok());
	CHECK(fresh.text.contains(u"[schedule]\n[[schedule.rulesets]]\n"
		"name    = \"phone\"\ndevice  = \"mobile\"\n"_q));
	CHECK(Parse(fresh.text).ok());

	// One key at a time, where it stands, comment and spacing kept.
	const auto moved = Purple::SetRulesetString(
		text,
		Path(),
		u"phone"_q,
		u"outside"_q,
		u"normal"_q);
	CHECK(moved.ok());
	CHECK(moved.text.contains(u"outside = \"normal\""_q));
	CHECK_EQ(moved.text.count('\n'), text.count('\n'));
	CHECK_EQ(
		Parse(moved.text).settings.schedule.rulesets[1].outside.value_or(
			QString()),
		u"normal"_q);

	// A key the ruleset never had joins the end of its own block, above the
	// first of its rules rather than inside it.
	const auto scoped = Purple::SetRulesetString(
		text,
		Path(),
		u"quiet"_q,
		u"device"_q,
		u"desktop"_q);
	CHECK(scoped.ok());
	CHECK(scoped.text.contains(u"name = \"quiet\"\nmode = \"always\"\n"
		"device = \"desktop\"\n\n[[schedule.rulesets.rules]]"_q));
	CHECK_EQ(
		Parse(scoped.text).settings.schedule.rulesets[2].device,
		u"desktop"_q);

	// An empty value takes the key out, which is how a screen says "back to the
	// default" without writing the default down.
	const auto cleared = Purple::SetRulesetString(
		text,
		Path(),
		u"phone"_q,
		u"outside"_q,
		QString());
	CHECK(cleared.ok());
	CHECK(cleared.changed);
	CHECK(!cleared.text.contains(u"outside = \"home\""_q));
	CHECK(cleared.text.contains(u"device  = \"mobile\"\n\n"
		"[[schedule.rulesets.rules]]"_q));
	CHECK(!Parse(cleared.text).settings.schedule.rulesets[1]
		.outside.has_value());

	// Clearing what is not there, and setting what is already set, both write
	// nothing at all.
	const auto twice = Purple::SetRulesetString(
		cleared.text,
		Path(),
		u"phone"_q,
		u"outside"_q,
		QString());
	CHECK(twice.ok());
	CHECK(!twice.changed);
	const auto same = Purple::SetRulesetString(
		text,
		Path(),
		u"phone"_q,
		u"device"_q,
		u"mobile"_q);
	CHECK(same.ok());
	CHECK(!same.changed);
	CHECK_EQ(same.text, text);

	// A rename is allowed; a rename onto an address already in use is not.
	const auto renamed = Purple::SetRulesetString(
		text,
		Path(),
		u"phone"_q,
		u"name"_q,
		u"pocket"_q);
	CHECK(renamed.ok());
	CHECK_EQ(
		Parse(renamed.text).settings.schedule.rulesets[1].name,
		u"pocket"_q);
	const auto collides = Purple::SetRulesetString(
		text,
		Path(),
		u"phone"_q,
		u"name"_q,
		u"quiet"_q);
	CHECK(!collides.ok());
	CHECK(collides.error.contains(u"already a schedule ruleset"_q));

	const auto missing = Purple::SetRulesetString(
		text,
		Path(),
		u"nobody"_q,
		u"device"_q,
		u"mobile"_q);
	CHECK(!missing.ok());
	CHECK(missing.error.contains(u"no schedule ruleset called 'nobody'"_q));

	// A ruleset goes with its rules: they are its rules and mean nothing
	// without it. The comment above it stays, the way it does for a rule.
	const auto gone = Purple::RemoveRuleset(text, Path(), u"phone"_q);
	CHECK(gone.ok());
	CHECK(gone.changed);
	CHECK(!gone.text.contains(u"08:00"_q));
	CHECK(!gone.text.contains(u"\"mobile\""_q));
	CHECK(gone.text.contains(u"# the phone works longer hours\n\n"
		"[[schedule.rulesets]]\nname = \"quiet\""_q));
	CHECK(gone.text.contains(u"[peek]"_q));
	const auto backGone = Parse(gone.text);
	CHECK(backGone.ok());
	CHECK_EQ(backGone.settings.schedule.rulesets.size(), size_t(3));
	CHECK_EQ(backGone.settings.schedule.rulesets[1].name, u"quiet"_q);
	CHECK_EQ(backGone.settings.schedule.rules.size(), size_t(1));

	// The last ruleset in the file leaves the section after it where it was.
	const auto lastGone = Purple::RemoveRuleset(
		text,
		Path(),
		u"the laptop"_q);
	CHECK(lastGone.ok());
	CHECK(lastGone.text.contains(u"preset = \"home\"\n\n[peek]"_q));
	CHECK(!lastGone.text.contains(u"mac-3f9a"_q));

	const auto neverWas = Purple::RemoveRuleset(text, Path(), u"nobody"_q);
	CHECK(!neverWas.ok());
	CHECK_EQ(neverWas.text, text);
}

void TestSpliceRulesetRules() {
	Begin("splice ruleset rules");

	const auto text = RulesetsExample();

	// The same op as for a flat rule, addressed inside a ruleset, and the index
	// counts within that ruleset alone.
	const auto edited = Purple::SetScheduleRule(
		text,
		Path(),
		u"phone"_q,
		0,
		{ 8 * 60, 18 * 60, u"work"_q },
		Rule(true, { 1, 2 }, 7 * 60 + 30, 19 * 60, u"home"_q));
	CHECK(edited.ok());
	CHECK(edited.changed);
	CHECK(edited.text.contains(u"days   = [\"mon\", \"tue\"]"_q));
	CHECK(edited.text.contains(u"from   = \"07:30\""_q));
	CHECK(edited.text.contains(u"to     = \"19:00\""_q));

	// The flat rule and the other rulesets are exactly where they were.
	CHECK(edited.text.contains(u"preset = \"work\"   # the important one"_q));
	CHECK(edited.text.contains(u"# the phone works longer hours"_q));
	CHECK(edited.text.contains(u"name = \"quiet\"\nmode = \"always\""_q));
	const auto back = Parse(edited.text);
	CHECK(back.ok());
	CHECK_EQ(back.settings.schedule.rules.size(), size_t(1));
	CHECK_EQ(back.settings.schedule.rules[0].from, 9 * 60);
	CHECK_EQ(back.settings.schedule.rulesets[1].rules[0].from, 7 * 60 + 30);
	CHECK_EQ(back.settings.schedule.rulesets[1].rules[0].preset, u"home"_q);
	CHECK_EQ(back.settings.schedule.rulesets[2].rules[0].from, 22 * 60);

	// The guard against a stale screen holds inside a ruleset too: the same
	// index in the flat array is a different rule, and it is not touched.
	const auto stale = Purple::SetScheduleRule(
		text,
		Path(),
		u"phone"_q,
		0,
		{ 9 * 60, 17 * 60, u"work"_q },
		Rule(true, { 1 }, 7 * 60, 19 * 60, u"home"_q));
	CHECK(!stale.ok());
	CHECK(!stale.changed);
	CHECK_EQ(stale.text, text);
	CHECK(stale.error.contains(u"schedule ruleset 'phone' rule 1"_q));
	CHECK(stale.error.contains(u"changed underneath"_q));

	const auto nowhere = Purple::SetScheduleRule(
		text,
		Path(),
		u"nobody"_q,
		0,
		{ 8 * 60, 18 * 60, u"work"_q },
		Rule(true, { 1 }, 7 * 60, 19 * 60, u"home"_q));
	CHECK(!nowhere.ok());
	CHECK(nowhere.error.contains(u"no schedule ruleset called 'nobody'"_q));

	// A rule appended to a ruleset goes after that ruleset's last rule, before
	// the next ruleset's header.
	const auto appended = Purple::AppendScheduleRule(
		text,
		Path(),
		u"quiet"_q,
		Rule(true, { 6, 7 }, 12 * 60, 13 * 60, u"home"_q));
	CHECK(appended.ok());
	CHECK(appended.text.contains(u"[[schedule.rulesets.rules]]\n"
		"enabled_p = true\ndays      = [\"sat\", \"sun\"]\n"
		"from      = \"12:00\"\nto        = \"13:00\"\n"
		"preset    = \"home\"\n\n[[schedule.rulesets]]\n"
		"name   = \"the laptop\""_q));
	const auto backAppended = Parse(appended.text);
	CHECK(backAppended.ok());
	CHECK_EQ(backAppended.settings.schedule.rulesets[2].rules.size(), size_t(2));
	CHECK_EQ(backAppended.settings.schedule.rulesets[2].rules[1].from, 12 * 60);
	CHECK_EQ(backAppended.settings.schedule.rulesets[3].name, u"the laptop"_q);
	CHECK_EQ(backAppended.settings.schedule.rules.size(), size_t(1));

	// A ruleset with no rules yet takes its first one under its own keys, which
	// is where somebody reading the file looks for it.
	const auto bare = u"[presets.work]\nlist_order = []\n\n"
		"[[schedule.rulesets]]\nname   = \"phone\"\ndevice = \"mobile\"\n\n"
		"[peek]\nhotkey = \"Ctrl+Alt+K\"\n"_q;
	const auto started = Purple::AppendScheduleRule(
		bare,
		Path(),
		u"phone"_q,
		Rule(true, { 1 }, 9 * 60, 17 * 60, u"work"_q));
	CHECK(started.ok());
	CHECK(started.text.contains(u"device = \"mobile\"\n\n"
		"[[schedule.rulesets.rules]]\nenabled_p = true"_q));
	CHECK(started.text.contains(u"[peek]"_q));
	const auto backStarted = Parse(started.text);
	CHECK(backStarted.ok());
	CHECK_EQ(backStarted.settings.schedule.rulesets[0].rules.size(), size_t(1));

	// And taking one out leaves the ruleset and everything after it alone.
	const auto removed = Purple::RemoveScheduleRule(
		text,
		Path(),
		u"phone"_q,
		0,
		{ 8 * 60, 18 * 60, u"work"_q });
	CHECK(removed.ok());
	CHECK(removed.changed);
	CHECK(!removed.text.contains(u"08:00"_q));
	CHECK(removed.text.contains(u"outside = \"home\"\n\n"
		"[[schedule.rulesets]]\nname = \"quiet\""_q));
	const auto backRemoved = Parse(removed.text);
	CHECK(backRemoved.ok());
	CHECK(backRemoved.settings.schedule.rulesets[1].rules.empty());
	CHECK_EQ(backRemoved.settings.schedule.rulesets[2].rules.size(), size_t(1));
	CHECK_EQ(backRemoved.settings.schedule.rules.size(), size_t(1));
}

void TestSetTableBoolImplicitHeader() {
	Begin("set table bool implicit header");

	// A file with rules but no [schedule] line of its own. toml++ still hands
	// back a table, pointing at the first rule - so the key has to bring the
	// missing header with it rather than being filed inside that rule.
	const auto text = u"# rules only\n\n[presets.work]\nlist_order = []\n\n"
		"[[schedule.rules]]\ndays   = [\"mon\"]\nfrom   = \"09:00\"\n"
		"to     = \"17:00\"\npreset = \"work\"\n"_q;
	const auto off = Purple::SetTableBool(
		text,
		Path(),
		u"schedule"_q,
		u"enabled_p"_q,
		false);
	CHECK(off.ok());
	CHECK(off.changed);
	CHECK(off.text.contains(u"[schedule]\nenabled_p = false\n\n"
		"[[schedule.rules]]"_q));
	CHECK(off.text.contains(u"# rules only"_q));
	const auto back = Parse(off.text);
	CHECK(back.ok());
	CHECK(!back.settings.schedule.enabled);
	CHECK_EQ(back.settings.schedule.rules.size(), size_t(1));

	// Once the header is there the key is edited in place like any other.
	const auto on = Purple::SetTableBool(
		off.text,
		Path(),
		u"schedule"_q,
		u"enabled_p"_q,
		true);
	CHECK(on.ok());
	CHECK(on.text.contains(u"[schedule]\nenabled_p = true\n"_q));
	CHECK_EQ(on.text.count('\n'), off.text.count('\n'));

	// A table written as dotted keys has no header to insert under, and a
	// header inserted above one would swallow it. So it says what to do.
	const auto dotted = Purple::SetTableBool(
		u"schedule.rules = []\n"_q,
		Path(),
		u"schedule"_q,
		u"enabled_p"_q,
		true);
	CHECK(!dotted.ok());
	CHECK(dotted.error.contains(u"dotted keys"_q));

	// An inline table is refused by name rather than by a parse failure the
	// user cannot act on.
	const auto inlined = Purple::SetTableBool(
		u"premium = { enabled_p = true }\n"_q,
		Path(),
		u"premium"_q,
		u"other_p"_q,
		true);
	CHECK(!inlined.ok());
	CHECK(inlined.error.contains(u"inline"_q));
}

void TestStateRoundTrip() {
	Begin("state round trip");

	auto state = Purple::State();
	state.activePreset = u"work"_q;
	state.activeSource = Purple::PresetSource::Schedule;
	state.previousPreset = u"normal"_q;
	state.previousSource = Purple::PresetSource::Manual;
	state.focusActive = true;
	state.focusSeen = true;
	state.schedulePaused = true;
	state.schedulePausedUntil = 1755500000;
	state.scheduleTarget = u"work"_q;
	state.peekActive = true;
	state.peekDeadlineUnix = 1755400000;
	state.resolvedCache.preset = u"work"_q;
	state.resolvedCache.viewName = u"Deep Work"_q;
	state.resolvedCache.hideEverywhere = true;
	state.resolvedCache.lists = {
		{ u"os"_q, Purple::ShowMode::Always, true },
		{ u"people"_q, Purple::ShowMode::Mention, false },
	};
	state.resolvedCache.folders = {
		{ .name = u"Music"_q, .notify = false },
		{ .name = u"Family"_q, .include = Purple::FolderInclude::All },
		{ .name = Purple::AllFoldersName(), .show = false, },
	};
	state.resolvedCache.views = {
		{
			u"Focus"_q,
			{ 5, 6 },
			{ { u"os"_q, Purple::ShowMode::Always, true } },
		},
	};

	const auto text = Purple::SerializeState(state);
	const auto back = Purple::ParseState(text, u"state.toml"_q);
	CHECK_EQ(back.activePreset, u"work"_q);
	CHECK(back.activeSource == Purple::PresetSource::Schedule);
	CHECK_EQ(back.previousPreset, u"normal"_q);
	CHECK(back.previousSource == Purple::PresetSource::Manual);
	CHECK(back.focusActive);
	CHECK(back.focusSeen);
	CHECK(back.schedulePaused);
	CHECK_EQ(back.schedulePausedUntil, int64(1755500000));
	CHECK_EQ(back.scheduleTarget, u"work"_q);
	CHECK(back.peekActive);
	CHECK_EQ(back.peekDeadlineUnix, int64(1755400000));

	CHECK(back.resolvedCache.valid());
	CHECK_EQ(back.resolvedCache.preset, u"work"_q);
	CHECK_EQ(back.resolvedCache.viewName, u"Deep Work"_q);
	CHECK(back.resolvedCache.hideEverywhere);
	CHECK_EQ(back.resolvedCache.lists.size(), size_t(2));
	CHECK_EQ(back.resolvedCache.lists[1].list, u"people"_q);
	CHECK_EQ(Mode(back.resolvedCache.lists[1].show),
		Mode(Purple::ShowMode::Mention));
	CHECK(!back.resolvedCache.lists[1].notify);
	
	
	CHECK_EQ(int(back.resolvedCache.folders.size()), 3);
	CHECK_EQ(back.resolvedCache.folders[0].name, u"Music"_q);
	CHECK(back.resolvedCache.folders[0].notify.has_value());
	CHECK(!back.resolvedCache.folders[0].notify.value_or(true));
	CHECK(!back.resolvedCache.folders[0].include.has_value());
	CHECK_EQ(back.resolvedCache.folders[1].name, u"Family"_q);
	CHECK_EQ(
		int(back.resolvedCache.folders[1].include.value_or(
			Purple::FolderInclude::None)),
		int(Purple::FolderInclude::All));
	CHECK(Purple::IsAllFolders(back.resolvedCache.folders[2]));
	CHECK(!back.resolvedCache.folders[2].show.value_or(true));

	CHECK_EQ(int(back.resolvedCache.views.size()), 1);
	CHECK_EQ(back.resolvedCache.views[0].name, u"Focus"_q);
	CHECK_EQ(back.resolvedCache.views[0].pinned,
		(std::vector<Purple::PeerIdValue>{ 5, 6 }));
	CHECK_EQ(int(back.resolvedCache.views[0].lists.size()), 1);
	CHECK_EQ(back.resolvedCache.views[0].lists[0].list, u"os"_q);

	// Serialising the round-tripped state reproduces the file exactly, so
	// nothing drifts as the app rewrites it over and over.
	CHECK_EQ(Purple::SerializeState(back), text);
}

void TestStateDefaults() {
	Begin("state defaults");

	// No file, and a file we cannot read, both start from stock behaviour
	// rather than from a preset the user never chose.
	for (const auto &text : { QString(), u"active_preset = ["_q }) {
		const auto state = Purple::ParseState(text, u"state.toml"_q);
		CHECK_EQ(state.activePreset, u"normal"_q);
		CHECK(state.activeSource == Purple::PresetSource::Manual);
		CHECK(!state.schedulePaused);
		CHECK_EQ(state.schedulePausedUntil, int64(0));
		CHECK(!state.focusActive);
		CHECK(!state.focusSeen);
		CHECK(!state.peekActive);
		CHECK(!state.resolvedCache.valid());
	}

	// A cache that names a preset but describes no lists is not usable: the
	// engine would resolve nothing and quietly default every chat.
	const auto partial = Purple::ParseState(
		u"active_preset = \"work\"\n[resolved_cache]\npreset = \"work\"\n"_q,
		u"state.toml"_q);
	CHECK_EQ(partial.activePreset, u"work"_q);
	CHECK(!partial.resolvedCache.valid());

	// A file written before the deadline existed: paused, and paused until it
	// is lifted by hand, which is exactly what it meant when it was written.
	const auto older = Purple::ParseState(
		u"active_preset = \"work\"\nschedule_paused = true\n"_q,
		u"state.toml"_q);
	CHECK(older.schedulePaused);
	CHECK_EQ(older.schedulePausedUntil, int64(0));

	// An unknown source name is not a reason to refuse the rest of the file.
	const auto odd = Purple::ParseState(
		u"active_preset = \"work\"\nactive_preset_source = \"telepathy\"\n"_q,
		u"state.toml"_q);
	CHECK_EQ(odd.activePreset, u"work"_q);
	CHECK(odd.activeSource == Purple::PresetSource::Manual);
}

void TestStateQuoting() {
	Begin("state quoting");

	// Preset names come out of the hand-written settings.toml, so they can
	// hold anything a TOML key can hold.
	auto state = Purple::State();
	state.activePreset = u"quote\" and \\ backslash"_q;
	state.resolvedCache.preset = state.activePreset;
	state.resolvedCache.lists = { { u"tab\there"_q, Purple::ShowMode::Always, true } };

	const auto text = Purple::SerializeState(state);
	const auto back = Purple::ParseState(text, u"state.toml"_q);
	CHECK_EQ(back.activePreset, state.activePreset);
	CHECK(back.resolvedCache.valid());
	CHECK_EQ(back.resolvedCache.lists[0].list, u"tab\there"_q);
}

// The two fingerprints, and the rule they exist for. Everything about the
// ping-pong lives in ShouldAutoSend, so all of it is provable here: the app
// around it has a network and a file watcher and proves nothing cheaply.
void TestAutoSend() {
	Begin("auto send");

	const auto bytes = QByteArray("[sync]\nsend_after_save_p = true\n");
	const auto other = QByteArray("[sync]\nsend_after_save_p = false\n");
	const auto fingerprint = Purple::SettingsFingerprint(bytes);

	// "<length>:<sha256hex>", which is the shape the Android watcher writes -
	// the two halves of a sync have to be comparing the same string.
	CHECK(fingerprint.startsWith(
		QString::number(bytes.size()) + QChar(':')));
	CHECK_EQ(int(fingerprint.size()),
		QString::number(bytes.size()).size() + 1 + 64);
	CHECK_EQ(fingerprint, Purple::SettingsFingerprint(bytes));
	CHECK(fingerprint != Purple::SettingsFingerprint(other));

	// Empty bytes still have a fingerprint, and it is not the empty string -
	// which matters, because empty is what "never sent" is written as.
	CHECK(!Purple::SettingsFingerprint(QByteArray()).isEmpty());

	auto settings = Purple::Settings();
	auto state = Purple::State();

	// Off is off: nothing else is even asked.
	CHECK(!Purple::ShouldAutoSend(settings, state, bytes, false));

	settings.sync.sendAfterSave = true;
	CHECK(Purple::ShouldAutoSend(settings, state, bytes, false));

	// The import's own write never sends, even now, before anything has been
	// recorded: the bytes came from the other device.
	CHECK(!Purple::ShouldAutoSend(settings, state, bytes, true));

	// Sent once, and the same file does not go again. A different one does.
	state.lastSentFingerprint = fingerprint;
	CHECK(!Purple::ShouldAutoSend(settings, state, bytes, false));
	CHECK(Purple::ShouldAutoSend(settings, state, other, false));

	// The ping-pong itself: what this device wrote because the other device
	// sent it does not come back, however the save that follows was triggered.
	state = Purple::State();
	state.lastImportedFingerprint = fingerprint;
	CHECK(!Purple::ShouldAutoSend(settings, state, bytes, false));
	CHECK(Purple::ShouldAutoSend(settings, state, other, false));

	// Both round trip, and an older state.toml that has neither reads as
	// "never sent, never imported" rather than as anything else.
	state = Purple::State();
	state.activePreset = u"work"_q;
	state.lastSentFingerprint = fingerprint;
	state.lastImportedFingerprint = Purple::SettingsFingerprint(other);
	const auto text = Purple::SerializeState(state);
	const auto back = Purple::ParseState(text, u"state.toml"_q);
	CHECK_EQ(back.lastSentFingerprint, fingerprint);
	CHECK_EQ(back.lastImportedFingerprint,
		Purple::SettingsFingerprint(other));
	CHECK_EQ(Purple::SerializeState(back), text);

	const auto older = Purple::ParseState(
		u"active_preset = \"work\"\n"_q,
		u"state.toml"_q);
	CHECK(older.lastSentFingerprint.isEmpty());
	CHECK(older.lastImportedFingerprint.isEmpty());

	// Which is exactly the state that sends: an upgrade does not owe the user
	// a suppressed first send.
	CHECK(Purple::ShouldAutoSend(settings, older, bytes, false));
}

// A file exercising every resolution rule at once: a locked list, a chat in two
// lists, a three-deep inheritance chain, and a child that empties its folders.
[[nodiscard]] QString Presets() {
	return uR"(
[lists.os]
members = [ 100 ]

[lists.emergency]
members = [ 200, 300 ]

[lists.colleagues]
members = [ 300, 400 ]

[lists.private]
kinds = ["private"]

[lists.groups]
kinds = ["groups"]

[lists.channels]
kinds = ["channels"]

[lists.bots]
kinds = ["bots"]

[lists.everything]
kinds = ["private", "groups", "channels", "bots"]

[list_sets.protected]
list_order = [
  { list = "os",        show_mode = "always", notify_p = true },
  { list = "emergency", show_mode = "always", notify_p = true },
]

[presets.work]
list_order = [
  "*protected",
  { list = "colleagues", show_mode = "mention",  notify_p = true },
  { list = "private",    show_mode = "always",  notify_p = false },
  { list = "groups",     show_mode = "mention",  notify_p = true },
  { list = "channels",   show_mode = "never", notify_p = false },
  { list = "bots",       show_mode = "always",  notify_p = true },
]
folders = [ { name = "Music", notify_p = false } ]

[presets.strict]
list_order = [
  "*protected",
  { list = "colleagues", show_mode = "always",  notify_p = true },
  { list = "private",    show_mode = "never", notify_p = false },
  { list = "groups",     show_mode = "mention",  notify_p = true },
  { list = "channels",   show_mode = "never", notify_p = false },
]
folders = [ "*ALL" ]

[presets.lockdown]
list_order = [ "*protected" ]
folders = []
)"_q;
}

void TestResolveBasics() {
	Begin("resolve basics");

	const auto parsed = Parse(Presets());
	CHECK(parsed.ok());

	const auto work = Purple::Resolve(parsed.settings, u"work"_q);
	CHECK(work.has_value());
	CHECK(!work->normal);

	// The preset's own entries, in its own order, with "*protected" spliced in
	// at the front - not every list in the file.
	CHECK_EQ(work->lists.size(), size_t(7));
	CHECK_EQ(work->lists[0].list, u"os"_q);
	CHECK_EQ(Mode(work->list(u"private"_q)->show),
		Mode(Purple::ShowMode::Always));
	CHECK(!work->list(u"private"_q)->notify);
	CHECK_EQ(Mode(work->list(u"channels"_q)->show),
		Mode(Purple::ShowMode::Never));
	CHECK(work->list(u"everything"_q) == nullptr);

	// notify collapses here; show deliberately does not, because its default
	// depends on the chat and one entry can claim several kinds.
	CHECK_EQ(Mode(work->list(u"os"_q)->show), Mode(Purple::ShowMode::Always));
	CHECK_EQ(Mode(work->list(u"colleagues"_q)->show),
		Mode(Purple::ShowMode::Mention));

	CHECK_EQ(int(work->folders.size()), 1);
	CHECK_EQ(work->folders[0].name, u"Music"_q);
	CHECK(work->views.empty());

	// Off unless a preset asks: hiding a chat from the work view is a small
	// thing to get wrong, and taking it out of the forward picker is not.
	CHECK(!work->hideEverywhere);

	// Normal is a bypass, not a preset with everything switched on.
	const auto normal = Purple::Resolve(parsed.settings, u"normal"_q);
	CHECK(normal.has_value());
	CHECK(normal->normal);
	CHECK(normal->lists.empty());
	CHECK(Purple::MatchList(
		parsed.settings,
		*normal,
		400,
		Purple::ChatKind::Private) == nullptr);
	CHECK(Purple::Visible(
		parsed.settings,
		*normal,
		400,
		Purple::ChatKind::Private).show
		!= Purple::ShowMode::Never);

	// A preset the settings do not describe cannot be resolved, and the caller
	// is meant to fall back rather than to defaults. With no inheritance there
	// is no implicit root to land on either.
	CHECK(!Purple::Resolve(parsed.settings, u"ghost"_q).has_value());
	CHECK(!Purple::Resolve(parsed.settings, u"default"_q).has_value());
	CHECK(!Purple::Resolve(parsed.settings, QString()).has_value());
}

void TestResolveViewName() {
	Begin("resolve view name");

	const auto named = Parse(uR"(
[presets.work]
default_view_name = "Deep Work"

[presets.plain]
list_order = []

[presets."deep focus"]
list_order = []
)"_q);
	CHECK(named.ok());
	const auto work = Purple::Resolve(named.settings, u"work"_q);
	const auto plain = Purple::Resolve(named.settings, u"plain"_q);
	const auto spaced = Purple::Resolve(named.settings, u"deep focus"_q);
	CHECK(work.has_value() && work->viewName == u"Deep Work"_q);
	CHECK(plain.has_value() && plain->viewName == u"Plain"_q);

	// Only the first letter, so a name with a space in it is not title-cased
	// into something the user did not write.
	CHECK(spaced.has_value() && spaced->viewName == u"Deep focus"_q);
	CHECK_EQ(Purple::DefaultViewName(u"work"_q), u"Work"_q);
	CHECK_EQ(Purple::DefaultViewName(u"WORK"_q), u"WORK"_q);
	CHECK(Purple::DefaultViewName(QString()).isEmpty());

	// A capital anywhere means the casing was chosen, so it is left alone
	// rather than tidied into something the user did not write.
	CHECK_EQ(Purple::DefaultViewName(u"iH"_q), u"iH"_q);
	CHECK_EQ(Purple::DefaultViewName(u"deep Focus"_q), u"deep Focus"_q);
	CHECK_EQ(Purple::DefaultViewName(u"eBay"_q), u"eBay"_q);

	// Digits and punctuation are not capitals and decide nothing.
	CHECK_EQ(Purple::DefaultViewName(u"p0"_q), u"P0"_q);
	CHECK_EQ(Purple::DefaultViewName(u"9to5"_q), u"9to5"_q);

	// The cache carries it, so a broken reload does not also rename the tab.
	const auto cached = Purple::FromCache(Purple::ToCache(*work));
	CHECK(cached.has_value() && cached->viewName == u"Deep Work"_q);

	// A cache written by a build that did not know the key still names the
	// tab something, rather than leaving it blank.
	auto older = Purple::ToCache(*work);
	older.viewName = QString();
	const auto restored = Purple::FromCache(older);
	CHECK(restored.has_value() && restored->viewName == u"Work"_q);

	// The same answer from the parsed preset, without resolving it. This is
	// what the preset picker reads: it lists presets from the file, and a row
	// naming one has to agree with the tab that preset produces.
	const auto presets = &named.settings;
	CHECK(presets->preset(u"work"_q) != nullptr);
	CHECK_EQ(
		Purple::PresetTitle(*presets->preset(u"work"_q)),
		u"Deep Work"_q);
	CHECK_EQ(Purple::PresetTitle(*presets->preset(u"plain"_q)), u"Plain"_q);
	CHECK_EQ(
		Purple::PresetTitle(*presets->preset(u"deep focus"_q)),
		u"Deep focus"_q);

	// The two-argument form is what the gate has in hand, since a resolution
	// carries the name and the view name rather than the preset.
	CHECK_EQ(Purple::PresetTitle(u"work"_q, QString()), u"Work"_q);
	CHECK_EQ(Purple::PresetTitle(u"work"_q, u"Deep Work"_q), u"Deep Work"_q);

	// A name whose casing was chosen comes back as it was written, whichever
	// letter carries the capital.
	CHECK_EQ(Purple::PresetTitle(u"WORK"_q, QString()), u"WORK"_q);
	CHECK_EQ(Purple::PresetTitle(u"iH"_q, QString()), u"iH"_q);
	CHECK(Purple::PresetTitle(QString(), QString()).isEmpty());
}

void TestFallThrough() {
	Begin("fall-through");

	const auto parsed = Parse(Presets());

	// lockdown names two lists and nothing else. Everything they do not hold is
	// hidden AND silenced: a preset names what gets through, and saying nothing
	// about a chat is saying no.
	const auto lockdown = Purple::Resolve(parsed.settings, u"lockdown"_q);
	CHECK(lockdown.has_value());
	CHECK(Purple::MatchList(
		parsed.settings,
		*lockdown,
		999,
		Purple::ChatKind::Private) == nullptr);
	const auto missed = Purple::Visible(
		parsed.settings,
		*lockdown,
		999,
		Purple::ChatKind::Private);
	CHECK_EQ(Mode(missed.show), Mode(Purple::ShowMode::Never));
	CHECK(!missed.notify);
	

	// What it does name still comes through, by id and by kind alike.
	const auto kept = Purple::Visible(
		parsed.settings,
		*lockdown,
		100,
		Purple::ChatKind::Channel);
	CHECK(kept.show != Purple::ShowMode::Never);
	CHECK(kept.notify);

	// An entry naming a list that does not exist claims nothing, so the chat
	// falls through rather than being swallowed by a name that means nothing.
	const auto ghost = Parse(uR"(
[lists.a]
kinds = ["private"]

[presets.work]
list_order = [ { list = "ghosts", show_mode = "always" }, { list = "a", show_mode = "always" } ]
)"_q);
	const auto resolved = Purple::Resolve(ghost.settings, u"work"_q);
	CHECK_EQ(Purple::MatchList(
		ghost.settings,
		*resolved,
		1,
		Purple::ChatKind::Private)->list, u"a"_q);
}

void TestMatchPriority() {
	Begin("match priority");

	const auto parsed = Parse(Presets());
	const auto work = Purple::Resolve(parsed.settings, u"work"_q);

	// 300 is in both emergency and colleagues; the earlier entry wins.
	const auto matched = Purple::MatchList(
		parsed.settings,
		*work,
		300,
		Purple::ChatKind::Private);
	CHECK(matched != nullptr);
	CHECK_EQ(matched->list, u"emergency"_q);

	// 400 is only in colleagues.
	CHECK_EQ(Purple::MatchList(
		parsed.settings,
		*work,
		400,
		Purple::ChatKind::Private)->list, u"colleagues"_q);

	// A member id beats a kind rule further down, which is the whole point of
	// being able to name one chat out of a sweeping rule.
	CHECK_EQ(Purple::MatchList(
		parsed.settings,
		*work,
		100,
		Purple::ChatKind::Channel)->list, u"os"_q);
	CHECK_EQ(Purple::MatchList(
		parsed.settings,
		*work,
		999,
		Purple::ChatKind::Channel)->list, u"channels"_q);
	CHECK_EQ(Purple::MatchList(
		parsed.settings,
		*work,
		999,
		Purple::ChatKind::Bot)->list, u"bots"_q);

	// A list matches when either half does; both empty matches nothing.
	const auto os = parsed.settings.list(u"os"_q);
	CHECK(Purple::ListHolds(*os, 100, Purple::ChatKind::Bot));
	CHECK(!Purple::ListHolds(*os, 101, Purple::ChatKind::Bot));
	const auto bots = parsed.settings.list(u"bots"_q);
	CHECK(Purple::ListHolds(*bots, 12345, Purple::ChatKind::Bot));
	CHECK(!Purple::ListHolds(*bots, 12345, Purple::ChatKind::Group));

	// Moving an entry flips which of the two wins - the order in the preset IS
	// the priority, and nothing else decides it. The spread stays in the file,
	// just below colleagues, so this is a reorder rather than a deletion.
	const auto before =
		u"  \"*protected\",\n"
		"  { list = \"colleagues\", show_mode = \"mention\",  "
		"notify_p = true },\n"_q;
	auto swapped = Presets();
	CHECK(swapped.contains(before));
	swapped.replace(
		before,
		u"  { list = \"colleagues\", show_mode = \"mention\",  "
		"notify_p = true },\n  \"*protected\",\n"_q);
	const auto reparsed = Parse(swapped);
	CHECK(reparsed.ok());
	const auto after = Purple::Resolve(reparsed.settings, u"work"_q);

	// Still seven entries: emergency moved, it did not disappear.
	CHECK_EQ(after->lists.size(), size_t(7));
	CHECK(after->list(u"emergency"_q) != nullptr);
	CHECK_EQ(Purple::MatchList(
		reparsed.settings,
		*after,
		300,
		Purple::ChatKind::Private)->list, u"colleagues"_q);

	// And 200, which only emergency holds, still lands there.
	CHECK_EQ(Purple::MatchList(
		reparsed.settings,
		*after,
		200,
		Purple::ChatKind::Private)->list, u"emergency"_q);
}

void TestViewMembership() {
	Begin("view membership");

	const auto parsed = Parse(uR"(
[lists.team]
members = [ 10, 20 ]

[lists.people]
kinds = ["private"]

[presets.work]
list_order = [ { list = "people", show_mode = "always" } ]

[[presets.work.views]]
name   = "Focus"
pinned = [ 20, 10 ]
list_order = [
  { list = "team",   show_mode = "never" },
  { list = "people", show_mode = "always" },
]
)"_q);
	CHECK(parsed.ok());
	const auto work = Purple::Resolve(parsed.settings, u"work"_q);
	CHECK(work.has_value());
	CHECK_EQ(int(work->views.size()), 1);

	const auto &view = work->views[0];
	CHECK_EQ(view.name, u"Focus"_q);
	CHECK_EQ(view.pinned, (std::vector<Purple::PeerIdValue>{ 20, 10 }));

	// The first entry claims the team ids and drops them; everyone else who is
	// a DM is on the tab. A kind nothing names is off it.
	CHECK(!Purple::ViewHolds(
		parsed.settings,
		view,
		10,
		Purple::ChatKind::Private));
	CHECK(Purple::ViewHolds(
		parsed.settings,
		view,
		30,
		Purple::ChatKind::Private));
	CHECK(!Purple::ViewHolds(
		parsed.settings,
		view,
		30,
		Purple::ChatKind::Bot));

	// A view selects membership; it never changes what a chat may do. Both
	// those team ids are still shown and still notifying on the main view.
	const auto visible = Purple::Visible(
		parsed.settings,
		*work,
		10,
		Purple::ChatKind::Private);
	CHECK(visible.show != Purple::ShowMode::Never);
	CHECK(visible.notify);

	// And the cache carries the whole thing, pins included.
	const auto cached = Purple::FromCache(Purple::ToCache(*work));
	CHECK(cached.has_value());
	CHECK_EQ(int(cached->views.size()), 1);
	CHECK_EQ(cached->views[0].name, u"Focus"_q);
	CHECK_EQ(cached->views[0].pinned,
		(std::vector<Purple::PeerIdValue>{ 20, 10 }));
	CHECK(!Purple::ViewHolds(
		parsed.settings,
		cached->views[0],
		10,
		Purple::ChatKind::Private));
}

void TestMentionGate() {
	Begin("mention gate");

	const auto parsed = Parse(Presets());
	const auto work = Purple::Resolve(parsed.settings, u"work"_q);
	const auto strict = Purple::Resolve(parsed.settings, u"strict"_q);

	// A visible group is gated when its entry asks for it.
	const auto group = Purple::Visible(
		parsed.settings,
		*work,
		999,
		Purple::ChatKind::Group);
	CHECK(group.show != Purple::ShowMode::Never);
	CHECK_EQ(Mode(group.show), Mode(Purple::ShowMode::Mention));

	// Channels and DMs never are, whatever the entry says.
	CHECK(Purple::Visible(
		parsed.settings,
		*work,
		999,
		Purple::ChatKind::Private).show
		!= Purple::ShowMode::Mention);
	CHECK(Purple::Visible(
		parsed.settings,
		*work,
		100,
		Purple::ChatKind::Channel).show
		!= Purple::ShowMode::Mention);

	// The gate is per entry, so strict can leave colleagues ungated while
	// gating everything the "groups" entry sweeps up.
	CHECK(Purple::Visible(
		parsed.settings,
		*strict,
		400,
		Purple::ChatKind::Group).show
		!= Purple::ShowMode::Mention);
	CHECK_EQ(Mode(Purple::Visible(
		parsed.settings,
		*strict,
		999,
		Purple::ChatKind::Group).show),
		Mode(Purple::ShowMode::Mention));

	// A chat nothing claims is hidden whether or not anyone mentioned us in it.
	const auto lockdown = Purple::Resolve(parsed.settings, u"lockdown"_q);
	const auto hidden = Purple::Visible(
		parsed.settings,
		*lockdown,
		999,
		Purple::ChatKind::Group);
	CHECK_EQ(Mode(hidden.show), Mode(Purple::ShowMode::Never));
	CHECK(hidden.show != Purple::ShowMode::Mention);
}

void TestPeek() {
	Begin("peek");

	const auto parsed = Parse(Presets());
	auto work = *Purple::Resolve(parsed.settings, u"work"_q);

	// What work does without a peek: channels are gone, DMs are silent, groups
	// wait for a mention.
	const auto channel = [&] {
		return Purple::Visible(
			parsed.settings,
			work,
			999,
			Purple::ChatKind::Channel);
	};
	const auto priv = [&] {
		return Purple::Visible(
			parsed.settings,
			work,
			999,
			Purple::ChatKind::Private);
	};
	const auto group = [&] {
		return Purple::Visible(
			parsed.settings,
			work,
			999,
			Purple::ChatKind::Group);
	};
	CHECK_EQ(Mode(channel().show), Mode(Purple::ShowMode::Never));
	CHECK(!priv().notify);
	CHECK_EQ(Mode(group().show), Mode(Purple::ShowMode::Mention));

	// A peek reveals everything the preset hides and lifts the mention gate,
	// and deliberately leaves the silencing alone: it is a look at the chat
	// list, not two minutes of notifications for chats already on the screen.
	work.peeking = true;
	CHECK(channel().show != Purple::ShowMode::Never);

	// Revealed and still silent, which is the rule stated as plainly as it can
	// be: a peek changes what you can see and nothing about what may interrupt.
	CHECK(!channel().notify);
	CHECK(group().show != Purple::ShowMode::Never);
	CHECK(group().show != Purple::ShowMode::Mention);
	CHECK(priv().show != Purple::ShowMode::Never);
	CHECK(!priv().notify);

	// It never reaches the cache, so a resolution restored from state.toml
	// cannot come back still revealed with nothing left to end it.
	const auto restored = Purple::FromCache(Purple::ToCache(work));
	CHECK(restored.has_value());
	CHECK(!restored->peeking);
	CHECK_EQ(Mode(restored->list(u"channels"_q)->show),
		Mode(Purple::ShowMode::Never));

	// The deadline is what expires a peek that outlived the app.
	auto state = Purple::State();
	state.peekActive = true;
	state.peekDeadlineUnix = 2000;
	CHECK(Purple::PeekLive(state, 1999));
	CHECK(!Purple::PeekLive(state, 2000));
	CHECK(!Purple::PeekLive(state, 5000));

	// No deadline is auto_off = "off": it runs until it is turned off by hand.
	state.peekDeadlineUnix = 0;
	CHECK(Purple::PeekLive(state, 5000));

	state.peekActive = false;
	CHECK(!Purple::PeekLive(state, 0));
}

void TestNamedExplicitly() {
	Begin("named explicitly");

	const auto parsed = Parse(uR"(
[lists.close_people]
members = [ 111, 222 ]

[lists.everyone]
kinds = ["private"]

[lists.banned]
members = [ 333 ]

[presets.work]
list_order = [
  { list = "close_people" },
  { list = "banned", show_mode = "never" },
  { list = "everyone" },
]

[[presets.work.views]]
name       = "P0"
list_order = [ { list = "close_people" } ]

[presets.plain]
list_order = [ { list = "everyone" } ]
)"_q);
	CHECK(parsed.ok());
	const auto work = Purple::Resolve(parsed.settings, u"work"_q);
	const auto plain = Purple::Resolve(parsed.settings, u"plain"_q);
	CHECK(work.has_value() && plain.has_value());

	const auto named = [&](const std::optional<Purple::Resolved> &resolved,
			Purple::PeerIdValue id) {
		return Purple::NamedExplicitly(parsed.settings, *resolved, id);
	};

	// Written into a list's members, and that list is ordered: yes.
	CHECK(named(work, 111));
	CHECK(named(work, 222));

	// The whole point of the predicate. 999 is a private chat, so
	// { list = "everyone" } matches it and it is very much in the view - but
	// nobody wrote it down, so it is not grounds for keeping an empty chat in
	// the chat list. Getting this wrong drags in every contact with no
	// conversation.
	CHECK(!named(work, 999));

	// Named in order to be hidden is not a request for it to be anywhere.
	CHECK(!named(work, 333));

	// A view counts, which is the case that motivated all of this: the main
	// order may gate the chat while a tab holds it unconditionally.
	const auto viewOnly = Parse(uR"(
[lists.close_people]
members = [ 111 ]

[presets.work]
list_order = []

[[presets.work.views]]
name       = "P0"
list_order = [ { list = "close_people" } ]
)"_q);
	CHECK(viewOnly.ok());
	const auto held = Purple::Resolve(viewOnly.settings, u"work"_q);
	CHECK(held.has_value());
	CHECK(Purple::NamedExplicitly(viewOnly.settings, *held, 111));

	// A preset that does not order the list gets nothing from it, even though
	// the list is right there in the file.
	CHECK(!named(plain, 111));

	// Normal is a bypass, and must not be keeping anything anywhere.
	const auto normal = Purple::Resolve(parsed.settings, Purple::NormalPreset());
	CHECK(normal.has_value());
	CHECK(!Purple::NamedExplicitly(parsed.settings, *normal, 111));

	// An id of zero is "we could not work out what this peer is", never a match.
	CHECK(!named(work, 0));
}

void TestPresetHotkeys() {
	Begin("preset hotkeys");

	const auto parsed = Parse(uR"(
[lists.all]
kinds = ["private"]

[presets.work]
hotkey = "Ctrl+Shift+W"
list_order = [ { list = "all" } ]

[presets.deep]
hotkey = "Ctrl+Shift+G"
list_order = [ { list = "all" } ]
)"_q);
	CHECK(parsed.ok());
	CHECK(parsed.warnings.empty());
	CHECK_EQ(parsed.settings.preset(u"work"_q)->hotkey, u"Ctrl+Shift+W"_q);
	CHECK_EQ(parsed.settings.preset(u"deep"_q)->hotkey, u"Ctrl+Shift+G"_q);

	// No key is the normal case and says nothing.
	const auto silent = Parse(
		u"[lists.all]\nkinds = [\"private\"]\n"
		"[presets.work]\nlist_order = [ { list = \"all\" } ]\n"_q);
	CHECK(silent.ok());
	CHECK(silent.warnings.empty());
	CHECK(silent.settings.preset(u"work"_q)->hotkey.isEmpty());

	// Two presets on one key: Qt fires NEITHER action when a sequence is
	// ambiguous, so the second is dropped rather than left to break both.
	const auto clash = Parse(uR"(
[lists.all]
kinds = ["private"]

[presets.work]
hotkey = "Ctrl+Shift+W"
list_order = [ { list = "all" } ]

[presets.deep]
hotkey = "ctrl+shift+W"
list_order = [ { list = "all" } ]
)"_q);
	CHECK(clash.ok());
	CHECK_EQ(int(clash.warnings.size()), 1);
	CHECK_EQ(clash.settings.preset(u"work"_q)->hotkey, u"Ctrl+Shift+W"_q);
	CHECK(clash.settings.preset(u"deep"_q)->hotkey.isEmpty());

	// And against peek, which is claimed first because it is not a preset and
	// cannot be renamed out of the way.
	const auto peek = Parse(uR"(
[lists.all]
kinds = ["private"]

[peek]
hotkey = "Ctrl+Shift+Y"

[presets.work]
hotkey = "Ctrl+Shift+Y"
list_order = [ { list = "all" } ]
)"_q);
	CHECK(peek.ok());
	CHECK_EQ(int(peek.warnings.size()), 1);
	CHECK(peek.settings.preset(u"work"_q)->hotkey.isEmpty());
	CHECK_EQ(peek.settings.peek.hotkey, u"Ctrl+Shift+Y"_q);
}

void TestOverrides() {
	Begin("until overrides");

	CHECK_EQ(
		int(*Purple::ParseOverrideKind(u" NOTIFY "_q)),
		int(Purple::OverrideKind::Notify));
	CHECK(!Purple::ParseOverrideKind(u"maybe"_q).has_value());
	CHECK_EQ(
		Purple::OverrideKindName(Purple::OverrideKind::Hide),
		u"hide"_q);

	auto state = Purple::State();
	state.activePreset = u"work"_q;
	state.overrides = {
		{ 10, Purple::OverrideKind::Show, 2000, 200, u"work"_q },
		{ 11, Purple::OverrideKind::Hide, 1500, 300, u"work"_q },
		{ 12, Purple::OverrideKind::Notify, 3000, 400, u"deep"_q },
		{ 13, Purple::OverrideKind::Show, 500, 100, u"work"_q },
	};

	// Scoped to the preset it was made under. The one made in "deep" is
	// invisible from "work" even though it has not expired.
	CHECK(Purple::OverrideFor(state, 10, u"work"_q, 1000) != nullptr);
	CHECK(Purple::OverrideFor(state, 12, u"work"_q, 1000) == nullptr);
	CHECK(Purple::OverrideFor(state, 12, u"deep"_q, 1000) != nullptr);
	CHECK(Purple::OverrideFor(state, 12, u"DEEP"_q, 1000) != nullptr);

	// And already filtered for expiry, so a caller is one test rather than
	// three. Entry 13 ran out at 500.
	CHECK(Purple::OverrideFor(state, 13, u"work"_q, 1000) == nullptr);
	CHECK(Purple::OverrideFor(state, 13, u"work"_q, 400) != nullptr);

	// Nothing matches under Normal, which has no preset name at all.
	CHECK(Purple::OverrideFor(state, 10, QString(), 1000) == nullptr);

	CHECK_EQ(
		int(Purple::OverrideFor(state, 11, u"work"_q, 1000)->kind),
		int(Purple::OverrideKind::Hide));

	// The timer is armed for the earliest still outstanding under the running
	// preset - "deep"'s later one must not hold "work"'s timer open.
	CHECK_EQ(Purple::NextOverrideDeadline(state, u"work"_q), int64(500));
	CHECK_EQ(Purple::NextOverrideDeadline(state, u"deep"_q), int64(3000));
	CHECK_EQ(Purple::NextOverrideDeadline(state, QString()), int64(0));

	// Pruning reports whether anything went, so the caller knows if a rebuild
	// is owed rather than doing one every tick.
	CHECK(!Purple::PruneOverrides(state, 400));
	CHECK_EQ(int(state.overrides.size()), 4);
	CHECK(Purple::PruneOverrides(state, 1600));
	CHECK_EQ(int(state.overrides.size()), 2);
	CHECK(Purple::OverrideFor(state, 11, u"work"_q, 1000) == nullptr);
	CHECK(Purple::OverrideFor(state, 10, u"work"_q, 1000) != nullptr);

	// They survive the state.toml round trip, deadlines and all - the whole
	// reason the deadline is unix seconds rather than crl::time.
	const auto text = Purple::SerializeState(state);
	const auto back = Purple::ParseState(text, Path());
	CHECK_EQ(int(back.overrides.size()), 2);
	CHECK_EQ(back.overrides.front().peer, Purple::PeerIdValue(10));
	CHECK_EQ(back.overrides.front().untilUnix, int64(2000));
	CHECK_EQ(back.overrides.front().startedUnix, int64(200));
	CHECK_EQ(back.overrides.front().preset, u"work"_q);
	CHECK_EQ(
		int(back.overrides.back().kind),
		int(Purple::OverrideKind::Notify));

	// A state with none writes no key at all, so the common file stays quiet.
	auto empty = Purple::State();
	empty.activePreset = u"work"_q;
	CHECK(!Purple::SerializeState(empty).contains(u"overrides"_q));

	// An entry missing anything it needs is skipped rather than fought over,
	// like every other reader in that file.
	const auto broken = Purple::ParseState(uR"(
active_preset = "work"
overrides = [
  { peer = 1, kind = "show", started = 9, until = 99, preset = "work" },
  { peer = 0, kind = "show", started = 9, until = 99, preset = "work" },
  { peer = 2, kind = "nonsense", started = 9, until = 99, preset = "work" },
  { peer = 3, kind = "show", started = 9, preset = "work" },
]
)"_q, Path());
	CHECK_EQ(int(broken.overrides.size()), 1);
	CHECK_EQ(broken.overrides.front().peer, Purple::PeerIdValue(1));
}

// The two keys added for the Android screens: one global, one per preset, both
// defaulting to true and both read in silence by a build that predates them.
void TestSuggestionsAndArchive() {
	Begin("suggestions and archive");

	const auto silent = Parse(uR"(
[lists.os]
members = [1]

[presets.work]
list_order = [ { list = "os" } ]
)"_q);
	CHECK(silent.ok());
	CHECK(silent.warnings.empty());
	CHECK(silent.settings.suggestions.hideInvisible);
	CHECK(!silent.settings.preset(u"work"_q)->hideArchive.has_value());
	CHECK(Purple::Resolve(silent.settings, u"work"_q)->hideArchive);

	const auto off = Parse(uR"(
[suggestions]
hide_invisible_p = false

[lists.os]
members = [1]

[presets.work]
hide_archive_p = false
list_order = [ { list = "os" } ]
)"_q);
	CHECK(off.ok());
	CHECK(off.warnings.empty());
	CHECK(!off.settings.suggestions.hideInvisible);
	CHECK_EQ(off.settings.preset(u"work"_q)->hideArchive.value_or(true), false);
	CHECK(!Purple::Resolve(off.settings, u"work"_q)->hideArchive);

	// Normal never asks - the consumer keys on whether a preset is filtering at
	// all - so it carries the default and nothing acts on it.
	CHECK(Purple::Resolve(off.settings, u"normal"_q)->hideArchive);

	// Anything that is not a boolean is a warning and the default, like every
	// other flag in the file.
	const auto wrong = Parse(uR"(
[suggestions]
hide_invisible_p = "yes"

[lists.os]
members = [1]

[presets.work]
hide_archive_p = "no"
list_order = [ { list = "os" } ]
)"_q);
	CHECK(wrong.ok());
	CHECK(wrong.settings.suggestions.hideInvisible);
	CHECK(WarnsAbout(wrong, u"'hide_invisible_p' should be true or false"_q));
	CHECK(WarnsAbout(wrong, u"'hide_archive_p' should be true or false"_q));
	CHECK(!wrong.settings.preset(u"work"_q)->hideArchive.has_value());
	CHECK(Purple::Resolve(wrong.settings, u"work"_q)->hideArchive);

	const auto notATable = Parse(u"suggestions = 1\n"_q);
	CHECK(notATable.ok());
	CHECK(notATable.settings.suggestions.hideInvisible);
	CHECK(WarnsAbout(notATable, u"'suggestions' should be a table"_q));

	// The archive flag rides in the resolved cache, so a settings.toml broken
	// mid-edit does not hand the archive back while the preset still runs.
	auto state = Purple::State();
	state.activePreset = u"work"_q;
	state.resolvedCache = Purple::ToCache(
		*Purple::Resolve(off.settings, u"work"_q));
	const auto written = Purple::SerializeState(state);
	CHECK(written.contains(u"hide_archive = false"_q));
	const auto read = Purple::ParseState(written, u"state.toml"_q);
	CHECK(read.resolvedCache.valid());
	CHECK(!read.resolvedCache.hideArchive);
	CHECK(!Purple::FromCache(read.resolvedCache)->hideArchive);

	// A state.toml written before the key existed restores the default rather
	// than the false an absent boolean would otherwise read as.
	const auto older = Purple::ParseState(
		u"active_preset = \"work\"\n[resolved_cache]\npreset = \"work\"\n"
		"lists = [{ list = \"os\", notify = true }]\n"_q,
		u"state.toml"_q);
	CHECK(older.resolvedCache.valid());
	CHECK(older.resolvedCache.hideArchive);
	CHECK(Purple::FromCache(older.resolvedCache)->hideArchive);
}

// The two flags that are off unless the file asks: one about a strip the server
// fills in, one about sending the file itself somewhere. Both default to false,
// which is what an older settings.toml with neither key says too.
void TestSyncAndRecommended() {
	Begin("sync and recommended channels");

	const auto silent = Parse(uR"(
[lists.os]
members = [1]

[presets.work]
list_order = [ { list = "os" } ]
)"_q);
	CHECK(silent.ok());
	CHECK(silent.warnings.empty());
	CHECK(!silent.settings.sync.sendAfterSave);
	CHECK(!silent.settings.suggestions.recommendedChannels);

	const auto on = Parse(uR"(
[sync]
send_after_save_p = true

[suggestions]
recommended_channels_p = true

[lists.os]
members = [1]

[presets.work]
list_order = [ { list = "os" } ]
)"_q);
	CHECK(on.ok());
	CHECK(on.warnings.empty());
	CHECK(on.settings.sync.sendAfterSave);
	CHECK(on.settings.suggestions.recommendedChannels);

	// Written out as false, which has to read as false rather than as "said
	// nothing" - the two agree here, but only by accident of the default.
	const auto off = Parse(uR"(
[sync]
send_after_save_p = false

[suggestions]
recommended_channels_p = false
)"_q);
	CHECK(off.ok());
	CHECK(off.warnings.empty());
	CHECK(!off.settings.sync.sendAfterSave);
	CHECK(!off.settings.suggestions.recommendedChannels);

	// Anything that is not a boolean is a warning and the default, like every
	// other flag in the file. Quoted is the mistake worth naming: "yes" is what
	// the key looks like it should take, and TOML has no idea what it means.
	const auto wrong = Parse(uR"(
[sync]
send_after_save_p = "yes"

[suggestions]
recommended_channels_p = "on"
)"_q);
	CHECK(wrong.ok());
	CHECK(!wrong.settings.sync.sendAfterSave);
	CHECK(!wrong.settings.suggestions.recommendedChannels);
	CHECK(WarnsAbout(wrong, u"'send_after_save_p' should be true or false"_q));
	CHECK(WarnsAbout(
		wrong,
		u"'recommended_channels_p' should be true or false"_q));

	// And a [sync] that is not a table at all leaves the defaults standing,
	// exactly as [suggestions] does.
	const auto notATable = Parse(u"sync = 1\n"_q);
	CHECK(notATable.ok());
	CHECK(!notATable.settings.sync.sendAfterSave);
	CHECK(WarnsAbout(notATable, u"'sync' should be a table"_q));
}

void TestReservedHotkeys() {
	Begin("reserved hotkeys");

	// Ctrl+Shift+P is the spoiler shortcut every message field registers, as a
	// Qt::WidgetShortcut on the field itself. It therefore only joins the
	// contest while a field has focus, Qt calls the sequence ambiguous and
	// fires neither - so the key works until you click the composer, with
	// nothing in the log to say why. Warned about rather than silently
	// dropped: it is the user's key and it does work most of the time.
	const auto spoiler = Parse(uR"(
[lists.a]
kinds = ["private"]

[peek]
hotkey = "ctrl+shift+p"

[presets.work]
list_order = [ { list = "a" } ]
)"_q);
	CHECK(spoiler.ok());
	CHECK_EQ(int(spoiler.warnings.size()), 1);
	CHECK(spoiler.warnings.front().contains(u"spoiler"_q));
	CHECK_EQ(spoiler.settings.peek.hotkey, u"ctrl+shift+p"_q);

	// A preset's key is checked the same way, and spacing and case do not let
	// one slip past.
	const auto preset = Parse(uR"(
[lists.a]
kinds = ["private"]

[presets.work]
hotkey = "Ctrl + Shift + M"
list_order = [ { list = "a" } ]
)"_q);
	CHECK(preset.ok());
	CHECK_EQ(int(preset.warnings.size()), 1);
	CHECK(preset.warnings.front().contains(u"monospace"_q));

	// The default is not one of them any more, which was the bug: every fork
	// shipped with a peek key that died the moment you clicked the composer.
	const auto fresh = Parse(u"[presets.work]\nlist_order = []\n"_q);
	CHECK(fresh.ok());
	CHECK_EQ(fresh.settings.peek.hotkey, u"Ctrl+Shift+E"_q);
	for (const auto &warning : fresh.warnings) {
		CHECK(!warning.contains(u"message field"_q));
	}

	// And a key nobody claims says nothing at all.
	const auto fine = Parse(uR"(
[lists.a]
kinds = ["private"]

[peek]
hotkey = "Ctrl+Shift+E"

[presets.work]
hotkey = "Ctrl+Shift+W"
list_order = [ { list = "a" } ]
)"_q);
	CHECK(fine.ok());
	CHECK(fine.warnings.empty());
}

void TestStories() {
	Begin("stories");

	// Two vocabularies, because there are two questions. A preset can talk
	// about everybody; an entry is already a set of people, so "all" there
	// would be a category error and is not accepted.
	CHECK_EQ(
		int(*Purple::ParseStoryPolicy(u"  FOLLOW_UNSEEN "_q)),
		int(Purple::StoryPolicy::FollowUnseen));
	CHECK(!Purple::ParseStoryPolicy(u"unseen"_q).has_value());
	CHECK_EQ(
		Purple::StoryPolicyName(Purple::StoryPolicy::AllUnseen),
		u"all_unseen"_q);
	CHECK_EQ(
		int(*Purple::ParseStoryMode(u"Unseen"_q)),
		int(Purple::StoryMode::Unseen));
	CHECK(!Purple::ParseStoryMode(u"all"_q).has_value());
	CHECK(!Purple::ParseStoryMode(u"follow"_q).has_value());
	CHECK_EQ(Purple::StoryModeName(Purple::StoryMode::Never), u"never"_q);

	const auto result = Parse(uR"(
[lists.close]
members = [ 5 ]
[lists.rest]
kinds = ["channels"]

[presets.work]
stories = "follow_unseen"
list_order = [
  { list = "close", show_mode = "message", stories = "always" },
  { list = "rest" },
]
folders = [
  { name = "Music", stories = "never" },
  { name = "B" },
]
)"_q);
	CHECK(result.ok());
	CHECK(result.warnings.empty());

	const auto work = result.settings.preset(u"work"_q);
	CHECK(work != nullptr);
	CHECK_EQ(
		int(*work->stories),
		int(Purple::StoryPolicy::FollowUnseen));
	CHECK_EQ(
		int(*work->listOrder[0].stories),
		int(Purple::StoryMode::Always));
	CHECK(!work->listOrder[1].stories.has_value());

	const auto resolved = Purple::Resolve(result.settings, u"work"_q);
	CHECK(resolved.has_value());
	CHECK_EQ(int(resolved->stories), int(Purple::StoryPolicy::FollowUnseen));

	// The entry's mode rides along on the resolved list, so the strip does not
	// have to go back to the settings for it.
	CHECK_EQ(
		int(*resolved->lists[0].stories),
		int(Purple::StoryMode::Always));
	CHECK(!resolved->lists[1].stories.has_value());

	// Only the folders that said something are lifted out, so a preset that
	// said nothing about any of them tests one empty vector.
	CHECK_EQ(int(resolved->storyFolders.size()), 1);
	CHECK_EQ(resolved->storyFolders.front().name, u"Music"_q);
	CHECK_EQ(
		int(resolved->storyFolders.front().mode),
		int(Purple::StoryMode::Never));
	CHECK(Purple::StoryFolderList({}).empty());

	// A disabled folder says nothing about anything, stories included.
	auto disabled = work->folders;
	disabled[0].enabled = false;
	CHECK(Purple::StoryFolderList(disabled).empty());

	// Unset means Follow, so a preset written before any of this existed
	// behaves the way the default describes rather than the way "all" would.
	const auto quiet = Parse(u"[presets.b]\nlist_order = []\n"_q);
	CHECK(quiet.ok());
	CHECK(!quiet.settings.preset(u"b"_q)->stories.has_value());
	const auto fallback = Purple::Resolve(quiet.settings, u"b"_q);
	CHECK(fallback.has_value());
	CHECK_EQ(int(fallback->stories), int(Purple::StoryPolicy::Follow));

	// Both survive the state.toml round trip. The policy has to, or a broken
	// settings.toml would silently restore the strip to Follow; the entry mode
	// has to for the same reason `show' does.
	const auto cached = Purple::FromCache(Purple::ToCache(*resolved));
	CHECK(cached.has_value());
	CHECK_EQ(int(cached->stories), int(Purple::StoryPolicy::FollowUnseen));
	CHECK_EQ(
		int(*cached->lists[0].stories),
		int(Purple::StoryMode::Always));
	CHECK(!cached->lists[1].stories.has_value());
	CHECK_EQ(int(cached->storyFolders.size()), 1);
	CHECK_EQ(
		int(cached->storyFolders.front().mode),
		int(Purple::StoryMode::Never));

	// enabled_p travels with it. Without this a folder switched off came back
	// switched on the moment the app fell back to its cached resolution.
	auto withDisabled = *resolved;
	withDisabled.folders[1].enabled = false;
	const auto round = Purple::FromCache(Purple::ToCache(withDisabled));
	CHECK(round.has_value());
	CHECK(!Purple::FolderEnabled(round->folders[1]));
	CHECK(Purple::FolderEnabled(round->folders[0]));

	// A spelling from the wrong vocabulary warns and is ignored rather than
	// being taken for something else.
	const auto wrong = Parse(uR"(
[lists.a]
kinds = ["private"]

[presets.work]
stories = "sometimes"
list_order = [ { list = "a", stories = "follow" } ]
)"_q);
	CHECK(wrong.ok());
	CHECK_EQ(int(wrong.warnings.size()), 2);
	CHECK(!wrong.settings.preset(u"work"_q)->stories.has_value());
	CHECK(!wrong.settings.preset(u"work"_q)->listOrder[0].stories.has_value());
}

void TestRecent() {
	Begin("recent");

	// Off unless the file asks, and the narrow scope unless it says otherwise:
	// nothing a preset hides should start appearing because a key was added
	// with no `applies_to' beside it.
	const auto silent = Parse(u"[presets.work]\nlist_order = []\n"_q);
	CHECK(silent.ok());
	CHECK_EQ(silent.settings.recent.staySecondsAfterClose, 0);
	CHECK(silent.settings.recent.scope
		== Purple::RecentScope::AlreadyInView);

	const auto parsed = Parse(uR"(
[recent]
stay_visible_after_close = "2m"
applies_to = "any_open_chat"
)"_q);
	CHECK(parsed.ok());
	CHECK(parsed.warnings.empty());
	CHECK_EQ(parsed.settings.recent.staySecondsAfterClose, 120);
	CHECK(parsed.settings.recent.scope == Purple::RecentScope::AnyOpenChat);

	// The same duration spellings as [peek] auto_off, which is the point of
	// reusing ParseDuration rather than inventing a seconds-only key.
	const auto units = [](const QString &text) {
		const auto one = Parse(
			u"[recent]\nstay_visible_after_close = \"%1\"\n"_q.arg(text));
		return one.settings.recent.staySecondsAfterClose;
	};
	CHECK_EQ(units(u"90s"_q), 90);
	CHECK_EQ(units(u"1h"_q), 3600);
	CHECK_EQ(units(u"45"_q), 45);
	CHECK_EQ(units(u"off"_q), 0);

	// Unparseable leaves it off and says so, rather than guessing at a number.
	const auto broken = Parse(
		u"[recent]\nstay_visible_after_close = \"soon\"\n"_q);
	CHECK(broken.ok());
	CHECK_EQ(broken.settings.recent.staySecondsAfterClose, 0);
	CHECK_EQ(int(broken.warnings.size()), 1);

	// A misspelt scope keeps the narrow default rather than the widest one.
	const auto scope = Parse(uR"(
[recent]
stay_visible_after_close = "2m"
applies_to = "any_chat"
)"_q);
	CHECK(scope.ok());
	CHECK_EQ(int(scope.warnings.size()), 1);
	CHECK(scope.settings.recent.scope == Purple::RecentScope::AlreadyInView);

	CHECK(Purple::ParseRecentScope(u"  ANY_OPEN_CHAT  "_q)
		== Purple::RecentScope::AnyOpenChat);
	CHECK(Purple::ParseRecentScope(
		u"any_open_chat_except_in_folder"_q)
			== Purple::RecentScope::AnyOpenChatExceptInFolder);
	CHECK(!Purple::ParseRecentScope(QString()).has_value());

	// Round trip, so a warning can name back what it kept.
	for (const auto value : {
			Purple::RecentScope::AlreadyInView,
			Purple::RecentScope::AnyOpenChat,
			Purple::RecentScope::AnyOpenChatExceptInFolder }) {
		CHECK(Purple::ParseRecentScope(Purple::RecentScopeName(value))
			== value);
	}
}

void TestHideScope() {
	Begin("hide scope");

	// The default is the middle one: a "hide until" leaves the chat on its
	// folder tabs and stops it counting there. A file that says nothing gets it
	// without having to know the key exists.
	const auto silent = Parse(u"[presets.work]\nlist_order = []\n"_q);
	CHECK(silent.ok());
	CHECK(silent.settings.overrides.hideScope
		== Purple::HideScope::KeepInFolderUncounted);

	const auto parsed = Parse(uR"(
[overrides]
hide_scope = "hide_everywhere"
)"_q);
	CHECK(parsed.ok());
	CHECK(parsed.warnings.empty());
	CHECK(parsed.settings.overrides.hideScope
		== Purple::HideScope::Everywhere);

	const auto keep = Parse(
		u"[overrides]\nhide_scope = \"keep_in_folder\"\n"_q);
	CHECK(keep.ok());
	CHECK(keep.settings.overrides.hideScope
		== Purple::HideScope::KeepInFolder);

	// A misspelling keeps the default and says which one it kept, rather than
	// quietly picking the widest reading of a key about hiding things.
	const auto broken = Parse(
		u"[overrides]\nhide_scope = \"everywhere\"\n"_q);
	CHECK(broken.ok());
	CHECK_EQ(int(broken.warnings.size()), 1);
	CHECK(broken.settings.overrides.hideScope
		== Purple::HideScope::KeepInFolderUncounted);
	CHECK(WarnsAbout(broken, u"keep_in_folder_but_exclude_from_badge_count"_q));

	// Not a table at all is a warning too, not a parse failure.
	const auto wrong = Parse(u"overrides = true\n"_q);
	CHECK(wrong.ok());
	CHECK_EQ(int(wrong.warnings.size()), 1);
	CHECK(wrong.settings.overrides.hideScope
		== Purple::HideScope::KeepInFolderUncounted);

	CHECK(Purple::ParseHideScope(u"  HIDE_EVERYWHERE  "_q)
		== Purple::HideScope::Everywhere);
	CHECK(!Purple::ParseHideScope(QString()).has_value());

	// Round trip, so a warning can name back what it kept.
	for (const auto value : {
			Purple::HideScope::Everywhere,
			Purple::HideScope::KeepInFolderUncounted,
			Purple::HideScope::KeepInFolder }) {
		CHECK(Purple::ParseHideScope(Purple::HideScopeName(value)) == value);
	}
}

void TestScheduleTarget() {
	Begin("schedule target");

	// 2026-08-17 is a Monday, so dayOfWeek() runs 1..7 across that week.
	const auto at = [](int weekday, int hour, int minute) {
		return QDateTime(
			QDate(2026, 8, 16 + weekday),
			QTime(hour, minute));
	};
	const auto rule = [](
			std::vector<int> days,
			int from,
			int till,
			const QString &preset) {
		auto result = Purple::ScheduleRule();
		result.days = std::move(days);
		result.from = from;
		result.till = till;
		result.preset = preset;
		return result;
	};
	const auto target = [](
			const Purple::Schedule &schedule,
			const QDateTime &when) {
		const auto result = Purple::ScheduleTarget(schedule, when);
		return result ? *result : u"<nothing>"_q;
	};

	// A schedule with no rules drives nothing. It must not read as "wants
	// Normal", or an empty section would override every other way of choosing.
	auto schedule = Purple::Schedule();
	CHECK_EQ(target(schedule, at(1, 10, 0)), u"<nothing>"_q);

	schedule.rules.push_back(rule({ 1, 2, 3, 4, 5 }, 9 * 60, 17 * 60, u"work"_q));
	CHECK_EQ(target(schedule, at(1, 10, 0)), u"work"_q);

	// Half-open: the start belongs to the window, the end does not, so
	// neighbouring windows cannot both claim the moment they meet.
	CHECK_EQ(target(schedule, at(1, 9, 0)), u"work"_q);
	CHECK_EQ(target(schedule, at(1, 8, 59)), u"normal"_q);
	CHECK_EQ(target(schedule, at(1, 16, 59)), u"work"_q);
	CHECK_EQ(target(schedule, at(1, 17, 0)), u"normal"_q);

	// Saturday is not listed.
	CHECK_EQ(target(schedule, at(6, 10, 0)), u"normal"_q);

	// Switching the whole schedule off is not the same as it wanting Normal:
	// it stops driving, and whatever is active stays.
	schedule.enabled = false;
	CHECK_EQ(target(schedule, at(1, 10, 0)), u"<nothing>"_q);
	schedule.enabled = true;

	// A window crossing midnight belongs to the day it starts on.
	auto night = Purple::Schedule();
	night.rules.push_back(rule({ 1 }, 22 * 60, 6 * 60, u"sleep"_q));
	CHECK_EQ(target(night, at(1, 22, 0)), u"sleep"_q);
	CHECK_EQ(target(night, at(1, 23, 59)), u"sleep"_q);
	CHECK_EQ(target(night, at(2, 5, 59)), u"sleep"_q);
	CHECK_EQ(target(night, at(2, 6, 0)), u"normal"_q);

	// ... and not to the morning of the day it is listed for, which is the
	// trap: Monday 02:00 is the tail of a Sunday window, not of Monday's.
	CHECK_EQ(target(night, at(1, 2, 0)), u"normal"_q);
	night.rules[0].days = { 7 };
	CHECK_EQ(target(night, at(1, 2, 0)), u"sleep"_q);

	// First match wins in file order, and a disabled rule is not a match.
	auto several = Purple::Schedule();
	several.rules.push_back(rule({ 1 }, 9 * 60, 17 * 60, u"first"_q));
	several.rules.push_back(rule({ 1 }, 9 * 60, 17 * 60, u"second"_q));
	CHECK_EQ(target(several, at(1, 10, 0)), u"first"_q);
	several.rules[0].enabled = false;
	CHECK_EQ(target(several, at(1, 10, 0)), u"second"_q);

	// The rule behind that answer, for a screen that has to say which window it
	// is in rather than only which preset it wants.
	const auto now = Purple::ScheduleRuleNow(several, at(1, 10, 0));
	CHECK(now != nullptr);
	CHECK_EQ(now->preset, u"second"_q);
	CHECK_EQ(now->till, 17 * 60);
	CHECK(now == &several.rules[1]);
	CHECK(!Purple::ScheduleRuleNow(several, at(6, 10, 0)));
	several.enabled = false;
	CHECK(!Purple::ScheduleRuleNow(several, at(1, 10, 0)));
}

void TestScheduleOutside() {
	Begin("schedule outside");

	// Nothing said: the fallback is what every file written before the key
	// existed meant, so an old settings.toml keeps behaving the same way.
	const auto plain = Parse(uR"(
[presets.work]
list_order = []

[[schedule.rules]]
days   = ["mon"]
from   = "09:00"
to     = "17:00"
preset = "work"
)"_q);
	CHECK(plain.ok());
	CHECK_EQ(plain.settings.schedule.outside, u"normal"_q);

	const auto named = Parse(uR"(
[presets.work]
list_order = []

[presets.home]
list_order = []

[schedule]
outside = "home"

[[schedule.rules]]
days   = ["mon"]
from   = "09:00"
to     = "17:00"
preset = "work"
)"_q);
	CHECK(named.ok());
	CHECK(!WarnsAbout(named, u"outside"_q));
	CHECK_EQ(named.settings.schedule.outside, u"home"_q);

	// A name nothing backs cannot be skipped the way a rule can - something has
	// to be wanted outside the windows - so it says so and takes normal.
	const auto ghost = Parse(uR"(
[schedule]
outside = "ghost"
)"_q);
	CHECK(ghost.ok());
	CHECK(WarnsAbout(ghost, u"does not exist, using normal"_q));
	CHECK(WarnsAbout(ghost, u"[schedule] outside: preset 'ghost'"_q));
	CHECK_EQ(ghost.settings.schedule.outside, u"normal"_q);

	// 2026-08-17 is a Monday, so dayOfWeek() runs 1..7 across that week.
	const auto at = [](int weekday, int hour, int minute) {
		return QDateTime(
			QDate(2026, 8, 16 + weekday),
			QTime(hour, minute));
	};
	const auto target = [](
			const Purple::Schedule &schedule,
			const QDateTime &when) {
		const auto result = Purple::ScheduleTarget(schedule, when);
		return result ? *result : u"<nothing>"_q;
	};
	const auto &schedule = named.settings.schedule;
	CHECK_EQ(target(schedule, at(1, 10, 0)), u"work"_q);
	CHECK_EQ(target(schedule, at(1, 17, 0)), u"home"_q);
	CHECK_EQ(target(schedule, at(6, 10, 0)), u"home"_q);

	// The boundary rule, restated with an outside preset that is not Normal.
	// Nine o'clock is a window starting and takes over whatever is running;
	// five o'clock is a window ending, so it lands only on a preset the
	// schedule itself put there. Getting this wrong is what a client testing
	// `target != normal' would do: it would read Home as a start and undo a
	// preset the user chose by hand.
	using Source = Purple::PresetSource;
	CHECK(Purple::ScheduleApplies(schedule, u"work"_q, Source::Manual));
	CHECK(Purple::ScheduleApplies(schedule, u"work"_q, Source::Schedule));
	CHECK(!Purple::ScheduleApplies(schedule, u"home"_q, Source::Manual));
	CHECK(Purple::ScheduleApplies(schedule, u"home"_q, Source::Schedule));

	// Focus outranks both directions.
	CHECK(!Purple::ScheduleApplies(schedule, u"work"_q, Source::Focus));
	CHECK(!Purple::ScheduleApplies(schedule, u"home"_q, Source::Focus));

	// And with the default outside the rule is exactly what it always was.
	const auto normal = plain.settings.schedule;
	CHECK(Purple::ScheduleApplies(normal, u"work"_q, Source::Manual));
	CHECK(!Purple::ScheduleApplies(normal, u"normal"_q, Source::Manual));
	CHECK(Purple::ScheduleApplies(normal, u"normal"_q, Source::Schedule));
}

void TestSchedulePauseUntil() {
	Begin("schedule pause until");

	// 2026-08-17 is a Monday, so dayOfWeek() runs 1..7 across that week.
	const auto at = [](int weekday, int hour, int minute) {
		return QDateTime(
			QDate(2026, 8, 16 + weekday),
			QTime(hour, minute));
	};
	const auto seconds = [](const QDateTime &when) {
		return int64(when.toSecsSinceEpoch());
	};

	auto paused = Purple::State();
	paused.schedulePaused = true;
	paused.schedulePausedUntil = seconds(at(1, 10, 0));
	CHECK(!Purple::ScheduleUnpauseDue(paused, seconds(at(1, 9, 59))));
	CHECK(Purple::ScheduleUnpauseDue(paused, seconds(at(1, 10, 0))));
	CHECK(Purple::ScheduleUnpauseDue(paused, seconds(at(2, 10, 0))));

	// Zero is "until I say otherwise" and no clock reaches it.
	auto openEnded = Purple::State();
	openEnded.schedulePaused = true;
	CHECK(!Purple::ScheduleUnpauseDue(openEnded, seconds(at(7, 23, 59))));

	// A deadline left behind by a pause that was already lifted is not a
	// reason to do anything, so nothing has to remember to clear it in order.
	auto running = Purple::State();
	running.schedulePausedUntil = seconds(at(1, 9, 0));
	CHECK(!Purple::ScheduleUnpauseDue(running, seconds(at(1, 10, 0))));

	const auto settings = Parse(uR"(
[presets.work]
list_order = []

[presets.home]
list_order = []

[schedule]
outside = "home"

[[schedule.rules]]
days   = ["mon"]
from   = "09:00"
to     = "17:00"
preset = "work"
)"_q);
	CHECK(settings.ok());
	const auto &schedule = settings.settings.schedule;

	// The tick both clients run, written out in the order they run it: expire
	// the pause first, then decide by the ordinary boundary rule. That order is
	// what makes an expiry catch up in the same tick rather than sitting on the
	// old preset until the next window edge.
	const auto tick = [&](Purple::State &state, const QDateTime &now) {
		if (Purple::ScheduleUnpauseDue(state, seconds(now))) {
			state.schedulePaused = false;
			state.schedulePausedUntil = 0;
		}
		if (state.schedulePaused) {
			return;
		}
		const auto target = Purple::ScheduleTarget(schedule, now);
		if (!target || *target == state.scheduleTarget) {
			return;
		}
		state.scheduleTarget = *target;
		if (Purple::ScheduleApplies(schedule, *target, state.activeSource)) {
			state.activePreset = *target;
			state.activeSource = Purple::PresetSource::Schedule;
		}
	};

	auto state = Purple::State();
	state.activePreset = u"home"_q;
	state.activeSource = Purple::PresetSource::Schedule;
	state.scheduleTarget = u"home"_q;
	state.schedulePaused = true;
	state.schedulePausedUntil = seconds(at(1, 10, 0));

	// Inside the pause the schedule drives nothing, even mid-window.
	tick(state, at(1, 9, 30));
	CHECK(state.schedulePaused);
	CHECK_EQ(state.activePreset, u"home"_q);
	CHECK_EQ(state.scheduleTarget, u"home"_q);

	// At the deadline the pause goes and the window it slept through is caught
	// up on at once - one tick, not two.
	tick(state, at(1, 10, 0));
	CHECK(!state.schedulePaused);
	CHECK_EQ(state.schedulePausedUntil, int64(0));
	CHECK_EQ(state.activePreset, u"work"_q);
	CHECK_EQ(state.scheduleTarget, u"work"_q);

	// And the same pause with no deadline is still paused a week later.
	auto open = Purple::State();
	open.activePreset = u"home"_q;
	open.activeSource = Purple::PresetSource::Schedule;
	open.scheduleTarget = u"home"_q;
	open.schedulePaused = true;
	tick(open, at(1, 10, 0));
	tick(open, at(7, 10, 0));
	CHECK(open.schedulePaused);
	CHECK_EQ(open.activePreset, u"home"_q);

	// A preset chosen by hand while paused survives the expiry, because the
	// catch-up is a window ENDING only when it aims at `outside'. Here it aims
	// at work, a window starting, so it takes over - and the mirror case, the
	// five o'clock end, leaves a hand-chosen preset alone.
	auto manual = Purple::State();
	manual.activePreset = u"work"_q;
	manual.activeSource = Purple::PresetSource::Manual;
	manual.scheduleTarget = u"work"_q;
	manual.schedulePaused = true;
	manual.schedulePausedUntil = seconds(at(1, 17, 0));
	tick(manual, at(1, 17, 0));
	CHECK(!manual.schedulePaused);
	CHECK_EQ(manual.scheduleTarget, u"home"_q);
	CHECK_EQ(manual.activePreset, u"work"_q);
	CHECK(manual.activeSource == Purple::PresetSource::Manual);
}

void TestRulesets() {
	Begin("schedule rulesets");

	const auto parsed = Parse(RulesetsExample());
	CHECK(parsed.ok());
	const auto &schedule = parsed.settings.schedule;

	// The flat array is still read as the flat array, and it is ALSO the first
	// ruleset: everything downstream then has one shape to work with, and a
	// file that never heard of rulesets resolves through the same path.
	CHECK_EQ(schedule.rules.size(), size_t(1));
	CHECK_EQ(schedule.rulesets.size(), size_t(4));
	CHECK(schedule.rulesets[0].implicit());
	CHECK_EQ(schedule.rulesets[0].name, u"rules"_q);
	CHECK_EQ(schedule.rulesets[0].device, u"any"_q);
	CHECK(schedule.rulesets[0].mode == Purple::RulesetMode::Enabled);
	CHECK(!schedule.rulesets[0].outside.has_value());
	CHECK_EQ(schedule.rulesets[0].rules.size(), size_t(1));

	CHECK(!schedule.rulesets[1].implicit());
	CHECK_EQ(schedule.rulesets[1].name, u"phone"_q);
	CHECK_EQ(schedule.rulesets[1].device, u"mobile"_q);
	CHECK(schedule.rulesets[1].mode == Purple::RulesetMode::Enabled);
	CHECK_EQ(schedule.rulesets[1].outside.value_or(QString()), u"home"_q);
	CHECK_EQ(schedule.rulesets[1].sourceIndex, 0);
	CHECK_EQ(schedule.rulesets[1].rules.size(), size_t(1));
	CHECK_EQ(schedule.rulesets[1].rules[0].from, 8 * 60);

	CHECK(schedule.rulesets[2].mode == Purple::RulesetMode::Always);
	CHECK_EQ(schedule.rulesets[2].device, u"any"_q);
	CHECK_EQ(schedule.rulesets[3].device, u"mac-3f9a"_q);
	CHECK_EQ(schedule.rulesets[3].sourceIndex, 2);

	// A rule inside a ruleset is addressed within that ruleset, so a broken
	// rule in one cannot move the addresses in another.
	CHECK_EQ(schedule.rulesets[1].rules[0].sourceIndex, 0);
	CHECK_EQ(schedule.rulesets[2].rules[0].sourceIndex, 0);

	// A ruleset with no name is no use to anything that has to edit it, and one
	// whose name is already taken would make the address ambiguous. Both are
	// skipped, named by the position a person can count to in the file.
	const auto broken = Parse(uR"(
[presets.work]
list_order = []

[[schedule.rulesets]]
device = "mobile"

[[schedule.rulesets]]
name = "phone"

[[schedule.rulesets]]
name = "PHONE"

[[schedule.rulesets]]
name   = "odd"
device = ""
mode   = "sometimes"
)"_q);
	CHECK(broken.ok());
	CHECK(WarnsAbout(broken, u"schedule ruleset 1: needs a 'name'"_q));
	CHECK(WarnsAbout(
		broken,
		u"schedule ruleset 3: another ruleset is already called 'PHONE'"_q));
	CHECK_EQ(broken.settings.schedule.rulesets.size(), size_t(2));
	CHECK_EQ(broken.settings.schedule.rulesets[0].name, u"phone"_q);

	const auto &odd = broken.settings.schedule.rulesets[1];
	CHECK_EQ(odd.name, u"odd"_q);
	CHECK_EQ(odd.device, u"any"_q);
	CHECK(WarnsAbout(broken, u"'device' is empty, applying it to every"_q));

	// A mode nobody can read switches the ruleset OFF rather than on: running
	// rules whose reason cannot be read back is the worse way to be wrong.
	CHECK(odd.mode == Purple::RulesetMode::Disabled);
	CHECK(WarnsAbout(broken, u"'mode' should be \"disabled\""_q));

	// A ruleset's outside is checked like any other preset reference, and when
	// it names nothing it falls back to the schedule's rather than to normal.
	const auto ghost = Parse(uR"(
[presets.work]
list_order = []

[[schedule.rulesets]]
name    = "phone"
outside = "ghost"
)"_q);
	CHECK(ghost.ok());
	CHECK(WarnsAbout(ghost, u"outside: preset 'ghost' does not exist"_q));
	CHECK(!ghost.settings.schedule.rulesets[0].outside.has_value());
}

void TestActiveSchedule() {
	Begin("active schedule");

	const auto parsed = Parse(RulesetsExample());
	CHECK(parsed.ok());
	const auto &schedule = parsed.settings.schedule;
	const auto phone = Device(u"pixel-11ff"_q, u"android"_q, u"mobile"_q);
	const auto laptop = Device(u"mac-3f9a"_q, u"macos"_q, u"desktop"_q);
	const auto other = Device(u"win-0001"_q, u"windows"_q, u"desktop"_q);

	CHECK_EQ(Purple::RulesetSpecificity(schedule.rulesets[0]), 0);
	CHECK_EQ(Purple::RulesetSpecificity(schedule.rulesets[1]), 1);
	CHECK_EQ(Purple::RulesetSpecificity(schedule.rulesets[3]), 3);
	CHECK(Purple::RulesetAppliesTo(schedule.rulesets[1], phone));
	CHECK(!Purple::RulesetAppliesTo(schedule.rulesets[1], laptop));
	CHECK(Purple::RulesetAppliesTo(schedule.rulesets[3], laptop));
	CHECK(!Purple::RulesetAppliesTo(schedule.rulesets[3], other));

	// On the phone the mobile ruleset REPLACES the flat one rather than piling
	// on top of it - that is what makes "one file, refined per device" work -
	// while the `always' ruleset comes along whatever won.
	const auto onPhone = Purple::ActiveSchedule(schedule, phone);
	CHECK_EQ(onPhone.chosen.size(), size_t(2));
	CHECK_EQ(onPhone.chosen[0]->name, u"phone"_q);
	CHECK_EQ(onPhone.chosen[1]->name, u"quiet"_q);
	CHECK_EQ(onPhone.rules.size(), size_t(2));
	CHECK_EQ(onPhone.rules[0]->from, 8 * 60);
	CHECK_EQ(onPhone.outside, u"home"_q);

	// The laptop's own ruleset is the most specific thing it has, so the flat
	// rules go the same way the phone's did.
	const auto onLaptop = Purple::ActiveSchedule(schedule, laptop);
	CHECK_EQ(onLaptop.chosen.size(), size_t(2));
	CHECK_EQ(onLaptop.chosen[0]->name, u"the laptop"_q);
	CHECK_EQ(onLaptop.chosen[1]->name, u"quiet"_q);
	CHECK_EQ(onLaptop.outside, u"normal"_q);

	// A device nothing was written for keeps the flat rules, which is exactly
	// what the file meant before any of this existed.
	const auto onOther = Purple::ActiveSchedule(schedule, other);
	CHECK_EQ(onOther.chosen.size(), size_t(2));
	CHECK(onOther.chosen[0]->implicit());
	CHECK_EQ(onOther.chosen[1]->name, u"quiet"_q);
	CHECK_EQ(onOther.rules.size(), size_t(2));
	CHECK_EQ(onOther.rules[0]->from, 9 * 60);
	CHECK_EQ(onOther.outside, u"normal"_q);

	// And a caller with no identity to offer sees only what asked for no device
	// in particular - the same answer as a file written before rulesets.
	const auto nowhere = Purple::ActiveSchedule(
		schedule,
		Purple::DeviceIdentity());
	CHECK_EQ(nowhere.chosen.size(), size_t(2));
	CHECK(nowhere.chosen[0]->implicit());

	// 2026-08-17 is a Monday, so dayOfWeek() runs 1..7 across that week.
	const auto at = [](int weekday, int hour, int minute) {
		return QDateTime(
			QDate(2026, 8, 16 + weekday),
			QTime(hour, minute));
	};
	const auto target = [&](
			const Purple::DeviceIdentity &device,
			const QDateTime &when) {
		const auto result = Purple::ScheduleTarget(schedule, when, device);
		return result ? *result : u"<nothing>"_q;
	};

	// Eight in the morning: the phone is working, and the desktop that only has
	// the flat rules is between windows - so it takes the outside preset even
	// though a rule covering the moment is right there in the file, because it
	// is a rule for somebody else's device.
	CHECK_EQ(target(phone, at(1, 8, 0)), u"work"_q);
	CHECK_EQ(target(other, at(1, 8, 0)), u"normal"_q);
	CHECK_EQ(target(laptop, at(1, 8, 0)), u"normal"_q);

	// Ten o'clock: everybody is working, each by its own rule, and the laptop
	// by the one written for it.
	CHECK_EQ(target(phone, at(1, 10, 0)), u"work"_q);
	CHECK_EQ(target(other, at(1, 10, 0)), u"work"_q);
	CHECK_EQ(target(laptop, at(1, 10, 30)), u"home"_q);

	// Six in the evening, and the phone's own outside is what it goes back to.
	CHECK_EQ(target(phone, at(1, 18, 0)), u"home"_q);
	CHECK_EQ(target(other, at(1, 18, 0)), u"normal"_q);

	// The `always' ruleset runs on all of them, at an hour none of the rest
	// covers.
	CHECK_EQ(target(phone, at(1, 23, 0)), u"home"_q);
	CHECK_EQ(target(other, at(1, 23, 0)), u"home"_q);
	CHECK_EQ(target(laptop, at(1, 23, 0)), u"home"_q);

	// The rule behind the answer, for a screen that has to name the window.
	const auto rule = Purple::ScheduleRuleNow(schedule, at(1, 8, 30), phone);
	CHECK(rule != nullptr);
	CHECK_EQ(rule->till, 18 * 60);
	CHECK(!Purple::ScheduleRuleNow(schedule, at(1, 8, 30), other));

	// The boundary rule reads the outside this device settled on, not the one
	// at the top of the file.
	using Source = Purple::PresetSource;
	CHECK(!Purple::ScheduleApplies(onPhone, u"home"_q, Source::Manual));
	CHECK(Purple::ScheduleApplies(onPhone, u"home"_q, Source::Schedule));
	CHECK(Purple::ScheduleApplies(onPhone, u"normal"_q, Source::Manual));
	CHECK(!Purple::ScheduleApplies(onOther, u"normal"_q, Source::Manual));
	CHECK(Purple::ScheduleApplies(onOther, u"home"_q, Source::Manual));

	// A ruleset switched off is written down and not run, and one for a device
	// nobody is holding is simply never chosen.
	const auto off = Parse(uR"(
[presets.work]
list_order = []

[[schedule.rulesets]]
name = "phone"
mode = "disabled"

[[schedule.rulesets.rules]]
days   = ["mon"]
from   = "09:00"
to     = "17:00"
preset = "work"
)"_q);
	CHECK(off.ok());
	CHECK_EQ(off.settings.schedule.rulesets.size(), size_t(1));
	const auto quiet = Purple::ActiveSchedule(off.settings.schedule, phone);
	CHECK(quiet.chosen.empty());
	CHECK(quiet.rules.empty());

	// Nothing at all rather than the outside preset: a file that describes only
	// other people's devices must not drive this one.
	CHECK(!Purple::ScheduleTarget(off.settings.schedule, at(1, 10, 0), phone));
}

void TestDevices() {
	Begin("devices");

	const auto parsed = Parse(uR"(
[devices]
"mac-3f9a"   = "Work laptop"
"pixel-11ff" = "Phone"
bare         = 7
)"_q);
	CHECK(parsed.ok());
	CHECK_EQ(parsed.settings.devices.size(), size_t(2));
	CHECK(parsed.settings.device(u"mac-3f9a"_q) != nullptr);
	CHECK_EQ(parsed.settings.device(u"MAC-3F9A"_q)->label, u"Work laptop"_q);
	CHECK(!parsed.settings.device(u"nothing"_q));
	CHECK(WarnsAbout(parsed, u"'bare' should be a name in quotes"_q));
}

void TestResolvedCache() {
	Begin("resolved cache");

	const auto parsed = Parse(Presets());
	const auto work = Purple::Resolve(parsed.settings, u"work"_q);
	const auto cache = Purple::ToCache(*work);
	CHECK(cache.valid());
	CHECK_EQ(cache.preset, u"work"_q);
	CHECK_EQ(cache.lists.size(), work->lists.size());

	// It survives the trip through state.toml, which is the point: a settings
	// file that stops parsing between runs must not reshuffle the chat list.
	const auto text = Purple::SerializeState([&] {
		auto state = Purple::State();
		state.activePreset = u"work"_q;
		state.resolvedCache = cache;
		return state;
	}());
	const auto reloaded = Purple::ParseState(text, u"state.toml"_q);
	const auto restored = Purple::FromCache(reloaded.resolvedCache);
	CHECK(restored.has_value());
	CHECK_EQ(restored->preset, u"work"_q);
	CHECK_EQ(Mode(restored->list(u"private"_q)->show),
		Mode(Purple::ShowMode::Always));
	CHECK(!restored->list(u"private"_q)->notify);
	CHECK_EQ(Mode(restored->list(u"channels"_q)->show),
		Mode(Purple::ShowMode::Never));
	CHECK_EQ(Mode(restored->list(u"colleagues"_q)->show),
		Mode(Purple::ShowMode::Mention));
	CHECK_EQ(int(restored->folders.size()), 1);

	// include_in_main_view_p survives the cache, since a broken settings.toml
	// must not quietly start hiding the chats a folder was pulling in.
	auto folders = std::vector<Purple::PresetFolder>{
		{ .name = u"Work"_q },
		{ .name = u"Family"_q, .include = Purple::FolderInclude::All },
		{ .name = u"Loud"_q, .include = Purple::FolderInclude::None },
	};
	CHECK_EQ(Purple::ExemptFolderList(folders).size(), size_t(1));
	CHECK_EQ(Purple::ExemptFolderList(folders).front().name, u"Family"_q);
	CHECK(Purple::ExemptFolderList({}).empty());

	// notify and include_in_main_view are independent: a folder can be silenced
	// without being pulled in, and pulled in without being silenced.
	folders.push_back({ .name = u"Noise"_q, .notify = false });
	folders.push_back({ .name = u"Loud2"_q, .notify = true });
	CHECK_EQ(Purple::SilencedFolderNames(folders).size(), size_t(1));
	CHECK_EQ(Purple::SilencedFolderNames(folders).front(), u"Noise"_q);
	CHECK_EQ(Purple::ExemptFolderList(folders).size(), size_t(1));
	CHECK(Purple::SilencedFolderNames({}).empty());

	// badge_p is its own axis: a folder can be pulled in, silenced and quiet
	// independently, and only an explicit false makes it quiet.
	CHECK(Purple::QuietFolderNames(folders).empty());
	folders.push_back({ .name = u"Hush"_q, .badge = false });
	folders.push_back({ .name = u"Counted"_q, .badge = true });
	CHECK_EQ(Purple::QuietFolderNames(folders).size(), size_t(1));
	CHECK_EQ(Purple::QuietFolderNames(folders).front(), u"Hush"_q);
	CHECK(Purple::QuietFolderNames({}).empty());

	// "pinned" and a mode ride along on the exempt entry, so the one walk in
	// History::purpleExemptFolderMode() has everything it needs.
	folders.push_back({
		.name = u"Music"_q,
		.showMode = Purple::ShowMode::Message,
		.include = Purple::FolderInclude::Pinned,
	});
	folders.push_back({ .name = u"Alone"_q });
	const auto exempt = Purple::ExemptFolderList(folders);
	CHECK_EQ(exempt.size(), size_t(2));
	CHECK_EQ(exempt.back().name, u"Music"_q);
	CHECK_EQ(int(exempt.back().include), int(Purple::FolderInclude::Pinned));
	CHECK_EQ(Mode(exempt.back().showMode), Mode(Purple::ShowMode::Message));
	CHECK_EQ(int(exempt.front().include), int(Purple::FolderInclude::All));
	CHECK(!exempt.front().showMode.has_value());

	auto withFolders = *work;
	withFolders.folders = folders;
	const auto cachedFolders = Purple::FromCache(
		Purple::ToCache(withFolders));
	CHECK(cachedFolders.has_value());
	CHECK_EQ(cachedFolders->exemptFolders.size(), size_t(2));
	CHECK_EQ(cachedFolders->exemptFolders.front().name, u"Family"_q);
	CHECK_EQ(cachedFolders->exemptFolders.back().name, u"Music"_q);
	CHECK_EQ(
		int(cachedFolders->exemptFolders.back().include),
		int(Purple::FolderInclude::Pinned));
	CHECK_EQ(
		Mode(cachedFolders->exemptFolders.back().showMode),
		Mode(Purple::ShowMode::Message));
	CHECK_EQ(cachedFolders->silencedFolders.size(), size_t(1));
	CHECK_EQ(cachedFolders->silencedFolders.front(), u"Noise"_q);
	CHECK_EQ(cachedFolders->quietFolders.size(), size_t(1));
	CHECK_EQ(cachedFolders->quietFolders.front(), u"Hush"_q);
	CHECK_EQ(int(cachedFolders->folders.size()), 9);

	// The marker survives too, so a broken reload does not take the whole
	// folder strip away along with everything else it cannot read.
	auto everyFolder = *work;
	everyFolder.folders = { { .name = Purple::AllFoldersName() } };
	const auto cachedAll = Purple::FromCache(Purple::ToCache(everyFolder));
	CHECK(cachedAll.has_value());
	CHECK_EQ(int(cachedAll->folders.size()), 1);
	CHECK(Purple::IsAllFolders(cachedAll->folders[0]));

	// Normal caches nothing - there is no resolution to remember.
	const auto normal = Purple::Resolve(parsed.settings, u"normal"_q);
	CHECK(!Purple::ToCache(*normal).valid());
	CHECK(!Purple::FromCache(Purple::ResolvedCache()).has_value());
}

void TestLastSeenKeys() {
	Begin("last seen keys");

	// A file that says nothing gets the documented defaults: the reason line
	// on, the trade offered, and the three durations the plan settled on.
	const auto silent = Parse(u"[presets.work]\nlist_order = []\n"_q);
	CHECK(silent.ok());
	CHECK(silent.settings.lastSeen.reasons);
	CHECK(silent.settings.lastSeen.trade);
	CHECK_EQ(silent.settings.lastSeen.tradeHoldSeconds, 10);
	CHECK_EQ(silent.settings.lastSeen.tradeRememberSeconds, 24 * 3600);
	CHECK_EQ(silent.settings.lastSeen.tradeCooldownSeconds, 5 * 60);

	const auto parsed = Parse(uR"(
[last_seen]
reasons_p = false
trade_p = false
trade_hold = "30s"
trade_remember = "2h"
trade_cooldown = "90s"
)"_q);
	CHECK(parsed.ok());
	CHECK(parsed.warnings.empty());
	CHECK(!parsed.settings.lastSeen.reasons);
	CHECK(!parsed.settings.lastSeen.trade);
	CHECK_EQ(parsed.settings.lastSeen.tradeHoldSeconds, 30);
	CHECK_EQ(parsed.settings.lastSeen.tradeRememberSeconds, 2 * 3600);
	CHECK_EQ(parsed.settings.lastSeen.tradeCooldownSeconds, 90);

	// The same duration spellings as [peek] auto_off and [recent], which is
	// the point of reusing ParseDuration rather than inventing seconds keys.
	const auto hold = [](const QString &text) {
		return Parse(u"[last_seen]\ntrade_hold = \"%1\"\n"_q.arg(text))
			.settings.lastSeen.tradeHoldSeconds;
	};
	CHECK_EQ(hold(u"45"_q), 45);
	CHECK_EQ(hold(u"2m"_q), 120);
	CHECK_EQ(hold(u"1h"_q), 3600);
	CHECK_EQ(hold(u"off"_q), 0);

	// Unparseable keeps the default and says so, rather than guessing.
	const auto broken = Parse(u"[last_seen]\ntrade_remember = \"soon\"\n"_q);
	CHECK(broken.ok());
	CHECK_EQ(broken.settings.lastSeen.tradeRememberSeconds, 24 * 3600);
	CHECK_EQ(int(broken.warnings.size()), 1);

	const auto wrongType = Parse(u"last_seen = 3\n"_q);
	CHECK(wrongType.ok());
	CHECK(WarnsAbout(wrongType, u"'last_seen' should be a table"_q));
	CHECK(wrongType.settings.lastSeen.reasons);
}

void TestLastSeenReasons() {
	Begin("last seen reasons");

	using Reason = Purple::LastSeenReason;

	// An exact was_online explains itself, so there is nothing to append -
	// and it stays nothing even if a by_me flag rode along, which would be the
	// server describing a coarsening that did not happen.
	CHECK(Purple::ReasonFor(true, false, false) == Reason::None);
	CHECK(Purple::ReasonFor(true, true, true) == Reason::None);

	// Coarse because of our own rules is the one case with something to offer.
	CHECK(Purple::ReasonFor(false, true, true) == Reason::ByMe);

	// Coarse and not by us is their setting, and nothing to say about it.
	CHECK(Purple::ReasonFor(false, true, false) == Reason::HiddenByThem);

	// userStatusEmpty - "a long time ago". Inactivity and a block look
	// identical here and the server does not say which, so the fork never
	// infers a block from it whatever else it was handed.
	CHECK(Purple::ReasonFor(false, false, false) == Reason::None);
	CHECK(Purple::ReasonFor(false, false, true) == Reason::None);
}

void TestLastSeenTrades() {
	Begin("last seen trades");

	auto state = Purple::State();
	const auto now = int64(1788000000);
	CHECK(!Purple::RememberedTrade(state, 7, now, 24 * 3600).has_value());
	CHECK(Purple::TradeAllowed(state, 7, now, 300));

	Purple::RememberTrade(state, 7, now - 60, now - 300);
	CHECK_EQ(state.lastSeenTrades.size(), size_t(1));

	const auto remembered = Purple::RememberedTrade(state, 7, now, 24 * 3600);
	CHECK(remembered.has_value());
	CHECK_EQ(remembered->peer, int64(7));
	CHECK_EQ(remembered->readAtUnix, now - 60);
	CHECK_EQ(remembered->wasOnlineUnix, now - 300);

	// Older than trade_remember is dropped rather than shown as older and
	// older news, and staleness is decided at read time - so shortening the
	// key stops showing it without touching the file.
	CHECK(!Purple::RememberedTrade(state, 7, now, 30).has_value());
	CHECK(!Purple::RememberedTrade(state, 7, now, 0).has_value());
	CHECK(!Purple::RememberedTrade(state, 8, now, 24 * 3600).has_value());

	// One record per person: a second trade replaces the first, so the list is
	// bounded by who you have traded with and not by how often.
	Purple::RememberTrade(state, 7, now, now - 10);
	CHECK_EQ(state.lastSeenTrades.size(), size_t(1));
	CHECK_EQ(state.lastSeenTrades[0].wasOnlineUnix, now - 10);

	// One trade per person per cooldown.
	CHECK(!Purple::TradeAllowed(state, 7, now, 300));
	CHECK(!Purple::TradeAllowed(state, 7, now + 299, 300));
	CHECK(Purple::TradeAllowed(state, 7, now + 300, 300));
	CHECK(Purple::TradeAllowed(state, 8, now, 300));
	CHECK(Purple::TradeAllowed(state, 7, now, 0));

	// A record with no peer or no read time is not a trade at all.
	Purple::RememberTrade(state, 0, now, now);
	Purple::RememberTrade(state, 9, 0, now);
	CHECK_EQ(state.lastSeenTrades.size(), size_t(1));

	// Round trip through state.toml, alongside an override so the array of
	// tables and the inline array can be proved to coexist.
	state.activePreset = u"work"_q;
	state.overrides.push_back({
		.peer = 42,
		.kind = Purple::OverrideKind::Show,
		.untilUnix = now + 3600,
		.startedUnix = now,
		.preset = u"work"_q,
	});
	Purple::RememberTrade(state, 11, now - 5, 0);
	const auto text = Purple::SerializeState(state);
	CHECK(text.contains(u"[[last_seen_trades]]"_q));
	const auto reloaded = Purple::ParseState(text, u"state.toml"_q);
	CHECK_EQ(reloaded.overrides.size(), size_t(1));
	CHECK(reloaded.lastSeenTrades == state.lastSeenTrades);

	// A trade whose hold ran out without an exact status is still worth
	// writing down: it is what the cooldown counts.
	CHECK_EQ(reloaded.lastSeenTrades[1].peer, int64(11));
	CHECK_EQ(reloaded.lastSeenTrades[1].wasOnlineUnix, int64(0));

	// Pruning is the caller's to run, because the serialiser has neither a
	// clock nor the settings that say how long a read stays worth showing.
	CHECK(!Purple::PruneLastSeenTrades(state, now, 3600));
	auto stale = state;
	CHECK(Purple::PruneLastSeenTrades(stale, now + 7200, 3600));
	CHECK(stale.lastSeenTrades.empty());
}

[[nodiscard]] Purple::Event Ev(
		int64 ms,
		Purple::EventKind kind,
		Purple::PeerIdValue dialog = 0,
		const QString &preset = QString()) {
	auto result = Purple::Event();
	result.unixMs = ms;
	result.kind = kind;
	result.dialogId = dialog;
	result.preset = preset;
	return result;
}

void TestScreenTimeSettings() {
	Begin("screen time settings");

	// Off, and with the plan's thresholds, until the file says otherwise:
	// nothing should start keeping a log of what you looked at because a
	// version number moved.
	const auto silent = Parse(u"[presets.work]\nlist_order = []\n"_q);
	CHECK(silent.ok());
	CHECK(!silent.settings.screenTime.enabled);
	CHECK_EQ(silent.settings.screenTime.actionSpanSeconds, 3);
	CHECK_EQ(silent.settings.screenTime.activeGapSeconds, 30);
	CHECK_EQ(silent.settings.screenTime.idleAfterSeconds, 60);
	CHECK_EQ(silent.settings.screenTime.retentionDays, 90);
	CHECK(silent.settings.screenTime.budgets.empty());

	const auto parsed = Parse(uR"(
[presets.work]
list_order = []

[screen_time]
enabled_p = true
action_span = "5s"
active_gap = "20s"
idle_after = "2m"
retention_days = 30

[[screen_time.budgets]]
target = "chat:7"
per_day = "30m"
mode = "hard"
snooze = "10m"
snoozes_per_day = 1

[[screen_time.budgets]]
target = "kind:groups"
per_day = "2h"

[[screen_time.budgets]]
target = "preset:work"
per_day = "1h"
mode = "soft"

[[screen_time.budgets]]
target = "all"
per_day = "4h"
)"_q);
	CHECK(parsed.ok());
	CHECK(!WarnsAbout(parsed, u"screen_time"_q));
	const auto &screen = parsed.settings.screenTime;
	CHECK(screen.enabled);
	CHECK_EQ(screen.actionSpanSeconds, 5);
	CHECK_EQ(screen.activeGapSeconds, 20);
	CHECK_EQ(screen.idleAfterSeconds, 120);
	CHECK_EQ(screen.retentionDays, 30);
	CHECK_EQ(screen.budgets.size(), size_t(4));

	CHECK(screen.budgets[0].kind == Purple::BudgetTarget::Chat);
	CHECK_EQ(screen.budgets[0].chat, int64(7));
	CHECK_EQ(screen.budgets[0].perDaySeconds, 30 * 60);
	CHECK(screen.budgets[0].mode == Purple::BudgetMode::Hard);
	CHECK_EQ(screen.budgets[0].snoozeSeconds, 600);
	CHECK_EQ(screen.budgets[0].snoozesPerDay, 1);

	// Everything not said takes the default: soft, five minutes, twice.
	CHECK(screen.budgets[1].kind == Purple::BudgetTarget::Kind);
	CHECK(screen.budgets[1].chatKind == Purple::ScreenTimeKind::Group);
	CHECK(screen.budgets[1].mode == Purple::BudgetMode::Soft);
	CHECK_EQ(screen.budgets[1].snoozeSeconds, 5 * 60);
	CHECK_EQ(screen.budgets[1].snoozesPerDay, 2);

	CHECK(screen.budgets[2].kind == Purple::BudgetTarget::Preset);
	CHECK_EQ(screen.budgets[2].preset, u"work"_q);
	CHECK(screen.budgets[3].kind == Purple::BudgetTarget::All);
	CHECK_EQ(screen.budgets[3].perDaySeconds, 4 * 3600);

	// A budget nobody can act on is skipped with a warning, and the ones
	// around it are unaffected - the same rule every list in the file follows.
	const auto broken = Parse(uR"(
[[screen_time.budgets]]
target = "chat:not-a-number"
per_day = "30m"

[[screen_time.budgets]]
target = "sideways"
per_day = "30m"

[[screen_time.budgets]]
target = "kind:postcards"
per_day = "30m"

[[screen_time.budgets]]
target = "all"
per_day = "soon"

[[screen_time.budgets]]
per_day = "30m"

[[screen_time.budgets]]
target = "all"
per_day = "10m"
)"_q);
	CHECK(broken.ok());
	CHECK_EQ(broken.settings.screenTime.budgets.size(), size_t(1));
	CHECK_EQ(broken.settings.screenTime.budgets[0].perDaySeconds, 600);
	CHECK_EQ(int(broken.warnings.size()), 5);

	const auto days = Parse(u"[screen_time]\nretention_days = -1\n"_q);
	CHECK_EQ(days.settings.screenTime.retentionDays, 90);
	CHECK_EQ(int(days.warnings.size()), 1);

	// The four real kind spellings are the ones a list already uses, so
	// `kind:groups' in a budget and `kinds = ["groups"]' in a list agree.
	CHECK(Purple::ParseScreenTimeKind(u"groups"_q)
		== Purple::ScreenTimeKind::Group);
	CHECK(Purple::ParseScreenTimeKind(u" GROUP "_q)
		== Purple::ScreenTimeKind::Group);
	CHECK(Purple::ParseScreenTimeKind(u"elsewhere"_q)
		== Purple::ScreenTimeKind::Elsewhere);
	CHECK(!Purple::ParseScreenTimeKind(u"postcards"_q).has_value());
	CHECK(Purple::ScreenTimeKindFor(Purple::ChatKind::Bot)
		== Purple::ScreenTimeKind::Bot);
	for (const auto value : {
			Purple::ScreenTimeKind::Private,
			Purple::ScreenTimeKind::Group,
			Purple::ScreenTimeKind::Channel,
			Purple::ScreenTimeKind::Bot,
			Purple::ScreenTimeKind::Elsewhere }) {
		CHECK(Purple::ParseScreenTimeKind(Purple::ScreenTimeKindName(value))
			== value);
	}
	for (const auto value : {
			Purple::BudgetMode::Soft,
			Purple::BudgetMode::Hard }) {
		CHECK(Purple::ParseBudgetMode(Purple::BudgetModeName(value)) == value);
	}
}

void TestScreenTimeLog() {
	Begin("screen time log");

	auto event = Purple::Event();
	event.unixMs = 1788000000000;
	event.kind = Purple::EventKind::Open;
	event.dialogId = -100500;
	event.chatKind = Purple::ScreenTimeKind::Channel;
	event.preset = u"deep work"_q;
	event.action = QString();
	event.hidden = true;

	const auto line = Purple::FormatEvent(event);
	CHECK_EQ(line.count(QChar('\t')), 6);
	const auto back = Purple::ParseEventLine(line);
	CHECK(back.has_value());
	CHECK(*back == event);

	// A tab in a name the user typed is flattened rather than escaped: a line
	// that cannot be split takes the rest of the history with it.
	auto tabbed = event;
	tabbed.preset = u"deep\twork"_q;
	const auto flattened = Purple::ParseEventLine(
		Purple::FormatEvent(tabbed));
	CHECK(flattened.has_value());
	CHECK_EQ(flattened->preset, u"deep work"_q);

	// Every kind round-trips by name, so a log written by one build reads in
	// another.
	for (const auto kind : {
			Purple::EventKind::Open,
			Purple::EventKind::Close,
			Purple::EventKind::Action,
			Purple::EventKind::Idle,
			Purple::EventKind::Resume,
			Purple::EventKind::Preset,
			Purple::EventKind::Foreground,
			Purple::EventKind::Background }) {
		CHECK(Purple::ParseEventKind(Purple::EventKindName(kind)) == kind);
	}

	// A line that cannot be read is skipped, not fought over: the log is
	// append-only and a truncated last line after a crash is expected.
	CHECK(!Purple::ParseEventLine(QString()).has_value());
	CHECK(!Purple::ParseEventLine(u"nonsense"_q).has_value());
	CHECK(!Purple::ParseEventLine(
		u"abc\topen\t0\tprivate\t\t\t0"_q).has_value());
	CHECK(!Purple::ParseEventLine(
		u"1788000000000\tsideways\t0\tprivate\t\t\t0"_q).has_value());

	// Six fields is the shape this had before `hidden' existed, and it still
	// reads - history is the whole point of keeping raw events.
	const auto older = Purple::ParseEventLine(
		u"1788000000000\topen\t5\tprivate\twork\t"_q);
	CHECK(older.has_value());
	CHECK(!older->hidden);
	CHECK_EQ(older->dialogId, int64(5));

	const auto log = u"%1\nnot an event at all\n\n%2\n"_q
		.arg(Purple::FormatEvent(event))
		.arg(Purple::FormatEvent(tabbed));
	CHECK_EQ(Purple::ParseEventLog(log).size(), size_t(2));
}

void TestScreenTimeSessions() {
	Begin("screen time sessions");

	const auto settings = Purple::ScreenTime();
	const auto t0 = int64(1788000000000);
	const auto s = [](int seconds) { return int64(seconds) * 1000; };
	using Kind = Purple::EventKind;

	// A lone action counts only its own span. The rest of the session is
	// reading, which is time but not active time.
	auto events = std::vector<Purple::Event>{
		Ev(t0, Kind::Open, 5, u"work"_q),
		Ev(t0 + s(10), Kind::Action, 5, u"work"_q),
		Ev(t0 + s(60), Kind::Close, 5, u"work"_q),
	};
	auto sessions = Purple::DeriveSessions(events, settings);
	CHECK_EQ(sessions.size(), size_t(1));
	CHECK_EQ(sessions[0].totalMs(), s(60));
	CHECK_EQ(sessions[0].activeMs, s(3));
	CHECK_EQ(sessions[0].dialogId, int64(5));
	CHECK_EQ(sessions[0].preset, u"work"_q);

	// Two actions inside active_gap: the whole gap counts, plus the last
	// action's own span. This is what makes a conversation read as one stretch
	// of active time rather than as a row of three-second spikes.
	events = {
		Ev(t0, Kind::Open, 5, u"work"_q),
		Ev(t0 + s(10), Kind::Action, 5, u"work"_q),
		Ev(t0 + s(30), Kind::Action, 5, u"work"_q),
		Ev(t0 + s(60), Kind::Close, 5, u"work"_q),
	};
	sessions = Purple::DeriveSessions(events, settings);
	CHECK_EQ(sessions.size(), size_t(1));
	CHECK_EQ(sessions[0].activeMs, s(23));

	// Outside the gap they are two lone actions again.
	events[2].unixMs = t0 + s(50);
	sessions = Purple::DeriveSessions(events, settings);
	CHECK_EQ(sessions[0].activeMs, s(6));

	// Active time never outruns the session it is inside: send and close
	// immediately and you were active for the moment you were there.
	events = {
		Ev(t0, Kind::Open, 5, u"work"_q),
		Ev(t0 + s(10), Kind::Action, 5, u"work"_q),
		Ev(t0 + s(11), Kind::Close, 5, u"work"_q),
	};
	sessions = Purple::DeriveSessions(events, settings);
	CHECK_EQ(sessions[0].activeMs, s(1));

	// Idle pauses the session, and the pause is stamped back to where the
	// input actually stopped - idle_after before the recorder noticed - so a
	// changed threshold re-derives the same log differently.
	events = {
		Ev(t0, Kind::Open, 5, u"work"_q),
		Ev(t0 + s(120), Kind::Idle, 5, u"work"_q),
		Ev(t0 + s(200), Kind::Resume, 5, u"work"_q),
		Ev(t0 + s(260), Kind::Close, 5, u"work"_q),
	};
	sessions = Purple::DeriveSessions(events, settings);
	CHECK_EQ(sessions.size(), size_t(1));
	CHECK_EQ(sessions[0].idleMs, s(140));
	CHECK_EQ(sessions[0].totalMs(), s(120));

	auto slower = settings;
	slower.idleAfterSeconds = 30;
	sessions = Purple::DeriveSessions(events, slower);
	CHECK_EQ(sessions[0].idleMs, s(110));
	CHECK_EQ(sessions[0].totalMs(), s(150));

	// An idle nobody resumed from runs to the end of the session.
	events.erase(events.begin() + 2);
	sessions = Purple::DeriveSessions(events, settings);
	CHECK_EQ(sessions[0].idleMs, s(200));
	CHECK_EQ(sessions[0].totalMs(), s(60));

	// A preset event cuts the session in two, so every second of it has
	// exactly one preset - and the chat carries across the cut.
	events = {
		Ev(t0, Kind::Open, 5, u"work"_q),
		Ev(t0 + s(10), Kind::Action, 5, u"work"_q),
		Ev(t0 + s(60), Kind::Preset, 0, u"home"_q),
		Ev(t0 + s(120), Kind::Close, 5, u"home"_q),
	};
	sessions = Purple::DeriveSessions(events, settings);
	CHECK_EQ(sessions.size(), size_t(2));
	CHECK_EQ(sessions[0].preset, u"work"_q);
	CHECK_EQ(sessions[0].totalMs(), s(60));
	CHECK_EQ(sessions[0].activeMs, s(3));
	CHECK_EQ(sessions[1].preset, u"home"_q);
	CHECK_EQ(sessions[1].dialogId, int64(5));
	CHECK_EQ(sessions[1].totalMs(), s(60));
	CHECK_EQ(sessions[1].activeMs, int64(0));

	// Going to the background ends the session - a chat you cannot see is not
	// screen time - and coming back does not start one on its own.
	events = {
		Ev(t0, Kind::Foreground),
		Ev(t0 + s(1), Kind::Open, 5, u"work"_q),
		Ev(t0 + s(31), Kind::Background),
		Ev(t0 + s(61), Kind::Foreground),
		Ev(t0 + s(61), Kind::Open, 5, u"work"_q),
		Ev(t0 + s(91), Kind::Close, 5, u"work"_q),
	};
	sessions = Purple::DeriveSessions(events, settings);
	CHECK_EQ(sessions.size(), size_t(2));
	CHECK_EQ(sessions[0].totalMs(), s(30));
	CHECK_EQ(sessions[1].totalMs(), s(30));

	// An Open with nothing closed before it is the ordinary case on a phone,
	// and a session still open at the end of the log ends where the log does.
	events = {
		Ev(t0, Kind::Open, 5, u"work"_q),
		Ev(t0 + s(20), Kind::Open, 6, u"work"_q),
		Ev(t0 + s(50), Kind::Action, 6, u"work"_q),
	};
	sessions = Purple::DeriveSessions(events, settings);
	CHECK_EQ(sessions.size(), size_t(2));
	CHECK_EQ(sessions[0].dialogId, int64(5));
	CHECK_EQ(sessions[0].totalMs(), s(20));
	CHECK_EQ(sessions[1].dialogId, int64(6));
	CHECK_EQ(sessions[1].totalMs(), s(30));

	CHECK(Purple::DeriveSessions({}, settings).empty());
}

void TestScreenTimeTotals() {
	Begin("screen time totals");

	// UTC rather than the machine's zone: a day boundary is a local-time
	// question, and a test that asked the machine would answer differently in
	// two places.
	const auto zone = QTimeZone::utc();
	const auto at = [&](int year, int month, int day, int hour, int minute) {
		return QDateTime(QDate(year, month, day), QTime(hour, minute), zone)
			.toMSecsSinceEpoch();
	};
	const auto minutes = [](int count) { return int64(count) * 60 * 1000; };

	auto first = Purple::Session();
	first.startMs = at(2026, 8, 31, 10, 0);
	first.endMs = at(2026, 8, 31, 10, 30);
	first.dialogId = 1;
	first.chatKind = Purple::ScreenTimeKind::Private;
	first.preset = u"work"_q;
	first.activeMs = minutes(10);

	auto second = Purple::Session();
	second.startMs = at(2026, 9, 1, 10, 30);
	second.endMs = at(2026, 9, 1, 11, 30);
	second.dialogId = 2;
	second.chatKind = Purple::ScreenTimeKind::Group;
	second.preset = u"home"_q;
	second.activeMs = minutes(20);
	second.hidden = true;

	const auto sessions = std::vector<Purple::Session>{ first, second };
	const auto from = at(2026, 8, 31, 0, 0);
	const auto to = at(2026, 9, 2, 0, 0);

	const auto totals = Purple::RangeTotals(sessions, from, to);
	CHECK_EQ(totals.totalMs, minutes(90));
	CHECK_EQ(totals.activeMs, minutes(30));

	// Time in a chat the preset was hiding is part of the total, not on top
	// of it - it is the "while peeking" number.
	CHECK_EQ(totals.hiddenMs, minutes(60));
	CHECK_EQ(totals.chats.size(), size_t(2));
	CHECK_EQ(totals.chats[0].dialogId, int64(2));
	CHECK_EQ(totals.chats[0].totalMs, minutes(60));
	CHECK_EQ(totals.chats[1].dialogId, int64(1));
	CHECK_EQ(totals.kinds.size(), size_t(2));
	CHECK(totals.kinds[0].chatKind == Purple::ScreenTimeKind::Group);
	CHECK_EQ(totals.presets.size(), size_t(2));
	CHECK_EQ(totals.presets[0].preset, u"home"_q);
	CHECK_EQ(totals.presets[1].preset, u"work"_q);

	// Days: the month boundary falls between the two, so they never land in
	// one bucket however close together they are.
	auto buckets = Purple::Buckets(
		sessions,
		from,
		to,
		Purple::BucketUnit::Day,
		zone);
	CHECK_EQ(buckets.size(), size_t(2));
	CHECK_EQ(buckets[0].label, u"2026-08-31"_q);
	CHECK_EQ(buckets[0].totalMs, minutes(30));
	CHECK_EQ(buckets[1].label, u"2026-09-01"_q);
	CHECK_EQ(buckets[1].totalMs, minutes(60));
	CHECK_EQ(buckets[1].activeMs, minutes(20));

	buckets = Purple::Buckets(
		sessions,
		from,
		to,
		Purple::BucketUnit::Month,
		zone);
	CHECK_EQ(buckets.size(), size_t(2));
	CHECK_EQ(buckets[0].label, u"2026-08"_q);
	CHECK_EQ(buckets[0].totalMs, minutes(30));
	CHECK_EQ(buckets[1].label, u"2026-09"_q);
	CHECK_EQ(buckets[1].totalMs, minutes(60));

	// A week runs Monday to Monday whatever the range does, so the last day of
	// August and the first of September - a Monday and the Tuesday after it -
	// are the same week even though they are not the same month.
	buckets = Purple::Buckets(
		sessions,
		from,
		to,
		Purple::BucketUnit::Week,
		zone);
	CHECK_EQ(buckets.size(), size_t(1));
	CHECK_EQ(buckets[0].label, u"2026-W36"_q);
	CHECK_EQ(buckets[0].totalMs, minutes(90));

	// Hour of day folds every day in the range onto one clock, and splits a
	// session across the hour boundary it crosses rather than filing all of it
	// under the hour it began in.
	buckets = Purple::Buckets(
		sessions,
		from,
		to,
		Purple::BucketUnit::HourOfDay,
		zone);
	CHECK_EQ(buckets.size(), size_t(24));
	CHECK_EQ(buckets[10].label, u"10"_q);
	CHECK_EQ(buckets[10].totalMs, minutes(60));
	CHECK_EQ(buckets[11].totalMs, minutes(30));
	CHECK_EQ(buckets[9].totalMs, int64(0));

	// The heat map is the same split, kept apart by weekday: 2026-08-31 is a
	// Monday and the day after it a Tuesday.
	const auto heat = Purple::HeatMapFor(sessions, from, to, zone);
	CHECK_EQ(heat.total(1, 10), minutes(30));
	CHECK_EQ(heat.total(2, 10), minutes(30));
	CHECK_EQ(heat.total(2, 11), minutes(30));
	CHECK_EQ(heat.total(3, 10), int64(0));
	CHECK_EQ(heat.active(2, 10), minutes(10));
	CHECK_EQ(heat.total(0, 10), int64(0));
	CHECK_EQ(heat.total(8, 10), int64(0));

	// Against the range of the same length immediately before it, which is
	// what "up 100% on yesterday" means.
	const auto compare = Purple::Compare(
		sessions,
		at(2026, 9, 1, 0, 0),
		at(2026, 9, 2, 0, 0));
	CHECK_EQ(compare.current.totalMs, minutes(60));
	CHECK_EQ(compare.previous.totalMs, minutes(30));
	CHECK_EQ(compare.deltaMs, minutes(30));
	CHECK(compare.changePercent.has_value());
	CHECK_EQ(*compare.changePercent, 100);

	// Nothing to compare against is not zero percent: there is no percentage
	// change from nothing, and any number shown there would be invented.
	const auto alone = Purple::Compare(
		sessions,
		at(2026, 8, 31, 0, 0),
		at(2026, 9, 1, 0, 0));
	CHECK(!alone.changePercent.has_value());
	CHECK_EQ(alone.deltaMs, minutes(30));
}

void TestScreenTimeBudgets() {
	Begin("screen time budgets");

	const auto zone = QTimeZone::utc();
	const auto at = [&](int year, int month, int day, int hour, int minute) {
		return QDateTime(QDate(year, month, day), QTime(hour, minute), zone)
			.toMSecsSinceEpoch();
	};
	const auto parsed = Parse(uR"(
[presets.work]
list_order = []

[screen_time]
enabled_p = true

[[screen_time.budgets]]
target = "chat:7"
per_day = "30m"
mode = "hard"
snooze = "5m"
snoozes_per_day = 2

[[screen_time.budgets]]
target = "kind:groups"
per_day = "2h"

[[screen_time.budgets]]
target = "preset:work"
per_day = "1h"

[[screen_time.budgets]]
target = "chat:7"
per_day = "10m"
mode = "hard"
snoozes_per_day = 0
)"_q);
	CHECK(parsed.ok());
	CHECK(!WarnsAbout(parsed, u"screen_time"_q));
	const auto &screen = parsed.settings.screenTime;

	auto open = Ev(
		at(2026, 9, 1, 10, 0),
		Purple::EventKind::Open,
		7,
		u"work"_q);
	open.chatKind = Purple::ScreenTimeKind::Private;
	const auto events = std::vector<Purple::Event>{
		open,
		Ev(at(2026, 9, 1, 10, 40), Purple::EventKind::Close, 7, u"work"_q),
	};

	const auto ledger = Purple::BudgetLedger(
		events,
		screen,
		QDate(2026, 9, 1),
		zone);
	CHECK_EQ(ledger.size(), size_t(4));
	CHECK_EQ(ledger[0].index, 0);
	CHECK_EQ(ledger[0].spentMs, int64(40) * 60 * 1000);
	CHECK_EQ(ledger[0].perDayMs, int64(30) * 60 * 1000);
	CHECK(ledger[0].reached);

	// A budget for something else has spent nothing and is not reached.
	CHECK_EQ(ledger[1].spentMs, int64(0));
	CHECK(!ledger[1].reached);

	// The preset budget counts the same forty minutes, because the session ran
	// under it - one session can be inside several budgets at once.
	CHECK_EQ(ledger[2].spentMs, int64(40) * 60 * 1000);
	CHECK(!ledger[2].reached);

	// Another day sees nothing of it.
	const auto other = Purple::BudgetLedger(
		events,
		screen,
		QDate(2026, 9, 2),
		zone);
	CHECK_EQ(other[0].spentMs, int64(0));
	CHECK(!other[0].reached);

	// The cover is a hard budget's, and only while snoozes are left.
	CHECK(Purple::CoverAllowed(0, screen.budgets[0]));
	CHECK(Purple::CoverAllowed(1, screen.budgets[0]));
	CHECK(!Purple::CoverAllowed(2, screen.budgets[0]));

	// A soft budget never puts one up, and a hard one that offers no snooze is
	// absolute.
	CHECK(!Purple::CoverAllowed(0, screen.budgets[1]));
	CHECK(!Purple::CoverAllowed(0, screen.budgets[3]));

	// Retention drops what is past it and keeps the order of what is left. A
	// retention of zero keeps everything, like every other zero here.
	const auto now = at(2026, 9, 1, 12, 0);
	const auto day = int64(24) * 60 * 60 * 1000;
	const auto log = std::vector<Purple::Event>{
		Ev(now - 100 * day, Purple::EventKind::Open, 1),
		Ev(now - 89 * day, Purple::EventKind::Open, 2),
		Ev(now - day, Purple::EventKind::Open, 3),
	};
	const auto kept = Purple::Prune(log, now, 90);
	CHECK_EQ(kept.size(), size_t(2));
	CHECK_EQ(kept[0].dialogId, int64(2));
	CHECK_EQ(kept[1].dialogId, int64(3));
	CHECK_EQ(Purple::Prune(log, now, 0).size(), size_t(3));
}

void TestNestedWindows() {
	Begin("nested schedule windows");

	// 2026-08-17 is a Monday, so dayOfWeek() runs 1..7 across that week.
	const auto at = [](int weekday, int hour, int minute) {
		return QDateTime(
			QDate(2026, 8, 16 + weekday),
			QTime(hour, minute));
	};
	const auto rule = [](
			std::vector<int> days,
			int from,
			int till,
			const QString &preset) {
		auto result = Purple::ScheduleRule();
		result.days = std::move(days);
		result.from = from;
		result.till = till;
		result.preset = preset;
		return result;
	};
	const auto target = [](
			const Purple::Schedule &schedule,
			const QDateTime &when) {
		const auto result = Purple::ScheduleTarget(schedule, when);
		return result ? *result : u"<nothing>"_q;
	};

	// Lunch inside work. The narrower window wins whichever order the two are
	// written in, which is the whole point: nobody remembers which line of
	// their own file came first.
	auto schedule = Purple::Schedule();
	schedule.rules.push_back(rule({ 1 }, 8 * 60, 17 * 60, u"work"_q));
	schedule.rules.push_back(rule({ 1 }, 12 * 60, 14 * 60, u"lunch"_q));
	CHECK_EQ(target(schedule, at(1, 9, 0)), u"work"_q);
	CHECK_EQ(target(schedule, at(1, 13, 0)), u"lunch"_q);
	CHECK_EQ(target(schedule, at(1, 14, 0)), u"work"_q);
	CHECK_EQ(target(schedule, at(1, 17, 0)), u"normal"_q);

	auto reversed = Purple::Schedule();
	reversed.rules.push_back(rule({ 1 }, 12 * 60, 14 * 60, u"lunch"_q));
	reversed.rules.push_back(rule({ 1 }, 8 * 60, 17 * 60, u"work"_q));
	CHECK_EQ(target(reversed, at(1, 9, 0)), u"work"_q);
	CHECK_EQ(target(reversed, at(1, 13, 0)), u"lunch"_q);
	CHECK_EQ(target(reversed, at(1, 14, 0)), u"work"_q);

	// The far end of the nested window is a window STARTING, not one ending,
	// because the target moves to something that is not the outside preset.
	// So work resumes at two o'clock even after a preset chosen by hand during
	// lunch - which is the sequence the whole rule exists for.
	CHECK(Purple::ScheduleApplies(
		schedule,
		u"lunch"_q,
		Purple::PresetSource::Schedule));
	CHECK(Purple::ScheduleApplies(
		schedule,
		u"work"_q,
		Purple::PresetSource::Manual));

	// ... and five o'clock is still a window ending, so it leaves a manual
	// choice alone.
	CHECK(!Purple::ScheduleApplies(
		schedule,
		u"normal"_q,
		Purple::PresetSource::Manual));

	// Two rules with the same window really are the same statement twice, so
	// file order is the tie-break and nothing else changed.
	auto equal = Purple::Schedule();
	equal.rules.push_back(rule({ 1 }, 9 * 60, 17 * 60, u"first"_q));
	equal.rules.push_back(rule({ 1 }, 9 * 60, 17 * 60, u"second"_q));
	CHECK_EQ(target(equal, at(1, 10, 0)), u"first"_q);
	equal.rules[0].enabled = false;
	CHECK_EQ(target(equal, at(1, 10, 0)), u"second"_q);

	// A window crossing midnight is measured through it. Naively it would be a
	// negative length and the narrowest thing in any file, so the evening
	// window nested inside the night one would never win.
	auto night = Purple::Schedule();
	night.rules.push_back(rule({ 1 }, 22 * 60, 6 * 60, u"sleep"_q));
	night.rules.push_back(rule({ 1 }, 21 * 60, 23 * 60 + 30, u"evening"_q));
	CHECK_EQ(target(night, at(1, 23, 0)), u"evening"_q);
	CHECK_EQ(target(night, at(1, 23, 30)), u"sleep"_q);
	CHECK_EQ(target(night, at(2, 5, 0)), u"sleep"_q);

	// The same the other side of midnight: a five-hour morning window beats
	// the eight-hour night one it sits inside.
	night.rules.push_back(rule({ 2 }, 2 * 60, 7 * 60, u"early"_q));
	CHECK_EQ(target(night, at(2, 5, 0)), u"early"_q);
	CHECK_EQ(target(night, at(1, 23, 0)), u"evening"_q);

	// And across rulesets: a narrow rule in an `always' ruleset beats a wider
	// one in the tier that was chosen for this device, even though the chosen
	// tier's rules are merged first.
	const auto parsed = Parse(uR"(
[presets.work]
list_order = []

[presets.lunch]
list_order = []

[[schedule.rulesets]]
name   = "phone"
device = "mobile"

[[schedule.rulesets.rules]]
days   = ["mon"]
from   = "08:00"
to     = "17:00"
preset = "work"

[[schedule.rulesets]]
name = "everywhere"
mode = "always"

[[schedule.rulesets.rules]]
days   = ["mon"]
from   = "12:00"
to     = "14:00"
preset = "lunch"
)"_q);
	CHECK(parsed.ok());
	CHECK(!WarnsAbout(parsed, u"schedule"_q));
	const auto phone = Device(u"pixel-11ff"_q, u"android"_q, u"mobile"_q);
	const auto active = Purple::ActiveSchedule(parsed.settings.schedule, phone);
	CHECK_EQ(active.rules.size(), size_t(2));
	CHECK_EQ(active.rules[0]->preset, u"work"_q);

	const auto onPhone = [&](const QDateTime &when) {
		const auto result = Purple::ScheduleTarget(
			parsed.settings.schedule,
			when,
			phone);
		return result ? *result : u"<nothing>"_q;
	};
	CHECK_EQ(onPhone(at(1, 9, 0)), u"work"_q);
	CHECK_EQ(onPhone(at(1, 13, 0)), u"lunch"_q);
	CHECK_EQ(onPhone(at(1, 14, 0)), u"work"_q);

	const auto now = Purple::ScheduleRuleNow(
		parsed.settings.schedule,
		at(1, 13, 0),
		phone);
	CHECK(now != nullptr);
	CHECK_EQ(now->preset, u"lunch"_q);
	CHECK_EQ(now->from, 12 * 60);
}

} // namespace

int main() {
	TestLists();
	TestKinds();
	TestPresets();
	TestPresetPolicies();
	TestSpread();
	TestFolderSelection();
	TestShowModes();
	TestViews();
	TestMembers();
	TestScheduleAndFocus();
	TestScalarParsers();
	TestPremiumStillParses();
	TestVersion();
	TestBrokenFile();
	TestSpliceAdd();
	TestSpliceRemove();
	TestSpliceCanonicalises();
	TestSpliceMissingArray();
	TestSpliceStability();
	TestSpliceOddFormatting();
	TestSpliceCrlf();
	TestNameSanitising();
	TestSpliceAddList();
	TestSplicePresetPinned();
	TestSpliceViewPinned();
	TestSetTableBool();
	TestScheduleRuleIdentity();
	TestSpliceScheduleSet();
	TestSpliceScheduleAppend();
	TestSpliceScheduleRemove();
	TestSpliceRulesets();
	TestSpliceRulesetRules();
	TestBudgetIdentity();
	TestSpliceBudgetAppend();
	TestSpliceBudgetSet();
	TestSpliceBudgetRemove();
	TestSetTableBoolImplicitHeader();
	TestSetTableString();
	TestStateRoundTrip();
	TestStateDefaults();
	TestStateQuoting();
	TestAutoSend();
	TestResolveBasics();
	TestResolveViewName();
	TestFallThrough();
	TestMatchPriority();
	TestViewMembership();
	TestMentionGate();
	TestPeek();
	TestNamedExplicitly();
	TestPresetHotkeys();
	TestOverrides();
	TestSuggestionsAndArchive();
	TestSyncAndRecommended();
	TestReservedHotkeys();
	TestStories();
	TestRecent();
	TestHideScope();
	TestScheduleTarget();
	TestScheduleOutside();
	TestRulesets();
	TestActiveSchedule();
	TestDevices();
	TestSchedulePauseUntil();
	TestResolvedCache();
	TestLastSeenKeys();
	TestLastSeenReasons();
	TestLastSeenTrades();
	TestScreenTimeSettings();
	TestScreenTimeLog();
	TestScreenTimeSessions();
	TestScreenTimeTotals();
	TestScreenTimeBudgets();
	TestNestedWindows();

	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}
