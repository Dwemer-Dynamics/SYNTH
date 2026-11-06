"""Original approach records and saved wait separation; no game installation access."""
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import build_gameplay as gameplay


def records(data, start=0, end=None):
    end = len(data) if end is None else end
    offset = start
    while offset < end:
        kind, size = struct.unpack_from('<4sI', data, offset)
        if kind == b'GRUP':
            yield from records(data, offset + 24, offset + size)
            offset += size
        else:
            form = struct.unpack_from('<I', data, offset + 12)[0]
            yield kind, form, data[offset + 24:offset + 24 + size]
            offset += 24 + size
    if offset != end:
        raise AssertionError('Record boundary differs')


def fields(data):
    offset = 0
    result = []
    while offset < len(data):
        tag, length = struct.unpack_from('<4sH', data, offset)
        offset += 6
        result.append((tag, data[offset:offset + length]))
        offset += length
    if offset != len(data):
        raise AssertionError('Subrecord boundary differs')
    return result


class ApproachGameplayTests(unittest.TestCase):
    def test_modern_package_schedules_are_unrestricted_in_every_month(self):
        # Fallout selects an ordinary alias package only if its schedule month
        # is zero or matches the current month. FF is the pre-version-122 encoding.
        self.assertGreaterEqual(struct.unpack_from('<H', gameplay.record('PACK', 0, b''), 20)[0], 122)
        packages = [(form, dict(fields(body))[b'PSDT']) for kind, form, body in records(gameplay.build_plugin())
                    if kind == b'PACK']
        self.assertEqual(len(packages), 274)
        for form, schedule in packages:
            month = struct.unpack_from('<b', schedule)[0]
            for current_month in range(12):
                with self.subTest(package=hex(form), current_month=current_month):
                    self.assertTrue(month == 0 or month == current_month,
                                    'Modern package schedule must use month 0 for Any')

    def test_live_wait_uses_ordinary_wait_public_defaults_and_rejects_combat_template(self):
        for slot in range(gameplay.WAIT_SLOTS):
            kind, form, body = next(records(gameplay.build_package(gameplay.PACKAGE_BASE + slot, gameplay.MARKER_FIRST + slot)))
            self.assertEqual((kind, form), (b'PACK', gameplay.PACKAGE_BASE + slot))
            ff = fields(body)
            by = dict(ff)
            self.assertEqual(by[b'PKCU'], struct.pack('<III', 2, 0x001A32D6, 2))
            self.assertEqual([value for tag, value in ff if tag == b'ANAM'], [b'Bool\0', b'Float\0'])
            self.assertEqual([value for tag, value in ff if tag == b'CNAM'], [b'\0', struct.pack('<f', 0.0)])
            self.assertEqual([value for tag, value in ff if tag == b'UNAM'], [b'\x0a', b'\x0b'])
            self.assertEqual(by[b'XNAM'], b'\x0c')
            self.assertFalse(any(tag in (b'PLDT', b'PTDA', b'CTDA') for tag, value in ff))
            self.assertEqual(by[b'PKDT'], struct.pack('<IBBBBHH', 0x100004, 18, 0, 0, 0, 0, 0))
        data = gameplay.build_plugin()
        old_binding = struct.pack('<III', 1, 0x0001D415, 1)
        damaged = data.replace(struct.pack('<III', 2, 0x001A32D6, 2), old_binding, 1)
        with self.assertRaisesRegex(ValueError, 'deterministic wait package'):
            gameplay.verify_plugin(damaged)

    def test_wait_priority_is_positive_signed_100_and_negative_ff_is_rejected(self):
        data = gameplay.build_plugin()
        wait = next(body for kind, form, body in records(data) if kind == b'QUST' and form == gameplay.WAIT_QUEST)
        priority = dict(fields(wait))[b'DNAM'][2:3]
        self.assertEqual(struct.unpack('<b', priority)[0], 100)
        prefix = gameplay.sub('EDID', b'SYNTH_WaitOwnership\0')
        offset = data.index(prefix) + len(prefix)
        self.assertEqual(data[offset:offset + 6], b'DNAM' + struct.pack('<H', 12))
        offset += 8  # DNAM subrecord header plus its uint16 flags.
        self.assertEqual(data[offset:offset + 1], priority)
        negative = data[:offset] + b'\xff' + data[offset + 1:]
        self.assertEqual(struct.unpack('<b', negative[offset:offset + 1])[0], -1)
        with self.assertRaisesRegex(ValueError, 'deterministic wait package'):
            gameplay.verify_plugin(negative)

    def test_separate_original_quest_has_exact_optional_ownership_slots(self):
        rr = list(records(gameplay.build_plugin()))
        self.assertTrue(all(kind in (b'TES4', b'STAT', b'PACK', b'QUST', b'CELL', b'REFR') for kind, _, _ in rr))
        self.assertEqual(len({form for _, form, _ in rr}), len(rr))
        quests = {form: data for kind, form, data in rr if kind == b'QUST'}
        self.assertEqual(set(quests), {gameplay.WAIT_QUEST, gameplay.APPROACH_QUEST, gameplay.VISIT_QUEST})
        wait = fields(quests[gameplay.WAIT_QUEST])
        approach = fields(quests[gameplay.APPROACH_QUEST])
        self.assertEqual(sum(tag == b'ALST' for tag, _ in wait), 512)
        self.assertFalse(any(tag == b'ALSP' for tag, _ in wait))
        self.assertEqual([struct.unpack('<I', value)[0] for tag, value in wait if tag == b'ALPC'],
                         list(range(gameplay.PACKAGE_BASE, gameplay.PACKAGE_BASE + 256)))
        self.assertEqual([struct.unpack('<I', value)[0] for tag, value in approach if tag == b'ALST'], list(range(16)))
        self.assertEqual([struct.unpack('<I', value)[0] for tag, value in approach if tag == b'ALPC'],
                         list(range(gameplay.APPROACH_PACKAGE_FIRST, gameplay.APPROACH_PACKAGE_FIRST + 8)))
        self.assertEqual([value for tag, value in approach if tag == b'FNAM'], [struct.pack('<I', 0x4020A)] * 16)
        self.assertNotIn(b'VMAD', dict(approach))
        header = dict(fields(rr[0][2]))
        _, count, next_id = struct.unpack('<fII', header[b'HEDR'])
        self.assertEqual(count, len(rr) - 1)
        self.assertEqual(next_id, 0xB48)
        self.assertEqual([value for tag, value in fields(rr[0][2]) if tag == b'MAST'], [b'Fallout4.esm\0'])

    def test_packages_navigate_to_player_without_combat_or_completion_override(self):
        for slot in range(8):
            kind, form, data = next(records(gameplay.build_approach_package(slot)))
            self.assertEqual((kind, form), (b'PACK', gameplay.APPROACH_PACKAGE_FIRST + slot))
            ff = fields(data)
            by = dict(ff)
            self.assertEqual(by[b'QNAM'], struct.pack('<I', gameplay.APPROACH_QUEST))
            self.assertEqual(by[b'PKCU'], struct.pack('<III', 5, gameplay.FOLLOW_TEMPLATE, 1))
            self.assertEqual(by[b'PKDT'], struct.pack('<IBBBBHH', 0, 18, 0, 0, 0, 0, 0))
            self.assertEqual(by[b'PTDA'], struct.pack('<iii', 4, 8 + slot, 0))
            self.assertEqual([v for k, v in ff if k == b'ANAM'], [b'SingleRef\0', b'Float\0', b'Float\0', b'Bool\0', b'Bool\0'])
            self.assertEqual([v for k, v in ff if k == b'CNAM'], [struct.pack('<f', 128), struct.pack('<f', 192), b'\x01', b'\0'])
            self.assertEqual([v for k, v in ff if k == b'UNAM'], [bytes([x]) for x in (0, 1, 2, 4, 6)])
            self.assertEqual(by[b'XNAM'], b'\x07')
            self.assertFalse(any(k in (b'CTDA', b'VMAD') for k, _ in ff))
        for slot in (-1, 8):
            with self.assertRaises(ValueError):
                gameplay.build_approach_package(slot)

    def test_cleanup_is_exact_and_navigation_does_not_recruit_or_teleport(self):
        source = (ROOT / 'scripts/papyrus/SYNTHWait.psc').read_text().split(
            '; Conversation navigation has separate original quest/alias ownership', 1)[1]
        for banned in ('MoveTo(', 'SetPlayerTeammate(', 'SetRestrained(', 'StopCombat(', 'AddToFaction(', 'SetFactionRank('):
            self.assertNotIn(banned, source)
        for guard in ('CurrentApproach(ticket)', 'previous.GetFormID() == subjectId', 'listener == Game.GetPlayer()',
                      '!subject.IsInCombat()', '!subject.IsPlayerTeammate()',
                      'ApproachSameArea(subject, listener)', '!source.IsInterior() && !target.IsInterior()', 'subject.GetWorldSpace() == listener.GetWorldSpace()', 'subject.GetDistance(listener) <= 2000.0',
                      'ObservedApproach(ticket, activeId, aliasOwned', '(subject || !acquire)',
                      'performerAlias.Clear()', 'listenerAlias.Clear()', 'ResetApproachByFormId'):
            self.assertIn(guard, source)
        self.assertLessEqual(len((ROOT / 'scripts/papyrus/SYNTHWait.psc').read_bytes()), 32768)

    def test_follow_is_unconditional_repeating_and_never_ends_on_arrival(self):
        kind, form, data = next(records(gameplay.build_follow_template()))
        self.assertEqual((kind, form), (b'PACK', gameplay.FOLLOW_TEMPLATE))
        ff = fields(data)
        by = dict(ff)
        self.assertEqual(by[b'PKDT'], struct.pack('<IBBBBHH', 0, 19, 0, 0, 0, 0, 0))
        self.assertEqual(by[b'PRCB'], struct.pack('<II', 1, 1))
        self.assertEqual(by[b'FNAM'], bytes(4))
        self.assertEqual([value for tag, value in ff if tag == b'PKC2'],
                         [bytes([index]) for index in (0, 1, 2, 6, 6, 6)])
        self.assertFalse(any(tag == b'CTDA' for tag, _ in ff))
        self.assertEqual([value for tag, value in ff if tag == b'PNAM' and len(value) != 4], [b'Follow\0'])

    def test_visit_aliases_cannot_point_into_player_approach_or_wait_quest(self):
        _, form, data = next(records(gameplay.build_approach_quest(visit=True)))
        self.assertEqual(form, gameplay.VISIT_QUEST)
        ff = fields(data)
        self.assertEqual([struct.unpack('<I', value)[0] for tag, value in ff if tag == b'ALPC'],
                         list(range(gameplay.VISIT_PACKAGE_FIRST, gameplay.VISIT_PACKAGE_FIRST + 8)))
        for slot in range(8):
            _, form, data = next(records(gameplay.build_follow_package(slot, visit=True)))
            self.assertEqual(form, gameplay.VISIT_PACKAGE_FIRST + slot)
            by = dict(fields(data))
            self.assertEqual(by[b'QNAM'], struct.pack('<I', gameplay.VISIT_QUEST))
            self.assertEqual(by[b'PTDA'], struct.pack('<iii', 4, 8 + slot, 0))
        source = (ROOT / 'scripts/papyrus/SYNTHWait.psc').read_text().split('Function VisitByFormIds(', 1)[1]
        self.assertIn('subject.IsPlayerTeammate()', source)
        self.assertIn('listener != Game.GetPlayer()', source)
        self.assertIn('previousListener.GetFormID() == listenerId', source)
        self.assertIn('If newOwner', source)
        for banned in ('SetPlayerTeammate(', 'SetRestrained(', 'StopCombat(', 'MoveTo('):
            self.assertNotIn(banned, source)

    def test_plugin_rejects_unreviewed_template_mutation(self):
        data = gameplay.build_plugin()
        gameplay.verify_plugin(data)
        damaged = data.replace(struct.pack('<III', 5, gameplay.FOLLOW_TEMPLATE, 1), struct.pack('<III', 5, 0x2CB1, 1), 1)
        with self.assertRaises(ValueError):
            gameplay.verify_plugin(damaged)


if __name__ == '__main__':
    unittest.main()
