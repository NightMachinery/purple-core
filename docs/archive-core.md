# Received-content archive core

The archive is a separate, local-only contract. `purple_archive_model.*`,
`purple_archive_store.*`, `purple_archive_codec.*` and `purple_archive_source.*`
contain pure values, validation, reducers and transaction plans. They use Qt Core
and the standard library. They do no SQL, file access, TL decoding, networking, key management,
randomness, downloading or live-history mutation. Archive schema 1 has its own
version line; settings and sync-state versions are unchanged.

## Identities and observed content

An `ArchiveState` belongs to exactly one validated `AccountKey` (production or
test environment plus positive 64-bit user id). Every message, media job and
reader lease repeats that account; mismatches are refused. Account slots and
currently selected accounts are never identities.

`ArchiveMessageKey` includes typed chat namespace (`User`, `Group`, `Channel`,
`Secret`), positive 64-bit chat id, message id, collection and representation.
Cloud message ids are positive server ids. Secret adapters supply their stable,
nonzero signed 64-bit secret-message/random id, never a transient cache-row id.
Received, scheduled and quick-reply collections stay separate; quick replies
also require their positive stable shortcut `collectionId` (zero for the other
collections), so different shortcuts cannot collide. These values do
not authorize a client to start capturing additional collections. A pending
send has no received server identity yet. After acknowledgement, capture under
the stable server key and put local/random/send aliases in observation context
using `SendMetadata`; changing that context never adds a body version. This
origin overlays supplied context/transport fields and preserves other sender,
reply, payload and observation metadata. Body observations supply a complete
observation value, rather than a metadata patch.

`original` and a token such as `translation:fa` are separate representations,
including in the key. A translated body can never replace the original's
source. Topic id is observation metadata under its actual parent chat, not a
new chat namespace. Group-to-channel migration aliases are explicit metadata;
original keys never collapse. Per-chat purges include aliases only when the
request explicitly names them. They cover every topic, collection and
representation of each named parent chat.

`ArchiveContent` contains stable text, caption, normalized entities, rich
structure, immutable media identities/descriptors and optional semantic opaque
blocks. Content hashes use canonical JSON and include unknown content fields.
Object member order is irrelevant; arrays retain their semantic order.
Adapters must normalize their structure consistently and encode all wide ids
and fractional quantities in structured payloads as strings. Put reactions,
views/read counts, refreshable file references, alternative qualities,
download progress, local id remaps and send state in `observation.transport`
or `context`, never in canonical content or stable media attributes. Raw TL
bytes belong in `observation.payload` with a client/layer format token; those
bytes are retained but do not decide whether an edit occurred. Semantic opaque
blocks in `content.opaque` do participate in comparison. An `Opaque` body must
carry semantic opaque bytes or retained rich structure; a raw transport payload
alone is refused, because replacing it on a refresh could discard an unknown
body without a comparable version. The client normalizer must distinguish
semantic content from transport metadata.

Completeness is explicit (`Partial`, `Complete`, `Opaque`). Context retains
sender/reply/album information through client-normalized structured values.
Source provenance is typed: actual compose Markdown or received Markdown-file
bytes. `ArchiveSourceMatches` checks a source within a complete version
whose canonical content hash, causal revision and representation match. A
source carries the accepted event id in `source.revision`, which must equal
`version.revision`; the same body seen again after intervening edits does not
make an earlier source eligible. The adapter must have verified actual
provenance; rendering ordinary received entities as Markdown
is not evidence of the author's source. Raw payloads and unknown blocks remain
available as fallback even when no complete rendering is possible.

## Genuine source for Copy as Markdown

`ArchiveSource::bytes` holds the actual outgoing compose Markdown captured before
normalization, or original `.md` document bytes. No source is represented by an
absent optional `source` field (or JSON null). Ordinary received text/entities,
serialized rich structures and the structural Markdown writer never constitute
source provenance. The core does not infer or reconstruct incoming Markdown.

The optional schema-1 `source.binding` records the full `ArchiveMessageKey`,
retained body-version id and optional nonzero signed 64-bit `localMessageId` and
`randomId`. The existing source hash, causal revision and representation bind
those exact bytes to the verified body. Aliases and all wide ids are canonical
decimal strings in JSON. Unknown source/binding/key/chat fields survive codecs.
Old records without source or binding remain valid; unbound legacy sources use
the structural writer until their full identity is verified. No schema bump is
needed: this adds identity metadata inside the already version-owned source,
and schema-1 purges already remove that whole payload with its body.

`ChooseCopySource(state, request)` returns `GenuineSource` with exact bytes only
for a complete full-body original whose retained key, version, causal revision,
canonical content hash and displayed representation all match the bound source.
The request also carries the capture generation. A matching hash alone never
revives source after an intervening edit: A to B to A uses only source verified
against the particular A revision requested. A selection, fragment, displayed
translation, absent/unbound/stale source or incomplete body uses
`StructuralWriter`. `partial` reports partial or opaque archived structures.
The low-level `ArchiveSourceMatches` predicate alone is insufficient for copy
eligibility because it does not check account/message ownership or generations.

`validTarget` is false for invalid, missing, purged, wrong-account or stale copy
requests. The client must abandon such work and must not write a clipboard from
its cached result. `CurrentMessage` additionally requires the live retained head;
`ArchivedVersion` explicitly allows an older retained version or a deleted
message's retained body. These are read-only views, not live Telegram messages.
The structural writer still owns full rendering and unsupported-block handling.

### Capture, acknowledgement and client duties

- Capture genuine bytes at the compose boundary before normalization. Keep them
  with that account's pending local/random send identity until acknowledgement;
  a pending send is not a received archive row. For a Markdown document, retain
  its actual original bytes rather than exporting the displayed entities.
- Verify the source against the complete normalized body that will be copied,
  including caption, entities, rich structures and semantic extensions. The
  adapter supplies this assertion; core cannot prove how bytes were obtained or
  parse arbitrary client Markdown. Never rewrite an old source's hash/revision
  merely to make it eligible after a changed server body or translation.
- After the server key and accepted causal revision are known, attach the bound
  source to a baseline, or use `PlanArchiveEvent` with `SendMetadata`, an exact
  `targetVersion`, `targetRevision`, `expectedContentHash` and current generation.
  This updates source/send metadata and a receipt, with no new body, edit event,
  causal revision, body order or first-observed time. A different existing source
  conflicts; an exact legacy source can gain its verified binding.
- Bind every copy request to the actual full-body identity, displayed
  representation, extent and current account state. Revalidate after asynchronous
  work and before a clipboard write, including the requesting view's lifetime
  and currently selected account. A stale UI context is not repaired by passing
  an old account's still-readable archive state.
- Store source bytes only with their account-owned version. Empty Edit History
  removes prior sources with prior versions and retains the latest body's source;
  Empty Trashbin removes every source of each purged deleted message. Both bump
  generations, so queued pre-purge copies and binding events are refused. A fresh
  request may still copy a retained latest body's source after an edit-history
  purge. Pending source buffers must follow the same account/session cleanup and
  cancellation boundaries in the adapter. Do not log source bodies or place
  temporary plaintext exports in an evictable/shared cache.

## Events, refreshes and edits

`PlanArchiveEvent` consumes an explicit `ArchiveOrigin`, stable event-id token,
causal order and capture generation. Event ids are unique within a message
representation and must survive restart. Causal order is an adapter-supplied
unsigned 64-bit total order for that message, shared across accepted server
updates and local destructive actions. It is not edit time or observation time.
The adapter must resolve platform update causality before calling core; do not
compare an unrelated local counter directly with channel pts. Distinct events
in the same second require distinct ids/orders.

- A first baseline records only the body actually observed. A first
  authoritative edit records one current body with `missingPreimage`, never an
  invented original. An unseen delete records a bodyless tombstone.
- Only a newer authoritative edit with changed canonical content appends an
  edit version. A to B to A therefore retains three bodies. Equal-content
  edits advance the order, causal revision and receipt without manufacturing a
  body version. A same-body baseline at a newer causal order also advances the
  revision conservatively, without asserting an edit. Either clears an earlier
  source unless a source for the new causal revision is supplied.
- An event-id replay with the same semantic fingerprint is `NoChange` even
  if local observation time or transport metadata changed. Reusing its id for
  different semantic input is `Conflict`. Stale unique events are `Stale`;
  they cannot regress the head or backfill speculative history. Equal causal
  orders for distinct edit/delete events are conflicts.
- An unchanged-body baseline can refresh metadata/completeness without adding
  a body. A changed-body baseline requires the exact expected head id, content
  hash, causal `targetRevision` and a newer causal order. It retains a new
  labelled baseline observation, preserving every earlier captured body and
  attachment. Its origin does not assert that an edit was observed. Readers
  must distinguish baseline observations from authoritative edits. An incomplete
  new baseline cannot erase a complete previous body. Same-revision enrichment
  uses hydration instead of pretending a new baseline was observed.
- Partial captures explicitly declare `missing.content` paths and
  `missing.media` descriptor identities. The reducer binds these permissions to
  the captured causal `revision`; `Partial` alone grants no permission to add
  content. The capture adapter must know which data belongs to that revision.
- A content slot is a vector of literal object keys in the canonical content
  object, such as `{ "rich" }`, `{ "rich", "article", "footer" }` or
  `{ "rich", "article", "paragraphs" }`. Dots and slashes inside keys have no
  special meaning. Unknown semantic extension keys use the same root object.
  Parent objects must exist; declare their parent slot when they do not.
  Duplicate/overlapping slots, array-index paths and media-root paths are
  rejected. Slots may cover an absent field or a declared incomplete subtree.
- Hydration requires an exact retained version id, causal `targetRevision` and
  current content hash. It may fill only a previously declared content slot or
  enrich a previously declared incomplete descriptor for an identity already in
  that version. New media identities are always refused, including identities
  that appeared in a later edit. Capture a known identity and kind at baseline
  even if its descriptor is only a stub. Bytes independently remain Missing,
  Staged, Complete or Failed in that version's media references.
- Outside declared slots every semantic value and field set stays exact. Within
  a slot, object fields and array tails may be added, but captured scalars,
  array order/multiplicity and every existing field must survive unchanged.
  Only the exact slot's null/empty-string placeholder may be filled. Empty
  strings or nulls nested inside an incomplete subtree remain captured values.
  Existing descriptor identity, kind and position remain exact; declared
  descriptor enrichment also preserves every known attribute/unknown field.
- A hydration event's `missing` contains only remaining declarations from the
  capture. It can retain or close slots, never add, widen or reopen them. A
  removed content slot must contain a nonempty value; an incomplete descriptor
  must contain metadata before closing its slot. A partially filled subtree
  keeps its slot. `Complete`/`Opaque` require no remaining declarations;
  attachment byte readiness remains independent. Closing all slots can leave
  an unclassified `Partial` body, but gives it no further enrichment permission.
  An equal-body later causal revision replaces the slot plan with its own
  capture declarations, never carries old permissions forward implicitly.
- Hydration cannot change a complete version or downgrade it. Clients must bind
  the response to the captured revision; newer content belongs in its own edit
  or baseline revision, even when its additions would fit an open slot. Slot
  declarations express trusted capture knowledge, not authentication of incoming
  Telegram responses. No missing slot is inferred from an absent rich body.
- Authoritative delete, local delete, expiry and consumption have distinct
  causes. Missing cache rows, history refreshes and cache eviction are never
  delete events. Deletion does not silently undelete an archive row later.

Receipts contain event ids and fingerprints, not old bodies. Each version keeps
immutable `firstObservedAt` separately from its latest observation metadata, so a refresh cannot erase when that body was first seen.
Version ids remain stable across hydration. Each version also keeps its latest
accepted edit or newer-baseline event id in `revision`. Hydration and send/id
metadata must name that revision as well as the body-version id, so even an equal-body edit
invalidates a stale request. Changed-body baselines keep earlier versions and
work bound to them; only an explicit purge releases their retained bodies. Tokens are bounded safe
ASCII identifiers; construct them from stable platform ids or caller-provided
secure entropy. Never reuse job or reader tokens for another operation.

## Transactions and the adapter boundary

A ready `ArchiveTransaction` contains `expected`, `next` and ordered abstract
`writes`: compare the account revision, put changed message/blob/job/lease
records, erase released lease/collected blob records, put changed generations,
and finally advance revision. Message writes carry their versions and media
references in `next`; a relational adapter must update its reference index in
the same transaction. No physical storage shape is assumed.

Execute only a `Ready` plan. Compare and write every logical operation atomically
in one account transaction. `CompleteArchiveTransaction(current, plan,
committed)` accepts an acknowledged durable commit only against the exact
expected logical state, returns unchanged state on a failed commit, and accepts
an already-applied result idempotently. A competing event/purge/read acquisition
makes the old completion stale. Do not turn `committed` true on an uncertain or
partial write. After restart, read the durable state and replay the original
stable event/job id instead of inventing a new one.

Capture commits precede destructive upstream mutation. The client owns the
write-ahead/retry path between its archive and Telegram's separate live store;
core does not promise a cross-database transaction. Archive failure must report
a retention gap, not success, and must not indefinitely block server updates.

`ArchiveState` is a complete logical account inventory for these reference
planners, not a page of cached messages. Preserve deterministic row ordering
when reconstructing it. Purge and garbage collection reference reducers especially need all scoped
records/references/leases. Production uses the bounded SQL slices, indexed
queries and maintenance plans in [the storage contract](archive-storage.md),
not a whole-account reconstruction.

## Media, readers and garbage collection

Media references distinguish missing, staged, complete and failed. Stage jobs
bind account, message, exact version, immutable media identity and capture
generation, captured revision and content hash, plus optional expected size/hash. At most one job stages a reference.
A failed job has a bounded reason token and can be retried with a new job id.
A verified owned copy is shared atomically across matching immutable media
identities/kinds in the account. New versions and explicit stage operations can reuse it. A completion changes
only its exact job version; other references and pending jobs are untouched. Purge plans
attach an already owned copy to a retained latest body before dropping its
prior owner, so emptying edit history cannot discard a current attachment.
A ready stage plan can therefore attach an existing copy without starting a
physical job. Complete references are kept; choosing/upgrading alternate
qualities remains the later media-lifecycle integration.
Duplicate stage/completion delivery is harmless; changed completion bytes,
digest or outcome are conflicts.

The adapter copies available bytes into archive-owned staging, checks source
identity, counts bytes and computes SHA-256. Complete only after full verified
bytes exist and can be atomically published. `verifiedHash` must equal the
lowercase digest blob id and declared expected hash; byte count must match any
expected size. An unknown-size zero-byte result is refused. A declared empty
file can be complete; missing/short/partial reads must be failed or partial,
never a fabricated successful empty file. Blob ids are account-local: no
cross-account deduplication. Core never returns cache paths, arbitrary filenames,
or a request to delete Telegram's ordinary media.

Publish immutable owned blobs before committing their references; reconcile an
interrupted publish as an unreferenced owned blob, never as a complete message
attachment. Persist staged jobs and retry/clean staging after restart. Hash and
size fields are assertions checked against each other by core, not a substitute
for the adapter actually hashing/reading/authenticating bytes.

A reader acquires and commits a lease before opening an owned blob. Its capture
generation must still match; stale requests cannot reopen retained content
after a purge. `revokeReaders` asks the adapter to stop delivery and close those
readers after commit. Revoked leases still pin their blob until release, so an
open handle is never deleted underneath a reader. On restart, the adapter may
release leases of sessions it has proved dead; it must not guess about another
active process.

`CountArchiveBlobReferences` derives version references and reader leases;
terminal job receipts do not pin content. Garbage collection first commits
`GarbagePending` for unreferenced owned blobs. Only after that successful
compare/commit may the adapter act on `deleteOwnedBlobs`. Pending blobs cannot
receive new references. Physical deletion is idempotent, including an already
missing owned file. `CompleteArchiveGarbageCollection` removes only successfully
removed pending ids. Crash before marking deletes nothing; crash after marking
can reissue the pending ids; crash after physical deletion can finish metadata
later. A live version/shared blob or any reader lease prevents collection.

## Provisional streaming-media staging

`purple_archive_staging.*` is an optional pure protocol for retaining observed
streaming bytes that arrive out of order. Complete-file retention continues to
use the existing media APIs. Register `purple_archive_staging.cpp` in each
client's core source list when adopting streaming retention; the shared runner
registers `test_archive_staging`. No SQLite tables, store-format changes, or
changes to existing completion APIs are needed.

An `ArchiveStagingBinding` contains the complete `ArchiveMediaSource`, staged
job id, immutable attachment byte count and optional expected whole-attachment
SHA-256. The source content hash identifies the **message body**, not attachment
bytes. The job must exist, still be Staged, and match the exact version,
revision, body hash, immutable media identity and purge generations. The external
account/store epoch must match too. The source may identify a retained historical
version after a newer head arrives. Completion fills only that exact historical
version. Purged, missing, rewritten or differently hashed versions refuse.

Fix total size before creating the durable staging header. An unknown-size
stream cannot yet use this protocol; an empty file needs an explicit zero-size
job. A job owns one durable binding. Never replace it with a different binding
or reset its progress under the same ownership entry. The keyed binding id
includes every fence, total size and expected hash. No raw body/attachment digest
is used as a visible envelope identity.

### Bounded coverage and checkpoint publication

Staging format 1 has Header, CoveragePage and Checkpoint records, encoded by
`EncodeArchiveStagingRecord` and admitted by `DecodeArchiveStagingRecord`.
Unknown versions, kinds or fields refuse. Unsigned values use canonical decimal
strings, retaining the complete uint64 range. Each record is at most 64 KiB;
range lists are capped at 128 entries. Format 1 uses exactly one image reference
per materialized page, a stricter bound than that cap.

The file is divided into fixed 256 KiB staging windows. A page references one
immutable encrypted window image; its range contains offset, exact window length
and image SHA-256. Partial pages carry an exact byte-coverage bitmap, at most
32 KiB before base64. Unseen image bytes are zero and do not count as retained
bytes. Complete pages omit the bitmap. This admits arbitrarily fragmented parts
without a growing range list. The cost is rewriting a bounded window image for
each new fragment. Adapters may batch adjacent observations before planning.
They must not discard observed bytes while waiting to batch.

Call `ArchiveStagingInputSize` to split each observation at window boundaries,
then `PlanArchiveStagingPiece` with the current authenticated header, old page
(or authenticated absence), membership proof and prior encrypted image. An input
larger than 1 MiB or crossing a window refuses; splitting is not a size cap on
the attachment. Checked uint64 addressing supports sparse offsets through the
maximum uint64 file length. Identical replay is NoChange; a disagreeing captured
byte is Conflict. Unknown bytes never overwrite captured bytes.

The header commits to all pages with a sparse Merkle root of fixed depth 46.
`ArchiveStagingEmptyProof` supplies the empty tree's siblings. Each membership
or absence proof has exactly 46 sibling hashes. Pages authenticate their keyed
binding id, index, update sequence, coverage and image reference. The header
records sequence, covered bytes and page count; a counter alone never proves
coverage. A native node index may live on disk and be untrusted: the supplied
proof must reproduce the authenticated root. `ArchiveStagingNodes` returns the
46 changed ancestors; the page hash is its leaf. Replay encrypted checkpoint
deltas to rebuild the cache, or scan pages using bounded native storage. Core
keeps no whole-file page, range or node vector in memory.

A Ready update is a plan, not retained bytes. Publish and authenticate its
immutable piece first, then atomically CAS the old header hash to the new
header while committing the new encrypted page and checkpoint. A checkpoint
delta binds old-header hash, new header, changed page hash and proof. CAS losers
must reload and replan; do not merge bitmap bytes independently. Only
`CompleteArchiveStagingPiece` with an exact durability acknowledgement credits
coverage. Both piece durability and atomic checkpoint durability are required;
acknowledgements bind binding id, piece id and both header hashes. Repeated
successful completion is AlreadyApplied. Lost pieces, rollback or an unpublished
page cannot be credited by reconstructing counts alone.

### Two-pass finalization

`FreezeArchiveStagingCheckpoint` requires every byte and page. Persist and reopen
that immutable encrypted snapshot before beginning finalization. Pass the
admitted snapshot and matching current durable header to
`BeginArchiveStagingFinalize`. The move-only session requests one authenticated
page/proof or piece at a time through `AdvanceArchiveStagingFinalize`:

1. In Verify, read pages and pieces in ascending offset order, authenticate every
   image and coverage proof, and compute exact size plus whole SHA-256. Compare
   the optional expected attachment hash. Holes, missing/forged/reordered pages,
   corrupted images and mismatches refuse without a canonical publication.
2. In Reseal, reread/authenticate the same immutable checkpoint. Rehash bytes and
   pack canonical 1 MiB chunks using the existing keyed-blob-token media context.
   Every individual plaintext buffer is at most 1 MiB; an image is at most
   256 KiB and progress is at most 64 KiB. Memory does not grow with file size.
   The final partial chunk has its exact size; a declared empty attachment has
   one authenticated empty canonical chunk.
3. For each Publish request, call `CheckArchiveStagingPublication` immediately
   before native publication, using fresh state, epoch and current header.
   Publish immutably and durably, then return the exact
   `ArchiveStagingPublishAck`. A false durability flag returns NotCommitted and
   the same pending frame. No subsequent chunk or completion is credited yet.
4. After all chunks are durable and the second whole-file hash matches, return
   the matching Finish request. Only then does core return the existing
   `PlanArchiveScopedMediaCompletion` transaction. Execute its checked SQL and
   existing durable commit protocol before reporting a retained attachment.

Each advance and publication check revalidates the full job/version/revision/
body-hash, external and purge fences. Request ids reject future/wrong callbacks
and coalesce old callbacks without advancing progress. Crypto failures expose
no publication/completion. The session is transient; restart reopens the frozen
checkpoint and rescans both passes. No private hash-library state is serialized.
Already durable canonical chunks can be reused after a crash or shared-blob
race: supply their actual encrypted frame and digest in `PublishAck.existingFrame`
and `frameHash`. Core authenticates the canonical context and exact plaintext
size/hash before accepting the durability claim. Never overwrite an immutable
chunk with a retry's different random-nonce frame.

Core does no I/O and cannot infer that an acknowledgement is truthful. Native
adapters own nonces, ownership manifests, fsync/CAS, encrypted node/page storage,
and bounded enumeration. All provisional pieces, superseded images, checkpoint
records and orphan canonical chunks remain archive-owned, count in physical
usage and participate in purge/logout reconciliation. A failed run never turns
an orphan into a complete reference, and ordinary Telegram cache files are
never owned or deleted by this protocol. See the
[storage contract](archive-storage.md#provisional-staging-storage-contract).

## Purges and generations

A purge names exactly one account and either one typed chat (plus explicit
migration aliases) or all chats of that account. `expectedRevision` binds the
confirmation to the inventory reviewed by the user. Any intervening change
requires a fresh plan; another account is invalid, never inferred.

Empty Trashbin clears all versions, source, rich/opaque payloads and media
references of deleted/expired/consumed messages in scope. It keeps a non-content
message tombstone, causal receipts and generations. Live messages stay intact.
Empty Edit History removes every prior version but retains each message's
latest body, including a deleted message's last retained body. Neither action
changes live Telegram history or performs cloud deletion.

Each purge bumps its account or explicit chat generations, including an empty
scope. Scoped staged jobs cancel and scoped readers revoke. Every queued event,
job and reader request carries the generation captured when work began; a late
pre-purge completion cannot refill the scope. A trash-purged tombstone also
refuses newly labelled body observations, so retrying with a fresh generation
does not resurrect discarded content. Retained live/latest content may start
fresh media jobs after an edit-history purge.

Unknown v1 extension fields are preserved as metadata; payload-bearing fields
belong inside versions/content, which purges remove. A future extension that
changes purge meaning must raise the archive schema and remain read-only in
older clients. Physical deletion and encrypted-store/WAL/backup guarantees
belong to the adapter; this pure contract does not claim forensic erasure.

## Codec and compatibility

`Encode/DecodeArchiveVersionSnapshot` is the durable identity-plus-version
codec. Each snapshot is bounded at 4 MiB and 128 JSON levels. Opaque bytes and
source/TL payloads use canonical base64. Every signed/unsigned 64-bit id, order,
size, account transaction revision and generation is a canonical decimal
string. Causal revisions and event ids are bounded tokens. Enums/schema use
small integer JSON numbers; persisted enum values are append-only and must
never be reordered. Duplicate decoded names, invalid UTF-8/Unicode,
noncanonical decimal strings, invalid base64 and inconsistent model references
are refused. The wire uses the strict integer JSON subset also used by sync;
fractions and out-of-safe-range numeric extension values are preserved only as
an opaque unsupported document, never rounded into a writable state.

Unknown fields survive at every object level. Known-field collisions in a
caller's `preserved` object are refused rather than silently overwritten.
Unknown enum values return `Unsupported`. Higher schemas, including versions
above the signed 32-bit range, return `NewerSchema` before narrowing. Both
outcomes return the exact original bytes and no writable typed value. Invalid
input also retains its exact opaque bytes. Preserve the only copy; do not
replace it with an empty archive or rewrite it through schema 1.

`Encode/DecodeArchiveSnapshot` additionally checkpoints a complete logical
account for fixtures/small stores, with the same 4 MiB frame limit. A larger
account is still a valid model and can use independent version records; failure
to fit a whole-account checkpoint is not an instruction to drop records.
Durable index/transaction metadata, authentication/encryption framing, key
ownership and recovery journals are adapter responsibilities. Authenticate and
bound untrusted physical input before decoding; core is not a cryptographic
storage envelope. No ordinary cache migration or historical reconstruction is
performed by this batch.

## Verification

`tests/run.sh` compiles all production archive modules and runs
`test_archive`, `test_archive_codec` and `test_archive_source`, alongside the
existing config, flow, sync-chat and carry-recovery suites. Regressions cover
causal replay/order, same-second edits,
A to B to A, observation noise, exact-version hydration, unseen events,
account/chat/secret/topic/migration/representation identities, full-width ids,
source eligibility and full-body/fragment/translation choices, local/random-id
binding without edits, legacy/unknown source fields, unknown/future data,
staging/failure/integrity, scoped purges, stale jobs/readers/commits, shared
blobs/leases and crash-retried GC.
Client storage, TL adapters, encryption, actual byte reads and native UI remain
future batches.

## Expiring-media lifecycle boundary

`purple_archive_media_lifecycle.*` accepts typed lifetime (ordinary, TTL,
view-once or secret), transition (expiry, consumption, expired replacement or
cache eviction), and source-byte availability facts. A source repeats the
account/store work epoch, full message key, current retained version, causal
revision, canonical content hash, immutable media identity and purge generation.
It never accepts a pointer, cache path or recycled file name as an identity.
Secret lifetime facts require the secret-chat namespace. Photo, video, voice
and round-video descriptors use the same ownership contract.

`PlanArchiveMediaLifecycle` requires that the pre-destruction body has already
been durably captured. It derives existing MediaExpiry/MediaConsumption events
only for a destructive transition of expiring media. An ordinary voice/round
read or non-destructive consumption is NoChange; cache eviction is always
NoChange and never creates deletion or expiry. A clear-media/service-placeholder
replacement is an expiry fact, not a new baseline that overwrites the captured
photo/video. Real authoritative/local deletion still uses its own existing
origin. Retained views can distinguish Deleted, Expired and Consumed without
inventing the actor or exact server deletion time.

The lifetime/destruction assertion comes from the client's real protocol hook;
core cannot infer it from a missing cache file. Capture actual lifetime/kind
metadata in the normalized descriptor/observed payload while it is available.
Do not classify ordinary media as expiring merely to discard a live message.

Before upstream TTL destruction, clearMediaAsExpired-style replacement or
view-once/secret/voice/round consumption: capture and durably acknowledge the
original body, bind the source, and independently copy bytes already fetched by
the existing auto-download/stream policy through the existing stage jobs. Then
commit the lifecycle event before allowing upstream destructive mutation. There
is no second cache and no extra download request. `captureBeforeDestruction`
expresses that ordering barrier, not an acknowledgement of storage success.
The client owns the write-ahead bridge and should report a retention gap if a
capture/copy cannot complete; server updates must not wait indefinitely.

`retention` is Owned only for an existing complete archive reference. Complete
source bytes with no owned reference are NeedsCopy; short/streaming bytes are
Partial, absent bytes are Unavailable. None is a successful empty attachment.
Only existing verified stage/completion commits can make a reference Complete.
Source expiry does not erase the retained body or an owned blob. One account's
same immutable blob can serve current and trashed versions; purging trash must
leave the current reference intact.

`PlanArchiveScopedMediaCompletion` checks the captured external account/store
work epoch before delegating to existing job/version/identity/purge-generation
checks. Persist that captured epoch with asynchronous work and recheck it before
publication and completion. Logout, re-login into a new store, account changes
and purges cannot be repaired by supplying a newer epoch to old work. Completion
may still attach to a retained historical or expired version, even after a newer
version was captured. This is an explicit Edit History retention rule. Staging
persists the exact version id, revision and canonical content hash in the job.
Completion requires that same version, revision, hash and unchanged purge epoch;
it never fills a different current version. `ArchiveMediaRefusal` distinguishes
store/purge epoch, missing version, revision, content hash and media mismatches.
A revision-only update also fences a pending job. Body/revision changes cancel
its obsolete staged work, leaving the reference available for a freshly bound job. Job row/snapshot codecs now
persist the revision and hash; unbound draft jobs are not admitted.

A returned lifecycle event carries its external epoch, media identity and the
version/revision/hash fence. `PlanArchiveEvent` checks the current head before
applying it. For `PlanArchiveSqlCapture`, pass the freshly loaded external epoch
as `currentEpoch`; omission or any account/store/work-epoch mismatch returns
Stale. The fresh slice supplies the purge epochs and current head. Never replace
the captured event epoch with the fresh epoch.

The unreleased SQLite store format is now 2. Its recorded schema fingerprint
and actual DDL must match the current core before admitting a connection. Old
format 1 and draft shape drift refuse with SchemaMismatch; there is no automatic
migration. See archive-storage.md for the open protocol.

## Deleted views and confirmation scope

`ArchiveViewScope` names one account, optionally one parent chat plus explicit
migration aliases. Optional collection and representation filters avoid mixing
scheduled/quick-reply or translated rows with the ordinary received original
transcript. No filter means all archived identities, not a deduplicated live
message count. A topic filter includes its actual typed parent chat as well as
its id; equal numeric topics in migration aliases are never inferred equivalent.
Unknown-topic bodyless tombstones appear in the parent view, not a guessed topic.

`ArchiveDeletedViewMatches` checks the decoded shell and optional head against
that scope. Authenticate row frames and verify their expected row identities
first; a positive lookup hint alone cannot admit a foreign account, chat, topic,
representation or collection. Check the head's full key and revision-owned
payload before rendering, and restore chunked bodies through the storage API.
Keep sender/reply context and completeness from the retained observation.
The deleted view is read-only: never insert snapshots into live history,
notifications, unread counts, dialog-top-message, search or forwarding state.

A confirmation uses `MakeArchivePurgeConfirmation` and `ConfirmArchivePurge`.
It binds account, store/work epoch, revision, account purge generation, purge
kind and normalized explicit parent/alias scope. Revalidate the current account,
selected kind/scope, store and header at confirm time and again on the executor
queue; a token is a deterministic stale-context guard, not an authentication
credential. Any intervening write invalidates it conservatively. An account-wide
confirmation must explicitly name that account and all chats. Parent-chat purges
still cover all topics, collections and representations of every named parent;
a topic-filtered view never silently narrows or widens the confirmed purge.
Use the returned existing request with the existing atomic purge planner.
