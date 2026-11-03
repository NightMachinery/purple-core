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
   value 0, no final xor, computed with the page's CRC field as zero);
2. stop at the first page that is not good. Nothing after it is kept, even if a later page would be good: the
   walk never resynchronises, so a corrupt middle page cannot splice two halves of a recording;
3. cut the stream after the last good page;
4. set the end-of-stream flag (`0x04`) on that page and recompute its CRC. The file variant rewrites only that
   page's 27-byte header and then truncates the file;
5. read the duration: for Opus, `(granule - pre_skip) / 48` milliseconds, at least 0, from the last good page
   that has a granule position (a page on which no packet ends has granule -1). `pre_skip` is read from the
   OpusHead. A stream that is not Opus walks the same way and reports duration -1.

Statuses: `Clean` (every byte is a good page and the last one ends the stream; nothing changes), `Finalised`,
and `Unrecoverable` (no good page, or only the Opus header pages, so no audio). An unrecoverable stream is left
byte for byte as it was, so the bin can keep it raw. `FinaliseOggFile` also sets `ioError` when it cannot read or
write the file.

A last kept page whose final packet continues onto the lost next page (its lacing ends in 255) is kept as it is:
decoders drop the unfinished packet at the end of the stream. The recorders write one packet per page, so this is
rare.

`ScanOgg(data, size)` is the walker on its own. Its `pages` give each page's offset, size, flags, granule,
serial and sequence; the orphan scan uses `endOfStream` to find unfinished files, and Desktop's preview trim cuts
on these page boundaries.
