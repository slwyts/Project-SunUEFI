"""Exact captured-PE derivation; no firmware build, execution or device access."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('inherited_clock', ROOT / 'tools/piano_inherited_clock.py')
CLOCK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CLOCK)


class InheritedClockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        catalog = json.loads((ROOT / 'private/analysis/native-driver-inventory.json').read_text())
        cls.source = Path(catalog['drivers']['ClockDxe']['pe_path'])
        cls.original = cls.source.read_bytes()

    def test_exact_six_bytes_return_zero_and_unchanged_abi(self):
        derived, record = CLOCK.derive(self.original)
        self.assertEqual(len(derived), len(self.original))
        differences = [i for i, (old, new) in enumerate(zip(self.original, derived)) if old != new]
        self.assertEqual(differences, list(range(0xc6d0, 0xc6d4)) + [0x31593, 0x3e588])
        self.assertEqual(struct.unpack_from('<I', derived, 0xc6d0)[0], 0x52800000)
        # MOVZ W0, imm16=0, shift=0: zero-extends x0 and makes following CBNZ fall through.
        word = struct.unpack_from('<I', derived, 0xc6d0)[0]
        self.assertEqual(word & 31, 0)
        self.assertEqual((word >> 5) & 0xffff, 0)
        self.assertEqual((word >> 21) & 3, 0)
        self.assertEqual(derived[0xc6d4:0xc6d8], bytes.fromhex('e0000035'))
        self.assertEqual(derived[0x3e588:0x3e58c], b'\0' * 4)
        self.assertEqual(struct.unpack_from('<I', derived, 0x31590)[0], 0x00004000)
        start = CLOCK.PROTOCOL_RVA
        self.assertEqual(derived[start:start + CLOCK.PROTOCOL_BYTES], self.original[start:start + CLOCK.PROTOCOL_BYTES])
        self.assertEqual(record['derived_sha256'], hashlib.sha256(derived).hexdigest())
        self.assertEqual(record['retained_clocks'], list(CLOCK.RETAINED_CLOCKS))
        self.assertEqual(record['retained_resource_count'], 7)
        self.assertFalse(record['display_only'])
        self.assertFalse(record['hardware_verified'])
        self.assertFalse(record['owned_retirement_claimed'])
        self.assertTrue(record['display_cesta_auto_init_deferred'])
        self.assertTrue(record['display_pll_auto_init_deferred'])
        self.assertFalse(record['full_display_preservation_claimed'])
        self.assertEqual(record['changed_byte_count'], 6)
        self.assertEqual(record['derived_sha256'], CLOCK.DERIVED_SHA256)
        self.assertEqual(self.source.read_bytes(), self.original)
        self.assertEqual(CLOCK.derive(self.original), (derived, record))

    def test_wrong_source_pin_and_repeat_derivation_rejected(self):
        altered = bytearray(self.original)
        altered[0x1000] ^= 1
        with self.assertRaisesRegex(ValueError, 'SHA256 pin'):
            CLOCK.derive(bytes(altered))
        derived, _ = CLOCK.derive(self.original)
        with self.assertRaisesRegex(ValueError, 'SHA256 pin'):
            CLOCK.derive(derived)

    def test_independent_neighbor_and_call_guards(self):
        for offset in (0xc6cc, 0xc6d0, 0xc6d4):
            altered = bytearray(self.original)
            altered[offset] ^= 1
            altered = bytes(altered)
            # Isolate the context check independently of the stronger full source pin.
            with patch.object(CLOCK, 'ORIGINAL_SHA256', hashlib.sha256(altered).hexdigest()):
                with self.assertRaisesRegex(ValueError, 'cleanup context'):
                    CLOCK.derive(altered)

    def test_independent_names_flags_bsp_and_opcode_guards(self):
        for offset, message in (
                (0x28388, 'CESTA BSP'), (0x1657a, 'CESTA descriptor names'),
                (0x3e588, 'CESTA automatic'), (0x2fe28, 'PLL BSP node/name'),
                (0x14159, 'PLL BSP node/name'), (0x31593, 'PLL automatic'),
                (0xc5b8, 'opcode guard'), (0xc608, 'opcode guard')):
            altered = bytearray(self.original)
            altered[offset] ^= 1
            altered = bytes(altered)
            with patch.object(CLOCK, 'ORIGINAL_SHA256', hashlib.sha256(altered).hexdigest()):
                with self.assertRaisesRegex(ValueError, message):
                    CLOCK.derive(altered)


if __name__ == '__main__':
    unittest.main()
