"""Check the native PipeWire split and its library dependencies."""
import io
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from desktop import build_libcamera_packages, build_pipewire_packages as builder, libcamera_arch, pipewire_arch


@unittest.skipUnless(shutil.which('bsdtar'), 'Package metadata inspection needs bsdtar')
class ArchPipeWirePackagesTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.folder = Path(temporary.name)
        self.runtime = self.folder / 'runtime'
        self.runtime.mkdir()
        self.archives = {}

    def archive(self, name, version, *, depends=()):
        filename = f'{name}-{version}-aarch64.pkg.tar.gz'
        records = [f'pkgname = {name}', f'pkgver = {version}', 'arch = aarch64']
        records.extend(f'depend = {value}' for value in depends)
        payload = ('\n'.join(records) + '\n').encode()
        with tarfile.open(self.runtime / filename, 'w:gz') as archive:
            entry = tarfile.TarInfo('.PKGINFO')
            entry.size = len(payload)
            archive.addfile(entry, io.BytesIO(payload))
        digest = builder.package_build.sha(self.runtime / filename)
        self.archives[filename] = {'sha256': digest}

    def test_same_version_spa_and_audio_abi(self):
        arch = pipewire_arch.policy()
        version = arch['upstream_package_version'].rsplit('-', 1)[0] + '-' + arch['package_release']
        audio = arch['spandsp']['soname']
        camera_abi = build_libcamera_packages.abi_version(build_libcamera_packages.policy())
        sonames = libcamera_arch.sonames(camera_abi)
        expected = pipewire_arch.REQUIRED | {'pipewire-audio'}
        for name in expected - {'pipewire-libcamera', 'pipewire-audio'}:
            self.archive(name, version)
        self.archive('pipewire-audio', version, depends=[audio])
        self.archive('pipewire-libcamera', version, depends=[f'pipewire={version}', *sonames])
        self.assertEqual(set(pipewire_arch.verify_packages(self.runtime, self.archives, expected,
                                                         version, audio, camera_abi)), expected)

        self.archive('pipewire-libcamera', version, depends=[f'pipewire={version}'])
        with self.assertRaisesRegex(ValueError, 'matching PipeWire and libcamera ABI'):
            pipewire_arch.verify_packages(self.runtime, self.archives, expected, version, audio, camera_abi)

        self.archive('pipewire-libcamera', version,
                     depends=[f'pipewire={arch["upstream_package_version"]}', *sonames])
        with self.assertRaisesRegex(ValueError, 'requires a different PipeWire version'):
            pipewire_arch.verify_packages(self.runtime, self.archives, expected, version, audio, camera_abi)


class ArchPipeWirePolicyTests(unittest.TestCase):
    def test_output_cannot_overwrite_libcamera_input(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            library = root / 'libcamera'
            library.mkdir()
            receipt = library / 'SOURCE.json'
            receipt.write_text('input receipt')
            for output in (root, library, library / 'pipewire'):
                with self.assertRaisesRegex(ValueError, 'separate outputs'):
                    pipewire_arch.build(root / 'chroot', output, 1, library, {}, {}, [])
            self.assertEqual(receipt.read_text(), 'input receipt')

    def test_arch_recipe_reuses_the_versioned_patch_lists(self):
        arch = pipewire_arch.policy()
        _, base = builder.arch_policy(arch['upstream_package_version'])
        selected = pipewire_arch.patch_records(base, arch['libcamera_patches'])
        self.assertEqual(selected[:len(base)], base)
        self.assertEqual([Path(row['path']).name for row in selected[len(base):]],
                         arch['libcamera_patches']['files'])
        self.assertEqual(len({Path(row['path']).resolve() for row in selected}), len(selected))

    def test_compatible_jack_split_is_preserved(self):
        jack = {'pipewire-jack', 'pipewire-jack-client'}
        splits = pipewire_arch.REQUIRED | jack | {'pipewire-docs', 'pipewire-pulse'}
        for installed_jack in jack:
            installed = {installed_jack, 'pipewire', 'pipewire-pulse'}
            self.assertEqual(set(pipewire_arch.install_names(splits, installed)),
                             pipewire_arch.REQUIRED | installed)


if __name__ == '__main__':
    unittest.main()
