# Shared retained-download policy

The Desktop inventory codec and bounded job policy now live in shared core.
Desktop keeps its existing local.json member and forwarding includes. Android
uses a separate android_playlist_downloads member, so a cross-platform local
state import cannot adopt another client's filesystem identity. Unknown root,
file and intent fields are preserved; malformed/newer state refuses rewrites.
Both clients use the same two-active scheduler, explicit Retry and cancellation
generation fence. Invalid terminal phase values are refused.

The shared policy performs no filesystem or network operations. Client adapters
must prove durable intent publication, owned transfer, identity and digest checks,
registration, cancellation and restart recovery. `tests/test_playlist_downloads.cpp`
covers the codec, client isolation, job bound and cleanup transitions. The Android
host roster also exercises this fixture through its download-policy driver.


Cleanup intent shares the client-local inventory, never synced library data.
The optional cleanup object has format1, a boolean prune flag and a sorted unique
playlist-id array. Unknown keys survive transitions; malformed/newer cleanup
refuses inventory mutation. MarkPrune/ClearPrune and MarkRemoval/ClearRemoval are
typed transitions, preserving the other client's inventory and unrelated fields.
This records a pending operation, not authority to retry automatically. Shared
reference policy also selects inactive transfer intents, protecting all unpurged
references during pruning. Native identities and durable absence are required
before forgetting a reservation; a published-but-uncommitted file is reconciled
before shared reference removal decides its fate.


`PlaylistDownloadObsoleteCleanupTargets` selects only ids in the shared purge
map. Absence from the loaded playlist map is insufficient. Global cleanup exposes
these markers as pending without retargeting live selections. A complete confirmed
prune clears the obsolete ids through existing typed transitions while preserving
unknown fields. Partial/reference-incomplete libraries and remaining eligible
files/intents refuse final normalization. No automatic cleanup authority is added.
