# Playlists core model and local operations (format 1)

This document specifies the first two A1 slices in `purple/purple_playlists.h`, `purple/purple_playlists.cpp`, `purple/purple_playlists_ops.h`, and `purple/purple_playlists_ops.cpp`, together with the opt-in JSON size-budget extension in `purple/purple_sync_json.h`.

## Scope of these slices

The model slice implements in-memory data structures, strict format 1 JSON parsing and validation, canonical JSON serialization, and pure register-level `JoinPlaylistLibraries` and `PlaylistLibraryContains` operations.

The local operations slice builds on that model with deterministic playlist ID formatting, local create/rename/delete/restore/purge/add/relink/remove/reorder editing, structured undo records and undo application, per-playlist monotonic `2^52` stamp allocation, the 24 MiB local edit size guard, and playlist/song display ordering.

These slices perform no file I/O, compression, network transport, clock reads, `AccountFiles` wiring, settings parsing, or UI integration.

## Shared JSON budget contract

`CanonicalizeSyncJson(json)` keeps its original one-argument signature and 4 MiB cap for settings and sync-state callers. A new opt-in overload `CanonicalizeSyncJson(json, maximumBytes)` accepts an optional byte budget so local playlist files (`library.json`) can be parsed uncapped (`std::nullopt`) while still reusing the same strict RFC 8785 lexical, UTF-8, surrogate, integer, duplicate-key, and depth checks.

## Format 1 data model and validation

A `PlaylistLibrary` holds `format` (exact integer `1`), `playlists` keyed by playlist ID, and a `purged` set of permanently deleted playlist IDs.

- **Format version:** `format` must be integer `1`. Any larger integer reports `NewerFormat` immediately before checking format-1 keys; anything else is `Invalid`.
- **Playlist IDs:** `pl-` followed by 16 lowercase RFC 4648 base32 characters (`[a-z2-7]`).
- **Purged set:** In JSON, `"purged"` maps playlist IDs to `1`. No purged ID may appear in `"playlists"`.
- **Playlists and partial records:** Each playlist holds optional registers `title`, `deleted`, `order`, and `songs`. At least one of the four must be present; an empty playlist object is invalid. Absent registers stay absent across parse and serialize round-trips, and act as the bottom element during merge.
- **Registers and stamps:** Every register is a `[value, stamp]` pair where `stamp` is an integer in `[1, 2^52]`.
- **Playlist title:** Valid Unicode, 1 to 64 code points, trimmed, single-line, with no C0/C1 control characters or Unicode line/paragraph separators.
- **Order list:** An array of distinct unsigned decimal document ID strings (`1` to `2^64 - 1`, no leading zeroes). Order lists may reference document IDs absent from `songs`.
- **Songs map:** Maps unsigned decimal document ID strings (`1` to `2^64 - 1`, no leading zeroes) to registers whose value is either `false` (removal tombstone) or a source object. A boolean `true` song value is invalid.
- **Source object:** Requires `peer`, `msg` (`1` to `2^31 - 1`), `meta` (`title`, `perf` of 0 to 96 valid Unicode code points each, and `dur` in `[0, 2^31 - 1]`), and `since` (`1` to `2^52`, with `since <= registerStamp`). Optional `un` is 4 to 32 characters of `[A-Za-z0-9_]`. Optional `alt` holds 1 to 3 fallback entries (`peer`, `msg`, optional `un`) when present in JSON, and is omitted when empty. Every `alt` entry must differ in normalized `(peer, msg)` from the primary source and from all other `alt` entries.
- **Peer normalization and privacy:** Peer tokens reuse `Purple::ParsePeerToken` and `Purple::PeerTokenText` with a caller-supplied `selfUserId`. Any `user:<selfUserId>` token normalizes to `self`, so the account's numeric user ID is never serialized. Access hashes, file references, and local paths are never part of the format.
- **Unknown keys:** Any unknown key at the root, playlist, source, metadata, or fallback level makes a format-1 document `Invalid`.

## Canonical serialization, join, and contains

`SerializePlaylistLibrary` validates the library, normalizes peer tokens against `selfUserId`, and emits compact UTF-8 canonical JSON with object keys sorted by UTF-16 code units (including sorting decimal document ID keys in `songs` lexicographically, so `"10"` precedes `"2"`) and RFC 8785 string escaping.

`JoinPlaylistLibraries(a, b, selfUserId)` merges two valid format-1 libraries without creating or modifying stamps:

- `purged` is the union of both purged sets, and any purged ID is dropped from `playlists`.
- Absent registers act as bottom (`join(absent, x) = x`).
- Present registers keep the pair with the larger stamp. On equal stamps, the pair with the larger canonical JSON value bytes (compared as unsigned UTF-8 byte sequences) wins, so a song source beats `false` and `deleted = true` beats `deleted = false`.
- `PlaylistLibraryContains(outer, inner, selfUserId)` holds iff `SerializePlaylistLibrary(JoinPlaylistLibraries(outer, inner))` equals `SerializePlaylistLibrary(outer)` byte for byte.

## Local operations, undo, and display order

`purple/purple_playlists_ops.h` and `purple/purple_playlists_ops.cpp` implement pure value-returning local mutations over `PlaylistLibrary`:

- **Playlist IDs from entropy:** `FormatPlaylistId` encodes 10 caller-supplied entropy bytes into `pl-` plus 16 lowercase RFC 4648 base32 characters. `CreatePlaylist` refuses collisions against either active `playlists` or `purged`.
- **Stamps and batching:** Stamped edits allocate `stamp = max(nowMs, playlistMaxStamp + 1)`. Batch operations (`AddPlaylistSongs`) reserve a contiguous span `s .. s + writeCount - 1` up front and fail atomically with `StampOverflow` if `s + writeCount - 1 > 2^52`.
- **Lifecycle edits and guards:** Mutation targets must have both title and deleted registers (`PlaylistIncomplete` otherwise). Rename, Add, Remove and Reorder refuse deleted playlists (`PlaylistDeleted`); Delete on an already deleted playlist and Restore on an active playlist are no-ops. All mutations return `changed = false` without allocating a stamp or undo entry on idempotent no-ops (same title, already deleted, already active, unchanged `order`, or songs already active at the same normalized `(peer, msg)`).
- **Adding, relinking, removing, and reordering songs:** `AddPlaylistSongs` rejects duplicate `documentId` inputs within one call, preserves input order via consecutive stamps on new or relinked songs, ignores unchanged already-active songs without overwriting `meta` or `since`, and relinks active songs with a changed normalized `(peer, msg)` by keeping `since`, taking the incoming primary source and metadata, and prepending the old primary and distinct `alt` entries up to 3 fallbacks. `RemovePlaylistSongs` tombstones only active songs. `ReorderPlaylistSongs` writes the full active `order` register only when the input is an exact permutation of currently active document IDs.
- **24 MiB local edit cap:** Edits that modify the library enforce a 24 MiB canonical JSON budget (`LibraryFull`). If an already oversized merged library (> 24 MiB) is passed in, mutations whose canonical JSON is strictly smaller than the input size (such as tombstoning songs or `DeleteForeverPlaylist`) remain permitted so users can free space.
- **Undo records and conflicts:** `CreatePlaylist` returns a `Create` undo record; `UndoPlaylistOperation` permanently purges the playlist ID only if the playlist is still untouched since creation, and returns `UndoConflict` otherwise (so merged remote activity or subsequent edits are never silently purged). Register-level edits return prior `title`, `deleted`, `order`, and affected `priorSongs` (`std::nullopt` for newly added songs so undo writes a removal tombstone) and replay only those captured registers using fresh monotonic stamps. Undoing a reorder whose prior order register was absent writes a fresh empty order register, so the undo survives merging the old reorder. Reorder checks the effective visible sequence as well as the stored list before declaring a no-op, because a re-added song can move to the end while the stored list stays unchanged. Non-delete undo writes fail with `PlaylistDeleted` if the playlist is currently deleted. `DeleteForeverPlaylist` has no undo and can proceed at stamp `2^52`.
- **Display ordering:** `PlaylistVisibleSongIds` emits active songs in `order` where `song.since <= order.stamp` (Group 1), followed by remaining active songs ordered by ascending `since` and numeric `documentId` (Group 2). `ListVisiblePlaylists` filters out deleted and incomplete playlists and sorts by descending caller-supplied `recentUseMs`, then case-insensitive title (`Qt::CaseInsensitive`), then ascending playlist ID.

## Remaining Phase A work

These slices do not complete Phase A1 or Phase A. Still to be implemented:

- Remaining A1: `QualifySong`, `CopyPlaylist`, `AutoPurge`, `ImportShare`, change diffs for notices, local sync file (`local.json`) serialization, `AccountFiles::playlists`, and `[playlists]` / `[playback] private_listening_p` settings readers.
- Later Phase A slices: A2 guarded gzip codec (`purple_playlists_codec`), A3 sync step and chat-move planner (`purple_playlists_sync`), A4 share file codec and import planner (`purple_playlists_share`), and A5 README overview.

## Verification

`tests/test_playlists.cpp` exercises the 4 MiB legacy cap alongside uncapped >4 MiB local library parsing, section 7.2 golden canonical round-trips, lexicographical song key order, partial playlist absence preservation, `self` peer normalization, `NewerFormat` handling, strict validation boundaries (`2^52`, `2^64 - 1`, code-point limits, duplicate/invalid Unicode, `alt` distinctness, `purged` disjointness), equal-stamp tie-breaking, purged remove-wins `Contains`, and 200 seeded commutative/associative/idempotent join property checks.

`tests/test_playlists_ops.cpp` exercises `FormatPlaylistId`, `CreatePlaylist` collision and untouched-vs-conflict `UndoCreate`, lifecycle transitions and no-op/deleted/incomplete refusals, batch `AddPlaylistSongs` order/dedup/no-op/relink/undo, `RemovePlaylistSongs`, `ReorderPlaylistSongs`, concurrent merge + undo cases, `2^52` stamp allocation and saturation, the 24 MiB local edit cap and oversized downward edits, and `PlaylistVisibleSongIds` / `ListVisiblePlaylists` ordering.

Run only this slice with `python3 tests/run_playlists.py`. It uses Qt6Core from
`pkg-config`, or a macOS framework selected with `--qt-prefix` (also `QT_PREFIX`).
On a shared build machine, pass the compiler hold wrapper using
`--compile-wrapper /path/to/wrapper`; each compile and link uses the wrapper,
while the test executable runs after the compiler hold is released. Outputs go
to `tests/build/playlists`, or the path selected by `--build-dir`.
