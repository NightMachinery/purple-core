# Settings sync flow

`purple/purple_sync_config_flow.{h,cpp}` holds the decisions a client makes
between a finished inventory (see [`sync-inventory.md`](sync-inventory.md)) and
a changed `settings.toml`, sync state or Saved Messages post.
`purple/purple_sync_config_describe.{h,cpp}` says which sentence and which
button a result deserves. The client keeps the file reads and writes, History,
its sync state store, Telegram posting and the wording.

Terms used below:

- **Local file** is `SyncSettingsFile`: the status of `settings.toml`
  (Present, Absent or Invalid), its bytes, its fingerprint and
  `usingLastGood`. The client reads the file (at most 256 KiB, a regular file,
  not a symlink) and builds the value with `MakeSyncSettingsFile(status, text,
  usingLastGood)`, which gives an absent file the fingerprint of empty bytes
  and turns an oversized Present into Invalid. `usingLastGood` is true when the
  settings in effect come from the client's last-good copy rather than from
  `settings.toml` (see Running from the last-good copy).
- **Review** is `ReviewSyncConfigInventory(inventory, state, staged, local)`:
  the pure evaluation of one inventory against this device's sync state (null
  when the device has not joined), its staged record and the local file.
- **Shown review** is the review a user saw and acted on. **Fresh review** is
  the review computed again at the moment of acting.
- **Stamp** is `SyncConfigReviewStamp(review)`, a SHA-256 over what a review
  showed. Two reviews with equal stamps offer the same choices.

## Review

The review refuses with UsingLastGood when the local file has `usingLastGood`
set, before any other check, bound or not. It refuses with InvalidSettings
when the local file is Invalid, and with Incomplete or NeedsReview when the
inventory is not complete, needs review, has unreadable candidates or
colliding message ids, or selects a space that cannot be published to. The
client adds AccountUnavailable, AccountUnbound and StoreError from its own
checks; they share the status enum so the describe step covers them. A review
made with a state carries that state's space and config data whatever its
status, so a refused review still shows whether this device has a post waiting
(see Describe).

Without a state the review takes the selected space. With no selected space
the verdict is Empty; otherwise the heads of every install in that space are
extracted and planned against an unjoined state. With a state the inventory's
empty directory takes the state's space, the selected space must be the
state's, the own records must reconcile (Absent, Present or PendingFound; see
`ReconcileOwnConfigInventory`), the own head must parse, and the heads of every
other install are extracted and planned with `PlanConfigSync`.

`ExtractSyncConfigHeads(inventory, space, ownInstall)` takes, for each config
group in the space other than `ownInstall`, the lowest message id among its
head candidates. It returns NeedsReview when a group is ambiguous or has no
supported head, or when the record is missing, not Valid, has a different
full-record hash, or parses to a different space, install or sequence.

## Stamp inputs

The stamp covers, in order: the review status, the account user id, the bound
flag, the review space, the state's space and install, the local file's status
and fingerprint, the verdict, `ownStale`, the offered keys and the same keys
(in plan order), and for every head its message id, space, install, sequence,
key, lineage and the SHA-256 of its text, then the own head's presence and,
when present, its message id, space, install, sequence, key and lineage.

It deliberately leaves out the state's base, lineage, equivalents, pending key
and seen sequences, and the writer's device, platform, app and time. A change
there that does not change the plan is not something the user decided on;
changes that do change the plan show up in the verdict and keys. Each field is
written as its name, byte length and bytes, so no two field lists hash alike.
The stamp is compared only on the device that made it.

## Apply

A client applies a choice (a remote version key, or none for this device's
settings) in this order:

1. `CheckSyncConfigApplyChoice(shown, key)` before any I/O: NeedsReview when
   the shown review is not Ready, its local file is Invalid, or it is unbound
   but carries an install; InvalidChoice when `PlanConfigChoice` refuses.
2. When the shown review is unbound: a fresh unbound review of the same
   inventory and a fresh local file, then `PlanSyncConfigApply(fresh,
   stamp(shown), key)`. Anything but Ready stops. The client then joins.
3. Open the state, read the staged record if any, read the local file again
   and review. `PlanSyncConfigApply(fresh, expected, key)`, where `expected`
   is `stamp(shown)` for a bound review and
   `stamp(SyncConfigJoinedReview(shown, joinedState))` right after a join.
   The joined review is the shown one with the bound flag set, the state's
   install taken from the new state, and, when the shown review had no space,
   the new space.

`PlanSyncConfigApply` returns NeedsRecheck when the stamps differ, the step 1
refusals on the fresh review, and NeedsReview when the chosen remote head has
no record. When Ready it carries the fresh state, heads and own head, the
choice, and for a remote choice the source record, the fingerprint to write,
whether this is an update (so History records "before update" rather than
"before choice"), the History version key (the base key when the local file
still matches it) and whether other versions remain offered.

After writing the file (for a remote choice) and reading it back,
`CompleteSyncConfigApply(plan, file)` checks the file: the source text and
fingerprint for a remote choice, the planned local fingerprint otherwise. It
then adopts the choice's heads with `AdoptConfigHeads` (AdoptRefused when that
fails) and raises the seen sequences of the choice's `seen` heads; `adopted`
holds the resulting state whenever either list is not empty. It plans again on
that state and proposes the next publish: needed when the new verdict is Empty,
LocalChanges, Choose or Conflict and keeping this device's settings publishes
without adopting, with those parents as the expected parents. `promiseKept` is
false when the next verdict breaks what the choice promised: after an update,
anything but UpToDate or LocalChanges; after a remote Choose or Conflict pick,
a different publish than this proposal. Clients log it.

The client commits an adopted state with `CheckSyncConfigDataCommit(state,
next)` before writing: InvalidTransition when anything is pending in the state
or in `next`, or when a seen sequence would drop or disappear; InvalidState
when the result does not serialize and parse back to the same config data;
Unchanged when nothing changes; otherwise Ready with the updated state and its
canonical bytes.

## Posting

`PlanSyncConfigPublishEntry(request, staged)` is the request gate's first
half. A pending-only request (Finish sending) may only finish a staged record
and must carry no expectations; a new-content request needs both the expected
fingerprint and the expected parents and no staged record; everything else is
refused. `PlanSyncConfigPublishGate` is the second half: on the click's
inventory, with the local fingerprint equal to the expected one, the verdict
must be Empty, LocalChanges, Choose or Conflict (UpToDate is AlreadySynced),
and keeping this device's settings must publish without adopting, with parents
equal as a set to the expected ones.

`PlanSyncConfigPost(state, token, staged, inventory, local, request, now,
writer, queue)` returns one step. `queue` says whether the client's own
Telegram send queue still holds an unsent settings record for this account,
one that is still being sent or that failed and can be retried
(`SyncConfigSendQueue::HoldsSyncRecord`), or not (`Empty`). A Check cannot see
such a message, because it is not in Saved Messages yet, so only the client
can report it.

- Finish with a status: NeedsReview for a refused entry, a failed gate, an
  exhausted sequence, a clock at or before the epoch or a publish planner that
  does not reserve; the own reconcile's Incomplete, CloneDetected or
  NeedsReview; AlreadySynced; InvalidSettings for an absent, empty, invalid or
  oversized (over 256 KiB as a record) local file, or one with `usingLastGood`
  set. A pending-only request still finishes with `usingLastGood` set, because
  it sends bytes staged earlier and reads nothing from the file.
- ConfirmFound with a message id and bytes when the staged record is already in
  Saved Messages; the client confirms it without posting.
- Stage with the canonical record and the next config data; the client stages
  them, then calls `PlanSyncConfigStagedPost` with the new state.
- Finish(StillSending) for a pending-only request whose staged record is not
  in Saved Messages while the queue holds a settings record. Posting the staged
  bytes again then would put a second copy in Saved Messages once the first
  arrives. The client tells the person to wait for the earlier copy, or to
  delete it if it failed, and Check again. ConfirmFound takes precedence: when
  the staged record has already arrived, it is confirmed whatever the queue
  says. The queue is ignored on every other path, since a new-content request
  is refused while a record is staged and so never reposts.
- Post with the staged bytes, for a pending-only request whose staged record is
  not in Saved Messages and whose queue is Empty.

`PlanSyncConfigStagedPost(state, token, own, staged)` returns Post when the
publish planner agrees that the staged record was reconciled absent, and
Finish(NeedsReview) otherwise. It takes no queue: a client calls it directly
only right after staging a record it has just built, which no earlier send can
hold, and the pending-only path reaches it through `PlanSyncConfigPost`, which
has already refused a held queue.

## Clients without native state

A client that calls the core statelessly (Android's JNI rebuilds the inventory
on every call) cannot hold the shown review or the pre-stage own reconcile
between calls. Two substitutions are equivalent and tested:

- After a join, the expected stamp may be built from the fresh unlinked review
  made just before joining (step 2 above, with the same file bytes) instead of
  the shown review: their stamps were equal, and `SyncConfigJoinedReview`
  changes only stamp inputs, identically in both.
- After staging, the `own` passed to `PlanSyncConfigStagedPost` may be
  reconciled again from the same inventory with the new state and the staged
  bytes instead of reusing the one from before staging.

## Describe

`DescribeSyncConfigReview(review, localPublishable)` returns a message, an
action, device names, a record time and a count of other devices. Every review
failure status has its own message, with no action (UsingLastGood gives the
UsingLastGood message); NeedsReview with a pending key on a bound device is
NeedsReviewWithPending. A Ready review maps its verdict: Invalid to
InvalidRecords; Pending to Pending with Finish sending; Choose and Conflict to
ChooseBound, ChooseUnbound, ConflictConcurrent, ConflictSplitBound or
ConflictSplitUnbound (`SyncConfigChooseMessage`) with Choose and the offered
devices; UpdateReady to UpdateReady with Review update, its device and time,
or UpdateMissing when the record is gone; Adopt to AdoptBound, or AdoptUnbound
with Join, with the devices already matching; Empty and LocalChanges to
NotPublishableAbsent or NotPublishableInvalid when the local file cannot be
published, else EmptyBound or EmptyUnbound with Publish, and
LocalChangesEdited (the file differs from the base) or LocalChangesOwnStale
with Publish changes; UpToDate to UpToDateAlone or UpToDateWith.

`localPublishable` is `SyncSettingsPublishable(review, device, writer)`: the
review's local file is Present, not empty and without `usingLastGood`, and the
record that keeping it would post is valid and at most 256 KiB. That record is
built the way the post builds it: with this device's id and writer, with the
parents `PlanConfigChoice(review.state, review.plan, std::nullopt)` publishes
with (none when that choice does not publish), and with the largest sequence
and time a record can carry. The parents matter: two of them and a full 64-key
lineage add about 6 KiB, so a file near the limit can fit on its own and still
be too large to post.

`SyncDeviceNameOf(platform, install)` gives the parts of a device name: the
platform with whitespace simplified and cut to 32 characters, and the first 4
characters after the install id's first `-`. The client composes them with its
own word for an empty platform (desktop writes "Device") and removes
duplicate composed names.

`SyncConfigChoices(review, localPublishable)` lists what a choice dialog
offers: each offered head that has a record (only the first for an update),
then this device's settings when the file is publishable and keeping them is a
valid choice. Each choice says whether it also publishes.

`DescribeSyncConfigApplyFailure` sorts a failed apply into JoinedNotWritten,
WrittenStateNotSaved, WrittenNotReadBack or NothingDone, carrying what the
wording needs. `SyncConfigUndoFinished(status)` says whether Undo should be
withdrawn after a restore: yes once it restored, found nothing to do or can
never succeed, no after a settings, History or write failure.

## Running from the last-good copy

Both clients keep a copy of the last `settings.toml` that loaded
(`settings.toml.good`) and run from it when the file is missing, cannot be
read or does not parse. A sync apply in that state would destroy the settings
the device is running: it writes `settings.toml`, History keeps only the
broken or missing file, and the reload that follows parses the new file and
overwrites the last-good copy with it. Undo would then bring back the broken
file and report the old settings as restored while the device runs the synced
ones. So sync changes nothing while the client runs from the copy. The person
fixes `settings.toml` or restores a version of it, and the next Check works as
usual.

The client contract:

- Pass `usingLastGood = true` to `MakeSyncSettingsFile` exactly when the
  settings in effect came from the last-good copy (desktop
  `UsingLastGoodSettings()`, Android `PurpleGate.usedLastGood()`), in every
  local file it passes to the core: reviews, the fresh reviews of an apply,
  and posts.
- Show the UsingLastGood message. It says that `settings.toml` is missing or
  does not load, that this device is running its last working copy, and that
  sync changes nothing until `settings.toml` is fixed or restored. It has no
  action.

The core does the rest. The review refuses with UsingLastGood, so
`SyncConfigChoices` offers nothing and `CheckSyncConfigApplyChoice` returns
NeedsReview. An apply of a review shown before the client fell back gets
NeedsRecheck, because the fresh review's status is part of the stamp.
`SyncSettingsPublishable` is false, and `PlanSyncConfigPost` finishes a
new-content request with InvalidSettings. A post already staged can still be
finished, since that writes nothing to the file.

History restore and Undo are not sync; the core does not plan them, and they
stay available because restoring a version is one way out of this state. A
restore while the client runs from the last-good copy replaces that copy too
(the missing or broken file is kept in History as usual). The person chose
the restored version, so nothing changes without their say, but the copy they
were running is not kept.

## Tests

`tests/test_sync_flow.cpp` runs the flows end to end on in-memory devices, and
`tests/test_config.cpp` covers the planner underneath them. As a manual check
that the tests still bite, mutate one rule at a time in a scratch copy and
confirm the suite fails. Each of these must fail it:

- let a pending-only request carry expectations;
- drop the expected-parents comparison from the gate, or its refusal of a keep
  that would adopt a head (a same-content head that appears after the choice
  was applied leaves the parents unchanged, so only this rule stops the post);
- remove any stamp input, or skip the stamp comparison in
  `PlanSyncConfigApply`;
- drop the seen-sequence rule from the commit check;
- force `promiseKept`, or judge an update's promise by its publish instead of
  its next verdict;
- let the completion propose a publish whose keep would adopt a head, or skip
  recording the choice's `seen` heads;
- drop the full-record hash comparison from `ExtractSyncConfigHeads` (the
  tampered record keeps the head's sequence, so only the hash differs);
- drop the StillSending refusal from `PlanSyncConfigPost` (moving it ahead of
  ConfirmFound, or applying it on paths that do not repost, fails too);
- build the publishability record without the planned parents, or with a small
  sequence and time;
- set a bound review's state only after the inventory refusals, which loses
  NeedsReviewWithPending;
- drop `usingLastGood` from `MakeSyncSettingsFile` or `SameSyncSettingsFile`,
  from the review refusal, from the post refusal or from
  `SyncSettingsPublishable`, or describe UsingLastGood as InvalidSettings;
- in the planner: turn two `Ahead` contents beside a `Same` head back into an
  update, leave out any of the heads a choice lists as seen (the `Same` heads
  of an update, or the `Stale` heads of an update, an adoption or a pick), keep
  this device's settings with the base from before adopting, or drop the
  equivalent keys from the own head's staleness test.

The staged-bytes comparison for ConfirmFound survives such a mutation by
design: `ReconcileOwnConfigInventory` already enforces it.
