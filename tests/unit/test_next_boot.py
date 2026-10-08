"""Exercise the real native command only on a small temporary regular file."""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
GENERATION = hashlib.sha256(b'next-boot-host-fixture').digest()[:16]


def fixture():
    image = bytearray(4096 + 16384)
    image[:8] = b'ANDROID!'
    struct.pack_into('<I', image, 8, 16384)
    struct.pack_into('<I', image, 40, 4)
    struct.pack_into('<I', image, 4100, (5 << 26) | ((12288 - 4) // 4))
    image[4152:4156] = b'ARMd'
    for at in (8192, 12288):
        record = bytearray(struct.pack('<16sIIQII16s8s', b'SUNUEFI-NEXTv1', 1, 64,
                                       0, 0, 0, GENERATION, bytes(8)))
        struct.pack_into('<I', record, 36, zlib.crc32(record))
        image[at:at+64] = record
    at = 16384
    image[at:at+16] = b'SUNUEFI-SPLITv1'.ljust(16, b'\0')
    struct.pack_into('<IIQQ', image, at+16, 1, 128, 12288, 4096)
    struct.pack_into('<QQQ', image, at+72, 15360, 512, 16384)
    image[at+96:at+112] = GENERATION
    return bytes(image)


class NextBootTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='next-boot-host-')
        cls.base = Path(cls.temp.name)
        cls.binary = cls.base / 'piano-boot-request'
        source = ROOT / 'uefi/handoff/bootselect'
        subprocess.run([shutil.which('cc'), '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(source), str(ROOT/'android/native/piano-boot-request.c'),
                        str(source/'BootRequest.c'), '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_native(self, *args):
        return subprocess.run([str(self.binary), *args], capture_output=True, text=True)

    def test_generation_pin_rejects_stale_before_any_file_write(self):
        path = self.base / 'stale.img'
        before = fixture(); path.write_bytes(before)
        result = self.run_native('set', '--device', str(path), '--target', 'menu',
                                 '--expect-generation', '11'*16)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('generation differs', result.stderr)
        self.assertEqual(path.read_bytes(), before)

    def test_native_status_set_and_consume_use_actual_shared_crc(self):
        path = self.base / 'valid.img'
        before = fixture(); path.write_bytes(before)
        record = json.loads(self.run_native('status', '--device', str(path)).stdout)
        self.assertEqual(record['app_generation'], GENERATION.hex())
        self.assertEqual(path.read_bytes(), before)
        result = self.run_native('set', '--device', str(path), '--target', 'menu',
                                 '--expect-generation', GENERATION.hex().upper())
        self.assertEqual(result.returncode, 0, result.stderr)
        record = json.loads(result.stdout)
        self.assertEqual((record['target'], record['sequence'], record['app_generation']),
                         (1, 1, GENERATION.hex()))
        after = path.read_bytes()
        self.assertEqual(before[:8192], after[:8192])
        self.assertEqual(before[8192+64:], after[8192+64:])
        result = self.run_native('consume', '--device', str(path), '--expect-generation', GENERATION.hex())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['target'], 0)
        # The old syntax remains available; no generation pin is inferred.
        self.assertEqual(self.run_native('set', '--device', str(path), '--target', 'linux').returncode, 0)

    def test_bad_generation_and_helper_arbitrary_arguments_rejected(self):
        result = self.run_native('set', '--device', '/nonexistent', '--expect-generation', 'z'*32)
        self.assertIn('32 hex digits', result.stderr)
        result = subprocess.run([str(ROOT/'linux/userspace/piano-next-boot-helper'), 'android', '--device', '/nonexistent'],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('用法', result.stderr)
        result = subprocess.run([str(ROOT/'linux/userspace/piano-next-boot-helper'), 'status', '--reboot'],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('只读', result.stderr)


if __name__ == '__main__':
    unittest.main()
