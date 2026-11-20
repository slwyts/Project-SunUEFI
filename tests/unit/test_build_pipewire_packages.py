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

    def archive(self, name, version, *, depends=(), conflicts=(), replaces=(), files=()):
        filename = f'{name}-{version}-aarch64.pkg.tar.gz'
        records = [f'pkgname = {name}', f'pkgver = {version}', 'arch = aarch64']
        records.extend(f'depend = {value}' for value in depends)
        records.extend(f'conflict = {value}' for value in conflicts)
        records.extend(f'replaces = {value}' for value in replaces)
        payload = ('\n'.join(records) + '\n').encode()
        with tarfile.open(self.runtime / filename, 'w:gz') as archive:
            entry = tarfile.TarInfo('.PKGINFO')
            entry.size = len(payload)
            archive.addfile(entry, io.BytesIO(payload))
            for file in files:
                archive.addfile(tarfile.TarInfo(file))
        digest = builder.package_build.sha(self.runtime / filename)
        self.archives[filename] = {'sha256': digest}

    def test_same_version_spa_and_audio_abi(self):
        arch = pipewire_arch.policy()
        version = arch['upstream_package_version'].rsplit('-', 1)[0] + '-' + arch['package_release']
        audio = arch['spandsp']['soname']
        camera_abi = build_libcamera_packages.abi_version(build_libcamera_packages.policy())
        sonames = libcamera_arch.sonames(camera_abi)
        expected = pipewire_arch.REQUIRED | {'pipewire-audio'}
        for name in expected - {'pipewire-libcamera', 'pipewire-audio', 'libpipewire'}:
            self.archive(name, version)
        self.archive('libpipewire', version, conflicts=['pipewire-v4l2-source'],
                     replaces=[pipewire_arch.v4l2_replacement(arch)], files=[pipewire_arch.V4L2_MODULE])
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

    def test_native_v4l2_requires_a_pacman_replacement(self):
        version, plugin = '1:1.6.11-2.4', 'pipewire-v4l2-source'
        self.archive('libpipewire', version, conflicts=[plugin])
        with self.assertRaisesRegex(ValueError, 'must replace the standalone plugin'):
            pipewire_arch.verify_packages(self.runtime, self.archives, {'libpipewire'},
                                         version, '', '0.8')
        self.archive('libpipewire', version, conflicts=[plugin],
                     replaces=[pipewire_arch.v4l2_replacement(pipewire_arch.policy())])
        with self.assertRaisesRegex(ValueError, 'must contain the standard V4L2 SPA module'):
            pipewire_arch.verify_packages(self.runtime, self.archives, {'libpipewire'},
                                         version, '', '0.8')

    def test_steamos_keeps_the_valve_v4l2_owner_and_audio_dependencies(self):
        policy = pipewire_arch.policy('steamos')
        audio, webrtc = policy['spandsp']['soname'], policy['webrtc_soname']
        version, abi = '1:1.6.8-5.4', '0.8'
        expected = pipewire_arch.REQUIRED | {'pipewire-audio', 'pipewire-netjack2'}
        plugin = 'pipewire-v4l2-source'
        self.archive('pipewire', version, conflicts=[plugin], replaces=[plugin],
                     files=[pipewire_arch.V4L2_MODULE])
        self.archive('libpipewire', version, files=[pipewire_arch.VALVE_SPA_LIBRARY])
        self.archive('gst-plugin-pipewire', version)
        self.archive('pipewire-netjack2', version)
        self.archive('pipewire-libcamera', version,
                     depends=[f'pipewire={version}', *libcamera_arch.sonames(abi)])

        def verify():
            return pipewire_arch.verify_packages(
                self.runtime, self.archives, expected, version, audio, abi,
                target='steamos', webrtc_soname=webrtc)

        self.archive('pipewire-audio', version, depends=['libspandsp.so=4-64', webrtc])
        with self.assertRaisesRegex(ValueError, 'target spandsp ABI'):
            verify()
        self.archive('pipewire-audio', version, depends=[audio])
        with self.assertRaisesRegex(ValueError, 'target WebRTC ABI'):
            verify()
        self.archive('pipewire-audio', version, depends=[audio, webrtc])
        with self.assertRaisesRegex(ValueError, 'matching netJACK2 modules'):
            verify()
        self.archive('pipewire-audio', version, depends=[audio, webrtc, f'pipewire-netjack2={version}'])
        self.assertEqual(set(verify()), expected)


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
                    pipewire_arch.build(root / 'chroot', output, 1, library)
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

    def test_valve_keeps_netjack_without_installing_the_optional_jack_client(self):
        splits = pipewire_arch.REQUIRED | {'pipewire-netjack2', 'pipewire-jack', 'pipewire-jack-client'}
        self.assertEqual(set(pipewire_arch.install_names(splits, {'pipewire-jack'}, target='steamos')),
                         pipewire_arch.REQUIRED | {'pipewire-netjack2', 'pipewire-jack'})


if __name__ == '__main__':
    unittest.main()
