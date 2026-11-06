#!/usr/bin/env python3
"""Compile only the two original radio probe scripts twice; never ship local SDK imports."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

from verify_papyrus_pex import COMPILER_RELEASE, COMPILER_SHA256

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = {'SYNTHRadioProbe': ('Quest', {'EnableProbe', 'StartProbe', 'ShowStatus', 'PlayA', 'PlayB', 'StopProbe', 'DisableProbe', 'Observe'}),
           'SYNTHRadioProbeNative': ('ScriptObject', {'Generation', 'Request', 'Observation'})}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def canonical(data, name):
    """Validate one bounded owned script identity; anonymize only compiler header metadata."""
    if name not in SCRIPTS or not 64 <= len(data) <= 65536 or data[:8] != bytes.fromhex('dec057fa03090200'):
        raise ValueError('invalid original radio PEX')
    offset = 16
    def number():
        nonlocal offset
        if offset + 2 > len(data):
            raise ValueError('truncated PEX')
        result = struct.unpack_from('<H', data, offset)[0]
        offset += 2
        return result
    def string():
        nonlocal offset
        size = number()
        if offset + size > len(data):
            raise ValueError('truncated PEX string')
        result = data[offset:offset + size].decode('utf-8')
        offset += size
        if '\0' in result:
            raise ValueError('invalid PEX string')
        return result
    source = string()
    string()  # User metadata is never emitted.
    string()  # Machine metadata is never emitted.
    if source.replace('\\', '/').split('/')[-1] != name + '.psc':
        raise ValueError('wrong radio script identity')
    body_start = offset
    count = number()
    if count > 256:
        raise ValueError('excessive radio PEX strings')
    strings = [string() for _ in range(count)]
    if offset >= len(data) or data[offset] != 0:
        raise ValueError('debug metadata is forbidden')
    offset += 1
    flags = number()
    if flags > 32 or offset + flags * 3 > len(data):
        raise ValueError('invalid PEX flags')
    offset += flags * 3
    objects, object_name = number(), number()
    if objects != 1 or object_name >= len(strings) or strings[object_name] != name:
        raise ValueError('unowned script object')
    if offset + 6 > len(data):
        raise ValueError('truncated script object')
    size = struct.unpack_from('<I', data, offset)[0]
    offset += 4
    parent = number()
    if parent >= len(strings) or strings[parent].lower() != SCRIPTS[name][0].lower() or offset - 2 + size != len(data):
        raise ValueError('wrong script parent or object length')
    if not SCRIPTS[name][1].issubset(strings):
        raise ValueError('missing original probe entry points')
    header = data[:8] + bytes(8)
    for value in (name + '.psc', '', ''):
        encoded = value.encode('ascii')
        header += struct.pack('<H', len(encoded)) + encoded
    return header + data[body_start:]


def verify_bundle(directory):
    manifest_path = directory / 'radio-build-manifest.json'
    if manifest_path.stat().st_size > 65536:
        raise ValueError('oversized provenance manifest')
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    expected = {'schema': 'synth.radio.papyrus.build.v1', 'compiler_release': COMPILER_RELEASE,
                'compiler_sha256': COMPILER_SHA256, 'isolated_compiles': 2,
                'gameplay_verified': False,
                'flags_sha256': digest((ROOT / 'scripts/papyrus/SYNTHNative.flg').read_bytes())}
    if set(manifest) != set(expected) | {'scripts', 'imports'} or any(type(manifest[k]) is not type(v) or manifest[k] != v for k, v in expected.items()):
        raise ValueError('radio compiler provenance mismatch')
    if set(manifest['scripts']) != set(SCRIPTS):
        raise ValueError('wrong original script allowlist')
    for name in SCRIPTS:
        source = (ROOT / 'scripts/papyrus' / (name + '.psc')).read_bytes()
        data = (directory / (name + '.pex')).read_bytes()
        if manifest['scripts'][name] != {'source_sha256': digest(source), 'pex_sha256': digest(data)} or canonical(data, name) != data:
            raise ValueError('radio source/artifact provenance mismatch')
    imports = manifest['imports']
    if not isinstance(imports, dict) or not 1 <= len(imports) <= 256 or 'quest.pex' not in imports or 'scriptobject.pex' not in imports:
        raise ValueError('invalid SDK import provenance')
    for name, value in imports.items():
        if not re.fullmatch(r'[a-z0-9_]+\.pex', name) or name.startswith('synth') or not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{64}', value):
            raise ValueError('invalid import hash/identity')
    return manifest


def build(compiler, imports, output):
    if digest(compiler.read_bytes()) != COMPILER_SHA256:
        raise ValueError('compiler is not pinned Caprica v0.1.5')
    sources = {name: (ROOT / 'scripts/papyrus' / (name + '.psc')).read_bytes() for name in SCRIPTS}
    flags = (ROOT / 'scripts/papyrus/SYNTHNative.flg').read_bytes()
    dependencies = {}
    for path in imports.iterdir():
        if path.is_symlink() or not path.is_file() or path.suffix.lower() != '.pex' or not 8 <= path.stat().st_size <= 2097152:
            raise ValueError('imports must be bounded regular PEX files')
        name = path.name.lower()
        data = path.read_bytes()
        if name in dependencies or name.startswith('synth') or data[:8] != bytes.fromhex('dec057fa03090200'):
            raise ValueError('duplicate, owned or invalid SDK import')
        dependencies[name] = data
    if not 1 <= len(dependencies) <= 256 or sum(map(len, dependencies.values())) > 33554432:
        raise ValueError('excessive imports')
    results = []
    for _ in range(2):
        with tempfile.TemporaryDirectory(prefix='synth-radio-') as temporary:
            stage = Path(temporary)
            (stage / 'imports').mkdir()
            (stage / 'output').mkdir()
            for name, source in sources.items():
                (stage / (name + '.psc')).write_bytes(source)
            (stage / 'probe.flg').write_bytes(flags)
            (stage / 'empty.cfg').write_bytes(b'')
            for name, data in dependencies.items():
                (stage / 'imports' / name).write_bytes(data)
            for name in SCRIPTS:
                command = [str(compiler.resolve()), '--import', str(stage), '--import', str(stage / 'imports'),
                           '--flags', str(stage / 'probe.flg'), '--output', str(stage / 'output'),
                           '--config-file', str(stage / 'empty.cfg'), '--enable-debug-info=false',
                           '--enable-language-extensions=false', '--enable-speculative-syntax=false',
                           '--champollion-compat=false', '--all-warnings-as-errors', str(stage / (name + '.psc'))]
                result = subprocess.run(command, cwd=stage, capture_output=True, text=True, timeout=60)
                if result.returncode or 'error' in (result.stdout + result.stderr).lower() or 'failed' in (result.stdout + result.stderr).lower():
                    raise ValueError('radio Papyrus compilation failed; inspect the local compiler/imports')
            artifacts = {name: canonical((stage / 'output' / (name + '.pex')).read_bytes(), name) for name in SCRIPTS}
            if {path.name for path in (stage / 'output').iterdir()} != {name + '.pex' for name in SCRIPTS}:
                raise ValueError('unowned compiler output')
            results.append(artifacts)
    if results[0] != results[1]:
        raise ValueError('radio compiles were not reproducible')
    manifest = {'schema': 'synth.radio.papyrus.build.v1', 'compiler_release': COMPILER_RELEASE,
                'compiler_sha256': COMPILER_SHA256, 'flags_sha256': digest(flags),
                'isolated_compiles': 2, 'gameplay_verified': False,
                'scripts': {name: {'source_sha256': digest(sources[name]), 'pex_sha256': digest(results[0][name])} for name in SCRIPTS},
                'imports': {name: digest(data) for name, data in sorted(dependencies.items())}}
    output.mkdir(parents=True, exist_ok=True)
    for name, data in results[0].items():
        (output / (name + '.pex')).write_bytes(data)
    (output / 'radio-build-manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    verify_bundle(output)
    print('Radio scripts: two identical canonical compiles; no SDK bytes published, no deployment.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--imports', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'out/radio-papyrus')
    args = parser.parse_args()
    build(args.compiler, args.imports, args.output)
