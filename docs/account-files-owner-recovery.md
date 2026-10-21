# Account files and startup ownership

`ResolveAccountFiles` returns relative public path descriptions for a validated
account and the immutable startup root owner. Root files keep the legacy layout.
Other accounts use `accounts/<environment>-<user-id>/`. Desktop History is
`sync/history`; Android History is `sync-history`. Root send fingerprints belong
to device `state.toml`; other accounts use their `send-state.toml`. Device state,
`local.toml` and `accounts/root.toml` are never placed inside account directories.
An invalid identity, unknown owner or invalid layout yields no account paths.

`PlanAccountOwnerStartup` consumes supplied file-presence snapshots and synthetic
or caller-provided TOML text through the existing codecs. It never reads files.
Two valid owners agree, or copy 2 wins with a notice and copy 1 repair. One valid
owner remains authoritative even while signed out. Copy 1 repairs retain parsed
global-account and Work Mode choices. A live global edit cannot change the
startup owner; call the startup planner only at startup.

An existing local file, owner record or accounts directory prevents automatic
migration when neither owner is usable. Empty or malformed files count as
presence. Copy 2 also implies accounts evidence even when the parent-presence
flag is absent. Contradictory per-account directory metadata, invalid identities,
duplicate identities and reassignment of a confirmed owner refuse conservatively.

With no migration evidence, one signed-in account can propose ownership,
including a sole test account. Among several accounts only one production binding
match selects automatically. No match, ambiguous matches or a test match require
choice. Explicit choice requires a signed-in account without its own directory.
No accounts means wait for first sign-in. Pending ownership is separate from
confirmed startup ownership. Its ordered plan writes `accounts/root.toml` first,
then `local.toml`, with both root and global set to the choice. Nothing moves,
no binding is restored and no store or settings file is copied.

All non-owned outcomes explicitly hold account-file loading, settings sync,
launch offers, message imports and automatic sending. Manual Send and Share
remain user actions. Single-file root settings continue to apply to all accounts;
frontend enforcement remains required. An initial pending plan has no confirmed
startup owner, so it cannot authorize account path resolution. A later snapshot
containing only successful copy 2 uses that owner and repairs copy 1.

These are pure descriptions and plans, not observed filesystem recovery or
isolation. Clients still need atomic writes with fresh preconditions, startup
routing, file loading and watching, UI notices and choices, permission enforcement
and account-specific storage. No frontend version/import gate is enabled here.
