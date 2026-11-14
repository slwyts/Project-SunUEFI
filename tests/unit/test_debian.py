#!/usr/bin/env python3
"""Check signed-source metadata parsing without network or a device."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from build import debian


class DebianSourceTests(unittest.TestCase):
    def test_clearsigned_dsc_keeps_only_source_body(self):
        text = '''-----BEGIN PGP SIGNED MESSAGE-----
Hash: SHA512

Format: 3.0 (quilt)
Source: ffmpeg
Version: 7:9.0.2-6
Checksums-Sha256:
 abcdef 42 ffmpeg_9.0.2.orig.tar.xz
 012345 24 ffmpeg_9.0.2-6.debian.tar.xz

-----BEGIN PGP SIGNATURE-----
Version: Not a Debian source field

signature
-----END PGP SIGNATURE-----
'''
        rows = [row for row in debian.controls(text) if 'Source' in row]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['Version'], '7:9.0.2-6')
        self.assertEqual(debian.checksum_rows(rows[0]), {
            'ffmpeg_9.0.2.orig.tar.xz': {'sha256': 'abcdef', 'size': 42},
            'ffmpeg_9.0.2-6.debian.tar.xz': {'sha256': '012345', 'size': 24}})

    def test_multiple_source_versions_remain_distinct(self):
        rows = debian.controls('Package: mesa\nVersion: 26.1.6-1\n\n'
                               'Package: mesa\nVersion: 26.2.6-1\n')
        self.assertEqual([row['Version'] for row in rows], ['26.1.6-1', '26.2.6-1'])


if __name__ == '__main__':
    unittest.main()
