"""Fake RAM/cache callback only; no MMIO, CurrentEL reads or reboot claims."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'uefi/handoff/bootselect/EarlyTrace.c'
RAM_BYTES = 0x200000
CAPACITY = RAM_BYTES - 12


def marker(stage, image=0xa8000000, dtb=0xab000000, trace_id='host-test'):
    return f'[SUNUEFI-EARLY] id={trace_id} stage={stage} image=0x{image:016x} dtb=0x{dtb:016x}\n'.encode()


class EarlyTraceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which('cc')
        if not cls.compiler:
            raise unittest.SkipTest('Host C compiler is required')
        cls.temp = tempfile.TemporaryDirectory(prefix='early-trace-tests-')
        cls.base = Path(cls.temp.name)
        cls.shim = cls.base / 'cache.c'
        cls.shim.write_text('''#include <stdint.h>
#include "EarlyTrace.h"
void *PianoBootSelectTraceHostRam;
static uint64_t offsets[8], sizes[8]; static unsigned calls;
void PianoBootSelectCleanPoC(void *p, uint64_t n) {
  if (calls < 8) { offsets[calls] = (uintptr_t)p - (uintptr_t)PianoBootSelectTraceHostRam; sizes[calls] = n; }
  ++calls;
}
void ResetCache(void) { calls = 0; }
unsigned CacheCalls(void) { return calls; }
uint64_t CacheOffset(unsigned i) { return offsets[i]; }
uint64_t CacheSize(unsigned i) { return sizes[i]; }
''')
        subprocess.run([cls.compiler, '-shared', '-fPIC', '-O2', '-std=c11', '-ffreestanding',
                        '-fno-builtin', '-Wall', '-Wextra', '-Werror', '-DHOSTTEST',
                        '-I', str(SOURCE.parent), str(SOURCE), str(cls.shim),
                        '-o', str(cls.base / 'trace.so')], check=True)
        cls.lib = ctypes.CDLL(str(cls.base / 'trace.so'))
        cls.lib.PianoBootSelectTrace.argtypes = [ctypes.c_uint, ctypes.c_uint64, ctypes.c_uint64]
        cls.lib.CacheCalls.restype = ctypes.c_uint
        for name in ('CacheOffset', 'CacheSize'):
            getattr(cls.lib, name).argtypes = [ctypes.c_uint]
            getattr(cls.lib, name).restype = ctypes.c_uint64

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.ram = ctypes.create_string_buffer(b'\x5a' * RAM_BYTES, RAM_BYTES)
        ctypes.c_void_p.in_dll(self.lib, 'PianoBootSelectTraceHostRam').value = ctypes.addressof(self.ram)
        self.lib.ResetCache()

    def header(self, signature=0x43474244, start=0, size=0):
        struct.pack_into('<III', self.ram, 0, signature, start, size)

    def calls(self):
        return [(self.lib.CacheOffset(i), self.lib.CacheSize(i)) for i in range(self.lib.CacheCalls())]

    def trace(self, stage=1, image=0xa8000000, dtb=0xab000000):
        self.lib.PianoBootSelectTrace(stage, image, dtb)

    def test_invalid_signature_header_or_stage_never_changes_ram(self):
        for signature, start, size, stage in ((0, 0, 0, 1), (0x43474244, CAPACITY, 0, 1),
                (0x43474244, 0, CAPACITY + 1, 1), (0x43474244, 0, 0, 0),
                (0x43474244, 0, 0, 7), (0x43474244, 2**32 - 1, 2**32 - 1, 1),
                (0x43474244, 18, 17, 1)):
            with self.subTest(header=(signature, start, size), stage=stage):
                self.header(signature, start, size);before = self.ram.raw
                self.trace(stage)
                self.assertEqual(self.ram.raw, before)
                self.assertEqual(self.calls(), [])

    def test_existing_ring_append_preserves_signature_and_other_data(self):
        self.header(start=17, size=17);before = self.ram.raw
        self.trace(2)
        text = marker(2)
        self.assertEqual(self.ram.raw[12 + 17:12 + 17 + len(text)], text)
        self.assertEqual(self.ram.raw[:4], before[:4])
        self.assertEqual(self.ram.raw[12:29], before[12:29])
        self.assertEqual(self.ram.raw[29 + len(text):], before[29 + len(text):])
        self.assertEqual(struct.unpack_from('<II', self.ram, 4), (17 + len(text), 17 + len(text)))
        self.assertEqual(self.calls(), [(29, len(text)), (0, 12)])

    def test_wrap_cleans_only_two_written_segments_then_header(self):
        self.header(start=CAPACITY - 5, size=CAPACITY - 2)
        self.trace(5)
        text = marker(5)
        self.assertEqual(self.ram.raw[-5:], text[:5])
        self.assertEqual(self.ram.raw[12:12 + len(text) - 5], text[5:])
        self.assertEqual(struct.unpack_from('<II', self.ram, 4), (len(text) - 5, CAPACITY))
        self.assertEqual(self.calls(), [(RAM_BYTES - 5, 5), (12, len(text) - 5), (0, 12)])

    def test_exact_end_wrap_and_all_six_stages_have_fixed_ascii(self):
        for stage in range(1, 7):
            with self.subTest(stage=stage):
                self.lib.ResetCache();text = marker(stage, 2**64 - 1, 0)
                self.header(start=CAPACITY - len(text), size=CAPACITY)
                self.trace(stage, 2**64 - 1, 0)
                self.assertEqual(self.ram.raw[-len(text):], text)
                self.assertEqual(struct.unpack_from('<II', self.ram, 4), (0, CAPACITY))
                self.assertEqual(self.calls(), [(RAM_BYTES - len(text), len(text)), (0, 12)])

    def test_null_host_ring_is_noop(self):
        ctypes.c_void_p.in_dll(self.lib, 'PianoBootSelectTraceHostRam').value = None
        self.trace()
        self.assertEqual(self.calls(), [])

    def test_compiled_candidate_id_is_visible_in_fixed_marker(self):
        identity = '0123456789abcdef'
        binary = self.base / 'candidate.so'
        subprocess.run([self.compiler, '-shared', '-fPIC', '-O2', '-std=c11', '-DHOSTTEST',
                        '-DPIANO_BOOTSELECT_TRACE_ID="' + identity + '"',
                        '-I', str(SOURCE.parent), str(SOURCE), str(self.shim), '-o', str(binary)], check=True)
        candidate = ctypes.CDLL(str(binary))
        candidate.PianoBootSelectTrace.argtypes = [ctypes.c_uint, ctypes.c_uint64, ctypes.c_uint64]
        ctypes.c_void_p.in_dll(candidate, 'PianoBootSelectTraceHostRam').value = ctypes.addressof(self.ram)
        self.header()
        candidate.PianoBootSelectTrace(3, 0xa8000000, 0xab000000)
        text = marker(3, trace_id=identity)
        self.assertEqual(self.ram.raw[12:12 + len(text)], text)

    def test_asan_guarded_ring_boundaries(self):
        harness = self.base / 'asan.c'
        harness.write_text('''#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "EarlyTrace.h"
void *PianoBootSelectTraceHostRam;
static unsigned calls;
void PianoBootSelectCleanPoC(void *p, uint64_t n) {
  uintptr_t at = (uintptr_t)p - (uintptr_t)PianoBootSelectTraceHostRam;
  if (!n || at > PIANO_EARLY_TRACE_RAM_BYTES || n > PIANO_EARLY_TRACE_RAM_BYTES - at ||
      (at == 0 && n != 12) || (at != 0 && at < 12) || n > 96) abort();
  ++calls;
}
int main(void) {
  uint32_t *p = malloc(PIANO_EARLY_TRACE_RAM_BYTES);
  if (!p) return 2;
  PianoBootSelectTraceHostRam = p; memset(p, 0x5a, PIANO_EARLY_TRACE_RAM_BYTES);
  for (unsigned stage = 1; stage <= 6; ++stage) {
    for (unsigned tail = 1; tail < 100; ++tail) {
      p[0] = 0x43474244; p[1] = PIANO_EARLY_TRACE_CAPACITY - tail; p[2] = PIANO_EARLY_TRACE_CAPACITY;
      PianoBootSelectTrace(stage, UINT64_MAX, UINT64_MAX);
      if (p[0] != 0x43474244 || p[1] >= PIANO_EARLY_TRACE_CAPACITY || p[2] != PIANO_EARLY_TRACE_CAPACITY) return 3;
    }
  }
  p[1] = PIANO_EARLY_TRACE_CAPACITY; calls = 0; PianoBootSelectTrace(1, 0, 0);
  if (calls) return 4;
  free(p); return 0;
}
''')
        binary = self.base / 'asan'
        subprocess.run([self.compiler, '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror', '-DHOSTTEST',
                        '-I', str(SOURCE.parent), str(SOURCE), str(harness), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())


if __name__ == '__main__':
    unittest.main()
