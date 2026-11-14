#!/usr/bin/env python3
"""Check signed-source metadata parsing without network or a device."""
from pathlib import Path
import sys
import json
import tempfile
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

    def test_resume_rejects_changed_target_or_patch(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            target = {'suite': 'sid', 'architecture': 'arm64', 'sources': {'ffmpeg': '7:9.0.2-6'},
                      'binaries': {'libc6': {'version': '2.43-7'}}}
            patches = [{'path': 'fix.patch', 'sha256': 'abc'}]
            record = {'target': target, 'patches': patches, 'package_version': '7:9.0.2-6+sunuefi4'}
            (output / 'SOURCE.json').write_text(json.dumps(record))
            debian.validate_resume(output, target, patches, record['package_version'])
            changed = {**target, 'architecture': 'amd64'}
            with self.assertRaisesRegex(ValueError, 'target ABI'):
                debian.validate_resume(output, changed, patches, record['package_version'])
            with self.assertRaisesRegex(ValueError, 'package version and patches'):
                debian.validate_resume(output, target, [], record['package_version'])

    def test_unchanged_quilt_series_keeps_its_timestamp(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            (source / 'debian/patches').mkdir(parents=True)
            series = source / 'debian/patches/series'
            series.write_text('upstream.patch\n')
            (source / 'debian/changelog').write_text('mesa (26.2.6-1+piano1) sid; urgency=medium\n')
            local = source / 'fix.patch'
            local.write_text('patch content\n')
            debian.add_patches(source, [local], 'mesa', '26.2.6-1+piano1', 'Recognise the GPU.')
            timestamp = series.stat().st_mtime_ns
            debian.add_patches(source, [local], 'mesa', '26.2.6-1+piano1', 'Recognise the GPU.')
            self.assertEqual(series.stat().st_mtime_ns, timestamp)


if __name__ == '__main__':
    unittest.main()
