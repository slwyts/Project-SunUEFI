#!/usr/bin/env python3
"""Seal existing fixed-block UFS write/restore evidence; never contacts a device."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from prepare_ufs_write_test import EXPECTED_FILES, ROOT, verify_capture


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def unique_line(log, prefix):
    lines = [line for line in log.splitlines() if line.startswith(prefix + ' ')]
    require(len(lines) == 1, 'Missing or duplicate ' + prefix)
    return lines[0]


def fields(line):
    return dict(re.findall(r'(\w+)=([^ ]+)', line))


def check_report(log):
    begin = fields(unique_line(log, 'SUNUEFI_UFS_WRITE_REPORT_BEGIN'))
    expected = dict(mode='restore-test', started='1', returned='1', executing='0',
                    lun='4', lba='375040', bytes='4096', write_doorbells='2',
                    sync_doorbells='2')
    require(begin == expected, 'Wrong transaction geometry or execution counts')
    result = fields(unique_line(log, 'SUNUEFI_UFS_WRITE_RESULT'))
    expected = dict(outcome='1', gate='Success', first_failure='Success',
                    final='Success', attempts='2', test_match='1', restore_match='1',
                    restored_verified='1', data_unchanged='1', safe_to_continue='1',
                    requires_recovery='0', quarantined='0')
    require(result == expected, 'Transaction did not complete verified restoration')
    attempts = [fields(line) for line in log.splitlines()
                if line.startswith('SUNUEFI_UFS_WRITE_ATTEMPT ')]
    require(len(attempts) == 2, 'Wrong attempt ledger count')
    for index, item in enumerate(attempts):
        require(item == dict(id=str(index), attempted='1', returned='1',
                            restore=str(index), lun='4', lba='375040', bytes='4096',
                            transferred='4096', status='Success'), 'Wrong attempt ledger')
    hashes = [fields(line) for line in log.splitlines()
              if line.startswith('SUNUEFI_UFS_WRITE_HASH ')]
    require(len(hashes) == 4 and [row.get('offset') for row in hashes] == ['0', '8', '16', '24'],
            'Incomplete or reordered hash report')
    for row in hashes:
        require(set(row) == {'offset', 'test', 'restore'} and
                all(re.fullmatch('[0-9A-F]{16}', row[key]) for key in ('test', 'restore')),
                'Malformed transaction hash')
    test_hash = ''.join(row['test'] for row in hashes).lower()
    restore_hash = ''.join(row['restore'] for row in hashes).lower()
    require(restore_hash == EXPECTED_FILES['first-block-original.bin'][1] and
            test_hash != restore_hash, 'Restored hash or distinct test pattern invalid')
    require(fields(unique_line(log, 'SUNUEFI_UFS_WRITE_REPORT_END')) ==
            dict(boot_blocked='1', complete_os_handoff_verified='0'), 'Unexpected OS handoff')
    return test_hash, restore_hash


def record(test_id, post_capture_id, root=ROOT):
    require(type(test_id) is int and test_id > 0 and
            type(post_capture_id) is int and post_capture_id > 1, 'Invalid evidence IDs')
    root = Path(root)
    baseline = verify_capture(root / 'private/captures/ufs-test-area-1',
                              root / 'private/analysis/ufs-gpt-android-test-57')
    post_dir = root / f'private/captures/ufs-test-area-{post_capture_id}'
    after = verify_capture(post_dir, root / 'private/analysis/ufs-gpt-android-test-57')
    require(all(after[name] == original for name, original in baseline.items()),
            'Post-Android bytes differ from pre-write backup')
    private = root / 'private/analysis'
    boot_path = private / f'stage0-test-{test_id}.json'
    boot = json.loads(boot_path.read_text())
    require(boot.get('profile') == 'gui' and boot.get('flash_commands_performed') is False and
            boot.get('fastboot_boot', {}).get('exit_code') == 0, 'Not a successful RAM-only boot')
    archive = root / f'artifacts/tests/stage0-test-{test_id}'
    image_path = archive / 'piano-stage0-UNTESTED.img'
    image_sha = digest(image_path.read_bytes())
    build = json.loads((archive / 'manifest.json').read_text())
    require(image_sha == boot['image_sha256'] ==
            build['files'][image_path.name]['sha256'], 'Archived boot image provenance mismatch')
    log_path = private / f'ramlog-test-{test_id}/uefi.txt'
    log_raw = log_path.read_bytes()
    test_sha, restore_sha = check_report(log_raw.decode('utf-8', errors='strict'))
    partitions_path = private / f'partition-verification-test-{test_id}.json'
    partitions = json.loads(partitions_path.read_text())
    require(partitions.get('test_id') == test_id and partitions.get('all_26_match') is True and
            len(partitions.get('partitions', {})) == 26 and all(
                row.get('matches') is True and row.get('expected') == row.get('actual')
                for row in partitions['partitions'].values()), 'Boot partition verification failed')
    props = partitions.get('properties', {})
    require(props.get('sys.boot_completed') == '1' and props.get('ro.product.device') == 'piano' and
            props.get('ro.boot.slot_suffix') == '_a' and
            partitions.get('root_status', '').startswith('uid=0(root)'), 'Android did not restore normally')
    evidence_paths = [boot_path, archive / 'manifest.json', log_path, partitions_path,
                      root / 'private/captures/ufs-test-area-1/manifest.json', post_dir / 'manifest.json']
    return dict(status='VERIFIED_FIXED_BLOCK_FUA_WRITE_AND_RESTORE', test_id=test_id,
                post_capture_id=post_capture_id, lun=4, lba=375040, block_bytes=4096,
                write_doorbells=2, sync_doorbells=2, test_pattern_sha256=test_sha,
                restored_block_sha256=restore_sha, boot_image_sha256=image_sha,
                build_id=build['build_id'], full_gap_bytes=14680064,
                post_android_all_nine_blobs_byte_equal=True, all_26_boot_partitions_match=True,
                compared_files={name: {'bytes': len(data), 'sha256': digest(data)}
                                for name, data in after.items()},
                evidence_sha256={str(path.relative_to(root)): digest(path.read_bytes())
                                 for path in evidence_paths},
                limitations=['Fixed one-block experiment; general writable BlockIO not verified.',
                             'GPT gap remains unreserved; GPT was not modified.',
                             'FUA/sync completed and reboot readback matched; sudden power loss not tested.',
                             'No OS handoff or persistent file-system write validation.'])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--test-id', type=int, required=True)
    ap.add_argument('--post-capture-id', type=int, required=True)
    args = ap.parse_args()
    result = record(args.test_id, args.post_capture_id)
    target = ROOT / f'artifacts/ufs/write-validation-test-{args.test_id}.json'
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open('x') as stream:
        stream.write(json.dumps(result, indent=2) + '\n')
    print(target)


if __name__ == '__main__':
    main()
