import importlib.util
from pathlib import Path
import struct
import unittest


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('bootfail', ROOT / 'tools/collect_piano_bootfail.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def make_header(magic):
    data = bytearray(4096)
    struct.pack_into('<QII', data, 0, magic, 0x1000, 4096)
    return data


class BootfailTest(unittest.TestCase):
    def test_observed_geometry(self):
        data = make_header(0x56775AF41BCDE0F0)
        capacity = 171966464
        base = module.bootfail_location(data, capacity, 0x9a00000)
        data = make_header(0x627759541BCDE0F0)
        struct.pack_into('<I', data, 20, 0x100000)
        ring = module.log_location(data, base, capacity)
        data = make_header(0x94682550DD25E1F0)
        struct.pack_into('<II', data, 20, 15, 10)
        self.assertEqual(module.ring_geometry(data, ring, capacity), (15, 10))

    def test_bad_magic_and_offset(self):
        with self.assertRaises(ValueError):
            module.bootfail_location(bytes(4096), 171966464, 0x9a00000)
        data = make_header(0x56775AF41BCDE0F0)
        with self.assertRaises(ValueError):
            module.bootfail_location(data, 171966464, 171966464)

    def test_ring_rejects_bad_count_current_and_extent(self):
        for count, current, capacity in [(33, 1, 171966464), (15, 15, 171966464), (15, 10, 0x9b01000)]:
            data = make_header(0x94682550DD25E1F0)
            struct.pack_into('<II', data, 20, count, current)
            with self.assertRaises(ValueError):
                module.ring_geometry(data, 0x9b00000, capacity)


if __name__ == '__main__':
    unittest.main()
