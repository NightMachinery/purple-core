# purple-core

The Work Mode core shared by the Purple Telegram apps: the `settings.toml`
parser, the splice writer that edits that file in place, the `state.toml`
serializer, the resolution engine that turns a preset into a decision about one
chat, and the screen-time log with everything derived from it.

It is a repository of its own because two apps need exactly this code and
nothing around it. Every policy in the Work Mode spec is a rule about data, and
rules about data are far easier to prove outside a running app than inside one.

## What is here

- `purple/purple_settings.{h,cpp}` - parses `settings.toml` into presets, lists,
  folders and rules, collecting warnings as it goes.
- `purple/purple_splice.{h,cpp}` - the only writes the apps ever make to
  `settings.toml`. It does not re-serialise the document: it locates the array
  it must touch through toml++'s source regions and edits the raw lines, so
  every byte the user wrote stays where they put it, comments included.
- `purple/purple_state.{h,cpp}` - reads and writes `state.toml`, the
  machine-owned half of the configuration. Rewritten whenever it changes, which
  is why it is a separate file: it must never touch the mtime of the
  `settings.toml` you are editing by hand.
- `purple/purple_engine.{h,cpp}` - resolves a preset into a flat table of "for
  this list, show and notify are these", and answers what that means for one
  chat. Resolution runs once per config or preset change, never per repaint.
- `purple/purple_screentime.{h,cpp}` - `screentime.log` and everything derived
  from it: the line format and its tolerant parser, session derivation, active
  time, the buckets, the heat map, the period comparison, the budget ledger and
  retention. The log is raw events and nothing else, so every threshold in
  `[screen_time]` is applied at read time and changing one re-derives the
  history you already have. It also holds `FormatSpan()`, the one piece of
  wording in the core: both apps had written "6 h 12 m" for themselves and the
  two spellings had already drifted apart.
- `purple/purple_types.h` - the two type aliases and the `_q` string literal the
  core borrowed from tdesktop's `base/basic_types.h` before extraction. It
  defers to that header when compiled inside tdesktop and defines them itself
  otherwise.
- `tomlplusplus/toml.hpp` - vendored toml++, verbatim and unmodified (MIT).

## Consumers

- Purple Telegram desktop, the tdesktop fork, as a submodule at
  `Telegram/ThirdParty/purple_core`.
- A future Android fork, built through the NDK.

Both put the repository root on their include path, so
`#include "purple/purple_settings.h"` resolves the same way in either.

## Design contract

- **No dependency beyond Qt Core, the vendored toml++, and the C++20 standard
  library.** No Qt Gui, no Qt Widgets, no tdesktop, no Android. Anything that
  needs a window, a network, or a platform belongs in the consuming app.
- **Warn, don't fail.** `settings.toml` is hand-owned, so nearly everything
  recoverable is a warning attached to the parse result rather than an error
  that loses the file. A typo degrades to "keep the last good settings and show
  a banner", never to an exception crossing an event handler. The splicer is the
  strict half: if it cannot find the exact line and column it must edit, it
  refuses to write at all rather than guess.
- **Every boolean key ends in `_p`.** A naming convention, enforced by the
  parser, so a key's type is readable from the key.
- **The `version` key** in `settings.toml` names the schema the file speaks. It
  moves only when a key changes *meaning*, because that is the only change an
  older build gets wrong rather than merely misses. It is not a build number and
  not a date.

The full schema reference lives with the desktop app, at `docs/purple/config.md`
in the tdesktop fork.

## Tests

`tests/test_config.cpp` compiles the core's translation units into a small
harness and drives them against several hundred fixture documents - far too
slow to iterate on through a full app build, which is the point.

```sh
QT_PREFIX=/path/to/qt tests/run.sh
```

`QT_PREFIX` must hold `frameworks/QtCore.framework`; it defaults to
`$HOME/code/misc/tdesktop-libs/local/qt`, the prefix the desktop fork builds
against. The build lands in the gitignored `tests/build/`. The script exits
nonzero if anything fails to compile or any check fails.

## History

This code was extracted from the Purple Telegram desktop fork. Everything before
the extraction commit lives in that repository's history, under
`Telegram/SourceFiles/purple/`.

## License

GPL-2.0-or-later. See `LICENSE` for the version 2 text; the "or any later
version" grant is stated in each source file's header, which is where the
version 3 desktop fork gets its right to link this in.

`tomlplusplus/toml.hpp` is third-party and stays under its own MIT license.
