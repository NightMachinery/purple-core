#!/usr/bin/env python3
"""Focused compile-and-run driver for tests/test_playlists_step.cpp."""

import argparse
import os
import pathlib
import platform
import shlex
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

TRANSITIVE_SOURCES = [
    "purple/purple_peer_token.cpp",
    "purple/purple_sync_json.cpp",
    "purple/purple_playlists.cpp",
    "purple/purple_playlists_local.cpp",
    "purple/purple_playlists_codec.cpp",
    "purple/purple_playlists_sync.cpp",
    "purple/purple_playlists_inventory.cpp",
    "purple/purple_playlists_cover.cpp",
    "purple/purple_playlists_ops.cpp",
    "purple/purple_playlists_policy.cpp",
    "purple/purple_playlists_diff.cpp",
    "purple/purple_playlists_step.cpp",
    "tests/test_playlists_step.cpp",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build and run focused playlist step tests."
    )
    parser.add_argument(
        "--qt-prefix",
        default=os.environ.get("QT_PREFIX", ""),
        help="Qt installation prefix containing lib/QtCore.framework or include/QtCore.",
    )
    parser.add_argument(
        "--cxx",
        default=os.environ.get("CXX", "clang++"),
        help="C++20 compiler executable.",
    )
    parser.add_argument(
        "--compile-wrapper",
        default=os.environ.get("COMPILE_WRAPPER", ""),
        help="Optional command prefix wrapping compile/link steps.",
    )
    parser.add_argument(
        "--build-dir",
        default=str(REPO_ROOT / "tests" / "build" / "step1"),
        help="Output directory for object files and test binary.",
    )
    return parser.parse_args()


def qt_flags(qt_prefix: pathlib.Path) -> tuple[list[str], list[str]]:
    include_dir = qt_prefix / "include"
    lib_dir = qt_prefix / "lib"
    if platform.system() == "Darwin":
        cflags = [
            f"-I{lib_dir / 'QtCore.framework' / 'Headers'}",
            f"-I{include_dir}",
            f"-F{lib_dir}",
        ]
        ldflags = [
            f"-F{lib_dir}",
            "-framework",
            "QtCore",
            f"-Wl,-rpath,{lib_dir}",
            "-lz",
        ]
        return cflags, ldflags

    cflags = [
        "-fPIC",
        f"-I{include_dir}",
        f"-I{include_dir / 'QtCore'}",
    ]
    ldflags = [
        f"-L{lib_dir}",
        "-lQt6Core",
        f"-Wl,-rpath,{lib_dir}",
        "-lz",
    ]
    return cflags, ldflags


def main() -> int:
    args = parse_args()
    if not args.qt_prefix:
        print(
            "error: --qt-prefix or QT_PREFIX environment variable is required",
            file=sys.stderr,
        )
        return 2

    qt_prefix = pathlib.Path(args.qt_prefix).expanduser().resolve()
    if not qt_prefix.exists():
        print(f"error: Qt prefix does not exist: {qt_prefix}", file=sys.stderr)
        return 2

    build_dir = pathlib.Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = REPO_ROOT / build_dir
    build_dir.mkdir(parents=True, exist_ok=True)

    wrapper_cmd = shlex.split(args.compile_wrapper) if args.compile_wrapper else []
    cflags, ldflags = qt_flags(qt_prefix)
    base_cxx = [
        args.cxx,
        "-std=c++20",
        "-O2",
        "-Wall",
        "-Wextra",
        f"-I{REPO_ROOT}",
        *cflags,
    ]

    binary_path = build_dir / "test_playlists_step"
    source_paths = [str(REPO_ROOT / rel) for rel in TRANSITIVE_SOURCES]
    compile_cmd = [
        *wrapper_cmd,
        *base_cxx,
        *source_paths,
        "-o",
        str(binary_path),
        *ldflags,
    ]

    print(f"Building {binary_path}...")
    subprocess.run(compile_cmd, cwd=REPO_ROOT, timeout=180, check=True)

    print(f"Running {binary_path}...")
    subprocess.run([str(binary_path)], cwd=REPO_ROOT, timeout=120, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
