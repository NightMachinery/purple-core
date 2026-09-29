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

## PlanConfigSync

`PlanConfigSync(localFp, state, remoteHeads)` classifies the heads with
`ClassifyConfig` and returns one verdict, the classification, the heads it
offers for a choice or an update (`offered`), and every head whose content
equals the local file (`same`).

An **unjoined preview** is a state whose install is empty, whose space is set,
and whose base, base lineage, equivalent keys, pending key and seen sequences
are all empty. A device that has not joined yet plans with such a state, so
joining and daily use share one decision. In a preview no head counts as this
install's own. Any other state with an empty install is invalid, and
`ClassifyConfig` itself still rejects an empty install.

The first matching verdict wins:

1. **Invalid**: the state or local fingerprint is invalid, or some head is
   `Invalid`. Nothing is offered and `same` is empty.
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
5. **UpdateReady**: some head is `Ahead`.
6. **Adopt**: some head has the same content as the local file.
7. **Empty**: the base is empty and no other install has any head in the
   space, whether or not its sequence was already seen. This is the only
   verdict that may publish a version without parents.
8. **LocalChanges**: the local file differs from the base content.
9. **UpToDate**.

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

When the base is empty or its content differs from `localFp`, the first head
in the ordering becomes the base, its lineage the base lineage, and the other
heads' keys the equivalent keys. When the base already has `localFp`, the base
stays and the equivalent keys gain the heads' keys. Equivalent keys never
include the base, never repeat, are kept sorted by generation descending and
then key ascending, and are cut to the 16 first in that order, the limit
`ClassifyConfig` enforces. Each head's install gets a seen sequence of at least
that head's sequence. Space, install and pending key are unchanged.

## PlanConfigChoice

`PlanConfigChoice(state, plan, chosenRemoteKey)` turns the user's choice in a
review into the work the client performs. `chosenRemoteKey` names one offered
head; `std::nullopt` means keep this device's text. The result says whether to
write a head's text into the settings file (`writeRemote`, `write`), which
heads to record afterwards with `AdoptConfigHeads` (`adopt`), and whether to
publish (`publish`, `parents`). The client builds the published record from
the current local text after any write, so publishing always means publishing
the local text with these parents. A parent is `ConfigVersion{ key, {},
lineage }` built from a head, or from the base and base lineage.

- **UpdateReady**: the key must be the one offered head; keeping the local
  text is not a choice here (the client offers Not now, which changes
  nothing). Write that head, then adopt every head that is not `Stale` and
  carries its content. Nothing is published.
- **Adopt**: no key. Adopt `plan.same`. Nothing is written or published.
- **LocalChanges**: no key. Publish with the base as the only parent.
- **Empty**: no key. Publish with no parents.
- **Choose or Conflict, remote key R** (one of the offered heads): write R and
  adopt every head that is not `Stale` and carries R's content. When another
  offered content remains, also publish, with R and the first offered head of
  another content as parents, so the other devices see an ordinary update
  instead of a lasting conflict. When R was the only offered content, nothing
  is published.
- **Choose or Conflict, keep local**: adopt `plan.same` (possibly none), then
  publish with the first two offered heads as parents. When only one head is
  offered, the base fills the second slot, unless the base is empty, is that
  head, or already appears in that head's lineage.
- **Invalid, Pending, UpToDate**: nothing to choose.

A choice that does not fit the verdict returns nothing, and so does any
parent set that `MakeConfigVersion` would reject (for example a parent at the
largest generation), so the client never stages a record the core cannot
build.

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
