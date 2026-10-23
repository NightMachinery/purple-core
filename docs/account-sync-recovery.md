# Account sync recovery and reset

The recovery APIs in `purple_sync_config_flow.h` are pure plans. They read only
supplied snapshots. They do not discover stores, change preferences, move files,
create identities, send messages or replace settings. Clients hold their account
operation lock, refresh the evidence on the click, execute authorized steps and
verify every expected byte before continuing.

## Binding and ownership

`PlanSyncAccountRecovery` takes the stable `AccountKey`, immutable startup owner,
platform layout, sign-in status, current preference token, complete store
discovery, other account preferences and optional cloud evidence. A store with
`directoryAccount` is a keyed `accounts/<key>/sync` store; no directory account
means the legacy root store. This field describes a verified directory identity,
not a caller-selected account slot. Snapshots retain exact state bytes and a
complete pending-file map when available. The pending config record is selected
by the state's `config.pendingSeq`, using `config-<sequence>.json`.

Results:

- Bound: the existing preference already matches the unique valid store.
- AutoRelink: a unique keyed non-root store supplies its valid token. Its path is
  the ownership proof; normal inventory, clone, pending and file checks still run.
- RootRelink: `ReviewSyncConfigRelink` proved the root store against a fresh,
  complete inventory for this exact account and complete store-space evidence.
  The ordinary review remains unbound until the client explicitly restores the
  token. Relink skips only binding; every existing cloud/state/fallback guard stays.
- StartAgain: the unique targeted store is unreadable, lacks a token, or a complete
  root proof refuses ownership. This only offers the explicit archive action.
- Setup: no state or pending data exists for this account. A valid old preference
  token can be reused; setup must not rotate it merely because the store is gone.
- WaitingForSignIn, Incomplete, NeedsReview, Ambiguous or InvalidInput: no relink
  or reset is authorized. Signed-out edits and identities remain untouched.

The planner refuses duplicate store identities, multiple target stores, a keyed
copy for the root owner, duplicate preference identities/tokens, tokens claimed
by other accounts/stores, invalid identities and malformed preference tokens.
It never chooses an owner among ambiguous candidates. Root cloud evidence names
the full account identity, including Telegram environment, as well as matching
the inventory's user id. An incomplete cloud read does not prove another owner.
A last-good/invalid primary blocks root proof rather than bypassing its checks.
The client must report complete discovery and complete store-space reads only
after actually performing them. Bound and AutoRelink do not claim normal sync
checks have passed.

## Start again

`PlanSyncStartAgain` recomputes the recovery verdict and requires a StartAgain
offer, explicit confirmation, no live operation, an empty account send queue,
complete pending contents and an unused archive destination. `archiveDate` is a
valid `yyyyMMdd-HHmmss` label; generated paths use `orphaned-<label>` under that
account's resolved sync directory. Unsafe pending entry names are refused.
The client must reject symlinks/nonregular entries and hold the account lock
through move/readback; the pure core cannot inspect filesystem object types.

The ordered moves contain only `sync/pending` (when present), then
`sync/state.json` (when present). Their destinations are inside the new orphan
directory. Pending contains all stream stages and unknown retained pending
entries, not just the current config stage. History is a separate preserved
path: `sync/history` on Desktop and `sync-history` on Android, under the same
account prefix. Settings, good copies, backups, History and device-local choices
are never archive targets. Invalid state bytes are archived exactly as supplied.

The client durably retains the plan before moving. On a partial failure it
resumes that same plan; it must not start setup because a state path disappeared
or construct a second archive from a half-moved store. Moves never overwrite an
existing archive. A failed archive publication or readback leaves setup blocked.
Do not delete uncertain staged records or send them from the new store.

`CompleteSyncStartAgain` requires a complete observation for the same account and
unchanged preference token. Both original paths must be absent and archived
state/pending presence and bytes must exactly match the plan. Missing, altered
or extra pending entries, a surviving original, changed preferences or another
account return NeedsRecheck; incomplete reads return Incomplete. Only SetupReady
allows setup and clearing that account's Undo row. Other accounts' Undo rows,
preferences, counters and stores remain untouched. The planner never resets a
live send counter; the Busy guard requires operations/queued sends to settle.

`PlanSyncSetupBindingToken` in `purple_sync_local_state.h` returns ReuseExisting
for a valid existing preference token, independent of supplied entropy. With no
preference it returns CreateNew from exactly 16 secure caller-supplied bytes,
or EntropyRequired until they are available. Invalid existing tokens refuse.
`IsSyncBindingToken` is the shared validator used by the normal binding check.
Completion uses this same token selection. Setup still generates a fresh install
identity through the client's existing path after archive completion. Logout
alone never rotates any identity or token.

## Reviews, fallback and account-local state

The `SyncConfigChoices(review, localPublishable, context)` overload attaches
M2B2 replacement impacts. Each choice includes the actual local replacement;
a publishing choice also includes every other offered version it supersedes.
This catches keeping a layered local file over another device's whole file.
Each impact names its replaced version key, or has no key for the current file.
The account role, global file, Work Mode choice and loaded account snapshots
come from `context`; current/proposed text is supplied by the review itself.
Clients must require `impactReady` and avoid preselection/default apply for
`dangerous` choices. The original overload leaves impact explicitly unreviewed.

History restore and Undo have no core review builder. Clients must call
`AnalyzeConfigChange` with the same account context before those replacements,
retain their exact-byte and History guards, and recompute on changed context.
The existing `SyncSettingsFile::usingLastGood` already belongs to each supplied
file, including an absent primary with a good fallback. The existing send queue
fact is per call; clients must provide the originating account's queue/counters.
Core owns no process-global Undo row or Finish counter, so no second map or new
settings/state schema is introduced.
