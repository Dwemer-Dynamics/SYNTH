#!/usr/bin/env python3
"""Compile and run dependency-free C++23 context tests."""
from __future__ import annotations
import os, shutil, subprocess, tempfile
from pathlib import Path

def main() -> int:
    root = Path(__file__).resolve().parents[2]
    requested = os.environ.get("CXX")
    candidates = ([requested] if requested else []) + (["cl", "clang-cl", "clang++"] if os.name == "nt" else ["clang++", "g++"])
    compiler = next((c for c in candidates if c and shutil.which(c)), None)
    if not compiler: raise SystemExit("No C++ compiler found")
    msvc = Path(compiler).name.lower() in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"}
    source = root / "tests/context/context_tests.cpp"
    with tempfile.TemporaryDirectory(prefix="synth-context-") as tmp:
        output = Path(tmp) / ("tests.exe" if os.name == "nt" else "tests")
        command = ([compiler, "/nologo", "/std:c++latest", "/EHsc", "/W4", "/WX", "/permissive-", f"/I{root/'src'}", str(source), f"/Fe:{output}"] if msvc else
                   [compiler, "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", f"-I{root/'src'}", str(source), "-o", str(output)])
        subprocess.run(command, cwd=root, check=True)
        return subprocess.run([str(output)], cwd=root, check=False).returncode
if __name__ == "__main__": raise SystemExit(main())
