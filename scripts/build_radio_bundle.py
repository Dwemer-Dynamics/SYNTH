#!/usr/bin/env python3
"""Build and audit a closed optional diagnostic bundle; never modify a game installation."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import stat
import tempfile

from build_radio_gameplay import PLUGIN_NAME, build_plugin, verify_plugin
from build_radio_papyrus import SCRIPTS, verify_bundle as verify_scripts
from prepare_radio_media import inspect_wave

ROOT = Path(__file__).resolve().parents[1]
SOURCE_FILES = ('scripts/build_radio_gameplay.py', 'scripts/build_radio_papyrus.py',
                'scripts/build_radio_bundle.py', 'scripts/prepare_radio_media.py',
                'src/radio/diagnostic.hpp', 'src/radio/probe_host.hpp',
                'src/adapters/flat_radio_probe.hpp', 'src/adapters/commonlib_radio_probe_api.hpp',
                'src/adapters/commonlib_papyrus_api.hpp', 'src/adapters/flat_fallout_runtime.hpp',
                'src/runtime/fallout_runtime.hpp', 'src/flat/plugin_entry.cpp',
                'tests/radio/radio_tests.cpp', 'tests/radio/test_radio_gameplay.py',
                'tests/radio/test_radio_media.py', 'docs/RADIO-PROTOTYPE.md')
NOTICES = ('LICENSE', 'THIRD_PARTY_NOTICES.md', 'LICENSES/DIALECTIC-MIT.txt',
           'LICENSES/CommonLibF4-GPL-3.0-or-later.txt', 'LICENSES/CommonLibF4-EXCEPTIONS.txt',
           'LICENSES/CommonLibF4-MIT.txt', 'LICENSES/spdlog-1.16.0-MIT.txt')
MEDIA = {'a': 'Data/Sound/FX/SYNTH/RadioDiagnostics/slot_a.wav',
         'b': 'Data/Sound/FX/SYNTH/RadioDiagnostics/slot_b.wav',
         'a_reused': 'Replacement/Sound/FX/SYNTH/RadioDiagnostics/slot_a.wav'}
MANIFEST = 'Radio/build-manifest.json'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def notice_source(path):
    if path.startswith('LICENSES/') and path != 'LICENSES/DIALECTIC-MIT.txt':
        return ROOT / 'packaging/licenses' / Path(path).name
    return ROOT / path


def check_dll(data):
    if not 4096 <= len(data) <= 32 * 1024 * 1024 or data[:2] != b'MZ':
        raise ValueError('bounded native DLL required')
    offset = struct.unpack_from('<I', data, 0x3C)[0]
    if offset + 26 > len(data) or data[offset:offset + 4] != b'PE\0\0':
        raise ValueError('invalid PE header')
    machine, characteristics = struct.unpack_from('<H', data, offset + 4)[0], struct.unpack_from('<H', data, offset + 22)[0]
    if machine != 0x8664 or not characteristics & 0x2000 or struct.unpack_from('<H', data, offset + 24)[0] != 0x20B:
        raise ValueError('Windows x64 PE32+ DLL required')


def allowed_names():
    names = set(MEDIA.values()) | set(NOTICES) | {MANIFEST, 'Data/' + PLUGIN_NAME,
        'Data/F4SE/Plugins/SYNTH.dll', 'Radio/radio-media-manifest.json',
        'Papyrus/radio-build-manifest.json', 'Papyrus/SYNTHNative.flg'}
    names |= {'Source/' + path for path in SOURCE_FILES}
    for name in SCRIPTS:
        names |= {'Data/Scripts/' + name + '.pex', 'Data/Scripts/Source/User/' + name + '.psc'}
    return names


def audit(directory):
    files = {}
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError('regular bundle directory required')
    for path in directory.rglob('*'):
        if path.is_symlink() or getattr(path.lstat(), 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError('linked entries are forbidden')
        if path.is_file():
            name = path.relative_to(directory).as_posix()
            if name not in allowed_names() or path.stat().st_size > 32 * 1024 * 1024:
                raise ValueError('unowned or oversized bundle entry: ' + name)
            files[name] = path.read_bytes()
    if set(files) != allowed_names():
        raise ValueError('bundle allowlist mismatch')
    manifest = json.loads(files[MANIFEST])
    expected = {'schema': 'synth.radio.prototype.bundle.v1', 'lane': 'flat',
                'runtime': '1.11.240', 'installed': False, 'enabled_by_default': False,
                'native_playback_verified': False, 'live_slot_refresh_verified': False,
                'dcr_insertion_implemented': False, 'vr_radio_supported': False,
                'files': {name: digest(data) for name, data in sorted(files.items()) if name != MANIFEST}}
    if manifest != expected:
        raise ValueError('bundle ownership/provenance mismatch')
    verify_plugin(files['Data/' + PLUGIN_NAME])
    check_dll(files['Data/F4SE/Plugins/SYNTH.dll'])
    for path in SOURCE_FILES:
        if files['Source/' + path] != (ROOT / path).read_bytes():
            raise ValueError('stale original source: ' + path)
    for path in NOTICES:
        if files[path] != notice_source(path).read_bytes():
            raise ValueError('notice mismatch: ' + path)
    if files['Papyrus/SYNTHNative.flg'] != (ROOT / 'scripts/papyrus/SYNTHNative.flg').read_bytes():
        raise ValueError('flags mismatch')
    with tempfile.TemporaryDirectory(prefix='synth-radio-audit-') as temporary:
        staged = Path(temporary)
        (staged / 'radio-build-manifest.json').write_bytes(files['Papyrus/radio-build-manifest.json'])
        for name in SCRIPTS:
            (staged / (name + '.pex')).write_bytes(files['Data/Scripts/' + name + '.pex'])
            if files['Data/Scripts/Source/User/' + name + '.psc'] != (ROOT / 'scripts/papyrus' / (name + '.psc')).read_bytes():
                raise ValueError('unowned Papyrus source')
        verify_scripts(staged)
    media = json.loads(files['Radio/radio-media-manifest.json'])
    if media.get('kind') != 'synth.radio.diagnostic.media' or media.get('sequence') != ['a', 'b', 'a_reused'] or set(media.get('clips', {})) != set(MEDIA):
        raise ValueError('wrong media identity')
    if any(media.get(key) is not False for key in ('installed', 'native_playback_verified', 'live_slot_refresh_verified')):
        raise ValueError('media claims game evidence')
    for key, path in MEDIA.items():
        expected_clip = inspect_wave(files[path])
        expected_clip.update(staged_path=path, engine_path='Data/Sound/FX/SYNTH/RadioDiagnostics/slot_' + ('b' if key == 'b' else 'a') + '.wav')
        if media['clips'][key] != expected_clip:
            raise ValueError('media bytes/path provenance mismatch')
    if media['clips']['a']['sha256'] == media['clips']['a_reused']['sha256'] or media['clips']['a']['duration_seconds'] == media['clips']['a_reused']['duration_seconds']:
        raise ValueError('reuse test needs different bytes and duration')
    return len(files)


def build(dll, papyrus, media, output):
    if not output.resolve().is_relative_to((ROOT / 'out').resolve()):
        raise ValueError('diagnostic build output must stay inside this checkout\'s out directory')
    if output.exists():
        raise FileExistsError('fresh build output required')
    verify_scripts(papyrus)
    entries = {'Data/' + PLUGIN_NAME: build_plugin(), 'Data/F4SE/Plugins/SYNTH.dll': dll.read_bytes(),
               'Papyrus/radio-build-manifest.json': (papyrus / 'radio-build-manifest.json').read_bytes(),
               'Papyrus/SYNTHNative.flg': (ROOT / 'scripts/papyrus/SYNTHNative.flg').read_bytes(),
               'Radio/radio-media-manifest.json': (media / 'radio-media-manifest.json').read_bytes()}
    check_dll(entries['Data/F4SE/Plugins/SYNTH.dll'])
    entries.update({path: (media / path).read_bytes() for path in MEDIA.values()})
    entries.update({path: notice_source(path).read_bytes() for path in NOTICES})
    entries.update({'Source/' + path: (ROOT / path).read_bytes() for path in SOURCE_FILES})
    for name in SCRIPTS:
        entries['Data/Scripts/' + name + '.pex'] = (papyrus / (name + '.pex')).read_bytes()
        entries['Data/Scripts/Source/User/' + name + '.psc'] = (ROOT / 'scripts/papyrus' / (name + '.psc')).read_bytes()
    manifest = {'schema': 'synth.radio.prototype.bundle.v1', 'lane': 'flat', 'runtime': '1.11.240',
                'installed': False, 'enabled_by_default': False, 'native_playback_verified': False,
                'live_slot_refresh_verified': False, 'dcr_insertion_implemented': False, 'vr_radio_supported': False,
                'files': {name: digest(data) for name, data in sorted(entries.items())}}
    entries[MANIFEST] = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode('utf-8')
    output.mkdir(parents=True, exist_ok=False)
    for name, data in entries.items():
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    count = audit(output)
    print(f'Original optional radio bundle audit passed: {count} files. Built only; not installed.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dll', type=Path)
    parser.add_argument('--papyrus', type=Path)
    parser.add_argument('--media', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--audit', type=Path)
    args = parser.parse_args()
    if args.audit:
        print('Radio bundle audit passed: ' + str(audit(args.audit)) + ' files')
    elif all((args.dll, args.papyrus, args.media, args.output)):
        build(args.dll, args.papyrus, args.media, args.output)
    else:
        parser.error('provide --audit or all build inputs')
