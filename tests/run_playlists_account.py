#!/usr/bin/env python3
"""Compile and run the playlist account-files and settings tests alone."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys


def run(command, timeout):
    print(shlex.join(command), flush=True)
    subprocess.run(command, check=True, timeout=timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-prefix", default=os.environ.get("QT_PREFIX"))
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--compile-wrapper", default=os.environ.get("COMPILE_WRAPPER", ""))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    build = (args.build_dir or root / "tests/build/account1").resolve()
    build.mkdir(parents=True, exist_ok=True)
    compile_flags = []
    link_flags = []
    if args.qt_prefix:
        prefix = Path(args.qt_prefix).resolve()
        frameworks = next((directory for directory in
            (prefix / "lib", prefix / "frameworks", prefix)
            if (directory / "QtCore.framework/Headers").is_dir()), None)
        if frameworks:
            compile_flags = ["-I" + str(frameworks / "QtCore.framework/Headers"),
                "-F" + str(frameworks)]
            link_flags = ["-F" + str(frameworks), "-framework", "QtCore",
                "-Wl,-rpath," + str(frameworks)]
        else:
            parser.error("--qt-prefix must contain a QtCore framework in lib/ or frameworks/")
    else:
        for flag, destination in (("--cflags", compile_flags), ("--libs", link_flags)):
            result = subprocess.run(["pkg-config", flag, "Qt6Core"],
                check=True, capture_output=True, text=True, timeout=10)
            destination.extend(shlex.split(result.stdout))
        compile_flags.append("-fPIC")
    compiler = shlex.split(args.compile_wrapper) + shlex.split(args.cxx)
    flags = [
        "-std=c++20",
        "-g",
        "-O0",
        "-I" + str(root),
        "-I" + str(root / "tomlplusplus"),
    ] + compile_flags
    sources = [
        "purple/purple_peer_token.cpp",
        "purple/purple_local_choice.cpp",
        "purple/purple_media_reply.cpp",
        "purple/purple_account_files.cpp",
        "purple/purple_settings.cpp",
    ]
    shared_objects = []
    for source in sources:
        target = build / (Path(source).stem + ".o")
        run(compiler + flags + ["-c", str(root / source), "-o", str(target)], 180)
        shared_objects.append(str(target))
    test_source = "tests/test_playlists_account.cpp"
    test_obj = build / "test_playlists_account.o"
    run(compiler + flags + ["-c", str(root / test_source), "-o", str(test_obj)], 180)
    executable = build / "test_playlists_account"
    run(compiler + shared_objects + [str(test_obj)] + link_flags + ["-o", str(executable)], 180)
    run([str(executable)], 120)


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
