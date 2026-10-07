"""Author SYNTH's saved wait ownership aliases and legacy package records."""
from pathlib import Path
import argparse
import struct

PLUGIN_NAME = 'SYNTH.esp'
WAIT_FORM = 0x01000800
WAIT_TEMPLATE = 0x00002CB1  # Fallout4.esm: Sandbox, public inputs verified against the master.
HOLD_TEMPLATE = 0x0001D415  # Fallout4.esm: HoldPosition, also used by Preston's vanilla hold overrides.
WAIT_SLOTS = 256
WAIT_QUEST = 0x01000801
MARKER_CELL = 0x01000802
MARKER_BASE = 0x01000803
PACKAGE_BASE = 0x01000900
MARKER_FIRST = 0x01000A00


def sub(tag, data=b''):
    return tag.encode('ascii') + struct.pack('<H', len(data)) + data


def record(tag, form, data, flags=0):
    return struct.pack('<4sIIIIHH', tag.encode(), len(data), flags, form, 0, 131, 0) + data


def build_package(form=WAIT_FORM, marker=None):
    """Each alias package references its own permanent marker; the old record is migration-only."""
    package = sub('EDID', (f'SYNTH_WaitHere_{form & 0xFFFFFF:06X}\0').encode())
    if marker is not None:
        # A real HoldPosition procedure, not a Sandbox procedure with its activities disabled.
        # One public Location input (index 0), zero radius, and no world-interaction interrupts.
        # PKDT byte 5 is Interrupt Override, NOT the procedure type. Combat (4)
        # excludes this from ordinary alias package selection; use None (0).
        # HoldPosition is selected by the PKCU template below.
        package += sub('PKDT', struct.pack('<IBBBBHH', 0x100004, 18, 0, 0, 0, 0, 0))
        package += sub('PSDT', bytes.fromhex('ffff00ffff00000000000000'))
        package += sub('PKCU', struct.pack('<III', 1, HOLD_TEMPLATE, 1))
        package += sub('ANAM', b'Location\0') + sub('PLDT', struct.pack('<iiii', 0, marker, 0, 0))
        package += sub('UNAM', b'\0') + sub('XNAM', b'\x01')
        for event in ('POBA', 'POEA', 'POCA'):
            package += sub(event) + sub('INAM', bytes(4)) + sub('PDTO', bytes(8))
        return record('PACK', form, package)
    # Preserve the old local-800 Sandbox record solely for saved run-once migration.
    # Must Complete + Ignore Combat. No ambient world-interaction interrupts.
    package += sub('PKDT', struct.pack('<IBBBBHH', 0x100004, 18, 0, 0, 0, 0, 0))
    package += sub('PSDT', bytes.fromhex('ffff00ffff00000000000000'))
    package += sub('PKCU', struct.pack('<III', 15, WAIT_TEMPLATE, 7))
    # New packages use type 0 (Near Reference); type 2 survives only in the migration record.
    package += sub('ANAM', b'Location\0') + sub('PLDT', struct.pack('<iiii', 0 if marker else 2, marker or 0, 64, 0))
    # Unlock doors, eating, sleeping, conversation, idle markers, sitting, special furniture,
    # wandering, and preferred-path wandering are all disabled. Direct dialogue still works.
    for _ in range(9):
        package += sub('ANAM', b'Bool\0') + sub('CNAM', b'\0')
    package += sub('ANAM', b'Float\0') + sub('CNAM', struct.pack('<f', 50.0))  # Default energy
    for _ in range(2):  # Owned refs only; prefer preferred path.
        package += sub('ANAM', b'Bool\0') + sub('CNAM', b'\0')
    package += sub('ANAM', b'TargetSelector\0') + sub('PTDA', struct.pack('<iii', 2, 0, 0))
    package += sub('ANAM', b'Float\0') + sub('CNAM', struct.pack('<f', 150.0))
    for index in (2, 12, 5, 6, 7, 8, 9, 14, 10, 22, 18, 20, 16, 24, 26):
        package += sub('UNAM', bytes([index]))
    package += sub('XNAM', b'\x1b')
    for marker in ('POBA', 'POEA', 'POCA'):
        package += sub(marker) + sub('INAM', bytes(4)) + sub('PDTO', bytes(8))
    return record('PACK', form, package)


def group(label, kind, data):
    if isinstance(label, int):
        label = struct.pack('<I', label)
    return struct.pack('<4sI4sIII', b'GRUP', len(data) + 24, label, kind, 0, 0) + data


def build_plugin():
    """Keep wait and restraint ownership persistent without injecting NPC AI packages."""
    header = sub('HEDR', struct.pack('<fII', 1.0, 4 + 2 * WAIT_SLOTS, 0xB00))
    header += sub('CNAM', b'Dwemer Dynamics\0') + sub('SNAM', b'SYNTH owned NPC wait aliases and anchors\0')
    header += sub('MAST', b'Fallout4.esm\0') + sub('DATA', bytes(8))
    # The quest is explicitly started by the bounded script command, not a start-game quest.
    quest = sub('EDID', b'SYNTH_WaitOwnership\0')
    quest += sub('DNAM', struct.pack('<HBBfI', 0, 255, 0, 0.0, 0))
    quest += sub('NEXT') + sub('ANAM', struct.pack('<I', 2 * WAIT_SLOTS))
    packages = build_package()  # Keep local 800 resolvable to release old saves' run-once instance.
    markers = b''
    for slot in range(WAIT_SLOTS):
        quest += sub('ALST', struct.pack('<I', slot)) + sub('ALID', f'WaitNPC{slot}\0'.encode())
        # Optional, allow reuse/reserved, actors only. No essential/protected/quest-object flags.
        # No fill-type subrecord: intentionally empty, like Fallout4.esm's optional script-filled aliases.
        quest += sub('FNAM', struct.pack('<I', 0x4020A))
        # Wait ownership no longer supplies a competing AI package. Keep old PACK/REFR IDs
        # resolvable for existing saves, but use the actor's restraint state for the hold.
        quest += sub('VTCK', bytes(4)) + sub('ALED')
        packages += build_package(PACKAGE_BASE + slot, MARKER_FIRST + slot)
        data = sub('EDID', f'SYNTH_WaitAnchor{slot}\0'.encode())
        data += sub('NAME', struct.pack('<I', MARKER_BASE)) + sub('DATA', bytes(24))
        markers += record('REFR', MARKER_FIRST + slot, data, 0x400)  # Persistent, owned invisible marker.
    # Separate saved receipts identify only restraint transitions SYNTH actually applied.
    # Existing restraint is never claimed and therefore never cleared by Release/Reset.
    for slot in range(WAIT_SLOTS):
        quest += sub('ALST', struct.pack('<I', WAIT_SLOTS + slot))
        quest += sub('ALID', f'RestraintOwned{slot}\0'.encode())
        quest += sub('FNAM', struct.pack('<I', 0x4020A))
        quest += sub('VTCK', bytes(4)) + sub('ALED')
    marker_base = record('STAT', MARKER_BASE, sub('EDID', b'SYNTH_WaitAnchorBase\0') + sub('OBND', bytes(12)))
    cell = record('CELL', MARKER_CELL, sub('EDID', b'SYNTH_InternalWaitAnchors\0') + sub('DATA', b'\x01\x00'))
    cell += group(MARKER_CELL, 6, group(MARKER_CELL, 8, markers))
    cells = group(b'CELL', 0, group(0, 2, group(5, 3, cell)))
    return record('TES4', 0, header) + group(b'STAT', 0, marker_base) + group(b'PACK', 0, packages) + \
        group(b'QUST', 0, record('QUST', WAIT_QUEST, quest)) + cells


def verify_plugin(data):
    """Release allowlist is byte-exact, including master, location, inputs and record ownership."""
    if data != build_plugin():
        raise ValueError('SYNTH.esp is not the original deterministic wait package')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(build_plugin())
    verify_plugin(args.output.read_bytes())
    print('Authored and verified: ' + str(args.output))
