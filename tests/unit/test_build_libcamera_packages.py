"""Check native libcamera package versions and matching runtime dependencies."""
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from desktop import build_libcamera_packages as builder
from desktop import libcamera_arch


class LibcameraPackageTests(unittest.TestCase):
    def test_new_abi_is_one_native_package_cohort(self):
        data = builder.policy()
        abi = builder.abi_version(data)
        version = builder.base_package_version(data)
        ipa = f'libcamera-ipa-{abi}'
        packages = {name: {'architecture': 'arm64', 'version': version}
                    for name in builder.runtime_packages(data)}
        manifest = {'package_version': version, 'packages': packages}
        self.assertIn(ipa, packages)
        builder.verify_debian_packages(manifest)
        with self.assertRaisesRegex(ValueError, 'same version'):
            wrong = {key: value for key, value in packages.items() if key != ipa}
            builder.verify_debian_packages({**manifest, 'packages': {
                **wrong, 'libcamera-ipa': packages[ipa],
            }})
        with self.assertRaisesRegex(ValueError, 'same version'):
            builder.verify_debian_packages({
                **manifest, 'packages': {**packages, 'libcamera-dev': {
                    'architecture': 'arm64', 'version': data['debian']['recipe_source_version'],
                }},
            })

    def test_ipa_package_migrates_install_and_signing_paths_together(self):
        abi = builder.abi_version(builder.policy())
        name = f'libcamera-ipa-{abi}'
        control = (f'Package: libcamera{abi}\n'
                   'Recommends: libcamera-ipa (= ${binary:Version})\n\n'
                   'Package: libcamera-ipa\n'
                   'Depends: ${misc:Depends},\n'
                   '         ${shlibs:Depends}\n')
        with tempfile.TemporaryDirectory() as directory:
            packaging = Path(directory)
            old_install = packaging / 'libcamera-ipa.install'
            old_install.write_text('usr/lib/*/libcamera/ipa\n'
                                   'usr/libexec/*/libcamera/*_ipa_proxy\n'
                                   'usr/share/libcamera\n')
            rules = packaging / 'rules'
            old_path = 'debian/libcamera-ipa/usr/lib/*/libcamera/ipa/ipa_*.so'
            rules.write_text(f'\tsign {old_path}\n\tverify {old_path}\n')

            updated = builder.prepare_ipa_package(packaging, control, abi)

            self.assertIn(f'Recommends: {name} (= ${{binary:Version}})\n', updated)
            self.assertIn(f'Package: {name}\nDepends: libcamera{abi} (= ${{binary:Version}}),\n', updated)
            self.assertFalse(old_install.exists())
            self.assertEqual((packaging / (name + '.install')).read_text().splitlines(), [
                f'usr/lib/*/libcamera/{abi}/ipa',
                f'usr/libexec/*/libcamera/{abi}/*_ipa_proxy',
                f'usr/share/libcamera/{abi}',
            ])
            qualifier = f'debian/{name}/usr/lib/*/libcamera/{abi}/ipa/ipa_*.so'
            self.assertEqual(rules.read_text(), f'\tsign {qualifier}\n\tverify {qualifier}\n')

    @unittest.skipUnless(shutil.which('dpkg'), 'Debian version comparison needs dpkg')
    def test_local_revision_sorts_before_the_first_official_release(self):
        data = builder.policy()
        upstream = builder.upstream_version(data)
        version = builder.base_package_version(data)
        for old, new in ((data['debian']['recipe_source_version'], version), (version, upstream + '-1')):
            subprocess.run(['dpkg', '--compare-versions', old, 'lt', new], check=True)

    def test_pacman_target_is_selected_from_the_build_root(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            release = root / 'etc/os-release'
            release.parent.mkdir()
            for text, target in (('ID=archarm\n', 'arch'), ('ID=archlinuxarm\n', 'arch'),
                                 ('ID=steamos\n', 'steamos'),
                                 ('ID="arch"\nSUNUEFI_BASE_CODENAME="holo"\n', 'steamos')):
                with self.subTest(text=text):
                    release.write_text(text)
                    self.assertEqual(libcamera_arch.build_target(root), target)
            release.write_text('ID=arch\n')
            with self.assertRaisesRegex(ValueError, 'Arch Linux ARM or SteamOS'):
                libcamera_arch.build_target(root)

    def test_steamos_tools_require_the_build_qt_minor(self):
        name, version, abi = 'libcamera-tools', '0.8.0-1.1', '0.8'
        common = [f'libcamera={version}', *libcamera_arch.sonames(abi)]
        with self.assertRaisesRegex(ValueError, 'Qt minor version'):
            libcamera_arch.verify_dependencies({'depend': [*common, 'qt6-base']}, name,
                                               version, abi, target='steamos')
        libcamera_arch.verify_dependencies({'depend': [*common, 'qt6-base>=6.11']}, name,
                                           version, abi, target='steamos')

    @unittest.skipUnless(shutil.which('bsdtar'), 'Native Arch package inspection needs bsdtar')
    def test_arch_camera_packages_require_the_same_library_and_ipa(self):
        data = builder.policy()
        abi = builder.abi_version(data)
        version = builder.upstream_version(data) + '-' + data['arch']['package_release']
        sonames = [f'libcamera.so={abi}-64', f'libcamera-base.so={abi}-64']
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            runtime = output / 'runtime'
            runtime.mkdir()
            archives = {}

            def package(name, relations):
                filename = f'{name}-{version}-aarch64.pkg.tar.gz'
                identity = [f'pkgname = {name}', f'pkgver = {version}', 'arch = aarch64']
                payload = ('\n'.join([*identity, *relations]) + '\n').encode()
                with tarfile.open(runtime / filename, 'w:gz') as archive:
                    info = tarfile.TarInfo('.PKGINFO')
                    info.size = len(payload)
                    archive.addfile(info, io.BytesIO(payload))
                archives[filename] = {'sha256': builder.package_build.sha(runtime / filename)}

            package('libcamera', [f'depend = libcamera-ipa={version}',
                                  *[f'provides = {item}' for item in sonames]])
            package('libcamera-docs', [])
            for name in libcamera_arch.PACKAGES - {'libcamera', 'libcamera-docs'}:
                package(name, [f'depend = {item}' for item in [f'libcamera={version}', *sonames]])
            result = libcamera_arch.verify_arch_packages(runtime, archives, version, abi)
            self.assertEqual(set(result), libcamera_arch.PACKAGES)
            build = {'package_format': 'pacman', 'recipe_sha256': '0' * 64}
            (output / 'SOURCE.json').write_text(json.dumps({
                **build, 'packages_built': True, 'package_version': version,
                'source': data['source'], 'arch_recipe': data['arch'], 'packages': result,
            }))
            (runtime / 'manifest.json').write_text(json.dumps({
                **build, 'architecture': 'aarch64', 'packages': archives,
            }))
            inputs = {name: result[name] for name in libcamera_arch.RUNTIME_INPUTS}
            self.assertEqual(libcamera_arch.runtime_inputs(output), (runtime, version, abi, inputs))
            with self.assertRaisesRegex(ValueError, 'Use a completed'):
                libcamera_arch.runtime_inputs(output, target='steamos')

            package('libcamera-ipa', [f'depend = libcamera={version}', f'depend = {sonames[0]}'])
            with self.assertRaisesRegex(ValueError, 'archive differs'):
                libcamera_arch.runtime_inputs(output)

            package('python-libcamera', [f'depend = libcamera={version}', f'depend = {sonames[0]}'])
            package('libcamera-ipa', [
                f'depend = {item}' for item in [f'libcamera={version}', *sonames]
            ])
            with self.assertRaisesRegex(ValueError, 'matching libcamera ABI: python-libcamera'):
                libcamera_arch.verify_arch_packages(runtime, archives, version, abi)


if __name__ == '__main__':
    unittest.main()
