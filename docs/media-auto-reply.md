# Media auto-reply

The shared policy replies with configurable text (initially `@TranscribeThisAI`)
to new voice notes and round video notes. Ordinary videos, audio files, history,
edits, scheduled messages and unacknowledged outgoing messages are excluded.
Every account starts Off, including accounts that share app-wide preferences
with an enabled account. This namespace is deliberately account-only.

`[media_reply] text` holds the reply. The `private`, `groups` and `channels`
subtables each accept `voice` and `round` (`off`, `own`, `received`, `both`) and
`round_follow_voice` (initially true). Checked round inheritance follows the
resolved voice mode after overrides. Unchecked `round = "off"` is explicit Off.

`[media_reply.sets.NAME]` adds an ordered named selected-chat set. `peers` holds
canonical `user:ID`, `chat:ID` and `channel:ID` strings. The first matching set
claims a chat; omitted row values inherit its chat-type defaults. Bare Work Mode
memberships cannot safely identify a peer type, so they never drive automatic
sends. Media sets do not change those memberships or the active preset.
Native editors accept human names, including spaces and non-Latin text. The writer
quotes names as one TOML key, so dots cannot become extra tables.
Resetting an override removes its key without rebuilding the document.

The clients supply a stable account key, typed original peer, final server
message ID, topic, direction, media kind, new-message origin and activation
cutoff. Eligibility requires a strictly later server timestamp. Settings changes
invalidate pending actions through a revision fence. Restart establishes a new
cutoff; it does not replay a queue. This intentionally sacrifices catch-up and
same-second activation messages to prevent a history flood.

`MediaReplyRandomId` uses a fixed feature namespace in the upper 32 bits and the
positive original server message ID in the lower 32. Original message IDs are
unique within the exact outgoing peer. Clients must never remap channel posts
to linked discussions. The ID is independent of device, text, topic and revision.
It is injective among this feature's actions in that scope, with no claim of a
reserved namespace among other clients' random message IDs.

[Telegram's update contract](https://core.telegram.org/api/updates) documents
same-peer random-ID deduplication across sessions of the current account, without
expiry, and a duplicate error for an overlapping in-flight request. The client
reserves each action durably before dispatch and uses that same ID on both
platforms. Any outcome, including duplicate, transport uncertainty and native
rights/flood/paid refusal, leaves the reservation non-sendable. No blind retry,
new-ID retry or optimistic local reply bubble is created. This is an at-most-once
attempt policy backed by documented server deduplication, not a live protocol
qualification or a guarantee that a reply is delivered.

The bounded journal retains at most 4096 reservations and fails closed when full
or unreadable. Clients bound pending callbacks and in-flight requests. Local
storage errors prevent dispatch. Reply text cannot retrigger media eligibility.
No paid-send permission or paid flood-skip flag is supplied. Mentioning a bot
does not grant it access to a private chat.

Run `tests/run.sh` for the policy, parser, writer, identity, journal and lifecycle
regressions together with the existing core floors. Native client compile and
real callback tests are separate qualification requirements.
