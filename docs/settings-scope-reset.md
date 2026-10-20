# Settings scope and lossless removal

SOURCE_IMPLEMENTED_UNQUALIFIED. This source candidate and its prepared genuine
test cases have not been compiled or run. The preceding account-codec source
opinion carries, but neither unit has genuine compiler/test qualification yet.

## The registry and real-reader completeness

`SettingsScope(table, key)` answers `PerAccount`, `AppWide`, `FileLocal` or
`Unknown`. The central `SettingsScopeRegistry()` sits in `purple_settings.cpp`
beside the real readers. Entries describe table defaults, named exceptions,
root scalar keys and explicitly approved future contracts. Scope is always
queried per key. The component-vector overload preserves literal names that
contain dots. The string overload accepts TOML header contents, including
quoted components, through the genuine toml++ parser. Invalid addresses return
Unknown. A root-key query uses an empty table path.

The approved account-settings design sections 5.1 through 5.6 provide the scope
rule. Work Mode sections 4.1, 5.1 and 10.1 add the nested list-member exception.
Notifications and Last Seen use per-account table defaults. Last Seen's
`trade_remember` and `trade_cooldown` are app-wide exceptions. The lists subtree
is app-wide except `members` directly under a named list, which is per-account.
Root `version` and the entire sync subtree are file-local. The other current
tables are app-wide. New keys inherit the registered table default.

`ParseResult::readRoots` is populated by the actual root lookup wrapper used by
every current reader, including lookups of absent tables. It does not derive
from another handwritten expected-table list. The prepared completeness case
parses an empty document, checks every actual requested root against the
registry, then removes `peek` from a registry copy and verifies that the same
actual lookup trace identifies it as missing. A new reader's root lookup thus
requires a registry entry. The wrapper borrows the parsed table for the same
parse lifetime and changes no settings value, warning, ordering or fallback.

The current reader inventory is:

- Root scalars: `version` through `ReadVersion`; retired root `list_order`
  through the warning in `ParseSettings`. The latter remains app-wide.
- `notifications`, through `ReadNotifications`, including `preview_always`.
  The `private`/`user` token synonym is a value spelling, not another table.
- `premium`, read directly in `ParseSettings`; retired `enabled` warns while
  `enabled_p` is active. Both remain in the app-wide subtree.
- `lists.<name>`, through `ReadLists`; structure, `title` and `kinds` remain
  app-wide and `members` is the named per-account exception. Retired `show`,
  `notify` and `locked` warn and stay app-wide.
- `list_sets.<name>.list_order` and `folder_sets.<name>.folders`, through the
  same `ReadSets` with its real section/key arguments. Their inline list/folder
  entry tables inherit app-wide scope.
- `presets.<name>`, through `ReadPresets`, and its list/folder entry tables,
  pinned arrays and `views` array of tables through `ReadViews` and `Expander`.
  All remain app-wide. Retired preset `inherit`, `overrides`,
  `groups_require_mention` and `hide_everywhere` are warnings, not active aliases.
  Retired inline entry spellings `show_p`, `groups_require_mention_p`,
  `filtered`, `include_in_main_view_p` and `pinned_only_p` inherit that scope.
- `schedule`, `schedule.rules`, `schedule.rulesets` and
  `schedule.rulesets.rules`, through `ReadSchedule`, `ReadScheduleRules` and
  `ReadRulesets`, all app-wide, including inline representations.
- `focus_sync`, `peek`, `recent`, `overrides`, `suggestions` and `devices`,
  through their corresponding real readers, all app-wide. Device labels are
  dynamic keys within the one devices table. Every Peek key, including delay
  and exit controls, remains app-wide; no per-account Peek state is introduced.
- `screen_time` and its `budgets` array of tables, through `ReadScreenTime`
  and `ReadBudgets`, all app-wide.
- `sync`, through `ReadSync`, including `send_after_save_p`, file-local.
- `last_seen`, through `ReadLastSeen`, per-account for `reasons_p`, `trade_p`
  and `trade_hold`, with the two app-wide duration exceptions above.

Approved future `playlists` and `music` entries are registry contracts only.
They add no reader or playlist behavior. Their default is per-account;
`playlists.menu_item_p` and `keep_downloaded_p` are app-wide exceptions.
`music.private_listening_p` remains per-account. Completeness explicitly
distinguishes future entries from current actual reader roots.

Unknown root tables/keys return Unknown and are retained. Unknown keys inside
a registered table inherit its default, as the approved design specifies.
App-wide and file-local subtree defaults extend through subtables. Unregistered
subtables under a per-account table remain Unknown and are retained. A quoted
root named `"lists.work"` is unrelated to the two-component path `lists.work`.

## Reset

`RemoveTableKey(text, path, table, key)` takes TOML header contents for the table
and a literal decoded key. It removes only the proven assignment's complete
source lines, including its own trailing comment and multiline array/value
body. It leaves every other byte, independent comment, unknown key, order,
spacing and line ending unchanged. Quoted keys containing `=` or `#` are
validated by toml++, not split by a regular expression.

The target must be a normal explicit table header, or the root. Dotted table
addresses such as `lists."work.one"` are supported; dotted assignment syntax
such as `last_seen.trade_p = true` is refused. Inline target tables,
array-of-tables addresses and structured table/nested-array values are refused.
Missing tables/keys are unchanged no-ops. A table header is removed only when
its sole key is removed and there are no remaining comment lines or header
comment. Unrelated explicit empty parent tables are retained.

The operation uses native source regions and a genuine parse of the assignment
key. It then reparses and compares the full expected TOML structure, including
unknown data. No DOM serialization rewrites the input. Refusal always returns
the original text with `changed == false` and an error.

## Explicit app-wide removal

`RemoveAppWideSettings(text, path)` removes app-wide content and writes an
explicit `version = 2`. It keeps per-account list members and their table
headers, notification/Last Seen account keys, file-local data and unknown
outside content. App-wide unknown keys inside registered app-wide tables are
removed according to the table's default. Last Seen and future playlist
exceptions are removed by key rather than by deleting their per-account tables.

Actual normal and array-of-tables headers are identified from the parsed tree
and checked against their decoded paths. Header-looking text in multiline
strings is never a boundary. A wholly app-wide physical table block goes from
its real header to the next real header; its internal comments and blank lines
go with it. Root preamble comments and bytes belonging to retained blocks stay.
Blocks may be interleaved with unknown tables without absorbing them.

Pure app-wide inline assignments and ordinary app-wide array-of-tables blocks
are supported. Dotted assignments requiring deletion and partial edits inside
mixed inline tables or retained array-of-tables elements are refused atomically.
Untouched unknown or per-account representations are not rewritten merely for
being unusual. A final native parse must exactly match the intended semantic
filter, with every retained value and container preserved.

Only valid versions 1 and 2 are accepted for this destructive action; invalid
or newer versions refuse unchanged. An existing version token becomes 2 while
its spacing/comment remain. A missing version is inserted at the root using
the file's newline convention. Repeating the action is unchanged. Other splice
operations and missing-version parsing still use the existing default 1.
The following source-only version/layering unit now supplies the core's
default-1/highest-2 split and core record guards; see settings-layering.md.
Frontend version/document-name integration remains pending. The compatibility
`kSettingsVersion` default remains 1.

## Prepared verification and remaining work

The genuine existing `test_config` gains scope/reader-completeness, exception,
dynamic/quoted path, unknown-data, Reset and bulk-removal cases. They cover
missing/last/commented keys, multiline arrays, quoted lookalikes, inline/dotted/
AOT refusals, table boundaries, LF/CRLF, version behavior and idempotence.
The original test body and all preceding codec cases are preserved exactly.
`test_sync_flow` and the existing 18-source runner registration are unchanged;
the implementation extends existing production TUs. No test was run.

Next steps are real compiler/API and genuine case qualification, independent
new-delta review, then root integration. The source-only core version split and
`LayerSettings` now have their own candidate documented in settings-layering.md.
Frontend version integration, scope-bearing diffs, client Reset/removal UI,
startup owner recovery, migration, per-account stores, transport and playlists
remain separate work. No frontend
hook or device-state format changes in this unit.

## Structured named-exception repair

P2-1 is addressed by an atomic preflight before block edits, version changes or
expected-tree filtering. `SettingsScopeOverride(components, key)` resolves only
explicit named exceptions from the same registry that `SettingsScope` uses.
It returns an optional scope independently of the value's TOML node type. The
splicer contains no separate list of exception keys.

A table-valued exception, or an exception array containing any non-scalar
value, refuses the entire app-wide removal with original text, changed false
and an error. This includes normal/inline tables, dotted table representations,
AOT, homogeneous/mixed table arrays and nested arrays. The preflight walks all
parsed tables and array elements, checking the parent path and key before
descending. Thus neither physical block selection nor the expected-tree filter
can reinterpret an unsupported structured exception through a child default.

Scalar values and flat arrays remain supported, including empty arrays. Their
parent-key scope still controls preservation/removal. Unrelated unknown
structured data remains unchanged. This adds no destructive syntax support and
does not change Reset, codecs, parser values, scope defaults or version policy.

The added genuine `test_config` regressions exercise all five registered named
exceptions in both scope directions, structured representations, atomic refusal
with removable content before the offending value, quoted dynamic names,
ordinary scalar/array controls and unrelated unknown structures. They are
prepared source only, NOT RUN. The preceding 73 scope and 100 codec check sites
and their entire original test body remain intact. Compiler qualification and
focused independent closure remain required before root integration.
