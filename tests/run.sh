#!/usr/bin/env bash
# Compiles and runs the standalone tests for the Purple config core.
#
# The core depends on nothing but Qt Core, the vendored toml++ and the standard
# library, which is the whole reason it is a repository of its own: settings.toml
# is hand-owned, the splicer's job is to leave the user's comments alone, and
# proving that takes a few hundred fixture edits - far too slow to iterate on
# through a full app build.
#
# Sixteen programs run: config, settings flow, sync-chat decisions, the magic sync chat, carry recovery,
# archive transitions/codecs/source, automatic settings policy, storage, staging, playback, the Ogg
# finaliser, the round video journal and repair, the recording rules, and a seeded fuzz loop over
# the three recording parsers (a few seconds).
# All link against the same core objects. ARCHIVE_SQLITE_TESTS=1 additionally
# executes the shared SQL against the system SQLite library.
#
# Qt Core is the one thing this cannot vendor. On macOS it is looked for as a
# framework under QT_PREFIX, defaulting to the merged prefix the tdesktop fork
# builds against; point QT_PREFIX elsewhere for another checkout:
#
#     QT_PREFIX=/path/to/qt tests/run.sh
#
# Without that framework (Linux, or a Qt that is not a framework build), Qt
# Core is taken from pkg-config's Qt6Core when it is installed, for example
# from a distribution's qt6-base-dev package:
#
#     tests/run.sh
set -e

RepoPath="$(cd "$(dirname "$0")/.." && pwd)"
QtPrefix="${QT_PREFIX:-$HOME/code/misc/tdesktop-libs/local/qt}"
BuildPath="$RepoPath/tests/build"

if [ -d "$QtPrefix/frameworks/QtCore.framework" ]; then
    QtFlags=(
        -I"$QtPrefix/frameworks/QtCore.framework/Headers"
        -F"$QtPrefix/frameworks"
    )
    QtLinkFlags=(
        -F"$QtPrefix/frameworks"
        -framework QtCore
        -Wl,-rpath,"$QtPrefix/frameworks"
    )
elif pkg-config --exists Qt6Core 2>/dev/null; then
    read -r -a QtFlags <<< "$(pkg-config --cflags Qt6Core) -fPIC"
    read -r -a QtLinkFlags <<< "$(pkg-config --libs Qt6Core)"
else
    echo "No QtCore.framework under $QtPrefix, and no Qt6Core in pkg-config." >&2
    echo "Set QT_PREFIX to a Qt 6 prefix holding frameworks/QtCore.framework," >&2
    echo "or install Qt 6 Core so that pkg-config finds Qt6Core." >&2
    exit 1
fi

Sources=(
    purple/purple_sync_auto.cpp
    purple/purple_sync_auto_state.cpp
    purple/purple_peer_token.cpp
    purple/purple_sync_chat.cpp
    purple/purple_sync_chat_magic.cpp
    purple/purple_archive_envelope.cpp
    purple/purple_archive_sql.cpp
    purple/purple_archive_sql_body.cpp
    purple/purple_archive_sql_maintenance.cpp
    purple/purple_archive_staging.cpp
    purple/purple_archive_media_lifecycle.cpp
    purple/purple_archive_views.cpp
    purple/purple_archive_lifecycle.cpp
    purple/purple_archive_source.cpp
    purple/purple_archive_model.cpp
    purple/purple_archive_store.cpp
    purple/purple_archive_codec.cpp
    purple/purple_sync_chat_carry.cpp
    purple/purple_sync_chat_carry_codec.cpp
    purple/purple_sync_chat_journal.cpp
    purple/purple_account_files.cpp
    purple/purple_local_choice.cpp
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
    purple/purple_playback.cpp
    purple/purple_recording_file.cpp
    purple/purple_ogg_repair.cpp
    purple/purple_round_journal.cpp
    purple/purple_mp4_repair.cpp
    purple/purple_recording.cpp
)
Flags=(
    -std=c++20 -g -O0
    -I"$RepoPath"
    -I"$RepoPath/tomlplusplus"
    "${QtFlags[@]}"
)
LinkFlags=(
    "${QtLinkFlags[@]}"
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
CompileStatus=0
for Pid in "${Pids[@]}"; do
    wait "$Pid" || CompileStatus=1
done
if [ "$CompileStatus" != 0 ]; then
    exit "$CompileStatus"
fi

Status=0
for Test in test_config test_sync_flow test_sync_chat test_sync_chat_magic test_sync_chat_carry test_archive test_archive_codec test_archive_source test_sync_auto test_archive_storage test_archive_staging test_playback test_ogg_repair test_mp4_repair test_recording test_recording_fuzz; do
    clang++ "${Flags[@]}" -o "$BuildPath/$Test" \
        "$RepoPath/tests/$Test.cpp" \
        "${Objects[@]}" \
        "${LinkFlags[@]}"
    echo "$Test:"
    "$BuildPath/$Test" || Status=1
done
if [ "${ARCHIVE_SQLITE_TESTS:-0}" = 1 ]; then
    clang++ "${Flags[@]}" -o "$BuildPath/test_archive_sqlite" \
        "$RepoPath/tests/test_archive_sqlite.cpp" \
        "${Objects[@]}" "${LinkFlags[@]}" -lsqlite3
    echo "test_archive_sqlite:"
    "$BuildPath/test_archive_sqlite" || Status=1
fi
exit "$Status"
