# Recording formats and repairs

The pure code behind F10 of [the media recording design](media-recording-design.md): what purple-core reads
and writes so a voice or round video recording survives a crash. Everything here is C++17 without Qt, so Android
compiles it into its JNI library and Desktop compiles it directly. The clients own the files, the threads, the
registry and the bin; purple-core owns the byte formats and the decisions.

## Interrupted Ogg voice recordings

`purple/purple_ogg_repair.h`.

A voice recording is an Ogg/Opus stream (OpusHead page with the begin-of-stream flag, OpusTags, then audio
pages). A crash leaves it with a torn last page and no end-of-stream flag. `FinaliseOgg(bytes)` and
`FinaliseOggFile(path)`:

1. walk the pages from offset 0. A page is good when it starts with `OggS` and version 0, its lacing table and
   body fit in the file, and its CRC matches (the Ogg CRC-32: polynomial `0x04C11DB7`, not reflected, initial
   value 0, no final xor, computed with the page's CRC field as zero), and it continues the stream: the same
   serial number as the first page and the next sequence number. A good page of another stream or out of
   sequence (stale pages of an earlier recording left in a reused file) is not good;
2. stop at the first page that is not good. Nothing after it is kept, even if a later page would be good: the
   walk never resynchronises, so a corrupt middle page cannot splice two halves of a recording;
3. cut the stream after the last good page that carries a granule position: trailing pages on which no packet
   ends (granule -1) go too, because the end-of-stream page must carry the final granule position (RFC 7845
   end trimming);
4. set the end-of-stream flag (`0x04`) on that page and recompute its CRC. The file variant rewrites only that
   page's 27-byte header and then truncates the file;
5. read the duration: for Opus, `(granule - pre_skip) / 48` milliseconds, at least 0, from the last good page
   that has a granule position (a page on which no packet ends has granule -1). `pre_skip` is read from the
   OpusHead. A stream that is not Opus walks the same way and reports duration -1.

Statuses: `Clean` (every byte is a good page and the last one ends the stream; nothing changes), `Finalised`,
and `Unrecoverable` (no good page, or only the Opus header pages, so no audio). An unrecoverable stream is left
byte for byte as it was, so the bin can keep it raw. `FinaliseOggFile` also sets `ioError` when it cannot read or
write the file.

A last kept page that completes some packets but whose final packet continues onto the lost next page (its
lacing ends in 255) is kept as it is: it has a granule position, and decoders drop the unfinished packet at the
end of the stream. The recorders write one packet per page, so this is rare.

`FinaliseOggFile` reads the file whole and refuses one above `RecordingFile::kMaxBytes` (512 MB; no voice
recording is that large), so a 32-bit build never mistakes a prefix for the file. None of the file operations
throw.

`ScanOgg(data, size)` is the walker on its own. Its `pages` give each page's offset, size, flags, granule,
serial and sequence; the orphan scan uses `endOfStream` to find unfinished files, and Desktop's preview trim cuts
on these page boundaries.

## The round video journal (`.pjr`)

`purple/purple_round_journal.h`.

The design describes the journal as a config header followed by CRC-checked batches. Here the header and each
track are framed as records with their own CRC like the batches, so a track the muxer learns after the first
samples can still be declared, and a damaged header is told apart from a missing one.

The sidecar a client writes next to a recording round video, `<media file>.pjr`. The media file is unchanged: the
recorder still writes upstream's `MP4Builder` layout (Desktop: its FFmpeg MP4). The journal is append-only and
little-endian. Android writes it from Java, so every field is fixed size and every CRC is the standard CRC-32
that `java.util.zip.CRC32` computes (zlib: reflected polynomial `0xEDB88320`, initial value and final xor
`0xFFFFFFFF`); `RoundJournalCrc` is the same function in C++.

### File header (8 bytes)

    offset size field
    0      4    magic "PJR1"
    4      2    version, 1
    6      2    reserved, 0

A reader that finds another version reports `UnknownVersion` and reads nothing else; the bin then lists the
recording as unrecoverable and keeps it raw.

### Records

Every record is framed the same way, so a torn append or a flipped byte is caught by the record it hits:

    offset size field
    0      1    type: 1 header, 2 track, 3 batch
    1      3    reserved, 0
    4      4    payload length N (at most 16 MB)
    8      N    payload
    8+N    4    CRC-32 of bytes 0 .. 8+N-1 (the 8-byte record head and the payload)

The reader stops at the first record that is not whole, whose CRC fails, or whose payload is invalid, and keeps
everything before it (`goodBytes`, `damaged`). A record of an unknown type with a good CRC is skipped, so later
additions within version 1 stay readable.

**Header record** (type 1, 16 bytes), always the first record:

    0  2  flags: bit 0 split mdat, bit 1 AVCC video framing
    2  2  rotation in degrees (0, 90, 180, 270)
    4  4  video width
    8  4  video height
    12 4  reserved, 0

AVCC framing is for Desktop's FFmpeg muxer (each NAL unit has its own 4-byte length). Without it, a video sample
is MP4Builder's: one 4-byte big-endian length, `size - 4`, over the whole sample.

**Track record** (type 2), one per track, in the muxer's track order (the index `Mp4Movie.addTrack` returns):

    0  1  index: the number of tracks declared before this one
    1  1  kind: 1 video, 2 audio
    2  1  MIME type length M
    3  1  codec configuration buffer count C (at most 4)
    4  M  MIME type, ASCII ("video/avc", "audio/mp4a-latm")
       4  timescale
       4  video width, or audio sample rate
       4  video height, or audio channel count
    then C times:
       4  buffer length L (at most 64 KB)
       L  the buffer: csd-0, csd-1, ... as MediaFormat holds them (H.264 SPS and PPS, the AAC AudioSpecificConfig)

**The config header** is the header record and the track records. It is written once at the start of every round
recording, **also when the `round_journal` setting is Off**: repair needs it to decode audio, and it costs a few
hundred bytes. A track may be declared after the header and after earlier batches (the muxer learns its tracks one
by one), but always before the first sample that names it; a batch that names an undeclared track is damage.

**Batch record** (type 3), appended every `round_journal` interval from the muxer's own thread:

    0  4  sample count K (1 to 65536)
    4  24 × K sample records:
       0  1  track index
       1  1  flags: bit 0 sync sample
       2  2  reserved, 0
       4  4  sample size in bytes
       8  8  offset of the sample in the media file
       16 8  presentation time in microseconds (signed)

The payload length must be exactly `4 + 24 K`. A sample's offset and size are the ones `Mp4Movie.addSample`
receives: the data offset MP4Builder had when it wrote the sample (after any new `mdat` header), and the buffer
size including a video sample's 4-byte length. Samples are in the order they were written.

At about 77 samples a second a batch costs 16 + 24 × 77 bytes, about 1.9 KB per second, or about 110 KB for a
60 s round.

## Recovering a round video

`purple/purple_mp4_repair.h`. `RecoverRound(media, mediaSize, journal, journalSize, measure)` returns a
`RoundRecovery`: the config, the sample list to rebuild, and a status.

1. **Read the journal.** Without one, without a config header, or without tracks, the status is `Unrecoverable`
   (`NoConfig`); a journal of an unknown version is `UnknownVersion`.
2. **Check the exact part.** A journal is checked by CRC, not trusted: anything that can write the sidecar can
   write good CRCs. The reader already treats a sample whose `offset + size` does not fit 64 bits as damage.
   Journaled samples are then used in order until the first one that is empty, names an unknown track, does
   not lie inside the media file (`offset <= size` and `size <= mediaSize - offset`, compared without adding),
   starts inside `ftyp` or overlaps the previous sample, has a time outside `[0, 2^40)` µs (`kRoundMaxPtsUs`,
   about 12.7 days), goes back more than one frame within its track (an audio frame, or 1/60 s for video),
   or, for video, no longer matches the bytes at its offset (the 4-byte length is not `size - 4`, or AVCC's
   first NAL does not fit, or the NAL header is not valid: the file was rewritten). That sample and the rest
   are counted in `journalDropped`. `RepairRound` applies the same checks to the samples it is given, so the
   timing arithmetic below always starts from an in-range base and cannot overflow.
3. **Scan the rest** (`RepairRound`), starting at the end of the last exact sample, or after `ftyp` (no `ftyp`
   and no exact sample: `NotMp4`). At each sample boundary, in order:
   - `moov`: the file was finished; the scan ends there;
   - an `mdat` header is skipped: 16 bytes when its size field is 1 (64-bit size) or when its next 8 bytes are
     zero (MP4Builder always writes 16 bytes), otherwise 8 bytes (FFmpeg); `free`, `skip` and `wide` boxes are
     skipped by their size;
   - a video sample: a 4-byte big-endian length L with `1 <= L`, `L + 4` within the data and below 1 MB, and a
     NAL header with the forbidden bit clear and type 1, 5, 6, 7, 8 or 9, with H.264's nal_ref_idc
     rules (never 0 for 5, 7 and 8; always 0 for 6 and 9), and for a slice (1 or 5) `first_mb_in_slice` equal
     to 0 (the first bit of the slice header set). In AVCC framing an access unit is non-VCL units followed by
     a picture's slices; further slices of the picture (`first_mb_in_slice` above 0) continue it. A video sample
     is accepted only if a valid boundary follows it: the end of the data, a box, another video sample, a
     measurable AAC frame, or a torn video sample (a plausible length and NAL header running past the end).
     A short tail alone is not a boundary, so the lookahead holds in the last 8 KB too; the price is that a
     last video sample followed only by a torn audio frame or a torn `mdat` header is dropped with it;
   - an AAC frame: `measure(audioTrack, data, available)` returns the bytes one 1024-sample frame takes,
     decoding with the track's AudioSpecificConfig; `available` is at most 8 KB and the callback must not read
     past it.

   The scan stops at the first position where nothing fits.
4. **Timing.** Audio is exact: frame n is at `n × 1024 / sample rate` after the last journaled audio frame (or
   from 0). A video sample is at the end of the audio written before it, plus the encoder offset, and at least
   1/60 s after the previous video sample. The encoder offset is the median, over the journaled video samples,
   of their time minus the end of the audio written before them, or 0 without a journal. A video sample is a
   sync sample when its picture is IDR (type 5) or, in MP4Builder framing, when it starts with an SPS. Audio
   samples are all sync samples.
5. **Status.** `Unrecoverable` (`NoVideoTrack`, `NoSamples`) when there is nothing to rebuild. `Partial` when
   the scan stopped before the end and what is left is more than a torn last sample: a torn tail is at most
   8 KB (the largest AAC frame, which also covers a torn `mdat` header) or a video length that runs past the end
   of the data. Otherwise `Exact` when every sample came from the journal, `Repaired` when some came from the
   scan.

The client rebuilds the file from `samples` with its own muxer, reading each sample's bytes at its `offset`:
Android through `MP4Builder` (`createMovie` with the config's split flag, `addTrack` with a `MediaFormat` from
the track record, `writeSampleData(track, bytes, info, false)` per sample, which reproduces a video sample's
length prefix exactly, then `finishMovie`); Desktop through FFmpeg. The result has the layout upstream sends.

### To verify on Android: a split `mdat` header not yet on disk

The scanner assumes every `mdat` header between samples is on disk at the time of the crash. For the Android
lane to confirm against `MP4Builder` (the split path the design cites at `MP4Builder.java:147-156`): if a new
`mdat`'s 16 bytes are only reserved when it starts and written when that box is flushed (a seek back), a crash
leaves 16 zero bytes in their place. `box()` does not recognise those, nothing else matches them, and the scan
stops there: Partial, losing everything after the last written header (up to one 32 KB stretch, about one
second, per crash, on every V2 recording, since V2 always splits). If that is what happens, the fix in
purple-core is small: skip exactly 16 zero bytes when a valid sample follows them. Check the source first (does
`writeSampleData` write the new header in place before its first sample, or only reserve it?), then confirm
with the planned device corpus check (truncating real V2 rounds at random offsets).

How the two sources combine: journal on and fresh gives the exact part plus a repaired tail of at most one
interval, timed with the learned encoder offset; a stale journal (a gap, a CRC failure, a rewritten file) gives
its exact part plus repair from there; Off gives repair from the start; no config header is unrecoverable, and
the bin keeps the raw file.
