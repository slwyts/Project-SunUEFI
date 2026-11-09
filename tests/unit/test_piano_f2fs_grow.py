"""Check early-expansion receipts and file-preserving growth on real images."""
import os
import ctypes
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
F2FS = ROOT / 'build/android-f2fs-layout-host'


class PianoF2fsGrowTests(unittest.TestCase):
    def test_only_one_confirmed_capacity_and_job_are_accepted(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as folder:
            binary = Path(folder) / 'grow'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                            str(ROOT / 'tests/native/test_piano_f2fs_grow.c'), '-o', str(binary)],
                           check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=10,
                            env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})

    @unittest.skipUnless((F2FS / 'resize.f2fs').is_file(), 'Build the host F2FS tools for image regression')
    def test_real_shrink_regrow_preserves_files_and_main_area(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as folder:
            folder = Path(folder)
            source, output = folder / 'input', folder / 'dump'
            source.mkdir()
            output.mkdir()
            payload = bytes(range(256)) * 16384
            (source / 'retained.bin').write_bytes(payload)
            image = folder / 'data.img'
            with image.open('wb') as stream:
                stream.truncate(4 << 30)

            def run(name, *args):
                return subprocess.run([str(F2FS / name), *map(str, args)], input='y\n',
                                      text=True, capture_output=True, timeout=90)

            for name, args in (('mkfs.f2fs', ('-f', '-O', 'extra_attr,inode_checksum,sb_checksum,encrypt', image)),
                               ('sload.f2fs', ('-f', source, image)),
                               ('resize.f2fs', ('-s', '-t', (2 << 30) // 512, image))):
                result = run(name, *args)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            with image.open('r+b') as stream:
                stream.truncate(2 << 30)
                stream.truncate(3 << 30)
                stream.seek(1024)
                before = stream.read(3072)
                cp_start = struct.unpack_from('<I', before, 76)[0]
                checkpoints = []
                for block in (cp_start, cp_start + 512):
                    stream.seek(block * 4096)
                    data = stream.read(4096)
                    checkpoints.append((struct.unpack_from('<Q', data)[0], block, data))
                _, cp_block, checkpoint = max(checkpoints)
                self.assertFalse(struct.unpack_from('<I', checkpoint, 132)[0] & 0x400)
                main_segments = struct.unpack_from('<I', before, 68)[0]
                sit_base = struct.unpack_from('<I', before, 80)[0]
                sit_segments = struct.unpack_from('<I', before, 56)[0]
                # Construct the journal state emitted by Android online shrink:
                # an empty old-tail record before a still-valid in-range record.
                for segment in range(main_segments):
                    page, slot = divmod(segment, 55)
                    alternate = bool(checkpoint[192 + page // 8] & (1 << (7 - page % 8)))
                    offset = (sit_base + page + alternate * sit_segments // 2 * 512) * 4096 + slot * 74
                    stream.seek(offset)
                    entry = stream.read(74)
                    if struct.unpack_from('<H', entry)[0] & 0x3ff:
                        break
                else:
                    self.fail('Populated image has no valid SIT entry')
                stream.seek(offset)
                stream.write(bytes(74))
                summary = cp_block + struct.unpack_from('<I', checkpoint, 140)[0] + 2
                stream.seek(summary * 4096 + 512 * 7)
                journal = struct.pack('<H', 2) + struct.pack('<I', main_segments) + bytes(74) + \
                    struct.pack('<I', segment) + entry
                stream.write(journal)

            # A nonempty removed-tail entry must be rejected, not silently lost.
            with image.open('r+b') as stream:
                stream.seek(summary * 4096 + 512 * 7 + 6)
                stream.write(struct.pack('<H', 1) + b'\x80' + bytes(71))
            result = run('resize.f2fs', '-P', image)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Nonempty SIT journal entry', result.stdout)
            with image.open('r+b') as stream:
                stream.seek(summary * 4096 + 512 * 7)
                stream.write(journal)
            start = struct.unpack_from('<I', before, 92)[0] * 4096
            # Hash allocated file bytes through dump, and the complete old main
            # area to catch relocation of encrypted or unreferenced blocks.
            length = struct.unpack_from('<I', before, 68)[0] * 512 * 4096

            def main_hash():
                value = hashlib.sha256()
                with image.open('rb') as stream:
                    stream.seek(start)
                    left = length
                    while left:
                        data = stream.read(min(left, 4 << 20))
                        self.assertTrue(data)
                        value.update(data)
                        left -= len(data)
                return value.hexdigest()

            original = main_hash()
            for args in (('-P', image), ('-k', image)):
                result = run('resize.f2fs', *args)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = run('fsck.f2fs', '--dry-run', '--no-kernel-check', '-N', image)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(main_hash(), original)
            with image.open('rb') as stream:
                stream.seek(1024)
                after = stream.read(3072)
            self.assertEqual(before[72:124], after[72:124])
            result = run('dump.f2fs', '-r', '-f', '-o', output, image)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((output / 'retained.bin').read_bytes(), payload)

            # Exercise the actual GPT preflight against the same on-disk layout.
            library = folder / 'capacity.so'
            subprocess.run(['cc', '-shared', '-fPIC', '-std=c11', '-O2',
                            str(ROOT / 'tools/android/piano_resize_f2fs.c'),
                            str(ROOT / 'tools/android/piano_f2fs_grow.c'), '-o', str(library)], check=True)
            capacity = ctypes.CDLL(str(library)).piano_f2fs_layout_capacity
            capacity.argtypes = [ctypes.c_char_p, ctypes.c_uint64]
            self.assertEqual(capacity(os.fsencode(image), 4 << 30), 0)
            self.assertNotEqual(capacity(os.fsencode(image), 64 << 30), 0)


if __name__ == '__main__':
    unittest.main()
