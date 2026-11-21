"""GMU system-memory layout, including the shipped firmware's preallocations."""
import ctypes as ct
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'windows/drivers/adreno'
FIRMWARE = ROOT / 'vendor/firmware/piano-qcom/gen80000_gmu.bin'


class Region(ct.Structure):
    _fields_ = [('address', ct.c_uint32), ('bytes', ct.c_uint32)]


class Layout(ct.Structure):
    _fields_ = [('cache_bytes', ct.c_uint32), ('hfi', Region), ('log', Region), ('count', ct.c_size_t)]


def request(address, size, kind=1):
    return struct.pack('<4I', address, 0, kind, size)


def payload(address=0):
    return struct.pack('<4I', address, 4, 0, 0) + b'ABCD'


class GmuMemoryLayout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        work = tempfile.TemporaryDirectory(prefix='adreno-memory-')
        cls.addClassCleanup(work.cleanup)
        so = Path(work.name) / 'layout.so'
        subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
                        DRIVER / 'gmu_firmware.c', DRIVER / 'gmu_memory_layout.c', '-o', so],
                       check=True)
        cls.lib = ct.CDLL(str(so))
        cls.call = cls.lib.adreno_gmu_memory_layout
        cls.call.argtypes = [ct.c_void_p, ct.c_size_t, ct.POINTER(Region), ct.c_size_t,
                             ct.POINTER(Layout)]

    def layout(self, image):
        out = Layout()
        status = self.call(image, len(image), None, 0, ct.byref(out))
        if status == 2:
            return None
        if status == 1:
            ranges = (Region * out.count)()
            status = self.call(image, len(image), ranges, len(ranges), ct.byref(out))
            if status == 2:
                return None
            self.assertEqual(status, 0)
            fixed = [(r.address, r.bytes) for r in ranges[:out.count]]
        else:
            self.assertEqual(status, 0)
            fixed = []
        return out.cache_bytes, fixed, (out.hfi.address, out.hfi.bytes), (out.log.address, out.log.bytes)

    def test_real_firmware_scatter_destinations_and_persistent_blocks(self):
        image = FIRMWARE.read_bytes()
        size, fixed, queue, log = self.layout(image)
        cache_end = 0x4000 + size
        offset = 0
        while offset < len(image):
            base, length, kind, value = struct.unpack_from('<4I', image, offset)
            if (not length and kind in (1, 7) and 0x4000 <= base < 0x1000000):
                self.assertGreaterEqual(cache_end, base + value)
            offset += 16 + length
        self.assertEqual(size, 0x321000)  # Includes zero-initialized runtime data, not only the file.
        self.assertEqual(fixed, [(0x60000000, 0x2000)])
        self.assertEqual(queue, (0x60002000, 0x4000))
        self.assertEqual(log, (0x60006000, 0x4000))

    def test_sparse_records_share_pages_and_leave_the_first_queue_sized_hole(self):
        image = (request(0x60006820, 0x70, 7) + request(0xc000c000, 0x800) +
                 request(0x60000000, 0x1010) + request(0x60006a00, 0x60) +
                 request(0x60000100, 0x1010) + payload(0x5ffc))
        self.assertEqual(self.layout(image),
                         (0x2000, [(0x60000000, 0x2000), (0x60006000, 0x1000),
                                   (0xc000c000, 0x1000)], (0x60002000, 0x4000), (0x60007000, 0x4000)))

    def test_tcm_and_nonallocation_metadata_do_not_create_dma_mappings(self):
        image = (request(0, 0x4000) + request(0x10004000, 0x4000) +
                 request(0xf1000000, 0x99999999, 4) + payload())
        self.assertEqual(self.layout(image), (0, [], (0x60000000, 0x4000), (0x60004000, 0x4000)))

    def test_malformed_firmware_and_missing_queue_space_are_rejected(self):
        for image in (payload()[:-1], request(0, 0x4001) + payload(),
                      request(0x7ffffffc, 8) + payload(), request(0xfffffffc, 16) + payload(),
                      request(0x60000000, 0x20000000) + payload(),
                      request(0x60004000, 0x1fffc000) + payload(),
                      request(0x4000, 0) + payload(), payload(0x10008000)):
            with self.subTest(image=image):
                self.assertIsNone(self.layout(image))


if __name__ == '__main__':
    unittest.main()
