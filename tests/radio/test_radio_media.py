"""Offline media tests exercise actual file bytes and staging ownership."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('prepare_radio_media', ROOT / 'scripts/prepare_radio_media.py')
media = importlib.util.module_from_spec(spec)
spec.loader.exec_module(media)


class RadioMediaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.a = media.diagnostic_tone(440, 2)
        cls.b = media.diagnostic_tone(660, 3)
        cls.reuse = media.diagnostic_tone(880, 5)

    def test_candidate_has_same_engine_slot_with_different_bytes_and_duration(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'candidate'
            manifest = media.prepare_media(target, self.a, self.b, self.reuse)
            saved = json.loads((target / 'radio-media-manifest.json').read_text())
            self.assertEqual(saved, manifest)
            clips = saved['clips']
            self.assertEqual(clips['a']['engine_path'], clips['a_reused']['engine_path'])
            self.assertNotEqual(clips['a']['sha256'], clips['a_reused']['sha256'])
            self.assertEqual([clips[key]['duration_seconds'] for key in saved['sequence']], [2, 3, 5])
            for key, expected in [('a', self.a), ('b', self.b), ('a_reused', self.reuse)]:
                self.assertEqual((target / clips[key]['staged_path']).read_bytes(), expected)
            self.assertFalse(saved['installed'])
            self.assertFalse(saved['native_playback_verified'])
            self.assertFalse(saved['live_slot_refresh_verified'])

    def test_existing_directory_is_never_written(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary)
            unrelated = target / 'slot_a.wav'
            unrelated.write_bytes(b'playing audio')
            with self.assertRaises(FileExistsError):
                media.prepare_media(target, self.a, self.b, self.reuse)
            self.assertEqual(unrelated.read_bytes(), b'playing audio')
            self.assertEqual(list(target.iterdir()), [unrelated])

    def test_same_bytes_or_duration_does_not_count_as_reuse_probe(self):
        with tempfile.TemporaryDirectory() as temporary:
            for replacement in [self.a, media.diagnostic_tone(880, 2)]:
                target = Path(temporary) / 'candidate'
                with self.assertRaises(ValueError):
                    media.prepare_media(target, self.a, self.b, replacement)
                self.assertFalse(target.exists())

    def test_bad_pcm_metadata_and_truncation_fail_before_output(self):
        mutations = []
        for offset, value in [(20, 3), (22, 2), (32, 4), (34, 24)]:
            malformed = bytearray(self.a)
            struct.pack_into('<H', malformed, offset, value)
            mutations.append(bytes(malformed))
        mutations += [self.a[:-1], self.a + b'junk', b'not a wav', bytes(media.MAX_WAVE_BYTES + 1)]
        with tempfile.TemporaryDirectory() as temporary:
            for clip in mutations:
                target = Path(temporary) / 'candidate'
                with self.subTest(size=len(clip)):
                    with self.assertRaises(ValueError):
                        media.prepare_media(target, clip, self.b, self.reuse)
                    self.assertFalse(target.exists())

    def test_duration_and_alignment_are_derived_from_pcm(self):
        # RIFF and data sizes are self-consistent; a partial PCM sample is still rejected.
        odd = bytearray(self.a[:-1])
        odd.append(0)  # odd-length data chunk followed by padding
        struct.pack_into('<I', odd, 40, len(self.a) - 45)
        with self.assertRaises(ValueError):
            media.inspect_wave(bytes(odd))
        empty = self.a[:44]
        empty = empty[:4] + struct.pack('<I', 36) + empty[8:40] + struct.pack('<I', 0)
        with self.assertRaises(ValueError):
            media.inspect_wave(empty)


if __name__ == '__main__':
    unittest.main()
