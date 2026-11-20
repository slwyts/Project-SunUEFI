"""Check the native FFmpeg source and package cache selection."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import build_arch_ffmpeg as ffmpeg


class ArchFFmpegTests(unittest.TestCase):
    def test_source_must_match_the_installed_package(self):
        for package, upstream in (('2:8.1.2-7', '8.1.2'), ('2:9.0.2-5', '9.0.2')):
            with self.subTest(package=package):
                _, series = ffmpeg.source_series(package)
                self.assertEqual(series['upstream_version'], upstream)
        with self.assertRaisesRegex(ValueError, 'needs source 8.1.3'):
            ffmpeg.source_series('2:8.1.3-1')

    def test_cache_uses_the_target_package_dependencies_and_configuration(self):
        package = {
            'version': '2:8.1.2-7',
            'depends': ['libx265.so=216-64'],
            'provides': ['libavcodec.so=62-64'],
        }
        configuration = ['--enable-libx265', '--enable-shared']
        series_path, series = ffmpeg.source_series(package['version'])
        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(ffmpeg.overlay_release, 'generation', return_value=None), \
                patch.object(ffmpeg, 'installed_package', return_value=package) as installed, \
                patch.object(ffmpeg, 'installed_options', return_value=configuration) as options:
            output = Path(temporary)
            archive = output / 'ffmpeg-2:8.1.2-7.1-aarch64.pkg.tar.zst'
            archive.write_bytes(b'unchanged package')
            (output / 'manifest.json').write_text('{}')
            record = {
                'upstream_version': series['upstream_version'],
                'local_revision': series['local_revision'],
                'archive_sha256': series['source']['sha256'],
                'patches': [
                    {'path': str((series_path.parent / row['path']).resolve().relative_to(ffmpeg.ROOT)),
                     'sha256': row['sha256']} for row in series['series']
                ],
                'package': archive.name,
                'package_sha256': ffmpeg.source.sha(archive),
                'target_package': package,
                'package_version': '2:8.1.2-7.1',
                'original_configuration': configuration,
            }
            (output / 'SOURCE.json').write_text(json.dumps(record))
            self.assertTrue(ffmpeg.is_current(output, output))
            installed.return_value = {**package, 'version': record['package_version']}
            self.assertTrue(ffmpeg.is_current(output, output))
            installed.return_value = {**package, 'depends': ['libx265.so=215-64']}
            self.assertFalse(ffmpeg.is_current(output, output))
            installed.return_value = {**package, 'version': '2:8.1.2-8'}
            self.assertFalse(ffmpeg.is_current(output, output))
            installed.return_value = package
            options.return_value = ['--enable-shared']
            self.assertFalse(ffmpeg.is_current(output, output))


if __name__ == '__main__':
    unittest.main()
