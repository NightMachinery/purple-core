# Config sync decisions

`purple/purple_config_sync.{h,cpp}` holds the pure decisions both clients make
about `settings.toml` when they check the selected sync space. The client reads
a fresh, complete inventory, extracts the newest supported config record of
each install as a `ConfigHead`, fingerprints the local file, and asks the core
what that means. The core performs no I/O and produces no user text.

Terms used below:

- **Local fingerprint** is `SettingsFingerprint()` of the current file bytes.
- **Remote heads** are the heads of other installs in the selected space.
- **Ordering** is the one deterministic order used whenever the core picks a
  representative head or lists heads: generation descending, then sequence
  descending, then install id ascending, then key ascending.
- **Own head** is this install's newest config record in the selected space,
  taken from the same inventory as the remote heads.
- **Known** keys of a state are its base, its base lineage and its equivalent
  keys.

## PlanConfigSync

`PlanConfigSync(localFp, state, remoteHeads, ownHead)` classifies the heads
with `ClassifyConfig` and returns one verdict, the classification, the heads it
offers for a choice or an update (`offered`), every head whose content equals
the local file (`same`), the own head it was given (`ownHead`) and whether that
own head is stale (`ownStale`). `ownHead` is optional; a caller that omits it
gets exactly the behavior the planner had before own heads existed.

A device's own newest record can stop describing it. When a device adopts a
version that does not descend from its own record, every other device still
sees that old record, finds it among its own known keys, and calls it `Stale`,
while this device has already seen their records. Both sides then report
UpToDate with different settings. For example, A and B both start at R, A
publishes V and B publishes W concurrently, A picks W and B picks V: without
the own head both end up UpToDate, A holding W and B holding V. The own head
lets the planner notice that its record no longer matches its state.

The own head is **stale** when the base is set and its key is not known. One
exception keeps long histories quiet: lineages hold at most 64 ancestors, so
when the base lineage is full and the own head's generation is not above the
oldest generation the lineage still guarantees (the smallest generation after
its first two entries), the own head may simply have been trimmed from the
lineage and counts as not stale. Without that exception a device that only
ever applies updates would report LocalChanges again after every 64 remote
versions.

An **unjoined preview** is a state whose install is empty, whose space is set,
and whose base, base lineage, equivalent keys, pending key and seen sequences
are all empty. A device that has not joined yet plans with such a state, so
joining and daily use share one decision. In a preview no head counts as this
install's own. Any other state with an empty install is invalid, and
`ClassifyConfig` itself still rejects an empty install.

The first matching verdict wins:

1. **Invalid**: the state or local fingerprint is invalid, or some head is
   `Invalid`, or an own head was given that does not belong here: it is in
   another space, it is not this install's (an unjoined preview has no own
   head), its sequence is 0, its key or lineage is invalid, or the base is
   empty while no send is pending (an install that has posted has a base once
   its read-back is confirmed). Nothing is offered, `same` is empty and
   `ownStale` is false.
2. **Pending**: this install has a staged, unconfirmed own record. Only
   finishing that send is allowed.
3. **Conflict**: some head is `Concurrent`, or the heads split. A split counts
   only when the classification reports one and at least two distinct contents
   remain among the `Ahead`, `Concurrent` and `Unrelated` heads. Maximal heads
   that are already known to this install (`Stale`) cannot force a conflict;
   they arise, for example, after this device merged two versions that the
   other devices have not seen yet.
4. **Choose**: some head is `Unrelated`: a first join with different settings,
   or a device with unrelated history.
5. **UpdateReady**: some head is `Ahead`. When the `Ahead` heads carry two or
   more distinct contents, the verdict is Conflict instead and offers each of
   them. The classification reports no split when a maximal head matches the
   local file, yet those heads still changed the settings without seeing each
   other, and an update offers only one head, so the others would stay hidden.
6. **Adopt**: some head has the same content as the local file.
7. **Empty**: the base is empty and no other install has any head in the
   space, whether or not its sequence was already seen. This is the only
   verdict that may publish a version without parents.
8. **LocalChanges**: the base is set and either the local file differs from
   the base content or the own head is stale.
9. **UpToDate**.

An own head whose record is still pending (the base is empty or older, and a
send is in flight) reaches Pending rather than Invalid, so Finish sending stays
available after a crash between the post and its read-back.

For UpdateReady, `offered` is exactly the first `Ahead` head. For Conflict and
Choose, it holds one representative per distinct content among the `Ahead`,
`Concurrent` and `Unrelated` heads, the first of each content in the ordering,
listed in the ordering. It is empty for every other verdict. `same` lists
every `Same` head in the ordering for every verdict except Invalid.

## AdoptConfigHeads

`AdoptConfigHeads(state, localFp, heads)` returns the next state once the local
file is known to have `localFp` and every head in `heads` carries that same
content. The client calls it after an update was written, after a choice, or
for a silent Adopt, and persists the result.

It returns nothing when `heads` is empty, when a send is pending, when the
state fails the checks `ClassifyConfig` applies (an unjoined preview state
cannot adopt; the install must exist first), or when any head is in another
space, belongs to this install, has sequence 0, has an invalid key or lineage,
or has a fingerprint other than `localFp`.

The versions being recorded are the heads, plus the current base when it
already has `localFp`. All of them carry the same content. The newest of them
(highest generation) becomes the base, and every other one joins the
equivalent keys, together with the old equivalent keys when the base content
is unchanged. The base lineage becomes the union of all their lineages plus the
other versions' keys, newest first, cut to the 64 newest. The base's own
ancestors win ties.

Keeping the newest version and the merged lineage matters. Suppose this device
kept an older base and filed a newer same-content version under the
equivalent keys. It would forget that version's history, so a version the
newer one had replaced could come back as Ahead, and the device would apply
old settings and then call itself UpToDate. When the base is empty or its
content differs, the result is the same as before this rule: the first head
in the ordering becomes the base.

Equivalent keys never include the base, never repeat, are kept sorted by
generation descending and then key ascending, and are cut to the 16 first in
that order, the limit `ClassifyConfig` enforces. Each head's install gets a
seen sequence of at least that head's sequence. Space, install and pending key
are unchanged.

## PlanConfigChoice

`PlanConfigChoice(state, plan, chosenRemoteKey)` turns the user's choice in a
review into the work the client performs. `chosenRemoteKey` names one offered
head; `std::nullopt` means keep this device's text. The result says whether to
write a head's text into the settings file (`writeRemote`, `write`), which
heads to record afterwards with `AdoptConfigHeads` (`adopt`), which other heads
to record as seen (`seen`, see below), and whether to publish (`publish`,
`parents`). The client builds the published record from the current local text
after any write, so publishing always means publishing the local text with
these parents. A parent is `ConfigVersion{ key, {}, lineage }` built from a
head, or from the base and base lineage.

- **UpdateReady**: the key must be the one offered head; keeping the local
  text is not a choice here (the client offers Not now, which changes
  nothing). Write that head, adopt every head that is not `Stale` and carries
  its content, and list every other head as seen. Nothing is published.
- **Adopt**: no key. Adopt `plan.same` and list the `Stale` heads as seen.
  Nothing is written or published.
- **LocalChanges**: no key. Publish with the base as a parent, and the own
  head as the second parent when it is stale, so the other devices see this
  device's current settings supersede its old record.
- **Empty**: no key. Publish with no parents.
- **Choose or Conflict, remote key R** (one of the offered heads): write R,
  adopt every head that is not `Stale` and carries R's content, and list the
  `Stale` heads as seen. Whether to publish, and with which parents, is exactly
  what a fresh check would propose right after that write, adoption and seen
  record. The core works this out by applying them and planning again on the
  same heads and own head.
  - One other offered content remains: the fresh check is Choose or Conflict,
    and keeping local there publishes with R and that content as parents. The
    other devices then see an ordinary update instead of a lasting conflict.
  - Two or more other contents remain: the fresh check is a Conflict among
    them, so the parents are the first two of them. Every other device then
    sees an update, and the devices holding R's content see Same.
  - R was the only offered content: the fresh check is LocalChanges when the
    own head is stale after the adoption, and the parents are R and the own
    head. Otherwise it is UpToDate and nothing is published.
  - If the fresh check is anything else, nothing is published. The one known
    case: the local file was edited, R has the base's content, and another
    offered head descends from the base. That head then returns as an update.

  When R is among the parents it comes first.
- **Choose or Conflict, keep local**: with no `plan.same`, publish with the
  first two offered heads as parents. When only one head is offered, the base
  fills the second slot, unless the base is empty, is that head, or already
  appears in that head's lineage. With `plan.same`, adopt it, list the `Stale`
  heads as seen, and publish exactly what a fresh check would then propose,
  worked out as for a remote pick. The adoption can move the base to a newer
  same-content version, and the fresh check then puts that version in the
  base's slot.
- **Invalid, Pending, UpToDate**: nothing to choose.

`seen` lists heads the choice records without adopting them. The client
raises each one's install seen sequence to at least the head's sequence
(`CompleteSyncConfigApply` does this), so the head stays out of later checks
until that device posts again. Adopting a different content replaces the base
lineage with the adopted heads' lineages. Without `seen`, a head this device
had already accounted for would then come back as `Unrelated` on the next
check and be offered as a choice that takes the device back: a `Same` head
beside an update holds the settings the update just replaced, and a `Stale`
head may be known only through the history the adoption dropped. Recording a
head as seen claims nothing about it in later posts; its device still sees
this device's versions as concurrent and decides for itself.

A choice that does not fit the verdict returns nothing, and so does any
parent set that `MakeConfigVersion` would reject (for example a parent at the
largest generation), so the client never stages a record the core cannot
build.

This gives the client a check it can rely on. After it writes R (or keeps its
text), adopts `adopt` and records `seen`, a fresh `PlanConfigSync` on the same
inventory and own head returns LocalChanges, Choose or Conflict whenever a
Choose or Conflict answer promised a publish. The fresh plan's
`PlanConfigChoice(..., std::nullopt)` then publishes with the same set of
parents without adopting anything, and when no publish was promised, it
proposes none. A client that re-plans on the click before posting therefore
posts exactly what the review promised, unless the inventory changed in
between.

Applying an update (UpdateReady) never publishes. A fresh check on the same
heads right after it is UpToDate, or LocalChanges when the own head was
already stale or the applied version descends an equivalent key rather than
the base (and so not the own record); that LocalChanges publishes with the own
head as the second parent.

`TestConfigConvergence` in `tests/test_config.cpp` checks these promises on
seeded random runs of two to five devices that often act on stale views of
each other's heads: an update hides no other content and leaves at most
LocalChanges behind it, a silent adoption does the same, every Choose or
Conflict answer publishes what the fresh check proposes, no plan is Invalid,
and the devices never all report UpToDate with different files.

## DiffConfigText

`DiffConfigText(before, after, context)` is the line diff behind the review's
Show lines. It returns unified-diff hunks: 1-based line numbers, `context`
unchanged lines around each change (3 by default, negative treated as 0), and
hunks merged when their context would touch or overlap. Within a change the
removed lines come before the added ones. A hunk with no old (or new) lines
starts at the line before it, as in `diff -u`, so an insertion at the top of
the file starts at old line 0. `added` and `removed` count every changed line,
and `identical` is set when the two texts have the same lines.

Lines split on LF. A trailing CR is ignored when lines are compared and
stripped from the displayed text, and a missing final newline is not a change
by itself, so CRLF and LF copies of one file are identical here even though
their fingerprints differ. Lines are compared as bytes; invalid UTF-8 is
decoded leniently, with replacement characters, only for display.

The algorithm is Myers' O(ND) difference algorithm in its linear-space form
after the common first and last lines are stripped, so memory stays linear in
the input and the result is a shortest edit script. Two bounds keep it fast on
any input of up to 256 KiB per side:

- **Edit limit**: when the shortest edit script is longer than
  `kConfigDiffEditLimit` (1,000 changed lines, counting removed plus added),
  the diff is reported as one hunk that removes every old line and adds every
  new one, with `truncated` set. A change that large is a rewrite, and a full
  replacement reads better than a thousand scattered hunks.
- **Work budget**: the search also stops after 5 million steps (diagonals
  visited plus lines compared) with the same truncated result. Only very
  repetitive texts, such as thousands of identical lines around many small
  changes, come near it.

Measured on 256 KiB inputs with an optimized build on an M2 under load: a
dozen scattered edits take about 1 ms, two unrelated files about 6 ms, and the
most repetitive inputs tried about 25 to 35 ms, mostly spent building the
truncated result's text. Unoptimized builds are several times slower.

## SummarizeConfigChange

`SummarizeConfigChange(before, after)` is the review's change summary. It
parses both texts with the vendored toml++ and compares values, not lines, so
comments, whitespace, line endings, key order inside a table, string quoting
and number spelling (`0x10` against `16`) produce no entry. A change of type
(`1` against `1.0`) is a change. When either side fails to parse, `parsed` is
false and there are no entries; the client then shows the line diff alone.

Each entry has a kind (Changed, Added or Removed), the TOML table it is about
(`table`, as dotted TOML with quoted keys where needed) and an English label.
The units compared are:

- Each table under the keyed collections `lists`, `presets`, `list_sets` and
  `folder_sets`: `[lists.work]` is List "work", `[presets.gym]` Preset "gym",
  `[list_sets.x]` List set "x" and `[folder_sets.x]` Folder set "x". Anything
  nested inside, such as a preset's `[[presets.work.views]]`, belongs to that
  entry. Keys directly in the collection table, and the order of its tables
  (which is the order the apps show them in), form one more entry labelled
  Lists, Presets, List sets or Folder sets.
- Each element of an array of tables whose elements all carry a unique string
  `name`, directly under a top-level table or at the top level. For
  `[[schedule.rulesets]]` the label is Schedule "name"; any other such array
  uses its dotted name followed by the quoted name. When names are missing or
  repeated on either side, the array is compared whole as part of its parent.
- Every other top-level table as a whole. Tables users edit have readable
  labels: `[devices]` Device names, `[notifications]` Notification previews,
  `[premium]` Local Premium, `[schedule]` Schedule settings (its own keys,
  the flat `[[schedule.rules]]` array and the order of its rulesets),
  `[focus_sync]` Focus sync, `[peek]` Peek, `[recent]` Recent chats,
  `[overrides]` Overrides, `[suggestions]` Suggestions, `[sync]` Send on save,
  `[last_seen]` Last seen and `[screen_time]` Screen time. Any other table is
  labelled with its dotted TOML name.
- The top-level keys outside every table, such as `version`, together form one
  entry with an empty `table`, labelled Top-level settings.

A unit present on both sides is Changed when its values or order differ. A
unit present on one side only is Added or Removed, except that a table whose
only content is split into entries of its own (for example a `[schedule]`
holding nothing but rulesets) produces no entry itself. Entries are ordered
Changed, then Added, then Removed, and within each kind by label ignoring
case, then by label and table. The labels are English; a client that
localizes can derive its own wording from `table` and the kind of unit.
