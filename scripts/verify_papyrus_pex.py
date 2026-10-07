#!/usr/bin/env python3
"""Strictly inspect SYNTH's native-only Fallout 4 PEX; never a general PEX validator."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from verify_papyrus import ROOT, verify

COMPILER_SHA256 = '75d1f8acb87f5b5dc0b4694ef2cdea7fe0e0beeadd858a2fe8e059889201bfe9'
COMPILER_RELEASE = 'https://github.com/Orvid/Caprica/releases/tag/v0.1.5'


def verify_bundle(pex: bytes, manifest: bytes, source: bytes, flags: bytes) -> None:
    """Bind a canonical owned PEX to its exact source and closed compiler-provenance record."""
    if len(manifest) > 65536 or len(source) > 65536 or len(flags) > 4096:
        raise ValueError('oversized Papyrus bundle')
    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate build manifest key')
            result[key] = value
        return result
    record = json.loads(manifest, object_pairs_hook=unique_pairs)
    expected = {'schema': 'synth.papyrus.build.v1', 'compiler_release': COMPILER_RELEASE,
                'compiler_sha256': COMPILER_SHA256, 'game': 'fallout4', 'pex_version': '3.9',
                'native_functions': 6, 'isolated_compiles': 2, 'gameplay_verified': False,
                'source_sha256': hashlib.sha256(source).hexdigest(),
                'flags_sha256': hashlib.sha256(flags).hexdigest(),
                'pex_sha256': hashlib.sha256(pex).hexdigest()}
    if not isinstance(record, dict) or set(record) != set(expected) | {'imports'}:
        raise ValueError('invalid build manifest fields')
    if any(type(record[key]) is not type(value) or record[key] != value for key, value in expected.items()):
        raise ValueError('Papyrus source/compiler/artifact provenance mismatch')
    imports = record['imports']
    if not isinstance(imports, dict) or not 1 <= len(imports) <= 256 or 'scriptobject.pex' not in imports:
        raise ValueError('invalid binary import provenance')
    for name, digest in imports.items():
        if not re.fullmatch(r'[a-z0-9_]+\.pex', name) or name == 'synthnative.pex' \
                or not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest):
            raise ValueError('invalid binary import identity/hash')
    text = source.decode('utf-8')
    verify(text, (ROOT / 'src/adapters/commonlib_papyrus_api.hpp').read_text(encoding='utf-8'))
    if inspect_pex(pex, text) != pex:
        raise ValueError('Papyrus artifact contains noncanonical header metadata')


def inspect_pex(data: bytes, source: str) -> bytes:
    """Verify every object/function and return a header-anonymized deterministic artifact."""
    if not 32 <= len(data) <= 65536:
        raise ValueError("invalid native PEX size")
    position = 0

    def number(fmt: str) -> int:
        nonlocal position
        size = struct.calcsize('<' + fmt)
        if position + size > len(data):
            raise ValueError("truncated PEX")
        value = struct.unpack_from('<' + fmt, data, position)[0]
        position += size
        return value

    def string() -> str:
        nonlocal position
        length = number('H')
        if position + length > len(data):
            raise ValueError("truncated PEX string")
        value = data[position:position + length].decode('utf-8')
        position += length
        if '\0' in value:
            raise ValueError("embedded NUL in PEX string")
        return value

    def expect(value, expected, label: str) -> None:
        if value != expected:
            raise ValueError('unexpected ' + label)

    expect((number('I'), number('B'), number('B'), number('H')),
           (0xFA57C0DE, 3, 9, 2), 'Fallout 4 header')
    number('Q')  # Build time is non-semantic, removed only after full validation.
    if string().replace('\\', '/').split('/')[-1] != 'SYNTHNative.psc':
        raise ValueError('unexpected source file')
    string()  # Compiler user; do not log or publish it.
    string()  # Compiler machine; do not log or publish it.
    body_start = position
    count = number('H')
    if count > 128:
        raise ValueError('oversized native string table')
    strings = [string() for _ in range(count)]

    def ref() -> str:
        index = number('H')
        if index >= len(strings):
            raise ValueError('invalid string reference')
        return strings[index]

    expect(number('B'), 0, 'debug information')
    expect(number('H'), 1, 'user flag count')
    expect((ref().lower(), number('B')), ('hidden', 0), 'hidden flag')
    expect(number('H'), 1, 'object count')
    expect(ref(), 'SYNTHNative', 'object identity')
    object_size = number('I')
    object_start = position
    expect(ref().lower(), 'scriptobject', 'parent identity')
    expect(ref(), '', 'object documentation')
    expect(number('B'), 0, 'const flag')
    expect(number('I'), 1, 'object user flags')
    expect(ref(), '', 'auto state')
    for label in ('structs', 'variables', 'properties'):
        expect(number('H'), 0, label)
    expect(number('H'), 1, 'state count')
    expect(ref(), '', 'state name')
    expected = {}
    for result, name, arguments in re.findall(r'(Bool|Int) Function (\w+)\(([^)]*)\) Global Native', source):
        types = [part.strip().split()[0].lower() for part in arguments.split(',') if part.strip()]
        expected[name.lower()] = (result.lower(), types)
    expect(number('H'), len(expected), 'function count')
    found = {}
    for _ in expected:
        name = ref().lower()
        if name in found:
            raise ValueError('duplicate native function')
        result = ref().lower()
        expect(ref(), '', 'function documentation')
        expect(number('I'), 0, 'function user flags')
        expect(number('B'), 3, 'global/native flags')
        parameters = []
        for _ in range(number('H')):
            ref()  # Parameter name does not alter the native signature.
            parameters.append(ref().lower())
        expect(number('H'), 0, 'locals')
        expect(number('H'), 0, 'instructions')
        found[name] = (result, parameters)
    expect(found, expected, 'native signatures')
    expect(position - object_start, object_size, 'object size')
    expect(position, len(data), 'trailing content')
    source_name = b'SYNTHNative.psc'
    return (data[:8] + bytes(8) + struct.pack('<H', len(source_name)) + source_name
            + bytes(4) + data[body_start:])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pex', type=Path)
    parser.add_argument('--normalize-output', type=Path)
    args = parser.parse_args()
    source = (ROOT / 'scripts/papyrus/SYNTHNative.psc').read_text(encoding='utf-8')
    verify(source, (ROOT / 'src/adapters/commonlib_papyrus_api.hpp').read_text(encoding='utf-8'))
    canonical = inspect_pex(args.pex.read_bytes(), source)
    if args.normalize_output:
        if args.normalize_output.resolve() == args.pex.resolve():
            raise ValueError('normalization must not replace the compiler output')
        args.normalize_output.parent.mkdir(parents=True, exist_ok=True)
        args.normalize_output.write_bytes(canonical)
    print('Native PEX contract passed: Fallout 4 3.9; six global natives; no executable instructions. Gameplay unverified.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, UnicodeError, ValueError) as error:
        raise SystemExit('Native PEX audit failed: ' + str(error))
