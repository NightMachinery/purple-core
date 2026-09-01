#!/usr/bin/env bash
# Compiles and runs the standalone tests for the Purple config core.
#
# The core depends on nothing but Qt Core, the vendored toml++ and the standard
# library, which is the whole reason it is a repository of its own: settings.toml
# is hand-owned, the splicer's job is to leave the user's comments alone, and
# proving that takes a few hundred fixture edits - far too slow to iterate on
# through a full app build.
#
# Qt Core is the one thing this cannot vendor. On macOS it is looked for as a
# framework under QT_PREFIX, defaulting to the merged prefix the tdesktop fork
# builds against; point QT_PREFIX elsewhere for another checkout:
#
#     QT_PREFIX=/path/to/qt tests/run.sh
set -e

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
QtPrefix="${QT_PREFIX:-$HOME/code/misc/tdesktop-libs/local/qt}"
BuildPath="$RepoPath/tests/build"

if [ ! -d "$QtPrefix/frameworks/QtCore.framework" ]; then
    echo "No QtCore.framework under $QtPrefix." >&2
    echo "Set QT_PREFIX to a Qt 6 prefix holding frameworks/QtCore.framework." >&2
    exit 1
fi

mkdir -p "$BuildPath"
Output="$BuildPath/test_config"

clang++ -std=c++20 -g -O0 -o "$Output" \
    "$RepoPath/tests/test_config.cpp" \
    "$RepoPath/purple/purple_settings.cpp" \
    "$RepoPath/purple/purple_splice.cpp" \
    "$RepoPath/purple/purple_state.cpp" \
    "$RepoPath/purple/purple_engine.cpp" \
    -I"$RepoPath" \
    -I"$RepoPath/tomlplusplus" \
    -I"$QtPrefix/frameworks/QtCore.framework/Headers" \
    -F"$QtPrefix/frameworks" \
    -framework QtCore \
    -Wl,-rpath,"$QtPrefix/frameworks"

"$Output"
