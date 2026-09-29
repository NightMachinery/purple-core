# purple-core

The Work Mode core shared by the Purple Telegram apps: the `settings.toml`
parser, the splice writer that edits that file in place, the `state.toml`
serializer, the resolution engine that turns a preset into a decision about one
chat, and the screen-time log with everything derived from it.

It is a repository of its own because two apps need exactly this code and
nothing around it. Every policy in the Work Mode spec is a rule about data, and
rules about data are far easier to prove outside a running app than inside one.

## What is here

- `purple/purple_settings.{h,cpp}` - parses `settings.toml` into presets, lists,
  folders and rules, collecting warnings as it goes.
- `purple/purple_splice.{h,cpp}` - the only writes the apps ever make to
  `settings.toml`. It does not re-serialise the document: it locates the array
  it must touch through toml++'s source regions and edits the raw lines, so
  every byte the user wrote stays where they put it, comments included. A key
  it has to add joins the column the block it lands in already settled on -
  read off the file, never recomputed - so the new line reads as part of that
  block and the lines above it are not re-padded to suit it. A key too long for
  that column, or a block whose own lines agree on no column, gets the one
  space instead.
- `purple/purple_state.{h,cpp}` - reads and writes `state.toml`, the
  machine-owned half of the configuration. Rewritten whenever it changes, which
  is why it is a separate file: it must never touch the mtime of the
  `settings.toml` you are editing by hand.
- `purple/purple_config_sync.{h,cpp}` - constructs content fingerprints with
  generation keys and bounded ancestry for account-backed config versions,
  then classifies remote heads as stale, same, ahead, concurrent, or unrelated.
  It reports a split when distinct maximal remote contents all differ from the
  local file. The caller handles any resulting state change and commits each
  head's sequence watermark only after its outcome is durably handled. When a
  lineage is truncated, both direct parents stay, newest first, followed by
  the newest remaining ancestors.
  `PlanConfigSync` turns the local fingerprint, config state and remote heads
  into one verdict (Invalid, Pending, Conflict, Choose, UpdateReady, Adopt,
  Empty, LocalChanges or UpToDate) with the heads to offer and the heads that
  already match the file. A device that has not joined plans with an empty
  install and empty config data, so joining and daily use share one decision.
  `AdoptConfigHeads` records heads whose content equals the local file,
  replacing the base when its content differs and otherwise growing the
  equivalent keys, and raises the seen sequences. `PlanConfigChoice` maps
  the user's choice for a verdict (apply the update, pick a remote version or
  keep this device's text) to the file write, the heads to adopt and the
  parents of any version to publish. See
  [`docs/config-sync.md`](docs/config-sync.md) for the precedence, the head
  ordering, the parent rules and every refusal.
- `purple/purple_sync_json.{h,cpp}` - validates and canonicalizes JSON for
  account-backed sync. It preserves every member, including unknown members,
  sorts object names by raw UTF-16 code units, emits compact UTF-8 with RFC 8785
  string escaping, and rejects duplicate decoded names and invalid Unicode.
  Numbers must have an exact mathematical value in the safe integer range
  `[-9007199254740991, 9007199254740991]`. Valid integer spellings such as
  `1.0`, `1e0`, and `-0` are accepted and emitted as `1`, `1`, and `0`. Fractions
  and larger values are rejected before floating-point rounding. Inputs are
  limited to 4 MiB and 128 levels of nesting. The result includes an error
  kind and byte offset on failure; callers must check it before hashing or
  interpreting the JSON.
- `purple/purple_sync_envelope.{h,cpp}` - parses and writes uncompressed sync
  records on top of that strict JSON format. It validates the version, stream,
  canonical 128-bit IDs, sequence, timestamp, writer metadata, and payload
  hash; it preserves unknown JSON members at every level. Its ID formatters
  encode exactly 16 caller-supplied entropy bytes as lowercase unpadded
  RFC 4648 base32 with `in-` or `sp-` prefixes; the core does not generate
  randomness. `FormatTimeOrderedSyncSpaceId` places a caller-supplied trusted
  server-time estimate in milliseconds before a caller-supplied secure random
  tail, while `FormatSyncSpaceId` keeps formatting any 16 bytes. Valid space
  IDs can be compared by decoded bytes with `CompareSyncSpaceIds`. Writing
  recomputes the payload hash and emits canonical JSON.
  Invalid records report a reason. Unknown stream names and unsupported
  encodings return separate non-valid outcomes with a typed, validated major-1
  header (space, stream, writer, sequence, timestamp); their payload is not
  exposed as a valid envelope. Such records must contain a payload and a
  lowercase SHA-256-shaped hash, but the parser does not interpret the payload
  or check its hash. Stream names are bounded safe ASCII identifiers, including
  future names such as `library.0`. A higher major remains opaque. Config
  records are capped at 256 KiB; library and unknown-stream records are capped
  at 4 MiB, including the full document bytes. The caller must
  separately verify the Telegram peer, document name, and forwarding metadata.
  A newer major that uses JSON numbers outside version 1's safe-integer subset
  is unreadable by this parser rather than receiving the higher-major outcome.
- `purple/purple_sync_directory.{h,cpp}` - resolves a caller-supplied inventory
  of original Saved Messages candidate documents without Telegram I/O. It
  groups records by space, stream and install, selects the highest sequence in
  each group, retains equal-sequence duplicates, and marks conflicting hashes
  as ambiguous. A supported head requires a validated stream payload and
  full-record SHA-256; a parser-valid library record remains opaque until a
  library adapter exists. The caller sets `payloadValidated` only after its
  stream adapter accepts the payload.
  Unsupported stream and encoding headers keep a space alive but yield no
  supported head. The oldest visible space is selected by decoded space ID.
  `publishableSpace` means only that space election is safe: it requires a
  complete inventory with no invalid, newer-major or colliding-message
  candidates, and no ambiguous group in the selected space. It does not
  authorize publishing into an opaque or unvalidated stream head. The caller
  must check that group's `supportedHead` (or that the group is absent) and
  satisfy the publish planner's other gates. `canCreateSpace` requires the
  same inventory checks and no visible space. The caller must exclude
  forwarded copies, or set `original` false, and provide the full-record hash
  of supported records after exact validation.
- `purple/purple_config_payload.{h,cpp}` - inspects a validated config envelope
  before its settings file can be used. It checks the key against the exact
  UTF-8 bytes represented by the JSON `text` string, validates direct parents
  and bounded lineage, parses TOML again, and compares the declared schema to
  the parsed version. It reports a newer schema separately from an invalid
  record and returns locally computed warnings alongside the writer's count.
  Arbitrary non-UTF-8 settings bytes cannot be represented by a JSON string.
  Without the parent records, inspection cannot prove that lineage is the
  complete union of their histories; it verifies membership and structure.
  `BuildConfigRecord` creates the canonical config envelope from exact UTF-8
  settings bytes, full parent versions, writer metadata, a sequence and a
  timestamp. It rejects malformed UTF-8 and TOML, validates the complete
  record by inspecting its own output, and leaves newer settings schemas
  read-only for older clients. Size, identity, metadata and number failures
  carry the underlying envelope error. It does not reserve or persist a
  sequence. To acknowledge an applied remote record, pass its inspected
  `ConfigVersion` in `ConfigRecordBuildInput::version` with `parents` empty.
  The builder keeps the exact key, direct parents and lineage while changing
  the writer metadata and sequence. Supplying both modes, mismatched file
  bytes, or malformed ancestry fails without emitting a record.
- `purple/purple_sync_status.{h,cpp}` - derives the shared status, attention
  tier and primary action from engine facts. Off and manual pause take priority
  over active sync; the remaining statuses follow the UI precedence from choice
  through up to date. The engine decides when a waiting or failed change is due
  for a notice, and each client localizes labels and deduplicates notices.
  See `docs/sync-status.md` for the input and output contract.
- `purple/purple_sync_local_state.{h,cpp}` - validates and writes versioned,
  device-local `sync/state.json` data: install, creation-device and space IDs;
  an optional account binding token;
  per-stream issue, pending and confirmation counters and the hash for the
  highest issued payload; and config ancestry and per-writer seen sequences.
  Unknown JSON members survive rewrites. Reserving a publish returns a new
  state with a higher `seq` and `pending_seq`, while confirmation clears pending
  only after a matching own record is read back. Pure clone checks report a
  changed device ID, an observed own sequence ahead of local state, or a
  different hash at the same sequence. They require the caller to supply a
  validated own record or an authoritative absence result; unresolved discovery
  leaves the publish gate closed. `ConfirmConfigReadBack` also advances the
  config base and lineage to the validated staged version after exact read-back,
  clears its pending key, and retains equivalent keys only when content is
  unchanged. The platform must persist that returned state before deleting the
  staged record. Optional top-level `own_messages` holds at most 256 confirmed
  config record message IDs with their space, writer identity, sequence,
  payload hash and SHA-256 of the entire canonical envelope; old states without
  it load an empty ledger. Entries from an older space remain valid after a
  local space change. Unknown fields in each entry survive round trips.
  `RecordConfirmedOwnConfigMessage` records a
  validated own server read-back after confirmation, updating an ID after an
  in-place edit or tracking multiple IDs for a duplicate post. A message ID
  cannot be reassigned across spaces.
  `CheckOwnConfigMessageDeletion` allows deletion only when a fresh canonical
  read-back exactly matches a listed ID with a sequence strictly older than
  the confirmed sequence. Even duplicate posts at the current confirmed
  sequence remain protected until a newer version is confirmed.
  `RemoveAbsentOwnConfigMessage` removes that entry only after the caller
  reports authoritative absence for the same ID. The caller persists every
  returned state and performs all Telegram reads and deletes; core does no I/O.

  Each stream may also hold `issued_records`, an ordered log of at most 32
  sequence and canonical-record SHA-256 pairs. Older state files load an empty
  log; unknown members of its entries survive rewrites. After staging a
  reserved config record, call `AppendIssuedConfigRecord` and persist its
  returned state in the same state write as the reservation, before sending.
  It validates the complete canonical config record and its install, creating
  device, sequence and payload hash. A repeated identical append is harmless;
  the oldest pair is evicted when the log reaches 32 entries.
  `AdoptIssuedOwnConfigMessage` can add a discovered older config message to
  the ledger only when its exact record hash remains in this log, its writer
  matches this install and creating device, and its sequence is confirmed.
  Adoption never deletes a message. Retirement still requires a ledger entry
  and a fresh exact-record check through `CheckOwnConfigMessageDeletion`.
  An exact issued record from the install's previous space can also be adopted
  after a space move; an unissued record from any space cannot.
  Existing current-head recording remains valid for state files that predate
  the log. Library log data is parsed and preserved, but library issuance and
  adoption await library payload validation.

  `FormatSyncBindingToken` formats exactly 16 platform-supplied secure random
  bytes as 32 lowercase hex characters. The same token is stored in the
  chosen account's local preferences and in `state.json` as `binding_token`.
  An absent field loads as empty for older states, while a present empty or
  malformed field is invalid. Writing an empty token omits the field.
  `CheckSyncAccountBinding` compares a valid state with the account preference
  bytes and reports bound, missing, malformed, or mismatched outcomes. The
  future publisher must require `Bound` before publishing. The token stays
  unchanged across space moves and install-ID changes; neither it nor the
  device-local state stores a Telegram user ID.

  `PlanSyncPublish` is a pure next-action decision for one install and stream.
  The caller supplies a completed own-record discovery, current device and
  account binding, desired payload hash, local policy, and verified read-backs.
  `ready` means network work may proceed now; while false, the planner waits
  before reconciling, sending, or offering a retirement candidate, but still
  pauses on invalid local state and mismatched staged records.
  It pauses for an invalid binding, clone signal, missing own head, or missing
  staged record, and waits while discovery or another publish is in progress.
  A pending attempt that may have reached the server returns `Reconcile` before
  any resend. `ReserveAndStage` means to reserve a sequence, construct the
  canonical envelope, and persist its staged bytes and issued-record hash
  before asking the planner again. `Unsent` is only for a stage known never to
  have been submitted; after an uncertain response or restart the caller must
  use `MayHaveReachedServer` until an authoritative scan establishes absence.
  `Edit` requires an enabled policy and a fresh exact own-head read-back whose
  full-record hash matches the ledger. A refused edit leads to `Post` for that
  attempt. A confirmed own head absent from the ledger needs reconciliation
  before the next sequence is reserved. `RetireCandidate` names one older
  ledger ID, but does not authorize
  deletion. The caller must freshly fetch the exact canonical server record
  and pass it through the stream's deletion check before deleting. No action
  performs I/O or validates a
  playlist payload. The library stream additionally requires the caller's
  explicit `libraryPayloadValidated` gate, which must come from a separate
  library payload validator before it is enabled.

The platform publisher must build the complete envelope for the reserved
sequence, atomically stage its bytes at
`sync/pending/<stream>-<seq>.json`, durably persist the new state and hash,
then upload. On restart, a pending state requires matching staged bytes;
missing or mismatched bytes pause publishing and report corruption. The stage
can be removed after confirmed read-back. The shared core performs no file or
network I/O. There is no older
`state.json` schema to migrate; partial version 1 files are rejected instead of
guessing defaults.
- `purple/purple_engine.{h,cpp}` - resolves a preset into a flat table of "for
  this list, show and notify are these", and answers what that means for one
  chat. Resolution runs once per config or preset change, never per repaint.
- `purple/purple_screentime.{h,cpp}` - `screentime.log` and everything derived
  from it: the line format and its tolerant parser, session derivation, active
  time, the buckets, the heat map, the period comparison, the budget ledger and
  retention. The log is raw events and nothing else, so every threshold in
  `[screen_time]` is applied at read time and changing one re-derives the
  history you already have. It also holds `FormatSpan()`, the one piece of
  wording in the core: both apps had written "6 h 12 m" for themselves and the
  two spellings had already drifted apart.
- `purple/purple_passcode.{h,cpp}` - maps a passcode candidate typed with the
  Persian keyboard layout to its English key positions. Verification callers
  can try the mapped candidate without changing the stored passcode.
- `purple/purple_types.h` - the two type aliases and the `_q` string literal the
  core borrowed from tdesktop's `base/basic_types.h` before extraction. It
  defers to that header when compiled inside tdesktop and defines them itself
  otherwise.
- `tomlplusplus/toml.hpp` - vendored toml++, verbatim and unmodified (MIT).

## Consumers

- Purple Telegram desktop, the tdesktop fork, as a submodule at
  `Telegram/ThirdParty/purple_core`.
- A future Android fork, built through the NDK.

Both put the repository root on their include path, so
`#include "purple/purple_settings.h"` resolves the same way in either.

## Design contract

- **No dependency beyond Qt Core, the vendored toml++, and the C++20 standard
  library.** No Qt Gui, no Qt Widgets, no tdesktop, no Android. Anything that
  needs a window, a network, or a platform belongs in the consuming app.
- **Warn, don't fail.** `settings.toml` is hand-owned, so nearly everything
  recoverable is a warning attached to the parse result rather than an error
  that loses the file. A typo degrades to "keep the last good settings and show
  a banner", never to an exception crossing an event handler. The splicer is the
  strict half: if it cannot find the exact line and column it must edit, it
  refuses to write at all rather than guess.
- **Every boolean key ends in `_p`.** A naming convention, enforced by the
  parser, so a key's type is readable from the key.
- **The `version` key** in `settings.toml` names the schema the file speaks. It
  moves only when a key changes *meaning*, because that is the only change an
  older build gets wrong rather than merely misses. It is not a build number and
  not a date.
- Config sync is a pure decision layer. Its `localFp` argument must be a valid
  `SettingsFingerprint()` result for the current file bytes. Malformed local
  fingerprints or persisted base, lineage, pending, or equivalent keys make
  `inputValid` false and produce no head decisions. The highest sequence per
  remote writer is selected before validation; a malformed highest record or
  two different records with the same highest sequence receive `Invalid` and
  suppress that writer's older record. No watermark is changed.
  The classifier rejects more than 16 equivalent base keys. The caller
  validates the full payload, including text and envelope, before considering
  a head for application.
  A `Same` result applies to that head alone: when several maximal heads remain,
  callers must inspect every outcome before clearing local Dirty state or
  choosing an update. The returned head sequence is an advisory watermark.

The full schema reference lives with the desktop app, at `docs/purple/config.md`
in the tdesktop fork.

The shared notification preview exception policy is documented in
[`docs/notifications.md`](docs/notifications.md). It is separate from Work Mode
notification mute decisions.

## Tests

`tests/test_config.cpp` compiles the core's translation units into a small
harness and drives them against several hundred fixture documents - far too
slow to iterate on through a full app build, which is the point.

```sh
QT_PREFIX=/path/to/qt tests/run.sh
```

`QT_PREFIX` must hold `frameworks/QtCore.framework`; it defaults to
`$HOME/code/misc/tdesktop-libs/local/qt`, the prefix the desktop fork builds
against. The build lands in the gitignored `tests/build/`. The script exits
nonzero if anything fails to compile or any check fails.

## History

This code was extracted from the Purple Telegram desktop fork. Everything before
the extraction commit lives in that repository's history, under
`Telegram/SourceFiles/purple/`.

## License

GPL-2.0-or-later. See `LICENSE` for the version 2 text; the "or any later
version" grant is stated in each source file's header, which is where the
version 3 desktop fork gets its right to link this in.

`tomlplusplus/toml.hpp` is third-party and stays under its own MIT license.
