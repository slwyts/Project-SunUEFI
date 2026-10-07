"""Streaming newc and real ARM64 GNU tar dependency-closure verification."""
from pathlib import Path
import io
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_piano_ram_bootstrap as boot
from make_kernel_initramfs import inspect_newc


class RamBootstrapTests(unittest.TestCase):
    def test_real_streamed_payload_has_exact_bytes_alignment_and_permissions(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='bootstrap-newc-') as temporary:
            payload = Path(temporary) / 'payload'; payload.write_bytes(bytes(range(256)) * 513)
            target = io.BytesIO()
            boot.write_newc(target, 'rootfs.tar.gz', stat.S_IFREG | 0o600, 1, source=payload)
            boot.write_newc(target, 'rootfs.sha256', stat.S_IFREG | 0o600, 2, data=b'digest\n')
            boot.write_newc(target, 'TRAILER!!!', 0, 3, data=b'')
            records = inspect_newc(target.getvalue())
            self.assertEqual(records[0]['data'], payload.read_bytes())
            self.assertEqual(records[0]['mode'], stat.S_IFREG | 0o600)
            self.assertEqual(records[1]['data'], b'digest\n')
            with self.assertRaises(ValueError):
                boot.write_newc(target, '../bad', stat.S_IFREG, 4, data=b'')

    def test_guest_absolute_links_are_resolved_inside_guest_and_escape_refused(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='bootstrap-links-') as temporary:
            root = Path(temporary); (root / 'usr/lib').mkdir(parents=True)
            (root / 'usr/lib/library').write_bytes(b'guest library')
            (root / 'lib').symlink_to('usr/lib'); (root / 'usr/lib/absolute').symlink_to('/usr/lib/library')
            self.assertEqual(boot.guest_resolve(root, '/lib/absolute'), root / 'usr/lib/library')
            (root / 'bad').symlink_to('../../outside')
            with self.assertRaisesRegex(ValueError, 'escapes root'):
                boot.guest_resolve(root, '/bad')

    def test_actual_arm_tar_runs_with_only_collected_libraries_and_static_busybox_parses_entry(self):
        distro = ROOT / 'build/distros/debian13-arm64-systemd'
        qemu = ROOT / 'build/distro-tools/usr/bin/qemu-aarch64'
        busybox = ROOT / 'build/linux-ram/busybox'
        if not (distro / 'usr/bin/tar').exists() or not qemu.exists() or not busybox.exists():
            self.skipTest('Real ARM GNU/runtime inputs are not present')
        files = boot.runtime_files(distro)
        with tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='bootstrap-arm-') as temporary:
            root = Path(temporary)
            (root / 'dev').mkdir(); (root / 'proc').mkdir()
            for name, source in files.items():
                target = root / name; target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target); target.chmod(0o755)
            (root / 'bin').mkdir(); shutil.copyfile(busybox, root / 'bin/busybox'); (root / 'bin/busybox').chmod(0o755)
            (root / 'opt').mkdir(); shutil.copyfile(qemu, root / 'opt/qemu'); (root / 'opt/qemu').chmod(0o755)
            entry = root / 'pianoinit'; entry.write_bytes((ROOT / 'linux/userspace/ram-bootstrap').read_bytes())
            prefix = ['bwrap', '--unshare-user', '--unshare-pid', '--die-with-parent', '--ro-bind', str(root), '/',
                      '--dev', '/dev', '--proc', '/proc', '--', '/opt/qemu']
            version = subprocess.run(prefix + ['/usr/bin/tar', '--version'], capture_output=True, text=True, timeout=30)
            self.assertEqual(version.returncode, 0, version.stderr); self.assertIn('GNU tar', version.stdout)
            syntax = subprocess.run(prefix + ['/bin/busybox', 'sh', '-n', '/pianoinit'], capture_output=True, text=True, timeout=30)
            self.assertEqual(syntax.returncode, 0, syntax.stderr)
            applets = subprocess.check_output(prefix + ['/bin/busybox', '--list'], text=True).splitlines()
            self.assertTrue(set(boot.APPLETS).issubset(applets))

    def test_actual_bootstrap_refuses_android_root_before_mount(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='bootstrap-policy-') as temporary:
            root = Path(temporary); (root / 'proc').mkdir(); (root / 'usr').mkdir()
            (root / 'bin').symlink_to('usr/bin'); (root / 'lib').symlink_to('usr/lib'); (root / 'lib64').symlink_to('usr/lib')
            (root / 'pianoinit').write_bytes((ROOT / 'linux/userspace/ram-bootstrap').read_bytes())
            (root / 'proc/cmdline').write_text('root=PARTLABEL=userdata piano.root=ram\n')
            result = subprocess.run(['bwrap', '--unshare-user', '--unshare-pid', '--die-with-parent',
                                     '--ro-bind', str(root), '/', '--ro-bind', '/usr', '/usr',
                                     '--', '/bin/sh', '/pianoinit'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn('Android block-root arguments are forbidden', result.stdout)
            self.assertFalse((root / 'sysroot').exists())


if __name__ == '__main__':
    unittest.main()
