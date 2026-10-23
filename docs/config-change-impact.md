# Settings replacement impact

`AnalyzeConfigChange(input)` in `purple_config_diff.h` supplies the shared facts
for imports, offers, editor saves and sync choices. It performs no I/O and
changes no input. `proposedText` preserves the exact proposed bytes, including
unknown content, comments and line endings. Clients still own the destination,
backup, fresh-byte/identity checks and the actual write.

## Inputs and refusal

`current` and `proposed` pair bytes with explicit presence. An absent source is
an empty document, regardless of its supplied bytes. A present empty document
is valid. `role` is `Global` for the app-wide source on this device, or `Layered`
for another account's file; role is independent of the file's schema version.
`workMode` is that account's device-local own/follow/stock choice.

For a layered file, supply the current `global` source to determine whether its
own list members apply. For a global replacement, supply `accounts`: each
loaded account's opaque identity, own-file bytes/presence and Work Mode choice.
These allow stranded-member warnings to name the affected account. Account
identities must be nonempty and unique. Only the context for the chosen role is
read. The caller supplies all relevant loaded accounts; core discovers none.

`Ready` means every relevant source parsed and is understood. Syntax errors
return InvalidCurrent, InvalidProposed, InvalidGlobal or InvalidAccount (with
its account identity). A schema above 2 returns NewerSchema. Invalid enum or
account-context values return InvalidContext. Refusals have no findings or
semantic summary and must not be treated as safe replacements. A caller using
a last-good source must select it explicitly before this call. Omitted or
invalid version values retain the existing parser fallback of 1. Raw integer
versions above 2 refuse even when they exceed the parser's int range. Explicit 2
is supported, and the default/highest version constants are unchanged.

## Findings and presentation

`summary` retains the existing semantic summary units and labels. Its key
changes now include optional `beforeValue` and `afterValue` strings formatted
as semantic TOML for display. Missing optionals mean absence. A collection-order
unit has an empty marker for each existing side rather than a literal value.
These strings are never replacement file bytes. `addressKind` continues to
distinguish literal keys (including empty keys), units and list definitions.

Each `findings` entry carries a kind, the addressed change and scope, label,
before/after use reasons, optional affected account and counts where relevant.
ValueChanged appears once per changed summary key. Pinned/Unpinned additionally
identify a per-account key newly present/absent in a layered file, even when a
pin equals the inherited value. Removing a pin and explicitly setting an empty
array remain distinct operations.

Use reasons distinguish Used, AppWideElsewhere, Follow, Stock, UndefinedList,
Unknown and Absent. Follow and stock make own list members inactive without
changing ordinary notification/Last Seen values. Follow on the global account
acts as own. App-wide global facts remain used even for a stock global account,
since they still supply every other account's Work Mode. Global members are
also used when a loaded non-stock account inherits them, even if the global
account itself is stock. Group changes by the
key facts; never use a mixed table's overall scope for that decision.

AppWideLoss reports removed registry-app-wide keys. ListLoss reports each lost
list definition, including schema-only whole-to-layered transitions. PresetLoss
reports each removed preset. ScheduleLoss reports removals or replaced rule
array entries, with the whole schedule before/after; its beforeCount is the
number of lost scalar fields, empty containers or array entries, afterCount 0.
Pure rule reordering, including within named rulesets, is not schedule loss.
Only known rule arrays receive that order-independent comparison; other ruleset
fields retain ordinary TOML equality. A modified rule counts as replacing
the old rule and conservatively needs review.

DefinedListEmptied reports a surviving list defined by the proposed file whose
own parsed members and kinds now both match nothing. It uses the file itself,
so an inherited nonempty list on this device cannot hide the risk to a device
where this account is global. A version-2 members-only fork does not define a
list; resetting it is ordinary unpinning. A list that still matches by kinds is
not empty. BeforeCount is its prior member count; the full values also preserve
kinds. StrandedMembers identifies each loaded account's nonempty own members
for a list removed from the global source, with unchanged member values/counts
and the account's use reasons on each side. Follow/stock accounts retain these
facts even when their members were already inactive.

All loss/emptying/stranding findings set `dangerous`, as does the overall result
if any such finding exists. Danger is independent of local use. Clients must
show the account-aware warning and avoid preselection or a default apply action
for dangerous choices. Finding kinds overlap intentionally: a removed preset
can have AppWideLoss key details and a PresetLoss whole-unit fact. Use kind to
select warning counts instead of summing the entire vector.

Registry-unknown changes remain Unknown, and their removal has an additional
UnknownRemoval fact, never AppWideLoss. Unknown keys inside a registered table
continue to inherit that table's scope under the established registry contract.
The exact proposed unknown text survives in `proposedText`.

Legacy export filename predicates do not live in core. Clients must accept
`settings.toml` and `account-settings.toml` and continue to name layered exports
with the latter. This API does not authorize sending an absent file.

Focused cases run through `tests/run.sh` alongside the existing config and
sync-flow suites. They cover role, mixed scopes, pin/unpin, own/follow/stock,
emptying, stranded accounts, unknown text, omitted version, refusal and account
byte isolation. Client review text and replacement/transport integration remain
separate work.
