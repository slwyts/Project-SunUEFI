"""Gen8 HFI bandwidth messages use the board's DDR table and AOP BCM units."""
import ctypes as ct
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'windows/drivers/adreno'
U32 = ct.c_uint32


class Bcm(ct.Structure):
    _fields_ = [('address', U32), ('unit', U32), ('width', ct.c_uint16), ('vcd', ct.c_uint8)]


class Fabric(ct.Structure):
    _fields_ = [('bcm', Bcm * 9), ('paths', U32)]


class Config(ct.Structure):
    _fields_ = [('fabric', ct.POINTER(Fabric)), ('kbps', ct.POINTER(U32)), ('count', ct.c_size_t), ('threshold', U32)]


class HfiBandwidth(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(ROOT / 'tools'))
        try:
            from compose_piano_dtb import read_fdt
        finally:
            sys.path.pop(0)
        gpu = read_fdt((ROOT / 'vendor/piano/dtb/piano.dtb').read_bytes())['tree']['/soc/qcom,kgsl-3d0@3d00000']
        cls.levels = [kbps for kbps, in struct.iter_unpack('>I', gpu['qcom,bus-table-ddr'])]
        work = tempfile.TemporaryDirectory(prefix='gmu-bw-')
        cls.addClassCleanup(work.cleanup)
        library = Path(work.name) / 'bandwidth.so'
        subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
                        SRC / 'hfi_bandwidth.c', '-o', library], check=True)
        cls.call = ct.CDLL(str(library)).adreno_hfi_bandwidth_table
        cls.call.argtypes = [ct.POINTER(Config), ct.POINTER(U32)]

    def setUp(self):
        # Resource metadata from the factory AOP command database; these
        # addresses describe its named requests, not initial AP power votes.
        self.fabric = Fabric()
        for slot, fields in (
            (0, (0x50068, 1, 0, 13)), (1, (0x50030, 140000, 4, 5)),
            (3, (0x50000, 4800000, 16, 0)), (5, (0x50004, 4800000, 64, 1)),
        ):
            self.fabric.bcm[slot] = Bcm(*fields)
        self.kbps = (U32 * len(self.levels))(*self.levels)
        self.cfg = Config(ct.pointer(self.fabric), self.kbps, len(self.kbps), 10687500)
        self.out = (U32 * 159)()

    def test_factory_bus_table_and_performance_threshold(self):
        self.assertEqual(self.call(ct.byref(self.cfg), self.out), 1)
        self.assertEqual(self.out[:6], [12, 1, 3, 1, 7, 0x50030])
        self.assertEqual(self.out[11], 0x40000000)
        self.assertEqual(self.out[17], 0x60000001)
        self.assertEqual(self.out[23:26], [0x50004, 0x50000, 0x50068])
        for i, bw in enumerate(self.kbps):
            with self.subTest(kbps=bw):
                offset = 31 + 8 * i
                count = min(bw * 4 * 1000 // 4800000, 0x3fff)
                vote = 0x60000000 | (count << 14) | count if bw else 0x40000000
                mode = 0x60000000 | (4 if bw >= 10687500 else 0) if bw else 0x40000000
                self.assertEqual(self.out[offset:offset+8], [vote, vote, mode, 0, 0, 0, 0, 0])
        self.assertFalse(any(self.out[31 + 8 * len(self.kbps):]))

    def test_shared_clock_domain_commits_only_its_last_request(self):
        self.fabric.bcm[5].vcd = self.fabric.bcm[3].vcd
        self.cfg.threshold = 0
        self.assertEqual(self.call(ct.byref(self.cfg), self.out), 1)
        self.assertEqual(self.out[4], 6)
        self.assertEqual(self.out[31:34], [0, 0x40000000, 0x40000000])
        self.assertEqual(self.out[31+8*11+2], 0x60000000)

    def test_invalid_levels_or_resources_do_not_publish_a_partial_table(self):
        for target, field, bad in ((self.cfg, 'count', 17), (self.kbps, 1, 0),
                                   (self.fabric.bcm[5], 'unit', 0), (self.fabric.bcm[3], 'width', 0),
                                   (self.fabric.bcm[1], 'address', 0x41e00),
                                   (self.fabric.bcm[0], 'address', self.fabric.bcm[3].address)):
            with self.subTest(field=field, bad=bad):
                if isinstance(field, int):
                    old, target[field] = target[field], bad
                else:
                    old = getattr(target, field)
                    setattr(target, field, bad)
                self.out[0] = 0xabba
                self.assertEqual(self.call(ct.byref(self.cfg), self.out), 0)
                self.assertEqual(self.out[0], 0xabba)
                if isinstance(field, int):
                    target[field] = old
                else:
                    setattr(target, field, old)


if __name__ == '__main__':
    unittest.main()
