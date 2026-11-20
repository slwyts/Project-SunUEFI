"""Check the native SteamOS package build and its published manifest."""
import json
import os
from pathlib import Path
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

            def makepkg(guest, recipe_dir, destination, *, check):
                self.assertEqual((guest, recipe_dir, destination), (root, recipe, packages))
                self.assertTrue(check)
                source = builder.build_arch_package.prepared_recipe(recipe / 'PKGBUILD')
                self.assertIn(f'pkgrel={pkgrel}\n'.encode(), source)
                manifest = {'packages': {}}
                destination.mkdir(parents=True)
                archive = destination / f'{name}-2.1-{pkgrel}-aarch64.pkg.tar.zst'
                archive.write_bytes(b'package fixture')
                manifest['packages'][archive.name] = {'sha256': builder.digest(archive)}
                manifest['recipe_sha256'] = builder.build_arch_package.recipe_sha256(recipe / 'PKGBUILD')
                (destination / 'manifest.json').write_text(json.dumps(manifest))

            with patch.dict(os.environ, {'SUNUEFI_OVERLAY_RELEASE': release}), \
                    patch.object(builder.build_arch_package, 'build', side_effect=makepkg) as build, \
                    patch.object(builder.build_arch_package, 'install_cached') as cached:
                first = builder.build_webrtc(root, output, config)
                second = builder.build_webrtc(root, output, config)
            build.assert_called_once_with(root, recipe, packages, check=True)
            cached.assert_called_once_with(root, packages)
            self.assertEqual(first, second)
            self.assertEqual(first[0], packages)

if __name__ == '__main__':
    unittest.main()
