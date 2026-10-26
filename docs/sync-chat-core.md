# Sync chat core contracts

The shared peer codec, message ownership predicate, mark parser, eligibility
check and resolver perform no Telegram requests or filesystem I/O. They consume
facts supplied by the client.
These local contracts do not establish search coverage, broadcast ownership or
server error mappings. The [carry journal](sync-chat-carry.md) defines settings carry and recovery.
Clients execute its actions before a returned state/cache can be committed.

## Peer tokens

`purple_peer_token.h` defines `PeerToken`, `ParsePeerToken`, `PeerTokenText` and
`ComparePeerTokens`. Canonical text is exactly `self`, `user:<id>`, `chat:<id>` or
`channel:<id>`. A typed id is a positive ASCII decimal in the core's existing
signed 64-bit id domain. Zero, signs, leading zeroes, whitespace, non-ASCII
digits, unknown kinds and overflow refuse. `self` has id zero in memory.

Supply the account's positive bare user id to normalize its `user:<id>` alias
to `self`; zero means the caller has not supplied that identity. Supply it when
converting client peers, before persisting chat state or a resolver cache. The
codec neither discovers that account nor resolves access hashes. Equal bare
ids of different kinds remain distinct. Comparison uses the canonical token's
case-sensitive string order, including decimal digits, so `user:2` sorts after
`user:10`. Invalid in-memory tokens yield no text or comparison.

## Marks and ownership

`IsOwnSyncChatMessage(SyncChatMessageFacts)` is the shared ownership predicate
for marks and sync records. It returns true exactly when
`GetSyncChatMessageOwnership(facts)` returns `Own`. The latter retains the
existing `Own` / `Other` / `Unknown` distinction for inventory and carry:
unknown origin must hold review, rather than being discarded as another
author's record. Pass that verdict to `ParseSyncChatMark` and to the ownership
argument of `MakeSyncCarryMessage`. Do not replace Unknown with Other by
converting the boolean result back into an enum. Record bytes, writer-install
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
  genuine self marks with `out=false` and absent `from_id`. False or unknown
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
predicate to discovery hits, exact mark rereads and record inventory/readbacks.

`ParseSyncChatMark(text, hasMedia, ownership)` accepts only an explicit `Own`
verdict, supplied by the ownership check above. Unknown and Other refuse. It
does not infer ownership from `out`, sender ids or a chat kind.

The final nonempty line is trimmed and case-folded. It must be exactly
`#purplechatsync`, or that tag followed by one ASCII space, `v`, and a positive
ASCII decimal version. An absent version is 1. Leading zeroes in a positive
version are allowed, since the mark grammar does not require canonical digits;
the result returns normalized decimal text. Arbitrarily large versions remain
NewerVersion without integer overflow. Zero, extra words, embedded mentions,
F2 pointer forms on the tag line and all media messages refuse. Earlier lines
are explanatory text and are not interpreted. A newer mark never authorizes
posting.

## Eligibility and reader facts

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

Private chats require a warning about the recipient; bot chats require one
about the bot owner. Groups/channels report other member/admin counts when
known and require a warning if either count is positive or unknown. A known
empty audience and Saved Messages need no warning. Clients localize the facts,
include the actual sync-chat readers and mark exposure, and apply the approved
account-file disclosure. Do not claim a roster of other accounts is synced.

## Resolver inputs and outputs

`ResolveSyncChat` consumes validated marks with canonical peer tokens, positive
message ids and dates, supplied eligibility, fresh direct-read status, record
presence/readability, an optional cache, account binding/chat, abstract R3 and Q
outcomes, and the completed search-retry count. A zero id/date pair is allowed
only for a cache inferred from binding or Q. Malformed identities, unsupported
mark statuses and contradictory copies of the same chat/message refuse with
Invalid; duplicates with identical facts are harmless. Normalize self aliases
before ordering and comparing marks.

An explicit cache wins over binding. Without one, a bound settings state's
chat supplies the effective cache at date zero. State version 1 names implicit
Saved Messages. An unbound settings chat alone supplies nothing. Q Found can
select implicit Saved Messages only when there is no effective cache or eligible
mark. Absent, Unknown and NotRun hints do not select a chat, and Q is only a hint
before the normal complete settings inventory.

Eligible marks sort newest first, then by larger canonical chat string, then
larger message id. `SyncChatMarkOutranks` shares that comparison with the carry
journal, so a losing Switch mark cannot be committed as a completed move. Ineligible marks cannot win. The newest eligible newer-version
mark holds posting and retains the effective cache as the read chat. A mark
that would change the winner requires a fresh direct reread; otherwise the
result is VerifyMark, with posting held. The adapter must keep R2 until evidence
permits removing it and must not confuse a search hit with that direct reread.

Valid and Unknown R3 outcomes keep the cached candidate at its cached date;
Unknown also holds posting. Older marks never displace that candidate. A fresh
winner in the same chat returns Use and updates the cache. A newer winner in
another chat returns Follow and keeps the old read chat/cache until carry
finishes. A new device returns Fill when its winner is known empty and an older
eligible chat is readable, or Q found records in Saved Messages. The source
inventory is read during carry even when its record presence is still unknown.
The newest such older source is chosen deterministically. When a new winner's
record presence is Unknown and one of these sources exists, ReadWinnerInventory
returns the winner and source with posting held and no committed cache/read chat.
The client completes the winner's inventory, retaining the original cache input,
then resolves again: Empty returns Fill, while Present can return Use and commit
the winner's cache. A fresh mark reread alone does not complete that inventory.
An Unknown winner with no eligible readable older or Q-found source can still
return Use; normal inventory/publish gates apply. Follow and Fill always hold
posting; no carry or cache/state commit happens in this function.

A lost/deleted/migrated cached mark with no replacement enters Searching.
An older winner in another chat is only `olderAlternative`; it never causes an
automatic Follow. Retry counts 0, 1 and 2 return delays of 30, 120 and 600 seconds
respectively; count 3 or greater returns Lost. The client tracks the episode,
waits those delays and increments the count after each retry. Core creates no
timer. Lost's Create action must resolve afresh before creating anything.

Use with other readers sets `needsReaderWarning` and holds posting until the
client supplies `readerWarningAccepted` for that winner. An unknown or refused
cached eligibility also holds posting. `postingHeld == false` only clears this
resolver gate: normal binding, clone, stage, full-info/R3, stream validation and
publish-planner gates still apply. Resolve on every required startup/post path;
never post directly from a search result, ReadWinnerInventory, Follow, Fill,
Searching or Lost.

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
Ordinary callers must treat every stage as unresolved here. The
[carry commit helper](sync-chat-carry.md) separately consumes its one exact
journal-owned stage through verified N readback before calling this transition;
its retained bytes then serve only crash recovery until the state write retires
them. A same-chat request is a no-op, retaining version 1 for implicit self. A real
move returns version 2, the new chat and an empty own-message ledger, including
removal of its preserved JSON copy. Install/device/space/binding, counters,
config ancestry, seen sequences and issued record hashes survive. Moving back
to Saved Messages retains version 2. Failures return the unchanged state.

Clients must not assign the chat field directly for a move. Carry and fresh
readback must complete before committing this returned state/cache. New copies
can re-enter the empty ledger through the existing exact issued-record adoption
rule; the adapter must establish that their ids/readbacks belong to the new
chat. The core's envelope bytes cannot establish Telegram origin. Older readers
that understand only state version 1 refuse the emitted version 2, protecting
them from interpreting another chat's message ids as Saved Messages ids. Clients
linking this newer core must route v2 reads/posts by `state.chat` or hold them
before running any existing Saved Messages-only scanner/publisher.

## Integration and remaining probes

Register `purple/purple_peer_token.cpp` and `purple/purple_sync_chat.cpp` in the
Desktop `Telegram/cmake/purple.cmake` core source list and Android's native
purple-core source list. The Android JNI layer needs typed mappings for peer,
mark, eligibility, reader and resolver enums/optionals. No client source list,
JNI bridge, UI, uploader, history reader or server adapter is changed here.

S1/S2/S8 still gate discovery coverage, tag/search behavior, fallback selection
and R2 removal. S3 gates the broadcast ownership branch. S7 gates raw R3 reply
mapping; network/flood/ambiguous outcomes must remain Unknown. S10 gates Q's
caption-versus-filename query. F2 pointers, broad broadcast defaults and any
arbitrary-chat fallback remain inactive. Forward/carry probes remain required; the pure [carry journal](sync-chat-carry.md)
does not establish their server behavior. Tests of supplied facts are local contract
checks, not evidence that these Telegram probes ran.

C0 adds only the pure ownership contract. Run 8 supplies the self exception;
each client's native normalization still needs N1/N2 qualification. Keep R2
until that client's self parity check passes. D2 and native origin checks remain
necessary for any future broad broadcast relaxation. The automatic settings
policy's Self-only guard remains in force; an Own verdict neither activates
non-self automation nor authorizes established-stream moves or native writes.

## Verification

`ARCHIVE_SQLITE_TESTS=1 tests/run.sh` passed all eleven programs with 258,996
checks and zero failures, including 10,622 config checks, 1,248 sync-flow checks,
50,665 sync-chat checks and 128,372 SQLite checks. Cases include refusal matrices,
3,000 peer round trips, 1,000 randomized mark sets with repeated permutations,
and 1,000 state moves. The winner-inventory regressions cover Unknown becoming
Empty or Present with marked/Q-found sources, including the reviewer's exact
input, and progress when no Fill source exists. A prior separate check compiled the
actual prior state-v1
reader: it accepted real serializer v1 bytes and returned NewerVersion for both
v2 channel and v2 self bytes. No server probe or client integration is claimed.

The C0 ordinary unit matrix covers every common guard as safe, excluded and
unknown across all six accepted kinds, unknown plus known exclusions, outgoing
states, typed sender normalization, malformed ids and cross-kind collisions.
It includes the Run 8 self case, private-recipient sender substitution, group
anonymous/channel senders, all broadcast mode/out/creator/sole-admin states,
and the same verdict consumed by mark parsing and settings/playlist carry
selection. Unknown records hold review; excluded records are ignored; own
opaque future records survive selection.
