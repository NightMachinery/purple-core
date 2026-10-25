# Archive storage contract

The shared SQL, row, envelope, maintenance and lifecycle modules are pure Qt
Core/C++ values and planners. They perform no SQL, file I/O, encryption,
randomness, downloading or Telegram history changes. Only the optional test
links SQLite. The [archive model](archive-core.md) supplies capture and version
semantics; this contract supplies bounded physical execution.

The policy is one SQLite store per account: retain what was seen and media
already fetched by existing auto-download settings, without automatic expiry.
Deleted messages initially have a dedicated per-chat view. Logout removes that
account's archive. Neither app should create a separate SQL or framing dialect.

## Account, capabilities and keys

Identity is the validated environment/user id, never an account slot. Map it
through a trusted, profile-bound catalogue to an owned directory. Desktop's
Purple root is shared across profiles, so use a durable random profile id and
independent store ids. Keep the account/store catalogue and removal intent
outside the removable directory, available even when keys are lost. Do not
accept deletion paths from it. Use owner-only permissions/ACLs, process locking
and protection against symlink/reparse substitution.

A new store gets a fresh random store id (at least 128 bits in a safe token),
a random 32-byte data key, an opaque data-key id and an external work generation.
Re-login creates a fresh store id/key. Check pending removal before opening or
creating anything. Key ids and filenames must not embed peer ids or filenames.

An absent capability defaults to disabled. `ArchiveKeyFacts::capability` explicitly identifies Desktop or Android, Android
API level, and wrapping availability. Android API 21/22 returns
`UnsupportedPlatform`: keep capture/viewing off with a visible API-23 requirement.
There is no legacy wrapping format or plaintext fallback. A missing wrapping
provider also disables opening. Existing data without its matching wrapper/key
is `Unreadable`; report it and offer removal, never silently replace its key or
show a newly created empty archive. Unsupported versions remain read-only.
Pending removal takes priority even when unlocking is unavailable.

`WrapArchiveDataKey` authenticates exactly 32 key bytes as a `WrappedKey`
envelope. Its row identity is the data-key id; its encryption key id names the
platform wrapping key. Desktop derives the wrapping key with HKDF-SHA256 from
its existing random local-storage key, not the network authorization key,
passcode hash, sync token or a truncated local key. Android uses a dedicated,
non-exportable API-23+ Keystore AES-256-GCM key per account/store. Do not reuse
its biometric/authentication-per-use key. Hardware backing is not assumed.

`ArchiveKeyDerivationFor` supplies fixed salt, length-framed info and 32-byte
output length. For `DesktopWrapping`, HKDF input is the local-storage key and
profile id is mandatory. For `IndexTokens`, HKDF input is the archive data key;
use a separate derived key exclusively for HMAC-SHA256. Its account/store/profile
context must stay fixed for that store. Desktop passcode changes protect the
same local key, so ordinary passcode changes do not rewrite archive data.

Platform-key rotation wraps the same data key under the new key with a fresh
nonce, durably publishes and verifies the new wrapper, then retires the old
wrapper/key. A crash must leave one usable wrapper. Data-key rotation requires
a resumable migration retaining both key ids until every row/chunk is durably
resealed; this batch does not implement that migration. Key loss never permits
creating a replacement over old data. Clients own key handles, buffer lifetime
and clearing; no raw key belongs in SQL, preferences, logs or the catalogue.

## Envelope version 1

All frame integers are unsigned big-endian. Exact layout:

1. Eight bytes `PARCHIVE`, 16-bit version 1, 16-bit suite 1.
2. Store-id UTF-8, 16-bit record kind, row-id bytes, key-id UTF-8. Every variable
   field has a preceding 32-bit byte length.
3. Twelve nonce bytes, 32-bit ciphertext byte count, ciphertext, sixteen tag bytes.

Suite 1 is AES-256-GCM. Associated data is the complete header through ciphertext
length, followed by the 32-bit length and UTF-8 canonical AccountKey text.
The account binding is supplied to AEAD, **not serialized in the frame**.
It authenticates account, store, record kind, row identity, key id, nonce,
version, suite and length. Wrong account/key/context, changed bytes, trailing
bytes, truncation and failed authentication expose no plaintext. Future
versions and unknown suites refuse before decrypting; preserve original bytes.

The client callbacks provide actual AEAD and fresh cryptographic nonces, with
unique nonces under each key across retries, processes and crashes. Return
failure on provider errors and expose plaintext only after tag verification.
For Keystore wrapping, create one scoped Cipher initialized for encryption
without a supplied IV; `random(12)` returns that Cipher's generated IV and the
seal callback uses that same Cipher/nonce plus core AAD. Never disable Keystore's
randomized-encryption requirement or share an initialized Cipher across calls.
Core checks framing and sizes, not the cryptographic quality of callbacks.
Tests deliberately use fake cryptographic callbacks.

Plaintext frames are bounded at 4 MiB; row ids at 4096 bytes; store/key ids at
128 safe ASCII bytes. Media uses 1 MiB plaintext chunks, including one empty
chunk for a declared empty file. Pass the account-keyed **blob token**, not its
raw digest, to `ArchiveMediaChunkContext`; total plaintext length and chunk index
are authenticated too. Enforce `ArchiveMediaChunkSize`, all expected chunks,
exact final length and whole-file SHA-256 against the authenticated blob record.
A single opened chunk does not establish a complete file. Never stage plaintext
media or buffer an entire large attachment. Publish immutable encrypted owned
bytes before committing complete references.

## Provisional staging storage contract

The optional `purple_archive_staging.*` protocol adds envelope kinds
**StagingPiece=13** and **StagingProgress=14**. Existing kinds 1 through 12,
envelope version 1, row codecs, SQL format 2 and the DDL fingerprint are
unchanged. Register `purple_archive_staging.cpp` in Desktop/Android's shared-core
source list when adopting streaming retention. Existing complete-file clients
need no API changes. Old core code refuses these new envelope domains; do not
feed provisional frames to a final-media reader.

Use `ArchiveStagingPieceContext` for immutable 256 KiB window images and
`ArchiveStagingRecordContext`/`SealArchiveStagingRecord` for progress. Their row
ids are account/store-keyed HMAC tokens. Piece ids bind the complete staging
binding, aligned offset, length and image digest. Progress ids bind the complete
binding, record kind, position and checkpoint sequence. Header position is 0,
page position is its window index, checkpoint position is 1 for an update delta
and 0 for a frozen finalization snapshot. Delta and frozen snapshot at the same
sequence therefore have distinct identities. Use `ArchiveStagingAddress` rather
than constructing these positions. Raw message-body/attachment hashes and
plaintext coverage belong inside encrypted records, never in visible ids.

Native storage supplies durable ownership, not a new SQL schema. Keep a bounded,
encrypted staging catalogue keyed to the job and binding token; persist only
core-admitted records. Exactly one binding may be created per job using atomic
ownership creation. Header and snapshot restoration must use
`OpenArchiveStagingRecord` with the catalogue's expected address; the decoded
value is not trusted before authentication. Enforce the codec's 64 KiB record,
128-range and 46-sibling limits before buffering untrusted native input. A page
contains one fixed-window image reference and optional coverage bitmap.
The native page/node index can span the attachment on disk, but enumerate or
lookup bounded pages instead of loading it wholesale. Membership and absence
proofs are checked against the authenticated header, so an untrusted index is
not an alternate authority. Rebuild it from authenticated checkpoint deltas
without accumulating a whole-file in-memory manifest.

For each Ready piece plan, the required write order is:

1. Record ownership of the new immutable piece before or atomically with file
   creation, so a crash leaves an enumerable owned orphan. Seal with a fresh
   nonce, publish, authenticate the actual bytes and make them durable. If the
   same piece id already exists, validate its context, length and plaintext
   digest, then reuse it. Identical images may arise from newly observed zero
   bytes; retries must not replace an existing immutable frame.
2. Seal the new page, checkpoint delta and header. Atomically compare the
   expected old-header hash and commit all three, including the new root and
   piece reference. Durably commit the catalogue/node-cache updates needed for
   recovery. A node cache is rebuildable and need not be trusted.
3. Return both durability flags and the exact binding/piece/old/new hashes to
   `CompleteArchiveStagingPiece`. A planned or buffered write, rename without
   required durability, missing piece, or failed CAS is not an acknowledgement.

On recovery, an old header means an unpublished piece is an owned orphan;
a new header requires its exact page and piece to exist and authenticate.
Report missing/corrupted referenced data as a retention gap, never credit it
by trusting counters. Keep superseded pieces and history owned until bounded
cleanup establishes they are unreachable from all live/checkpoint readers.
Credit no bytes from a failed CAS. Persist the frozen snapshot before starting
the two-pass finalizer, and retain its exact reachable pages and pieces through
completion or abort. Do not mutate its root underneath an active session.

Each canonical publication requires a fresh
`CheckArchiveStagingPublication` immediately before native I/O. Serialize this
check and publication/ownership update with purge, account removal and job
changes, or recheck under the same native guard. Register provisional or
canonical orphans in owned accounting even if a fence changes after writing.
A pure planner cannot close a native time-of-check/time-of-use race itself.
Do not commit complete references before every canonical chunk is durable.
After a restart, rescan from the frozen snapshot. A canonical chunk already
published with another nonce is reusable only after the finalizer authenticates
its actual frame via `existingFrame` and its actual `frameHash`. Acknowledging a
new frame when different bytes are durable is invalid.

The finalizer returns an ordinary `ArchiveTransaction`, translated and committed
with the existing checked SQL protocol. Crashes between publication and SQL
commit leave owned, unreferenced chunks; crashes during SQL commit leave either
the staged job or its complete exact-version reference. A new head does not
cancel a historical job, and it never receives that job's completion. Purge,
revision/hash changes, removed versions and external epoch changes refuse at
every step. Use existing reader/GC pins for canonical shared blobs; staging
ownership and active-checkpoint readers are native obligations, not fabricated
SQL media references. Physical usage and logout removal include all provisional
images, encrypted progress, node caches and canonical orphans. Completion or
abandonment must not leave an unowned staging directory.

`test_archive_staging` covers sparse uint64 offsets, fragmented/out-of-order
coverage, codec/domain isolation, overlap refusal, every durability cut,
page/image corruption, all finalizer restart boundaries, exact historical
fences, 1 MiB boundaries and a generated 128 MiB stream with bounded buffers.
`test_archive_sqlite` also translates staging completion, rolls back at each SQL
cut, reuses the retained owned copy and checks shared-blob purge/GC behavior.

## SQL schema, privacy and checked execution

Schema 1 separates account metadata, message shells, versions, event receipts,
owned blobs, media refs/jobs, reader leases, purge epochs, maintenance and capture
gaps. The current unreleased store format is 2. Before admitting an existing
connection, read its format without narrowing integers and preflight it with
`CheckArchiveSqlVersion`. Then verify the recorded fingerprint and actual DDL
with `CheckArchiveSqlSchema`, as specified under Draft store admission below.
Old shapes and fingerprint drift refuse; there is no automatic migration.
Only genuinely new stores execute `InitializeArchiveSql`. Enable and read back
foreign keys, WAL and FULL synchronous mode on the owning connection. An ignored
PRAGMA is not evidence of configuration. Serialize writers on a checked queue.

`ArchiveSqlSecurity::hmacSha256` uses the separate index key. Core domain-separates
account, store, index kind and length-framed identity, requires 32 output bytes
and emits lowercase hex tokens. Account, chat/peer, message, version, event,
blob, job, lease and immutable-media equality identities use these tokens in
SQL and envelope row ids. This includes raw collection ids, representations,
blob digests and event ids that could themselves contain peer ids. Lookup uses
`ArchiveSqlMessageId`, `ArchiveSqlChatId`, `ArchiveSqlChildId` and `ArchiveSqlToken`.
Never substitute an unkeyed digest or raw peer id.

Every public SQL plan and keyed read latches derivation failure across the entire
operation, including intermediate chat/alias tokens inside a scope. A missing,
failed or wrong-length HMAC refuses the whole operation with no executable
statements or external actions. Do not persist empty tokens or substitute a
fallback. Retrying with the healthy provider recomputes the same account/store
scope committed by Begin, so recovery never depends on a failed derivation.

What remains plaintext is explicit: schema/store/key ids, record/index kinds,
server message ids, transcript dates, status flags, insertion positions,
fixed-width causal order keys, generations/revisions, logical blob byte counts,
opaque HMAC equality links, maintenance phase/cursor and bounded generic gap
reason codes. Bodies, sources, original filenames, raw payloads, peer ids,
representations and extension fields remain encrypted. SQL pages/WAL and file
sizes can still reveal this listed metadata and access patterns. AEAD protects
records, not index completeness or whole-database rollback by a malicious
filesystem. Authenticate indexed results and verify their decoded identities;
inconsistent projections are corruption. No plaintext diagnostic binds or FTS.

Bind every value by its NULL/int64/UTF-8 TEXT/BLOB variant. Signed ids use SQLite
int64. Unsigned revisions/generations/byte counts use canonical decimal TEXT;
causal order uses a separate zero-padded 20-digit key for lexical uint64 order.
Never cast wide ids through floating point. Keep all result codes, including
prepare, bind, step, finalize, close, BEGIN and COMMIT. BUSY is a retry/failure,
never a successful empty result.

Every `ArchiveSqlStatement` carries explicit row and change ranges and a change
counter selection. `CheckArchiveSqlResult` requires reaching SQLITE_DONE and
those counts. Direct changes exclude FK cascades; total changes mean the
statement-local `sqlite3_total_changes64` difference, not its lifetime total.
Consume results incrementally and abort on the row/byte limit instead of buffering
an unbounded result. Non-DML uses total delta zero, avoiding stale `sqlite3_changes` from prior writes.
`OneRow` and `OneChange` require exactly one; bounded queries cap returned rows;
scope deletes allow explicitly unbounded direct changes without returning rows.
Any error or failed expectation rolls back the whole program, including a failed
COMMIT. Do not use Android's unchecked stock transaction/step helpers.

`AcknowledgeArchiveSqlCommit` requires both all checked results and an acknowledged
durable commit. Only then acknowledge a model transaction or perform its external
effects. On an uncertain commit, reload the durable header/journal and replay the
same stable event/job/operation id. Never invent a new id or continue after an
uncertain write. Filesystem publication and parent-directory syncing remain
separate client durability barriers.

## Bounded event and read slices

No production planner loads an account inventory. `TranslateArchiveSql` accepts
at most **one message and 256 logical rows**, 2048 statements and 16 MiB of binds.
It rejects oversized input before model validation. Supply a header plus the
exact current message head, any exact hydration/source target, the relevant
version-id/revision receipts and replay receipt, current scoped epoch, and
referenced blob/staged-job/lease facts. Read these in one SQLite snapshot using
the exact equality helpers. `RestoreArchiveSqlSlice` is bounded too; isolated
row decoding is only wire-shape validation, not reference authorization.

Use the normal pure event/media/lease reducer on that closed slice. SQL then
checks account/store/revision/generations and the exact old head/order. It
upserts only changed loaded versions/receipts and their media refs; it never
erases unloaded history. Duplicate event-id fingerprints must agree. Shared
media completion fills only the exact staged version; never gather
every account reference into one model. A missing head/replay/dependency fact
is an incomplete read, not permission to treat the account as empty.

Existing whole-state purge/GC reducers remain logical reference models for tests;
production uses the maintenance APIs below. Their plans are refused by the
normal translator when they remove versions/change generations. Nothing exposes
a mandatory whole-account load. `ArchiveSqlReadPage` scans at most 128 rows and
must be processed/discarded before the next page. Never accumulate all pages.
Independent encrypted version rows preserve source provenance and unknown fields
without imposing the whole-snapshot codec's 4 MiB limit on an account.

Head/history/exact-version queries and the dedicated deleted transcript share
identities. Transcript pages include deleted, non-trash-purged rows of the typed
chat, ordered descending by `(date, messageId, messageToken)`. The date is set on
first insertion from the first observed body, or deletion time for a bodyless
tombstone. Preserve all three cursor fields and a stable read snapshot for
consistent multi-page browsing. Limits are 1 to 128. Ordinary reads/writes are
gated while maintenance is active; its own head helper requires the journal id.

Snapshot wrapper extensions stay outside the schema-1 version object.
`RestoreArchiveSqlSlice` returns `ArchiveSqlSlice`: the original `ArchiveState`
and a separate `versionMetadata` sidecar keyed by message/version. Keep both
while applying core plans. Use `TranslateArchiveSql(plan, restored, security)` for
rewrites; after a durable commit, advance the slice's state while retaining its
sidecar, or reload the slice. The two-argument translator serves model-only
inputs without standalone wrapper metadata. `ArchiveSqlVersionSnapshot` accepts
the sidecar when reconstructing an individual snapshot. Purge works directly
with authenticated row snapshots and keeps their wrapper extensions intact.
No name is reserved in the schema-1 version's open extension space: old object,
scalar, array and null values remain ordinary opaque fields. Existing version
codec/model behavior and canonical bytes are unchanged.

## Capture admission and chunked version bodies

Clients capture through `PlanArchiveSqlCapture` (the `ArchiveSqlSlice` overload
for a restored slice with its metadata sidecar), check both the model outcome
and SQL program, and acknowledge only a durable checked commit. `NoChange` has
an empty ready program and requires no new write. Any retention refusal has
`ArchiveSqlStatus::RetentionGap`, a nonempty `retentionGapReason`, and no
executable capture SQL. Show the gap immediately and durably record it using
`PlanArchiveCaptureGap` when storage is available. Never claim the body was
retained. Crypto, derivation, encoding and transaction failures also require a
visible capture failure; do not silently drop them.

The shared version budget is at most 128 attachment refs, a 4 MiB **encoded
snapshot** body, and a compact authenticated envelope below 64 KiB.
Capture reserves maximum media-ref growth and full-width header counters so
reconciliation and revision/generation bumps stay within the same bound. This initial
storage contract intentionally keeps the existing schema-1 codec's 4 MiB limit.
Base64 and metadata count toward that limit, so it is not a 4 MiB source-file
allowance. A 10 MiB source is visibly refused before capture, not admitted and
later stranded. The 50 KB and 1 MiB genuine-source fixtures are admitted and
remain byte-exact. Large keys or identity metadata that cannot fit the compact
projection likewise refuse at capture. There is no whole-account body-byte cap.

Snapshots over 16 KiB are stored as archive-owned encrypted chunked blobs in
SQLite. `body_refs` binds one version to its keyed blob token, byte count and
chunk count. `body_chunks` stores at most four 1 MiB plaintext chunks, using the
existing `ArchiveMediaChunkContext`/size contract and AEAD framing. These are
owned retained bytes, not paths to Telegram cache files. Foreign keys cascade
version removal through its body reference to its chunks in the same checked
transaction. This reuses blob-reference ownership and chunk authentication while
avoiding external-file publication/GC for version payloads. Media's existing
owned-file and lease/GC contract is unchanged.

A distinct `purple-archive-body-row` storage format contains the immutable body
hash/length plus a compact authenticated projection of version identity, ordering
and attachment identity/kind/state/owner relations. It reserves no names in the
schema-1 version's open extension fields. Bodies include full source documents,
long text, captions, rich structures, observation payloads and unknown fields.
Capture atomically publishes chunks, the compact version and reference. A failed
statement, BUSY or non-durable commit acknowledges none of them.

For a normal read, decode the row, fetch `ArchiveSqlReadBodyChunks` for that one
version, then call `RestoreArchiveSqlVersionBody` before restoring a model slice.
The helper checks exact chunk count/size, authenticated account/store/blob/index
and length, the whole-body hash, and agreement with the authenticated projection.
Missing, swapped or corrupted evidence refuses. Unhydrated projections cannot be
passed to `RestoreArchiveSqlSlice`. Full body and unknown metadata survive
byte-exactly; only authenticated media state/blob updates are overlaid.

Maintenance reads the compact projection without loading, rebinding or rewriting
body chunks. It retains the immutable body reference when reconciling media.
Trashbin deletion removes all body refs/chunks for deleted versions; Edit History
keeps the head body's chunks and deletes earlier ones. Sizes affect bounded read
work for a single item, not purge plan size or feasibility for another scope.
The extra plaintext columns are opaque keyed body tokens, body byte/chunk counts
and chunk ordinal; actual body hashes remain inside authenticated envelopes.
Physical SQLite usage includes these bytes; logout's database removal includes
them automatically. The store format is an unreleased format 2 contract, with no migration
over an already released storage schema.

## Resumable purge, GC and usage

`BeginArchiveSqlPurge` accepts only a header, exact confirmation revision and
scoped chat epoch facts (primary chat plus explicit migration aliases, at most
32 chats). Its constant-size transaction writes an encrypted journal, advances
the account revision, bumps account or scoped chat generations and closes the
normal-access gate. Before any epoch, intent or gate write, the same SQLite
transaction proves admission for the selected chat/aliases, or the account when
explicitly requested: version/message/job/lease envelopes are at most 64 KiB and
versions have at most 128 media references. Unrelated chats never participate in
this feasibility scan. Capture enforces the same bounds before producing any
executable SQL. Blob metadata is limited to 4 KiB at capture. A failed admission
expectation requires rollback, with the old revision/generations/gate untouched.
These checks detect invalid storage; an accepted capture cannot exceed them.
The checks stream inside SQLite without materializing account rows. Revoke
generation-stale delivery immediately. No account
rows or histories are materialized. Other accounts are independent.

`ArchiveSqlPurgePage` returns at most 128 version/message/job/lease envelopes.
Authenticate each expected row id/kind, retain its exact original frame (and
exact head frame for Edit History), and pass exactly that page to
`ContinueArchiveSqlPurge`. It checks sealed intent/scope, each exact current frame and candidate membership,
and cursor coverage inside the transaction,
then updates rows and durably advances the encrypted journal cursor atomically.
All original target, owner and blob evidence for the selected prefix is checked
before any page write. Evidence is cached by scoped owner version and exact
frame, and by blob identity/frame; contradictory frames refuse. Each owner's
whole projection is checked once with at most 515 binds in one projection query,
plus its count/frame guards. Each attachment still checks its authenticated
identity, kind, position, Complete state and blob relationship in core.

Continuation tries the page, then smaller leading prefixes if the statement or
bind budget requires it. `processedRows` and the returned cursor identify the
committed prefix; suffix rows are read again later. No suffix job/reader actions
are released. An admitted single unit always fits the unchanged 2048-statement
and 16-MiB bind budgets: at most 128 distinct copy owners, 128 refs per owner,
128 blob records, and one target. An own rewrite can grow a version by at most
64 bytes per ref. Capture reserves that space in compact projections, keeping
evidence frames below 64 KiB even after reconciliation.
The worst unit stays below 800 statements and 14 MiB of binds. These bounds,
preflight admission and prefix fallback prevent an accepted purge from wedging
on a unit that cannot fit. They do not weaken frame or projection checks.
An empty page advances a phase only after SQL proves exhaustion. Replaying a
stale cursor fails without changes. Reopen by authenticating and decoding the
journal; compare its operation/phase/cursor with SQL metadata before resuming.
No normal archive access resumes before the final journal/gate clear.

The first phase reconciles **all surviving incomplete version refs before any
message history is deleted**. This preserves copies whose only previous owner
is an older version or a different deleted message. Skipping this ordering can
strand a live attachment even though the owned blob still exists. Query and
supply authenticated owned-copy facts for those versions too; absence is checked
in SQL. Only then does the message-deletion/truncation phase run.

Trashbin removes every version/ref of scoped deleted messages and keeps encrypted
non-content tombstones/receipts. Edit History retains only the exact head.
For deterministic reuse, owned-copy selection follows message insertion order,
version causal order and media position, as in the reference reducer. Before
dropping old versions, query `ArchiveSqlFindOwnedMedia` for each incomplete
head attachment. The lookup returns the blob token/frame, scoped owning-version
id/frame, and media position. Supply all five columns in `ArchiveOwnedMediaCopy`,
along with the requested identity/kind and decoded blob id. Core authenticates
both frames, recomputes their scoped ids, and checks the owner's descriptor kind,
attachment identity, exact reference position, Complete state and blob relation.
The transaction checks both original frames and the owner's complete media index
projection before reuse, once per owner version per page. It also checks the retained target's original projection,
including Complete references, before sealing a replacement. A substituted index
hint, missing proof, wrong kind/owner/blob or stale frame refuses the operation.
The transaction verifies the copy or its authoritative absence before deleting
history. Retained incomplete refs reuse owned copies; remaining staged refs become Missing,
scoped jobs cancel and readers
revoke in bounded later phases. Revoked leases keep pinning bytes until release.
The SQL may delete a whole message's old versions without returning them; the
generation fence/journal makes interrupted phases safe to resume.

`ArchiveSqlGarbagePage` scans at most 128 unreferenced, unleased blobs.
`PlanArchiveSqlGarbage` durably marks candidates GarbagePending, rechecking
references, leases and metadata in SQL. Only after commit may owned-file deletion
run. `CompleteArchiveSqlGarbage` removes only acknowledged pending metadata,
again checking pins. Media identity tokens never release a GC pin: every
`media_refs.blob_id` and reader lease pins its blob, even when a media lookup hint
is corrupt. GC makes no positive attachment association from a lookup token.
As with other negative lookups, adversarial removal of index entries remains the
explicit index-completeness exclusion above. Missing files are idempotent success; pending blobs cannot
receive new refs or be revived by a bounded completion whose slice omitted them.
Serialize owned publication with GC on the store queue, check pending identity
before publication, and never overwrite an existing owned blob path. Keep new
bytes staged until a pending older copy is deleted/reconciled. Each GC chunk advances a guarded header revision and returns
a cursor; restart scans pending candidates again, including interrupted deletes.

Usage measures actual owned DB/WAL/SHM/blob/staging file bytes, separately from
logical retained bytes or multiplied reference counts. `ContinueArchiveUsage`
consumes at most 128 monotonically ordered iterator entries and a resumable
cursor, checks store epoch/overflow, and retains only the running sum. Use bounded
filesystem traversal, never an all-path list. Restart an invalidated directory
snapshot; usage during concurrent writes is a display estimate. `TotalArchiveUsage`
checks summation overflow. Logout likewise uses bounded directory traversal.

Stable gap ids record capture failure through `PlanArchiveCaptureGap`; same-id
same-content retries are harmless, changed content conflicts. Gap details and
fingerprints are encrypted/keyed; generic reason/time remain visible.
`ArchiveSqlReadCaptureGaps` is bounded. If SQL itself cannot save a gap, report it
immediately and use an account-scoped external retry/outbox. Never claim retention
success after failure. There is no automatic age-based cleanup.

## Logout removal

`BeginArchiveRemoval` bumps the external work generation. Stop new work, then:

1. Durably publish its account/store/operation tombstone outside the archive,
   including parent-directory synchronization, before acknowledging logout intent.
2. Durably fence the external generation across processes. Queued work checks
   account/store/work epoch again before publication; model work also checks its
   independent account/chat purge generations.
3. Drain readers and writers, stop delivery, finalize statements and check close.
   Acknowledgement requires zero readers/writers and a closed database.
4. Remove this store's DB, WAL, SHM, rollback journal, owned blob and staging
   directories, wrapped data key and account-specific Android Keystore alias.
   Then remove the empty owned directory and its binding entry. Directory walks
   are bounded/resumable; never enumerate all files into a plan.
5. Durably clear the tombstone after all artifacts are durably absent. Retain
   the external generation fence. Re-login can then create a fresh empty store.

`AdvanceArchiveRemoval` checks every acknowledgement's exact account/store/
operation/generation/action. Failure or non-durability waits. Only drain uses
live handle facts instead of a durability acknowledgement. Missing artifacts are
idempotent success. Do not delete another account, shared platform wrapping keys,
Telegram's live store/cache, or ordinary media. At startup, pending tombstones
resume at the fence step before opening or key creation. Unknown/malformed intent
blocks opening; key loss does not block explicit removal. App quit and account
switching close the store without invoking logout removal.

Deletion is local cleanup, not forensic erasure of SSDs, snapshots or backups.
Desktop adapters must establish macOS backup exclusions and profile-safe roots;
Android must exclude archive/intent/catalogue/key roots from both cloud backup
and device transfer. These are client integration and platform test duties.

## Checks and integration

`tests/run.sh` runs the pure contracts and all existing suites.
`ARCHIVE_SQLITE_TESTS=1 tests/run.sh` additionally executes system SQLite against
temporary stores: exact statements/counts, rollback cuts, BUSY, commit failure,
disk-full, source bindings, 64-bit ids, paging, purge/readers/GC, gap retries and
a streaming 100,000-message account with bounded purge plans. Pure tests cover
100,000 usage entries, logout crash barriers, key loss/API gates and frame
truncation/tampering. Actual production AEAD/HKDF/HMAC, filesystem durability,
Keystore, profile binding, backups and native UI remain client duties.

Register `purple_archive_envelope.cpp`, `purple_archive_sql.cpp`,
`purple_archive_sql_body.cpp`, `purple_archive_sql_maintenance.cpp` and `purple_archive_lifecycle.cpp` in each
app's native build together with the existing archive codec/model/store/source.
The codec includes the new lossless row API; no production SQLite linkage is
introduced by core. Desktop's adapter should own its private SQLite target;
Android's checked JNI shim should use its existing engine and dedicated queue.

## Scoped deleted transcript and Settings queries

`purple_archive_views.*` adds `ArchiveSqlDeletedView`, `ArchiveSqlScopeCounts`
and `ArchiveSqlScopeBlobs`. They use the same account/store/security binding and
maintenance gate. No query materializes the account in application memory.
The unreleased SQL store format 2 projects keyed parent/topic and representation
tokens plus the collection enum onto messages. Topic tokens include the actual
parent chat; no raw topic/peer id or representation text is added to SQL. An
unknown-topic bodyless tombstone has no topic token. The original deleted-query
API remains available. Format 1 stores are refused; no automatic migration or ALTER is provided.
Initialize only a genuinely new store. Draft data must remain untouched on
refusal; recreation requires a separate explicit data-discard decision.

Deleted-view scope is one parent plus at most 31 explicit migration aliases,
with optional actual-parent topic, collection and representation filters. Set
Received/original for the ordinary transcript; absent filters include every
archived namespace. Pages return message token, stable transcript date, message
id, encrypted shell, head-version token and optional encrypted head snapshot.
A bodyless tombstone has no head. A nonempty head token with a missing frame or
missing body chunks is corruption/a retention gap, never a bodyless fallback.
The shell carries the observed deletion cause;
the head retains sender/reply context and full renderable content. Restore body
chunks before rendering a projected version. Unavailable/partial attachments
remain labelled; no query downloads or revives newer content into old snapshots.

Both chronological and reverse chronological traversal use the strict tuple
(date, message id, message token). Preserve all cursor fields and one SQLite
read snapshot across pages. Date is fixed at first insertion; when no server
message time was observed, it falls back to first-observed time, or observed
delete time for a bodyless tombstone. A later edit does not move the row.
Limits stay 1..128. Authenticate shell/head frames under the returned expected
ids, verify full decoded keys and run `ArchiveDeletedViewMatches` before display.
A contradictory positive index result is corruption, not permission to display
another scope. Index completeness/order and aggregate estimates retain the
existing malicious-filesystem exclusions; never use a count as purge authority.

ScopeCounts returns one row: tracked non-purged message identities, deleted
identities (including expired/consumed), retained versions, prior versions,
attachment references, complete references and distinct referenced blobs.
The guarded account/store/maintenance metadata row must exist; a failed OneRow
expectation is not a valid zero count. Counts are scoped index display data.
A bodyless observed tombstone counts as a deleted identity but has no version.

ScopeBlobs pages unique referenced blob tokens, canonical decimal byte counts
and encrypted blob frames. Authenticate/decode each blob and verify the indexed
size and Complete state against it, then sum its verified uint64 size once with overflow checking
(for example `ContinueArchiveUsage` over ordered tokens). Do not SUM/CAST the
wide decimal strings in SQL, multiply bytes by reference count, or count a
shared current/trash blob twice. These are logical retained bytes. Account
physical usage still uses existing DB/WAL/SHM/blob/staging scans and includes
leased/GC-pending/unreferenced overhead; it is a display estimate during writes,
not a promise of bytes released by a purge.

Build the confirmation token from the same account/header snapshot used for
counts. At confirm time reload the current header and external work epoch,
revalidate the actual dialog scope through `ConfirmArchivePurge`, and pass its
request to `BeginArchiveSqlPurge`. The existing SQL revision/generation guard
closes the race between confirmation and the first durable purge transaction.
A stale token, changed selected account, new store, changed aliases or logout
must reopen confirmation, never silently target current UI selection. Purges
still cover all topics/collections/representations of the named parent chats.

Media lifecycle capture and its existing stage/completion use the normal
bounded capture/translation path. `PlanArchiveScopedMediaCompletion` is the
external-epoch gate around the existing media reducer; it introduces no new
blob cache, ownership table or alternate purge/GC mechanism. The adapter still
owns actual byte copying, hashing, queue serialization, durability and capture
before upstream destruction. Add `purple_archive_media_lifecycle.cpp` and
`purple_archive_views.cpp` to client native source lists.

## Draft store admission and review fences

The SQL store format is 2 (the logical archive snapshot/row format remains 1).
`archive_meta.schema_fingerprint` records `ArchiveSqlExpectedSchemaFingerprint()`.
On every connection open, read the format number first and call
`CheckArchiveSqlVersion`; old positive formats return SchemaMismatch, future
formats return ReadOnly. Do not run capture, maintenance or content reads first.
For format 2, read the recorded fingerprint and all SQL strings returned by
`ArchiveSqlReadSchema` from sqlite_schema in the same read snapshot. Pass them
to `CheckArchiveSqlSchema`. Ready requires both the recorded fingerprint and
the normalized, sorted actual DDL fingerprint to match the current core. Missing
metadata or unreadable schema is refusal. Fingerprint drift is SchemaMismatch,
not an empty archive. Do not migrate, overwrite, add missing columns or initialize
an existing draft store. There is no released store to migrate. Version 2 also
fences the 9a7fa48 writer, whose version-1 mutation guards refuse this store.
Serialize schema changes and writers, and repeat admission on reopen. Mutation
and new scoped-view guards also check the recorded fingerprint; this does not
replace checking actual DDL on open. Existing token domains and encrypted row
formats are unchanged except that media job rows now require revision/hash.

A lifecycle event handed to `PlanArchiveSqlCapture` retains the captured external
epoch and exact head/media fence. Supply the fresh external epoch via its final
argument; a missing or changed epoch, changed head/revision/hash or purge epoch
returns a Stale transaction with no executable program. The SQL compare guard
still protects changes between planning and commit. A staged media completion
may fill a retained historical version after a newer head exists, but only its
captured version/revision/hash under unchanged purge and external epochs. It
does not populate another version or cancel its independent staging job.
