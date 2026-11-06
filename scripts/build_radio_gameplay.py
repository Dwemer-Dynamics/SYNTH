#!/usr/bin/env python3
"""Author a separate, opt-in, original native radio diagnostic plugin; never install it."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from build_gameplay import group, record, sub

PLUGIN_NAME = 'SYNTHRadioDiagnostics.esp'
FORM = {'quest': 0x01000800, 'transmitter_base': 0x01000801,
        'cell': 0x01000802, 'transmitter': 0x01000803,
        'receiver_base': 0x01000804, 'receiver': 0x01000805,
        'slot_a_scene': 0x01000806, 'slot_b_scene': 0x01000807,
        'slot_a_sound': 0x01000808, 'slot_b_sound': 0x01000809,
        'output': 0x0100080A, 'category': 0x0100080B}
FREQUENCY = 106.9
# Identifier reference only: Fallout4.esm's existing radio menu slider category.
RADIO_CATEGORY = 0x00021B16
FORMAT_SOURCE = 'https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/Core/wbDefinitionsFO4.pas'


def z(value):
    return value.encode('ascii') + b'\0'


def u(value):
    return struct.pack('<I', value)


def build_scene(slot):
    """One bounded, non-looping native radio action, referencing only our transmitter alias."""
    data = sub('EDID', z('SYNTH_RadioSlot' + slot.upper())) + sub('FNAM', u(0x100))
    data += sub('HNAM') + sub('NAM0', z('Diagnostic ' + slot.upper()))
    data += sub('NEXT') + sub('NEXT') + sub('WNAM', u(350)) + sub('HNAM')
    data += sub('ALID', u(0)) + sub('LNAM', u(0)) + sub('DNAM', u(0))
    data += sub('ANAM', struct.pack('<H', 6)) + sub('NAM0', z('Owned slot ' + slot.upper()))
    data += sub('ALID', u(0)) + sub('INAM', u(1)) + sub('SNAM', u(0)) + sub('ENAM', u(0))
    data += sub('DATA', u(0)) + sub('HTID', u(FORM['slot_' + slot + '_sound']))
    data += sub('DMAX', struct.pack('<f', 0)) + sub('ANAM')
    data += sub('PNAM', u(FORM['quest'])) + sub('INAM', u(1))
    data += sub('VNAM', struct.pack('<IIII', 3, 3, 3, 3)) + sub('XNAM', u(0 if slot == 'a' else 1))
    return record('SCEN', FORM['slot_' + slot + '_scene'], data)


def build_sound(slot):
    data = sub('EDID', z('SYNTH_RadioSound' + slot.upper()))
    data += sub('CNAM', u(0x1EEF540A)) + sub('GNAM', u(FORM['category']))
    data += sub('ANAM', z('Data\\Sound\\FX\\SYNTH\\RadioDiagnostics\\slot_' + slot + '.wav'))
    data += sub('ONAM', u(FORM['output'])) + sub('LNAM', bytes(4))
    data += sub('BNAM', struct.pack('<bbBBH', 0, 0, 128, 0, 0))
    return record('SNDR', FORM['slot_' + slot + '_sound'], data)


def build_plugin():
    header = sub('HEDR', struct.pack('<fII', 1.0, len(FORM), 0x80C))
    header += sub('CNAM', z('Dwemer Dynamics')) + sub('SNAM', z('Original native radio diagnostic; optional and disabled by default'))
    header += sub('MAST', z('Fallout4.esm')) + sub('DATA', bytes(8))
    # No start-game flag or scene begin-on-quest-start flag. Console opt-in starts the quest.
    quest = sub('EDID', z('SYNTH_RadioDiagnostics'))
    # VMAD v6, object format2, one original script, no properties/fragments/alias scripts.
    script = b'SYNTHRadioProbe'
    vmad = struct.pack('<HHH', 6, 2, 1) + struct.pack('<H', len(script)) + script + b'\0' + struct.pack('<H', 0)
    # The optional quest fragments/alias-script suffix is absent; none are authored.
    quest += sub('VMAD', vmad) + sub('FULL', z('SYNTH Radio Diagnostics'))
    quest += sub('DNAM', struct.pack('<HBBfI', 0, 255, 0, 0.0, 0))
    quest += sub('NEXT') + sub('ANAM', u(1))
    # Optional + allow disabled. StartProbe fills the exact owned transmitter after Start().
    # No fill-type subrecord or conditions: the empty alias does not block quest startup.
    quest += sub('ALST', u(0)) + sub('ALID', z('OwnedTransmitter')) + sub('FNAM', u(0x82))
    quest += sub('VTCK', u(0)) + sub('ALED')
    transmitter = sub('EDID', z('SYNTH_RadioTransmitter')) + sub('OBND', bytes(12))
    transmitter += sub('FULL', z('SYNTH Radio Diagnostics')) + sub('PNAM', bytes(4)) + sub('FNAM', bytes(2))
    transmitter = record('ACTI', FORM['transmitter_base'], transmitter)
    # A reference to an installed game mesh, never a redistributed game asset.
    receiver = sub('EDID', z('SYNTH_RadioReceiver')) + sub('OBND', bytes(12))
    receiver += sub('FULL', z('SYNTH Diagnostic Receiver')) + sub('MODL', z('SetDressing\\Radio\\Radio1.nif'))
    receiver += sub('FNAM', struct.pack('<H', 0x10))
    receiver += sub('RADR', struct.pack('<IffBB', FORM['output'], FREQUENCY, 1.0, 1, 0))
    receiver = record('ACTI', FORM['receiver_base'], receiver)
    output = sub('EDID', z('SYNTH_RadioOutput')) + sub('NAM1', bytes(4)) + sub('MNAM', u(1))
    output += sub('VNAM', struct.pack('<h', 0))
    output += sub('ONAM', bytes([100, 100, 0, 0, 50, 50, 50, 50]) * 3)
    category = sub('EDID', z('SYNTH_RadioCategory')) + sub('FNAM', u(4))
    category += sub('PNAM', u(RADIO_CATEGORY)) + sub('ONAM', u(RADIO_CATEGORY))
    category += sub('VNAM', struct.pack('<H', 65535)) + sub('UNAM', struct.pack('<H', 65535))
    category += sub('MNAM', struct.pack('<f', 0.1)) + sub('CNAM', struct.pack('<f', 1.0))
    refs = sub('EDID', z('SYNTH_RadioTransmitterRef')) + sub('NAME', u(FORM['transmitter_base']))
    refs += sub('XRDO', struct.pack('<fffI', FREQUENCY, 0, 0, 1)) + sub('DATA', bytes(24))
    refs = record('REFR', FORM['transmitter'], refs, 0xC00)  # Persistent + initially disabled.
    receiver_ref = sub('EDID', z('SYNTH_RadioReceiverRef')) + sub('NAME', u(FORM['receiver_base']))
    receiver_ref += sub('DATA', struct.pack('<ffffff', 128, 0, 64, 0, 0, 0))
    refs += record('REFR', FORM['receiver'], receiver_ref, 0xC00)
    cell = record('CELL', FORM['cell'], sub('EDID', z('SYNTHRadioDiagnosticCell')) + sub('DATA', b'\x01\x00'))
    cell += group(FORM['cell'], 6, group(FORM['cell'], 8, refs))
    # FO4 stores SCEN records in the owning quest's type-10 child group.
    return record('TES4', 0, header) + group(b'ACTI', 0, transmitter + receiver) + \
        group(b'SNCT', 0, record('SNCT', FORM['category'], category)) + \
        group(b'SOPM', 0, record('SOPM', FORM['output'], output)) + \
        group(b'SNDR', 0, build_sound('a') + build_sound('b')) + \
        group(b'QUST', 0, record('QUST', FORM['quest'], quest) +
              group(FORM['quest'], 10, build_scene('a') + build_scene('b'))) + \
        group(b'CELL', 0, group(0, 2, group(5, 3, cell)))


def verify_plugin(data):
    if data != build_plugin():
        raise ValueError('diagnostic plugin differs from the original deterministic record allowlist')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.name != PLUGIN_NAME:
        raise SystemExit('output must use the separate diagnostic plugin name')
    data = build_plugin()
    verify_plugin(data)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    manifest = {'schema': 'synth.radio.gameplay.build.v1', 'plugin': PLUGIN_NAME,
                'sha256': hashlib.sha256(data).hexdigest(), 'master': 'Fallout4.esm',
                'forms': {key: f'{value:08X}' for key, value in FORM.items()},
                'record_count': len(FORM), 'frequency': FREQUENCY,
                'format_reference': FORMAT_SOURCE, 'overrides': [], 'enabled_by_default': False,
                'flat_runtime': '1.11.240', 'vr_supported': False, 'gameplay_verified': False}
    args.output.with_suffix('.manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    print('Built original diagnostic records; no installation or game proof: ' + str(args.output))


if __name__ == '__main__':
    main()
