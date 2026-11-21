"""Exercise the cold download of the actual WN8030 RAM image."""
import ctypes as ct
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'windows/drivers/wn8030'
FIRMWARE = ROOT / 'vendor/firmware/piano-keyboard/MCU_Upgrade.bin'
PTR = ct.POINTER(ct.c_ubyte)
READ_IMAGE = ct.CFUNCTYPE(ct.c_int, ct.c_void_p, ct.c_uint64, PTR, ct.c_uint32)
WRITE = ct.CFUNCTYPE(ct.c_int, ct.c_void_p, PTR, ct.c_uint32)
READ = ct.CFUNCTYPE(ct.c_int, ct.c_void_p, ct.c_ubyte, PTR)
WAIT = ct.CFUNCTYPE(None, ct.c_void_p, ct.c_uint)


class Bus(ct.Structure):
    _fields_ = [('context', ct.c_void_p), ('image', READ_IMAGE), ('write', WRITE),
                ('read', READ), ('wait', WAIT)]


class Wn8030Boot(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temp.cleanup)
        library = Path(temp.name) / 'boot.so'
        subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
                        DRIVER / 'boot.c', '-o', library], check=True)
        cls.boot = ct.CDLL(str(library)).wn8030_boot_ram
        cls.boot.argtypes = [ct.POINTER(Bus), ct.c_uint64]
        cls.firmware = FIRMWARE.read_bytes()

    def transfer(self, image, replies=(0xc8, 0, 1, 3), fail=None):
        writes, waits, regs = [], [], []
        values = iter(replies)

        def read_image(_, offset, out, length):
            if offset > len(image) or length > len(image) - offset:
                return 0
            ct.memmove(out, image[offset:offset + length], length)
            return 1

        def write(_, data, size):
            packet = ct.string_at(data, size)
            writes.append(packet)
            return packet[1] != fail

        def read(_, reg, out):
            regs.append(reg)
            value = next(values, None)
            if value is None:
                return 0
            out[0] = value
            return 1

        io = Bus(None, READ_IMAGE(read_image), WRITE(write), READ(read),
                 WAIT(lambda _, ms: waits.append(ms)))
        return self.boot(ct.byref(io), len(image)), writes, waits, regs

    def test_stock_image_reaches_verified_start(self):
        result, writes, waits, regs = self.transfer(self.firmware)
        self.assertEqual(result, 0)
        self.assertEqual(regs, [0x5c] * 4)
        self.assertTrue(all(p[0] == 0x5c for p in writes))
        size, = struct.unpack_from('<I', self.firmware, 0x7fc0 + 48)
        code = self.firmware[0x8000:0x8000 + size]
        chunks = (size + 502) // 503
        self.assertEqual([p[1] for p in writes], [4, 5, 6, 5] + [7] * chunks + [8, 5, 9])
        hdr = writes[2]
        self.assertEqual(len(hdr), 66)
        self.assertEqual(hdr[2:9], b'NANO IC')
        self.assertEqual(struct.unpack_from('<HIII', hdr, 9), (0x8140, 0x2345, 0x40, size))
        self.assertEqual(hdr[23:33], self.firmware[0x7fc0 + 21:0x7fc0 + 31])
        self.assertEqual(struct.unpack_from('<I', hdr, 54)[0],
                         sum(v for v, in struct.iter_unpack('<I', code)) & 0xffffffff)
        self.assertEqual(hdr[-1], sum(hdr[2:-1]) & 0xff)
        for i, p in enumerate(writes[4:-3]):
            off, length = struct.unpack_from('>II', p, 2)
            self.assertEqual((off, length, len(p)), (503 * i, min(503, size - 503 * i), 513))
            self.assertEqual(p[10:], code[off:off + length].ljust(503, b'\0'))
        self.assertEqual(waits, [3] * (len(writes) - 3) + [13, 3, 53])

    def test_never_start_after_failed_identification_transfer_or_verification(self):
        for replies, fail, want in (((0,), None, 3), ((0xc8, 2), None, 4),
                                   ((0xc8, 0, 0), None, 5), ((0xc8, 0, 1, 2), None, 6),
                                   ((0xc8, 0, 1), 7, 1)):
            with self.subTest(replies=replies, fail=fail):
                result, writes, _, _ = self.transfer(self.firmware, replies, fail)
                self.assertEqual(result, want)
                self.assertNotIn(9, [p[1] for p in writes])
        result, writes, _, _ = self.transfer(self.firmware[:0x8004])
        self.assertEqual((result, writes), (2, []))


if __name__ == '__main__':
    unittest.main()
