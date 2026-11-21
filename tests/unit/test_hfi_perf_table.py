"""Check Gen8 performance messages using Piano's packaged firmware and factory OPP lists."""
import ctypes as ct
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'windows/drivers/adreno'
U16, U32, SIZE = ct.c_uint16, ct.c_uint32, ct.c_size_t


class Opp(ct.Structure):
    _fields_ = [('khz', U32), ('corner', U16)]


class Rail(ct.Structure):
    _fields_ = [('levels', ct.POINTER(U16)), ('count', SIZE)]


class Config(ct.Structure):
    _fields_ = [('gpu', ct.POINTER(Opp)), ('gmu', ct.POINTER(Opp)), ('gpu_count', SIZE), ('gmu_count', SIZE),
                *[(name, Rail) for name in ('gfx', 'gmxc', 'cx', 'mx', 'gbx')]]


class Table(ct.Structure):
    _fields_ = [('count', SIZE), ('words', U32 * 110)]


class Hfi(ct.Structure):
    _fields_ = [('memory', ct.POINTER(U32)), ('sequence', U32)]


def rail(values):
    return Rail((U16 * len(values))(*values), len(values)) if values else Rail()


def config(gpu, gmu, levels):
    return Config((Opp * len(gpu))(*(Opp(*entry) for entry in gpu)),
                  (Opp * len(gmu))(*(Opp(*entry) for entry in gmu)), len(gpu), len(gmu), *map(rail, levels))


def firmware_hfi_version():
    # KGSL queries DTCM word 0xffc. Read that same word from the packaged GMU
    # scatter image; it isn't the first file header or an arbitrary matching word.
    dtcm_word = 0x10004000 + 0xffc * 4
    data = (ROOT / 'vendor/firmware/piano-qcom/gen80000_gmu.bin').read_bytes()
    pos = 0
    while pos < len(data):
        start, size, _, _ = struct.unpack_from('<4I', data, pos)
        pos += 16
        if size > len(data) - pos:
            raise ValueError('truncated GMU segment')
        if start <= dtcm_word and dtcm_word + 4 <= start + size:
            return struct.unpack_from('<I', data, pos + dtcm_word - start)[0]
        pos += size
    raise ValueError('missing HFI version in packaged GMU DTCM')


class HfiPerfTableTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(ROOT / 'tools'))
        try:
            from compose_piano_dtb import read_fdt
        finally:
            sys.path.pop(0)
        cls.tree = read_fdt((ROOT / 'vendor/piano/dtb/piano.dtb').read_bytes())['tree']
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        library = Path(tmp.name) / 'perf.so'
        subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        SRC / 'hfi_perf_table.c', ROOT / 'windows/drivers/adreno/hfi_queue.c', '-o', library],
                       check=True)
        cls.lib = ct.CDLL(str(library))
        cls.lib.adreno_hfi_perf_table.argtypes = [U32, ct.POINTER(Config), ct.POINTER(Table)]
        cls.lib.adreno_hfi_init.argtypes = [ct.POINTER(Hfi), ct.c_void_p, SIZE, ct.c_uint64]
        cls.lib.adreno_hfi_send.argtypes = [ct.POINTER(Hfi), ct.c_uint8, ct.POINTER(U32), SIZE, ct.POINTER(U32)]

    def test_factory_gpu_bins_and_both_gmu_rates_reach_command_ring(self):
        version = firmware_hfi_version()
        self.assertGreaterEqual(version, 0x20060001)
        gmu = self.tree['/soc/qcom,gmu@3d37000']['qcom,gmu-freq-table']
        gmu = [(hz // 1000, corner) for hz, corner in struct.iter_unpack('>2I', gmu)]
        self.assertEqual([khz for khz, _ in gmu], [500000, 650000])
        bins = '/soc/qcom,kgsl-3d0@3d00000/qcom,gpu-pwrlevel-bins'
        found = set()
        for group in (p for p in self.tree if p.rpartition('/')[0] == bins):
            with self.subTest(group=group):
                gpu = sorted((int.from_bytes(v['qcom,gpu-freq'], 'big') // 1000,
                              int.from_bytes(v['qcom,level'], 'big'))
                             for p, v in self.tree.items() if p.rpartition('/')[0] == group)
                found.add(gpu[-1][0])
                # These are test rail tables, not a claim about live RPMh votes.
                corners = sorted({0, *(c for _, c in gpu), *(c for _, c in gmu)})
                cfg = config(gpu, gmu, [corners] * 4 + [[]])
                table, hfi, token = Table(), Hfi(), U32()
                self.assertEqual(self.lib.adreno_hfi_perf_table(version, ct.byref(cfg), ct.byref(table)), 0)
                shared = (U32 * 3284)()
                self.assertEqual(self.lib.adreno_hfi_init(ct.byref(hfi), shared, ct.sizeof(shared), 0x60000000), 0)
                self.assertEqual(self.lib.adreno_hfi_send(ct.byref(hfi), 15, table.words, table.count,
                                                          ct.byref(token)), 0)
                ring = shared[0x350 // 4:0x350 // 4 + table.count + 1]
                self.assertEqual(ring[0], (1 << 20) | (len(ring) << 8) | 15)
                self.assertEqual(ring[1:5], [0, 1, len(gpu) + 1, 3])
                self.assertEqual(ring[5:8], [0, 0xffffffff, 0])
                cx_at = 5 + 3 * (len(gpu) + 1)
                self.assertEqual(ring[10:cx_at:3], [f for f, _ in gpu])
                self.assertEqual(ring[cx_at:cx_at+4], [3, 2, 0, 0])
                self.assertEqual(ring[cx_at+5::2], [f for f, _ in gmu])
        self.assertEqual(found, {900000, 1050000, 1100000, 1150000, 1200000})

    def test_round_to_resident_corner_and_encode_dependent_rails(self):
        cfg = config([(900000, 60)], [(500000, 64)], [[0, 64, 0, 0], [0, 60, 64, 0], [0, 64],
                                                       [0, 32], [0, 32, 64]])
        table = Table()
        self.assertEqual(self.lib.adreno_hfi_perf_table(0x20070001, ct.byref(cfg), ct.byref(table)), 0)
        self.assertEqual(table.words[7], (64 << 16) | (2 << 8) | 1)
        self.assertEqual(table.words[8], ((0x0fffc0ff & ~0x03f00000) | (2 << 20) | (1 << 8)))
        self.assertEqual(table.words[14], (64 << 16) | (1 << 8) | 1)
        before = bytes(table)
        cfg.gpu[0].corner = 65
        self.assertEqual(self.lib.adreno_hfi_perf_table(0x20070001, ct.byref(cfg), ct.byref(table)), 3)
        self.assertEqual(bytes(table), before)
        self.assertEqual(self.lib.adreno_hfi_perf_table(0x20060000, ct.byref(cfg), ct.byref(table)), 1)
        cfg.gpu_count = 32
        self.assertEqual(self.lib.adreno_hfi_perf_table(0x20070001, ct.byref(cfg), ct.byref(table)), 2)
        self.assertEqual(bytes(table), before)


if __name__ == '__main__':
    unittest.main()
