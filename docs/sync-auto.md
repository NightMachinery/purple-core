# Automatic settings sync policy

`purple_sync_auto.h/.cpp` is the shared pure policy for opted-in accounts.
It returns one next action and a durable per-account run record. It performs
no I/O, starts no timer, opens no dialog, chooses no conflict and joins no
store. Modern automatic settings sync defaults off. Only the explicit Enable
event turns it on; legacy `send_after_save` and library/playlist publishing
are separate and do not enable this policy.

## Triggers and ownership

Feed SessionReady, Foreground, Unlock and an actual LocalSaved event for this
account's file. LocalSaved is delivered after the client's existing save
debounce. Creating a file, applying an import, another account's save and
changing legacy auto-send settings are not LocalSaved events. The account
must be signed in, Q1 ownership answered, its file assigned to it and its
existing sync store bound to its preference token. Automatic mode never
runs setup or joins a discovered store. M2B3 recovery/Relink/Start again remain
explicitly separate from this policy.

Startup/foreground/unlock checks start at most once per account in 15 minutes.
A local save bypasses that gate. Triggers during an active step coalesce;
a newer settled save replaces the queued fingerprint. There is no periodic
polling timer. Fresh reads needed inside an existing transaction or to recover
an uncertain outcome are continuation steps rather than new startup checks.
The client may arm only the emitted retry deadline, never a polling loop.
While a retry deadline is pending, every ordinary trigger coalesces, even after
a restart or after the deadline has elapsed. Only Retry continues that run at
the deadline. New saved bytes update the pending intent without replacing an
already staged record. Neither triggers nor restarts reset the retry budget;
successful progress or an eligible trigger after a terminal stop resets it.
The persisted `runStopped` field records the terminal phase of `run`, separately
from the attention reason and enabled state. Gate Holds, Disable and restarts
preserve that phase; the next eligible ordinary trigger increments `run` and
starts a fresh budget. An ineligible trigger or Retry cannot start a new run.
Disable takes effect immediately. Holds and Disable suspend active backoff
without changing its deadline or count. Once enabled and eligible again, resume
the existing Retry at or after its retained deadline.

The record belongs to a full account key, including server environment. All
facts, inventory and exact server reads must refer to that account and chat.
A state-v2 non-self stream is held as SyncChatNotResolved under the frozen
policy, including after carry support is available. Existing local-state and
sync-chat decisions remain unchanged for manual callers.

## Persist before execution

The client serializes transitions with its per-account lock. It assigns a
strictly increasing event sequence, loads the current record and calls
`PlanSyncAuto`. Persist the returned record with compare-and-swap against the
input record before executing its action. Persist coalesced/NoOp transitions
too. A duplicate or stale sequence produces no action. A losing CAS must
recompute; it must not execute the discarded plan.

Each emitted operation has a ticket and session owner. Return Completed only
for that ticket and the owning session. Session identifiers must change when
an old executor can no longer be trusted. A live operation owned by another
session blocks execution; stale callbacks cannot consume a new session's
operation. These facts are assertions from the client executor, not locks that
pure core can acquire or authenticate.

The record codec is independently versioned, currently 1. It stores exact
64-bit decimal counters, explicit terminal-run state, bounded pending bytes,
notices and unknown fields. The unreleased schema-1 codec requires `runStopped`;
older drafts missing it remain opaque and cannot execute automatic work.
Malformed/newer records retain opaque input and provide no executable record.
Preserve those bytes and hold automatic work; do not silently reset to an
enabled record. This is a new per-account file, not a settings schema or
`SyncLocalState` format change. A step whose checkpoint would exceed the record
limits is held before any effect is authorized.

## Actions

- Check requests a fresh complete inventory. Its observation must carry the
  requested account, chat, session, ticket and a read time no earlier than the
  request. Use the production candidate classifier and account inventory
  finisher. Cached inventory must not be relabelled fresh.
- AutoApply is offered only for one strict successor of the current base,
  with unchanged local base bytes, a Ready safe impact, no loss, no conflict
  and unchanged Work Mode/schema scope. Supply every relevant loaded-account
  and global source for impact analysis and assert that context is complete.
  The plan includes the existing apply plan, exact replacement bytes and next
  state. The executor owns History, backup, atomic file replacement, fresh
  account/path/Work Mode guards and exact readback. Applying a remote version
  must not synthesize a LocalSaved trigger. Completion verifies file and state.
- Publish stages the exact current saved bytes through existing post, sequence
  reservation and issued-log planners. `savedFingerprint` must come from an
  actual account-local saved file/provenance record, not mere file creation.
  Existing exact staged bytes finish first; they are never rebuilt from newer
  editor text. Write/read back the canonical stage and next local state using
  the existing pending-store crash protocol before reporting Staged.
- EditInPlace and Post execute only after a fresh inventory sees no unseen
  head, with exact staged bytes and issued-log authorization. Supported edits
  additionally require a fresh owned exact read of the ledger head. Definitive
  edit refusal goes through a fresh Reconcile before post fallback. A save
  racing an operation stays queued for a later fresh check.
- Reconcile normally requests a fresh read. When `commitOnly` is true, its
  `nextState` commits an exact found issued record or a verified absent obsolete
  message, without a new send. Persist that state and clear the completed stage
  before Committed. A send callback alone never confirms publication. Duplicate
  byte-identical posts reconcile to an existing id, never another blind post.
- Retire authorizes deletion of at most four obsolete records per run. The
  exact server read must be fresh, owned and byte-identical to this install's
  issued/own-message ledger in the current chat and space. No other install,
  current head, unissued record or wider carry cleanup is authorized. Deleted
  means a definitive deletion result and durable state removal; uncertainty
  requires fresh reconciliation. Supply a fresh exact-id server read with typed
  Present, Absent or Unknown presence; default Unknown never proves absence.
  Omission from the candidate inventory does not prove message absence. Only
  affirmative Absent with no body and no contradictory candidate confirms it.
  Unknown/missing reads retry; changed bytes/ownership hold. Unchanged owned
  Present bytes can authorize another bounded attempt, preserving the ledger.
  Refusal is held for attention.
- Hold provides a reason and at most one attention notice per account per
  distinct reason. The client renders status/badge data, never a background
  dialog. RetryAt uses 30 seconds, 2 minutes, then 10 minutes. Another failure
  stops until a subsequent trigger. Retry events before the deadline do nothing.

## Restart and client integration

Persisting an emitted step does not make its side effect atomic. A restarted
session reconciles staged/sent/retiring work from fresh local and server facts.
It never treats a lost callback as proof of absence. If an apply was interrupted,
the fully matching file and state allow progress; an incomplete apply is held
as InterruptedApply for existing recovery/manual review, rather than repeating
an unguarded file replacement. Pending store inconsistencies are held.

Bind completion facts to the exact physical store and current session. Recheck
chat eligibility and ownership immediately before Telegram writes. Honor the
per-account lock and per-session in-flight owner, preserve pending bytes across
logout/restart, and follow existing readback/issued-log rules. Keep auto record,
local state and staged-file commits in the same recovery protocol; this library
does not provide a filesystem transaction or network authenticity.

Clients register `purple_sync_auto.cpp` and `purple_sync_auto_state.cpp` in their
core source lists. They own per-account persistence, lifecycle/save hooks,
retry scheduling, fresh inventories, owned message reads, localized status and
the opt-in control. `tests/test_sync_auto.cpp` exercises the shared pure policy
alongside all existing suites in `tests/run.sh`. No client executor or UI is
activated by adding these core modules.
