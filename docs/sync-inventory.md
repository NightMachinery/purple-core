# Sync inventory

`purple/purple_sync_inventory.{h,cpp}` holds the decisions both clients make
while they read one account's Saved Messages for sync records. The client does
the Telegram requests and the downloads; the core decides which messages are
candidates, when the history scan is finished, what each downloaded record is,
whether the whole read can be trusted, and what this install's own records say.
Two clients that disagreed on any of these could show "Up to date" on one
device and "needs review" on another for the same Saved Messages.

Terms used below:

- **Inventory** is one complete pass: the history scan, then a fresh read of
  every candidate message, then `ResolveSyncDirectory`.
- **Opaque record** is a record whose envelope header is readable but whose
  stream, encoding or library payload this version does not understand
  (`UnsupportedStream`, `UnsupportedEncoding` or `UnsupportedLibrary` with a
  header).

## Candidates and pages

`IsSyncHistoryCandidate(meta)` takes a plain description of one history
message: whether it is an ordinary message (not a service or empty one),
whether it was forwarded, whether its media is a real document, its caption,
and the file names of its document. A candidate is an ordinary, unforwarded
document message whose caption contains `#purplesync` (case-sensitive), or one
of whose file names is exactly `Purple settings sync.json` or
`Purple playlists sync.json`.

`SyncHistoryPages` follows a newest-first scan of the account's own chat, one
page of `kSyncHistoryPageSize` (100) messages at a time, offset by the oldest
id seen. `Add(page)` returns Complete for an empty page and More after taking a
page. It returns Stalled, and keeps nothing from that page, when an id is not
positive, exceeds the 32-bit range, is not below the current offset, or is not
strictly below the previous id on the page. Only a Complete scan may be used
to decide anything; a stalled, failed or cancelled scan is incomplete.

## Records and the read

`ClassifySyncCandidate(id, bytes)` classifies downloaded bytes. More than
`kSyncRecordMaximumBytes` (4 MiB) is Oversized, with no bytes kept. Otherwise
the envelope decides NewerMajor, UnsupportedStream, UnsupportedEncoding or
Invalid; a valid `config` envelope is Valid, NewerSchema or Invalid according
to `InspectConfigPayload`, and a valid envelope of any other stream is
UnsupportedLibrary. The client records the transport outcomes itself (Vanished,
Changed, Oversized before download, Inaccessible, RequestFailed, Cancelled) and
stores the document id and edit date on the record.

`AggregateSyncCandidateRead(records)` is Incomplete when any record is
RequestFailed, Inaccessible or Cancelled; otherwise NeedsReview when any record
is neither Valid nor opaque; otherwise Complete. The result does not depend on
the order of the records.

`FinishSyncAccountInventory(accountUserId, scan, read)` assembles the result.
Without a read it is Incomplete. With one, every record becomes a directory
candidate (`SyncDirectoryCandidateOf`: Valid records are validated payloads,
UnsupportedLibrary counts as a valid but unvalidated envelope, the full-record
SHA-256 is set when a header and bytes exist) and `ResolveSyncDirectory` runs,
told the inventory is complete only when the scan is Complete and the read is
not Incomplete. The inventory is then Incomplete in that same case, Complete
when the read is Complete and the directory has no unreadable candidate, no
message id collision and no selected space that cannot be published to, and
NeedsReview otherwise.

## Own records

`ReconcileOwnConfigInventory(state, inventory, expectedAccountUserId, staged)`
checks this install's own config records against its local state. It is
NeedsReview for a missing or different account id and Incomplete for an
incomplete inventory. It is NeedsReview when the inventory needs review, when
the selected or publishable space is not the state's space, or when the state
does not serialize. With a pending stage, the staged record must be this
install's canonical record at the pending sequence, carry the state's own
payload hash, and match the issued record hash.

Every record written by this install in the state's space must then be a Valid,
canonical record by this device; a record by another device under this install
id is CloneDetected with DeviceMismatch, and any other bad record is
NeedsReview. Records at one sequence must be byte-identical, and a record at
the pending sequence must be the staged bytes. The head is the highest
sequence; its lowest message id is the head message and the other ids at that
sequence are listed as duplicates. The result is Absent when there are no own
records, and otherwise Present, unless `CheckSyncClone` reports a clone
(CloneDetected with its verdict) or the staged record was found, which makes it
PendingFound with the lowest message id holding it.
