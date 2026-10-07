#!/usr/bin/env python3
"""Audit adapter isolation, compile dependency-free entries, and run adapter tests."""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def find_compiler() -> tuple[str, str]:
    requested = os.environ.get("CXX")
    candidates = [requested] if requested else []
    candidates.extend(["cl", "clang-cl", "clang++"] if os.name == "nt" else ["clang++", "g++"])
    for candidate in candidates:
        if candidate and shutil.which(candidate):
            name = Path(candidate).name.lower()
            style = "msvc" if name in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"} else "unix"
            return candidate, style
    raise SystemExit("No C++23 compiler found. Set CXX to clang++, g++, cl, or clang-cl.")


def run(command: list[str], root: Path) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=root, check=True)


def audit_boundaries(root: Path) -> None:
    engine_include = re.compile(r'^\s*#\s*include\s*[<"](?:F4SE|RE|REL|Windows(?:\.h)?)(?:[/\\>"])')
    forbidden_type = re.compile(r"\b(?:F4SE|RE|REL)::")
    adapters = list((root / "src" / "adapters").rglob("*"))
    for path in adapters:
        if not path.is_file():
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if engine_include.search(line) or forbidden_type.search(line):
                raise SystemExit(f"engine ABI leaked into adapter seam: {path}:{number}: {line.strip()}")

    flat = (root / "src" / "flat" / "plugin_entry.cpp").read_text(encoding="utf-8")
    vr = (root / "src" / "vr" / "plugin_entry.cpp").read_text(encoding="utf-8")
    if flat.count("F4SE_PLUGIN_VERSION") != 1 or "F4SEPlugin_Version" in flat:
        raise SystemExit("flat entry must own exactly one macro-defined F4SEPlugin_Version")
    if vr.count("F4SEPlugin_Version") != 1 or "F4SE_PLUGIN_VERSION" in vr:
        raise SystemExit("VR entry must own exactly one explicit F4SEPlugin_Version")
    for text, runtime_type in ((flat, "FlatFalloutRuntime"), (vr, "VrFalloutRuntime")):
        for required in (runtime_type, "GameThreadDispatcher", "runtime->invalidate()", "capture_snapshot"):
            if required not in text:
                raise SystemExit(f"plugin entry is missing runtime wiring: {required}")


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    compiler, style = find_compiler()
    audit_boundaries(root)

    sources = [
        root / "tests" / "adapters" / "adapters_tests.cpp",
        root / "src" / "flat" / "plugin_entry.cpp",
        root / "src" / "vr" / "plugin_entry.cpp",
    ]
    with tempfile.TemporaryDirectory(prefix="synth-adapter-tests-") as temporary:
        temporary_path = Path(temporary)
        objects: list[Path] = []
        for index, source in enumerate(sources):
            obj = temporary_path / (f"source-{index}.obj" if style == "msvc" else f"source-{index}.o")
            objects.append(obj)
            if style == "msvc":
                command = [
                    compiler, "/nologo", "/std:c++latest", "/EHsc", "/W4", "/WX",
                    "/permissive-", f"/I{root / 'src'}", "/c", str(source), f"/Fo:{obj}",
                ]
            else:
                command = [
                    compiler, "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                    "-pthread", f"-I{root / 'src'}", "-c", str(source), "-o", str(obj),
                ]
            run(command, root)

        output = temporary_path / ("adapter_tests.exe" if os.name == "nt" else "adapter_tests")
        test_object = objects[0]
        if style == "msvc":
            run([compiler, "/nologo", str(test_object), f"/Fe:{output}"], root)
        else:
            run([compiler, "-pthread", str(test_object), "-o", str(output)], root)
        print("+", output, flush=True)
        return subprocess.run([str(output)], cwd=root, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
