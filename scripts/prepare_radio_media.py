#!/usr/bin/env python3
"""Prepare original radio diagnostic media in a new staging directory, never an install."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
from pathlib import Path
import struct
import wave


ENGINE_ROOT = 'Sound/FX/SYNTH/RadioDiagnostics'
MAX_WAVE_BYTES = 8 * 1024 * 1024
RATES = {16000, 22050, 24000, 44100, 48000}


def inspect_wave(data: bytes) -> dict:
    """Strict bounded PCM16 mono; no decoder, network, or provider is involved."""
    if not 44 <= len(data) <= MAX_WAVE_BYTES:
        raise ValueError('WAV must be between 44 bytes and 8 MiB')
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise ValueError('RIFF/WAVE required')
    if struct.unpack_from('<I', data, 4)[0] + 8 != len(data):
        raise ValueError('RIFF size mismatch')
    chunks = {}
    offset = 12
    while offset < len(data):
        if offset + 8 > len(data):
            raise ValueError('Truncated WAV chunk')
        tag, size = struct.unpack_from('<4sI', data, offset)
        offset += 8
        end = offset + size
        if end + (size % 2) > len(data):
            raise ValueError('WAV chunk size mismatch')
        if tag in {b'fmt ', b'data'}:
            if tag in chunks:
                raise ValueError('Duplicate WAV chunk')
            chunks[tag] = data[offset:end]
        offset = end + (size % 2)
    if set(chunks) != {b'fmt ', b'data'} or len(chunks[b'fmt ']) != 16:
        raise ValueError('Canonical PCM format and data chunks required')
    codec, channels, rate, byte_rate, alignment, bits = struct.unpack('<HHIIHH', chunks[b'fmt '])
    if codec != 1 or channels != 1 or bits != 16 or rate not in RATES:
        raise ValueError('Supported WAV is PCM16 mono at a declared sample rate')
    if alignment != 2 or byte_rate != rate * alignment or len(chunks[b'data']) % alignment:
        raise ValueError('Invalid PCM alignment or byte rate')
    frames = len(chunks[b'data']) // alignment
    duration = frames / rate
    if not 0.1 <= duration <= 40.0:
        raise ValueError('Clip duration must be 0.1 to 40 seconds')
    return {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data),
            'sample_rate': rate, 'frames': frames, 'duration_seconds': duration}


def read_wave(path: Path) -> bytes:
    with path.open('rb') as source:
        data = source.read(MAX_WAVE_BYTES + 1)
    inspect_wave(data)
    return data


def diagnostic_tone(frequency: int, seconds: int) -> bytes:
    """Quiet original test tones with fades; their pitch AND duration identify reuse."""
    rate = 24000
    frames = rate * seconds
    pcm = bytearray()
    for frame in range(frames):
        fade = min(1.0, frame / 240, (frames - 1 - frame) / 240)
        value = round(2048 * fade * math.sin(2 * math.pi * frequency * frame / rate))
        pcm.extend(struct.pack('<h', value))
    result = io.BytesIO()
    with wave.open(result, 'wb') as writer:
        writer.setnchannels(1)
        writer.setsampwidth(2)
        writer.setframerate(rate)
        writer.writeframes(pcm)
    return result.getvalue()


def prepare_media(output: Path, slot_a: bytes, slot_b: bytes, replacement_a: bytes) -> dict:
    """Create fresh candidate files only. This cannot rewrite a playing engine slot."""
    clips = {'a': slot_a, 'b': slot_b, 'a_reused': replacement_a}
    rows = {key: inspect_wave(value) for key, value in clips.items()}
    if rows['a']['sha256'] == rows['a_reused']['sha256']:
        raise ValueError('Reuse test needs different bytes')
    if rows['a']['frames'] / rows['a']['sample_rate'] == rows['a_reused']['frames'] / rows['a_reused']['sample_rate']:
        raise ValueError('Reuse test needs a different duration')
    # mkdir is the ownership boundary: an existing output node is rejected.
    # This builds a fresh candidate; it is not a live slot publication operation.
    output.mkdir(parents=True, exist_ok=False)
    paths = {'a': f'Data/{ENGINE_ROOT}/slot_a.wav',
             'b': f'Data/{ENGINE_ROOT}/slot_b.wav',
             'a_reused': f'Replacement/{ENGINE_ROOT}/slot_a.wav'}
    for key, name in paths.items():
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open('xb') as target:
            target.write(clips[key])
        rows[key]['staged_path'] = name
        rows[key]['engine_path'] = f'Data/{ENGINE_ROOT}/slot_{"b" if key == "b" else "a"}.wav'
    manifest = {'schema_version': 1, 'kind': 'synth.radio.diagnostic.media',
                'installed': False, 'native_playback_verified': False,
                'live_slot_refresh_verified': False, 'clips': rows,
                'sequence': ['a', 'b', 'a_reused'],
                'reuse_requirement': 'Replace only after native audio completion is independently observed; '
                                     'verify changed pitch and duration when the already played slot is reused.'}
    (output / 'radio-media-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New staging directory; must not exist')
    parser.add_argument('--slot-a', type=Path)
    parser.add_argument('--slot-b', type=Path)
    parser.add_argument('--replacement-a', type=Path)
    args = parser.parse_args()
    paths = (args.slot_a, args.slot_b, args.replacement_a)
    if any(paths) and not all(paths):
        parser.error('Supply all three owned WAVs, or omit all three for original diagnostic tones')
    try:
        clips = tuple(read_wave(path) for path in paths) if all(paths) else (
            diagnostic_tone(440, 2), diagnostic_tone(660, 3), diagnostic_tone(880, 5))
        manifest = prepare_media(args.output, *clips)
    except (OSError, ValueError) as error:
        parser.exit(1, f'Media preparation failed: {error}\n')
    print(json.dumps({'status': 'prepared', 'installed': False,
                      'clips': {key: {'sha256': value['sha256'], 'duration_seconds': value['duration_seconds']}
                                for key, value in manifest['clips'].items()}}))


if __name__ == '__main__':
    main()
