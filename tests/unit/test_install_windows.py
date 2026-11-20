"""Windows deployment must leave the stock GPT and the shared EFI fallback alone."""
from dataclasses import replace
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import install_piano as piano
from device import install_windows as windows


class WindowsInstallationTest(unittest.TestCase):
    def setUp(self):
        def part(i, name, first, mib, kind):
            count = mib * windows.MIB // piano.BLOCK
            return {'name': name, 'index': i, 'first': first, 'last': first + count - 1,
                    'bytes': mib * windows.MIB, 'guid': str(uuid.uuid4()),
                    'type': str(uuid.UUID(bytes_le=kind)), 'attributes': 0x4000000000000000}
        self.user = part(2, 'userdata', 256, 128, piano.LINUX_TYPE)
        self.esp = part(7, 'sunuefi_esp', self.user['last'] + 1, 512, piano.ESP_TYPE)
        self.root = part(9, 'sunuefi_root', self.esp['last'] + 1, 8192, piano.LINUX_TYPE)
        self.gpt = piano.Gpt(12 * 1024**3, b'', b'', b'', b'',
                             {'disk_guid': str(uuid.uuid4())}, 1, (self.user, self.esp, self.root))

    def test_probe_keeps_existing_system_and_gpt_unless_resize_requested(self):
        same = windows.layout(self.gpt, None, False)
        self.assertEqual(same, (self.esp, self.root))
        self.assertEqual(windows.gpt_command('/test', same, same), [])
        converted = windows.layout(self.gpt, None, True)
        self.assertEqual(windows.gpt_command('/test', same, converted),
                         ['sgdisk', '--set-alignment=1', f'--typecode=10:{converted[1]["type"]}', '/test'])

    def test_adjacent_resize_moves_only_the_shared_boundary(self):
        esp, root = windows.layout(self.gpt, 2048, True)
        self.assertEqual(esp['first'], self.esp['first'])
        self.assertEqual(root['last'], self.root['last'])
        self.assertEqual((root['first'], root['bytes']), (esp['last'] + 1, (8192 - 1536) * windows.MIB))
        self.assertEqual([p['guid'] for p in (esp, root)], [self.esp['guid'], self.root['guid']])
        after = replace(self.gpt, partitions=(self.user, esp, root))
        windows.verify_gpt(self.gpt, after, (esp, root))
        with self.assertRaisesRegex(ValueError, 'GPT readback differs'):
            windows.verify_gpt(self.gpt, replace(after, partitions=({**self.user, 'last': 999}, esp, root)),
                               (esp, root))
        with self.assertRaisesRegex(ValueError, 'immediately followed'):
            windows.layout(replace(self.gpt, partitions=(self.user, self.esp, {**self.root, 'first': 123})),
                           2048, False)

    def test_install_media_cannot_replace_os_fallback(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fallback = 'esp/EFI/BOOT/BOOTAA64.EFI'
            path = root / fallback
            path.parent.mkdir(parents=True)
            path.write_bytes(b'an unrelated loader')
            (root/'media.json').write_text(json.dumps({
                'format': 'sunuefi-windows-media', 'loader': windows.LOADER,
                'files': {fallback: piano.file_sha(path)}}))
            with self.assertRaisesRegex(ValueError, 'outside the Windows boot files'):
                windows.media_files(root)

    @unittest.skipUnless(all(shutil.which(cmd) for cmd in ('mkfs.fat', 'fsck.fat', 'mmd', 'mcopy', 'mdir')),
                         'Install dosfstools and mtools for the FAT32 integration test')
    def test_growing_4kn_fat_preserves_existing_loader_and_volume_id(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            old, new = folder/'old.fat', folder/'new.fat'
            with old.open('xb') as f:
                f.truncate(300 * windows.MIB)  # >= 65,525 FAT32 clusters on 4Kn
            subprocess.run(['mkfs.fat', '-F', '32', '-S', '4096', '-s', '1', '-i', '1234abcd',
                            '-n', 'SUNUEFI_ESP', old], check=True, stdout=subprocess.DEVNULL)
            boot = folder/'boot.efi'
            boot.write_bytes(b'original EFI fallback')
            subprocess.run(['mmd', '-i', old, '::/EFI', '::/EFI/BOOT'], check=True)
            subprocess.run(['mcopy', '-i', old, boot, '::/EFI/BOOT/BOOTAA64.EFI'], check=True)
            new_loader = folder/'esp'/windows.LOADER
            new_loader.parent.mkdir(parents=True)
            new_loader.write_bytes(b'Microsoft EFI loader')
            windows.build_esp(old, new, {'first': 256, 'bytes': 330 * windows.MIB}, [windows.LOADER], folder)
            for name, expected in [('EFI/BOOT/BOOTAA64.EFI', boot), (windows.LOADER, new_loader)]:
                data = subprocess.check_output(['mcopy', '-i', new, '::/' + name, '-'])
                self.assertEqual(data, expected.read_bytes())
            with new.open('rb') as f:
                bpb = f.read(512)
            self.assertEqual(int.from_bytes(bpb[11:13], 'little'), 4096)
            self.assertEqual(int.from_bytes(bpb[67:71], 'little'), 0x1234abcd)
            self.assertEqual(int.from_bytes(bpb[32:36], 'little') * 4096, new.stat().st_size)


if __name__ == '__main__':
    unittest.main()
