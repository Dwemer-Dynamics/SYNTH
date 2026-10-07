#!/usr/bin/env python3
"""Compile and run dependency-free JSON tests portably."""
from __future__ import annotations
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def compiler() -> tuple[str, str]:
    options = ([os.environ["CXX"]] if os.environ.get("CXX") else [])
    options += ["cl", "clang-cl", "clang++"] if os.name == "nt" else ["clang++", "g++"]
    for item in options:
        if shutil.which(item):
            return item, "msvc" if Path(item).name.lower() in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"} else "unix"
    raise SystemExit("No C++23 compiler found; set CXX")


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    cxx, style = compiler()
    with tempfile.TemporaryDirectory(prefix="synth-json-") as temporary:
        output = Path(temporary) / ("json_tests.exe" if os.name == "nt" else "json_tests")
        if style == "msvc":
            command = [cxx, "/nologo", "/std:c++latest", "/EHsc", "/W4", "/WX", "/permissive-",
                       f"/I{root / 'src'}", str(root / "tests/json/json_tests.cpp"), f"/Fe:{output}"]
        else:
            command = [cxx, "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                       f"-I{root / 'src'}", str(root / "tests/json/json_tests.cpp"), "-o", str(output)]
        print("+", " ".join(map(str, command)), flush=True)
        subprocess.run(command, cwd=root, check=True)
        return subprocess.run([str(output)], cwd=root, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
