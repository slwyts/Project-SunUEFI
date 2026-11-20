"""Check the native SteamOS package build and its published manifest."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_steamos_rootfs as builder
from build import overlay_packages


class SteamOSBuildRootTests(unittest.TestCase):
    def test_checks_target_before_compiling_any_overlays(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'etc').mkdir()
            (root / 'etc/os-release').write_text('ID=archarm\n')
            with patch.object(builder, 'kde_recipes') as packages, \
                    self.assertRaisesRegex(ValueError, 'Valve pacman build root'):
                builder.build_overlays(root, root / 'out', root / 'sources', {})
            packages.assert_not_called()


class SteamOSKDETests(unittest.TestCase):
    def check_kwindowsystem(self, version, order, relocation):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            library = f'/usr/lib/libKF6WindowSystem.so.{version}'
            path = root / library.lstrip('/')
            path.parent.mkdir(parents=True)
            path.write_bytes(b'\x7fELF')

            def check_output(command, **kwargs):
                if command == ['chroot', root, 'vercmp', version, '6.26.0']:
                    return f'{order}\n'
                self.assertEqual(command, ['chroot', root, 'pacman', '-Qlq', 'kwindowsystem'])
                return library + '\n'

            def run(command, **kwargs):
                if command[2:] == ['pacman', '-Q', 'kwindowsystem']:
                    return subprocess.CompletedProcess(command, 0, f'kwindowsystem {version}-1\n', '')
                self.assertEqual(command[2:], ['env', 'LC_ALL=C', 'ldd', '-r', library])
                return subprocess.CompletedProcess(command, 0, relocation, '')

            with patch.object(builder.subprocess, 'run', side_effect=run), \
                    patch.object(builder.subprocess, 'check_output', side_effect=check_output):
                return builder.kde_repository_package_works(root, 'kwindowsystem')

    def test_keeps_newer_valve_framework_if_it_works(self):
        self.assertTrue(self.check_kwindowsystem('6.27.0', 1, 'libQt6Core.so.6 => /usr/lib/libQt6Core.so.6'))

    def test_rebuilds_broken_framework_from_matching_source(self):
        self.assertFalse(self.check_kwindowsystem('6.26.0', 0, 'undefined symbol: qt_internal'))

    def test_does_not_downgrade_newer_framework_with_broken_relocations(self):
        with self.assertRaisesRegex(ValueError, 'Valve kwindowsystem 6.27.0 failed'):
            self.check_kwindowsystem('6.27.0', 1, 'libQt6Core.so.6 => not found')

    def test_patched_kde_component_still_uses_recipe(self):
        with patch.object(builder.subprocess, 'run') as query:
            self.assertFalse(builder.kde_repository_package_works(Path('/not-used'), 'kwin'))
            query.assert_not_called()


class SteamOSWebRTCTests(unittest.TestCase):
    def test_build_checks_the_abi_and_reuses_the_exact_release(self):
        config = json.loads((ROOT / 'config/steamos.json').read_text())['webrtc_audio_processing']
        name = overlay_packages.STEAMOS_WEBRTC_PACKAGE
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'rootfs'
            output = Path(folder) / 'build'
            recipe = output / (name + '-recipe')
            packages = output / 'packages' / name
            release = '123456789.1'
            pkgrel = '8.2000000000001234567890000000001'
            abseil_version = '20260526.0-2'

            def makepkg(guest, recipe_dir, destination, *, check):
                self.assertEqual((guest, recipe_dir, destination), (root, recipe, packages))
                self.assertTrue(check)
                source = builder.build_arch_package.prepared_recipe(recipe / 'PKGBUILD')
                self.assertIn(f'pkgrel={pkgrel}\n'.encode(), source)
                self.assertIn(f'# Built with abseil-cpp {abseil_version}.'.encode(), source)
                version = abseil_version.rsplit('-', 1)[0]
                self.assertIn(f'depends=(abseil-cpp={version} gcc-libs glibc)'.encode(), source)
                manifest = {'packages': {}}
                destination.mkdir(parents=True, exist_ok=True)
                archive = destination / f'{name}-2.1-{pkgrel}-aarch64.pkg.tar.zst'
                archive.write_bytes(b'package fixture')
                manifest['packages'][archive.name] = {'sha256': builder.digest(archive)}
                manifest['recipe_sha256'] = builder.build_arch_package.recipe_sha256(recipe / 'PKGBUILD')
                (destination / 'manifest.json').write_text(json.dumps(manifest))

            with patch.dict(os.environ, {'SUNUEFI_OVERLAY_RELEASE': release}), \
                    patch.object(builder.subprocess, 'check_output',
                                 return_value=f'abseil-cpp {abseil_version}\n') as abseil, \
                    patch.object(builder.build_arch_package, 'build', side_effect=makepkg) as build, \
                    patch.object(builder.build_arch_package, 'install_cached') as cached:
                first = builder.build_webrtc(root, output, config)
                second = builder.build_webrtc(root, output, config)
                abseil_version = '20260928.0-1'
                abseil.return_value = f'abseil-cpp {abseil_version}\n'
                builder.build_webrtc(root, output, config)
            abseil.assert_called_with(['chroot', root, 'pacman', '-Q', 'abseil-cpp'], text=True)
            self.assertEqual(build.call_count, 2)
            build.assert_called_with(root, recipe, packages, check=True)
            cached.assert_called_once_with(root, packages)
            self.assertEqual(first, second)
            self.assertEqual(first[0], packages)

if __name__ == '__main__':
    unittest.main()
