"""Exercise history pruning with real UKIs when the native EFI stub is installed."""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

from test_kernel_install import BLS_PLUGIN, BOOT_HISTORY_PLUGIN, UKI_PLUGIN


ARCH = {'x86_64': 'x64', 'aarch64': 'aa64'}.get(platform.machine(), '')
EFI_STUB = Path(f'/usr/lib/systemd/boot/efi/linux{ARCH}.efi.stub')


@unittest.skipUnless(all(shutil.which(tool) for tool in ('bootctl', 'kernel-install', 'ukify'))
                     and EFI_STUB.is_file() and BLS_PLUGIN.is_file()
                     and UKI_PLUGIN.is_file(), 'systemd BLS/UKI tools and native EFI stub required')
class KernelInstallUkiTests(unittest.TestCase):
    def test_uki_replaces_same_version_bls_and_retires_counted_uki(self):
        with tempfile.TemporaryDirectory(prefix='kernel-install-uki-history-') as folder:
            work = Path(folder)
            esp, config = work / 'esp', work / 'config'
            esp.mkdir()
            config.mkdir()
            (config / 'cmdline').write_text('root=LABEL=PIANOROOT rw\n')
            (config / 'devicetree').write_text('board.dtb\n')
            (config / 'board.dtb').write_bytes(b'board')
            (config / 'tries').write_text('3\n')
            image, initrd = work / 'Image', work / 'initrd'
            image.write_bytes(b'kernel')
            initrd.write_bytes(b'initramfs')
            env = dict(os.environ, KERNEL_INSTALL_CONF_ROOT=str(config),
                       KERNEL_INSTALL_PLUGINS=f'{BLS_PLUGIN} {UKI_PLUGIN} {BOOT_HISTORY_PLUGIN}',
                       SYSTEMD_ESP_PATH=str(esp), SYSTEMD_RELAX_XBOOTLDR_CHECKS='1',
                       BOOT_ROOT=str(esp), BOOT_MNT=str(esp), SYSTEMD_LOG_LEVEL='warning')
            command = ['kernel-install', '--entry-token=literal:linux', 'add']
            old, previous, current = [f'7.3.3-piano-b{i}-g123456789abc' for i in (10, 20, 30)]
            (config / 'install.conf').write_text('layout=bls\ninitrd_generator=none\n')
            for version in (old, previous):
                subprocess.run([*command, version, image, initrd], env=env, check=True)

            def add_uki(version):
                (config / 'install.conf').write_text('layout=uki\ninitrd_generator=none\n')
                uki = work / f'{version}.efi'
                subprocess.run(['ukify', 'build', f'--stub={EFI_STUB}', f'--linux={image}',
                                f'--uname={version}', '--os-release=NAME=Fixture\nID=fixture\nVERSION_ID=rolling\n',
                                f'--output={uki}'], check=True, capture_output=True)
                subprocess.run([*command, version, uki], env=env, check=True)

            add_uki(previous)
            bls = esp / 'loader/entries'
            uki_dir = esp / 'EFI/Linux'
            self.assertEqual({path.name for path in bls.glob('*.conf')}, {f'linux-{old}+3.conf'})
            self.assertFalse((esp / 'linux' / previous / 'linux').exists())
            self.assertTrue((uki_dir / f'linux-{previous}+3.efi').is_file())

            add_uki(current)
            self.assertFalse(list(bls.glob('*.conf')))
            self.assertFalse((esp / 'linux' / old / 'linux').exists())
            addons = uki_dir / f'linux-{previous}.efi.extra.d'
            addons.mkdir()
            (addons / 'private.cred').write_bytes(b'test credential')
            (uki_dir / f'linux-{previous}+3.efi').rename(uki_dir / f'linux-{previous}+2-1.efi')
            (config / 'install.conf').write_text('layout=bls\ninitrd_generator=none\n')
            subprocess.run([*command, previous, image, initrd], env=env, check=True)
            self.assertFalse(addons.exists())
            self.assertEqual({path.name for path in uki_dir.glob('*.efi')}, {f'linux-{current}+3.efi'})
            self.assertTrue((bls / f'linux-{previous}+3.conf').is_file())
