#!/usr/bin/env python3
"""Compile and run dependency-free config tests with the Python stdlib."""
from __future__ import annotations
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def compiler() -> tuple[str, str]:
    requested = os.environ.get("CXX")
    choices = ([requested] if requested else []) + (["cl", "clang-cl", "clang++"] if os.name == "nt" else ["clang++", "g++"])
    for choice in choices:
        if choice and shutil.which(choice):
            return choice, "msvc" if Path(choice).name.lower() in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"} else "unix"
    raise SystemExit("No C++ compiler found. Set CXX to clang++, g++, cl, or clang-cl.")


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    cxx, style = compiler()
    with tempfile.TemporaryDirectory(prefix="synth-config-tests-") as temporary:
        output = Path(temporary) / ("config_tests.exe" if os.name == "nt" else "config_tests")
        source = root / "tests/config/config_tests.cpp"
        command = ([cxx, "/nologo", "/std:c++latest", "/EHsc", "/W4", "/WX", "/permissive-", f"/I{root / 'src'}", str(source), f"/Fe:{output}"]
                   if style == "msvc" else
                   [cxx, "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", f"-I{root / 'src'}", str(source), "-o", str(output)])
        print("+", " ".join(map(str, command)), flush=True)
        subprocess.run(command, cwd=root, check=True)
        print("+", output, flush=True)
        return subprocess.run([str(output)], cwd=root, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
