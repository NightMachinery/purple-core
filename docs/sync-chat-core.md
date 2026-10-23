# Sync chat core contracts

The shared peer codec, mark parser, eligibility check and resolver perform no
Telegram requests or filesystem I/O. They consume facts supplied by the client.
These local contracts do not establish search coverage, broadcast ownership or
server error mappings. Moving existing streams still requires client carry and
journal execution before a returned state can be committed.

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

`ParseSyncChatMark(text, hasMedia, ownership)` accepts only an explicit `Own`
verdict, supplied by a separate ownership check. Unknown and Other refuse. It
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
`CreatorAndSoleAdmin` requires both supplied facts. Both branches are testable;
this core change selects neither for a client. It does not compute a message
ownership predicate or map raw Telegram sender fields.

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
larger message id. Ineligible marks cannot win. The newest eligible newer-version
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
A same-chat request is a no-op, retaining version 1 for implicit self. A real
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
arbitrary-chat fallback remain inactive. Forward/carry probes and the move
journal belong to the next batch. Tests of supplied facts are local contract
checks, not evidence that these Telegram probes ran.

## Verification

`tests/run.sh` passed with 10,622 config checks, 1,248 sync-flow checks and
48,192 sync-chat checks, all with zero failures. Cases include refusal matrices,
3,000 peer round trips, 1,000 randomized mark sets with repeated permutations,
and 1,000 state moves. The winner-inventory regressions cover Unknown becoming
Empty or Present with marked/Q-found sources, including the reviewer's exact
input, and progress when no Fill source exists. A separate check compiled the
actual prior state-v1
reader: it accepted real serializer v1 bytes and returned NewerVersion for both
v2 channel and v2 self bytes. No server probe or client integration is claimed.
