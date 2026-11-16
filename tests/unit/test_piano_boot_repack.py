"""Execute the real native BOOT parser on bounded AVB/BOOT4 fixture files."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_boot_repack

EEA_FINGERPRINT = (
    b'Xiaomi/piano_eea/piano:15/AQ3A.250226.002/OS3.0.305.0.WPYEUXM:user/release-keys'
)


def boot_image(fingerprint, algorithm='sha256'):
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
    image_hash = hashlib.new(algorithm, salt + original).digest()
    descriptor = bytearray((132 + len(name) + len(salt) + len(image_hash) + 7) & ~7)
    struct.pack_into('>QQQ', descriptor, 0, 2, len(descriptor) - 16, len(original))
    descriptor[24:31] = algorithm.encode() + b'\0'
    struct.pack_into('>III', descriptor, 56, len(name), len(salt), len(image_hash))
    descriptor[132:132 + len(name) + len(salt) + len(image_hash)] = name + salt + image_hash

    key = b'com.android.build.boot.fingerprint'
    prop = bytearray()
    if fingerprint is not None:
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

    def test_custom_gki_fingerprint_is_metadata_not_device_identity(self):
        for fingerprint in (None, b'', b'Custom/GKI/arm64:16/custom:userdebug/test-keys',
                            b'Xiaomi/missi/missi:17/custom:user/release-keys'):
            with self.subTest(fingerprint=fingerprint):
                result = self.probe(boot_image(fingerprint))
                self.assertEqual(result.returncode, 0, result.stderr)
                record = json.loads(result.stdout)
                self.assertTrue(record['avb_hash_matches'])
                self.assertFalse(record['boot_source_confirm_required'])

    def test_malformed_fingerprint_property_still_rejected(self):
        fingerprints = (
            b'Custom/GKI/arm64:x\0extra',
            b'Xiaomi/piano/piano:x\0extra',
            b'Xiaomi/piano_eea/piano:x\0extra',
            b'Xiaomi/piano_global/piano:x\0extra', b'Xiaomi/piano/piano:' + b'x' * 1024,
        )
        for fingerprint in fingerprints:
            with self.subTest(fingerprint=fingerprint):
                result = self.probe(boot_image(fingerprint))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('invalid BOOT fingerprint property', result.stderr)

    def test_global_support_keeps_hash_and_extent_checks(self):
        fingerprint = b'Xiaomi/piano_global/piano:15/AQ3A.250226.002/OS3.0.304.0.WPYMIXM:user/release-keys'
        image = boot_image(fingerprint)
        image[4096 + 128] ^= 1
        result = self.probe(image)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)['boot_source_confirm_required'])

        image = boot_image(fingerprint)
        struct.pack_into('>Q', image, len(image) - 64 + 20, len(image))
        result = self.probe(image)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('AVB extent is outside BOOT', result.stderr)

    def test_eea_support_keeps_hash_extent_and_geometry_checks(self):
        image = boot_image(EEA_FINGERPRINT)
        image[4096 + 128] ^= 1
        result = self.probe(image)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)['boot_source_confirm_required'])

        image = boot_image(EEA_FINGERPRINT)
        struct.pack_into('>Q', image, len(image) - 64 + 20, len(image))
        result = self.probe(image)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('AVB extent is outside BOOT', result.stderr)

        for offset, value, error in (
                (40, 2, 'expected Android BOOT3/4'),
                (12, len(image), 'ramdisk or boot signature'),
                (1580, len(image), 'ramdisk or boot signature'),
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

    def test_custom_gki_roundtrip_and_explicit_modified_source_choice(self):
        variants = {}
        variants['custom-sha512-metadata'] = boot_image(b'Custom/GKI/arm64:x', 'sha512')
        variants['missing-fingerprint'] = boot_image(None)
        image = boot_image(None)
        image[4096 + 128:4096 + 140] = b'custom patch'
        variants['stale-avb'] = image
        image = boot_image(None)
        image[12288:] = bytes(len(image) - 12288)
        variants['without-avb'] = image
        image = boot_image(None)
        struct.pack_into('>Q', image, 12288 + 104, 0)
        variants['without-hash'] = image
        image = boot_image(None)
        metadata = image[12288:12800]
        image[12288:12800] = bytes(512)
        image[20480:20992] = metadata
        image[12288:16384] = b'custom extra'.ljust(4096, b'\0')
        struct.pack_into('<I', image, 8, 12288)
        struct.pack_into('>Q', image, len(image) - 64 + 20, 20480)
        variants['old-hash-extent'] = image
        image = boot_image(None)
        struct.pack_into('<I', image, 4096, 0xD503201F)
        variants['non-efi-entry'] = image
        image = boot_image(None)
        struct.pack_into('<I', image, 20, 1580)
        struct.pack_into('<I', image, 40, 3)
        variants['boot-v3'] = image
        image = boot_image(None)
        image[12288:16384] = b'generic ramdisk'.ljust(4096, b'\0')
        image[16384:20480] = b'GKI signature'.ljust(4096, b'\0')
        image[20480:20992] = boot_image(None)[12288:12800]
        struct.pack_into('<I', image, 12, 100)
        struct.pack_into('<I', image, 1580, 4096)
        struct.pack_into('>Q', image, len(image) - 64 + 20, 20480)
        variants['ramdisk-and-signature'] = image
        for label, image in variants.items():
            with self.subTest(label=label), tempfile.TemporaryDirectory(dir=self.directory) as directory:
                base = Path(directory)
                source, wrapped = base / 'source.img', base / 'wrapped.img'
                source.write_bytes(image)
                selector, shim, fd, app = (base / name for name in ('selector', 'shim', 'fd', 'app'))
                selector.write_bytes(bytes(512))
                shim.write_bytes(bytes(56) + b'ARMd' + bytes(68))
                fd.write_bytes(bytes(0x300000))
                code = b'MZ' + bytes(4094)
                app.write_bytes(struct.pack('<16sIIQ32s', b'SUNUEFI-APPv1\0', 1, 64, len(code),
                                            hashlib.sha256(code).digest()) + code)
                command = [str(self.binary), 'repack', '--input', str(source), '--output', str(wrapped),
                           '--selector', str(selector), '--selector-memory-bytes', '8704',
                           '--selector-metadata-offset', '256', '--shim', str(shim), '--fd', str(fd), '--app', str(app)]
                probe = json.loads(self.probe(image).stdout)
                needs_choice = label not in ('custom-sha512-metadata', 'missing-fingerprint')
                self.assertEqual(probe['boot_source_confirm_required'], needs_choice)
                result = subprocess.run(command, capture_output=True, text=True)
                if needs_choice:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse(wrapped.exists())
                    result = subprocess.run([*command, '--allow-modified-boot'], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                data = wrapped.read_bytes()
                after_kernel = 4096 + struct.unpack_from('<I', data, 8)[0]
                if label == 'ramdisk-and-signature':
                    self.assertEqual(data[after_kernel:after_kernel + 8192], image[12288:20480])
                if label == 'boot-v3':
                    self.assertEqual(struct.unpack_from('<I', data, 40)[0], 3)
                if label == 'non-efi-entry':
                    self.assertEqual(struct.unpack_from('<I', data, 4096)[0], 0xD503201F)
                requested = base / 'requested.img'
                subprocess.run([str(self.binary), 'request', '--input', str(wrapped), '--output', str(requested),
                                '--target', 'linux'], capture_output=True, check=True)
                restored = base / 'restored.img'
                subprocess.run([str(self.binary), 'restore', '--input', str(requested), '--output', str(restored)],
                               capture_output=True, check=True)
                self.assertEqual(restored.read_bytes(), image)
                malformed = bytearray(image)
                struct.pack_into('<I', malformed, 4100, 0)
                source.write_bytes(malformed)
                wrapped.unlink()
                result = subprocess.run([*command, '--allow-modified-boot'], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('unsupported GKI branch', result.stderr)
                self.assertFalse(wrapped.exists())


if __name__ == '__main__':
    unittest.main()
