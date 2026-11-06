"""Independent structural checks of original radio records, not in-game proof."""
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import build_radio_gameplay as radio
import build_gameplay as wait


def records(data, include_groups=False):
    def read(begin, end, groups=()):
        result = []
        while begin < end:
            tag, size, flags, form = struct.unpack_from('<4sIII', data, begin)
            if tag == b'GRUP':
                label = data[begin + 8:begin + 12]
                kind = struct.unpack_from('<I', data, begin + 12)[0]
                result += read(begin + 24, begin + size, groups + ((label, kind),))
                begin += size
            else:
                row = (tag, flags, form, data[begin + 24:begin + 24 + size])
                result.append(row + (groups,) if include_groups else row)
                begin += 24 + size
        if begin != end:
            raise ValueError('truncated record group')
        return result
    return read(0, len(data))


def subs(data):
    result = []
    offset = 0
    while offset < len(data):
        tag, size = struct.unpack_from('<4sH', data, offset)
        result.append((tag, data[offset + 6:offset + 6 + size]))
        offset += 6 + size
    if offset != len(data):
        raise ValueError('truncated subrecord')
    return result


class RadioRecordTests(unittest.TestCase):
    def setUp(self):
        self.data = radio.build_plugin()
        self.rows = records(self.data)
        self.forms = {form: (tag, flags, subs(body)) for tag, flags, form, body in self.rows}

    def test_closed_original_form_ownership_and_master(self):
        self.assertEqual(len(self.rows), 13)
        self.assertEqual(set(self.forms) - {0}, set(radio.FORM.values()))
        self.assertTrue(all(form >> 24 == 1 for form in self.forms if form))
        header = self.forms[0][2]
        self.assertEqual([value for tag, value in header if tag == b'MAST'], [b'Fallout4.esm\0'])
        self.assertEqual(struct.unpack('<fII', dict(header)[b'HEDR'])[1:], (12, 0x80C))

    def test_radio_actions_are_native_nonlooping_and_owned(self):
        for key in ('a', 'b'):
            tag, flags, scene = self.forms[radio.FORM[f'slot_{key}_scene']]
            self.assertEqual(tag, b'SCEN')
            self.assertEqual(flags, 0)
            self.assertEqual([value for tag, value in scene if tag == b'ANAM'], [b'\x06\0', b''])
            fields = dict(scene)
            self.assertEqual(struct.unpack('<I', fields[b'FNAM'])[0], 0x100)
            self.assertEqual(struct.unpack('<I', fields[b'HTID'])[0], radio.FORM[f'slot_{key}_sound'])
            self.assertEqual(struct.unpack('<I', fields[b'PNAM'])[0], radio.FORM['quest'])
            self.assertEqual(fields[b'DMAX'], bytes(4))
            self.assertEqual(fields[b'SNAM'], bytes(4))
            self.assertEqual(fields[b'ENAM'], bytes(4))

    def test_scenes_are_children_of_the_owning_quest(self):
        # FO4 loads scenes through a type-10 quest child group, not a top SCEN group.
        paths = {form: groups for _, _, form, _, groups in records(self.data, include_groups=True)}
        owner = struct.pack('<I', radio.FORM['quest'])
        self.assertEqual(paths[radio.FORM['quest']], ((b'QUST', 0),))
        for key in ('a', 'b'):
            form = radio.FORM[f'slot_{key}_scene']
            self.assertEqual(paths[form], ((b'QUST', 0), (owner, 10)))
            fields = dict(self.forms[form][2])
            self.assertEqual(fields[b'PNAM'], owner)

    def test_references_are_in_the_cells_persistent_child_group(self):
        paths = {form: groups for _, _, form, _, groups in records(self.data, include_groups=True)}
        local_cell = radio.FORM['cell'] & 0xFFFFFF
        cell_path = ((b'CELL', 0), (struct.pack('<I', local_cell % 10), 2),
                     (struct.pack('<I', local_cell // 10 % 10), 3))
        self.assertEqual(paths[radio.FORM['cell']], cell_path)
        owner = struct.pack('<I', radio.FORM['cell'])
        for key in ('transmitter', 'receiver'):
            self.assertEqual(paths[radio.FORM[key]], cell_path + ((owner, 6), (owner, 8)))
            self.assertNotEqual(self.forms[radio.FORM[key]][1] & 0x400, 0)

    def test_standard_sound_paths_use_anam_not_auto_weapon_fnam(self):
        for key in ('a', 'b'):
            fields = dict(self.forms[radio.FORM[f'slot_{key}_sound']][2])
            self.assertNotIn(b'FNAM', fields)
            self.assertEqual(fields[b'ANAM'], f'Data\\Sound\\FX\\SYNTH\\RadioDiagnostics\\slot_{key}.wav\0'.encode())
            self.assertEqual(struct.unpack('<I', fields[b'CNAM'])[0], 0x1EEF540A)
            self.assertEqual(fields[b'LNAM'], bytes(4))

    def test_no_automatic_quest_or_enabled_transmitter(self):
        fields = dict(self.forms[radio.FORM['quest']][2])
        self.assertEqual(struct.unpack_from('<H', fields[b'DNAM'])[0] & 1, 0)
        self.assertEqual(self.forms[radio.FORM['transmitter']][1], 0xC00)
        self.assertEqual(self.forms[radio.FORM['receiver']][1], 0xC00)
        self.assertEqual(self.forms[radio.FORM['transmitter_base']][0], b'ACTI')
        vmad = fields[b'VMAD']
        self.assertEqual(struct.unpack_from('<HHH', vmad), (6, 2, 1))
        length = struct.unpack_from('<H', vmad, 6)[0]
        self.assertEqual(vmad[8:8 + length], b'SYNTHRadioProbe')
        self.assertEqual(vmad[8 + length:], b'\0\0\0')

    def test_transmitter_alias_is_optional_and_script_filled(self):
        quest = self.forms[radio.FORM['quest']][2]
        self.assertEqual([value for tag, value in quest if tag == b'ALST'], [struct.pack('<I', 0)])
        fields = dict(quest)
        self.assertEqual(fields[b'FNAM'], struct.pack('<I', 0x82))
        self.assertNotIn(b'ALFR', fields)
        self.assertNotIn(b'CTDA', fields)

    def test_same_frequency_and_receiver_output(self):
        tx = dict(self.forms[radio.FORM['transmitter']][2])[b'XRDO']
        receiver = dict(self.forms[radio.FORM['receiver_base']][2])[b'RADR']
        tx_fields = struct.unpack('<fffI', tx)
        receiver_fields = struct.unpack('<IffBB', receiver)
        self.assertAlmostEqual(tx_fields[0], 106.9, delta=1e-4)
        self.assertEqual(tx_fields[0], receiver_fields[1])
        self.assertEqual(tx_fields[1:], (0, 0, 1))
        self.assertEqual(receiver_fields[0], radio.FORM['output'])
        self.assertEqual(receiver_fields[2:], (1, 1, 0))

    def test_mutation_rejected_and_wait_plugin_stays_distinct(self):
        radio.verify_plugin(self.data)
        with self.assertRaises(ValueError):
            radio.verify_plugin(self.data[:-1] + bytes([self.data[-1] ^ 1]))
        with self.assertRaises(ValueError):
            wait.verify_plugin(self.data)
        wait.verify_plugin(wait.build_plugin())


if __name__ == '__main__':
    unittest.main()
