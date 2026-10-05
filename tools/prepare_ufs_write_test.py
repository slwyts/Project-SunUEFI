#!/usr/bin/env python3
"""Verify the fixed PC capture and emit a host-only UFS test baseline header.

This never contacts a device, prepares a firmware profile, or authorizes WRITE.
ExternalArchiveVerified refers only to the independently read-back PC archive;
the live transport must still perform all authorization and runtime gates.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parent.parent
CAPTURE = ROOT / 'private/captures/ufs-test-area-1'
TEST57 = ROOT / 'private/analysis/ufs-gpt-android-test-57'
HEADER_NAME = 'PianoUfsWriteTestBaseline.h'
OUTPUT = ROOT / 'build/ufs-write-test' / HEADER_NAME
LUN = 4
BLOCK_BYTES = 4096
CAPACITY_BYTES = 1551892480
FIRST_LBA = 375040
LAST_LBA = 378623
GAP_BYTES = 14 * 1024 * 1024
LAST_DISK_LBA = CAPACITY_BYTES // BLOCK_BYTES - 1

# These pins are independent of the capture manifest. Updating a manifest to
# describe altered data must never make an altered archive eligible.
EXPECTED_FILES = {
    'mbr.bin': (4096, 'e53182c76db6385b5bb8e9235f35c193fa0dd2fd2941a0fd86a641dec8db9920'),
    'primary-header.bin': (4096, '22c6c0349cd190bdc6f5e13ac3cba0f337f58729144d90b4281d90530184e565'),
    'primary-entries.bin': (12288, 'fd10bb4f7142eccb28a6acc5fc6e928c599883fb44a0338b521473c5669a3506'),
    'backup-header.bin': (4096, 'b76a916e296fefa2048c19f38f2ae86da4f01b61f46bac739eb65baf75a1ac8f'),
    'backup-entries.bin': (12288, 'fd10bb4f7142eccb28a6acc5fc6e928c599883fb44a0338b521473c5669a3506'),
    'gap-original.bin': (14680064, 'e86bae8c0598c4ff83c695f467daa4a1e8fa01d57f9140372993366204022a4d'),
    'first-block-original.bin': (4096, 'ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7'),
    'neighbor-before.bin': (4096, 'ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7'),
    'neighbor-after.bin': (4096, 'ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7'),
}
EXPECTED_METADATA = {
    'status': 'READ_ONLY_BACKUP_NO_WRITE_TEST', 'lun': LUN,
    'block_device': 'sde', 'block_bytes': BLOCK_BYTES,
    'capacity_bytes': CAPACITY_BYTES, 'first_lba': FIRST_LBA,
    'last_lba': LAST_LBA, 'original_reads_match': True,
    'all_zero': True, 'active_partitions': 79, 'device_writes': False,
}
EXPECTED_TEST57_LUN = {
    'lun': LUN, 'block_device': 'sde', 'capacity_bytes': CAPACITY_BYTES,
    'logical_block_bytes': BLOCK_BYTES, 'header_crc': '115C4E81',
    'array_crc': '0540A9DA', 'entries': 96, 'active_partitions': 79,
    'matches_uefi': True, 'device_writes': False,
}

# Reuse the reviewed read-only GPT parser without running the capture CLI.
_spec = importlib.util.spec_from_file_location(
    '_piano_read_only_capture', Path(__file__).with_name('capture_ufs_test_area.py'))
_gpt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_gpt)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f'Duplicate JSON key: {key}')
        result[key] = value
    return result


def _read_file(path, size):
    path = Path(path)
    require(not path.is_symlink() and path.is_file(), f'Missing regular backup file: {path.name}')
    require(path.stat().st_size == size, f'Backup length mismatch: {path.name}')
    data = path.read_bytes()
    require(len(data) == size, f'Backup short read: {path.name}')
    return data


def _read_manifest(path):
    path = Path(path)
    require(not path.is_symlink() and path.is_file(), f'Missing regular manifest: {path}')
    require(0 < path.stat().st_size <= 65536, f'Manifest size invalid: {path}')
    raw = path.read_bytes()
    value = json.loads(raw, object_pairs_hook=_unique_object)
    require(type(value) is dict, f'Manifest must be an object: {path}')
    return value, raw


def _same_typed(actual, expected):
    """JSON booleans cannot stand in for numeric geometry fields."""
    if type(actual) is not type(expected):
        return False
    if type(expected) is dict:
        return actual.keys() == expected.keys() and all(
            _same_typed(actual[key], value) for key, value in expected.items())
    return actual == expected


def verify_capture(capture_dir=None, test57_dir=None):
    """Read-only verification; return checked bytes, never runtime authorization.

    Optional directories support copied PC fixtures and an explicit local archive
    location. The bytes, geometry, capture ID and test57 evidence remain pinned.
    """
    capture_dir = Path(capture_dir) if capture_dir is not None else CAPTURE
    test57_dir = Path(test57_dir) if test57_dir is not None else TEST57
    manifest, manifest_raw = _read_manifest(capture_dir / 'manifest.json')
    for key, expected in EXPECTED_METADATA.items():
        require(_same_typed(manifest.get(key), expected), f'Capture metadata drift: {key}')
    records = manifest.get('files')
    require(type(records) is dict and records.keys() == EXPECTED_FILES.keys(),
            'Capture must describe exactly the nine pinned backup files')
    blobs = {}
    for name, (size, digest) in EXPECTED_FILES.items():
        require(_same_typed(records[name], {'bytes': size, 'sha256': digest}),
                f'Capture manifest hash/length drift: {name}')
        data = _read_file(capture_dir / name, size)
        require(sha256(data) == digest, f'Pinned backup SHA256 mismatch: {name}')
        blobs[name] = data

    primary = _gpt.header(blobs['primary-header.bin'], 1, LAST_DISK_LBA)
    backup = _gpt.header(blobs['backup-header.bin'], LAST_DISK_LBA, LAST_DISK_LBA)
    _gpt.validate_gap(primary, backup, blobs['primary-entries.bin'], blobs['backup-entries.bin'])
    require(primary['entries'] == 96 and primary['width'] == 128 and
            primary['entry_lba'] == 2 and backup['entry_lba'] == 378873,
            'Pinned GPT array geometry drift')
    require(_same_typed(manifest.get('primary_gpt'), primary) and
            _same_typed(manifest.get('backup_gpt'), backup), 'Capture GPT metadata drift')
    gap = blobs['gap-original.bin']
    require(len(gap) == GAP_BYTES and len(gap) == (LAST_LBA - FIRST_LBA + 1) * BLOCK_BYTES,
            'Pinned gap length mismatch')
    require(not any(gap), 'Captured 14 MiB gap is not all zero')
    original = blobs['first-block-original.bin']
    require(original == gap[:BLOCK_BYTES], 'Original block differs from start of captured gap')
    require(blobs['neighbor-before.bin'] == original and blobs['neighbor-after.bin'] == original,
            'Pinned adjacent block comparison mismatch')

    test_manifest, test_raw = _read_manifest(test57_dir / 'manifest.json')
    for key, expected in (('test_id', 57), ('all_six_match', True), ('device_writes', False)):
        require(_same_typed(test_manifest.get(key), expected), f'test57 metadata drift: {key}')
    luns = test_manifest.get('luns')
    require(type(luns) is list and len(luns) == 6 and all(type(item) is dict for item in luns),
            'test57 six-LUN evidence absent')
    require(all(type(item.get('lun')) is int for item in luns) and
            {item['lun'] for item in luns} == set(range(6)), 'test57 LUN identity drift')
    lun4 = next(item for item in luns if item['lun'] == LUN)
    require(_same_typed(lun4, EXPECTED_TEST57_LUN), 'test57 LUN4 metadata drift')
    test_header = _read_file(test57_dir / 'lun-4-mbr-header.bin', 2 * BLOCK_BYTES)
    test_entries = _read_file(test57_dir / 'lun-4-entries.bin', 12288)
    require(test_header == blobs['mbr.bin'] + blobs['primary-header.bin'] and
            test_entries == blobs['primary-entries.bin'], 'Capture differs from audited test57 baseline')

    # A second open/read of every archive file detects changes during validation.
    # This is PC readback, never an assertion about storage currently on a phone.
    for name, data in blobs.items():
        require(_read_file(capture_dir / name, len(data)) == data,
                f'PC backup changed between independent reads: {name}')
    require(_read_file(test57_dir / 'lun-4-mbr-header.bin', len(test_header)) == test_header and
            _read_file(test57_dir / 'lun-4-entries.bin', len(test_entries)) == test_entries,
            'test57 evidence changed between independent reads')
    require((capture_dir / 'manifest.json').read_bytes() == manifest_raw and
            (test57_dir / 'manifest.json').read_bytes() == test_raw,
            'PC manifest changed during verification')
    return blobs


def _c_array(symbol, data):
    rows = [f'STATIC CONST UINT8 {symbol}[{len(data)}] = {{']
    rows.extend('  ' + ', '.join(f'0x{byte:02X}' for byte in data[offset:offset + 16]) + ','
                for offset in range(0, len(data), 16))
    rows.append('};')
    return '\n'.join(rows)


def _render_header(blobs):
    parts = [
        '// Generated only after fixed capture1 and test57 PC archive verification.',
        '// SPDX-License-Identifier: BSD-2-Clause-Patent',
        '// PC attestation does not authorize WRITE or satisfy live capability/GPT gates.',
        '#ifndef PIANO_UFS_WRITE_TEST_BASELINE_H_',
        '#define PIANO_UFS_WRITE_TEST_BASELINE_H_',
        '#include <Uefi.h>',
        '',
        '#define PIANO_UFS_WRITE_BASELINE_EXTERNAL_ARCHIVE_VERIFIED TRUE',
        '#define PIANO_UFS_WRITE_BASELINE_CAPTURE_ID 1U',
        '#define PIANO_UFS_WRITE_BASELINE_TEST_ID 57U',
        '#define PIANO_UFS_WRITE_BASELINE_LUN 4U',
        '#define PIANO_UFS_WRITE_BASELINE_BLOCK_BYTES 4096U',
        '#define PIANO_UFS_WRITE_BASELINE_CAPACITY_BYTES 1551892480ULL',
        '#define PIANO_UFS_WRITE_BASELINE_FIRST_LBA 375040ULL',
        '#define PIANO_UFS_WRITE_BASELINE_LAST_LBA 378623ULL',
        '#define PIANO_UFS_WRITE_BASELINE_GAP_BYTES 14680064ULL',
    ]
    for symbol, name in (
        ('mPianoUfsWriteTestPrimaryHeader', 'primary-header.bin'),
        ('mPianoUfsWriteTestPrimaryEntries', 'primary-entries.bin'),
        ('mPianoUfsWriteTestBackupHeader', 'backup-header.bin'),
        ('mPianoUfsWriteTestBackupEntries', 'backup-entries.bin'),
        ('mPianoUfsWriteTestOriginalBlock', 'first-block-original.bin'),
    ):
        parts.extend(('', _c_array(symbol, blobs[name])))
    parts.extend(('', _c_array('mPianoUfsWriteTestGapSha256',
                              hashlib.sha256(blobs['gap-original.bin']).digest()),
                  '', '#endif // PIANO_UFS_WRITE_TEST_BASELINE_H_', ''))
    return '\n'.join(parts).encode('ascii')


def prepare_ufs_write_test(capture_dir=None, test57_dir=None, output=None):
    """Verify first, then atomically write the named header; return its Path."""
    path = Path(output) if output is not None else OUTPUT
    require(path.name == HEADER_NAME, f'Output must be named {HEADER_NAME}')
    require(not path.is_symlink(), 'Generated header output must not be a symlink')
    path = path.resolve()
    for source in (Path(capture_dir) if capture_dir is not None else CAPTURE,
                   Path(test57_dir) if test57_dir is not None else TEST57):
        require(not path.is_relative_to(source.resolve()), 'Output must be outside backup evidence directories')
    blobs = verify_capture(capture_dir, test57_dir)
    data = _render_header(blobs)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.ufs-baseline-', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        require(temporary.read_bytes() == data, 'Generated header independent readback mismatch')
        os.replace(temporary, path)
        temporary = None
        descriptor = os.open(path.parent, os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        require(path.read_bytes() == data, 'Generated header final readback mismatch')
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, default=CAPTURE,
                        help='Local capture1 archive; exact pinned contents remain required')
    parser.add_argument('--test57', type=Path, default=TEST57,
                        help='Local audited test57 evidence; exact pinned LUN4 remains required')
    parser.add_argument('--output', type=Path, default=OUTPUT,
                        help=f'Host-only header path, named {HEADER_NAME} (default: %(default)s)')
    args = parser.parse_args()
    try:
        path = prepare_ufs_write_test(args.capture, args.test57, args.output)
    except (ValueError, OSError) as error:
        parser.exit(1, f'PC UFS write-test attestation refused: {error}\n')
    print(f'PC_ARCHIVE_VERIFIED_ONLY: {path}')


if __name__ == '__main__':
    main()
