"""Real GNU tar round trips for guest ownership/links/xattrs and root policy."""
from pathlib import Path
import os
import subprocess
import struct
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import package_piano_ram_root as pack


class RamRootArchiveTests(unittest.TestCase):
    def fixture(self, root):
        (root / 'usr/lib/modules/fixture').mkdir(parents=True)
        (root / 'usr/lib/modules/fixture/driver.ko').write_bytes(b'runtime module')
        (root / 'usr/lib/modules/fixture/build').symlink_to('/host/source')
        (root / 'usr/lib/modules/fixture/source').symlink_to('/host/source')
        (root / 'usr/bin').mkdir()
        data = root / 'usr/bin/fixture'
        data.write_bytes(b'guest executable'); data.chmod(0o6755)
        os.setxattr(data, 'user.piano-test', b'keep attributes')
        os.link(data, root / 'usr/bin/hardlink')
        (root / 'bin').symlink_to('usr/bin')
        (root / 'absolute').symlink_to('/usr/bin/fixture')
        (root / 'etc').mkdir(); (root / 'etc/config').write_bytes(b'configuration')
        acl = struct.pack('<I', 2) + b''.join(struct.pack('<HHI', tag, perm, uid) for tag, perm, uid in
            ((1, 7, 0xffffffff), (2, 5, 1234), (4, 5, 0xffffffff), (16, 5, 0xffffffff), (32, 0, 0xffffffff)))
        os.setxattr(root / 'etc', 'system.posix_acl_access', acl)
        (root / 'var/cache/apt/archives').mkdir(parents=True)
        (root / 'var/cache/apt/archives/cached.deb').write_bytes(b'disposable')
        (root / 'dev').mkdir(); (root / 'dev/host-file').write_bytes(b'not guest data')
        (root / 'host-rootfs').mkdir(); (root / 'host-rootfs/secret').write_bytes(b'not payload')
        return data

    def test_real_archive_preserves_modes_links_and_xattrs_without_host_contents(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='ram-tar-test-') as temporary:
            base = Path(temporary); root = base / 'root'; root.mkdir()
            self.fixture(root)
            before = pack.snapshot(root); archive = base / 'root.tar.gz'
            pack.write_archive(root, archive)
            verified = pack.verify_archive(archive, before)
            self.assertEqual(verified['all_members_checked'], before['entries'])
            target = base / 'extract'; target.mkdir()
            subprocess.run(['tar', '--extract', '--gzip', '--same-permissions', '--xattrs', '--xattrs-include=*', '--acls',
                            '--file', str(archive), '--directory', str(target)], check=True)
            self.assertEqual((target / 'usr/bin/fixture').stat().st_mode & 0o7777, 0o6755)
            self.assertEqual(os.getxattr(target / 'usr/bin/fixture', 'user.piano-test'), b'keep attributes')
            self.assertEqual(os.getxattr(target / 'etc', 'system.posix_acl_access'), os.getxattr(root / 'etc', 'system.posix_acl_access'))
            self.assertEqual((target / 'usr/bin/fixture').stat().st_ino, (target / 'usr/bin/hardlink').stat().st_ino)
            self.assertEqual(os.readlink(target / 'absolute'), '/usr/bin/fixture')
            self.assertEqual(os.readlink(target / 'bin'), 'usr/bin')
            self.assertTrue((target / 'usr/lib/modules/fixture/driver.ko').is_file())
            for name in ('usr/lib/modules/fixture/build', 'usr/lib/modules/fixture/source',
                         'var/cache/apt/archives/cached.deb', 'host-rootfs', 'dev/host-file'):
                self.assertFalse((target / name).exists(), name)
            self.assertTrue((target / 'dev').is_dir())
            self.assertEqual(pack.snapshot(root), before)
            with tarfile.open(archive) as stream:
                file = stream.getmember('./usr/bin/fixture')
                self.assertEqual(file.uid, os.getuid()); self.assertEqual(file.gid, os.getgid())
                self.assertEqual(file.mtime, 0)
                self.assertIn('SCHILY.xattr.user.piano-test', file.pax_headers)

    def test_real_archive_is_reproducible_and_tree_change_is_visible(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='ram-tar-repeat-') as temporary:
            base = Path(temporary); root = base / 'root'; root.mkdir()
            data = self.fixture(root)
            before = pack.snapshot(root)
            first, second = base / 'first.gz', base / 'second.gz'
            pack.write_archive(root, first)
            os.utime(data, (1, 42))
            pack.write_archive(root, second)
            self.assertEqual(pack.sha_file(first), pack.sha_file(second))
            self.assertEqual(pack.snapshot(root), before)
            data.write_bytes(b'changed content')
            self.assertNotEqual(pack.snapshot(root)['tree_sha256'], before['tree_sha256'])

    def test_no_special_nodes_or_imported_base_packaging(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='ram-tar-policy-') as temporary:
            root = Path(temporary) / 'root'; root.mkdir(); os.mkfifo(root / 'pipe')
            with self.assertRaisesRegex(ValueError, 'special node'):
                pack.snapshot(root)
            (root / 'pipe').unlink(); (root / 'opt').mkdir(); (root / 'opt/application').write_bytes(b'keep this feature')
            with self.assertRaisesRegex(ValueError, 'contains guest data'):
                pack.snapshot(root)
            with self.assertRaisesRegex(ValueError, 'derived workspace'):
                pack.package(ROOT / 'artifacts/distros/debian13-arm64-v1/rootfs', ROOT / 'artifacts/test-forbidden')


if __name__ == '__main__':
    unittest.main()
