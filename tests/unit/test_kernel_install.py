"""Exercise BLS entry updates with systemd and a disposable boot directory."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


BLS_PLUGIN = Path('/usr/lib/kernel/install.d/90-loaderentry.install')


class KernelInstallTests(unittest.TestCase):
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


if __name__ == '__main__':
    unittest.main()
