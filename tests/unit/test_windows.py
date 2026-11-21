import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/build'))
import windows


class WindowsMediaTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def test_loader_requires_arm64_efi_subsystem(self):
        loader = self.root / 'bootaa64.efi'
        header = bytearray(158)
        header[:2] = b'MZ'
        struct.pack_into('<I', header, 60, 64)
        header[64:68] = b'PE\0\0'
        struct.pack_into('<H', header, 68, 0xaa64)
        struct.pack_into('<H', header, 88, 0x20b)
        struct.pack_into('<H', header, 156, 10)
        loader.write_bytes(header)
        windows.check_loader(loader)
        for offset, value in [(68, 0x8664), (156, 3)]:
            with self.subTest(offset=offset):
                wrong = header.copy()
                struct.pack_into('<H', wrong, offset, value)
                loader.write_bytes(wrong)
                with self.assertRaisesRegex(ValueError, 'not an ARM64 EFI application'):
                    windows.check_loader(loader)

    def test_publish_never_replaces_existing_esp(self):
        output = self.root / 'esp'
        fallback = output / 'EFI/BOOT/BOOTAA64.EFI'
        fallback.parent.mkdir(parents=True)
        fallback.write_bytes(b'existing systemd-boot')
        with self.assertRaisesRegex(ValueError, 'Not a generated Windows media directory'):
            windows.publish(self.root / 'media', output, self.root / 'previous')
        self.assertEqual(fallback.read_bytes(), b'existing systemd-boot')

    @unittest.skipUnless(shutil.which('wimlib-imagex'), 'wimlib-imagex is needed to exercise WIM servicing')
    def test_driver_files_only_enter_the_selected_boot_image(self):
        tool = shutil.which('wimlib-imagex')
        base = self.root / 'base'
        base.mkdir()
        (base / 'original').write_bytes(b'original file')
        wim = self.root / 'boot.wim'
        subprocess.run([tool, 'capture', base, wim, 'Base', '--quiet'], check=True)
        subprocess.run([tool, 'append', base, wim, 'Setup', '--quiet'], check=True)
        original = windows.digest(wim)
        windows.customize_winpe(tool, wim, 2, self.root, False, None)
        self.assertEqual(windows.digest(wim), original)

        drivers = self.root / 'vendor packages'
        package = drivers / 'storage chip'
        (package / 'arm64').mkdir(parents=True)
        (package / 'STORAGE.INF').write_bytes(b'inf bytes')
        (package / 'storage.cat').write_bytes(b'catalog bytes')
        (package / 'arm64' / 'device.sys').write_bytes(b'payload bytes')
        self.assertEqual(windows.driver_infs(drivers), ['storage chip/STORAGE.INF'])
        windows.customize_winpe(tool, wim, 2, self.root, False, drivers)
        unchanged = subprocess.check_output([tool, 'dir', wim, '1'], text=True)
        self.assertNotIn('/SunUEFI', unchanged)
        self.assertIn('/original', unchanged)
        for item in package.rglob('*'):
            if item.is_file():
                data = subprocess.check_output([tool, 'extract', wim, '2',
                                                '/SunUEFI/Drivers/' + item.relative_to(drivers).as_posix(),
                                                '--to-stdout'])
                self.assertEqual(data, item.read_bytes())
        ini = subprocess.check_output([tool, 'extract', wim, '2', '/Windows/System32/winpeshl.ini', '--to-stdout'])
        self.assertIn(b'\\SunUEFI\\drivers.cmd setup"\r\n', ini)


if __name__ == '__main__':
    unittest.main()
