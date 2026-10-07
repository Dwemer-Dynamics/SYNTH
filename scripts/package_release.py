#!/usr/bin/env python3
"""Create a deterministic, audited SYNTH lane archive from a real DLL."""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY_ROOT / "tools"))
from audit_release_tree import AuditError, LANES, audit_path, build_file_manifest, _directory_entries  # noqa: E402
from verify_papyrus_pex import verify_bundle
from build_gameplay import build_plugin
from build_wait_script import FILES as WAIT_FILES, verify_wait_bundle

ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)


def _copy(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def stage_release(lane: str, dll: Path, stage: Path, papyrus_build: Path | None = None) -> None:
    lane_config = LANES[lane]
    if dll.name.casefold() != lane_config["artifact"].casefold():
        raise AuditError(f"expected {lane_config['artifact']}, got {dll.name}")
    if not dll.is_file() or dll.stat().st_size == 0:
        raise AuditError(f"built artifact is missing or empty: {dll}")

    _copy(dll, stage / "Data/F4SE/Plugins" / lane_config["artifact"])
    _copy(REPOSITORY_ROOT / "config/SYNTH.ini", stage / "Data/F4SE/Plugins/SYNTH.ini")
    _copy(REPOSITORY_ROOT / f"packaging/README-{lane}.txt", stage / "README.txt")
    _copy(REPOSITORY_ROOT / f"packaging/manifests/{lane}.json", stage / "dependency-manifest.json")
    if lane == "flat":
        (stage / 'Data/SYNTH.esp').write_bytes(build_plugin())
        wait_build = REPOSITORY_ROOT / 'out/wait-papyrus'
        wait_data = ((wait_build / 'SYNTHWait.pex').read_bytes(),
                     (REPOSITORY_ROOT / 'scripts/papyrus/SYNTHWait.psc').read_bytes(),
                     (wait_build / 'wait-build-manifest.json').read_bytes(),
                     (REPOSITORY_ROOT / 'scripts/papyrus/SYNTHNative.flg').read_bytes())
        verify_wait_bundle(wait_data[0], wait_data[2], wait_data[1], wait_data[3])
        for name, data in zip(WAIT_FILES, wait_data):
            target = stage / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    for license_name in lane_config["licenses"]:
        _copy(REPOSITORY_ROOT / "packaging/licenses" / license_name, stage / "LICENSES" / license_name)
    _copy(REPOSITORY_ROOT / "LICENSE", stage / "LICENSE")
    _copy(REPOSITORY_ROOT / "LICENSES/DIALECTIC-MIT.txt", stage / "LICENSES/DIALECTIC-MIT.txt")
    _copy(REPOSITORY_ROOT / "THIRD_PARTY_NOTICES.md", stage / "THIRD_PARTY_NOTICES.md")

    if papyrus_build is not None:
        pex = (papyrus_build / 'SYNTHNative.pex').read_bytes()
        provenance = (papyrus_build / 'build-manifest.json').read_bytes()
        source = (REPOSITORY_ROOT / 'scripts/papyrus/SYNTHNative.psc').read_bytes()
        flags = (REPOSITORY_ROOT / 'scripts/papyrus/SYNTHNative.flg').read_bytes()
        try:
            verify_bundle(pex, provenance, source, flags)
        except (ValueError, UnicodeError) as error:
            raise AuditError('Papyrus build is invalid or stale: ' + str(error)) from error
        for name, data in [('Data/Scripts/SYNTHNative.pex', pex),
                           ('Data/Scripts/Source/User/SYNTHNative.psc', source),
                           ('Papyrus/build-manifest.json', provenance), ('Papyrus/SYNTHNative.flg', flags)]:
            target = stage / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)

    manifest = {
        "schema_version": 1,
        "lane": lane,
        "artifact": lane_config["artifact"],
        "files": build_file_manifest(_directory_entries(stage)),
    }
    (stage / "manifest.json").write_bytes(
        (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    )


def write_zip(stage: Path, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(item for item in stage.rglob("*") if item.is_file()):
            relative = path.relative_to(stage).as_posix()
            info = zipfile.ZipInfo(relative, ZIP_TIMESTAMP)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lane", choices=sorted(LANES), required=True)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument('--papyrus-build', type=Path, help='verified build directory; auto-detects out/papyrus otherwise')
    args = parser.parse_args(argv)
    try:
        with tempfile.TemporaryDirectory(prefix=f"synth-{args.lane}-") as temporary:
            stage = Path(temporary) / "stage"
            stage.mkdir()
            papyrus_build = args.papyrus_build
            if papyrus_build is None and (REPOSITORY_ROOT / 'out/papyrus').exists():
                papyrus_build = REPOSITORY_ROOT / 'out/papyrus'
            stage_release(args.lane, args.dll.resolve(), stage, papyrus_build)
            audit_path(stage, args.lane)
            write_zip(stage, args.output.resolve())
            audit_path(args.output.resolve(), args.lane)
    except (AuditError, OSError) as error:
        print(f"package failed: {error}", file=sys.stderr)
        return 1
    print(f"created audited package: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
