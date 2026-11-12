"""Sparse exports from temporary image files; never invoke Fastboot or a device."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

import package_sparse_images as sparse


class SparsePackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.bundle = self.base / 'bundle'
        self.bundle.mkdir()
        self.output = self.base / 'sparse'
        self.contents = {'esp.img': b'E' * 4096 + bytes(8192),
                         'root.ext4.img': bytes(4096) + b'R' * 4096}

    def tearDown(self):
        self.temporary.cleanup()

    def manifest(self, names):
        record = {'kernel_release': 'fixture', 'files': {}}
        for name in names:
            data = self.contents[name]
            (self.bundle / name).write_bytes(data)
            record['files'][name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        (self.bundle / 'manifest.json').write_text(json.dumps(record))

    def expanded(self, path):
        data = path.read_bytes()
        magic, major, minor, header, chunk_header, block, blocks, chunks, crc = struct.unpack_from('<IHHHHIIII', data)
        self.assertEqual((magic, major, minor, header, chunk_header, block, crc),
                         (sparse.MAGIC, 1, 0, 28, 12, 4096, 0))
        offset, expanded = header, bytearray()
        for _ in range(chunks):
            kind, reserved, count, size = struct.unpack_from('<HHII', data, offset)
            self.assertEqual(reserved, 0)
            payload = data[offset + chunk_header:offset + size]
            if kind == sparse.RAW:
                self.assertEqual(len(payload), count * block)
                expanded.extend(payload)
            else:
                self.assertEqual((kind, len(payload)), (sparse.FILL, 4))
                expanded.extend(payload * (count * block // 4))
            offset += size
        self.assertEqual((offset, len(expanded)), (len(data), blocks * block))
        return bytes(expanded)

    def test_esp_only_ignores_unlisted_root_and_documents_only_esp(self):
        self.manifest(('esp.img',))
        # A leftover file must not make an undeclared root image an output.
        (self.bundle / 'root.ext4.img').write_bytes(b'UNLISTED ROOT FIXTURE')
        result = sparse.package(self.bundle, self.output)
        self.assertEqual(set(result['files']), {'esp.img'})
        self.assertEqual(self.expanded(self.output / 'esp.img'), self.contents['esp.img'])
        self.assertEqual({p.name for p in self.output.iterdir()},
                         {'esp.img', 'manifest.json', 'SHA256SUMS', 'README.txt'})
        self.assertEqual(json.loads((self.output / 'manifest.json').read_text()), result)
        self.assertEqual((self.output / 'SHA256SUMS').read_text(),
                         sparse.digest(self.output / 'esp.img') + '  esp.img\n')
        readme = (self.output / 'README.txt').read_text()
        self.assertIn('flash sunuefi_esp esp.img\n', readme)
        self.assertNotIn('sunuefi_root', readme)
        self.assertNotIn('root.ext4.img', readme)

    def test_full_bundle_preserves_both_images_and_commands(self):
        self.manifest(('esp.img', 'root.ext4.img'))
        result = sparse.package(self.bundle, self.output)
        self.assertEqual(set(result['files']), set(self.contents))
        for name, content in self.contents.items():
            self.assertEqual(self.expanded(self.output / name), content)
        readme = (self.output / 'README.txt').read_text()
        self.assertIn('flash sunuefi_root root.ext4.img\n', readme)
        self.assertIn('flash sunuefi_esp esp.img\n', readme)

    def test_declared_missing_source_is_rejected_before_output(self):
        for missing in ('esp.img', 'root.ext4.img'):
            with self.subTest(missing=missing):
                self.manifest(('esp.img', 'root.ext4.img'))
                (self.bundle / missing).unlink()
                with self.assertRaisesRegex(ValueError, 'Missing source image declared by bundle: ' + missing):
                    sparse.package(self.bundle, self.output)
                self.assertFalse(self.output.exists())

    def test_esp_must_be_declared_and_bundle_complete(self):
        self.manifest(('root.ext4.img',))
        with self.assertRaisesRegex(ValueError, 'must include esp.img'):
            sparse.package(self.bundle, self.output)
        self.assertFalse(self.output.exists())
        self.manifest(('esp.img',))
        (self.bundle / '.incomplete').touch()
        with self.assertRaisesRegex(ValueError, 'bundle is incomplete'):
            sparse.package(self.bundle, self.output)
        self.assertFalse(self.output.exists())

    def test_changed_source_cannot_publish_manifest(self):
        self.manifest(('esp.img',))
        (self.bundle / 'esp.img').write_bytes(b'X' * len(self.contents['esp.img']))
        with self.assertRaisesRegex(ValueError, 'does not match its bundle: esp.img'):
            sparse.package(self.bundle, self.output)
        for name in ('manifest.json', 'SHA256SUMS', 'README.txt'):
            self.assertFalse((self.output / name).exists())


if __name__ == '__main__':
    unittest.main()
