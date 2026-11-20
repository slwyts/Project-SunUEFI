import struct
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


if __name__ == '__main__':
    unittest.main()
