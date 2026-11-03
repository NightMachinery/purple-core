# Sync chat core contracts

The shared peer codec, message ownership predicate, eligibility refusals and
chat move transition perform no Telegram requests or filesystem I/O. They
consume facts supplied by the client. Which chat an account syncs through is
the [Magic Purple Sync](sync-chat-magic.md) policy's decision; it calls the
eligibility check and the move below. These local contracts do not establish
broadcast ownership or server error mappings.

Older clients still post and search for `#purplechatsync` marks and run
mark-based carries; this core reads neither, so update an account's devices together.

## Peer tokens

`purple_peer_token.h` defines `PeerToken`, `ParsePeerToken`, `PeerTokenText` and
`ComparePeerTokens`. Canonical text is exactly `self`, `user:<id>`, `chat:<id>` or
`channel:<id>`. A typed id is a positive ASCII decimal in the core's existing
signed 64-bit id domain. Zero, signs, leading zeroes, whitespace, non-ASCII
digits, unknown kinds and overflow refuse. `self` has id zero in memory.

Supply the account's positive bare user id to normalize its `user:<id>` alias
to `self`; zero means the caller has not supplied that identity. Supply it when
converting client peers and before persisting chat state. The
codec neither discovers that account nor resolves access hashes. Equal bare
ids of different kinds remain distinct. Comparison uses the canonical token's
case-sensitive string order, including decimal digits, so `user:2` sorts after
`user:10`. Invalid in-memory tokens yield no text or comparison.

## Message ownership

`IsOwnSyncChatMessage(SyncChatMessageFacts)` is the shared ownership predicate
for sync records. It returns true exactly when
`GetSyncChatMessageOwnership(facts)` returns `Own`. The latter retains the
`Own` / `Other` / `Unknown` distinction for inventory: unknown origin must hold
review, rather than being discarded as another author's record. Do not replace
Unknown with Other by converting the boolean result back into an enum. Record bytes, writer-install
fields, captions and filenames cannot establish Telegram ownership.

All six common facts are required, including in Saved Messages: `regular` must
be true, while `forwarded`, `viaBot`, `scheduled`, `hasTtl` and `sentAsOther`
must be false. Each is an optional boolean. An omitted value means unknown,
not false. A known service message, forward, via-bot message, scheduled message,
TTL message or alternate send-as identity returns Other. If there is no known
exclusion but a required fact is unknown, the verdict is Unknown. Both refuse
admission. Default-constructed facts authorize nothing.

After those common guards, ownership depends on the chat kind:

- Saved Messages (`Self`) requires no `out` or sender fact. Run 8 observed
  genuine self messages with `out=false` and absent `from_id`. False or unknown
  outgoing state and an absent sender therefore remain acceptable here.
- A private or bot chat requires affirmative `out`. Sender identity does not
  replace it and is not compared. This accommodates native clients substituting
  the private recipient for an absent sender.
- A basic group or supergroup requires affirmative `out` and a self sender.
  Supply a typed `PeerToken` sender and the account's bare `selfUserId`, or an
  already established canonical `Self` token. The existing peer codec normalizes
  `user:<selfUserId>` to self. A user sender with no account id is Unknown;
  absent/malformed senders or out-of-domain account ids are also Unknown.
  Another user, a chat sender or a channel sender is Other. Numeric equality
  across different peer kinds never proves self, so anonymous/send-as channel
  senders fail even if their bare id equals the account's user id.
- A broadcast requires affirmative `out`, `creator` and `soleAdmin`, with
  `broadcastOwnership` explicitly set to `CreatorAndSoleAdmin`. Sender identity
  is not required: normal channel attribution or absent `from_id` does not
  establish an alternate send-as identity. With affirmative `out`, unselected,
  invalid and `OutgoingVerified` modes remain Unknown, including when all rights are true.
  Broad outgoing ownership is disabled in this predicate.

Unknown/invalid chat kinds return Unknown; secret chats and channel direct
messages return Other. Fields that a branch does not use may remain unknown:
self ignores outgoing/sender/rights; private and bot ignore sender/rights;
groups ignore broadcast rights; broadcasts ignore sender. Every fact required
by the selected branch must be known and safe before Own can be returned.

Native mappers must bind the chat and current account before constructing these
facts. `Self` means that account's Saved Messages, not an absent sender or an
unresolved peer. Optional `sender` defaults to absent, distinct from an explicit
canonical Self token. Map native constructor/flag absence to false only when
that absence is understood; unsupported, partial or contradictory observations
stay unknown. `sentAsOther` describes an alternate posting identity, not every
broadcast message attributed to its channel. Confirm creator and sole-admin
state from the origin account's current full info. These pure facts cannot
validate their native provenance or freshness. Apply the same mapper and
predicate to record inventory and readbacks.

## Eligibility

`SyncChatEligibility(facts, broadcastOwnership)` reports a typed refusal. It
rejects secret chats, forums/channel direct messages, migrated basic groups,
public usernames, protected content, linked discussions, auto-delete, paid
messages, missing file rights, anonymous/default other senders, and non-admin
slow mode. Slow-mode admins remain allowed when the other checks pass. Unknown
chat kinds refuse, and file rights must be supplied affirmatively.

The broadcast branch is a required argument. `Unselected` refuses broadcasts,
including a channel with a creator and sole admin. `OutgoingVerified` represents
the broad branch only after server probes validate ownership. The alternative
`CreatorAndSoleAdmin` requires both supplied facts. Both eligibility branches
remain testable; eligibility alone never proves message ownership. The message
predicate above permits only the restricted creator-and-sole-admin broadcast
branch. Clients must pass both checks and map raw Telegram fields themselves.

## Local state version 2 and moves

`SyncLocalState::chat` is a typed peer token. Version 1 is implicit Saved
Messages and emits no `chat` member. Version 2 requires canonical `chat` text,
including explicit `self` after moving back. Missing/malformed v2 chat tokens,
and any chat field or non-self in-memory chat under version 1, refuse. Unknown
JSON members remain preserved; `chat` is now a known reserved member. Versions
above 2 report NewerVersion before interpreting their fields. This state version
is independent of the `settings.toml` schema and the sync envelope major version.

`MoveSyncChat(state, newChat, stagePresent, selfUserId)` validates the input,
normalizes the target and refuses any staged file, pending config/library
sequence or pending config key. The required `stagePresent` fact must describe
all streams under the client's account operation lock, including orphan stages.
Callers must treat every stage as unresolved here; the magic policy's
`MoveSyncStateToChat` abandons a pending config stage before calling it. A
same-chat request is a no-op, retaining version 1 for implicit self. A real
move returns version 2, the new chat and an empty own-message ledger, including
removal of its preserved JSON copy. Install/device/space/binding, counters,
config ancestry, seen sequences and issued record hashes survive. Moving back
to Saved Messages retains version 2. Failures return the unchanged state.

Clients must not assign the chat field directly for a move. New copies
can re-enter the empty ledger through the existing exact issued-record adoption
rule; the adapter must establish that their ids/readbacks belong to the new
chat. The core's envelope bytes cannot establish Telegram origin. Older readers
that understand only state version 1 refuse the emitted version 2, protecting
them from interpreting another chat's message ids as Saved Messages ids. Clients
linking this newer core must route v2 reads/posts by `state.chat` or hold them
before running any existing Saved Messages-only scanner/publisher.

## Integration

Register `purple/purple_peer_token.cpp` and `purple/purple_sync_chat.cpp` in the
Desktop `Telegram/cmake/purple.cmake` core source list and Android's native
purple-core source list. S3 gates the broadcast ownership branch; broad
broadcast defaults remain inactive. Tests of supplied facts are local contract
checks, not evidence that any Telegram probe ran.

The automatic settings policy's Self-only guard remains in force; an Own
verdict neither activates non-self automation nor authorizes established-stream
moves or native writes.

## Verification

`tests/test_sync_chat.cpp` covers 3,000 peer round trips, the refusal matrix
and every combination of refusals, and 1,000 randomized state moves. The
ownership matrix covers every common guard as safe, excluded and unknown across
all six accepted kinds, unknown plus known exclusions, outgoing states, typed
sender normalization, malformed ids and cross-kind collisions, the Run 8 self
case, private-recipient sender substitution, group anonymous/channel senders
and all broadcast mode/out/creator/sole-admin states. A prior separate check
compiled the actual prior state-v1 reader: it accepted real serializer v1 bytes
and returned NewerVersion for both v2 channel and v2 self bytes.
