import contextlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

from package import droidloom


class DroidloomPackageTests(unittest.TestCase):
    def setUp(self):
        self.workspace = tempfile.TemporaryDirectory()
        self.addCleanup(self.workspace.cleanup)
        self.root = Path(self.workspace.name)
        self.policy = droidloom.read_policy()

    def test_unrecognized_source_is_rejected_before_extraction(self):
        archive = self.root / 'source.tar.gz'
        archive.write_bytes(b'not the pinned upstream source')
        output = self.root / 'source'
        with self.assertRaisesRegex(ValueError, 'pinned SHA-256'):
            droidloom.take_source(archive, output, self.policy)
        self.assertFalse(output.exists())

    def test_placeholders_cannot_become_android_image_packages(self):
        name = droidloom.PARTITION_DIR + '/vendor.raw.img'
        magic = bytearray(4096)
        magic[1080:1082] = b'\x53\xef'
        for contents, length in ((magic, len(magic)), (bytes(4096), 1024 ** 3)):
            with self.subTest(length=length):
                with self.assertRaisesRegex(ValueError, 'real Android partition'):
                    droidloom.check_content(name, contents, length, 'image')

    def test_stage_keeps_guest_storage_separate_from_the_host(self):
        allowed_image = droidloom.allowed_paths('image', 'debian')
        allowed_base = droidloom.allowed_paths('base', 'debian')
        vendor = droidloom.PARTITION_DIR + '/vendor.raw.img'
        system = droidloom.PARTITION_DIR + '/system.img'
        self.assertTrue(droidloom.check_path(vendor, allowed_image, False))
        self.assertTrue(droidloom.check_path(system, allowed_base, False))
        cases = ((vendor, 'base'), (system, 'image'), ('etc/droidloom/cell.json', 'image'),
                 ('var/lib/droidloom/users', 'base'), (droidloom.CURRENT + '/hidden', 'image'))
        for index, (name, component) in enumerate(cases):
            source = self.root / str(index)
            path = source / name
            path.parent.mkdir(parents=True)
            path.touch()
            with self.subTest(name=name), self.assertRaises(ValueError):
                droidloom.stage_tree(source, self.root / 'out', component, False)

    @unittest.skipUnless(shutil.which('dpkg-deb'), 'dpkg-deb is required')
    def test_native_debian_metadata_rejects_wrong_image_generation(self):
        recipe = self.root / 'package'
        (recipe / 'DEBIAN').mkdir(parents=True)
        package_version = droidloom.version(self.policy, 'debian')
        control = recipe / 'DEBIAN/control'
        archive = self.root / 'runtime.deb'
        for expected in (True, False):
            dependency = package_version if expected else '0.0.1-1'
            control.write_text(
                f'Package: droidloom-runtime\nVersion: {package_version}\nArchitecture: arm64\n'
                'Maintainer: Package Test <test@example.org>\nDescription: Package metadata test\n'
                f'Depends: libc6, droidloom-image (= {dependency})\n')
            subprocess.run(['dpkg-deb', '--root-owner-group', '--build', recipe, archive],
                           check=True, stdout=subprocess.DEVNULL)
            if expected:
                droidloom.package_metadata(archive, 'runtime', 'debian', package_version)
            else:
                with self.assertRaisesRegex(ValueError, 'matching generation'):
                    droidloom.package_metadata(archive, 'runtime', 'debian', package_version)

    def test_archive_rejects_personal_directory_sharing(self):
        path = droidloom.SHARE + '/cell-template.json'
        for template in ({'share_user_folders': True, 'shared_storage_directories': []},
                         {'share_user_folders': False, 'shared_storage_directories': ['/home/test']}):
            data = json.dumps(template).encode()
            raw = io.BytesIO()
            with tarfile.open(fileobj=raw, mode='w') as archive:
                member = tarfile.TarInfo(path)
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))
            raw.seek(0)
            with tarfile.open(fileobj=raw, mode='r') as archive:
                with patch.object(droidloom, 'archive_members',
                                  return_value=contextlib.nullcontext(archive)):
                    with self.assertRaisesRegex(ValueError, 'not share personal folders'):
                        droidloom.archive_payload(self.root / 'fixture.deb', 'runtime', 'debian')


if __name__ == '__main__':
    unittest.main()
