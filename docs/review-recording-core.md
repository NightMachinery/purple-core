# Review: purple-core media recording core (`3e25cb8..recording`)

Reviewed 3 Nov 2026, read-only against branch `recording` at `fb1ceba`. Four commits after the design commit
`3e25cb8`:

- `d7f3785` Read the recording table and the seek step (with the `d` duration suffix)
- `58f305a` Finalise an interrupted Ogg recording
- `5256098` Rebuild a round video's index from its media data (scanner, journal and config header format)
- `fb1ceba` Decide recording nudges and bin expiry

Checked against `docs/media-recording-design.md` (F10, Extras 5, Settings, Tests, and the owner's answers in the
comment at its top, all Option A) and `docs/recording-formats.md`.

**Verdict: CHANGES.** One P1 (an out-of-bounds read reachable from a CRC-valid journal), one P2, seven P3. The
rest of the work is solid: the Ogg finaliser is correct and idempotent, the journal reader fails closed on every
torn or damaged record I tried, the scanner always advances and never reads past the buffer, and the settings
match the design exactly.

## How this was checked

- `tests/run.sh` is macOS-only (it requires `QtCore.framework`). I ran the same source list and the same fifteen
  test programs on Linux against the distribution's Qt 6.4, with clang (what `run.sh` uses). All fifteen pass:
  `test_config` 10941 checks, `test_ogg_repair` 74, `test_mp4_repair` 130, `test_recording` 60, and the eleven
  older programs unchanged, 0 failures.
- The three recording parsers have no Qt dependency, so `test_ogg_repair` and `test_mp4_repair` were also built
  with GCC at `-std=c++17` (the design's language level) under AddressSanitizer and UndefinedBehaviorSanitizer:
  both clean.
- A throwaway fuzz loop (not committed) ran 100,000 iterations each over `FinaliseOgg` and `RecoverRound` under
  ASan and UBSan: valid synthetic streams, then truncation at a random byte, random byte flips, appended junk, pure
  random bytes, flips in the journal, and journals with one sample field corrupted and every CRC recomputed (a
  well-formed but hostile journal). Invariants: Unrecoverable leaves the Ogg bytes unchanged; a finalised stream
  finalises again as Clean with identical bytes and the same duration; every recovered round sample lies inside
  the media buffer; `scanEnd <= mediaSize`. Random 64-bit offsets almost never hit the narrow wraparound window, so
  P1 below was found by reading and then confirmed with a targeted input; targeted extreme timestamps produced
  P2.
- Unrelated to this branch: built with GCC 13 instead of clang, `test_config`, `test_sync_flow` and
  `test_sync_auto` segfault in `SettingsTablePath` (`purple_settings.cpp:2070`, inside
  `QString::fromUtf8`). The same happens at `3e25cb8`, so it predates this work; it is worth its own look,
  because Android builds purple-core with a different toolchain than the macOS tests.

## P1

### P1-1. A journal sample whose `offset + size` wraps past 2^64 is read out of bounds

`purple/purple_mp4_repair.cpp:444-447` (`RecoverRound`) and `purple/purple_round_journal.h` (`RoundSample::end`).

`RecoverRound` accepts a journaled sample when `sample.end() > mediaSize` is false, where `end()` is
`offset + size` in `uint64_t`. Both fields come straight from the journal (`ReadRoundJournal` checks only the
track index). With `offset = 2^64 - 16` and `size = 32`, `end()` wraps to 16, which passes the check on any media
file of at least 16 bytes. `previousEnd` is 0 for the first sample, so the overlap check passes too, and
`MatchesMedia` then reads `media + offset`.

Trace (confirmed under ASan):

    RoundConfig with one video track; EncodeRoundJournalStart(config);
    EncodeRoundJournalBatch({ track 0, size 0x20, offset 0xFFFFFFFFFFFFFFF0 });
    media = 64 bytes starting with a 16-byte ftyp;
    RecoverRound(media, 64, journal, ...)
    -> heap-buffer-overflow in Big32, purple_mp4_repair.cpp:17
       via MatchesMedia (:282) via RecoverRound (:447)

For an audio track `MatchesMedia` returns true without reading, so the wrapped sample lands in
`RoundRecovery::samples` and `exactSamples`. The client then reads the sample's bytes at `offset` to rebuild the
file (Android through `MP4Builder.writeSampleData`, Desktop through FFmpeg), which is the same out-of-bounds read
one layer up, or a Java exception in the recovery thread.

The journal's CRCs do not protect against this: anyone or anything that can write the sidecar can compute them,
and the brief treats hostile files as in scope. A normal crash cannot produce it. It still needs fixing because the
recovery runs unattended at start (the orphan scan and crash recovery), so a bad sidecar becomes a crash loop.

Fix: compare without adding, in `RecoverRound`:

    if (sample.offset > mediaSize
        || sample.size > mediaSize - sample.offset
        || ...)

and consider giving `ReadRoundJournal` the same guard against `UINT64_MAX` (`offset > UINT64_MAX - size` is
damage), so no other caller of `RoundJournal::samples` can meet a wrapping sample. Add the input above as a test
case in `TestJournalDamage`.

## P2

### P2-1. Extreme journal timestamps cause signed integer overflow in the repair timing

`purple/purple_mp4_repair.cpp:254` (`AudioTime`), `:343`, `:369` and `:376` (`RepairRound`).

`ptsUs` is a signed 64-bit field read from the journal without a range check. With a CRC-valid journal whose last
audio sample has `ptsUs` near `INT64_MAX`, `AudioTime(base, 1, rate)` overflows; with a video sample near
`INT64_MIN`, `sample.ptsUs - audioEnd` overflows; `audioEnd + encoderOffsetUs` and `lastVideo + 16667` overflow
the same way. UBSan reports all four from the fuzz loop's targeted case, for example:

    purple_mp4_repair.cpp:254:60: runtime error: signed integer overflow:
        9223372036854775230 + 21333 cannot be represented in type 'long int'

It is undefined behaviour, not a memory error, and in practice it gives wrapped timestamps that the client's muxer
then writes (MP4Builder derives sample durations from the deltas, so a wrap becomes a huge or negative `stts`
entry). Same reachability as P1: a damaged or hostile journal, never a normal crash.

Fix: in `RecoverRound`, stop the exact part at the first sample whose `ptsUs` is outside a sane range (for
example `0 <= ptsUs < 2^40` µs, about 12 days) or goes backwards by more than a frame within its track, and
treat that as staleness like the other drop reasons. Then the arithmetic in `RepairRound` stays in range because
the repaired part only ever adds about 21 ms per frame to an in-range base.

## P3

### P3-1. `RecordingFile::ReadAll` can throw, and truncates the size on 32-bit builds

`purple/purple_recording_file.cpp:38`.

The header says every call "reports failure instead of throwing", but `bytes.resize(std::size_t(size))` throws
`std::bad_alloc` (or `std::length_error`) for a file too large to hold in memory. On Android an exception
escaping into JNI aborts the process. On a 32-bit ABI (armeabi-v7a), `std::size_t(size)` also truncates a file of
4 GiB or more to its size modulo 2^32: the function then reads and "succeeds" with only a prefix, and
`FinaliseOggFile` may truncate the real file to the end of that prefix. Neither is a realistic voice recording,
but the orphan scan reads every unfinished-looking `.ogg` in `MEDIA_DIR_AUDIO`, so cap the size (a voice
recording beyond, say, 512 MB is not ours) and catch allocation failure inside `ReadAll`.

### P3-2. Near the end of the data the lookahead no longer protects against a misread audio frame

`purple/purple_mp4_repair.cpp:214-236` (`tornTail` used inside `boundary`).

`boundary()` accepts a torn tail as a valid boundary after a video candidate, and `tornTail()` is true whenever
8 KB or less remains. So in the last 8 KB of a file the one-step lookahead the design relies on ("misreading
audio bytes as a video length is ruled out by the lookahead") is off: an AAC frame whose first four bytes read
as a small big-endian length followed by a plausible NAL header is accepted as a video sample, and what follows is
lost. The status is still Repaired, not Partial. It needs a first byte of 0 and a second byte below about 0x20,
which is rare for AAC-LC (it means a very low global gain), so this is polish: require that the video candidate
either ends exactly at the end of the data or is followed by another real boundary, and keep the torn-tail rule
only for deciding Partial versus Repaired.

### P3-3. The Ogg walker does not check serial number or page sequence

`purple/purple_ogg_repair.cpp:155-169` (`ScanOgg`).

Every page with a good CRC is kept, whatever its serial and sequence number. If a recording file is ever reused
without truncation (Desktop's `MirrorRecordingWrite` writes at offsets into a per-`Private` file, so a mirror
opened without `O_TRUNC` would do it), stale pages of an earlier recording after the crash point pass the CRC
check and are spliced on, with a granule jump that makes the reported duration wrong. Stopping at the first page
whose serial differs from the first page's, or whose sequence is not the previous plus one, is two comparisons
and matches the finaliser's stated rule ("bytes that are not a page stop the walk").

### P3-4. The end-of-stream flag can land on a page with granule -1

`purple/purple_ogg_repair.cpp:184-191`.

If the last good page completes no packet (granule -1, one packet spanning pages), the finaliser sets EOS on it
while `durationMs` comes from an earlier page. RFC 7845 expects the end-of-stream page to carry the final granule
position for end trimming. `docs/recording-formats.md` already says the recorders write one packet per page, so
this is rare; cutting back to the last page with a granule is a one-line change if you want the output to be
strictly valid.

### P3-5. The recording rules overflow on absurd inputs

`purple/purple_recording.cpp`: `NextNudgeAt` computes `(elapsedMs / interval + 1) * interval`, and
`BinItemExpiresAt` computes `binnedAt + keepSeconds`. Both are signed overflow for values near `INT64_MAX`. A clock
can't reach that, but `binnedAt` will come from `recordings.json`, which can be damaged. Saturate, or treat a
`binnedAt` that far in the future the same way as the existing "item from the future" rule.

### P3-6. To verify on Android: a split `mdat` header that is not yet written

The scanner assumes every `mdat` header between samples is present on disk at the time of the crash. If
`MP4Builder`'s split path reserves the 16 bytes for a new `mdat` and writes them only when it flushes that box
(seeking back), a crash leaves 16 zero bytes there. `box()` does not match those, the video and audio checks fail
on them, and the scan stops: Partial, losing everything after the last flushed header (up to one 32 KB stretch,
about one second, per crash, on every V2 recording since V2 always splits). I could not check this from
purple-core (the Android repository is outside this review's scope); the design cites `MP4Builder.java:147-156`
for the split. If it does reserve, skipping exactly 16 zero bytes when a valid sample follows them is a cheap fix.
Either way, the planned device corpus check (truncate real V2 rounds at random offsets) settles it.

### P3-7. Tests: add the hostile-journal cases and a small fuzz loop

The suites cover the design's list well (truncation at every byte of the last three samples, a torn append at
every byte of the last batch, the lookahead, VFR, journal plus tail, a corrupt middle Ogg page, idempotence on
file). What is missing is a well-formed journal with bad values: a wrapping offset (P1-1), extreme timestamps
(P2-1), a size of 0, an offset inside the `ftyp`. A deterministic fuzz loop like the one used for this review is
cheap: under 100 lines, seeded `mt19937_64`, about 3 seconds for 10,000 iterations each under ASan, with the
invariants listed above. It would have caught P2-1 directly and P1-1 with an extreme-value mutation. Also worth
adding: a test that runs `run.sh`'s list on Linux, or a note in `run.sh` on how to point it at a non-framework Qt,
since today it exits early anywhere but macOS.

## Checked and fine

- **Settings.** Defaults, ranges and spellings match the design: `seek_step` only 5/10/15/30 s (default 10),
  `round_journal` only off/1/2/5 s (default 1), `bin_keep` off or at least 1 h (default 48 h, "7d" reads as
  604800), `keep_discarded_p` default on (owner answer 1), `headset_hold_start_p` default off, every other boolean
  default on. Wrong shapes warn and keep the default. Scope is AppWide, the diff label is "Recording", and the
  scope and sync docs are updated. `long_nudge` also requires at least 1 min, which the design does not state;
  the README documents it, so only the design should be updated to match.
- **ParseDuration.** The `d` suffix is additive. The multiplication now happens in 64 bits and refuses anything
  above `INT_MAX`, which also removes undefined behaviour the old `number * multiplier` had for inputs like
  `"999999h"`. Existing callers see only two changes: values ending in `d` now parse, and values that used to
  overflow now warn and keep the default. `DurationText` in `purple_splice.cpp` never writes `d`, so a spliced
  `bin_keep` of 7 days is written as `"168h"`, which still reads back correctly.
- **Ogg finaliser.** The CRC is the Ogg polynomial, unreflected, zero init, computed with the CRC field zeroed.
  Every read in `ReadPage` is bounds-checked before use (header, lacing table, body), a page's size cannot exceed
  27 + 255 + 255 × 255, and the walk always advances by at least 27 bytes. The pre-skip is read little-endian from
  the OpusHead and the duration is `(granule - pre_skip) / 48`, clamped at 0. A headers-only file is reported
  Unrecoverable and left byte for byte. Running it twice gives Clean the second time (tested on file, and by the
  fuzz invariant). The file variant rewrites only the 27-byte header, then truncates; a crash between the two
  leaves a file that the next run finalises correctly.
- **Journal format.** Magic plus a version and a reserved field; an unknown version is reported, not guessed.
  Every record has its own CRC-32 (the `java.util.zip.CRC32` function, so Java can write it), a length cap of
  16 MB, a batch count cap of 65536 checked before the size multiplication, at most 8 tracks and 4 codec buffers of
  64 KB each. Reading stops at the first torn or bad record and keeps everything before it. Unknown record types
  with a good CRC are skipped, so version 1 can grow. The config header is a separate record from the batches, so
  journal Off (no batches) still recovers through repair from `ftyp`, as owner answer 2 requires.
- **Repair scanner.** Box skips are bounded by the remaining size, every iteration advances or stops, the AAC
  callback is never given more than 8 KB or more than what remains and its result is checked against that, and
  every result allocation is bounded by the media size. Sync flags, the 1/60 s minimum step and the median encoder
  offset follow the design.
- **Nudges and bin expiry.** The nudge fires at each whole multiple of the interval of recorded time (30:00,
  60:00, ...), once per poll however late the poll, never on a poll that goes backwards, and never when off. Bin
  expiry is inclusive at exactly `bin_keep`, never with "off", and never for an item stamped in the future. Seek
  clamps at 0 and one second before the end and never moves a forward seek backwards.

## Re-check 1

Scoped re-check of `fb1ceba..a0a7d40` on `recording` (3 Nov 2026): `3fc50b9` "Harden the recording parsers
against hostile input" and `a0a7d40` "Fuzz the recording parsers in the core suite". Read-only for `recording`.

**Verdict: CHANGES**, for one new P2 that the P3-2 fix introduced. P1-1 and P2-1 are fixed, as are all the
requested P3s (1, 2, 3, 4, 5, 7). I found no new memory-safety issue.

### How this was checked

- `tests/run.sh` now falls back to pkg-config's `Qt6Core`, and it ran unmodified here (Linux, clang, Qt 6.4). All
  sixteen programs pass with 0 failures: `test_ogg_repair` 97 checks, `test_mp4_repair` 166, `test_recording` 70,
  `test_recording_fuzz` 5, and the twelve older programs with the same counts as before. The fuzz program's 5
  checks are each one invariant accumulated over all 3000 iterations, not 5 inputs.
- Under ASan and UBSan with `-fno-sanitize-recover=all` (GCC, C++17 for the Qt-free parsers, C++20 with Qt for
  `test_recording`): `test_ogg_repair`, `test_mp4_repair`, `test_recording` and the in-tree
  `test_recording_fuzz` are all clean.
- My fuzz loop from the first round ran 100,000 iterations over each parser, clean. Its extreme-value variant (offsets in
  `[2^64 - 64, 2^64)` and timestamps within 1000 of `INT64_MAX` or `INT64_MIN`, every CRC recomputed) ran
  50,000 iterations, clean: before the fix it hit the overflow at once, and UBSan reported the four overflow sites.
- The P1-1 repro from the first round now returns Unrecoverable with no read. The same wrapping sample passed straight to the
  public `RepairRound`, which skips the journal reader, is dropped (`journalDropped = 1`) with no read.

### Original findings

- **P1-1, fixed.** The bounds check no longer adds: `sample.offset > mediaSize || sample.size > mediaSize -
  sample.offset` (`UsableExact`). The check lives in `RepairRound`, so the public entry point is protected
  too, not only `RecoverRound`. `ReadRoundJournal` also rejects any sample with `offset > UINT64_MAX - size` as
  damage. A sample starting inside `ftyp`, an empty one and one naming an unknown track are dropped as well.
- **P2-1, fixed.** Journaled times outside `[0, 2^40)` µs, or going back more than one frame within a track,
  end the exact part. `AudioTime` is split into whole seconds and remainder, so even a hostile 32-bit sample rate
  stays below about `4.4 * 10^18` in the intermediate product. With every base below `2^40`, the remaining sums
  in `RepairRound` cannot overflow.
- **P3-1, fixed.** `ReadAll` refuses files above 512 MB, checks the size against `size_t` before the cast, and
  catches every exception, as do `WriteAt` and `Truncate`. `FinaliseOggFile` reports a file it refuses as
  `ioError` and leaves it alone.
- **P3-2, fixed, but see N1.** Inside `boundary()` a short tail no longer counts; only a torn video sample does.
- **P3-3, fixed.** The walk ends at a page with another serial number or a sequence that is not the previous one plus one.
- **P3-4, fixed.** Trailing pages with granule -1 are cut before the flag is set. `hasAudio()` guarantees a page with a granule remains,
  so the loop cannot empty the vector.
- **P3-5, fixed.** `NextNudgeAt` returns nothing when no later multiple fits. `BinItemExpiresAt` and forward
  `SeekTarget` saturate.
- **P3-6, documented, not changed.** `docs/recording-formats.md` now describes the unwritten split `mdat`
  header case and its Partial result. That is acceptable until the device corpus check settles it.
- **P3-7, fixed.** `tests/test_recording_fuzz.cpp` is in `run.sh`. It is seeded, includes hostile CRC-valid journals with
  the extreme values that found P1-1 and P2-1, and checks bounds, timing range, counts and Ogg idempotence.

### New findings

#### N1 (P2). A crash that tears the audio frame after the last video frame drops that frame, and a large one makes the result Partial

`purple/purple_mp4_repair.cpp`, `Scanner::boundary` and the status at the end of `RepairRound`.

With the P3-2 change, a whole video sample followed only by a torn audio frame has no valid boundary after
it. It is dropped, and the scan stops at its start. The author accepted that trade-off and documented it. The
status was not adjusted, though: it is decided by `tornTail(position)`, and `position` is now the start of the
whole video sample. When that sample plus the torn bytes come to more than 8 KB, the status becomes **Partial**,
so the bin shows "Partly recovered" for an ordinary crash. A P-frame at round bitrates is around 4 KB and a
keyframe is larger, so the over-8 KB case is mainly keyframes.

Concrete input (stub measure from the suite: `0x21`, then half the frame size). The layout is `ftyp`, `mdat`, IDR 5000 B,
two audio frames of 300 B, a P-frame of N bytes, then 50 bytes of a 300 B audio frame:

    N = 4000:   before a0a7d40: Repaired, 4 samples, 50 B unparsed
                now:            Repaired, 3 samples, 4050 B unparsed (last frame lost)
    N = 12000:  before a0a7d40: Repaired, 4 samples, 50 B unparsed
                now:            Partial,  3 samples, 12050 B unparsed (last frame lost, wrong label)

A crash lands in an audio frame roughly in proportion to the audio share of the bytes (about 6% at 1 Mbit/s video
and 64 kbit/s audio), and only the frame just after a video sample counts, so this affects a few percent of
crashes, not most. When it happens, the user loses about 33 ms of video and sees "Partly recovered".

Fix, smallest first:

1. Status only: when the scan stops at a position where `video(position).size` is non-zero and
   `tornTail(position + video.size)` holds, treat the stop as a torn tail, not Partial. That restores the label;
   the frame stays dropped as documented.
2. To keep the frame too: accept a video candidate followed by a short tail when the tail starts with a
   plausible audio frame start that the measure rejects only for lack of bytes. This needs the measure to tell
   "too short" apart from "not AAC", which the callback contract does not express today, so (1) is the
   proportionate change.

Add the N = 12000 case to `TestStops`.

#### Notes (no severity)

- `UsableExact` ends the exact part at a video time that goes back more than 16.7 ms. An encoder that emits
  B-frames writes samples in decode order, so its times go back routinely, and such a journal would then be used
  only up to its first B-frame, with repair doing the rest. Android's MediaCodec emits no B-frames unless asked to,
  and Desktop rounds use the fragmented copy rather than this journal, so this is a note for the corpus check,
  not a defect.
- `test_recording_fuzz` reports one aggregated check per invariant, so a failure does not say which iteration
  failed. Printing the iteration number and the mutation mode on the first failure would make a seeded failure
  quicker to reproduce.
- The GCC segfault in `SettingsTablePath` noted in the first round is unchanged and predates this branch. `run.sh`
  uses clang, where it does not occur.
