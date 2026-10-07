#!/usr/bin/env python3
"""Compile and run the engine-free native tests with only the Python stdlib."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def find_compiler() -> tuple[str, str]:
    requested = os.environ.get("CXX")
    candidates = [requested] if requested else []
    if os.name == "nt":
        candidates.extend(["cl", "clang-cl", "clang++"])
    else:
        candidates.extend(["clang++", "g++"])

    for candidate in candidates:
        if candidate and shutil.which(candidate):
            name = Path(candidate).name.lower()
            style = "msvc" if name in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"} else "unix"
            return candidate, style
    raise SystemExit("No C++ compiler found. Set CXX to clang++, g++, cl, or clang-cl.")


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    source = root / "tests" / "native" / "native_tests.cpp"
    compiler, style = find_compiler()

    with tempfile.TemporaryDirectory(prefix="synth-native-tests-") as temporary:
        output = Path(temporary) / ("native_tests.exe" if os.name == "nt" else "native_tests")
        if style == "msvc":
            command = [
                compiler,
                "/nologo",
                "/std:c++latest",
                "/EHsc",
                "/W4",
                "/WX",
                "/permissive-",
                f"/I{root / 'src'}",
                str(source),
                f"/Fe:{output}",
            ]
        else:
            command = [
                compiler,
                "-std=c++23",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Werror",
                "-pthread",
                f"-I{root / 'src'}",
                str(source),
                "-o",
                str(output),
            ]

        print("+", " ".join(map(str, command)), flush=True)
        subprocess.run(command, cwd=root, check=True)
        print("+", output, flush=True)
        return subprocess.run([str(output)], cwd=root, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
