"""Provenance refusal and metadata-race tests for the host-only file exporter."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'tools'))
import export_os_boot_bundle as bundle


class BootBundleExportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='piano-export-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root/'artifacts/kernels/stable/ram'
        self.source.mkdir(parents=True)
        self.out = self.root/'bundle'
        self.dtb = self.root/'selected.dtb'
        image = bytearray(1024)
        image[:2], image[56:60], image[128:132] = b'MZ', b'ARM\x64', b'PE\0\0'
        struct.pack_into('<I', image, 60, 128)
        struct.pack_into('<HH', image, 132, 0xaa64, 1)
        struct.pack_into('<H', image, 148, 112)
        struct.pack_into('<H', image, 152, 0x20b)
        struct.pack_into('<II', image, 280, 512, 512)
        files = {'Image': image, 'initramfs.cpio.gz': b'fixture initrd', 'config': b'CONFIG_EFI=y\n'}
        checksums = {}
        for name, data in files.items():
            (self.source/name).write_bytes(data)
            checksums[name] = hashlib.sha256(data).hexdigest()
        data = bytearray(40)
        struct.pack_into('>II', data, 0, 0xd00dfeed, len(data))
        self.dtb.write_bytes(data)
        self.write(self.root/'linux/kernel-profiles.json', {'profiles': {'stable': {'commit': 'abc'}}})
        self.write(self.source/'manifest.json', {'source_commit': 'abc', 'source_dirty': False,
            'status': 'HOST_BUILT_NOT_HARDWARE_VERIFIED', 'image': {'sha256': checksums['Image']},
            'config_sha256': checksums['config']})
        self.write(self.source/'initramfs-manifest.json', {'source_commit': 'abc',
            'kernel_image': {'sha256': checksums['Image']}, 'initramfs': {'sha256': checksums['initramfs.cpio.gz']}})
        self.evidence = self.source/'ram-validation-test-71.json'
        self.write(self.evidence, {'source_commit': 'abc', 'status': 'VERIFIED_RAW_ARM64_RAM_SMOKE_ONLY',
            'kernel_sha256': checksums['Image'], 'initrd_sha256': checksums['initramfs.cpio.gz'],
            'config_sha256': checksums['config'], 'dtb_sha256': hashlib.sha256(data).hexdigest()})
        self.override = patch.object(bundle, 'ROOT', self.root)
        self.override.start()
        self.addCleanup(self.override.stop)

    def write(self, path, data):
        path.write_text(json.dumps(data))

    def test_export_preserves_scope_and_refuses_overwrite(self):
        result = bundle.export('stable', self.dtb, self.out)
        self.assertFalse(result['entry']['default_enabled'])
        self.assertFalse(result['efi_handoff_verified'])
        self.assertEqual((self.out/'raw-smoke-evidence.json').read_bytes(), self.evidence.read_bytes())
        for name in ('Image', 'config', 'initramfs.cpio.gz'):
            self.assertEqual((self.out/name).read_bytes(), (self.source/name).read_bytes())
        with self.assertRaisesRegex(ValueError, 'Output exists'):
            bundle.export('stable', self.dtb, self.out)

    def test_changed_payload_and_false_smoke_scope_refuse_before_output(self):
        image = self.source/'Image'
        original = image.read_bytes()
        image.write_bytes(original[:-1]+b'x')
        with self.assertRaisesRegex(ValueError, 'Source bytes changed'):
            bundle.export('stable', self.dtb, self.out)
        self.assertFalse(self.out.exists())
        image.write_bytes(original)
        record = json.loads(self.evidence.read_text())
        record['status'] = 'EFI_VERIFIED'
        self.write(self.evidence, record)
        with self.assertRaisesRegex(ValueError, 'raw smoke record'):
            bundle.export('stable', self.dtb, self.out)
        self.assertFalse(self.out.exists())

    def test_metadata_change_during_copy_marks_incomplete(self):
        copy = bundle.shutil.copyfile
        def change_metadata(source, destination):
            result = copy(source, destination)
            if Path(source).name == 'Image':
                self.evidence.write_text(self.evidence.read_text()+'\n')
            return result
        with patch.object(bundle.shutil, 'copyfile', change_metadata):
            with self.assertRaisesRegex(ValueError, 'provenance changed'):
                bundle.export('stable', self.dtb, self.out)
        self.assertTrue((self.out/'FAILED.txt').exists())
        self.assertFalse((self.out/'manifest.json').exists())


if __name__ == '__main__':
    unittest.main()
