"""Check the native libcamera package cohort and its Debian upgrade order."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from desktop import build_libcamera_packages as builder


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


if __name__ == '__main__':
    unittest.main()
