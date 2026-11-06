"""Author SYNTH's original owned Wait packages, saved aliases and legacy records."""
from pathlib import Path
import argparse
import struct

# Record version 131 uses month 0 for Any (the encoding changed at version 122).
# Pre-122 vanilla records use FF; copying that byte into modern records disables the schedule.
PLUGIN_NAME = 'SYNTH.esp'
WAIT_FORM = 0x01000800
WAIT_TEMPLATE = 0x00002CB1  # Fallout4.esm: Sandbox, public inputs verified against the master.
OWNED_WAIT_TEMPLATE = 0x001A32D6  # Fallout4.esm: ordinary Wait, public Bool/Float inputs audited in the master.
WAIT_SLOTS = 256
WAIT_QUEST = 0x01000801
WAIT_QUEST_PRIORITY = 100  # QUEST_DATA::priority is signed int8; 0xFF would become -1.
MARKER_CELL = 0x01000802
MARKER_BASE = 0x01000803
PACKAGE_BASE = 0x01000900
MARKER_FIRST = 0x01000A00
APPROACH_QUEST = 0x01000B00
APPROACH_PACKAGE_FIRST = 0x01000B10
APPROACH_SLOTS = 8
FOLLOW_TEMPLATE = 0x01000B20  # Original unconditional Follow procedure, audited public five-input layout.
VISIT_QUEST = 0x01000B30
VISIT_PACKAGE_FIRST = 0x01000B40
FOLLOW_MIN_RADIUS = 128
FOLLOW_MAX_RADIUS = 192


def sub(tag, data=b''):
    return tag.encode('ascii') + struct.pack('<H', len(data)) + data


def record(tag, form, data, flags=0):
    return struct.pack('<4sIIIIHH', tag.encode(), len(data), flags, form, 0, 131, 0) + data


def build_package(form=WAIT_FORM, marker=None):
    """Live aliases use ordinary Wait; local 800 is migration-only."""
    package = sub('EDID', (f'SYNTH_WaitHere_{form & 0xFFFFFF:06X}\0').encode())
    if marker is not None:
        # Ordinary Wait, not the combat HoldPosition template. Keep the existing
        # owned flags and ordinary Interrupt Override (0); native ownership bounds its lifetime.
        package += sub('PKDT', struct.pack('<IBBBBHH', 0x100004, 18, 0, 0, 0, 0, 0))
        package += sub('PSDT', bytes.fromhex('00ff00ffff00000000000000'))
        package += sub('PKCU', struct.pack('<III', 2, OWNED_WAIT_TEMPLATE, 2))
        # Public inputs 10=False and 11=ActualSeconds match the normal vanilla wrapper.
        # No Location/marker or target input is needed by this Wait procedure.
        package += sub('ANAM', b'Bool\0') + sub('CNAM', b'\0')
        package += sub('ANAM', b'Float\0') + sub('CNAM', struct.pack('<f', 0.0))
        package += sub('UNAM', b'\x0a') + sub('UNAM', b'\x0b') + sub('XNAM', b'\x0c')
        for event in ('POBA', 'POEA', 'POCA'):
            package += sub(event) + sub('INAM', bytes(4)) + sub('PDTO', bytes(8))
        return record('PACK', form, package)
    # Preserve the old local-800 Sandbox record solely for saved run-once migration.
    # Must Complete + Ignore Combat. No ambient world-interaction interrupts.
    package += sub('PKDT', struct.pack('<IBBBBHH', 0x100004, 18, 0, 0, 0, 0, 0))
    package += sub('PSDT', bytes.fromhex('00ff00ffff00000000000000'))
    package += sub('PKCU', struct.pack('<III', 15, WAIT_TEMPLATE, 7))
    # Preserve the migration record's original Near Package Start/radius64 input.
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


def follow_inputs(target):
    data = sub('ANAM', b'SingleRef\0') + sub('PTDA', struct.pack('<iii', *target))
    for radius in (FOLLOW_MIN_RADIUS, FOLLOW_MAX_RADIUS):
        data += sub('ANAM', b'Float\0') + sub('CNAM', struct.pack('<f', radius))
    for enabled in (True, False):
        data += sub('ANAM', b'Bool\0') + sub('CNAM', bytes([enabled]))
    for index in (0, 1, 2, 4, 6):
        data += sub('UNAM', bytes([index]))
    return data + sub('XNAM', b'\x07')


def package_events():
    return b''.join(sub(event) + sub('INAM', bytes(4)) + sub('PDTO', bytes(8))
                    for event in ('POBA', 'POEA', 'POCA'))


def build_follow_template():
    """Original native Follow tree, without game quest conditions or teleport branches."""
    data = sub('EDID', b'SYNTH_BoundedFollowTemplate\0')
    data += sub('PKDT', struct.pack('<IBBBBHH', 0, 19, 0, 0, 0, 0, 0))
    data += sub('PSDT', bytes.fromhex('00ff00ffff00000000000000'))
    data += sub('PKCU', struct.pack('<III', 5, 0, 1))
    data += follow_inputs((0, 0, 0))  # A missing wrapper/alias never defaults to a real actor.
    data += sub('ANAM', b'Stacked\0') + sub('CITC', bytes(4))
    data += sub('PRCB', struct.pack('<II', 1, 1))  # One child; Repeat when Complete.
    data += sub('ANAM', b'Procedure\0') + sub('CITC', bytes(4))
    data += sub('PNAM', b'Follow\0') + sub('FNAM', bytes(4))  # Success does not end the package.
    for index in (0, 1, 2, 6, 6, 6):
        data += sub('PKC2', bytes([index]))
    for index, label, editable in ((0, 'ActorToFollow', 1), (1, 'MinimumDistance', 1),
                                    (2, 'MaximumDistance', 1), (4, 'Enabled', 0), (6, 'Disabled', 0)):
        data += sub('UNAM', bytes([index])) + sub('BNAM', (label + '\0').encode())
        data += sub('PNAM', struct.pack('<I', editable))
    return record('PACK', FOLLOW_TEMPLATE, data + package_events())


def build_follow_package(slot, *, visit=False):
    """Alias-owned continuous navigation; public inputs contain only the exact saved listener alias."""
    if not 0 <= slot < APPROACH_SLOTS:
        raise ValueError('invalid approach slot')
    owner = VISIT_QUEST if visit else APPROACH_QUEST
    first = VISIT_PACKAGE_FIRST if visit else APPROACH_PACKAGE_FIRST
    label = 'SYNTH_SocialVisit' if visit else 'SYNTH_ConversationApproach'
    package = sub('EDID', f'{label}{slot}\0'.encode())
    # Normal combat/scene interruption. No Must Complete, Ignore Combat, warp, or recruitment flags.
    package += sub('PKDT', struct.pack('<IBBBBHH', 0, 18, 0, int(visit), 0, 0, 0))
    package += sub('PSDT', bytes.fromhex('00ff00ffff00000000000000'))
    package += sub('QNAM', struct.pack('<I', owner))
    package += sub('PKCU', struct.pack('<III', 5, FOLLOW_TEMPLATE, 1))
    package += follow_inputs((4, APPROACH_SLOTS + slot, 0))
    return record('PACK', first + slot, package + package_events())


def build_approach_package(slot):
    return build_follow_package(slot)


def build_approach_quest(*, visit=False):
    """Separate ownership from all saved Wait aliases. The native bridge fills these aliases."""
    label = 'SYNTH_SocialVisitOwnership' if visit else 'SYNTH_ConversationApproachOwnership'
    first = VISIT_PACKAGE_FIRST if visit else APPROACH_PACKAGE_FIRST
    quest = sub('EDID', (label + '\0').encode())
    # Explicitly started, bounded optional aliases. No quest scripts or start-game flags.
    quest += sub('DNAM', struct.pack('<HBBfI', 0, 100, 0, 0.0, 0))
    quest += sub('NEXT') + sub('ANAM', struct.pack('<I', 2 * APPROACH_SLOTS))
    for slot in range(2 * APPROACH_SLOTS):
        quest += sub('ALST', struct.pack('<I', slot))
        name = f'ApproachNPC{slot}' if slot < APPROACH_SLOTS else f'ApproachListener{slot - APPROACH_SLOTS}'
        quest += sub('ALID', (name + '\0').encode())
        quest += sub('FNAM', struct.pack('<I', 0x4020A))
        if slot < APPROACH_SLOTS:
            quest += sub('ALPC', struct.pack('<I', first + slot))
        quest += sub('VTCK', bytes(4)) + sub('ALED')
    return record('QUST', VISIT_QUEST if visit else APPROACH_QUEST, quest)


def group(label, kind, data):
    if isinstance(label, int):
        label = struct.pack('<I', label)
    return struct.pack('<4sI4sIII', b'GRUP', len(data) + 24, label, kind, 0, 0) + data


def build_plugin():
    """Keep original record identities while binding separate owned AI packages."""
    header = sub('HEDR', struct.pack('<fII', 1.0, 7 + 2 * WAIT_SLOTS + 2 * APPROACH_SLOTS, 0xB48))
    header += sub('CNAM', b'Dwemer Dynamics\0') + sub('SNAM', b'SYNTH owned NPC wait aliases and anchors\0')
    header += sub('MAST', b'Fallout4.esm\0') + sub('DATA', bytes(8))
    # The quest is explicitly started by the bounded script command, not a start-game quest.
    quest = sub('EDID', b'SYNTH_WaitOwnership\0')
    quest += sub('DNAM', struct.pack('<HbBfI', 0, WAIT_QUEST_PRIORITY, 0, 0.0, 0))
    quest += sub('NEXT') + sub('ANAM', struct.pack('<I', 2 * WAIT_SLOTS))
    packages = build_follow_template()
    packages += b''.join(build_approach_package(slot) for slot in range(APPROACH_SLOTS))
    packages += b''.join(build_follow_package(slot, visit=True) for slot in range(APPROACH_SLOTS))
    packages += build_package()  # Keep local 800 resolvable to release old saves' run-once instance.
    markers = b''
    for slot in range(WAIT_SLOTS):
        quest += sub('ALST', struct.pack('<I', slot)) + sub('ALID', f'WaitNPC{slot}\0'.encode())
        # Optional, allow reuse/reserved, actors only. No essential/protected/quest-object flags.
        # No fill-type subrecord: intentionally empty, like Fallout4.esm's optional script-filled aliases.
        quest += sub('FNAM', struct.pack('<I', 0x4020A))
        # ALPC is the alias package field (ALSP would assign a spell). Bind this
        # exact original Wait package at the existing quest's priority.
        quest += sub('ALPC', struct.pack('<I', PACKAGE_BASE + slot))
        quest += sub('VTCK', bytes(4)) + sub('ALED')
        packages += build_package(PACKAGE_BASE + slot, MARKER_FIRST + slot)
        data = sub('EDID', f'SYNTH_WaitAnchor{slot}\0'.encode())
        data += sub('NAME', struct.pack('<I', MARKER_BASE)) + sub('DATA', bytes(24))
        markers += record('REFR', MARKER_FIRST + slot, data, 0x400)  # Persistent, owned invisible marker.
    # Preserve the retired restraint receipts for load migration only. New Wait
    # assignments never fill these aliases or change generic restraint.
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
        group(b'QUST', 0, record('QUST', WAIT_QUEST, quest) + build_approach_quest() + build_approach_quest(visit=True)) + cells


def verify_plugin(data):
    """Byte-exact allowlist includes signed Wait priority100, inputs and record ownership."""
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
