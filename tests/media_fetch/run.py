#!/usr/bin/env python3
"""Compile and run the portable media fetch C++ tests."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(__file__).with_name("media_fetch_tests.cpp")


def compiler() -> str:
    requested = os.environ.get("CXX")
    if requested:
        return requested
    for candidate in ("c++", "clang++", "g++", "cl"):
        found = shutil.which(candidate)
        if found:
            return found
    raise RuntimeError("no C++ compiler found; set CXX to a C++23 compiler")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="synth-media-fetch-") as temporary:
        executable = Path(temporary) / (
            "media_fetch_tests.exe" if os.name == "nt" else "media_fetch_tests"
        )
        selected = compiler()
        if Path(selected).name.lower() in {"cl", "cl.exe"}:
            command = [
                selected,
                "/nologo",
                "/std:c++latest",
                "/W4",
                "/EHsc",
                f"/I{ROOT / 'src'}",
                str(SOURCE),
                f"/Fe:{executable}",
            ]
        else:
            command = [
                selected,
                "-std=c++23",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                f"-I{ROOT / 'src'}",
                str(SOURCE),
                "-o",
                str(executable),
            ]
            if os.name != "nt":
                command.insert(4, "-pthread")
        subprocess.run(command, check=True)
        return subprocess.run([str(executable)], check=False).returncode


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        print(f"media fetch test runner failed: {error}", file=sys.stderr)
        raise SystemExit(1)
