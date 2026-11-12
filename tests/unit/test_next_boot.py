"""Exercise the real native command only on a small temporary regular file."""
import sys
import hashlib
import json
import runpy
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from core_source import core_source

ROOT = Path(__file__).resolve().parents[2]
GENERATION = hashlib.sha256(b'next-boot-host-fixture').digest()[:16]
HELPER = runpy.run_path(str(ROOT/'linux/userspace/piano-next-boot-helper'))


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

    def test_gpt_slot_selection_and_corrupt_or_ambiguous_metadata(self):
        for sector in (512, 4096):
            with self.subTest(sector=sector):
                disk = bytearray(sector * 256)
                entries = bytearray(128 * 8)
                def entry(index, name, first, last, attributes):
                    at = index * 128
                    entries[at:at+16] = b't' * 16
                    entries[at+16:at+32] = bytes([index+1]) * 16
                    struct.pack_into('<3Q', entries, at+32, first, last, attributes)
                    entries[at+56:at+56+len(name)*2] = name.encode('utf-16le')
                entry(0, 'boot_a', 32, 63, 0x00f3000000000000)
                entry(1, 'boot_b', 64, 95, 0x0077000000000000)
                blocks = (len(entries)+sector-1)//sector
                for lba, alternate, table in ((1,255,2),(255,1,255-blocks)):
                    h = bytearray(sector);h[:8] = b'EFI PART'
                    struct.pack_into('<II', h, 8, 0x10000, 92)
                    struct.pack_into('<4Q', h, 24, lba, alternate, 16, 240)
                    h[56:72] = b'd'*16
                    struct.pack_into('<Q3I', h, 72, table, 8, 128, zlib.crc32(entries))
                    struct.pack_into('<I', h, 16, zlib.crc32(h[:92]))
                    disk[lba*sector:(lba+1)*sector] = h
                    disk[table*sector:table*sector+len(entries)] = entries
                read = lambda size, offset: disk[offset:offset+size]
                _, boots = HELPER['read_gpt'](read, sector, len(disk))
                self.assertEqual(HELPER['select_active_boot'](boots), 'boot_b')
                changed = dict(boots)
                changed['boot_a'] = (*boots['boot_a'][:2], 0x0077000000000000, boots['boot_a'][3])
                with self.assertRaises(ValueError):HELPER['select_active_boot'](changed)
                changed['boot_b'] = (*boots['boot_b'][:2], 0x00f3000000000000, boots['boot_b'][3])
                self.assertEqual(HELPER['select_active_boot'](changed), 'boot_a')
                changed['boot_a'] = (*boots['boot_a'][:2], 0x0007000000000000, boots['boot_a'][3])
                with self.assertRaises(ValueError):HELPER['select_active_boot'](changed)
                disk[(255-blocks)*sector+48] ^= 1
                with self.assertRaises(ValueError):HELPER['read_gpt'](read, sector, len(disk))
                disk[(255-blocks)*sector+48] ^= 1
                disk[sector+16] ^= 1
                with self.assertRaises(ValueError):HELPER['read_gpt'](read, sector, len(disk))


if __name__ == '__main__':
    unittest.main()
