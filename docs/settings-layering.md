# Pure account settings layering

SOURCE_IMPLEMENTED_UNQUALIFIED. This unit, the preceding account codecs and the
scope/reset unit have prepared genuine cases but no compiler or test execution.
Root qualification, independent review and integration are still required.

## Version semantics

`kDefaultSettingsVersion` is 1. `kSettingsVersion` remains a compatibility alias
for that default. `Settings::version`, a missing version and an invalid version
fallback still read as 1. `kHighestSettingsVersion` is 2 and controls newer-file
warnings, record inspection and record publication. Explicit versions 1 and 2
are understood; 3 still warns and has NewerSchema/NeedsReview publish/sync guards.
A valid version 2 is not itself a reason to reject a record in the new core.

Record schema is still the file's parsed version. A file omitting version
therefore still publishes schema 1. Ordinary splice writes and exports keep the
file's existing bytes/version. Only the previously explicit app-wide-removal
action writes version 2 here. Account-file creation, its version-2 writer and
frontend import/publish/document-name integration remain pending. In particular,
Android's separate supported-schema constant and the clients' highest-version
questions must follow 2 when their integration lands. No frontend was changed.

Six old future-version test boundaries now use 3, including the payload schema
and matching observed version values. Their NewerSchema, warning, NeedsReview
and no-publication expectations are preserved. `obsolete-version-boundary-updates.json`
in the private handoff records every exact before/after change. Legitimate
version-2 inputs, the old missing-version assertion and all unrelated checks
stay unchanged. These source fixtures have not been executed.

## API and inputs

`LayerSettings(globalText, globalPath, accountText, accountPath, options = {})`
is pure. The named four-text/path call remains available. It neither opens nor
writes a file and never reads account/profile/config state. Inputs are const
strings; they are never normalized or serialized back.

`SettingsLayerOptions` defaults to own list membership. Its optional
`followListMembers` skips only list-member overlays. Its `globalPresent` and
`accountPresent` distinguish missing files from present empty text. Missing
account text inherits the global per-account settings and built-in file-local
defaults. Present empty text is a usable empty overlay. Missing global text
means a default global base. An absent file does not select a stale `.good`.

The caller may supply independent `globalGood` and `accountGood` text/path pairs.
For a present malformed file, its own usable fallback is selected. A present
valid file, including empty text, takes precedence. An invalid or absent good
cannot supply values. A broken account with no usable own good inherits the
base; a broken global with no usable own good supplies defaults. One file's good
is never used for the other. If an existing frontend policy has already chosen
a last-good global source, the caller can provide that chosen source as the
global text/path. The pure API does not replace filesystem/watch/last-good policy.
The frontend must pass each file's presence and its available usable-good bytes
before selecting defaults; an omitted option cannot magically read its good.

`LayerSettingsResult.effective` is a real ParseResult containing effective
Settings and the actual reader trace. Its warnings are deliberately empty.
`global` and `account` separately retain current path, presence and their real
ParseResult; a supplied fallback actually tried gets its own path/ParseResult.
`usable` and `usedGood` state the selection. An original syntax error is retained
even when a good source supplies effective values. Warnings contain the real
own-file lines and are owned by the report's path. They are never relabelled as
the global file or copied into an app-wide banner. The consumer must keep these
report groups separate. Unknown keys retain the real parser's silent behavior.

## Merge and source positions

The API runs the unchanged real readers on the independently parsed sources.
The account's selected tree is retained for native moves. The selected global
text is then parsed again into a fresh tree. No toml::table copy is used:
vendored toml++ copy constructors drop source information, while node moves
retain it. `ReadSettingsTree` extracts the old reader body unchanged and is
shared with the existing ParseSettings API.

File-local nodes are removed from the inherited global tree first. Account
file-local values then replace their own nodes; omitted version becomes 1 and
omitted sync.send_after_save_p is off. Account per-account fields move with
native `insert_or_assign(key, std::move(node))`. Arrays are moved whole and
replace, including empty arrays. The central SettingsScope and
SettingsScopeOverride queries decide each key's scope before node-kind handling.
Named exceptions never become a child-path default merely because they contain
a table or array. App-wide values are inactive; unknown root content is ignored.

Root table namespaces merge by key. A malformed non-table per-account namespace
is ignored with its own parser diagnostic, rather than replacing the entire
global namespace and erasing app-wide exceptions inside it. A structured leaf
value at a per-account key is moved as that key, so its real reader applies the
usual warning/default. These operations do not rewrite or destroy either
input. The earlier destructive-removal API's atomic structured-exception
refusal remains byte-exact and is not weakened.

Inherited lists, presets, sets and their expanded contents keep the global
source positions/order. Moved account nodes keep their original paths/lines.
The `applied` vector records source positions from the actual destination node
AFTER its native move, so consumers and genuine cases can inspect that fact.
Merged-pass diagnostics are discarded; the independent own-file reader reports
already name the correct sources and lines.

## Lists and inactive facts

List existence, title, kinds and preset priority come only from the selected
global source. Only the named per-account members key can overlay a global list
recognized by the genuine global readers. The API never creates an account-only
list. Missing members inherit; an explicit empty array clears IDs for that list
while global kinds still match. Other named lists inherit independently.
Decoded quoted names containing dots remain one component.

Under follow, all own list-member values are inactive and stay global; ordinary
notification and Last Seen per-account fields still overlay. Resolving the
global identity, stock/Normal behavior, timers and client state remain outside
this pure API.

`inactive` contains typed source facts for app-wide values, members of undefined
global lists and own members skipped by follow. Facts carry decoded table/key,
actual source path/line/column and a reason. No frontend toast or account data
is manufactured. Inactive title/kinds/member bytes stay untouched in the
caller's original own text. Unknown content produces no new parser warnings.

## Prepared qualification and remaining work

The genuine test_config additions cover missing/explicit/future versions,
schema-1 omitted-version publication, per-key inheritance and explicit false,
array replacement/empty arrays, file-local noninheritance, list fork/empty/kinds,
undefined and quoted names, follow and two-account isolation. They also cover
fresh global list/preset/set/folder order, moved-node source positions, malformed
namespace/structured leaf behavior, separate bad/good sources, missing/empty
states, path/line warnings and unchanged input strings. Existing assertions
remain exact except the six documented obsolete future-version boundaries.
The 100 codec, 73 scope and 26 repair static check sites remain intact.

No new TU or runner registration is needed. All 18 genuine production units and
both real test programs must be qualified later. This source-only handoff does
not measure parse cost, actual file fallback, watchers, client banners or native
behavior. Frontend AccountFiles, migration/startup owners, UI, scope-bearing
reviews, auto-send fingerprints, Relink, document-name guards, stores and
transport remain pending. Playlist entries remain registry contracts only.
