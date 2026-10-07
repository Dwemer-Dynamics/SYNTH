#!/usr/bin/env python3
"""Audit a staged SYNTH release directory or zip without native toolchains."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Iterable

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from verify_papyrus_pex import verify_bundle  # noqa: E402
from build_gameplay import verify_plugin
from build_wait_script import FILES as WAIT_FILES, verify_wait_bundle

PAPYRUS_FILES = ('Data/Scripts/SYNTHNative.pex', 'Data/Scripts/Source/User/SYNTHNative.psc',
                 'Papyrus/build-manifest.json', 'Papyrus/SYNTHNative.flg')
LANES = {
    "flat": {
        "artifact": "SYNTH.dll",
        "forbidden_artifact": "SYNTHVR.dll",
        # Notices for code compiled into or linked by the DLL (see THIRD_PARTY_NOTICES.md).
        "licenses": (
            "CommonLibF4-GPL-3.0-or-later.txt",
            "CommonLibF4-EXCEPTIONS.txt",
            "CommonLibF4-MIT.txt",
            # xmake spdlog v1.16.0 source LICENSE; the VR lane's vcpkg 1.17.0 text differs.
            "spdlog-1.16.0-MIT.txt",
        ),
    },
    "vr": {
        "artifact": "SYNTHVR.dll",
        "forbidden_artifact": "SYNTH.dll",
        "licenses": (
            "CommonLibF4VR-MIT.txt",
            "VRAddressLibrary-MIT.txt",
            "spdlog-MIT.txt",
            "fmt-MIT.txt",
            "rapidcsv-BSD-3-Clause.txt",
            "rsm-mmio-MIT.txt",
        ),
    },
}
# Root LICENSE: the unmodified GNU GPL version 3 text (LF line endings).
PROJECT_LICENSE = "LICENSE"
PROJECT_LICENSE_SHA256 = "3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986"
FORBIDDEN_SUFFIXES ={".esp", ".esl", ".esm", ".pex", ".ba2", ".log", ".pdb"}
FORBIDDEN_PARTS = {
    ".git", ".github", ".xmake", ".vs", "build", "builds", "out", "output",
    "secrets", "secret", "logs", "runtime", "game", "fallout4", "fallout4vr",
}
SECRET_NAME = re.compile(r"(^|[._-])(secret|token|credential|apikey|api-key|password|passwd)([._-]|$)", re.I)
GAME_BINARY = re.compile(r"^(fallout4|fallout4vr|f4se|f4sevr)(?:_loader|_[0-9_]+)?\.(?:exe|dll)$", re.I)


class AuditError(ValueError):
    pass


@dataclass(frozen=True)
class Entry:
    name: str
    data: bytes


def _canonical_name(raw: str) -> str:
    if "\\" in raw:
        raise AuditError(f"non-portable backslash path: {raw}")
    path = PurePosixPath(raw)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        raise AuditError(f"unsafe path: {raw}")
    return path.as_posix()


def _directory_entries(root: Path) -> list[Entry]:
    if not root.is_dir():
        raise AuditError(f"release root is not a directory: {root}")
    entries: list[Entry] = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise AuditError(f"symbolic links are forbidden: {path.relative_to(root)}")
        if path.is_file():
            entries.append(Entry(path.relative_to(root).as_posix(), path.read_bytes()))
    return entries


def _zip_entries(archive: Path) -> list[Entry]:
    if not archive.is_file():
        raise AuditError(f"archive does not exist: {archive}")
    entries: list[Entry] = []
    seen: set[str] = set()
    with zipfile.ZipFile(archive) as package:
        for info in package.infolist():
            if info.is_dir():
                continue
            name = _canonical_name(info.filename)
            folded = name.casefold()
            if folded in seen:
                raise AuditError(f"duplicate archive path: {name}")
            seen.add(folded)
            unix_mode = info.external_attr >> 16
            if unix_mode & 0o170000 == 0o120000:
                raise AuditError(f"symbolic links are forbidden: {name}")
            entries.append(Entry(name, package.read(info)))
    return sorted(entries, key=lambda entry: entry.name)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def build_file_manifest(entries: Iterable[Entry]) -> list[dict[str, object]]:
    return [
        {"path": entry.name, "sha256": _sha256(entry.data), "size": len(entry.data)}
        for entry in sorted(entries, key=lambda item: item.name)
        if entry.name != "manifest.json"
    ]


def audit_entries(entries: list[Entry], lane: str) -> None:
    lane_config = LANES[lane]
    names: dict[str, Entry] = {}
    for entry in entries:
        name = _canonical_name(entry.name)
        folded = name.casefold()
        if folded in names:
            raise AuditError(f"case-insensitive duplicate path: {name}")
        names[folded] = entry
        path = PurePosixPath(name)
        parts = {part.casefold() for part in path.parts[:-1]}
        if parts & FORBIDDEN_PARTS:
            raise AuditError(f"forbidden directory in release: {name}")
        owned_gameplay = lane == 'flat' and name == 'Data/SYNTH.esp'
        if owned_gameplay:
            try:
                verify_plugin(entry.data)
            except ValueError as error:
                raise AuditError(str(error)) from error
        if path.suffix.casefold() in FORBIDDEN_SUFFIXES and name != PAPYRUS_FILES[0] and not (lane == 'flat' and name == WAIT_FILES[0]) and not owned_gameplay:
            raise AuditError(f"forbidden file type in release: {name}")
        if path.suffix.casefold() in {'.psc', '.flg'} and name not in PAPYRUS_FILES and not (lane == 'flat' and name in WAIT_FILES):
            raise AuditError(f"unowned script source in release: {name}")
        if SECRET_NAME.search(path.name):
            raise AuditError(f"secret-like filename in release: {name}")
        if GAME_BINARY.match(path.name):
            raise AuditError(f"game or script-extender binary in release: {name}")
        if path.suffix.casefold() == ".dll" and path.name.casefold() != lane_config["artifact"].casefold():
            raise AuditError(f"unexpected or cross-lane DLL in {lane} release: {name}")
        # SYNTH registers native F4SE Menu Framework pages. Shipping an MCM
        # config would additionally list it under "MCM Mod Configs (Legacy)".
        if len(path.parts) > 1 and path.parts[0].casefold() == "data" and path.parts[1].casefold() == "mcm":
            raise AuditError(f"legacy MCM asset in release: {name}")

    wait_present = [name.casefold() in names for name in WAIT_FILES]
    if lane == 'flat' and 'data/synth.esp' in names and not all(wait_present):
        raise AuditError('incomplete wait controller bundle for SYNTH.esp')
    if any(wait_present):
        if lane != 'flat' or not all(wait_present):
            raise AuditError('incomplete or cross-lane wait controller bundle')
        pex, source, provenance, flags = (names[name.casefold()].data for name in WAIT_FILES)
        try:
            verify_wait_bundle(pex, provenance, source, flags)
        except (ValueError, UnicodeError) as error:
            raise AuditError('invalid wait controller bundle: ' + str(error)) from error
    present = [name.casefold() in names for name in PAPYRUS_FILES]
    if any(present):
        if not all(present):
            raise AuditError('incomplete owned Papyrus bundle')
        pex, source, provenance, flags = (names[name.casefold()].data for name in PAPYRUS_FILES)
        try:
            verify_bundle(pex, provenance, source, flags)
        except (ValueError, UnicodeError) as error:
            raise AuditError('invalid owned Papyrus bundle: ' + str(error)) from error

    required = {
        f"Data/F4SE/Plugins/{lane_config['artifact']}",
        "Data/F4SE/Plugins/SYNTH.ini",
        "README.txt",
        "dependency-manifest.json",
        "manifest.json",
        PROJECT_LICENSE,
        "THIRD_PARTY_NOTICES.md",
        "LICENSES/DIALECTIC-MIT.txt",
    }
    required.update(f"LICENSES/{name}" for name in lane_config["licenses"])
    missing = sorted(name for name in required if name.casefold() not in names)
    if missing:
        raise AuditError("missing required release files: " + ", ".join(missing))
    if _sha256(names[PROJECT_LICENSE.casefold()].data) != PROJECT_LICENSE_SHA256:
        raise AuditError("LICENSE is not the unmodified GNU GPL version 3 text")

    manifest_entry = names["manifest.json"]
    try:
        manifest = json.loads(manifest_entry.data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AuditError(f"invalid manifest.json: {error}") from error
    if manifest.get("schema_version") != 1 or manifest.get("lane") != lane:
        raise AuditError("manifest lane or schema version is incorrect")
    if manifest.get("artifact") != lane_config["artifact"]:
        raise AuditError("manifest artifact is incorrect")
    if manifest.get("files") != build_file_manifest(entries):
        raise AuditError("manifest file list, size, or SHA-256 does not match release")

    try:
        dependencies = json.loads(names["dependency-manifest.json"].data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AuditError(f"invalid dependency-manifest.json: {error}") from error
    if dependencies.get("lane") != lane or dependencies.get("artifact") != lane_config["artifact"]:
        raise AuditError("dependency manifest does not match release lane")


def audit_path(path: Path, lane: str) -> None:
    entries = _zip_entries(path) if path.suffix.casefold() == ".zip" else _directory_entries(path)
    audit_entries(entries, lane)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lane", choices=sorted(LANES), required=True)
    parser.add_argument("--path", type=Path, required=True, help="staged directory or zip archive")
    args = parser.parse_args(argv)
    try:
        audit_path(args.path, args.lane)
    except (AuditError, OSError, zipfile.BadZipFile) as error:
        print(f"release audit failed: {error}", file=sys.stderr)
        return 1
    print(f"release audit passed: {args.lane} {args.path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
