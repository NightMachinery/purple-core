# Account identity and device-local choice codecs

The production codecs and their `test_config` cases compile and pass in the
standalone core harness. Native client integration remains a separate check.

## Identity contract

`AccountKey` contains a Telegram environment and a stable signed-in user id.
It never contains a desktop ordering position, Android account slot, sync
installation id, binding token, peer token or account display name.
Production is environment 0 and test servers are environment 1. Equal user ids
in different environments are distinct accounts.

`AccountKeyText(environment, userId)` returns the canonical key or no value.
`ParseAccountKey(text)` returns the identity or no value. The grammar is exactly
`0-<id>` or `1-<id>`, with a positive ASCII decimal id in the existing core
`int64` domain. Zero, signs, leading zeroes, whitespace, non-ASCII digits,
overflow and extra path components are rejected. No normalization makes an
invalid path component acceptable. The parser does not prove that an account
is signed in, or that a directory belongs to it.

The approved account-settings design section 1 defines this key; sections 3.6
and 4.1 require its stable identity and directory use. There is no existing
account/environment codec in the base core tree to extend. This unit reuses
`purple_types.h`, Qt Core and vendored toml++ instead of introducing client
dependencies or another sync representation.

## Local choices

`ParseLocalChoice(text, path)` reads `global_account`, `root_file_account` and
`[account_work_mode]`. Only TOML syntax errors set `error`. A malformed known
field produces a warning and leaves that choice unset, or leaves the affected
account on `Own`. Unknown root keys and tables are ignored. There is no
version field in this local-file contract, so an unknown `version` key does
not introduce an invented version gate. Unsupported TOML syntax is refused
by the genuine toml++ parser; unsupported value types in known fields warn.

`globalAccount` is the valid requested choice, which may refer to a signed-out
account. Absence and invalid values do not fabricate an identity.
`GlobalAccountFor(choice, startupOwner)` uses the valid global choice, otherwise
the caller's validated startup owner. It deliberately does not use the owner
spelling found in a live-edited `local.toml`. An invalid startup owner and no
valid global choice produce no value. This implements account-settings
section 4.2 without bypassing section 4.3's startup ownership rules.

Work Mode values are exactly `own`, `follow` and `stock`, as specified by the
account-workmode design section 6.1. Missing entries use `Own`.
`AccountWorkModeFor(choice, account, globalAccount)` preserves environment
separation, returns `Own` for a missing entry and ignores `Follow` for the
global account. `Stock` remains allowed for the global account. The parsed
choice is retained even when it is currently ignored, so switching the global
account does not erase a device preference. The parser emits a note when an
explicit global choice also says `follow`. When the effective global account
comes from startup fallback instead, the client must supply the corresponding
note when it applies the choice. No settings layering, list resolution or
chat refresh occurs here.

## Lossless Work Mode edits

`SetAccountWorkMode` in `purple_splice.h` edits one account's local Work Mode
choice while preserving unrelated text and unknown fields. Use it for the
account chooser after initialization; the whole-template writers below retain
their existing serialization contract. See
[list member writes](list-member-writes.md) for the splice's refusals and the
own-list fork workflow. No local choice is written to a synced settings file.

## Root-file ownership

`ParseOwnerRecord(text, path)` reads the same `root_file_account` key from
either copy. It distinguishes `Missing`, `Invalid` and `Valid`; an invalid
result exposes no account. Empty valid TOML means a missing key, not permission
to migrate. Unknown fields do not invalidate a known owner.

Account-settings section 4.3 makes the record startup-only. A caller must parse
both copies, compare the complete identities and separately check file and
directory existence. Copy 2 wins a valid disagreement; neither codec chooses
an owner, rewrites a file, infers migration eligibility or authorizes a send.
Different valid owners, including equal ids in different environments, remain
different results for that comparison. The parsed local-file owner is a
record, not a live binding. A caller restoring ownership must replace it with
the chosen startup owner before serializing.

Root-file ownership is not proof of a sync-store binding. Existing binding
tokens and the separately planned Relink cloud proof remain authoritative for
sync, as account-settings section 6.4 requires. This unit does not change the
manual sync engine, its envelope format or the sync-channel cache/journal.

## Whole-template writers

`SerializeLocalChoice` and `SerializeOwnerRecord` return no value for invalid
identities. The local writer requires a root-file owner, retains a valid
hand-edited global choice and writes an absent global choice as the owner.
It refuses invalid in-memory Work Mode keys or enum values. The output has
quoted canonical keys, device-only and startup-only guidance, the global
settings-file path, and the account Work Mode table when present. A caller
may supply its already-known accounts with files for the explanatory file
list. This is data supplied by the caller, not filesystem discovery.

An optional display-name callback supplies comments only. Controls and Unicode
line/paragraph separators are replaced with spaces, preventing a name from
adding another TOML line. This API performs no account lookup. The owner
writer emits the one owner key and an app-managed notice.

The approved section 4.2 explicitly specifies a whole-template rewrite.
Accordingly, these writers serialize the known model rather than preserving
unknown fields, original comments or formatting. Parsing ignores unknown
fields; callers must not claim a lossless text round trip. This differs from
the hand-owned settings splicer and the sync JSON envelope, whose existing
unknown-field preservation contracts are unchanged. No local choice, display
name or owner record is added to a synced settings document.

## Headless checks and remaining integration

The existing `test_config` program gains production-function cases covering
canonical boundaries, overflow/path rejection, production/test separation,
absent/default choices, malformed known fields and TOML, valid conflicting
owners, unknown-field tolerance, Work Mode defaults and global-follow handling,
serialization refusal, comment injection resistance and semantic round trips.
The existing `tests/run.sh` adds this real production source once to its source
list and leaves its flags, link route and two real programs unchanged. No
substitute parser, policy mirror or new runner is introduced. The existing
`test_sync_flow` source is unchanged because this unit does not change a sync
transition.

`tests/run.sh` compiles the production TU and genuine tests against Qt Core
and vendored toml++. Both test programs passed; the current counts are recorded
in [list member writes](list-member-writes.md). Native client integration and
independent source review remain separate from these headless checks.

Remaining account work includes owner-copy reconciliation and durable write
ordering, migration and single-file mode, client account discovery and file
watching, signed-in identity checks, registry/layering/list membership, ordered
refresh, per-account stores and Relink, frontend build integration, UI and
native verification. Shared Peek runtime state remains device-wide; list
structure remains app-wide. Per-account layering and lossless member forks are
provided by the core layering and splice APIs. No transport, playlist or live-file behavior is implemented by
this codec unit.
