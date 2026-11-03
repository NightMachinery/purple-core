# The Magic Purple Sync chat

@Opus5.5 [2026-11-03 Tue]

Each device finds its account's sync chat in its own local chat list, or
creates it. No search request is part of this. The chat is a private
supergroup titled exactly `Magic Purple Sync` (`kMagicSyncChatTitle`); the user
is its creator and its only member and admin. `purple_sync_chat_magic` holds the
policy. Clients supply facts and perform every request.

## Eligibility

`CheckMagicChat` returns Eligible, Unverified or Ineligible with a
`SyncChatProblem`. In order, it refuses:

- a chat the user left or lost (ChatLeft);
- anything that is not a megagroup, including gigagroups and broadcasts (NotMegagroup);
- a public or active secondary username (PublicUsername);
- not being the creator (NotCreator);
- another member (ExtraMember) or another admin (ExtraAdmin), when the count is known;
- the shared `SyncChatEligibility` refusals: CannotPost for missing file rights,
  Restricted for the rest (protected content, topics, auto-delete, linked chat,
  paid messages, anonymous admin or send-as, migration, slow mode).

Unknown member or admin counts give Unverified. The title is not checked, so a
renamed bound chat keeps its role. `PickMagicChat` returns the lowest channel id
among Eligible and Unverified chats. Ids grow over time, so this is the oldest
chat, and every device makes the same choice.

## Binding

`sync/chat.json` (`SyncChatBinding`, a strict canonical JSON codec that keeps
unknown fields) records this device's choice for one account:

- mode: none, magic or manual;
- the bound chat token and when its full info was last read;
- transient waits, keyed by Telegram method name;
- the creation step and when it started;
- a real problem and whether it has been shown;
- the migration step;
- an opaque login authority supplied by the client.

`RecordSyncChatWait` only extends a deadline. `SyncChatWaitUntil` tells every
caller, after a restart too, whether a method may be sent.

## Lookup

`PlanSyncChatLookup` returns one of Use, Verify, Wait, ProveAbsent, Create or
Problem.

- A recorded problem stays until the user acts (`ClearSyncChatProblem`). It is
  reported with `show` until the client records that it was shown. Raising a
  different problem shows again.
- Manual binding: Saved Messages is always usable. Any other chat must be
  present locally and joined, have no other member or admin, and pass the
  shared refusals. Manual mode never creates and never considers magic chats.
- Magic binding: a bound chat that is gone or ineligible is a Problem. An older
  eligible magic chat in view leads to Verify of that chat with `rebind`, so
  devices that each created a chat converge on the oldest. Full info is read
  again when the last read is 24 hours old and the trigger needs fresh evidence
  or the local counts are unknown, unless that read is under a wait.
- No binding: the oldest usable magic chat in the local lists is verified.
  Magic chats that are all ineligible are a Problem (MagicIneligible), never a
  reason to create another. Otherwise one `messages.getAllChats` reply is the
  absence proof (ProveAbsent). Only an empty reply leads to Create.
- A creation that was requested or uncertain blocks another one for 24 hours,
  so the chat can still arrive through updates.

`CompleteSyncChatVerify` checks fresh full info. It either binds the chat in
magic mode, finishing any creation record, or raises the problem.

## Moving the state into the bound chat

A client moves its existing state with `MoveSyncStateToChat` once the lookup
binds a chat other than the one in `state.json`. A config stage still pending in
the old chat is abandoned first, since the old chat is never read again; the
next publish reserves a fresh sequence. A pending library stage refuses the
move. The move empties the own-message
ledger, because those message ids belong to the old chat. Nothing of this
install is in the new chat yet, so the confirmed config head is expectedly
absent there. `SyncConfigAwaitsMovedPublish` names that case: a v2 state with a
confirmed head and an empty ledger. A confirmed head is never removed from the
ledger, so the ledger is empty only between the move and the first publish.

In that case the config review reports LocalChanges instead of UpToDate,
`PlanSyncPublish` lets the absent head through, and automatic sync publishes
the local settings as a new version whose parent is the old base. Later
absence of a published head is still NeedsReview. When the new chat already
holds another device's newer version, the ordinary review applies, so a move
never needs its own merge rules.

## Tests

`tests/test_sync_chat_magic.cpp` covers the title, every refusal, the oldest
pick, the codec round trip and refusals, the persisted waits, show-once
problems, the unbound, magic and manual plans, and verification.
It also covers moves that abandon a stage, keep the same chat or are refused.
`tests/test_sync_auto.cpp` covers the first publish after a move, a move that
abandoned a stage, a later
missing head, an unmoved missing head, a move without saved bytes and a move
into a chat with a newer version.
