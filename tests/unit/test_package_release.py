import gzip
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import package_release as pr
from make_kernel_initramfs import make_newc


class ReleasePackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.root = self.base / 'rootfs'
        (self.root / 'lib/modules/6.6-fixture').mkdir(parents=True)
        (self.root / 'etc').mkdir()
        (self.root / 'etc/machine-id').touch()
        (self.root / 'etc/os-release').write_text('FIXTURE ONLY\n')
        for name in ('dev', 'proc', 'sys', 'run'):
            (self.root / name).mkdir()
        self.kernel = self.base / 'kernel'
        self.kernel.mkdir()
        data = bytearray(256)
        data[:2] = b'MZ'
        data[56:60] = b'ARM\x64'
        struct.pack_into('<I', data, 60, 128)
        data[128:132] = b'PE\0\0'
        struct.pack_into('<H', data, 132, 0xaa64)
        struct.pack_into('<H', data, 152, 0x20b)
        struct.pack_into('<H', data, 220, 10)
        (self.kernel / 'Image').write_bytes(data)
        self.uefi = self.base / 'uefi/PianoUEFI-product.img'
        self.uefi.parent.mkdir()
        self.uefi.write_bytes(b'ANDROID!' + b'FIXTURE ONLY')
        self.dtb = self.base / 'dtb/board.dtb'
        self.dtb.parent.mkdir()
        self.dtb.write_bytes(struct.pack('>10I', 0xd00dfeed, 72, 56, 72, 40, 17, 16, 0, 0, 16)
                            + b'\0' * 16 + struct.pack('>I4sII', 1, b'\0' * 4, 2, 9))
        self.initrd = self.base / 'initrd/initramfs.cpio.gz'
        self.initrd.parent.mkdir()
        self.initrd.write_bytes(gzip.compress(make_newc([
            {'name': 'init', 'mode': stat.S_IFREG | 0o755, 'data': b'#!/bin/sh\n'},
            {'name': 'etc/piano/kernel-release', 'mode': stat.S_IFREG | 0o644, 'data': b'6.6-fixture\n'}]), mtime=0))
        self.write_manifest(self.uefi.parent, {'files': {self.uefi.name: {'sha256': pr.sha(self.uefi)}}})
        self.write_manifest(self.kernel, {'kernel_release': '6.6-fixture', 'root_policy': 'LABEL=PIANOROOT',
                                         'source_clean': True, 'image': {'sha256': pr.sha(self.kernel / 'Image')}})
        self.write_manifest(self.dtb.parent, {'output': 'board.dtb', 'output_sha256': pr.sha(self.dtb)})
        self.write_manifest(self.initrd.parent, {'kernel_release': '6.6-fixture', 'initramfs_sha256': pr.sha(self.initrd)})
        self.args = pr.parser().parse_args(['--uefi', str(self.uefi), '--kernel', str(self.kernel), '--dtb', str(self.dtb),
                                          '--initramfs', str(self.initrd), '--rootfs', str(self.root), '--output', str(self.base / 'bundle')])

    def tearDown(self):
        self.temp.cleanup()

    def write_manifest(self, folder, data):
        (folder / 'manifest.json').write_text(json.dumps(data))

    def test_component_hash_cannot_override_manifest(self):
        with self.assertRaisesRegex(ValueError, 'disagrees'):
            pr.component(self.uefi, pr.record(self.uefi.parent / 'manifest.json'), '0' * 64, 'uefi')
        self.uefi.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'mismatch'):
            pr.package(self.args)
        self.assertFalse(self.args.output.exists())

    def test_release_and_root_policy_must_match(self):
        for change, reason in (({'kernel_release': 'other'}, 'release mismatch'),
                               ({'root_policy': 'PARTUUID=old-test-machine'}, 'selector mismatch'),
                               ({'command_line': 'piano.root=ram'}, 'another root'),
                               ({'source_clean': False}, 'clean build')):
            with self.subTest(change=change):
                meta = {'kernel_release': '6.6-fixture', 'root_policy': 'LABEL=PIANOROOT', 'image': {'sha256': pr.sha(self.kernel / 'Image')}}
                meta.update(change)
                self.write_manifest(self.kernel, meta)
                with self.assertRaisesRegex(ValueError, reason):
                    pr.package(self.args)

    def test_nonempty_identity_detected_without_reading_credentials(self):
        for name in ('etc/machine-id', 'root/.ssh/custom-key', 'etc/ssh/ssh_host_ed25519_key'):
            with self.subTest(name=name):
                p = self.root / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b'NOT A CREDENTIAL; FIXTURE')
                with patch.object(Path, 'read_bytes', side_effect=AssertionError('Do not read credentials')):
                    with self.assertRaisesRegex(ValueError, 'identity/SSH'):
                        pr.scan_root(self.root, '6.6-fixture')
                p.unlink()

    def test_guest_absolute_links_and_empty_machine_id_alias(self):
        (self.root / 'var/lib/dbus').mkdir(parents=True)
        (self.root / 'var/lib/dbus/machine-id').symlink_to('/etc/machine-id')
        (self.root / 'bin').symlink_to('/usr/bin')
        (self.root / 'usr/bin').mkdir(parents=True)
        pr.scan_root(self.root, '6.6-fixture')

    def test_usr_module_fallback_resolves_inside_guest(self):
        (self.root / 'opt').mkdir()
        shutil.move(self.root / 'lib', self.root / 'opt/lib')
        (self.root / 'usr').symlink_to('/opt')
        pr.scan_root(self.root, '6.6-fixture')

    def test_runtime_contents_escape_and_host_hardlinks_refused(self):
        runtime = self.root / 'proc/version'
        runtime.write_text('FIXTURE')
        with self.assertRaisesRegex(ValueError, 'Runtime tree'):
            pr.scan_root(self.root, '6.6-fixture')
        runtime.unlink()
        link = self.root / 'etc/escape'
        link.symlink_to('../../outside')
        with self.assertRaisesRegex(ValueError, 'escapes rootfs'):
            pr.scan_root(self.root, '6.6-fixture')
        link.unlink()
        outside = self.base / 'outside'
        outside.write_text('FIXTURE')
        link.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, 'host/dangling'):
            pr.scan_root(self.root, '6.6-fixture')
        link.unlink()
        os.link(outside, link)
        with self.assertRaisesRegex(ValueError, 'Hardlink outside'):
            pr.scan_root(self.root, '6.6-fixture')

    def test_mixed_kernel_modules_refused(self):
        (self.root / 'lib/modules/old').mkdir()
        with self.assertRaisesRegex(ValueError, 'modules/release'):
            pr.scan_root(self.root, '6.6-fixture')

    def test_missing_tools_fail_before_creating_output(self):
        with patch.object(pr.shutil, 'which', return_value=None):
            with self.assertRaisesRegex(ValueError, 'Missing host tools'):
                pr.package(self.args)
        self.assertFalse(self.args.output.exists())

    def test_cross_manifest_kernel_binding_refused(self):
        self.write_manifest(self.initrd.parent, {'kernel_release': '6.6-fixture', 'initramfs_sha256': pr.sha(self.initrd),
                                               'kernel_manifest_sha256': '0' * 64})
        with self.assertRaisesRegex(ValueError, 'another kernel manifest'):
            pr.package(self.args)
        self.assertFalse(self.args.output.exists())

    def test_external_failure_never_marks_package_complete(self):
        with patch.object(pr.shutil, 'which', return_value='/fixture/tool'), \
             patch.object(pr, 'pinned_mkbootimg', return_value=(Path('/fixture/mkbootimg.py'), {})), \
             patch.object(pr, 'run', side_effect=subprocess.CalledProcessError(1, ['fixture-tool'])):
            with self.assertRaises(subprocess.CalledProcessError):
                pr.package(self.args)
        self.assertTrue((self.args.output / '.incomplete').is_file())
        self.assertFalse((self.args.output / 'manifest.json').exists())
        self.assertFalse((self.args.output / 'SHA256SUMS').exists())

    def test_boot_parser_rejects_wrong_sizes_trailing_and_changed_bytes(self):
        data = bytearray(4096)
        data[:8] = b'ANDROID!'
        struct.pack_into('<9I', data, 8, (self.kernel / 'Image').stat().st_size, 0, self.initrd.stat().st_size, 0, 0, 0, 0, 4096, 2)
        struct.pack_into('<II', data, 1644, 1660, self.dtb.stat().st_size)
        for p in (self.kernel / 'Image', self.initrd, self.dtb):
            blob = p.read_bytes()
            data.extend(blob + b'\0' * (-len(blob) % 4096))
        boot = self.base / 'parser-fixture.img'
        boot.write_bytes(data)
        pr.verify_boot(boot, (self.kernel / 'Image', self.initrd, self.dtb))
        for offset, value, reason in ((4096, 123, 'component mismatch'), (40, 3, 'Expected Android'), (64, 1, 'command line')):
            changed = bytearray(data)
            changed[offset] = value
            boot.write_bytes(changed)
            with self.assertRaisesRegex(ValueError, reason):
                pr.verify_boot(boot, (self.kernel / 'Image', self.initrd, self.dtb))
        boot.write_bytes(data + b'extra')
        with self.assertRaisesRegex(ValueError, 'trailing'):
            pr.verify_boot(boot, (self.kernel / 'Image', self.initrd, self.dtb))

    @unittest.skipUnless(shutil.which('tar') and shutil.which('zstd'), 'tar/zstd unavailable')
    def test_real_archive_preserves_numeric_owner_mode_and_link(self):
        source = self.root / 'etc/os-release'
        source.chmod(0o640)
        (self.root / 'etc/release-link').symlink_to('os-release')
        target = self.base / 'root.tar.zst'
        pr.archive_root(self.root, target, 100)
        import tarfile
        tar = subprocess.check_output(['zstd', '-q', '-d', '-c', target])
        import io
        with tarfile.open(fileobj=io.BytesIO(tar)) as archive:
            member = archive.getmember('./etc/os-release')
            self.assertEqual((member.uid, member.gid, member.mode, member.mtime), (source.stat().st_uid, source.stat().st_gid, 0o640, 100))
            self.assertEqual(archive.getmember('./etc/release-link').linkname, 'os-release')

    def test_real_pinned_mkbootimg_wrapper(self):
        try:
            script, origin = pr.pinned_mkbootimg()
        except (ValueError, OSError, subprocess.SubprocessError) as error:
            self.skipTest('Pinned local mkbootimg unavailable: ' + str(error))
        target = self.base / 'real-wrapper.img'
        pr.run([sys.executable, script, '--header_version', '2', '--pagesize', '4096', '--kernel', self.kernel / 'Image',
                '--ramdisk', self.initrd, '--dtb', self.dtb, '-o', target], env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'})
        pr.verify_boot(target, (self.kernel / 'Image', self.initrd, self.dtb))
        self.assertRegex(origin['commit'], '^[0-9a-f]{40}$')

    @unittest.skipUnless(shutil.which('mke2fs') and shutil.which('debugfs'), 'ext4 tools unavailable')
    def test_real_ext4_owner_modes_epoch_and_xattr(self):
        path = self.root / 'etc/os-release'
        path.chmod(0o640)
        try:
            os.setxattr(path, 'user.fixture', b'fixture metadata')
            acl = struct.pack('<I', 2) + b''.join(struct.pack('<HHI', tag, perm, uid) for tag, perm, uid in
                  ((1, 6, 0xffffffff), (2, 4, 1001), (4, 0, 0xffffffff), (16, 4, 0xffffffff), (32, 0, 0xffffffff)))
            os.setxattr(path, 'system.posix_acl_access', acl)
        except OSError:
            pass
        self.root.chmod(0o700)
        work = self.base / 'work'
        work.mkdir()
        target = self.base / 'root.ext4.img'
        pr.ext4(self.root, target, 64, 'PIANOROOT', 100, pr.scan_root(self.root, '6.6-fixture'), work)
        self.assertEqual(target.stat().st_size, 64 * pr.MIB)
        superblock = pr.run(['debugfs', '-R', 'stats', target]).stdout
        self.assertIn('PIANOROOT', superblock)


if __name__ == '__main__':
    unittest.main()
