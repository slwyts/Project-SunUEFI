#!/usr/bin/env python3
"""Independently validate exact provisioning proposal bytes; never contacts a device."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import uuid

from emit_nv_seed import emit
from prepare_product_storage import (ROOT, BLOCK_BYTES, BLOCKS, FAT_FIRST, FAT_BLOCKS,
                                    NV_FIRST, NV_BLOCKS, volume_header, proposed_gpt,
                                    inspect_fat, metadata)
from prepare_ufs_write_test import verify_capture


def read(path, size=None):
    if path.is_symlink() or not path.is_file():
        raise ValueError('Expected regular proposal file: '+path.name)
    data = path.read_bytes()
    if size is not None and len(data) != size:
        raise ValueError('Unexpected proposal file size: '+path.name)
    return data


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate manifest key: '+key)
        result[key] = value
    return result


def check(directory, live_capture=None):
    directory = Path(directory)
    raw_manifest = read(directory/'manifest.json')
    manifest = json.loads(raw_manifest, object_pairs_hook=unique)
    expected = {'schema': 1, 'status': 'PC_PROPOSAL_REQUIRES_EXPLICIT_PERMANENT_RESERVATION_APPROVAL',
                'device_operations': False, 'device_writes_authorized': False,
                'gpt_entry_index': 95, 'lun': 4, 'block_bytes': 4096,
                'first_lba': 375040, 'last_lba': 378623, 'bytes': 14680064,
                'original_active_partitions': 79, 'existing_entries_byte_equal': True,
                'gpt_header_changes_only_crc_fields': True, 'mbr_changes': False}
    for key, value in expected.items():
        if type(manifest.get(key)) is not type(value) or manifest[key] != value:
            raise ValueError('Proposal manifest scope mismatch: '+key)
    identity = uuid.UUID(manifest['volume_uuid'])
    if not identity.int:
        raise ValueError('Zero volume identity')
    original = verify_capture()
    if live_capture is not None:
        if verify_capture(Path(live_capture)) != original:
            raise ValueError('Latest readonly capture differs from original backup')
    if manifest['original_files'] != {name: metadata(data) for name, data in original.items()}:
        raise ValueError('Original provenance mismatch')
    gpt = proposed_gpt(original, identity)
    expected_names = set(gpt) | {'volume-header.bin', 'PianoUEFI-storage.img', 'nv-snapshot.bin'}
    if set(manifest['proposal_files']) != expected_names:
        raise ValueError('Unexpected proposed file set')
    files = {}
    for name in expected_names:
        files[name] = read(directory/name)
        if metadata(files[name]) != manifest['proposal_files'][name]:
            raise ValueError('File hash/size mismatch: '+name)
    for name, data in gpt.items():
        if files[name] != data:
            raise ValueError('GPT change exceeds exact proposed reservation: '+name)
    header = volume_header(identity, original['primary-header.bin'][56:72])
    if files['volume-header.bin'] != header:
        raise ValueError('Immutable volume header mismatch')
    image = files['PianoUEFI-storage.img']
    if len(image) != BLOCKS*BLOCK_BYTES or image[:2*BLOCK_BYTES] != header*2:
        raise ValueError('Container bounds/header copies mismatch')
    fat = image[FAT_FIRST*BLOCK_BYTES:(FAT_FIRST+FAT_BLOCKS)*BLOCK_BYTES]
    if inspect_fat(fat) != manifest['fat']:
        raise ValueError('FAT structure differs from manifest')
    if fat != read(directory/'fat-child.img', FAT_BLOCKS*BLOCK_BYTES):
        raise ValueError('Exported FAT differs from container')
    # Regenerate from current actual C code, not from manifest assertions.
    with tempfile.TemporaryDirectory(prefix='piano-proposal-check-') as temporary:
        seed = Path(temporary)/'seed'
        emitted = emit(identity, seed)
        snapshot = read(seed/'nv-ftw.bin')
        if snapshot != files['nv-snapshot.bin']:
            raise ValueError('Standard NV seed does not match actual C backend')
        for slot in range(2):
            actual = image[NV_FIRST[slot]*BLOCK_BYTES:(NV_FIRST[slot]+NV_BLOCKS)*BLOCK_BYTES]
            if actual != read(seed/('nv-slot-a.bin' if slot == 0 else 'nv-slot-b.bin')):
                raise ValueError('NV journal wire/commit mismatch: slot'+str(slot))
        if emitted['files'] != manifest['nv']['files'] or emitted['sources'] != manifest['nv']['sources']:
            raise ValueError('NV source provenance changed; regenerate the proposal')
    # Recheck files after the expensive compilation/verification phase.
    if read(directory/'manifest.json') != raw_manifest or any(read(directory/name) != data for name, data in files.items()):
        raise ValueError('Proposal changed while being checked')
    return {'status': 'EXACT_PC_PROPOSAL_VALIDATED_NO_DEVICE_ACTION',
            'manifest_sha256': hashlib.sha256(raw_manifest).hexdigest(),
            'container': metadata(image), 'latest_capture_checked': live_capture is not None,
            'permanent_reservation_authorized': False,
            'scope': 'LUN4 gap and entry95 only; no executor or live write permission'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--proposal', type=Path, required=True)
    parser.add_argument('--readonly-capture', type=Path)
    args = parser.parse_args()
    try:
        result = check(args.proposal, args.readonly_capture)
    except (ValueError, OSError, KeyError) as error:
        parser.exit(1, str(error)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
