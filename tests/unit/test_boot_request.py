"""Compile the pure selector and use only temporary regular-file request pages."""
import ctypes
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'uefi/handoff/bootselect/BootRequest.c'
GENERATION = hashlib.sha256(b'HOST FIXTURE APPv1').digest()[:16]


def record(sequence=0, target=0, generation=GENERATION):
    data = bytearray(64)
    struct.pack_into('<16sIIQII16s8s', data, 0, b'SUNUEFI-NEXTv1', 1, 64,
                     sequence, target, 0, generation, b'\0' * 8)
    struct.pack_into('<I', data, 36, zlib.crc32(data))
    return bytes(data)


def pages(first=None, second=None):
    return (record() if first is None else first).ljust(4096, b'\0') + \
           (record() if second is None else second).ljust(4096, b'\0')


class HostNativeReqWriter:
    """Fixture writer, not an Android/block-device implementation."""
    def __init__(self, path):
        self.path = path
        self.path.write_bytes(pages())

    def write_page(self, index, data, torn_at=None):
        assert index in (0, 1) and len(data) == 64
        block = data.ljust(4096, b'\0')
        with self.path.open('r+b') as stream:
            stream.seek(index * 4096)
            stream.write(block if torn_at is None else block[:torn_at])


class BootRequestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('Host C compiler is required')
        cls.temp = tempfile.TemporaryDirectory(prefix='boot-request-tests-')
        cls.base = Path(cls.temp.name)
        subprocess.run([compiler, '-shared', '-fPIC', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(SOURCE), '-o', str(cls.base / 'request.so')], check=True)
        cls.lib = ctypes.CDLL(str(cls.base / 'request.so'))
        cls.lib.PianoBootRequestSelect.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        cls.lib.PianoBootRequestSelect.restype = ctypes.c_uint
        cls.lib.PianoBootRequestCrc32.argtypes = [ctypes.c_void_p]
        cls.lib.PianoBootRequestCrc32.restype = ctypes.c_uint32

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def select(self, data, generation=GENERATION, unaligned=False):
        buffer = ctypes.create_string_buffer((b'x' if unaligned else b'') + data)
        before = buffer.raw
        result = self.lib.PianoBootRequestSelect(ctypes.byref(buffer, int(unaligned)),
                                                ctypes.create_string_buffer(generation))
        self.assertEqual(before, buffer.raw)
        return result

    def test_crc_agrees_with_host_reference_and_ignores_stored_crc(self):
        for target in range(4):
            data = record(1, target)
            expected = struct.unpack_from('<I', data, 36)[0]
            self.assertEqual(self.lib.PianoBootRequestCrc32(ctypes.create_string_buffer(data)), expected)
            changed = bytearray(data);changed[36:40] = b'XXXX'
            self.assertEqual(self.lib.PianoBootRequestCrc32(ctypes.create_string_buffer(bytes(changed))), expected)

    def test_default_zero_missing_and_null_are_stock(self):
        self.assertEqual(self.select(pages()), 0)
        self.assertEqual(self.select(b'\0' * 8192), 0)
        self.assertEqual(self.lib.PianoBootRequestSelect(None, ctypes.create_string_buffer(GENERATION)), 0)
        self.assertEqual(self.lib.PianoBootRequestSelect(ctypes.create_string_buffer(pages()), None), 0)

    def test_highest_valid_sequence_and_new_none_cancels_request(self):
        for target in (1, 2, 3):
            self.assertEqual(self.select(pages(record(7, target), record(6, 1))), target)
            self.assertEqual(self.select(pages(record(6, 1), record(7, target))), target)
        self.assertEqual(self.select(pages(record(7, 2), record(8, 0))), 0)
        self.assertEqual(self.select(pages(record(2**64 - 1, 3), record(8, 1))), 3)

    def test_equal_sequence_conflict_is_stock(self):
        self.assertEqual(self.select(pages(record(5, 1), record(5, 2))), 0)
        self.assertEqual(self.select(pages(record(5, 3), record(5, 3))), 3)
        self.assertEqual(self.select(pages(record(5, 0), record(5, 3))), 0)

    def test_malformed_record_does_not_mask_valid_other_page(self):
        good = record(3, 2)
        for offset in (0, 14, 16, 20, 24, 32, 36, 40, 56, 63):
            with self.subTest(offset=offset):
                bad = bytearray(record(9, 1));bad[offset] ^= 0xff
                self.assertEqual(self.select(pages(bytes(bad), good)), 2)
                self.assertEqual(self.select(pages(good, bytes(bad))), 2)
                self.assertEqual(self.select(pages(bytes(bad), bytes(bad))), 0)

    def test_recomputed_crc_cannot_allow_bad_version_size_reserved_or_range(self):
        for offset, value in ((16, 2), (20, 63), (32, 4), (56, 1)):
            with self.subTest(offset=offset):
                bad = bytearray(record(4, 1));struct.pack_into('<I', bad, offset, value)
                struct.pack_into('<I', bad, 36, 0);struct.pack_into('<I', bad, 36, zlib.crc32(bad))
                self.assertEqual(self.select(pages(bytes(bad), bytes(bad))), 0)
        self.assertEqual(self.select(pages(record(0, 1), record(0, 3))), 0)

    def test_stale_or_zero_generation_is_stock(self):
        old = record(17, 1)
        self.assertEqual(self.select(pages(old, old), b'other-generation'), 0)
        self.assertEqual(self.select(pages(record(9, 1, b'\0' * 16), record(9, 1, b'\0' * 16)), b'\0' * 16), 0)

    def test_regular_file_torn_inactive_page_continues_old_request(self):
        writer = HostNativeReqWriter(self.base / 'request-pages.fixture')
        writer.write_page(0, record(11, 1))
        writer.write_page(1, record(12, 2), torn_at=35)
        self.assertEqual(self.select(writer.path.read_bytes()), 1)
        writer.write_page(1, record(12, 2))
        self.assertEqual(self.select(writer.path.read_bytes()), 2)
        writer.write_page(0, record(13, 0))
        self.assertEqual(self.select(writer.path.read_bytes()), 0)
        self.assertEqual(writer.path.stat().st_size, 8192)

    def test_unaligned_records_and_page_tail_are_not_interpreted(self):
        data = bytearray(pages(record(1, 3), record()))
        data[64:4096] = b'\xff' * (4096 - 64)
        data[4096 + 64:] = b'\xff' * (4096 - 64)
        self.assertEqual(self.select(bytes(data), unaligned=True), 3)

    def test_exact_8192_readonly_span_with_guard_pages(self):
        harness = self.base / 'guard.c'
        harness.write_text('''#define _GNU_SOURCE
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include "BootRequest.h"
int main(void) {
  unsigned char *map, *p, gen[16]; unsigned crc;
  if (sysconf(_SC_PAGESIZE) != 4096) return 77;
  map = mmap(0, 16384, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (map == MAP_FAILED || mprotect(map + 4096, 8192, PROT_READ | PROT_WRITE)) return 2;
  p = map + 4096; memset(gen, 0x34, sizeof(gen));
  memcpy(p + 4096, "SUNUEFI-NEXTv1", 14); p[4096 + 16] = 1; p[4096 + 20] = 64;
  p[4096 + 24] = 1; p[4096 + 32] = 2; memcpy(p + 4096 + 40, gen, 16);
  crc = PianoBootRequestCrc32(p + 4096);
  for (unsigned i = 0; i < 4; ++i) p[4096 + 36 + i] = (unsigned char)(crc >> (8 * i));
  if (mprotect(p, 8192, PROT_READ)) return 3;
  return PianoBootRequestSelect(p, gen) == 2 ? 0 : 4;
}
''')
        binary = self.base / 'guard'
        subprocess.run([shutil.which('cc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I', str(SOURCE.parent), str(harness), str(SOURCE), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True)
        if result.returncode == 77:
            self.skipTest('Guard fixture needs a host with 4096-byte pages')
        self.assertEqual(result.returncode, 0, 'Read escaped the two pages or attempted a write')


if __name__ == '__main__':
    unittest.main()
