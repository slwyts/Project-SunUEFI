"""Exercise the WN8030 wire reports and audit the report lengths seen by HIDClass."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'windows/drivers/wn8030'


class Wn8030Input(unittest.TestCase):
    def test_wire_to_windows_reports(self):
        with tempfile.TemporaryDirectory() as temp:
            exe = Path(temp) / 'reports'
            subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-I', DRIVER,
                            DRIVER / 'reports.c', ROOT / 'tests/native/Wn8030ReportsTest.c',
                            '-o', exe], check=True)
            subprocess.run([exe], check=True)

    def test_hid_class_report_lengths(self):
        with tempfile.TemporaryDirectory() as temp:
            so = Path(temp) / 'reports.so'
            subprocess.run(['cc', '-std=c17', '-shared', '-fPIC', DRIVER / 'reports.c',
                            '-o', so], check=True)
            dll = ctypes.CDLL(str(so))
            length = ctypes.c_size_t.in_dll(dll, 'wn8030_descriptor_length').value
            descriptor = bytes((ctypes.c_ubyte * length).in_dll(dll, 'wn8030_descriptor'))
        bits = {}
        width = count = report_id = depth = offset = 0
        while offset < length:
            tag = descriptor[offset]
            size = (0, 1, 2, 4)[tag & 3]
            value = int.from_bytes(descriptor[offset + 1: offset + size + 1], 'little')
            offset += 1 + size
            prefix = tag & 0xfc
            if prefix == 0x74:
                width = value
            elif prefix == 0x94:
                count = value
            elif prefix == 0x84:
                report_id = value
            elif prefix == 0xa0:
                depth += 1
            elif prefix == 0xc0:
                depth -= 1
                self.assertGreaterEqual(depth, 0)
            elif prefix in (0x80, 0x90, 0xb0):
                self.assertGreater(depth, 0)
                key = (prefix, report_id)
                bits[key] = bits.get(key, 0) + width * count
        self.assertEqual((offset, depth), (length, 0))
        sizes = {key: (total + 7) // 8 + 1 for key, total in bits.items()}
        self.assertEqual(sizes, {(0x80, 5): 9, (0x90, 5): 2, (0x80, 6): 3, (0x80, 2): 8,
                                 (0x80, 0x19): 23, (0xb0, 0x30): 3, (0xb0, 0x31): 257,
                                 (0xb0, 0x32): 2, (0xb0, 0x33): 2})


if __name__ == '__main__':
    unittest.main()
