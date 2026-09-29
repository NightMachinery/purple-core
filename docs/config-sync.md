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
