"""Exercise the Windows driver's GMU block loader with the shipped Piano image."""
import ctypes as ct
from pathlib import Path
import struct
import subprocess
from tempfile import TemporaryDirectory
import unittest


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'windows/drivers/adreno'
FIRMWARE = ROOT / 'vendor/firmware/piano-qcom/gen80000_gmu.bin'


class Region(ct.Structure):
    _fields_ = [('address', ct.c_uint32), ('size', ct.c_size_t)]


class Segment(ct.Structure):
    _fields_ = [('region', ct.c_size_t), ('offset', ct.c_size_t),
                ('data', ct.c_void_p), ('size', ct.c_size_t)]


Writer = ct.CFUNCTYPE(None, ct.c_void_p, ct.POINTER(Segment))


class GmuFirmware(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temp = TemporaryDirectory(prefix='piano-gmu-')
        cls.addClassCleanup(temp.cleanup)
        library = Path(temp.name) / 'gmu.so'
        subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        DRIVER / 'gmu_firmware.c', '-o', library], check=True)
        cls.lib = ct.CDLL(str(library))
        cls.lib.adreno_gmu_load.argtypes = [ct.c_void_p, ct.c_size_t, ct.POINTER(Region),
                                             ct.c_size_t, Writer, ct.c_void_p]
        cls.lib.adreno_gmu_load.restype = ct.c_int
        cls.regions = (Region * 3)((0, 0x4000), (0x10004000, 0x4000),
                                   (0x4000, 0x1000000 - 0x4000))

    def uploads(self, blob, regions=None):
        if regions is None:
            regions = self.regions
        segments = []

        @Writer
        def append(_, ptr):
            s = ptr.contents
            segments.append((regions[s.region].address + s.offset, ct.string_at(s.data, s.size)))

        return (self.lib.adreno_gmu_load(blob, len(blob), regions, len(regions), append, None),
                segments)

    def test_shipped_gmu_image(self):
        blob = FIRMWARE.read_bytes()
        expected = []
        offset = 0
        while offset < len(blob):
            address, size, _, _ = struct.unpack_from('<4I', blob, offset)
            offset += 16
            if size:
                expected.append((address, blob[offset:offset + size]))
                offset += size
        self.assertEqual(offset, len(blob))
        self.assertTrue(expected)
        self.assertEqual(self.uploads(blob), (1, expected))
        # A malformed last header must not leave TCM half programmed.
        self.assertEqual(self.uploads(blob + b'\0'), (0, []))

    def test_windows_firmware_must_fit_the_mapped_destination(self):
        def block(address, data):
            return struct.pack('<4I', address, len(data), 0, 0) + data

        prefix = block(0, b'GOOD')
        for address in (0x3ffc, 0x10007ffc, 0xfffffffc, 1):
            with self.subTest(address=address):
                self.assertEqual(self.uploads(prefix + block(address, b'12345678')), (0, []))
        self.assertEqual(self.uploads(prefix + block(0x4000, b'ODD')), (0, []))
        self.assertEqual(self.uploads(struct.pack('<4I', 0x60000000, 0, 7, 4096)), (0, []))
        overlap = (Region * 2)((0, 16), (0, 16))
        self.assertEqual(self.uploads(prefix, overlap), (0, []))


if __name__ == '__main__':
    unittest.main()
