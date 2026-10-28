# Sync chat carry and journal contracts

The carry API moves settings records between a source chat O and destination N.
It performs no I/O. The client supplies account identity, canonical peer tokens,
complete inventories, ownership, eligibility, readback and persistence receipts.
S4 (forwarded bytes and ownership) and S12 (slow mode) remain unmeasured. Nothing
in this API establishes Telegram forwarding behavior or selects a broadcast
ownership branch. Existing non-admin slow-mode refusal still applies.

## Planning and records

`PlanCarry` handles Switch, Follow and Fill. Supply current bound state and its
preference token for an existing install. Fill has no local state. Its chosen source must remain Readable for planning;
Protected, Unreadable, ChatGone and Unknown refuse. Only a complete readable
inventory with no records counts as empty. The plan codec and commit preview
reject forged unreadable Fill plans. The target
must pass `SyncChatEligibility`, including any reader consent. Pending settings
must first finish in O, be reconciled through the protected-source path below,
or be abandoned after an affirmative abstract ChatGone result. A library pending sequence or any other staged file holds the operation.
Unknown source access holds; it never becomes an implicit lost-chat result.

`SelectSyncCarryHeads` checks a complete, account/chat-scoped inventory. Each
message includes fresh raw bytes, date, message id, family and supplied ownership.
Complete means all Purple candidates from the completed chat inventory, not every
ordinary chat message. Legacy settings.toml documents and non-Purple messages
are excluded; presenting an invalid owned candidate holds the plan.
Unknown ownership holds; Other contributes no records. The settings classifier
and directory select every highest-sequence tie per (space, stream, install),
including unsupported stream/encoding and newer settings schemas. Every
newer-major candidate survives independently. Identical raw records deduplicate
by SHA-256 of all bytes, never by Telegram document or message id. Two different
observations for one scoped id refuse. Lower records remain in the inventory
for explicit cleanup policy, without becoming carry heads.

A readable O requires its complete inventory. The plan freezes O heads, missing
forward references, required hashes and cleanup candidates. N must also be fully
inventoried. Protected, unreadable and gone O expose no other install's bytes:
only this install's locally held confirmed base can be reissued. Reissue also
covers a readable source whose current exact own record is missing. It requires
the exact base key/lineage and base text; dirty local text stays local. The core
builds one immutable next-sequence record, reserves it once and appends its exact
issued-record hash. Seeing a higher own sequence, another own device or a current raw-record hash
that conflicts with an existing issued entry holds for review. Legacy current
heads without an issued entry require a valid current payload hash equal to
local ownHash, then a newly issued local-base record in N. A different legacy
payload holds; legacy bytes never become issued evidence merely by observation. Never rebuild that record on a retry.

`VerifySyncCarryReadback` requires a fresh complete N inventory with accepted
ownership and every frozen required raw hash. Forward success, an upload reply,
a matching payload alone, and a partial scan never satisfy it. Missing records
are explicit retry data. Scope mismatches, changed bytes and unknown ownership
refuse. `PrepareSyncCarryStage` reproduces the frozen reservation idempotently.
`SyncCarryPlanUsesState` validates the frozen owner, counters and issued entries
against the source or reserved state image. The journal checks this when it
accepts a plan and prepares a commit. `PrepareSyncCarryCommit` previews the exact
next state image after verification;
it does not authorize writing state before the journal reaches CommitState.
It logically consumes the one exact own carry stage through its verified N
readback before calling `MoveSyncChat`; no unresolved stage remains in that
preview. Its physical bytes remain a journal recovery artifact until state
commit, then RetireOwnStage removes them. This exception belongs only to the
journal helper: ordinary callers must not pass stagePresent=false around a
staged file. All other stages must be absent under the account operation lock.
The helper clears the O ledger, then adopts
only exact issued own records read in N. It selects the smallest N message id
per full raw-record hash, independent of scan order. Duplicate copies cannot
exhaust the ledger; the unique eligible set is bounded by the issued log.
N message ids never enter O's ledger.
Implicit self remains state v1 until a real move; moving back remains v2.

## Lost pending settings

Finish an existing pending publication through its normal send/reconciliation
protocol and immutable request identity; ResolvePending is not permission to
blindly resend it. `FinishPendingStageForCarry` requires the exact issued stage and a fresh Own
readback from O, confirms its version and records its O-scoped message id.
`AbandonStageForLostChat` requires ChatGone, state v2, the exact issued stage,
and retained local text equal to the staged text. Network errors, a missing
mark, migration and Unknown are insufficient. The local desired text must
remain durable independently of the retired stage. Confirmed base and ancestry
stay unchanged, so a later normal settings sync still sees any dirty edit.

The optional v2 config-stream `abandoned_seq` equals the spent seq, has no pending
seq, exceeds confirmed seq and requires the matching last issued entry. It is
forbidden in state v1 and the library stream. Abandonment never decrements seq
or pretends the record was confirmed. The next reservation clears the marker
and uses seq+1. The old reader's contiguous-counter rule rejects this gap;
omitting the marker also rejects it in the new reader. Other unknown JSON
members remain preserved. An abandoned stage must not be silently recovered
as a normal unpublished stage by an older client.

## Durable journal and execution

`BeginSyncCarryJournal` binds a caller-generated operation id, account, O/N,
mode, expected resolver result, immutable request ids, mark text and current
state hash. Absent state needs an affirmative absence fact. Switch needs the
current source cache; Follow and Fill need the exact resolver winner. Resolve
again after pending handling. A changed winner supersedes the operation before
new delivery. The journal never picks an arbitrary fallback chat.

Hold the account operation lock for the operation, including every account
file, stream stage and ordinary sync timer. A live or unreadable journal blocks
ordinary posting. The client owns atomic durable writes, secure operation-id
and request-id generation, freshness, Telegram adapters and operation recovery.
The journal JSON codec rejects duplicate members, malformed scope, unsafe
counters and contradictory phases, while preserving unknown members. It accepts
legacy version 1 and version 2 with the explicit `purple-carry-v2` format marker.
Newer versions and an unrecognized version-2 format hold. Version 2 additionally
records login authority, accepted readers, protected pending bytes, delivery
history and distinct terminal outcomes. The existing 4 MiB JSON bound applies to
the complete journal, including embedded records and unknown fields. An oversize
result refuses before a command may execute; never truncate it or place it in
the 64 KiB admitted cache. Stage files and immutable state images live in
account-scoped sidecars; the journal binds their full SHA-256, not mutable paths.

The ordered actions are:

1. Finish or abandon pending O settings. Prepare the resulting immutable image,
   durably record its hash, atomically write state, then retire the exact old
   stage. Both StateObserved barriers require objectsDurable=true, including
   the later N state commit; Success with false waits at the write phase and
   never permits stage retirement, cache completion or cleanup. A crash after
   either write is reconciled by image/stage identity.
2. Resolve again, freeze and persist the plan. If needed, durably persist the
   exact own record plus reservation state before any delivery.
3. Capture the current forwards/own-post commands. Apply and durably persist
   DeliveryPrepared **before** executing those captured commands once. That
   write moves the next action to VerifyDestination. Report DeliveryResult
   afterward if available. Failure and Unknown never advance past readback.
   Resume reads N before any resend and reuses the same own sequence/bytes and
   ownRequestId. Missing hashes permit bounded-by-client retries; an unavailable
   frozen source requires review or safe replanning before mark/own reservation.
4. Verify N, prepare the exact next-state image durably and record its hash.
   For Switch, persist MarkPostPrepared **before** posting the frozen mark with
   the same markRequestId. The next action becomes ReadMark. An ambiguous result
   always rereads; search absence alone cannot authorize a new post. Absent
   means the adapter has authoritatively reconciled that same request as never
   having landed. Follow and Fill only reread their existing exact winner mark.
   The read must pass the real mark grammar and supplied ownership. A Switch
   mark must outrank every known old winner under the resolver's shared date,
   canonical chat string and message-id ordering (`SyncChatMarkOutranks`). A
   losing or older mark returns NeedsReview with O state/cache intact and
   posting held, even if old-mark cleanup was authorized. It never reports Done.
   A same-second mark may win only when that deterministic tie-break favors N;
   blindly retrying the same request id cannot change a losing mark's date.
   Parsing also refuses saved Switch journals whose accepted target mark loses
   to their known old winner, including an invalid saved Done state.
5. Persist an incomplete cache receipt containing operation/account and the
   exact new mark. Only then atomically write the prepared state image. Retire
   any exact own stage, then mark the cache complete. For no-state Fill, commit
   records affirmative continued absence and creates no binding/state file.
6. Perform only explicitly authorized old cleanup, then persist Done and release
   ordinary sync. Keep the journal through cleanup so interrupted deletion is
   reconciled idempotently. Remove sidecars only after terminal recovery policy.

Every event carries operation id, current revision, account and O/N. Wrong or
stale receipts refuse. Persist **every nonempty canonical result**, including
Wait, Retry, NeedsReview and Superseded, before taking its next action. The
boolean result means Valid only, so it must not be used as a discard predicate
for journals that record ambiguous outcomes. `objectsDurable` affirms that
referenced stages, state images, cache and retained local text really survived
the platform's persistence boundary; it does not make them durable by itself.

On startup parse the journal and call `ResumeSyncCarryJournal` with the actual
state or affirmative absence. When recovery consumes an observed pending or
committed state image as a write-barrier receipt, supply the additional explicit
stateDurableConfirmed=true. Its default is false: the matching image alone
returns Wait with no accepted canonical recovery and no phase advance. A startup
read must come from the durable account store, not a prepared in-memory image.
A matching committed N image and prepared cache
resume cache completion/cleanup. Precommit resumes reverify N and the mark;
an already-reserved own stage is reconstructed exactly, never resequenced.
Missing/mismatched bound state requires review. Safe Cancel requires no pending
files or requests, unchanged source state, no own carry reservation and no mark
attempt/verification or state commit. It leaves unmarked N copies alone and
returns Aborted. After a mark attempt, reconcile and finish rather than rolling
state/cache back across a potentially visible winner. An empty canonical result
is a hold; never execute its journal action. Before a resumed operation performs
new network effects, the client must also resolve afresh under the existing
resolve-before-post rule. The frozen journal alone does not discover a winner
posted elsewhere while it was offline. A changed winner requires explicit
reconciliation, without discarding attempted marks or resequencing an own stage.

## Protected pending settings

`ReconcileProtectedStageForCarry` handles an exact issued config stage in a
bound state v2 whose O is Protected. This never authorizes posting into O.
With `NotIssued`, the caller supplies durable evidence that no send intent was
issued. With `Issued`, supply a complete account/O-scoped readable inventory.
An exact Own copy is confirmed by `FinishPendingStageForCarry` and becomes an
ordinary own version to carry. Complete absence spends the old sequence and
returns `restageInTarget`. Unknown intent, incomplete reads, unknown ownership,
conflicting own identity/sequence, invalid stage, lost retained text or a library
pending stage hold. Protected never means ChatGone; the existing lost-chat path
remains separate. No pending stage returns NoPending without a transition.

For restaging, pass `pendingAccess`, `pendingSendIntent`, `pendingInventory`,
exact `stagedRecord`, retained text and the prepared image hash in PendingPrepared.
Version 2 retains `deferredRecord` before WritePendingState. Persist that journal
before writing the pending-cleared state and retiring the old physical stage.
A crash before the write retries the same image; a crash after it resumes
RetirePendingStage only with a durable state observation. The immutable deferred
record survives retirement in the journal. Resolve and carry the confirmed base
normally, without delivering the deferred version before commit.

Use `PrepareSyncCarryJournalCommit` instead of the plain commit preview for this
path. It returns both the N-bound state image and `targetPendingRecord`, with the
same exact version, lineage and text reserved at the next unused sequence. Make
both durable as recovery objects before DestinationRead; its receipt must match
the returned image hash and stage bytes. Mark and cache barriers then precede
state commit normally. Only after CommitState may the client install the target
stage as the ordinary pending file. RetireOwnStage and CompleteCache require
`targetPendingStageDurable`, `objectsDurable` and exact `stagedRecord` bytes.
After a crash, reinstall those bytes idempotently from the journal before these
barriers; never rebuild or resequence them. Release ordinary publishing only at
a terminal phase. Normal pending handling can then publish the version in N.

## Repairing the bound mark

`SyncCarryMode::RepairMark` is a journal-only operation with O = N. Begin requires
a valid bound state, its cached explicit mark, a direct R3 MarkGone result, a
readable chat, current eligible rights and known readers with required consent.
A winner elsewhere, ChatGone, protected/unreadable chat, pending config/library
or other stage, unknown readers or missing confirmation refuses. Supply a new
login authority and operation/request identities as for other version-2 work.
`PlanCarry` does not accept this mode.

Its phases are Mark, PrepareCache, CommitState, CompleteCache, Done. Persist
MarkPostPrepared before posting; after an uncertain result or restart, read the
same request before any retry. A valid exact Own mark must outrank the known old
mark. Persist the incomplete cache receipt, observe the unchanged durable state
image, then complete the cache. The binding, state bytes, counters and own-message
ledger never change; no record is staged, forwarded or cleaned up. Before the
incomplete cache barrier, restart rereads the mark. After that barrier, a durable
state observation resumes CompleteCache. Retrying cache receipts is idempotent.

## Readers and outward intents

Provide `loginAuthority` to Begin to create a version-2 journal. This is a secure
caller-generated 32-lowercase-hex login-generation token, distinct from the stable
account binding token; keep it unchanged only within that login. Begin freezes
`readerBaseline` from the accepted target eligibility before any delivery or
mark intent, including a mark-only RepairMark. Persist Begin before effects.
The baseline never moves to mark time or changes on retry/replan.

Before each outward write, refresh eligibility for the frozen target and call
`CheckSyncCarryReaders`: Proceed preserves the accepted population;
WarnAndProceed reports known additions while allowing continuation; Hold covers
unknown readers, missing previously known counts or lost eligibility. The client
owns freshness, exact target/account scoping and the warning UI. The helper does
not perform discovery. Do not apply a new first-time consent refusal to known
additions after the baseline has been accepted.

Version-2 DeliveryPrepared records `deliveryAttempted` and all issued forward
references in `deliveryHistory` before the client sends the captured commands.
History survives replanning and terminal abandonment. Reservations and request
identities remain frozen in the plan. A version-2 Absent mark read cannot clear
`markAttempted` unless `neverLandedConfirmed`, `neverLandedRequestId` matching the
frozen request and `noRequestInFlight` supply retained proof of that same request
never landing. Empty search results are insufficient. Without retained proof on
restart, hold and keep reading. Legacy version-1 paths retain their existing
behavior and do not gain these extensions; new integrations must supply login
authority rather than creating a legacy journal.

## Logout and interrupted abandonment

Cancel and SkipCleanup do not implement logout Discard: Cancel deliberately
refuses reservations and attempted marks, while SkipCleanup requires a committed
move. The Start-again recovery contract also does not apply: it requires no live
operation and archives state/pending files instead of retaining a nonterminal
move's unresolved effects.

`AbandonInterruptedOperation` implements explicit Discard after a different login
of the same account. It requires a nonterminal version-2 journal, explicit
confirmation, a valid new login authority different from its original one,
matching actual durable state (or affirmative absence for no-state Fill), and
exact pending-stage bytes. A committed journal accepts only its N commit image.
Unknown state, missing/mismatched stage, same login, wrong account or nondurable
objects refuse or hold. Advance and Resume refuse nonterminal version-2 journals
under a missing or different login; never resume one under the new login.

Persist the returned InterruptedAbandoned journal before releasing the hold.
This terminal transition sends nothing and changes neither the settings/state
nor the pending stage. It retains exact state/stage snapshots, deferred text,
all plan reservations and issued records, mark identity/attempt and every
historical forward identity as unresolved external effects. Action helpers emit
no commands for terminal journals. Clients must retain and expose these fields
to normal discovery, stale-mark handling and record dedup; terminal does not mean
those effects are absent. Pending contents return to normal pending handling,
including retained deferred content. Fresh resolution decides what is actually
in Telegram. Logout must revoke RAM callbacks immediately and retain the journal
outside the admitted cache; removing the account deletes it with account data.
Repeated terminal Resume is a read-only round trip, including under a new login.

## Cleanup and playlists

`SyncCarryCleanupCandidates` is a candidate calculator, not permission to delete.
Use `CheckSyncCarryDeletion` with the journal, fresh source readback and fresh
complete N inventory immediately before each deletion. It only authorizes the
Cleanup phase after durable state commit and cache completion. The explicit
Switch option "Also delete Purple's messages in <old chat>" authorizes
AllSettings for this ACCOUNT, across any install, as specified by design §7.7.
It does not require this install's issued log. Saved Messages is unticked by
default. Clients apply §7.7's checked default for other chats through the switch
confirmation; this pure API always requires affirmative authorization. Authorization covers only ownership-passing
Purple candidates frozen by the carry's completed O inventory, after complete N
readback. Each target must be reread with the exact account, chat, message id,
date, bytes/hash, protocol classification and ownership immediately before its
deletion. Records that appeared after that inventory are never included, even
if they were later copied into N. Legacy settings.toml documents, non-Purple
messages and playlist records never qualify for this settings cleanup.

OwnLatePosts is a separate follower rule: it requires Follow, non-self O, a date
strictly after the winner mark, this install/device's issued exact raw record,
a confirmed sequence and that same hash in N. Other installs' late posts remain
for their followers. The all-install Switch authorization does not broaden this
rule or permit downloading/resending other installs' protected records.
No implicit Fill, self-follow or protected-source deletion exists. Old mark
removal is a separate explicit Switch authorization checked by
`CheckSyncCarryOldMarkDeletion`; it never removes arbitrary marks. Cleanup
completion requires a scoped Absent or ChatGone readback for an explicitly removed
old mark.

Version-2 SkipCleanup is an explicit Cleanup-phase event. It requires committed
N state, complete cache, no in-flight request, Success and `skipCleanupConfirmed`.
Supply `deleted` only for exact frozen references already confirmed deleted or
absent; duplicate, foreign or unplanned references refuse. The terminal
CleanupSkipped phase retains all remaining record and authorized old-mark
references in `abandonedCleanup`, releases the write hold and emits no deletion
commands. It never reports cleanup as Done. A crash before persisting the result
retains the Cleanup hold; after persistence Resume keeps CleanupSkipped.

A CleanupChatGone event requires the same commit/cache/no-request barriers and a
definite account/O-scoped ChatGone readback. It records `cleanupChatGone` and
finishes Done because nothing remains to delete. MarkGone, Absent, protection and
uncertain reads do not qualify. Neither event changes N state or ledger.
`SyncCarryJournalHoldsWrites` stays true for live or invalid journals and false
only for valid Done, Aborted, CleanupSkipped or InterruptedAbandoned journals.

`CheckStaleMarkRemoval` is independent of a move journal. It authorizes only an
exact Own mark readback for this account, after an explicitly complete fresh
resolution has a different current winner that outranks it. The supplied
resolution must be usable and unheld, with matching winner/cache. Wrong scope,
id/date, media, malformed/newer mark, Other/Unknown ownership, incomplete
resolution, same winner or missing winner refuses. Never manufacture a Switch
journal to delete an arbitrary stale mark.

The playlist hook deliberately performs no playlist merge, pruning or forwarding.
A known playlist user, observed playlist-family record or known `library` stream
requires a Verified receipt bound to account, O/N, the exact plan stamp and
complete N inventory
stamp. Readable-source plans also freeze `sourceStamp` over all accepted O
candidates, including playlist bytes. The hook must validate and carry that
source snapshot; changing it requires a new plan and receipt. Missing, failed,
stale or wrong-scope receipts hold the journal before mark/commit. Clients must classify playlist-family messages and supply a proper
future playlist implementation, or hold. They must never assert Verified merely
to bypass this gate.

## Integration and verification

Register these new translation units in Desktop's `Telegram/cmake/purple.cmake`
and Android's native purple-core source list:

- `purple/purple_sync_chat_carry.cpp`
- `purple/purple_sync_chat_carry_codec.cpp`
- `purple/purple_sync_chat_journal.cpp`

Both apps still need adapters, account-operation persistence/recovery, scoped
history reads, user confirmations and the unmeasured server probes. No client
source lists, JNI, network behavior or GUI are implemented by this pure slice.

`tests/test_sync_chat_carry.cpp` drives real state/envelope/mark/resolver functions
against an in-memory cloud. It covers monotonic abandon/finish, raw dedup and
opaque heads, wrong-chat ids, ownership/playlist gates, every journal crash phase
and effect-before-acknowledgement point, pending stages, no-state Fill, protected
and deleted sources, offline late posts, and seeded three-to-five-device chain
and simultaneous moves. These tests establish local contract behavior only.

Run `ARCHIVE_SQLITE_TESTS=1 tests/run.sh` with a configured Qt prefix to
qualify all eleven core executables, including the SQLite integration suite.

The version-2 regression groups cover protected NotIssued/Issued absent/Issued
found paths at every journal phase and effect-before-acknowledgement point,
RepairMark crash barriers and unchanged state, cleanup terminal replay,
interrupted-operation effect retention and login fences, exact target-stage
installation, matched never-landed proof, reader baseline preservation, stale
mark refusal, unreadable Fill and malformed/oversize/unknown-field codecs.
