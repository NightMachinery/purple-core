#!/usr/bin/env bash
# Compiles and runs the standalone tests for the Purple config core.
#
# The core depends on nothing but Qt Core, the vendored toml++ and the standard
# library, which is the whole reason it is a repository of its own: settings.toml
# is hand-owned, the splicer's job is to leave the user's comments alone, and
# proving that takes a few hundred fixture edits - far too slow to iterate on
# through a full app build.
#
# Two programs run: test_config covers the parser, splicer, engine and sync
# records; test_sync_flow covers the settings sync flow built on them. The
# core sources compile once and both link against the same objects.
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

Sources=(
    purple/purple_settings.cpp
    purple/purple_splice.cpp
    purple/purple_state.cpp
    purple/purple_config_sync.cpp
    purple/purple_config_diff.cpp
    purple/purple_config_payload.cpp
    purple/purple_sync_json.cpp
    purple/purple_sync_envelope.cpp
    purple/purple_sync_directory.cpp
    purple/purple_sync_inventory.cpp
    purple/purple_sync_config_flow.cpp
    purple/purple_sync_config_describe.cpp
    purple/purple_sync_local_state.cpp
    purple/purple_sync_status.cpp
    purple/purple_engine.cpp
    purple/purple_passcode.cpp
    purple/purple_screentime.cpp
)
Flags=(
    -std=c++20 -g -O0
    -I"$RepoPath"
    -I"$RepoPath/tomlplusplus"
    -I"$QtPrefix/frameworks/QtCore.framework/Headers"
    -F"$QtPrefix/frameworks"
)
LinkFlags=(
    -F"$QtPrefix/frameworks"
    -framework QtCore
    -Wl,-rpath,"$QtPrefix/frameworks"
)

ObjectPath="$BuildPath/objects"
mkdir -p "$ObjectPath"
Objects=()
Pids=()
for Source in "${Sources[@]}"; do
    Object="$ObjectPath/$(basename "$Source" .cpp).o"
    Objects+=("$Object")
    clang++ "${Flags[@]}" -c -o "$Object" "$RepoPath/$Source" &
    Pids+=($!)
done
for Pid in "${Pids[@]}"; do
    wait "$Pid"
done

Status=0
for Test in test_config test_sync_flow; do
    clang++ "${Flags[@]}" -o "$BuildPath/$Test" \
        "$RepoPath/tests/$Test.cpp" \
        "${Objects[@]}" \
        "${LinkFlags[@]}"
    echo "$Test:"
    "$BuildPath/$Test" || Status=1
done
exit "$Status"
