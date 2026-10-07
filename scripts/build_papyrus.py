#!/usr/bin/env python3
"""Build only original SYNTH native declarations from a pinned compiler and local binary imports."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from verify_papyrus import ROOT, verify
from verify_papyrus_pex import COMPILER_SHA256, COMPILER_RELEASE, inspect_pex


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def build(compiler: Path, imports: Path, output: Path) -> None:
    """Compile twice in isolated directories and publish only validated owned output."""
    if os.name != 'nt':
        raise ValueError('the pinned compiler requires Windows')
    compiler = compiler.resolve(strict=True)
    if digest(compiler.read_bytes()) != COMPILER_SHA256:
        raise ValueError('compiler does not match pinned Caprica v0.1.5')
    if not imports.is_dir() or imports.is_symlink():
        raise ValueError('binary import directory required')
    imports = imports.resolve(strict=True)
    source = (ROOT / 'scripts/papyrus/SYNTHNative.psc').read_bytes()
    flags = (ROOT / 'scripts/papyrus/SYNTHNative.flg').read_bytes()
    verify(source.decode('utf-8'), (ROOT / 'src/adapters/commonlib_papyrus_api.hpp').read_text(encoding='utf-8'))
    dependencies = {}
    for path in sorted(imports.iterdir()):
        if not path.is_file() or path.is_symlink() or path.suffix.lower() != '.pex':
            raise ValueError('imports must contain only flat, regular PEX files')
        name = path.name.lower()
        if name == 'synthnative.pex' or name in dependencies or len(dependencies) >= 256:
            raise ValueError('duplicate, owned or excessive import files')
        if not 8 <= path.stat().st_size <= 2 * 1024 * 1024:
            raise ValueError('oversized binary import: ' + path.name)
        data = path.read_bytes()
        if data[:8] != bytes.fromhex('dec057fa03090200'):
            raise ValueError('import is not bounded Fallout 4 PEX 3.9: ' + path.name)
        dependencies[name] = data
    if sum(map(len, dependencies.values())) > 32 * 1024 * 1024:
        raise ValueError('binary imports exceed the total size bound')
    if 'scriptobject.pex' not in dependencies:
        raise ValueError('real ScriptObject.pex import required')
    artifacts = []
    for _ in range(2):
        with tempfile.TemporaryDirectory(prefix='synth-papyrus-') as temporary:
            stage = Path(temporary)
            input_dir, import_dir, output_dir = (stage / name for name in ('input', 'imports', 'output'))
            for directory in (input_dir, import_dir, output_dir):
                directory.mkdir()
            (input_dir / 'SYNTHNative.psc').write_bytes(source)
            (input_dir / 'SYNTHNative.flg').write_bytes(flags)
            (stage / 'empty.cfg').write_bytes(b'')
            for name, data in dependencies.items():
                (import_dir / name).write_bytes(data)
            command = [str(compiler), '--import', str(input_dir), '--import', str(import_dir),
                       '--flags', str(input_dir / 'SYNTHNative.flg'), '--output', str(output_dir),
                       '--config-file', str(stage / 'empty.cfg'), '--enable-debug-info=false',
                       '--enable-language-extensions=false', '--enable-speculative-syntax=false',
                       '--champollion-compat=false', '--all-warnings-as-errors',
                       str(input_dir / 'SYNTHNative.psc')]
            result = subprocess.run(command, cwd=stage, capture_output=True, text=True, timeout=30)
            log = (result.stdout + result.stderr).lower()
            # This compiler can return zero after a fatal error: never trust its exit status alone.
            if result.returncode or 'error' in log or 'failed' in log:
                raise ValueError('Papyrus compilation failed; verify the complete local binary imports')
            files = list(output_dir.iterdir())
            if len(files) != 1 or files[0].name != 'SYNTHNative.pex':
                raise ValueError('compiler did not produce exactly the owned PEX')
            canonical = inspect_pex(files[0].read_bytes(), source.decode('utf-8'))
            if inspect_pex(canonical, source.decode('utf-8')) != canonical:
                raise ValueError('PEX normalization was not idempotent')
            artifacts.append(canonical)
    if artifacts[0] != artifacts[1]:
        raise ValueError('isolated compiles were not reproducible')
    manifest = {'schema': 'synth.papyrus.build.v1', 'compiler_release': COMPILER_RELEASE,
                'compiler_sha256': COMPILER_SHA256, 'source_sha256': digest(source),
                'flags_sha256': digest(flags), 'pex_sha256': digest(artifacts[0]),
                'imports': {name: digest(data) for name, data in dependencies.items()},
                'game': 'fallout4', 'pex_version': '3.9', 'native_functions': 6,
                'isolated_compiles': 2, 'gameplay_verified': False}
    output = output.resolve()
    if output == imports or output in imports.parents or output == ROOT or output in ROOT.parents:
        raise ValueError('output must not replace a source/import root')
    output.mkdir(parents=True, exist_ok=True)
    # Publish only our PEX and its provenance manifest; never publish imported game scripts.
    with tempfile.TemporaryDirectory(prefix='synth-publish-', dir=output) as temporary:
        stage = Path(temporary)
        (stage / 'SYNTHNative.pex').write_bytes(artifacts[0])
        (stage / 'build-manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n', encoding='utf-8')
        for name in ('SYNTHNative.pex', 'build-manifest.json'):
            os.replace(stage / name, output / name)
    print('Papyrus build passed: two identical canonical compiles; PEX SHA256 ' + manifest['pex_sha256'])
    print('Build-only output. Not packaged, deployed or verified in the game.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--imports', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'out/papyrus')
    args = parser.parse_args()
    try:
        build(args.compiler, args.imports, args.output)
    except (OSError, UnicodeError, ValueError, subprocess.SubprocessError) as error:
        raise SystemExit('Papyrus build failed: ' + str(error))
