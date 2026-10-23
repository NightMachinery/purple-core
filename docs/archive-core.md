# Received-content archive core

The archive is a separate, local-only contract. `purple_archive_model.*`,
`purple_archive_store.*` and `purple_archive_codec.*` contain pure values,
validation, reducers and transaction plans. They use Qt Core and the standard
library. They do no SQL, file access, TL decoding, networking, key management,
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
bytes. `ArchiveSourceMatches` accepts a source only for a complete version
whose canonical content hash, causal revision and representation match. A
source carries the accepted event id in `source.revision`, which must equal
`version.revision`; the same body seen again after intervening edits does not
make an earlier source eligible. The adapter must have verified actual
provenance; rendering ordinary received entities as Markdown
is not evidence of the author's source. Raw payloads and unknown blocks remain
available as fallback even when no complete rendering is possible.

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
when reconstructing it. Purge and garbage collection especially need all
scoped records/references/leases. The store can map operations to indexed rows;
this batch does not implement an indexed/paged engine or benchmark its scale.

## Media, readers and garbage collection

Media references distinguish missing, staged, complete and failed. Stage jobs
bind account, message, exact version, immutable media identity and capture
generation, plus optional expected size/hash. At most one job stages a reference.
A failed job has a bounded reason token and can be retried with a new job id.
A verified owned copy is shared atomically across matching immutable media
identities/kinds in the account. New versions reuse it, and completion can
satisfy other pending references and cancel their duplicate jobs. Purge plans
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
`test_archive` and `test_archive_codec`, alongside the existing config, flow
and sync-chat suites. Regressions cover causal replay/order, same-second edits,
A to B to A, observation noise, exact-version hydration, unseen events,
account/chat/secret/topic/migration/representation identities, full-width ids,
source eligibility, unknown/future data, staging/failure/integrity, scoped
purges, stale jobs/readers/commits, shared blobs/leases and crash-retried GC.
Client storage, TL adapters, encryption, actual byte reads and native UI remain
future batches.
