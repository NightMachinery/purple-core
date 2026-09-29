# Sync status contract

`ResolveSyncStatus` is a pure projection of engine facts. Both clients can use
its `SyncStatus` value to choose a localized status label, its
`SyncAttentionTier` to decide where to surface the situation, and its
`SyncPrimaryAction` to offer a localized control. The core produces no user
text and never performs I/O.

The engine supplies `enabled` and `manuallyPaused`. Off and manual pause override
all active sync facts. While sync is active, the first true condition wins in
this order: needs a choice, paused for a problem, update ready, file error,
can't sync, waiting for connection, syncing, up to date. A problem reason maps
to the relevant action: choose an account, keep syncing after deletion, update
the client, or open details. A conflict or unresolved fork supplies
`needsChoice`.

An update is Tier 1. A choice or problem is Tier 2. Normal waiting, syncing,
rate-limit delays, and up to date are Tier 0. The engine sets `attentionDue`
when a waiting change or failed change reaches its notice threshold, such as
an hour. The model then returns Tier 1 for the selected waiting or failure
status. It does not retain timestamps, track episodes, or deduplicate notices;
the caller handles those jobs. An explicit Sync now result and successful
user action are separate events, so the caller reports them directly instead
of trying to encode them as persistent status.

The caller must not infer a file error or sync failure solely from a transient
request error. It supplies `fileError` when the current settings file cannot be
synced and `cantSync` when a transport or server condition blocks sync. A
connection wait uses `waitingForConnection`. These independent facts allow the
same precedence to hold when several conditions are present.
