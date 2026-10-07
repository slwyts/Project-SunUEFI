"""PC-only attestation tests using copies of the actual fixed capture fixtures.

The prepare CLI is never invoked. Every generated header and every deliberately
altered fixture lives in a temporary directory; no device or build runs here.
"""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import tempfile
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('prepare_ufs_write_test', ROOT / 'tools/prepare_ufs_write_test.py')
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)
FIXTURES_PRESENT = tool.CAPTURE.is_dir() and tool.TEST57.is_dir()


@unittest.skipUnless(FIXTURES_PRESENT, 'actual private capture1/test57 PC fixtures absent')
class PrepareUfsWriteTestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.capture = self.folder / 'capture1'
        self.test57 = self.folder / 'test57'
        shutil.copytree(tool.CAPTURE, self.capture)
        self.test57.mkdir()
        for name in ('manifest.json', 'lun-4-mbr-header.bin', 'lun-4-entries.bin'):
            shutil.copyfile(tool.TEST57 / name, self.test57 / name)
        self.output = self.folder / 'generated' / tool.HEADER_NAME

    def manifest(self, directory=None):
        return json.loads(((directory or self.capture) / 'manifest.json').read_text())

    def save_manifest(self, value, directory=None):
        ((directory or self.capture) / 'manifest.json').write_text(json.dumps(value))

    def verify(self):
        return tool.verify_capture(self.capture, self.test57)

    def generate(self):
        return tool.prepare_ufs_write_test(self.capture, self.test57, self.output)

    def changed(self, name):
        value = bytearray((self.capture / name).read_bytes())
        value[-1] ^= 1
        return bytes(value)

    def replace_and_repin(self, replacements):
        """Test only: pass the hash layer to independently exercise GPT checks.

        Production has no pin override or allow-unverified option. Altered bytes
        are always refused without this in-process mock of source constants.
        """
        pins = copy.deepcopy(tool.EXPECTED_FILES)
        manifest = self.manifest()
        for name, value in replacements.items():
            (self.capture / name).write_bytes(value)
            digest = hashlib.sha256(value).hexdigest()
            pins[name] = (len(value), digest)
            manifest['files'][name] = {'bytes': len(value), 'sha256': digest}
        self.save_manifest(manifest)
        return mock.patch.object(tool, 'EXPECTED_FILES', pins)

    def changed_gpt_entries(self, edit):
        entries = bytearray((self.capture / 'primary-entries.bin').read_bytes())
        edit(entries)
        replacements = {'primary-entries.bin': bytes(entries), 'backup-entries.bin': bytes(entries)}
        for name in ('primary-header.bin', 'backup-header.bin'):
            header = bytearray((self.capture / name).read_bytes())
            struct.pack_into('<I', header, 88, zlib.crc32(entries))
            struct.pack_into('<I', header, 16, 0)
            size = struct.unpack_from('<I', header, 12)[0]
            struct.pack_into('<I', header, 16, zlib.crc32(header[:size]))
            replacements[name] = bytes(header)
        return replacements

    def test_actual_pc_archive_all_nine_pins_and_source_preservation(self):
        before = {name: (path.read_bytes(), path.stat().st_mtime_ns)
                  for name in (*tool.EXPECTED_FILES, 'manifest.json')
                  for path in (self.capture / name,)}
        with (mock.patch.object(tool._gpt, 'main', side_effect=AssertionError('CLI must not run')),
              mock.patch.object(tool._gpt.subprocess, 'check_output', side_effect=AssertionError('no device calls'))):
            blobs = self.verify()
        self.assertEqual(set(tool.EXPECTED_FILES), set(blobs))
        for name, (size, digest) in tool.EXPECTED_FILES.items():
            self.assertEqual(size, len(blobs[name]))
            self.assertEqual(digest, hashlib.sha256(blobs[name]).hexdigest())
        self.assertEqual(14 * 1024 * 1024, len(blobs['gap-original.bin']))
        self.assertFalse(any(blobs['gap-original.bin']))
        for name, (data, modified) in before.items():
            self.assertEqual(data, (self.capture / name).read_bytes())
            self.assertEqual(modified, (self.capture / name).stat().st_mtime_ns)
        self.assertFalse(self.output.parent.exists())

    def test_temporary_header_exact_arrays_hashes_and_attestation_macros(self):
        result = self.generate()
        self.assertEqual(self.output, result)
        text = result.read_text()
        symbols = {
            'mPianoUfsWriteTestPrimaryHeader': 'primary-header.bin',
            'mPianoUfsWriteTestPrimaryEntries': 'primary-entries.bin',
            'mPianoUfsWriteTestBackupHeader': 'backup-header.bin',
            'mPianoUfsWriteTestBackupEntries': 'backup-entries.bin',
            'mPianoUfsWriteTestOriginalBlock': 'first-block-original.bin',
        }
        total = 0
        for symbol, name in symbols.items():
            match = re.search(r'STATIC CONST UINT8 ' + symbol + r'\[(\d+)\] = \{(.*?)\};', text, re.S)
            self.assertIsNotNone(match, symbol)
            value = bytes(int(item, 16) for item in re.findall(r'0x([0-9A-F]{2})', match[2]))
            self.assertEqual(int(match[1]), len(value))
            self.assertEqual((self.capture / name).read_bytes(), value)
            self.assertEqual(tool.EXPECTED_FILES[name][1], hashlib.sha256(value).hexdigest())
            total += len(value)
        gap = re.search(r'mPianoUfsWriteTestGapSha256\[32\] = \{(.*?)\};', text, re.S)
        self.assertIsNotNone(gap)
        digest = bytes(int(item, 16) for item in re.findall(r'0x([0-9A-F]{2})', gap[1]))
        self.assertEqual(bytes.fromhex(tool.EXPECTED_FILES['gap-original.bin'][1]), digest)
        self.assertEqual(36864, total)
        for suffix, value in (
            ('EXTERNAL_ARCHIVE_VERIFIED', 'TRUE'), ('CAPTURE_ID', '1U'), ('TEST_ID', '57U'),
            ('LUN', '4U'), ('BLOCK_BYTES', '4096U'), ('CAPACITY_BYTES', '1551892480ULL'),
            ('FIRST_LBA', '375040ULL'), ('LAST_LBA', '378623ULL'), ('GAP_BYTES', '14680064ULL'),
        ):
            self.assertIn(f'#define PIANO_UFS_WRITE_BASELINE_{suffix} {value}\n', text)
        self.assertNotIn('#define PIANO_UFS_WRITE_TEST_AUTHORIZED', text)
        self.assertNotIn('private/', text)
        self.assertEqual(6, text.count('STATIC CONST UINT8'))
        self.assertEqual([], list(self.output.parent.glob('.ufs-baseline-*')))

    def test_default_output_is_generated_build_header_and_never_invoked(self):
        self.assertEqual(ROOT / 'build/ufs-write-test/PianoUfsWriteTestBaseline.h', tool.OUTPUT)
        self.assertFalse(tool.OUTPUT.is_relative_to(ROOT / 'uefi'))
        # The actual default-output prepare function and CLI are not called.

    def test_every_binary_hash_and_length_drift_is_refused(self):
        for name in tool.EXPECTED_FILES:
            original = (self.capture / name).read_bytes()
            with self.subTest(name=name, drift='hash'):
                (self.capture / name).write_bytes(self.changed(name))
                with self.assertRaisesRegex(ValueError, 'Pinned backup SHA256'):
                    self.verify()
                (self.capture / name).write_bytes(original)
            with self.subTest(name=name, drift='short'):
                (self.capture / name).write_bytes(original[:-1])
                with self.assertRaisesRegex(ValueError, 'Backup length mismatch'):
                    self.verify()
                (self.capture / name).write_bytes(original)

    def test_manifest_cannot_approve_changed_binary(self):
        baseline = self.manifest()
        for name in tool.EXPECTED_FILES:
            original = (self.capture / name).read_bytes()
            changed = self.changed(name)
            (self.capture / name).write_bytes(changed)
            manifest = copy.deepcopy(baseline)
            manifest['files'][name] = {'bytes': len(changed), 'sha256': hashlib.sha256(changed).hexdigest()}
            self.save_manifest(manifest)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'manifest hash/length drift'):
                self.verify()
            (self.capture / name).write_bytes(original)
        self.save_manifest(baseline)

    def test_missing_extra_or_malformed_manifest_file_records_refused(self):
        baseline = self.manifest()
        for mutation in ('missing', 'extra', 'record-field', 'numeric-type', 'sha-type'):
            manifest = copy.deepcopy(baseline)
            if mutation == 'missing':
                del manifest['files']['mbr.bin']
            elif mutation == 'extra':
                manifest['files']['other.bin'] = manifest['files']['mbr.bin']
            elif mutation == 'record-field':
                manifest['files']['mbr.bin']['verified'] = True
            elif mutation == 'numeric-type':
                manifest['files']['mbr.bin']['bytes'] = '4096'
            else:
                manifest['files']['mbr.bin']['sha256'] = None
            self.save_manifest(manifest)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.verify()

    def test_capture_geometry_and_status_metadata_drift_refused(self):
        baseline = self.manifest()
        for key, value in (
            ('status', 'WRITE_AUTHORIZED'), ('lun', 3), ('block_device', 'sda'),
            ('block_bytes', 512), ('capacity_bytes', tool.CAPACITY_BYTES + tool.BLOCK_BYTES),
            ('first_lba', tool.FIRST_LBA + 1), ('last_lba', tool.LAST_LBA - 1),
            ('original_reads_match', False), ('all_zero', False), ('active_partitions', 78),
            ('device_writes', True), ('lun', True), ('original_reads_match', 1),
        ):
            manifest = copy.deepcopy(baseline)
            manifest[key] = value
            self.save_manifest(manifest)
            with self.subTest(key=key, value=value), self.assertRaisesRegex(ValueError, 'Capture metadata drift'):
                self.verify()

    def test_primary_and_backup_gpt_manifest_fields_must_match_parsed_bytes(self):
        baseline = self.manifest()
        for side in ('primary_gpt', 'backup_gpt'):
            for key in baseline[side]:
                manifest = copy.deepcopy(baseline)
                value = manifest[side][key]
                manifest[side][key] = 'changed' if type(value) is str else value + 1
                self.save_manifest(manifest)
                with self.subTest(side=side, key=key), self.assertRaisesRegex(ValueError, 'GPT metadata drift'):
                    self.verify()

    def test_missing_and_symlinked_evidence_files_refused(self):
        path = self.capture / 'neighbor-before.bin'
        data = path.read_bytes()
        path.unlink()
        with self.assertRaisesRegex(ValueError, 'Missing regular backup file'):
            self.verify()
        alternate = self.folder / 'alternate-neighbor.bin'
        alternate.write_bytes(data)
        path.symlink_to(alternate)
        with self.assertRaisesRegex(ValueError, 'Missing regular backup file'):
            self.verify()
        path.unlink()
        path.write_bytes(data)
        manifest = self.capture / 'manifest.json'
        alternate = self.folder / 'alternate-manifest.json'
        manifest.rename(alternate)
        manifest.symlink_to(alternate)
        with self.assertRaisesRegex(ValueError, 'Missing regular manifest'):
            self.verify()

    def test_duplicate_json_metadata_is_refused(self):
        path = self.capture / 'manifest.json'
        path.write_text(path.read_text().replace('{', '{"lun":4,', 1))
        with self.assertRaisesRegex(ValueError, 'Duplicate JSON key: lun'):
            self.verify()

    def test_test57_lun4_primary_header_and_array_drift_refused(self):
        for name in ('lun-4-mbr-header.bin', 'lun-4-entries.bin'):
            path = self.test57 / name
            original = path.read_bytes()
            changed = bytearray(original)
            changed[-1] ^= 1
            path.write_bytes(changed)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'audited test57 baseline'):
                self.verify()
            path.write_bytes(original[:-1])
            with self.subTest(name=name, short=True), self.assertRaisesRegex(ValueError, 'Backup length mismatch'):
                self.verify()
            path.write_bytes(original)

    def test_test57_identity_and_lun4_metadata_drift_refused(self):
        baseline = self.manifest(self.test57)
        for key, value in (('test_id', 56), ('all_six_match', False), ('device_writes', True), ('test_id', '57')):
            manifest = copy.deepcopy(baseline)
            manifest[key] = value
            self.save_manifest(manifest, self.test57)
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'test57 metadata drift'):
                self.verify()
        for key in tool.EXPECTED_TEST57_LUN:
            manifest = copy.deepcopy(baseline)
            row = next(row for row in manifest['luns'] if row['lun'] == 4)
            value = row[key]
            row[key] = not value if type(value) is bool else 'changed' if type(value) is str else value + 1
            self.save_manifest(manifest, self.test57)
            with self.subTest(lun4_field=key), self.assertRaises(ValueError):
                self.verify()
        manifest = copy.deepcopy(baseline)
        manifest['luns'][0] = copy.deepcopy(manifest['luns'][4])
        self.save_manifest(manifest, self.test57)
        with self.assertRaisesRegex(ValueError, 'LUN identity drift'):
            self.verify()

    def test_gpt_crc_and_primary_backup_identity_rechecked_after_hashes(self):
        changed = self.changed('primary-header.bin')
        # Alter a byte covered by header CRC, rather than the zero padding.
        changed = bytearray(changed)
        changed[40] ^= 1
        with self.replace_and_repin({'primary-header.bin': bytes(changed)}):
            with self.assertRaisesRegex(ValueError, 'GPT header CRC/location mismatch'):
                self.verify()
        # Restore only the copied real primary before testing the backup.
        shutil.copyfile(tool.CAPTURE / 'primary-header.bin', self.capture / 'primary-header.bin')
        baseline = json.loads((tool.CAPTURE / 'manifest.json').read_text())
        self.save_manifest(baseline)
        backup = bytearray((self.capture / 'backup-header.bin').read_bytes())
        backup[56] ^= 1
        struct.pack_into('<I', backup, 16, 0)
        struct.pack_into('<I', backup, 16, zlib.crc32(backup[:92]))
        with self.replace_and_repin({'backup-header.bin': bytes(backup)}):
            with self.assertRaisesRegex(ValueError, 'Primary/backup GPT mismatch'):
                self.verify()

    def test_79_active_partitions_and_nonoverlap_are_rechecked(self):
        # Real active entries can be in any order; select their actual offsets.
        original = (self.capture / 'primary-entries.bin').read_bytes()
        active = [offset for offset in range(0, len(original), 128) if any(original[offset:offset + 16])]
        self.assertEqual(79, len(active))
        def erase_first(entries):
            entries[active[0]:active[0] + 16] = bytes(16)
        with self.replace_and_repin(self.changed_gpt_entries(erase_first)):
            with self.assertRaisesRegex(ValueError, 'Unexpected LUN4 active partition count'):
                self.verify()
        for name in ('primary-entries.bin', 'backup-entries.bin', 'primary-header.bin', 'backup-header.bin', 'manifest.json'):
            shutil.copyfile(tool.CAPTURE / name, self.capture / name)
        first_range = struct.unpack_from('<QQ', original, active[0] + 32)
        def overlap(entries):
            struct.pack_into('<QQ', entries, active[1] + 32, *first_range)
        with self.replace_and_repin(self.changed_gpt_entries(overlap)):
            with self.assertRaisesRegex(ValueError, 'Active GPT partitions overlap each other'):
                self.verify()

    def test_gap_occupancy_is_rechecked_independently_of_hash_pins(self):
        entries = (self.capture / 'primary-entries.bin').read_bytes()
        active = next(offset for offset in range(0, len(entries), 128) if any(entries[offset:offset + 16]))
        def occupy_gap(value):
            struct.pack_into('<QQ', value, active + 32, tool.FIRST_LBA, tool.FIRST_LBA)
        with self.replace_and_repin(self.changed_gpt_entries(occupy_gap)):
            with self.assertRaisesRegex(ValueError, 'test area overlaps an active partition'):
                self.verify()

    def test_full_gap_zero_check_and_target_and_neighbor_comparisons(self):
        with self.replace_and_repin({'gap-original.bin': self.changed('gap-original.bin')}):
            with self.assertRaisesRegex(ValueError, '14 MiB gap is not all zero'):
                self.verify()
        shutil.copyfile(tool.CAPTURE / 'gap-original.bin', self.capture / 'gap-original.bin')
        self.save_manifest(json.loads((tool.CAPTURE / 'manifest.json').read_text()))
        with self.replace_and_repin({'first-block-original.bin': self.changed('first-block-original.bin')}):
            with self.assertRaisesRegex(ValueError, 'Original block differs'):
                self.verify()
        shutil.copyfile(tool.CAPTURE / 'first-block-original.bin', self.capture / 'first-block-original.bin')
        self.save_manifest(json.loads((tool.CAPTURE / 'manifest.json').read_text()))
        for name in ('neighbor-before.bin', 'neighbor-after.bin'):
            with self.replace_and_repin({name: self.changed(name)}):
                with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'adjacent block comparison mismatch'):
                    self.verify()
            shutil.copyfile(tool.CAPTURE / name, self.capture / name)
            self.save_manifest(json.loads((tool.CAPTURE / 'manifest.json').read_text()))

    def test_changes_between_independent_pc_reads_refused(self):
        native = tool._read_file
        seen = {}
        def changing_read(path, size):
            data = native(path, size)
            key = Path(path)
            seen[key] = seen.get(key, 0) + 1
            if key == self.capture / 'neighbor-after.bin' and seen[key] == 2:
                return data[:-1] + b'\x01'
            return data
        with mock.patch.object(tool, '_read_file', side_effect=changing_read):
            with self.assertRaisesRegex(ValueError, 'PC backup changed between independent reads'):
                self.verify()

    def test_manifest_changes_during_verification_refused(self):
        native = Path.read_bytes
        calls = 0
        def changing_manifest(path):
            nonlocal calls
            value = native(path)
            if path == self.capture / 'manifest.json':
                calls += 1
                if calls == 2:
                    value += b'\n'
            return value
        with mock.patch.object(Path, 'read_bytes', changing_manifest):
            with self.assertRaisesRegex(ValueError, 'PC manifest changed during verification'):
                self.verify()

    def test_invalid_input_creates_no_output_and_preserves_existing_header(self):
        path = self.capture / 'neighbor-after.bin'
        path.write_bytes(self.changed('neighbor-after.bin'))
        with self.assertRaisesRegex(ValueError, 'Pinned backup SHA256'):
            self.generate()
        self.assertFalse(self.output.parent.exists())
        self.output.parent.mkdir()
        self.output.write_bytes(b'prior generated baseline')
        with self.assertRaisesRegex(ValueError, 'Pinned backup SHA256'):
            self.generate()
        self.assertEqual(b'prior generated baseline', self.output.read_bytes())
        self.assertEqual([self.output], list(self.output.parent.iterdir()))

    def test_output_name_symlink_and_backup_directory_protection(self):
        for path in (self.folder / 'other.h', self.capture / tool.HEADER_NAME, self.test57 / tool.HEADER_NAME):
            with self.subTest(path=path), self.assertRaises(ValueError):
                tool.prepare_ufs_write_test(self.capture, self.test57, path)
            self.assertFalse(path.exists())
        self.output.parent.mkdir()
        target = self.folder / 'preserved-original'
        target.write_bytes(b'original')
        self.output.symlink_to(target)
        with self.assertRaisesRegex(ValueError, 'must not be a symlink'):
            self.generate()
        self.assertEqual(b'original', target.read_bytes())

    def test_header_fsync_failure_cleans_temporary_and_preserves_output(self):
        self.output.parent.mkdir()
        self.output.write_bytes(b'existing output')
        with mock.patch.object(tool.os, 'fsync', side_effect=OSError('fixture fsync failure')):
            with self.assertRaisesRegex(OSError, 'fixture fsync failure'):
                self.generate()
        self.assertEqual(b'existing output', self.output.read_bytes())
        self.assertEqual([self.output], list(self.output.parent.iterdir()))

    def test_header_readback_failure_cleans_temporary_and_preserves_output(self):
        self.output.parent.mkdir()
        self.output.write_bytes(b'existing output')
        native = Path.read_bytes
        def damaged_temp(path):
            if path.name.startswith('.ufs-baseline-'):
                return b'damaged'
            return native(path)
        with mock.patch.object(Path, 'read_bytes', damaged_temp):
            with self.assertRaisesRegex(ValueError, 'Generated header independent readback mismatch'):
                self.generate()
        self.assertEqual(b'existing output', self.output.read_bytes())
        self.assertEqual([self.output], list(self.output.parent.iterdir()))


if __name__ == '__main__':
    unittest.main()
