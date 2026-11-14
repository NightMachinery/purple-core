#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import shlex
import subprocess


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

CORE_SOURCES = (
    "purple/purple_peer_token.cpp",
    "purple/purple_sync_json.cpp",
    "purple/purple_playlists.cpp",
    "purple/purple_playlists_local.cpp",
    "purple/purple_playlists_codec.cpp",
    "purple/purple_playlists_sync.cpp",
)

TEST_SOURCE = "tests/test_playlists_records.cpp"
EXTRACT_START = "// EXTRACT_SYNC_INVENTORY_START"
EXTRACT_END = "// EXTRACT_SYNC_INVENTORY_END"
INVENTORY_HEADER = "purple/purple_sync_inventory.h"
INVENTORY_SOURCE = "purple/purple_sync_inventory.cpp"


def sha256_file(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_extracted_settings_predicate() -> None:
    inv_h = REPO_ROOT / INVENTORY_HEADER
    inv_cpp = REPO_ROOT / INVENTORY_SOURCE
    test_cpp = REPO_ROOT / TEST_SOURCE

    print(f"sha256 {INVENTORY_HEADER}: {sha256_file(inv_h)}", flush=True)
    print(f"sha256 {INVENTORY_SOURCE}: {sha256_file(inv_cpp)}", flush=True)

    source = inv_cpp.read_text(encoding="utf-8")
    first = "QString SyncRecordCaptionTag() {"
    following = "SyncCandidateRecord ClassifySyncCandidate("
    if source.count(first) != 1 or source.count(following) != 1:
        raise RuntimeError("Settings predicate extraction boundaries changed")
    begin = source.index(first)
    finish = source.index(following)
    if finish <= begin:
        raise RuntimeError("Settings predicate extraction boundaries reordered")
    expected_slice = source[begin:finish].strip().splitlines()

    test_lines = test_cpp.read_text(encoding="utf-8").splitlines()
    if test_lines.count(EXTRACT_START) != 1 or test_lines.count(EXTRACT_END) != 1:
        raise RuntimeError(f"Expected unique extraction markers in {TEST_SOURCE}")
    start_idx = test_lines.index(EXTRACT_START)
    end_idx = test_lines.index(EXTRACT_END)
    if end_idx <= start_idx + 1:
        raise RuntimeError(f"Empty extracted slice in {TEST_SOURCE}")
    extracted_slice = test_lines[start_idx + 1:end_idx]
    if [line.expandtabs(4) for line in expected_slice] != [
        line.expandtabs(4) for line in extracted_slice
    ]:
        raise RuntimeError(
            f"Extracted IsSyncHistoryCandidate block in {TEST_SOURCE} differs from "
            f"{INVENTORY_SOURCE}"
        )
    print(f"Verified verbatim settings predicate: {INVENTORY_SOURCE}", flush=True)


def qt_flags(qt_prefix: pathlib.Path) -> tuple[list[str], list[str]]:
    lib_dir = qt_prefix / "lib"
    inc_dir = qt_prefix / "include"
    framework_headers = lib_dir / "QtCore.framework" / "Headers"

    compile_flags: list[str] = []
    link_flags: list[str] = []

    if framework_headers.is_dir():
        compile_flags.extend([f"-F{lib_dir}", f"-I{framework_headers}"])
        if inc_dir.is_dir():
            compile_flags.append(f"-I{inc_dir}")
        link_flags.extend([f"-F{lib_dir}", "-framework", "QtCore", f"-Wl,-rpath,{lib_dir}"])
        return compile_flags, link_flags

    if (inc_dir / "QtCore").is_dir():
        compile_flags.extend([f"-I{inc_dir}", f"-I{inc_dir / 'QtCore'}"])
        link_flags.extend([f"-L{lib_dir}", "-lQt6Core", f"-Wl,-rpath,{lib_dir}"])
        return compile_flags, link_flags

    raise RuntimeError(f"Could not find QtCore headers under qt-prefix {qt_prefix}")


def run_step(cmd: list[str], timeout_s: int) -> None:
    print("+", " ".join(shlex.quote(part) for part in cmd), flush=True)
    subprocess.run(cmd, cwd=REPO_ROOT, check=True, timeout=timeout_s, stdin=subprocess.DEVNULL)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build and run focused PLAYLISTS A3 RECORDS1 test.")
    parser.add_argument(
        "--qt-prefix",
        default=os.environ.get("QT_PREFIX"),
        help="Path to Qt6 installation prefix.",
    )
    parser.add_argument(
        "--build-dir",
        default="tests/build/records1",
        help="Output build directory relative to repository root or absolute.",
    )
    parser.add_argument(
        "--compile-wrapper",
        default=os.environ.get("COMPILE_WRAPPER", ""),
        help="Wrapper command prefix for each compile/link step.",
    )
    parser.add_argument(
        "--cxx",
        default=os.environ.get("CXX", "clang++"),
        help="C++20 compiler binary.",
    )
    args = parser.parse_args()

    verify_extracted_settings_predicate()

    if not args.qt_prefix:
        parser.error("Set QT_PREFIX or pass --qt-prefix for your Qt6 installation")
    qt_prefix = pathlib.Path(args.qt_prefix)
    build_dir = pathlib.Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = REPO_ROOT / build_dir
    build_dir.mkdir(parents=True, exist_ok=True)

    wrapper_cmd = shlex.split(args.compile_wrapper) if args.compile_wrapper.strip() else []
    qt_cxx_flags, qt_ld_flags = qt_flags(qt_prefix)

    base_cxx_flags = [
        "-std=c++20",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        f"-I{REPO_ROOT}",
        *qt_cxx_flags,
    ]

    objects: list[pathlib.Path] = []
    for rel_src in CORE_SOURCES:
        src_path = REPO_ROOT / rel_src
        obj_path = build_dir / f"{src_path.stem}.o"
        objects.append(obj_path)
        run_step(
            [
                *wrapper_cmd,
                args.cxx,
                *base_cxx_flags,
                "-c",
                str(src_path),
                "-o",
                str(obj_path),
            ],
            timeout_s=180,
        )

    test_src_path = REPO_ROOT / TEST_SOURCE
    test_obj_path = build_dir / f"{test_src_path.stem}.o"
    run_step(
        [
            *wrapper_cmd,
            args.cxx,
            *base_cxx_flags,
            "-DPURPLE_PLAYLISTS_RECORDS_EXTRACT_SETTINGS",
            "-c",
            str(test_src_path),
            "-o",
            str(test_obj_path),
        ],
        timeout_s=180,
    )

    binary_path = build_dir / "test_playlists_records"
    run_step(
        [
            *wrapper_cmd,
            args.cxx,
            "-std=c++20",
            str(test_obj_path),
            *[str(obj) for obj in objects],
            "-o",
            str(binary_path),
            *qt_ld_flags,
            "-lz",
        ],
        timeout_s=180,
    )

    # Runtime runs outside compile-wrapper so compiler:local hold is never held during tests.
    run_step([str(binary_path)], timeout_s=120)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
