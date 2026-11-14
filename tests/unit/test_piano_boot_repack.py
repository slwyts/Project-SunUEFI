"""Execute the real native BOOT parser on bounded AVB/BOOT4 fixture files."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_boot_repack

EEA_FINGERPRINT = (
    b'Xiaomi/piano_eea/piano:15/AQ3A.250226.002/OS3.0.305.0.WPYEUXM:user/release-keys'
)


def boot_image(fingerprint):
    kernel = bytearray(8192)
    struct.pack_into('<IIQQQ', kernel, 0, 0xfa405a4d, 0x14000010, 0, 16384, 10)
    kernel[56:60] = b'ARMd'
    header = bytearray(4096)
    header[:8] = b'ANDROID!'
    struct.pack_into('<I', header, 8, len(kernel))
    struct.pack_into('<I', header, 20, 1584)
    struct.pack_into('<I', header, 40, 4)
    original = bytes(header + kernel)

    salt, name = b'Piano BOOT fixture', b'boot'
    image_hash = hashlib.sha256(salt + original).digest()
    descriptor = bytearray((132 + len(name) + len(salt) + len(image_hash) + 7) & ~7)
    struct.pack_into('>QQQ', descriptor, 0, 2, len(descriptor) - 16, len(original))
    descriptor[24:31] = b'sha256\0'
    struct.pack_into('>III', descriptor, 56, len(name), len(salt), len(image_hash))
    descriptor[132:132 + len(name) + len(salt) + len(image_hash)] = name + salt + image_hash

    key = b'com.android.build.boot.fingerprint'
    prop = bytearray((32 + len(key) + 1 + len(fingerprint) + 1 + 7) & ~7)
    struct.pack_into('>QQQQ', prop, 0, 0, len(prop) - 16, len(key), len(fingerprint))
    prop[32:32 + len(key)] = key
    prop[33 + len(key):33 + len(key) + len(fingerprint)] = fingerprint
    descriptors = descriptor + prop
    aux_bytes = (len(descriptors) + 63) & ~63
    vbmeta = bytearray(256 + aux_bytes)
    vbmeta[:4] = b'AVB0'
    struct.pack_into('>I', vbmeta, 4, 1)
    struct.pack_into('>Q', vbmeta, 20, aux_bytes)
    struct.pack_into('>Q', vbmeta, 104, len(descriptors))
    vbmeta[256:256 + len(descriptors)] = descriptors

    image = bytearray(65536)
    image[:len(original)] = original
    image[len(original):len(original) + len(vbmeta)] = vbmeta
    footer = len(image) - 64
    image[footer:footer + 4] = b'AVBf'
    struct.pack_into('>I', image, footer + 4, 1)
    struct.pack_into('>QQQ', image, footer + 12, len(original), len(original), len(vbmeta))
    return image


class PianoBootRepackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (ROOT / 'upstream/simple-init/libs/libmd/src/sha2.c').is_file():
            raise unittest.SkipTest('The fixed libmd source checkout is required for native compilation')
        cls.temporary = tempfile.TemporaryDirectory(prefix='piano-boot-repack-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        cls.binary, _ = build_boot_repack.build(cls.directory / 'native', 'host')

    def probe(self, image):
        path = self.directory / 'boot.img'
        path.write_bytes(image)
        import subprocess
        return subprocess.run([str(self.binary), 'probe', '--input', str(path)],
                              capture_output=True, text=True)

    def test_china_global_and_eea_piano_fingerprints(self):
        for product in ('piano', 'piano_global', 'piano_eea'):
            for suffix in ('15/AQ3A.250226.002/OS3.0.304.0.WPYMIXM:user/release-keys',
                           '15/AQ3A.250226.002/OS3.0.305.0.WPYEUXM:user/release-keys',
                           '16/BP2A.250605.031.A3/OS3.0.304.0.WPYMIXM:user/release-keys', 'x'):
                fingerprint = ('Xiaomi/' + product + '/piano:' + suffix).encode()
                with self.subTest(fingerprint=fingerprint):
                    result = self.probe(boot_image(fingerprint))
                    self.assertEqual(result.returncode, 0, result.stderr)
                    record = json.loads(result.stdout)
                    self.assertFalse(record['wrapped'])
                    self.assertTrue(record['avb_hash_matches'])
                    self.assertFalse(record['device_operation_performed'])

    def test_other_devices_and_malformed_fingerprints_are_rejected(self):
        fingerprints = (
            b'Other/piano/piano:x', b'xiaomi/piano/piano:x',
            b'Xiaomi/pianox/piano:x', b'Xiaomi/piano_globalx/piano:x',
            b'Xiaomi/piano_eeax/piano:x', b'Xiaomi/piano_eea_global/piano:x',
            b'Other/piano_eea/piano:x', b'Xiaomi/piano_eea/pianox:x',
            b'Xiaomi/piano_pro/piano:x', b'Xiaomi/piano/pianox:x',
            b'Xiaomi/piano_global/pianox:x', b'Xiaomi/piano/other:x',
            b'Xiaomi/piano_global/other:x', b'Xiaomi/piano//piano:x',
            b'Xiaomi/piano_eea/other:x', b'Xiaomi/piano_eea//piano:x',
            b'Xiaomi/piano/piano', b'Xiaomi/piano/piano:',
            b'Xiaomi/piano_global/piano:', b'Xiaomi/piano/piano:x\0extra',
            b'Xiaomi/piano_eea/piano:', b'Xiaomi/piano_eea/piano:x\0extra',
            b'Xiaomi/piano_global/piano:x\0extra', b'Xiaomi/piano/piano:' + b'x' * 1024,
        )
        for fingerprint in fingerprints:
            with self.subTest(fingerprint=fingerprint):
                result = self.probe(boot_image(fingerprint))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('BOOT is not the supported Piano ROM family', result.stderr)

    def test_global_support_keeps_hash_and_extent_checks(self):
        fingerprint = b'Xiaomi/piano_global/piano:15/AQ3A.250226.002/OS3.0.304.0.WPYMIXM:user/release-keys'
        image = boot_image(fingerprint)
        image[4096 + 128] ^= 1
        result = self.probe(image)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('stock BOOT hash descriptor mismatch', result.stderr)

        image = boot_image(fingerprint)
        struct.pack_into('>Q', image, len(image) - 64 + 20, len(image))
        result = self.probe(image)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('AVB extent is outside BOOT', result.stderr)

    def test_eea_support_keeps_hash_extent_and_geometry_checks(self):
        image = boot_image(EEA_FINGERPRINT)
        image[4096 + 128] ^= 1
        result = self.probe(image)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('stock BOOT hash descriptor mismatch', result.stderr)

        image = boot_image(EEA_FINGERPRINT)
        struct.pack_into('>Q', image, len(image) - 64 + 20, len(image))
        result = self.probe(image)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('AVB extent is outside BOOT', result.stderr)

        for offset, value, error in (
                (40, 3, 'expected BOOT4'),
                (12, 4096, 'expected BOOT4'),
                (1580, 4096, 'expected BOOT4'),
                (8, len(image), 'invalid kernel extent')):
            with self.subTest(offset=offset):
                image = boot_image(EEA_FINGERPRINT)
                struct.pack_into('<I', image, offset, value)
                result = self.probe(image)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(error, result.stderr)

    def test_eea_file_repack_requests_and_restore_are_lossless(self):
        with tempfile.TemporaryDirectory(prefix='eea-roundtrip-', dir=self.directory) as directory:
            base = Path(directory)
            stock = base / 'stock.img'
            image = boot_image(EEA_FINGERPRINT)
            image[32768:32784] = b'original padding'
            stock.write_bytes(image)
            selector, product, output = (base / name for name in ('selector', 'product', 'output'))
            for path in (selector, product, output):
                path.mkdir()
            (selector / 'selector.bin').write_bytes(bytes(512))
            (selector / 'selector.json').write_text(json.dumps({
                'selector_bytes': 512, 'selector_sha256': build_boot_repack.sha(selector / 'selector.bin'),
                'selector_memory_bytes': 8704, 'metadata_offset': 256,
            }))
            shim = bytearray(128)
            shim[56:60] = b'ARMd'
            (product / 'BootShim.bin').write_bytes(shim)
            (product / 'PianoUEFI-product.fd').write_bytes(bytes(0x300000))
            (product / 'manifest.json').write_text(json.dumps({
                'target': 'product', 'files': {
                    name: {'bytes': (product / name).stat().st_size,
                           'sha256': build_boot_repack.sha(product / name)}
                    for name in ('BootShim.bin', 'PianoUEFI-product.fd')},
            }))
            code = b'MZ' + bytes(4094)
            app = base / 'app.bin'
            app.write_bytes(struct.pack('<16sIIQ32s', b'SUNUEFI-APPv1\0', 1, 64, len(code),
                                        hashlib.sha256(code).digest()) + code)

            record = build_boot_repack.roundtrip(self.binary, stock, selector, product, app, output)
            self.assertTrue(record['byte_equal'])
            self.assertEqual(record['restored_sha256'], hashlib.sha256(image).hexdigest())
            self.assertTrue(record['persistent_request']['restored_byte_equal'])
            self.assertFalse(record['device_operation_performed'])
            self.assertFalse(record['oem_signature_verified'])
            self.assertFalse(record['new_wrapper_oem_signed'])
            self.assertFalse(record['persistent_request']['changed_wrapper_probe']['avb_hash_matches'])
            self.assertIn(EEA_FINGERPRINT, (output / record['wrapped_file']).read_bytes())
            self.assertEqual(stock.read_bytes(), image)


if __name__ == '__main__':
    unittest.main()
