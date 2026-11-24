"""Exercise native boot entry updates with systemd and a disposable boot directory."""
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import unittest


BLS_PLUGIN = Path('/usr/lib/kernel/install.d/90-loaderentry.install')
UKI_PLUGIN = BLS_PLUGIN.with_name('90-uki-copy.install')
BOOT_SPACE_PLUGIN = Path(__file__).resolve().parents[2] / 'tools/package/85-piano-boot-space.install'
BOOT_HISTORY_PLUGIN = BOOT_SPACE_PLUGIN.with_name('95-piano-boot-history.install')


class KernelInstallTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('kernel-install') and shutil.which('bootctl')
                         and BLS_PLUGIN.is_file(), 'systemd BLS tools required')
    def test_bls_retains_two_generations_after_successful_kernel_install(self):
        with tempfile.TemporaryDirectory(prefix='kernel-install-history-') as folder:
            work = Path(folder)
            esp = work / 'esp'
            config = work / 'config'
            entries = esp / 'loader/entries'
            entries.mkdir(parents=True)
            config.mkdir()
            (config / 'install.conf').write_text('layout=bls\ninitrd_generator=none\n')
            (config / 'cmdline').write_text('root=LABEL=PIANOROOT rw\n')
            (config / 'devicetree').write_text('board.dtb\n')
            (config / 'board.dtb').write_bytes(b'board')
            (config / 'tries').write_text('3\n')
            image, initrd = work / 'Image', work / 'initrd'
            image.write_bytes(b'kernel')
            initrd.write_bytes(b'initramfs')
            unmanaged = entries / 'another-os.conf'
            unmanaged.write_text('title Another OS\nlinux /another-os/Image\n')
            other_kernel = esp / 'another-os/Image'
            other_kernel.parent.mkdir()
            other_kernel.write_bytes(b'another kernel')
            env = dict(os.environ, KERNEL_INSTALL_CONF_ROOT=str(config),
                       KERNEL_INSTALL_PLUGINS=f'{BLS_PLUGIN} {BOOT_HISTORY_PLUGIN}',
                       SYSTEMD_ESP_PATH=str(esp), SYSTEMD_XBOOTLDR_PATH=str(esp),
                       BOOT_ROOT=str(esp), BOOT_MNT=str(esp), SYSTEMD_LOG_LEVEL='warning')
            command = ['kernel-install', '--entry-token=literal:linux', 'add']
            versions = [f'7.3.3-piano-b{i}-g123456789abc' for i in (10, 20, 30, 40)]
            for version in versions[:3]:
                subprocess.run([*command, version, image, initrd], env=env, check=True)

            def check_entries():
                self.assertEqual({path.name for path in entries.glob('*.conf')},
                                 {'another-os.conf',
                                  *(f'linux-{version}+3.conf' for version in versions[1:3])})
                self.assertEqual(other_kernel.read_bytes(), b'another kernel')
                for name in ('linux', 'initrd', 'board.dtb'):
                    self.assertFalse((esp / 'linux' / versions[0] / name).exists())
                    for version in versions[1:3]:
                        self.assertTrue((esp / 'linux' / version / name).is_file())

            check_entries()
            # A package trigger that refreshes an older initramfs must not
            # turn the retired release into the latest one.
            subprocess.run([*command, versions[0], image, initrd], env=env, check=True)
            check_entries()
            (config / 'devicetree').write_text('missing-board.dtb\n')
            failed = subprocess.run([*command, versions[3], image, initrd], env=env,
                                    capture_output=True)
            self.assertNotEqual(failed.returncode, 0)
            check_entries()

    def test_running_older_generation_is_the_fallback(self):
        retire = runpy.run_path(str(BOOT_HISTORY_PLUGIN))['retire']
        old, previous, current = [f'7.3.3-piano-b{i}-g123456789abc' for i in (10, 20, 30)]
        entries = [(old, 'type1', 'old.conf'), (current, 'type2', 'current.efi'),
                   (current, 'type1', 'current.conf'), (previous, 'type1', 'previous.conf')]
        self.assertEqual(retire(entries, old, 'type2'),
                         [('type1', 'current.conf'), ('type1', 'previous.conf')])
        self.assertEqual(retire(entries, 'outside', 'type2'),
                         [('type1', 'old.conf'), ('type1', 'current.conf')])

    @unittest.skipUnless(shutil.which('kernel-install') and BLS_PLUGIN.is_file(),
                         'systemd kernel-install and BLS plugin required')
    def test_bls_add_upgrade_remove_and_failed_generation(self):
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
                       KERNEL_INSTALL_PLUGINS=str(BLS_PLUGIN), BOOT_ROOT=str(esp), BOOT_MNT=str(esp),
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

    @unittest.skipUnless(shutil.which('kernel-install') and BLS_PLUGIN.is_file() and UKI_PLUGIN.is_file(),
                         'systemd kernel-install and BLS/UKI plugins required')
    def test_piano_reinstall_preserves_boot_files_when_space_is_insufficient(self):
        for layout, native in (('bls', BLS_PLUGIN), ('uki', UKI_PLUGIN)):
            with self.subTest(layout=layout), tempfile.TemporaryDirectory(
                    prefix='kernel-install-space-') as folder:
                work = Path(folder)
                esp = work / 'custom boot partition'
                config = work / 'config'
                old = work / 'previous'
                new = work / 'replacement'
                for path in (esp, config, old, new):
                    path.mkdir()
                (config / 'install.conf').write_text(f'layout={layout}\ninitrd_generator=none\n')
                (config / 'tries').write_text('3\n')
                (config / 'cmdline').write_text('root=LABEL=PIANOROOT rw\n')
                (config / 'devicetree').write_text('board.dtb\n')
                (config / 'board.dtb').write_bytes(b'previous DTB')
                images = ('linux', 'initrd') if layout == 'bls' else ('kernel.efi',)
                for name in images:
                    (old / name).write_bytes(b'previous ' + name.encode())
                    (new / name).write_bytes(b'replacement ' + name.encode())
                env = dict(os.environ, KERNEL_INSTALL_CONF_ROOT=str(config),
                           KERNEL_INSTALL_PLUGINS=f'{BOOT_SPACE_PLUGIN} {native}',
                           BOOT_ROOT=str(esp), BOOT_MNT=str(esp), SYSTEMD_LOG_LEVEL='warning')
                release = '7.4.0-rc1-piano-fixture'
                token = 'tablet-test'
                command = ['kernel-install', f'--entry-token=literal:{token}', 'add', release]
                subprocess.run([*command, *(old / name for name in images)], env=env, check=True)
                old_files = {path: path.read_bytes() for path in esp.rglob('*') if path.is_file()}
                self.assertEqual(len(old_files), 4 if layout == 'bls' else 1)

                space = os.statvfs(esp)
                oversized = new / images[-1]
                with oversized.open('wb') as stream:
                    stream.truncate((space.f_blocks + 1) * space.f_frsize)
                (config / 'board.dtb').write_bytes(b'replacement DTB')
                # If the guard regresses, also limit what a test subprocess can
                # write; a sparse input must never fill the CI host's disk.
                limited = ['sh', '-ec', 'ulimit -f 2048; exec "$@"', 'limit-boot-copy']
                result = subprocess.run([*limited, *command, *(new / name for name in images)],
                                        env=env, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f'Not enough free space on the boot partition ({esp})', result.stderr)
                self.assertIn('keeping a working boot entry', result.stderr)
                self.assertEqual(old_files,
                                 {path: path.read_bytes() for path in esp.rglob('*') if path.is_file()})

                oversized.write_bytes(b'replacement ' + oversized.name.encode())
                subprocess.run([*command, *(new / name for name in images)], env=env, check=True)
                if layout == 'bls':
                    self.assertTrue((esp / 'loader/entries' / f'{token}-{release}+3.conf').is_file())
                    payload = esp / token / release
                    for name in images:
                        self.assertEqual((payload / name).read_bytes(), (new / name).read_bytes())
                    self.assertEqual((payload / 'board.dtb').read_bytes(), b'replacement DTB')
                else:
                    self.assertEqual((esp / 'EFI/Linux' / f'{token}-{release}+3.efi').read_bytes(),
                                     oversized.read_bytes())


if __name__ == '__main__':
    unittest.main()
