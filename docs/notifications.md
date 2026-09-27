# Notification preview exceptions

`[notifications].preview_always` names chats whose full notification previews
the Purple apps may show even when their own preview redaction would hide them,
including redaction while the app passcode is locked. The operating system's
notification privacy settings still determine what finally appears.

```toml
[notifications]
preview_always = [
  "MAGIC_BOTS",
  "MAGIC_CHANNELS",
  "private:123",
  "bot:456",
  "group:789",
  "channel:321",
]
```

`MAGIC_BOTS` covers every bot and `MAGIC_CHANNELS` covers every channel. Both
are enabled when the key is absent. A present array replaces those defaults,
so `preview_always = []` disables all exceptions. To keep one class, include
its magic name explicitly.

Each chat entry consists of its type and its positive decimal bare peer ID.
The canonical type spellings are `private`, `bot`, `group`, and `channel`.
`user` is accepted as an alias for `private`. IDs are matched together with
their types: `private:123` does not match `bot:123`. Zero, signs, whitespace,
overflowing IDs, unknown types, and non-string array entries are ignored with
warnings. Repeated entries, including `user:123` beside `private:123`, are
collapsed.

The shared query is `Purple::PreviewAlways(settings, bareId, kind)`. Clients
must pass the positive bare ID and the actual `ChatKind`. This setting only
controls app-owned preview redaction. Work Mode mute policy remains independent:
a muted notification is not made eligible for delivery by this exception.
