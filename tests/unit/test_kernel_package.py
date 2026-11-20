"""Exercise native boot packages with real dpkg-deb/makepkg and shell parsing."""
import json
import os
from pathlib import Path
import pwd
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
    def assertBootUpdateOrder(self, root):
        drop_in = root / 'usr/lib/systemd/system/systemd-boot-update.service.d/20-piano-esp.conf'
        self.assertEqual(drop_in.read_text(), '[Unit]\nAfter=efi.mount boot.mount boot-efi.mount\n')
        self.assertEqual(drop_in.stat().st_mode & 0o777, 0o644)

    @unittest.skipUnless(shutil.which('dpkg') and shutil.which('vercmp'),
                         'APT and pacman version comparators required')
    def test_packaging_updates_do_not_change_the_kernel_abi_version(self):
        release = '7.4.0-rc1-piano-b1794850508-g13eff9d06e11'
        for package_format in ('debian', 'pacman'):
            previous = kernel.package_versions('7.4.0-rc1', release, package_format, 1794850508)
            current = kernel.package_versions('7.4.0-rc1', release, package_format, 1794850510)
            next_release = '7.4.0-rc1-piano-b1794850509-g33eff9d06e11'
            next_kernel = kernel.package_versions(
                '7.4.0-rc1', next_release, package_format, 1794850509)
            stable_kernel = kernel.package_versions(
                '7.4.0', '7.4.0-piano-b1794850511-g43eff9d06e11', package_format, 1794850511)
            self.assertEqual(previous[0], current[0])
            self.assertNotEqual(previous[0], next_kernel[0])
            if package_format == 'debian':
                subprocess.run(['dpkg', '--compare-versions', current[1], 'gt', previous[1]], check=True)
                subprocess.run(['dpkg', '--compare-versions', next_kernel[1], 'gt', current[1]], check=True)
                subprocess.run(['dpkg', '--compare-versions', stable_kernel[1], 'gt', next_kernel[1]], check=True)
            else:
                self.assertEqual(subprocess.check_output(
                    ['vercmp', current[1], previous[1]], text=True).strip(), '1')
                self.assertEqual(subprocess.check_output(
                    ['vercmp', next_kernel[1], current[1]], text=True).strip(), '1')
                self.assertEqual(subprocess.check_output(
                    ['vercmp', stable_kernel[1], next_kernel[1]], text=True).strip(), '1')

    @unittest.skipUnless(shutil.which('dpkg') and shutil.which('vercmp'),
                         'APT and pacman version comparators required')
    def test_pinned_versions_upgrade_from_head_dated_archives(self):
        # The previous product scheme used the project's HEAD date. Compare
        # against both its original and later ABI-first metadata versions.
        previous = ('7.3.3+1794950937.4ddc50e6f1a7.fb0b45dd97e9-1',
                    '7.3.3+1794995467.1794995467.4ddc50e6f1a7.fb0b45dd97e9-1')
        policy = kernel.release_kernel_policy()
        release = f"7.3.3-piano-b{policy['abi_epoch']}-g13eff9d06e11"
        for package_format in ('debian', 'pacman'):
            _, version = kernel.package_versions('7.3.3', release, package_format, policy['package_epoch'])
            for old in previous:
                if package_format == 'debian':
                    subprocess.run(['dpkg', '--compare-versions', version + '-1', 'gt', old], check=True)
                else:
                    self.assertEqual(subprocess.check_output(
                        ['vercmp', version + '-1', old], text=True).strip(), '1')

    def test_packages_cannot_predate_abi_epoch(self):
        with self.assertRaisesRegex(ValueError, 'precedes the sealed ABI epoch'):
            kernel.package_versions('7.3.3', '7.3.3-piano-b2-g13eff9d06e11', 'pacman', 1)

    @unittest.skipUnless(all(shutil.which(name) for name in (
        'makepkg', 'pacman', 'gpg', 'bsdtar', 'fakeroot', 'zstd')),
        'native pacman packaging tools required')
    def test_pacman_config_upgrade_preserves_admin_layout(self):
        build_user = None
        namespace = []
        if os.geteuid() == 0:
            if not Path('/.dockerenv').is_file():
                self.skipTest('use a regular user or the project builder container')
            try:
                build_user = pwd.getpwnam('sunuefi-build')
            except KeyError:
                self.skipTest('the container needs the ordinary sunuefi-build account')
        else:
            namespace = ['unshare', '--user', '--map-root-user']
            if not shutil.which('unshare') or subprocess.run(
                    [*namespace, 'true'], capture_output=True).returncode:
                self.skipTest('unprivileged user namespaces are unavailable')

        with tempfile.TemporaryDirectory(prefix='kernel-pacman-') as folder:
            work = Path(folder)
            if build_user:
                work.chmod(0o711)
            root = work / 'root'
            database = root / 'var/lib/pacman'
            cache = work / 'cache'
            gpgdir = work / 'gnupg'
            hooks = work / 'hooks'
            database.mkdir(parents=True)
            cache.mkdir()
            gpgdir.mkdir(mode=0o700)
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
                if build_user:
                    shutil.chown(output, user=build_user.pw_uid, group=build_user.pw_gid)
                plan = {'format': 'pacman', 'build': str(output),
                        'kernel_release': '7.3-piano-b1', 'version': version,
                        'abi_version': '7.3+1', 'revision': 1, 'package_epoch': 1,
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
                self.assertEqual(archive.name, f'piano-boot-config-{version}-3-any.pkg.tar.zst')
                archives.append(archive)
            command = [*namespace, 'pacman',
                       '--root', str(root), '--dbpath', str(database),
                       '--cachedir', str(cache), '--gpgdir', str(gpgdir),
                       '--logfile', str(work / 'pacman.log'), '--config', str(pacman_config),
                       '--hookdir', str(hooks), '--noconfirm', '--nodeps', '--nodeps', '--noscriptlet']
            subprocess.run([*command, '-U', archives[0]], check=True)
            config = root / 'etc/kernel/install.conf'
            config.write_text('layout=uki\ninitrd_generator=mkinitcpio\n')
            subprocess.run([*command, '-U', archives[1]], check=True)
            self.assertEqual(config.read_text(), 'layout=uki\ninitrd_generator=mkinitcpio\n')
            self.assertBootUpdateOrder(root)
            self.assertTrue((root / 'etc/pacman.d/hooks/90-mkinitcpio-install.hook').is_symlink())
            self.assertIn('[sunuefi]\n', (root / 'etc/pacman.d/sunuefi.conf').read_text())
            boot_space = root / 'usr/lib/kernel/install.d' / boot.BOOT_SPACE_HOOK.name
            self.assertEqual(boot_space.read_bytes(), boot.BOOT_SPACE_HOOK.read_bytes())
            self.assertEqual(boot_space.stat().st_mode & 0o777, 0o755)
            subprocess.run(['sh', '-n', boot_space], check=True)
            initramfs_config = root / 'etc/mkinitcpio.conf.d/zz-piano.conf'
            initramfs = subprocess.check_output(['bash', '-ec',
                'HOOKS=(base systemd keyboard blockdev_ro partsets initrd_shell factory_reset '
                'etc_overlay deckard_fw custom_hook); MODULES=(local_module); . "$1"; '
                'printf "%s\\n" "${HOOKS[@]}" "${MODULES[@]}"',
                'check-mkinitcpio-config', initramfs_config], text=True)
            self.assertEqual(initramfs.splitlines(), [
                'base', 'systemd', 'keyboard', 'custom_hook', 'local_module',
                'arm_smmu', 'ufs_qcom', 'phy_qcom_qmp_ufs', 'pinctrl_sm8750', 'hid_nanosic_wn8030?',
            ])
            transaction = root / 'usr/libexec/piano-kernel-transaction'
            subprocess.run(['bash', '-n', transaction], check=True)
            subprocess.run([*command, '-R', 'piano-boot-config'], check=True)
            self.assertFalse(config.exists())
            self.assertFalse((root / 'usr/lib/systemd/system/systemd-boot-update.service.d'
                              / '20-piano-esp.conf').exists())
            self.assertEqual(config.with_suffix('.conf.pacsave').read_text(),
                             'layout=uki\ninitrd_generator=mkinitcpio\n')

    @unittest.skipUnless(shutil.which('dpkg-deb'), 'dpkg-deb required')
    def test_debian_packages_own_versioned_dtb_and_standard_hooks(self):
        with tempfile.TemporaryDirectory(prefix='kernel-native-') as folder:
            output = Path(folder)
            plan = {'format': 'debian', 'build': str(output), 'kernel_release': '7.3-piano-b1',
                    'version': '7.3+2', 'abi_version': '7.3+1',
                    'revision': kernel.DEBIAN_REVISION, 'package_epoch': 1}
            with patch.object(boot, 'prepare_dtb', return_value=b'device tree fixture'):
                packages = boot.add_packages(
                    plan, output, {'command_line': 'rdinit=/pianoinit piano.root=LABEL=PIANOROOT ro rw'},
                    kernel.sha, image_modules=(('linux-piano-v4l2loopback-7.3-piano-b1', '0.15.4-1'),))
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
            boot_space = next(output.glob(
                f'piano-boot-config_*/usr/lib/kernel/install.d/{boot.BOOT_SPACE_HOOK.name}'))
            self.assertEqual(boot_space.read_bytes(), boot.BOOT_SPACE_HOOK.read_bytes())
            self.assertEqual(boot_space.stat().st_mode & 0o777, 0o755)
            boot_package = next(path for path in packages if path.name.startswith('piano-boot-config_'))
            self.assertBootUpdateOrder(output / boot_package.stem)
            self.assertIn('systemd (>= 258)', subprocess.check_output(
                ['dpkg-deb', '-f', boot_package, 'Depends'], text=True))
            control_dir = output / 'boot-control'
            subprocess.run(['dpkg-deb', '-e', boot_package, control_dir], check=True)
            self.assertEqual((control_dir / 'triggers').read_text(),
                             'activate-noawait update-initramfs\n')
            self.assertFalse(list(output.glob('piano-boot-config_*/usr/share/kernel/postinst.d/*')))
            self.assertFalse(list(output.glob('piano-boot-config_*/usr/libexec/piano-kernel-install')))
            cmdline = next(output.glob('piano-boot-config_*/usr/lib/kernel/cmdline')).read_text()
            self.assertEqual(cmdline, 'root=LABEL=PIANOROOT rw\n')
            self.assertFalse(list(output.glob('*/usr/lib/kernel/install.conf')))
            dtb = next(output.glob('linux-piano-dtb-*/usr/lib/linux-image-*/piano/board.dtb'))
            self.assertEqual(dtb.read_bytes(), b'device tree fixture')
            image_meta = next(path for path in packages if path.name.startswith('linux-image-piano_'))
            depends = subprocess.check_output(['dpkg-deb', '-f', image_meta, 'Depends'], text=True)
            self.assertIn('linux-image-7.3-piano-b1 (= 7.3+1-2)', depends)
            self.assertIn('linux-piano-dtb-7.3-piano-b1 (= 7.3+2-2)', depends)
            self.assertIn('linux-piano-v4l2loopback-7.3-piano-b1 (= 0.15.4-1)', depends)
            self.assertIn('piano-boot-config', depends)

    @unittest.skipUnless(all(shutil.which(name) for name in ('make', 'dpkg-gencontrol')),
                         'make and Debian package tools required')
    def test_debian_image_declares_matched_boot_dependencies(self):
        release = '7.3.3-piano-b42-g13eff9d06e11'
        version = '7.3.3+42.13eff9d06e11-2'
        image = 'linux-image-' + release
        headers = 'linux-headers-' + release
        dependencies = kernel.debian_image_dependencies('7.3.3', release, 42)
        with tempfile.TemporaryDirectory(prefix='kernel-debian-metadata-') as folder:
            work = Path(folder)
            debian = work / 'debian'
            debian.mkdir()
            control = debian / 'control'
            control.write_text(
                'Source: linux-piano\nSection: kernel\nPriority: optional\n'
                'Maintainer: Test <test@example.org>\n\n'
                f'Package: {image}\nArchitecture: any\nDepends: kmod\nDescription: Image\n\n'
                f'Package: {headers}\nArchitecture: any\nDescription: Headers\n')
            (debian / 'changelog').write_text(
                f'linux-piano ({version}) unstable; urgency=low\n\n'
                '  * Fixture.\n\n'
                ' -- Test <test@example.org>  Tue, 14 Nov 2023 00:00:00 +0000\n')
            (debian / 'rules').write_text(
                'revision = $(shell dpkg-parsechangelog -S Version | sed -n "s/.*-//p")\n'
                'build-arch:\n\t@printf "%s\\n" "$(revision)"\n')
            (debian / 'tmp/DEBIAN').mkdir(parents=True)
            makefile = ROOT / 'tools/package/kernel-debian.mk'
            command = ['make', '--no-print-directory', '-sf', makefile,
                       f'KERNELRELEASE={release}', 'build-arch']
            env = dict(os.environ, SOURCE_DATE_EPOCH='42', KBUILD_BUILD_VERSION='1',
                       SUNUEFI_DEBIAN_IMAGE_DEPENDS=dependencies)
            for _ in range(2):
                self.assertEqual(subprocess.check_output(
                    command, cwd=work, env=env, text=True).strip(), '1')
            image_control = subprocess.check_output(
                ['dpkg-gencontrol', '-p' + image, '-Pdebian/tmp', '-O'], cwd=work, text=True)
            headers_control = subprocess.check_output(
                ['dpkg-gencontrol', '-p' + headers, '-Pdebian/tmp', '-O'], cwd=work, text=True)
            self.assertIn(f'Version: {version}\n', image_control)
            self.assertIn('Depends: linux-piano-dtb-' + release
                          + ', piano-boot-config (>= 7.3.3+42.42.13eff9d06e11-2), kmod\n',
                          image_control)
            self.assertNotIn('Depends:', headers_control)

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
