/*
purple-core - the Work Mode core shared by Purple Telegram apps.
https://github.com/NightMachinery/purple-core

Licensed under the GNU General Public License, version 2 or (at your
option) any later version.
*/
#pragma once

#include "purple/purple_types.h"

#include <QtCore/QString>

#include <optional>
#include <vector>

// The parsed form of settings.toml. Deliberately free of every tdesktop
// dependency beyond base/ and Qt Core, so purple/test_config.sh can compile it
// on its own and drive it against fixture files - the file is hand-owned, so
// the parser has to survive anything a text editor can produce, and that is far
// easier to prove outside a running app.
namespace Purple {

// The schema this build speaks, and what a file's own `version' is measured
// against. It moves only when a key changes MEANING, because that is the only
// change an older build gets wrong rather than merely misses: adding a key
// costs an old build nothing, since it ignores what it has never heard of.
//
// So it is not a build number and not a date. Bumping it on every release
// would make it noise, and a warning nobody has a reason to act on is a
// warning everybody learns to skip.
inline constexpr auto kSettingsVersion = 1;

// Peer ids exactly as written in the file. Turning these into tdesktop PeerIds
// is the engine's job; the parser stays ignorant of what a peer is.
using PeerIdValue = int64;

// What a chat is, for lists that match by type rather than by member. Lives
// here rather than in the engine because a list definition names these, and
// the engine sits above the parser.
enum class ChatKind : uchar {
	Private,
	Group,
	Channel,
	Bot,
};

// "private", "groups", "channels", "bots" - the spellings `kinds' accepts.
[[nodiscard]] std::optional<ChatKind> ParseChatKind(const QString &value);
[[nodiscard]] QString ChatKindName(ChatKind kind);

// How much of a folder a preset pulls into its own view, whatever the lists
// decided - the escape hatch for "hide everything except what is in here".
//
// `Pinned' is for a folder you keep a handful of current things at the top of:
// the difference between the two albums you are listening to and every channel
// ever filed there.
enum class FolderInclude : uchar {
	None,
	Pinned,
	All,
};

// "none", "pinned", "all" - the spellings `include_in_main_view' accepts.
[[nodiscard]] std::optional<FolderInclude> ParseFolderInclude(
	const QString &value);
[[nodiscard]] QString FolderIncludeName(FolderInclude value);

// When a chat is in the view at all. Everything except Always and Never
// depends on the chat's unread state, so a chat can enter and leave the view
// on its own as messages arrive and are read.
//
// Mention is the narrow one and is what `groups_require_mention_p' used to
// spell; it is now simply what a group defaults to.
enum class ShowMode : uchar {
	Always,
	Message,          // An unread message, or a manual unread mark.
	MessageOrReaction,
	Mention,
	Never,
};

// "always", "message", "message_or_reaction", "mention", "never".
[[nodiscard]] std::optional<ShowMode> ParseShowMode(const QString &value);
[[nodiscard]] QString ShowModeName(ShowMode value);

// What a PRESET does with the stories strip as a whole. A ladder: each value
// hides strictly more than the one before it.
//
// Follow is the default, and the interesting one. It hides a peer the preset
// excludes OUTRIGHT - one no list claims, or one claimed with "never" - and
// keeps a peer the preset admits but happens to be holding back because they
// are quiet. A story IS new activity, which is exactly what `message',
// `message_or_reaction' and `mention' are asking to be shown; suppressing the
// one thing such a person does have would invert the setting.
enum class StoryPolicy : uchar {
	All,          // No peer filtering. Stock Telegram.
	AllUnseen,    // Everyone, but only while they have something unseen.
	Follow,       // Hide whoever the preset excludes outright.
	FollowUnseen, // Follow, and only while unseen.
	None,         // No strip at all.
};

// "all", "all_unseen", "follow", "follow_unseen", "none".
[[nodiscard]] std::optional<StoryPolicy> ParseStoryPolicy(const QString &value);
[[nodiscard]] QString StoryPolicyName(StoryPolicy value);

// What ONE list entry or folder entry says about its own people's stories.
//
// A narrower vocabulary than StoryPolicy on purpose: an entry is already
// scoped to a set of people, so "all" would mean nothing here. It is the
// vocabulary `show_mode' already uses next door, which is the point - these sit
// side by side in the same inline table.
enum class StoryMode : uchar {
	Always, // Seen or not, whatever the preset's policy says.
	Unseen, // Only while unseen.
	Never,  // None of them.
};

// "always", "unseen", "never".
[[nodiscard]] std::optional<StoryMode> ParseStoryMode(const QString &value);
[[nodiscard]] QString StoryModeName(StoryMode value);

// What a chat of this kind does when nothing says otherwise. Channels and bots
// are things you subscribed to or started, so they stay; groups are the noisy
// ones and only speak up when they name you; a private chat appears when the
// person has said something.
//
// The default depending on the chat rather than being flat is why a resolved
// entry cannot collapse its mode: one list_order entry can match several kinds.
[[nodiscard]] ShowMode DefaultShowMode(ChatKind kind);

// Whether the mode needs the chat's unread state to answer. False for exactly
// Always and Never, which is what keeps a preset made of those two out of the
// re-check path entirely.
[[nodiscard]] bool ShowModeWatchesUnread(ShowMode value);

// How often the mode shows a chat, as a number, so "the most permissive of
// these two wins" is a comparison rather than a table of special cases. Spelt
// out rather than taken from the declaration order, which does not match:
// MessageOrReaction shows strictly more often than Message and is declared
// after it.
[[nodiscard]] int ShowModeRank(ShowMode value);

// A list says who is in it and nothing else. What happens to those chats is
// decided entirely by the preset that names the list, which is what lets one
// list mean "let through" in one preset and "swallow silently" in another
// without a second table saying so. See docs/purple/work_mode.md.
struct List {
	QString name;
	QString title;

	// A chat matches when its id is here, or when its kind is in `kinds'. Both
	// may be empty, which is a list that matches nothing - useful as a
	// placeholder you fill in from the chat menu later.
	std::vector<PeerIdValue> members;
	std::vector<ChatKind> kinds;
};

// One step of a preset's ordered list_order: the list it names, and what the
// preset does with the chats that list claims. Order is priority AND capture -
// the first entry whose list holds a chat decides it, and later entries never
// see that chat again.
//
// Tri-state so that "said nothing" stays distinguishable from "said false",
// which matters for warnings rather than for behaviour: an unset flag takes
// the documented default below.
struct ListEntry {
	QString list;

	// Unset means DefaultShowMode() for whatever kind the chat turns out to be,
	// which is not knowable here: one entry can claim private chats, groups and
	// channels at once. Collapsed by Visible(), which has the chat in hand.
	std::optional<ShowMode> show;

	std::optional<bool> notify; // Default true.

	// What this entry's people's stories do, overriding the preset's own
	// stories policy for them. Unset leaves them to it.
	std::optional<StoryMode> stories;

	friend bool operator==(const ListEntry &, const ListEntry &) = default;
};

// One of the account's real Telegram folders, as a preset sees it.
struct PresetFolder {
	QString name;

	// False makes the preset ignore this entry entirely: no tab, nothing
	// silenced, nothing fed into the main view, nothing counted. Default true.
	//
	// Distinct from show_p below, which only takes the tab off the strip - a
	// folder with show_p = false still silences, still feeds the main view and
	// still counts. This one is the comment character you do not have to add
	// and remove: an entry you are keeping around for a preset you are still
	// tuning, with the settings you chose for it intact and doing nothing.
	//
	// It also beats "*ALL". A folder you disabled by hand stays disabled in a
	// preset that also asks for every folder, because otherwise disabling one
	// would silently stop working the day you added the spread.
	std::optional<bool> enabled;

	// Whether the folder's tab appears in the strip. Default true: naming a
	// folder at all is normally how you ask for it.
	std::optional<bool> show;

	// False silences the folder's chats, on top of whatever their list said.
	std::optional<bool> notify;

	// False takes this folder out of every count: no number on its own tab, and
	// its chats left out of the app badge. Default true. For a folder that is
	// background noise on purpose, a count is just a number you have decided in
	// advance not to act on.
	std::optional<bool> badge;

	// The mode for the chats this folder contributes, on the same terms as
	// `notify' above - it is about the folder's chats, not about its tab, which
	// is what `show' means. Unset leaves them to the default for what they are,
	// because naming a folder chose which chats come in rather than when they
	// show.
	std::optional<ShowMode> showMode;

	// How much of this folder joins the preset's main view. Default None.
	//
	// Whatever it lets in comes in even when the chat is archived. Archiving is
	// how visibility is controlled in stock Telegram; under a preset the preset
	// controls it, so a folder that asked for its chats gets them wherever they
	// happen to be filed. The chats stay archived - they are simply also in the
	// view, the way a real Telegram folder holds archived chats too.
	std::optional<FolderInclude> include;

	// What this folder's people's stories do. Beats a list entry, the same way
	// a folder already beats one for hiding, and beats the preset's policy.
	std::optional<StoryMode> stories;

	friend bool operator==(
		const PresetFolder &,
		const PresetFolder &) = default;
};

// An extra tab a preset invents, alongside its main view. Its list_order picks
// membership only: a chat an entry claims with show = false is dropped from
// this tab, and `notify' means nothing here because a chat has one mute state
// however many tabs it appears in.
struct PresetView {
	QString name;

	// The tab's pinned order, in the order it should appear. Owned by the file
	// rather than by the server, which knows nothing about a tab you invented.
	std::vector<PeerIdValue> pinned;

	std::vector<ListEntry> listOrder;

	friend bool operator==(const PresetView &, const PresetView &) = default;
};

struct Preset {
	QString name;

	// What the preset's main tab is called where All chats used to be. Empty
	// means the preset's own name with its first letter capitalised, which is
	// what almost everyone wants and nobody should have to type.
	QString viewName;

	// A key that turns this preset on, and off again if it is already on. Qt
	// portable text, the same spelling `[peek] hotkey' uses - so on macOS
	// "Ctrl" is Command and "Meta" is the physical Control key. Empty for a
	// preset you only ever pick from the box.
	QString hotkey;

	// Whether a chat this preset hides is gone from the whole app rather than
	// only from the preset's own view of the chat list - so out of the forward
	// picker, out of search, out of recent chats. Nothing means no, which is
	// the default because a work mode is about what you are looking at, not
	// about what you are allowed to reach. See docs/purple/work_mode.md.
	std::optional<bool> hideEverywhere;

	// Whether the archive is out of the way while this preset runs: no pull
	// gesture, no row in the list, the same state as an account that has never
	// archived anything.
	//
	// Nothing means YES, which is the opposite default from hide_everywhere_p
	// above and deliberately so: the archive is where you put what you are not
	// dealing with, so a preset that has already named what gets through has no
	// reason to leave a door to the rest of it open. Say false to keep it.
	//
	// Only ever asked while a preset is filtering, so Normal is untouched.
	std::optional<bool> hideArchive;

	// What the stories strip does while this preset runs. Unset means Follow:
	// the strip follows the preset's decision about the person, so somebody it
	// excludes outright loses their story and somebody it is merely holding
	// back for being quiet keeps theirs.
	std::optional<StoryPolicy> stories;

	// The main view's pinned order, when the preset wants one of its own.
	//
	// Empty - the usual case - means the main view mirrors the account's own
	// pinned chats, which is what it has always done and what a preset saying
	// nothing about pins should keep doing. Non-empty means the preset owns the
	// order outright: a pin made inside it stays inside it, never reaches the
	// server, and so leaves the account's order and your other devices alone.
	//
	// That is also what lifts the five-pin ceiling. Five is the server's limit
	// on the thing the mirror was mirroring; an order kept in this file is
	// bounded by nothing.
	std::vector<PeerIdValue> pinned;

	// Priority order, first match wins. A chat no entry claims is hidden and
	// silenced: a preset names what gets through.
	std::vector<ListEntry> listOrder;

	// The real folders this preset shows. Empty means no folder tabs at all -
	// write "*ALL" to get them back.
	std::vector<PresetFolder> folders;

	std::vector<PresetView> views;
};

struct ScheduleRule {
	bool enabled = true;
	std::vector<int> days; // Qt::Monday .. Qt::Sunday, 1 .. 7.
	int from = -1; // Minutes since local midnight.
	int till = -1;
	QString preset;

	// Where this rule sits in the raw [[schedule.rules]] array, counted from
	// zero and counting the rules the parser threw away - which is what makes
	// it an address the splicer can edit by. A rule has no name key and needs
	// none: adding one would be a second thing to keep in step with the file,
	// and a screen editing a rule already has to say which rule it read.
	//
	// -1 for a rule that did not come from a file at all.
	int sourceIndex = -1;

	// The line its [[schedule.rules]] header is on, 1-based, for a screen that
	// wants to point at it. Zero when there is no file behind the rule.
	int sourceLine = 0;
};

// What a ruleset does on a device it applies to. Three states rather than a
// boolean, because "on" has two meanings that a file with several rulesets has
// to be able to tell apart: the ordinary one competes for the device and only
// the most specific competitor wins, while `always' runs alongside whichever
// one that turns out to be.
enum class RulesetMode : uchar {
	// Written down and switched off. Not skipped by the parser - a ruleset you
	// are not using this month is not a mistake in the file - just never chosen.
	Disabled,

	Enabled,

	// Merged in on every device it applies to, whatever else wins. For the rules
	// that are true everywhere - "never during the night" - so they do not have
	// to be copied into each device's ruleset and kept in step by hand.
	Always,
};

[[nodiscard]] std::optional<RulesetMode> ParseRulesetMode(const QString &value);
[[nodiscard]] QString RulesetModeName(RulesetMode value);

// A named group of rules, and the answer to "which devices is this for". One
// settings.toml is meant to be carried between a phone and a laptop unchanged,
// so the file says what belongs where rather than each device keeping its own
// copy to drift out of step.
struct ScheduleRuleset {
	// Required, unique among rulesets ignoring case, and the address every
	// splice op uses - a ruleset is edited by name because its position moves
	// whenever one above it is added or taken away, and a screen that read it
	// would then edit the wrong one. A ruleset with no name, or with a name
	// already taken, is skipped with a warning.
	QString name;

	// "any", a class ("mobile", "desktop"), a platform ("android", "ios",
	// "macos", "windows", "linux") or a device id the client reports for
	// itself. Anything else is taken as a device id: the list of platforms is
	// closed, but the list of devices belongs to the person carrying them.
	QString device = u"any"_q;

	RulesetMode mode = RulesetMode::Enabled;

	// What this ruleset wants between its windows. Unset - which is not the
	// same as "normal" - leaves the question to [schedule] outside, so a
	// ruleset only says it when it means to override that.
	std::optional<QString> outside;

	std::vector<ScheduleRule> rules;

	// Where it sits in the raw [[schedule.rulesets]] array, counting the ones
	// the parser threw away, so a warning can name a ruleset that has no usable
	// name to be named by.
	//
	// -1 for the implicit ruleset described on Schedule::rulesets below.
	int sourceIndex = -1;

	// The line its [[schedule.rulesets]] header is on, 1-based. Zero when there
	// is no file behind it.
	int sourceLine = 0;

	// Whether this is the flat [[schedule.rules]] array wearing a ruleset's
	// clothes rather than something the file spelled out.
	[[nodiscard]] bool implicit() const {
		return sourceIndex < 0;
	}
};

struct Schedule {
	bool enabled = true;

	// The preset the schedule wants whenever no rule covers the moment. Not
	// every life has stock Telegram at the edges of the day - someone whose
	// default is Home wants five o'clock to put Home back, not Normal - so the
	// fallback is a key rather than the constant it used to be.
	//
	// A name no preset backs is warned about and replaced with normal, the same
	// rule a rule's own 'preset' follows.
	QString outside = u"normal"_q;

	// The flat [[schedule.rules]] array. It is the shape every file had before
	// rulesets existed and it keeps working forever; the splice ops still
	// address it with an empty ruleset name.
	std::vector<ScheduleRule> rules;

	// Every [[schedule.rulesets]] block, in file order - and, first, an implicit
	// one wrapping the flat rules above when there are any: name "rules", device
	// "any", mode Enabled, no outside of its own.
	//
	// It is materialised rather than special-cased at every use because the
	// resolution has to hand back pointers to the rulesets it chose, and a
	// ruleset synthesised on the way out would be dangling by the time the
	// caller read it. Its rules are therefore the same rules as `rules' above,
	// written down twice; nothing mutates a parsed Schedule, so the two cannot
	// drift.
	std::vector<ScheduleRuleset> rulesets;
};

struct FocusSync {
	bool enabled = false;
	QString enterPreset;
	QString exitPreset;
};

// The `[peek]' table: the temporary look past the running preset, and how long
// one lasts. Not part of a preset - it is a decision about the gesture, not
// about what any one preset lets through.
struct Peek {
	// The key sequence that starts one on the desktop, "Ctrl+Shift+E" and the
	// like. A SEQUENCE, not a length, which is why the length key beside it is
	// spelled `hotkey_length': `hotkey' was documented and in use long before
	// there was a second way to start a peek, and renaming a key people have
	// in their files to make room for a new one would be a poor trade.
	QString hotkey;

	// How long a peek lasts when nothing more specific was said. Zero disables
	// the timer, so the peek runs until it is turned off by hand.
	int autoOffSeconds = 0;

	// How long the two ways of starting one last, when they want to differ.
	// Unset - which is what a file that says nothing gives - means "whatever
	// `auto_off' says", so one key still governs both for anybody who does not
	// care about the difference. Zero is "off": until I stop.
	//
	// They exist because the two gestures are not the same gesture. A tap on a
	// phone's peek control is a look at the list you are holding, and a phone
	// wants longer than a keyboard shortcut fired mid-sentence on a desktop
	// does. Read through PeekTapSeconds() and PeekHotkeySeconds() rather than
	// directly, so the fallback is written once.
	std::optional<int> tapSeconds;
	std::optional<int> hotkeyLengthSeconds;

	// What a tap is worth on a phone, when the phone is to differ. Unset does
	// NOT mean `tap' or `auto_off' here, which is the one place these keys break
	// their own pattern: a phone has no settings.toml of its own to edit - it
	// reads one written at a keyboard and carried over - so a fallback would hand
	// every phone a length nobody chose for it. Absent is five minutes. Read it
	// through PeekTapSeconds(settings, device) in purple_engine.h, where the
	// reasoning sits with the device it is about. Zero is still "off".
	std::optional<int> tapMobileSeconds;
};

// Declared here so the two readers below can sit with the keys they read,
// rather than at the far end of the file away from the comment that explains
// them. Settings itself is assembled further down, out of this and its peers.
struct Settings;

// How long a peek started by tapping the control lasts, and how long one
// started by the desktop hotkey lasts. `[peek] tap' and `[peek] hotkey_length'
// respectively, each falling back to `[peek] auto_off' when it is not set.
//
// Zero from either is a real answer - "off", a peek with no clock on it - and
// not a missing one, which is why the fallback lives here rather than in a
// `.value_or(0)' at each call site.
[[nodiscard]] int PeekTapSeconds(const Settings &settings);
[[nodiscard]] int PeekHotkeySeconds(const Settings &settings);

// The lengths a peek control offers: the chips a sheet shows and the positions
// a dial snaps to, shortest first. One minute to one hour.
//
// Both apps must read the row from here rather than writing their own. Two
// hand-written lists is how a phone's chips and a desktop's dial come to offer
// different minutes for the same feature, and the row is exactly the kind of
// thing somebody edits on one side only.
[[nodiscard]] const std::vector<int> &PeekDetentsSeconds();

// Which detent a length is. The index into the row above, or its size() for
// zero - "until I stop", which lives one position PAST the last detent so a
// dial is a single continuous track of size() + 1 stops rather than a track
// plus a checkbox somewhere else.
//
// A length between two detents reads as the nearer one, and a tie reads as the
// SHORTER: a control that silently rounds a peek up is a control that reveals
// more than was asked for. A length past the last detent reads as the last
// detent rather than as "until I stop" - zero is the only thing that means
// that, and a file asking for two hours has asked for a peek that ends.
[[nodiscard]] int PeekDetentIndex(int seconds);

// And back: the length at that position. Zero - "until I stop" - for size()
// and for anything out of range, so a dial dragged past the end lands on the
// only thing that is there.
[[nodiscard]] int PeekDetentSecondsAt(int index);

// Which chats a recently-closed grace period covers.
enum class RecentScope : uchar {
	// Only a chat that was in the preset's view when it was opened. Reading it
	// is what would have taken it out, so this is the narrow repair: nothing
	// you open can pull in a chat the preset was hiding.
	AlreadyInView,

	// Any chat you open, hidden or not. One rule - a chat you have looked at
	// recently is in the view - and the only one of the three that helps when
	// you reach a hidden chat through search or through an extra view.
	AnyOpenChat,

	// The above, minus the chats that are already one click away somewhere else
	// in this preset: on an extra view, or in a folder whose tab is showing.
	AnyOpenChatExceptInFolder,
};

// "already_in_view", "any_open_chat", "any_open_chat_except_in_folder".
[[nodiscard]] std::optional<RecentScope> ParseRecentScope(const QString &value);
[[nodiscard]] QString RecentScopeName(RecentScope value);

// Keeping a chat in the view for a while after you stop looking at it. Without
// it a gated chat vanishes on the exact frame you click away from it, which is
// both startling and wrong: having just read something is the best evidence
// there is that you are still working on it.
// How the chat list marks a row that is only there for the moment - one inside
// its close buffer, or one a "show until" is holding open. Both are chats that
// will leave on a clock, and neither is otherwise distinguishable from a chat
// the preset simply lets through.
enum class RecentStyle : uchar {
	None,   // The default. Nothing is drawn.
	Stripe, // A bar down the row's left edge.
	Timer,  // A ring where the date sits, emptying as the time runs out.
};

// "none", "stripe", "timer".
[[nodiscard]] std::optional<RecentStyle> ParseRecentStyle(const QString &value);
[[nodiscard]] QString RecentStyleName(RecentStyle value);

struct Recent {
	int staySecondsAfterClose = 0; // Zero disables it entirely.
	RecentScope scope = RecentScope::AlreadyInView;
	RecentStyle style = RecentStyle::None;
};

// How far a "hide until" reaches. It always takes the chat out of the preset's
// own view; the question this answers is what happens to the folder tabs, where
// the chat is a member of somebody else's list and the preset has no say over
// the rows at all.
enum class HideScope : uchar {
	// Out of the chat list entirely, the way `hide_everywhere_p' does it for a
	// whole preset: no row anywhere, on any tab, for as long as it lasts.
	Everywhere,

	// The default. The row stays on its folder tabs, but its unread stops
	// counting towards them - so the folder does not sit there with a badge for
	// a chat you have just put away. A hidden chat that still drives a number
	// is the one thing that would keep pulling your eye back to it.
	KeepInFolderUncounted,

	// The row stays and keeps counting. What a "hide until" did before this key
	// existed, kept because "out of my view, but still part of that folder's
	// total" is a coherent thing to want.
	KeepInFolder,
};

// "hide_everywhere", "keep_in_folder_but_exclude_from_badge_count",
// "keep_in_folder".
[[nodiscard]] std::optional<HideScope> ParseHideScope(const QString &value);
[[nodiscard]] QString HideScopeName(HideScope value);

// The `[overrides]' table: how the three "until" menu entries behave. Not part
// of a preset - the menu offers the same decision whichever preset is running,
// and the overrides themselves live in state.toml rather than here.
struct Overrides {
	HideScope hideScope = HideScope::KeepInFolderUncounted;
};

// The `[suggestions]' table: what the strips of chats the app suggests for you
// - recent searches, frequent contacts, the share sheet's targets - do with a
// chat the running preset hides.
//
// Not part of a preset, and for the same reason [peek], [overrides] and
// [recent] are not: it is a decision about those strips, not about what any one
// preset lets through.
struct Suggestions {
	// Whether a chat the preset hides is left out of them. Only ever asked
	// while a preset is filtering - under Normal the strips are stock.
	bool hideInvisible = true;

	// Whether the "similar channels" strip a channel offers is shown at all.
	// Off by default, unlike everything else here: it is the one suggestion
	// not assembled out of your own chats - the server picks it - so it is the
	// one a fork about deciding who reaches you should not switch on for you.
	// Asked under every preset, Normal included, because it is a claim about
	// the strip itself rather than about what a preset lets through.
	bool recommendedChannels = false;
};

// The `[sync]' table: what settings.toml does on its way to your other
// devices. See docs/purple/sync.md in the desktop fork.
struct Sync {
	// Whether saving settings.toml also sends it to Saved Messages, so the
	// other machine has something newer to find on its next launch. Off by
	// default because sending is a message in a real chat: it should be
	// something you asked for once, in words, and not something an upgrade
	// started doing on your behalf.
	bool sendAfterSave = false;
};

// The `[last_seen]' table: what the fork says about a last seen it cannot
// read, and whether it will offer to trade for one.
//
// Not part of a preset, for the same reason [peek], [overrides], [recent] and
// [suggestions] are not: it is a decision about how a status line reads, not
// about what any one preset lets through.
struct LastSeen {
	// Whether the reason a last seen is coarse is appended to the status text
	// in the chat header and the profile. On by default: "last seen recently"
	// with no reason is the fork withholding something it knows.
	bool reasons = true;

	// Whether the "show mine to see theirs" sheet is offered at all. Every way
	// into it is gated on this and on nothing else, so turning it off leaves
	// the explanations standing and takes away the offer.
	//
	// Two lines can open the sheet, and `reasons_p' reaches only one of them:
	// the reason tail, which is drawn only when explanations are on, and the
	// remembered line, which is drawn either way and so stays tappable either
	// way. LastSeenNoteNow() in purple_state.h is where that is decided, for
	// both apps at once.
	bool trade = true;

	// How long to wait for their exact `was_online' after asking, in seconds,
	// before putting our privacy rules back. Short by design: the whole
	// exposure is this window, and a trade that has not answered in ten
	// seconds is not going to.
	int tradeHoldSeconds = 10;

	// How long a read stays worth showing, in seconds. Past it the remembered
	// moment is dropped rather than shown as older and older news - "last seen
	// 14:32, as of 3 min ago" is useful and "as of 2 days ago" is not.
	int tradeRememberSeconds = 24 * 3600;

	// The least time between two trades with the same person, in seconds. One
	// trade is a moment of exposure you chose; a trade every time you open
	// their chat is a standing subscription you did not.
	int tradeCooldownSeconds = 5 * 60;
};

// What a last seen the server coarsened is coarse BECAUSE of. Three answers
// and no fourth: the fork never guesses at a block, because the server has no
// field that says so and "blocked" is not a thing to be wrong about.
enum class LastSeenReason : uchar {
	// Nothing to say. An exact `was_online' (there is no reason to explain),
	// or `userStatusEmpty' - "a long time ago" - which is inactivity or a
	// block and the server does not say which.
	None,

	// Coarse, and the server says it is because of OUR privacy rules: we do
	// not show them ours, so they do not show us theirs. The one case with
	// something the user can act on, and what the trade is offered for.
	ByMe,

	// Coarse, and not by us. Their setting, and nothing to offer about it.
	HiddenByThem,
};

// The mapping, given what the status carried:
//
// - `exactKnown' is a status with a real `was_online' in it;
// - `coarse' is one of userStatusRecently / LastWeek / LastMonth;
// - `byMe' is the `by_me' flag the server sets on a coarse status when the
//   coarsening is the consequence of our own privacy rules.
//
// A status that is neither exact nor coarse is userStatusEmpty or offline
// with nothing usable, and gets None.
[[nodiscard]] LastSeenReason ReasonFor(
	bool exactKnown,
	bool coarse,
	bool byMe);

// What a stretch of screen time was spent in: one of the chat kinds, or the
// time that was not in a chat at all - the list, search, settings - which is
// "elsewhere" so that the splits add up to foreground time rather than to
// something smaller with no name.
//
// A separate vocabulary from ChatKind because ChatKind is what a list's
// `kinds' accepts, and there is no such thing as a list of elsewheres. The
// four real spellings are the same as ChatKind's, so `kind:groups' in a budget
// and `kinds = ["groups"]' in a list mean the same word.
enum class ScreenTimeKind : uchar {
	Private,
	Group,
	Channel,
	Bot,
	Elsewhere,
};

// "private", "groups", "channels", "bots", "elsewhere" - and the singular
// spellings too, since a person writing `kind:channel' in a budget has said
// exactly what they meant.
[[nodiscard]] std::optional<ScreenTimeKind> ParseScreenTimeKind(
	const QString &value);
[[nodiscard]] QString ScreenTimeKindName(ScreenTimeKind value);
[[nodiscard]] ScreenTimeKind ScreenTimeKindFor(ChatKind kind);

// What a budget does when the day's allowance is gone.
enum class BudgetMode : uchar {
	// A bulletin at the limit and nothing else. The default: a budget you
	// wrote down is first of all a thing you wanted to know about.
	Soft,

	// The chat goes behind a cover naming the budget, with one snooze offered.
	// Never touches messages or notifications - it is a screen, not a mute.
	Hard,
};

// "soft", "hard".
[[nodiscard]] std::optional<BudgetMode> ParseBudgetMode(const QString &value);
[[nodiscard]] QString BudgetModeName(BudgetMode value);

// What a budget is counting. Spelled in the file as one string - "all",
// "chat:<id>", "kind:<kind>", "preset:<name>" - because a budget names one
// thing and a table of four mutually exclusive keys would let it name two.
enum class BudgetTarget : uchar {
	All,
	Chat,
	Kind,
	Preset,
};

// One `[[screen_time.budgets]]' entry.
struct ScreenTimeBudget {
	// The target as written, kept so a screen and a warning can say back what
	// the file said rather than what it was understood as.
	QString target;

	BudgetTarget kind = BudgetTarget::All;

	// Filled according to `kind'. Only one of the three ever means anything,
	// which is what the target string decided.
	PeerIdValue chat = 0;
	ScreenTimeKind chatKind = ScreenTimeKind::Elsewhere;
	QString preset;

	// The day's allowance, in seconds. Zero is a budget that is reached the
	// moment the day starts, which is a coherent thing to ask for.
	int perDaySeconds = 0;

	BudgetMode mode = BudgetMode::Soft;

	// How long one snooze lasts, in seconds, and how many are offered in a
	// day. Zero for either disables snoozing, so a hard cap can be made
	// absolute by writing `snoozes_per_day = 0'.
	int snoozeSeconds = 5 * 60;
	int snoozesPerDay = 2;

	// Where this budget sits in the raw [[screen_time.budgets]] array, counted
	// from zero and counting the budgets the parser threw away - the same
	// address, for the same reason, as ScheduleRule::sourceIndex. A budget has
	// no name key either: what it is about is the target, and a screen editing
	// one already has to say which budget it read.
	//
	// -1 for a budget that did not come from a file at all.
	int sourceIndex = -1;
};

// The `[screen_time]' table. Off until switched on: it is a log of what you
// looked at and for how long, and nothing should start keeping one of those
// because a version number moved.
struct ScreenTime {
	bool enabled = false;

	// How long one action counts as active for, in seconds. Short, because a
	// burst of typing is one action and not one per keystroke.
	int actionSpanSeconds = 3;

	// How close two actions have to be for the whole gap between them to count
	// as active, in seconds. This is what makes a conversation read as active
	// time rather than as a row of three-second spikes.
	int activeGapSeconds = 30;

	// How long without any input pauses the session, in seconds. Applied to
	// the raw log at read time, so changing it re-derives history - which is
	// the whole reason the log stores events and not totals.
	int idleAfterSeconds = 60;

	// How many days of the log to keep. Zero keeps everything.
	int retentionDays = 90;

	std::vector<ScreenTimeBudget> budgets;
};

struct Premium {
	bool enabled = true;
};

// A friendly name for a device id, out of [devices]. The id is what a ruleset
// names and what the client reports for itself - a machine-readable thing that
// is no use on a screen - so the file gets to say what to call it. Nothing
// depends on a device being listed here: an unlisted id shows as itself.
struct DeviceLabel {
	QString id;
	QString label;
};

struct Settings {
	// What the file says it is, as written. A file with no `version' at all is
	// one - every file written before the key existed is - and that is read in
	// silence rather than warned about, since an absent key here is a fact
	// about when the file was written and not a mistake in it.
	int version = kSettingsVersion;

	Premium premium;

	// Definitions only, in file order. Priority is a preset's business.
	std::vector<List> lists;

	std::vector<Preset> presets;
	Schedule schedule;
	FocusSync focusSync;
	Peek peek;
	Recent recent;
	Overrides overrides;
	Suggestions suggestions;
	Sync sync;
	LastSeen lastSeen;
	ScreenTime screenTime;

	// [devices], in file order.
	std::vector<DeviceLabel> devices;

	[[nodiscard]] const List *list(const QString &name) const;
	[[nodiscard]] const Preset *preset(const QString &name) const;

	// The label written for this device id, or null. Ignores case, the same as
	// a ruleset matching one.
	[[nodiscard]] const DeviceLabel *device(const QString &id) const;
};

// Everything recoverable is a warning and leaves usable settings behind; only
// a TOML syntax error sets `error', because nothing else leaves the file
// without something meaningful to run on. See docs/purple/config.md.
struct ParseResult {
	Settings settings;
	std::vector<QString> warnings;
	QString error;

	[[nodiscard]] bool ok() const {
		return error.isEmpty();
	}
};

[[nodiscard]] ParseResult ParseSettings(
	const QString &text,
	const QString &path);

// The name the parser will not accept for a preset, because the engine uses it
// for the stock-behaviour bypass.
[[nodiscard]] bool IsReservedPresetName(const QString &name);

// Whether exit_preset says "previous" - put back whatever was active when the
// focus mode came on, rather than a preset named outright.
[[nodiscard]] bool IsPreviousPresetName(const QString &name);

// The preset name with its first letter capitalised, which is what a preset
// that did not write `default_view_name' calls its tab. Only the first letter:
// a preset called "deep focus" becomes "Deep focus", not "Deep Focus", because
// guessing at word boundaries in a name the user chose is how you end up
// mangling one.
//
// And only when there is no capital in it anywhere. A name that already has one
// has had its casing decided - "iH" stays "iH" - and the whole point of the
// rule is to tidy a name nobody thought about, not to overrule one somebody
// did.
[[nodiscard]] QString DefaultViewName(const QString &preset);

// The label a preset carries wherever one is shown to the user: its own
// `default_view_name', or the above. The tab standing in for All chats, the
// preset picker and every line naming the preset that silenced a chat all read
// the same word this way - the picker used to offer "work" above a tab that
// said "Work", and a preset that renamed its tab was two names for one thing.
[[nodiscard]] QString PresetTitle(
	const QString &name,
	const QString &viewName);
[[nodiscard]] QString PresetTitle(const Preset &preset);

// The spread marker: "*core" in a list_order or folders array splices in the
// entries of [list_sets.core] or [folder_sets.core]. Nothing if the string is
// not a reference at all.
[[nodiscard]] std::optional<QString> SpreadReference(const QString &value);

// The one built-in folder set, written "*ALL": every real folder the account
// has, with whatever flags the entry carries. It survives parsing as a
// PresetFolder holding this exact name, because the parser has never heard of
// a Telegram folder and cannot expand it - the display side does, in place, so
// the entry keeps its position in the strip and its flags. The asterisk stays
// in the name so it can never collide with a folder actually called "ALL".
[[nodiscard]] const QString &AllFoldersName();

// Whether this entry is that marker rather than a folder of the user's.
[[nodiscard]] bool IsAllFolders(const PresetFolder &folder);

// "90s", "2m", "1h", "0" / "off" for no timer. Nothing for unparseable input.
[[nodiscard]] std::optional<int> ParseDuration(const QString &value);

// "HH:MM" to minutes since midnight.
[[nodiscard]] std::optional<int> ParseTimeOfDay(const QString &value);

// And back, zero-padded, so a time the app writes into the file looks like one
// a person typed. Empty for a number that is not a time of day.
[[nodiscard]] QString TimeOfDayText(int minutes);

// "mon" .. "sun" to Qt::Monday .. Qt::Sunday.
[[nodiscard]] std::optional<int> ParseWeekday(const QString &value);

// And back. Empty for anything outside Monday .. Sunday.
[[nodiscard]] QString WeekdayName(int day);

} // namespace Purple
