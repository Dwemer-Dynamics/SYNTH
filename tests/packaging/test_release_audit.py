from __future__ import annotations

import json
import re
import shutil
import tempfile
import unittest
import zipfile
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))

from audit_release_tree import LANES, AuditError, audit_path  # noqa: E402
from package_release import stage_release, write_zip  # noqa: E402


class ReleaseAuditTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _package(self, lane: str) -> tuple[Path, Path]:
        artifact = "SYNTH.dll" if lane == "flat" else "SYNTHVR.dll"
        dll = self.root / artifact
        dll.write_bytes(b"test-only input, not a tracked or released DLL")
        stage = self.root / f"stage-{lane}"
        stage.mkdir()
        stage_release(lane, dll, stage)
        archive = self.root / f"{lane}.zip"
        write_zip(stage, archive)
        return stage, archive

    def test_flat_stage_and_archive_pass(self) -> None:
        stage, archive = self._package("flat")
        audit_path(stage, "flat")
        audit_path(archive, "flat")

    def test_vr_stage_and_archive_pass(self) -> None:
        stage, archive = self._package("vr")
        audit_path(stage, "vr")
        audit_path(archive, "vr")

    def test_owned_gameplay_record_is_exact_and_not_cross_lane(self) -> None:
        stage, _ = self._package('flat')
        plugin = stage / 'Data/SYNTH.esp'
        original = plugin.read_bytes()
        self.assertIn(b'SYNTH_WaitHere', original)
        self.assertIn(b'Fallout4.esm', original)
        # The retired local-800 migration record retains its old Sandbox layout.
        import struct
        self.assertIn(b'PLDT' + struct.pack('<Hiiii', 16, 2, 0, 64, 0), original)
        self.assertIn(b'PKCU' + struct.pack('<HIII', 12, 15, 0x2CB1, 7), original)
        self.assertIn(b'PKDT' + struct.pack('<HIBBBBHH', 12, 0x100004, 18, 0, 0, 0, 0, 0), original)
        # Every live slot uses HoldPosition v1, one Location input, zero radius.
        self.assertEqual(original.count(b'PKCU' + struct.pack('<HIII', 12, 1, 0x1D415, 1)), 256)
        # All 256 live holds plus the migration package are normal packages, not combat overrides.
        self.assertEqual(original.count(b'PKDT' + struct.pack('<HIBBBBHH', 12, 0x100004, 18, 0, 0, 0, 0, 0)), 257)
        self.assertNotIn(b'PKDT' + struct.pack('<HIBBBBHH', 12, 0x100004, 18, 4, 0, 0, 0, 0), original)
        # Existing packages/markers retain their IDs, but no alias injects them anymore.
        # A second bank of saved aliases records only SYNTH-owned restraint transitions.
        self.assertIn(b'SYNTH_WaitOwnership', original)
        self.assertEqual(original.count(b'ALST'), 512)
        self.assertEqual(original.count(b'ALPC'), 0)
        for slot in range(256):
            self.assertIn(b'ALST' + struct.pack('<HI', 4, 256 + slot), original)
            self.assertIn(f'RestraintOwned{slot}\0'.encode(), original)
            self.assertIn(b'PLDT' + struct.pack('<Hiiii', 16, 0, 0x01000A00 + slot, 0, 0), original)
        self.assertNotIn(struct.pack('<f', 300.0), original)
        plugin.write_bytes(original + b'unreviewed record')
        with self.assertRaisesRegex(AuditError, 'deterministic wait package'):
            audit_path(stage, 'flat')

    def test_wait_controller_is_source_bound_and_cannot_be_omitted(self):
        stage, _ = self._package('flat')
        script = stage / 'Data/Scripts/SYNTHWait.pex'
        original = script.read_bytes()
        script.write_bytes(original + b'tampered')
        with self.assertRaisesRegex(AuditError, 'wait controller bundle'):
            audit_path(stage, 'flat')
        script.unlink()
        with self.assertRaisesRegex(AuditError, 'incomplete.*wait controller'):
            audit_path(stage, 'flat')

    def test_cross_lane_dll_is_rejected(self) -> None:
        stage, _ = self._package("flat")
        (stage / "Data/F4SE/Plugins/SYNTHVR.dll").write_bytes(b"wrong lane")
        with self.assertRaisesRegex(AuditError, "cross-lane DLL"):
            audit_path(stage, "flat")

    def test_forbidden_game_content_is_rejected(self) -> None:
        stage, _ = self._package("vr")
        for suffix in ('.esp', '.esl', '.esm', '.pex', '.ba2'):
            target = stage / ('Data/Forbidden' + suffix)
            target.write_bytes(b"forbidden")
            with self.assertRaisesRegex(AuditError, "forbidden file type"):
                audit_path(stage, "vr")
            target.unlink()
        owned = stage / 'Data/Scripts/SYNTHNative.pex'
        owned.parent.mkdir()
        owned.write_bytes(b'not an audited native PEX')
        with self.assertRaisesRegex(AuditError, 'incomplete owned Papyrus bundle'):
            audit_path(stage, 'vr')

    def test_game_and_f4se_binaries_are_rejected(self) -> None:
        stage, _ = self._package("flat")
        (stage / "F4SE_loader.exe").write_bytes(b"forbidden")
        with self.assertRaisesRegex(AuditError, "game or script-extender binary"):
            audit_path(stage, "flat")

    def test_logs_build_trees_and_secret_names_are_rejected(self) -> None:
        for relative, message in [
            ("logs/client.txt", "forbidden directory"),
            ("build/object.txt", "forbidden directory"),
            ("api-token.txt", "secret-like filename"),
        ]:
            with self.subTest(relative=relative):
                stage, _ = self._package("flat")
                target = stage / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("forbidden", encoding="utf-8")
                with self.assertRaisesRegex(AuditError, message):
                    audit_path(stage, "flat")
                for child in self.root.iterdir():
                    if child.is_dir() and child.name.startswith("stage-flat"):
                        import shutil
                        shutil.rmtree(child)

    def test_legacy_mcm_assets_are_rejected(self) -> None:
        stage, _ = self._package("flat")
        target = stage / "Data/MCM/Config/SYNTH/config.json"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("{}", encoding="utf-8")
        with self.assertRaisesRegex(AuditError, "legacy MCM asset"):
            audit_path(stage, "flat")

    def test_packages_ship_no_mcm_tree(self) -> None:
        for lane in ("flat", "vr"):
            with self.subTest(lane=lane):
                stage, archive = self._package(lane)
                self.assertFalse((stage / "Data/MCM").exists())
                with zipfile.ZipFile(archive) as package:
                    names = package.namelist()
                self.assertFalse([name for name in names if name.startswith("Data/MCM")])
                self.assertIn("Data/F4SE/Plugins/SYNTH.ini", names)
                self.assertIn("LICENSES/DIALECTIC-MIT.txt", names)
                self.assertIn("THIRD_PARTY_NOTICES.md", names)
                for notice in LANES[lane]["licenses"]:
                    self.assertIn(f"LICENSES/{notice}", names)
                dependency_notice = LANES[lane]["licenses"][-1]
                (stage / "LICENSES" / dependency_notice).unlink()
                with self.assertRaisesRegex(AuditError, re.escape(f"LICENSES/{dependency_notice}")):
                    audit_path(stage, lane)
                shutil.copyfile(
                    ROOT / "packaging/licenses" / dependency_notice,
                    stage / "LICENSES" / dependency_notice,
                )
                audit_path(stage, lane)
                self.assertIn("LICENSE", names)
                project_license = stage / "LICENSE"
                self.assertEqual(project_license.read_bytes(), (ROOT / "LICENSE").read_bytes())
                project_license.write_bytes(project_license.read_bytes().replace(b"\n", b"\r\n"))
                with self.assertRaisesRegex(AuditError, "GNU GPL version 3"):
                    audit_path(stage, lane)
                project_license.unlink()
                with self.assertRaisesRegex(AuditError, r"missing required release files: LICENSE$"):
                    audit_path(stage, lane)
                shutil.copyfile(ROOT / "LICENSE", project_license)
                audit_path(stage, lane)
                (stage / "LICENSES/DIALECTIC-MIT.txt").unlink()
                with self.assertRaisesRegex(
                    AuditError, r"missing required release files: LICENSES/DIALECTIC-MIT\.txt$"
                ):
                    audit_path(stage, lane)

    def test_manifest_tampering_is_rejected(self) -> None:
        stage, _ = self._package("flat")
        manifest_path = stage / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["files"][0]["sha256"] = "0" * 64
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(AuditError, "does not match"):
            audit_path(stage, "flat")

    def test_zip_path_traversal_is_rejected(self) -> None:
        archive = self.root / "unsafe.zip"
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr("../escape.txt", b"bad")
        with self.assertRaisesRegex(AuditError, "unsafe path"):
            audit_path(archive, "flat")

    def test_safe_sample_has_no_secret_and_is_loopback_only(self) -> None:
        sample = (ROOT / "config/SYNTH.ini").read_text(encoding="utf-8").casefold()
        self.assertIn("baseurl=http://127.0.0.1:", sample)
        self.assertIn("allowremote=false", sample)
        for word in ("apikey=", "api_key=", "password=", "token="):
            self.assertNotIn(word, sample)


if __name__ == "__main__":
    unittest.main()
