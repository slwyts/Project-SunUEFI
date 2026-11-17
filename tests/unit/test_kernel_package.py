"""Exercise native boot packages with real dpkg-deb/makepkg and shell parsing."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from package import boot, kernel


class KernelPackageTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('dpkg') and shutil.which('vercmp'),
                         'APT and pacman version comparators required')
    def test_packaging_updates_do_not_change_the_kernel_abi_version(self):
        manifest = {'source_tree': 'c' * 40, 'config_sha256': 'a' * 64}
        release = '7.4.0-rc1-piano-b1794850508-g13eff9d06e11'
        for package_format in ('debian', 'pacman'):
            previous = kernel.package_versions('7.4.0-rc1', release, manifest, package_format, '1794850508')
            current = kernel.package_versions('7.4.0-rc1', release, manifest, package_format, '1794850509')
            next_release = release.replace('b1794850508', 'b1794850509')
            next_kernel = kernel.package_versions(
                '7.4.0-rc1', next_release, {**manifest, 'source_tree': '4' * 40},
                package_format, '1794850509')
            self.assertEqual(previous[0], current[0])
            if package_format == 'debian':
                subprocess.run(['dpkg', '--compare-versions', current[1], 'gt', previous[1]], check=True)
                subprocess.run(['dpkg', '--compare-versions', next_kernel[1], 'gt', current[1]], check=True)
            else:
                self.assertEqual(subprocess.check_output(
                    ['vercmp', current[1], previous[1]], text=True).strip(), '1')
                self.assertEqual(subprocess.check_output(
                    ['vercmp', next_kernel[1], current[1]], text=True).strip(), '1')

    @unittest.skipUnless(os.geteuid() != 0 and all(shutil.which(name) for name in (
        'makepkg', 'pacman', 'unshare', 'gpg', 'bsdtar')),
        'unprivileged Arch package tools and user namespaces required')
    def test_pacman_config_upgrade_preserves_admin_layout(self):
        namespace = subprocess.run(['unshare', '--user', '--map-root-user', 'true'],
                                   capture_output=True)
        if namespace.returncode:
            self.skipTest('unprivileged user namespaces are unavailable')
        with tempfile.TemporaryDirectory(prefix='kernel-pacman-') as folder:
            work = Path(folder)
            root = work / 'root'
            database = root / 'var/lib/pacman'
            hooks = work / 'hooks'
            database.mkdir(parents=True)
            hooks.mkdir()
            pacman_config = work / 'pacman.conf'
            pacman_config.write_text('[options]\nSigLevel = Never\n')
            # Package-manager configuration handling is tested separately from
            # the real kernel-install plugin test below.
            for name in ('40-piano-kernel-remove.hook', '95-piano-kernel-install.hook'):
                (hooks / name).symlink_to('/dev/null')
            archives = []
            for version in ('7.3+1', '7.3+2'):
                output = work / version
                output.mkdir()
                plan = {'format': 'pacman', 'build': str(output),
                        'kernel_release': '7.3-piano-b1', 'version': version,
                        'abi_version': '7.3+1', 'package_epoch': 1,
                        'environment': {'PACMAN_PKGBASE': 'linux-piano-7.3-piano-b1'}}
                with patch.object(boot, 'prepare_dtb', return_value=b'matched device tree'):
                    packages = boot.add_packages(plan, output,
                                                 {'command_line': 'rw'}, kernel.sha)
                self.assertTrue(any(path.name.startswith('linux-piano-dtb-7.3-piano-b1-')
                                    for path in packages))
                meta = next(path for path in packages if path.name.startswith('linux-piano-' + version))
                self.assertIn('depend = linux-piano-dtb-7.3-piano-b1=' + version + '-1',
                              subprocess.check_output(['bsdtar', '-xOf', meta, '.PKGINFO'], text=True))
                archive = next(path for path in packages if path.name.startswith('piano-boot-config-'))
                archives.append(archive)
            command = ['unshare', '--user', '--map-root-user', 'pacman',
                       '--root', str(root), '--dbpath', str(database),
                       '--logfile', str(work / 'pacman.log'), '--config', str(pacman_config),
                       '--hookdir', str(hooks), '--noconfirm', '--nodeps', '--nodeps', '--noscriptlet']
            subprocess.run([*command, '-U', archives[0]], check=True)
            config = root / 'etc/kernel/install.conf'
            config.write_text('layout=uki\ninitrd_generator=mkinitcpio\n')
            subprocess.run([*command, '-U', archives[1]], check=True)
            self.assertEqual(config.read_text(), 'layout=uki\ninitrd_generator=mkinitcpio\n')
            self.assertTrue((root / 'etc/pacman.d/hooks/90-mkinitcpio-install.hook').is_symlink())
            self.assertIn('[sunuefi]\n', (root / 'etc/pacman.d/sunuefi.conf').read_text())
            initramfs_config = root / 'etc/mkinitcpio.conf.d/zz-piano.conf'
            hooks = subprocess.check_output(['bash', '-ec',
                'HOOKS=(base systemd keyboard blockdev_ro partsets initrd_shell factory_reset '
                'etc_overlay deckard_fw custom_hook); . "$1"; printf "%s\\n" "${HOOKS[@]}"',
                'check-mkinitcpio-config', initramfs_config], text=True)
            self.assertEqual(hooks.splitlines(), ['base', 'systemd', 'keyboard', 'custom_hook'])
            transaction = root / 'usr/libexec/piano-kernel-transaction'
            subprocess.run(['bash', '-n', transaction], check=True)
            subprocess.run([*command, '-R', 'piano-boot-config'], check=True)
            self.assertFalse(config.exists())
            self.assertEqual(config.with_suffix('.conf.pacsave').read_text(),
                             'layout=uki\ninitrd_generator=mkinitcpio\n')

    @unittest.skipUnless(shutil.which('kernel-install') and Path(
        '/usr/lib/kernel/install.d/90-loaderentry.install').is_file(),
        'systemd kernel-install and BLS plugin required')
    def test_systemd_bls_add_upgrade_remove_and_failed_generation(self):
        with tempfile.TemporaryDirectory(prefix='kernel-install-native-') as folder:
            work = Path(folder)
            esp = work / 'esp'
            config = work / 'config'
            esp.mkdir()
            config.mkdir()
            (config / 'install.conf').write_text('layout=bls\ninitrd_generator=none\n')
            (config / 'entry-token').write_text('fixture\n')
            (config / 'cmdline').write_text('root=LABEL=PIANOROOT rw\n')
            (config / 'devicetree').write_text('piano/board.dtb\n')
            (config / 'piano').mkdir()
            (config / 'piano/board.dtb').write_bytes(b'matching-device-tree')
            image = work / 'Image'
            initrd = work / 'initrd'
            image.write_bytes(b'kernel-install copies EFI kernel bytes without modification')
            initrd.write_bytes(b'initramfs fixture')
            env = dict(os.environ, KERNEL_INSTALL_CONF_ROOT=str(config),
                       KERNEL_INSTALL_PLUGINS='/usr/lib/kernel/install.d/90-loaderentry.install',
                       BOOT_ROOT=str(esp), BOOT_MNT=str(esp),
                       SYSTEMD_LOG_LEVEL='warning')
            command = ['kernel-install', '--entry-token=literal:fixture']
            for version in ('1.0', '2.0'):
                subprocess.run([*command, 'add', version, image, initrd], env=env, check=True)
            first = esp / 'loader/entries/fixture-1.0.conf'
            second = esp / 'loader/entries/fixture-2.0.conf'
            self.assertTrue(first.is_file() and second.is_file())
            self.assertEqual((esp / 'fixture/2.0/linux').read_bytes(), image.read_bytes())
            self.assertEqual((esp / 'fixture/2.0/initrd').read_bytes(), initrd.read_bytes())
            self.assertEqual((esp / 'fixture/2.0/board.dtb').read_bytes(), b'matching-device-tree')
            before = first.read_bytes(), second.read_bytes()
            (config / 'devicetree').write_text('missing-board.dtb\n')
            result = subprocess.run([*command, 'add', '3.0', image, initrd], env=env,
                                    capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(before, (first.read_bytes(), second.read_bytes()))
            self.assertFalse((esp / 'loader/entries/fixture-3.0.conf').exists())
            subprocess.run([*command, 'remove', '2.0'], env=env, check=True)
            self.assertTrue(first.is_file())
            self.assertFalse(second.exists())

    @unittest.skipUnless(shutil.which('dpkg-deb'), 'dpkg-deb required')
    def test_debian_packages_own_versioned_dtb_and_standard_hooks(self):
        with tempfile.TemporaryDirectory(prefix='kernel-native-') as folder:
            output = Path(folder)
            plan = {'format': 'debian', 'build': str(output), 'kernel_release': '7.3-piano-b1',
                    'version': '7.3+2', 'abi_version': '7.3+1', 'package_epoch': 1}
            with patch.object(boot, 'prepare_dtb', return_value=b'device tree fixture'):
                packages = boot.add_packages(
                    plan, output, {'command_line': 'rdinit=/pianoinit piano.root=LABEL=PIANOROOT ro rw'},
                    kernel.sha)
            self.assertEqual(len(packages), 4)
            for archive in packages:
                destination = output / archive.stem
                subprocess.run(['dpkg-deb', '-x', archive, destination], check=True)
                for script in destination.rglob('*'):
                    if script.is_file() and script.read_bytes().startswith(b'#!/bin/sh'):
                        subprocess.run(['sh', '-n', script], check=True)
            config = next(output.glob('piano-boot-config_*/etc/kernel/install.conf'))
            self.assertEqual('layout=bls\n', config.read_text())
            hook = next(output.glob('piano-boot-config_*/usr/share/initramfs-tools/hooks/piano'))
            self.assertIn('arm_smmu', hook.read_text())
            boot_package = next(path for path in packages if path.name.startswith('piano-boot-config_'))
            self.assertIn('systemd (>= 258)', subprocess.check_output(
                ['dpkg-deb', '-f', boot_package, 'Depends'], text=True))
            self.assertFalse(list(output.glob('piano-boot-config_*/usr/share/kernel/postinst.d/*')))
            self.assertFalse(list(output.glob('piano-boot-config_*/usr/libexec/piano-kernel-install')))
            cmdline = next(output.glob('piano-boot-config_*/usr/lib/kernel/cmdline')).read_text()
            self.assertEqual(cmdline, 'root=LABEL=PIANOROOT rw\n')
            self.assertFalse(list(output.glob('*/usr/lib/kernel/install.conf')))
            dtb = next(output.glob('linux-piano-dtb-*/usr/lib/linux-image-*/piano/board.dtb'))
            self.assertEqual(dtb.read_bytes(), b'device tree fixture')
            image_meta = next(path for path in packages if path.name.startswith('linux-image-piano_'))
            depends = subprocess.check_output(['dpkg-deb', '-f', image_meta, 'Depends'], text=True)
            self.assertIn('linux-image-7.3-piano-b1 (= 7.3+1-1)', depends)
            self.assertIn('linux-piano-dtb-7.3-piano-b1 (= 7.3+2-1)', depends)
            self.assertIn('piano-boot-config', depends)

    @unittest.skipUnless(shutil.which('dpkg-deb'), 'dpkg-deb required')
    def test_native_image_preserves_arm64_efi_binary(self):
        import gzip
        with tempfile.TemporaryDirectory(prefix='kernel-efi-image-') as folder:
            root = Path(folder) / 'payload'
            (root / 'boot').mkdir(parents=True)
            (root / 'DEBIAN').mkdir()
            (root / 'DEBIAN/control').write_text(
                'Package: linux-image-test\nVersion: 1\nArchitecture: arm64\n'
                'Maintainer: Test <test@example.org>\nDescription: Test image\n')
            image = root / 'boot/vmlinuz-test'
            pe = bytearray(128)
            pe[:2] = b'MZ'
            pe[56:64] = b'ARM\x64\x40\0\0\0'
            pe[64:70] = b'PE\0\0\x64\xaa'
            image.write_bytes(pe)
            module = root / 'lib/modules/test/kernel/driver.ko'
            module.parent.mkdir(parents=True)
            module.write_bytes(b'driver bytes')
            modules = [{'path': module.relative_to(root).as_posix(), 'sha256': kernel.sha(module)}]
            archive = Path(folder) / 'image.deb'
            subprocess.run(['dpkg-deb', '--root-owner-group', '--build', root, archive], check=True)
            kernel.verify_image(archive, 'debian', 'test', kernel.sha(image), modules)
            with self.assertRaisesRegex(ValueError, 'Packaged module differs'):
                kernel.verify_image(archive, 'debian', 'test', kernel.sha(image),
                                    [{**modules[0], 'sha256': '0' * 64}])
            image.write_bytes(gzip.compress(pe, mtime=0))
            subprocess.run(['dpkg-deb', '--root-owner-group', '--build', root, archive], check=True)
            with self.assertRaisesRegex(ValueError, 'uncompressed EFI Image'):
                kernel.verify_image(archive, 'debian', 'test', kernel.sha(image), modules)

    @unittest.skipUnless(shutil.which('zstd'), 'zstd required')
    def test_headers_reject_wrong_architecture_tools(self):
        import io
        import tarfile
        with tempfile.TemporaryDirectory(prefix='kernel-headers-') as folder:
            archive = Path(folder) / 'headers.tar'
            with tarfile.open(archive, 'w') as contents:
                for name in ('include/config/auto.conf', 'Module.symvers',
                             'include/generated/utsrelease.h', 'scripts/basic/fixdep',
                             'scripts/mod/modpost'):
                    data = b'fixture'
                    if name.endswith('modpost'):
                        data = b'\x7fELF' + bytes(14) + b'\x3e\x00'
                    member = tarfile.TarInfo('usr/src/headers/' + name)
                    member.size = len(data)
                    contents.addfile(member, io.BytesIO(data))
            compressed = Path(folder) / 'headers.pkg.tar.zst'
            subprocess.run(['zstd', '-q', archive, '-o', compressed], check=True)
            with self.assertRaisesRegex(ValueError, 'rather than ARM64'):
                kernel.verify_headers(compressed, 'pacman')


if __name__ == '__main__':
    unittest.main()
