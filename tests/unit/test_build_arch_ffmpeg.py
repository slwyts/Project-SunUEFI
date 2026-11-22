"""Check the native FFmpeg source and package cache selection."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import build_arch_ffmpeg as ffmpeg


class ArchFFmpegTests(unittest.TestCase):
    def test_source_must_match_the_installed_package(self):
        for package, upstream in (('2:8.1.2-7', '8.1.2'),
                                  ('2:9.0.2-5', '9.0.2'),
                                  ('2:9.1-1', '9.1')):
            with self.subTest(package=package):
                _, series = ffmpeg.source_series(package)
                self.assertEqual(series['upstream_version'], upstream)
        with self.assertRaisesRegex(ValueError, 'needs source 8.1.3'):
            ffmpeg.source_series('2:8.1.3-1')
        with self.assertRaisesRegex(ValueError, 'Port the BSP fixes'):
            ffmpeg.source_series('2:9.99-1')

    def test_ffmpeg_9_1_keeps_arch_chromium_symbol_and_reuses_codec_patches(self):
        def patches(package):
            series_path, series = ffmpeg.source_series(package)
            return {Path(row['path']).name: (series_path.parent / row['path']).resolve()
                    for row in series['series']}

        steam = patches('2:8.1.2-7')
        debian = patches('7:9.0.2-6')
        arch = patches('2:9.1-1')
        symbol_patch = '0001-avformat-first-dts-for-chromium.patch'
        buffer_backport = '0000-v4l2m2m-count-buffer-once.patch'
        self.assertEqual(arch.pop(symbol_patch), steam[symbol_patch])
        self.assertEqual(debian.pop(buffer_backport), steam[buffer_backport])
        self.assertEqual(arch, debian)

    def test_recipe_preserves_qt_faststart_when_shipped(self):
        for shipped in (True, False):
            with self.subTest(shipped=shipped):
                recipe = ffmpeg.package_recipe('9.1', '2', '1.1', ['--enable-shared'], [],
                                               ['libavformat.so=63-64'], qt_faststart=shipped)
                self.assertEqual('make tools/qt-faststart\n' in recipe, shipped)
                self.assertEqual('$pkgdir/usr/bin/qt-faststart' in recipe, shipped)

    def test_cache_uses_the_target_package_dependencies_and_configuration(self):
        package = {
            'version': '2:8.1.2-7',
            'depends': ['libx265.so=216-64'],
            'provides': ['libavcodec.so=62-64'],
            'qt_faststart': True,
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
            installed.return_value = {**package, 'qt_faststart': False}
            self.assertFalse(ffmpeg.is_current(output, output))
            installed.return_value = package
            options.return_value = ['--enable-shared']
            self.assertFalse(ffmpeg.is_current(output, output))


if __name__ == '__main__':
    unittest.main()
