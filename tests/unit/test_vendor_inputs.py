"""Verify the actual fixed bundle and refusal of altered/native unsafe inputs."""
import copy
import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import piano_vendor_inputs as vendor
from compose_piano_dtb import read_fdt, write_fdt
from piano_inherited_clock import derive, ORIGINAL_SHA256, DERIVED_SHA256


class VendorInputsTests(unittest.TestCase):
    def fixture(self, directory):
        root = Path(directory)
        shutil.copytree(ROOT / vendor.BUNDLE, root / vendor.BUNDLE)
        return root

    def write_manifest(self, root, manifest):
        (root / vendor.BUNDLE / 'manifest.json').write_text(json.dumps(manifest, indent=2))

    def test_actual_bundle_has_exact_original_drivers_and_unchanged_clock_derivation(self):
        bundle = vendor.load(ROOT)
        self.assertEqual(set(bundle['native_drivers']), set(vendor.NATIVE_NAMES))
        self.assertEqual(len(bundle['paths']), 28)
        self.assertEqual(vendor.sha(bundle['board_dtb']), vendor.BOARD_DTB_SHA256)
        self.assertEqual(vendor.sha(bundle['xbl_config_dtb']), vendor.XBL_DTB_SHA256)
        self.assertEqual(bundle['manifest']['board_dtb']['source_sha256'], vendor.BOARD_SOURCE_SHA256)
        clock = bundle['native_drivers']['ClockDxe']['pe_path'].read_bytes()
        self.assertEqual(vendor.sha(clock), ORIGINAL_SHA256)
        derived, record = derive(clock)
        self.assertEqual(vendor.sha(derived), DERIVED_SHA256)
        self.assertNotEqual(clock, derived)
        self.assertTrue(record)

    def test_only_native_firmware_and_dtbs_are_sufficient_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, target = ROOT / vendor.BUNDLE, root / vendor.BUNDLE
            target.mkdir(parents=True)
            shutil.copyfile(source / 'manifest.json', target / 'manifest.json')
            for name in ('native', 'dtb'):
                shutil.copytree(source / name, target / name)
            bundle = vendor.load(root)
            self.assertEqual(set(bundle['paths']), set(bundle['manifest']['files']))
            self.assertEqual(len(bundle['native_drivers']), 13)
            self.assertEqual(set(path.parts[0] for path in map(Path, bundle['paths'])), {'native', 'dtb'})
            # Unused historical material has no authority over firmware inputs.
            history = target / 'history';history.mkdir()
            (history / 'device-layout.bin').write_bytes(b'not a firmware input')
            self.assertEqual(vendor.load(root)['paths'], bundle['paths'])

    def test_native_code_tampering_rejected_even_if_manifest_is_rehashed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.fixture(directory)
            path = root / vendor.BUNDLE / 'native/ClockDxe.efi'
            raw = bytearray(path.read_bytes())
            raw[0xc6d0] ^= 1  # Actual audited ARM64 cleanup instruction.
            path.write_bytes(raw)
            with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
                vendor.load(root)
            manifest = json.loads((root / vendor.BUNDLE / 'manifest.json').read_text())
            manifest['files']['native/ClockDxe.efi']['sha256'] = vendor.sha(raw)
            self.write_manifest(root, manifest)
            with self.assertRaisesRegex(ValueError, 'Fixed vendor manifest SHA256 mismatch'):
                vendor.load(root)

    def test_manifest_paths_cannot_escape_or_alias_inputs(self):
        original = json.loads((ROOT / vendor.BUNDLE / 'manifest.json').read_text())
        for path in ('../outside.efi', '/tmp/outside.efi', 'native/../outside.efi', 'native\\outside.efi'):
            with self.subTest(path=path), tempfile.TemporaryDirectory() as directory:
                root = self.fixture(directory)
                manifest = copy.deepcopy(original)
                manifest['native_drivers']['ClockDxe']['pe'] = path
                self.write_manifest(root, manifest)
                with self.assertRaisesRegex(ValueError, 'Unsafe vendor input path'):
                    vendor.load(root)
        with tempfile.TemporaryDirectory() as directory:
            root = self.fixture(directory)
            manifest = copy.deepcopy(original)
            manifest['native_drivers']['ClockDxe']['pe'] = manifest['native_drivers']['SmemDxe']['pe']
            self.write_manifest(root, manifest)
            with self.assertRaisesRegex(ValueError, 'aliased inputs'):
                vendor.load(root)

    def test_file_and_parent_symlinks_are_rejected(self):
        for parent in (False, True):
            with self.subTest(parent=parent), tempfile.TemporaryDirectory() as directory:
                root = self.fixture(directory)
                path = root / vendor.BUNDLE / ('native' if parent else 'native/ClockDxe.efi')
                shutil.rmtree(path) if parent else path.unlink()
                path.symlink_to(ROOT / vendor.BUNDLE / ('native' if parent else 'native/ClockDxe.efi'), target_is_directory=parent)
                with self.assertRaisesRegex(ValueError, 'symlink'):
                    vendor.load(root)

    def test_missing_or_unlisted_payloads_and_missing_native_members_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.fixture(directory)
            (root / vendor.BUNDLE / 'native/HALIOMMU.depex').unlink()
            with self.assertRaisesRegex(ValueError, 'Missing vendor input'):
                vendor.load(root)
        with tempfile.TemporaryDirectory() as directory:
            root = self.fixture(directory)
            (root / vendor.BUNDLE / 'unlisted.bin').write_bytes(b'not a build input')
            with self.assertRaisesRegex(ValueError, 'Unlisted vendor input'):
                vendor.load(root)
        with tempfile.TemporaryDirectory() as directory:
            root = self.fixture(directory)
            manifest = json.loads((root / vendor.BUNDLE / 'manifest.json').read_text())
            del manifest['native_drivers']['HALIOMMU']
            self.write_manifest(root, manifest)
            with self.assertRaisesRegex(ValueError, 'native driver set'):
                vendor.load(root)
        with tempfile.TemporaryDirectory() as directory:
            root = self.fixture(directory)
            manifest = json.loads((root / vendor.BUNDLE / 'manifest.json').read_text())
            del manifest['board_dtb']['path']
            self.write_manifest(root, manifest)
            with self.assertRaisesRegex(ValueError, 'field set'):
                vendor.load(root)

    def test_product_native_staging_preserves_original_code_depex_and_clock_rules(self):
        from prepare_product import native_modules
        with tempfile.TemporaryDirectory() as directory:
            root = self.fixture(directory)
            bundle = vendor.load(root)
            app = root / 'product-app';app.mkdir()
            fdf, identities = native_modules(root, app, bundle)
            self.assertEqual(set(identities), set(vendor.NATIVE_NAMES))
            for name in vendor.NATIVE_NAMES:
                original = bundle['native_drivers'][name]['pe_path'].read_bytes()
                staged = root / 'upstream/Mu-Silicium/Binaries/piano/ProductFoundation' / name / (name + '.efi')
                expected = derive(original)[0] if name == 'ClockDxe' else original
                self.assertEqual(staged.read_bytes(), expected)
                self.assertEqual(identities[name]['pe_sha256'], vendor.sha(expected))
                self.assertEqual(identities[name]['depex_sha256'], vendor.sha(bundle['native_drivers'][name]['depex_path'].read_bytes()))
            self.assertEqual(fdf.count('SECTION PE32'), 13)
            self.assertTrue((app / 'NativeProbeTable.h').is_file())

    def test_arm64_pe_code_and_section_validation(self):
        data = (ROOT / vendor.BUNDLE / 'native/ClockDxe.efi').read_bytes()
        vendor.validate_pe(data)
        pe = struct.unpack_from('<I', data, 60)[0]
        optional = pe + 24
        table = optional + struct.unpack_from('<H', data, pe + 20)[0]
        executable = next(table + index * 40 for index in range(struct.unpack_from('<H', data, pe + 6)[0])
                          if struct.unpack_from('<I', data, table + index * 40 + 36)[0] & 0x20000000)
        cases = []
        changed = bytearray(data);struct.pack_into('<H', changed, pe + 4, 0x8664);cases.append(changed)
        changed = bytearray(data);struct.pack_into('<I', changed, optional + 16, 0);cases.append(changed)
        changed = bytearray(data);struct.pack_into('<I', changed, executable + 20, len(data));cases.append(changed)
        changed = bytearray(data);struct.pack_into('<I', changed, executable + 36, 0);cases.append(changed)
        for changed in cases:
            with self.subTest(change=cases.index(changed)), self.assertRaises(ValueError):
                vendor.validate_pe(changed)

    def test_depex_bounds_and_boolean_stack(self):
        vendor.validate_depex(b'\x02' + b'\0' * 16 + b'\x06\x03\x08')
        for data in (b'\x02\0\x08', b'\x03\x08', b'\x05\x08', b'\x06', b'\x06\x08\x06', b'\x09\x09\x06\x08'):
            with self.subTest(data=data), self.assertRaises(ValueError):
                vendor.validate_depex(data)

    def test_board_dtb_refuses_snapshot_identity_and_stale_handoff(self):
        data = (ROOT / vendor.BOARD_DTB).read_bytes()
        vendor.validate_board_dtb(data)
        for key, value in (('serial-number', b'fixture-only\0'), ('rng-seed', b'\x11' * 32), ('linux,initrd-start', b'\0' * 8)):
            parsed = read_fdt(data)
            parsed['tree']['/chosen'][key] = value
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'snapshot fields'):
                vendor.validate_board_dtb(write_fdt(parsed))


if __name__ == '__main__':
    unittest.main()
