"""Compile the original flat wait controller twice with the pinned compiler and local game imports."""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import tempfile
from pathlib import Path
from verify_papyrus_pex import COMPILER_SHA256, COMPILER_RELEASE

ROOT = Path(__file__).resolve().parents[1]
FILES = ('Data/Scripts/SYNTHWait.pex', 'Data/Scripts/Source/User/SYNTHWait.psc',
         'Papyrus/wait-build-manifest.json', 'Papyrus/SYNTHWait.flg')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def canonical(data):
    """Normalize only PEX header metadata; validate bounded FO4/script identity, not game behavior."""
    if not 64 <= len(data) <= 131072 or data[:8] != bytes.fromhex('dec057fa03090200'):
        raise ValueError('invalid Fallout 4 wait PEX')
    offset = 16
    def string():
        nonlocal offset
        size = struct.unpack_from('<H', data, offset)[0]
        offset += 2
        value = data[offset:offset + size].decode('utf-8')
        offset += size
        if offset > len(data) or '\0' in value:
            raise ValueError('invalid PEX string')
        return value
    source, user, machine = string(), string(), string()
    if source.replace('\\', '/').split('/')[-1] != 'SYNTHWait.psc':
        raise ValueError('wrong wait script identity')
    body_start = offset
    count = struct.unpack_from('<H', data, offset)[0]
    offset += 2
    strings = [string() for _ in range(count)]
    # Debug is disabled, then user flags precede one script object.
    if data[offset] != 0:
        raise ValueError('unexpected debug info')
    offset += 1
    flags = struct.unpack_from('<H', data, offset)[0]
    offset += 2 + flags * 3
    objects, name = struct.unpack_from('<HH', data, offset)
    if objects != 1 or name >= len(strings) or strings[name].lower() != 'synthwait':
        raise ValueError('unowned executable script')
    if not {'Current', 'Finished', 'Observed', 'Apply', 'Reset', 'ApplyByFormIds', 'ResetByFormId',
            'RefreshByFormIds', 'HoldActor', 'ReleaseRestraint'}.issubset(strings):
        raise ValueError('missing wait entry points')
    header = data[:8] + bytes(8)
    for text in ('SYNTHWait.psc', '', ''):
        encoded = text.encode()
        header += struct.pack('<H', len(encoded)) + encoded
    return header + data[body_start:]


def verify_wait_bundle(pex, manifest, source, flags):
    if len(manifest) > 65536 or len(source) > 16384 or len(flags) > 4096:
        raise ValueError('oversized wait bundle')
    if source != (ROOT / 'scripts/papyrus/SYNTHWait.psc').read_bytes() or flags != (ROOT / 'scripts/papyrus/SYNTHNative.flg').read_bytes():
        raise ValueError('wait source is stale or unowned')
    record = json.loads(manifest)
    expected = {'schema': 'synth.wait.build.v1', 'compiler_release': COMPILER_RELEASE,
                'compiler_sha256': COMPILER_SHA256, 'source_sha256': digest(source),
                'flags_sha256': digest(flags), 'pex_sha256': digest(pex),
                'isolated_compiles': 2, 'gameplay_verified': False}
    if not isinstance(record, dict) or set(record) != set(expected) | {'imports'} or any(type(record[k]) is not type(v) or record[k] != v for k, v in expected.items()):
        raise ValueError('wait build provenance mismatch')
    imports = record['imports']
    if not isinstance(imports, dict) or not 1 <= len(imports) <= 256:
        raise ValueError('invalid wait imports')
    for name, value in imports.items():
        if not re.fullmatch(r'[a-z0-9_]+\.pex', name) or name.startswith('synth') or not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{64}', value):
            raise ValueError('unowned wait import')
    if canonical(pex) != pex:
        raise ValueError('noncanonical wait PEX')


def build(compiler, imports, output):
    if digest(compiler.read_bytes()) != COMPILER_SHA256:
        raise ValueError('compiler is not pinned Caprica v0.1.5')
    source = (ROOT / 'scripts/papyrus/SYNTHWait.psc').read_bytes()
    flags = (ROOT / 'scripts/papyrus/SYNTHNative.flg').read_bytes()
    dependencies = {}
    for path in imports.iterdir():
        if not path.is_file() or path.is_symlink() or path.suffix.lower() != '.pex' or not 8 <= path.stat().st_size <= 2097152:
            raise ValueError('invalid binary import')
        data = path.read_bytes()
        if data[:8] != bytes.fromhex('dec057fa03090200') or path.name.lower().startswith('synth'):
            raise ValueError('imports must be original flat game/SDK scripts')
        dependencies[path.name.lower()] = data
    if len(dependencies) > 256 or sum(map(len, dependencies.values())) > 33554432:
        raise ValueError('excessive imports')
    results = []
    for _ in range(2):
        with tempfile.TemporaryDirectory(prefix='synth-wait-') as temporary:
            stage = Path(temporary)
            (stage / 'imports').mkdir()
            (stage / 'output').mkdir()
            (stage / 'SYNTHWait.psc').write_bytes(source)
            (stage / 'SYNTHWait.flg').write_bytes(flags)
            (stage / 'empty.cfg').write_bytes(b'')
            for name, data in dependencies.items():
                (stage / 'imports' / name).write_bytes(data)
            command = [str(compiler.resolve()), '--import', str(stage / 'imports'), '--flags', str(stage / 'SYNTHWait.flg'),
                       '--output', str(stage / 'output'), '--config-file', str(stage / 'empty.cfg'),
                       '--enable-debug-info=false', '--enable-language-extensions=false', '--enable-speculative-syntax=false',
                       '--champollion-compat=false', '--all-warnings-as-errors', str(stage / 'SYNTHWait.psc')]
            result = subprocess.run(command, cwd=stage, capture_output=True, text=True, timeout=60)
            log = result.stdout + result.stderr
            if result.returncode or 'error' in log.lower() or 'failed' in log.lower():
                raise ValueError('wait compile failed: ' + log)
            results.append(canonical((stage / 'output/SYNTHWait.pex').read_bytes()))
    if results[0] != results[1]:
        raise ValueError('wait compiles are not reproducible')
    manifest = {'schema': 'synth.wait.build.v1', 'compiler_release': COMPILER_RELEASE,
                'compiler_sha256': COMPILER_SHA256, 'source_sha256': digest(source), 'flags_sha256': digest(flags),
                'pex_sha256': digest(results[0]), 'isolated_compiles': 2, 'gameplay_verified': False,
                'imports': {name: digest(data) for name, data in sorted(dependencies.items())}}
    receipt = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode()
    verify_wait_bundle(results[0], receipt, source, flags)
    output.mkdir(parents=True, exist_ok=True)
    (output / 'SYNTHWait.pex').write_bytes(results[0])
    (output / 'wait-build-manifest.json').write_bytes(receipt)
    print('Wait PEX: two identical canonical compiles, SHA256 ' + digest(results[0]))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--imports', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'out/wait-papyrus')
    args = parser.parse_args()
    build(args.compiler, args.imports, args.output)
