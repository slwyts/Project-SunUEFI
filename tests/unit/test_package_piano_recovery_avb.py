"""Host-only recovery metadata packaging, using the pinned actual AOSP tool."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('recovery_avb', ROOT / 'tools/package_piano_recovery_avb.py')
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)


class RecoveryAvbTests(unittest.TestCase):
    def setUp(self):
        (ROOT / 'private/provisioning').mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='.test-recovery-avb-', dir=ROOT / 'private/provisioning')
        self.directory = Path(self.temporary.name)
        payload = bytearray(12288)
        payload[:8] = b'ANDROID!'
        struct.pack_into('<II', payload, 8, 4096, 4096)
        struct.pack_into('<I', payload, 20, 1580)
        struct.pack_into('<I', payload, 40, 3)
        payload[4096:4100] = b'test'
        self.payload = bytes(payload)
        self.image = self.directory / 'PianoUEFI-product.img'
        self.image.write_bytes(self.payload)
        self.manifest = self.directory / 'product.json'
        self.record = {'target': 'product', 'artifact': self.image.name,
                       'core_application_sha256': 'test-core-pin',
                       'files': {self.image.name: {'bytes': len(self.payload),
                                                  'sha256': hashlib.sha256(self.payload).hexdigest()}}}
        self.manifest.write_text(json.dumps(self.record))

    def tearDown(self):
        self.temporary.cleanup()

    def test_actual_footer_relocation_hash_and_determinism(self):
        first = PACKAGE.package(self.image, self.manifest, self.directory / 'first')
        second = PACKAGE.package(self.image, self.manifest, self.directory / 'second')
        self.assertEqual(first['container_sha256'], second['container_sha256'])
        self.assertEqual(first['status'], 'RECOVERY_ONLY_INSTALL_CONTAINER')
        self.assertEqual(first['avb']['hash_partition_names'], ['recovery'])
        self.assertEqual(first['avb']['salt_hex'], '')
        self.assertTrue(first['host_verify_image_passed'])
        self.assertFalse(first['signed'])
        self.assertFalse(first['device_boot_performed'])
        self.assertEqual(first['entry_compatibility']['boot_partition'], 'NOT_CLAIMED')
        self.assertEqual(self.image.read_bytes(), self.payload)
        container = self.directory / 'first/install-container.bin'
        self.assertEqual(container.stat().st_size, 104857600)
        self.assertTrue(PACKAGE.audit_container(container, self.payload)['host_hash_verified'])
        with container.open('r+b') as stream:
            stream.seek(first['avb']['vbmeta_offset'] + 120)
            stream.write(struct.pack('>I', 2))
        with self.assertRaisesRegex(ValueError, 'flags 0'):
            PACKAGE.audit_container(container, self.payload)

    def test_manifest_mismatch_rejected_before_output(self):
        self.record['files'][self.image.name]['sha256'] = '0' * 64
        self.manifest.write_text(json.dumps(self.record))
        output = self.directory / 'rejected'
        with self.assertRaisesRegex(ValueError, 'manifest size/SHA256'):
            PACKAGE.package(self.image, self.manifest, output)
        self.assertFalse(output.exists())
        self.assertEqual(self.image.read_bytes(), self.payload)

    def test_tool_pin_and_existing_output_preserved(self):
        with patch.object(PACKAGE, 'AVBTOOL_SHA256', '0' * 64):
            with self.assertRaisesRegex(ValueError, 'source pin'):
                PACKAGE.package(self.image, self.manifest, self.directory / 'pin-rejected')
        output = self.directory / 'existing'
        output.mkdir()
        marker = output / 'keep.txt'
        marker.write_text('preserved')
        with self.assertRaisesRegex(ValueError, 'already exists'):
            PACKAGE.package(self.image, self.manifest, output)
        self.assertEqual(marker.read_text(), 'preserved')


if __name__ == '__main__':
    unittest.main()
