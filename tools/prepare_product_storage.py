#!/usr/bin/env python3
"""Prepare a PC-only permanent-storage proposal; no device executor exists here.

The original gap and both GPT copies are pinned and revalidated. Only unused
GPT entry95 and the two CRC fields change. The result requires separate human
approval and fresh device checks before any future provisioning operation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import uuid
import zlib

from prepare_ufs_write_test import ROOT, verify_capture, BLOCK_BYTES, FIRST_LBA, LAST_LBA, LAST_DISK_LBA
from capture_ufs_test_area import header as parse_gpt_header

TYPE_GUID = uuid.UUID('3e4ea305-5b3d-49a4-b3b4-5844ef513648')
ENTRY_INDEX = 95
BLOCKS = 3584
FAT_FIRST, FAT_BLOCKS = 2, 2046
NV_FIRST = (2048, 2816)
NV_BLOCKS, NV_COMMIT_BLOCK = 768, 767
SNAPSHOT_BYTES = 576 * 1024
MAGIC = b'PIANO-VOLUME-v1\0'
PARTITION_NAME = 'PianoUEFI Storage'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def metadata(data):
    return {'bytes': len(data), 'sha256': digest(data)}


def volume_header(identity, disk_guid):
    if not identity.int or len(disk_guid) != 16 or not any(disk_guid):
        raise ValueError('Nonzero volume/disk GUIDs required')
    block = bytearray(BLOCK_BYTES)
    block[:16] = MAGIC
    struct.pack_into('<IIII', block, 16, 1, 128, 0, 1)
    block[32:48], block[48:64], block[64:80] = identity.bytes_le, disk_guid, TYPE_GUID.bytes_le
    struct.pack_into('<IIQIIIIIII', block, 80, 4, BLOCK_BYTES, FIRST_LBA, BLOCKS,
                     FAT_FIRST, FAT_BLOCKS, NV_FIRST[0], NV_FIRST[1], NV_BLOCKS, ENTRY_INDEX)
    struct.pack_into('<I', block, 24, zlib.crc32(block))
    return bytes(block)


def proposed_gpt(blobs, identity):
    original = blobs['primary-entries.bin']
    if len(original) != 96 * 128 or any(original[ENTRY_INDEX * 128:(ENTRY_INDEX + 1) * 128]):
        raise ValueError('Expected unused original GPT entry95')
    entries = bytearray(original)
    entry = bytearray(128)
    entry[:16], entry[16:32] = TYPE_GUID.bytes_le, identity.bytes_le
    # NO_BLOCK_IO: expose only the bounded FAT child and private NV interface.
    struct.pack_into('<QQQ', entry, 32, FIRST_LBA, LAST_LBA, 2)
    encoded = PARTITION_NAME.encode('utf-16le')
    entry[56:56+len(encoded)] = encoded
    entries[ENTRY_INDEX * 128:(ENTRY_INDEX + 1) * 128] = entry
    if entries[:ENTRY_INDEX*128] != original[:ENTRY_INDEX*128]:
        raise ValueError('Existing GPT entries changed')
    result = {'primary-entries.bin': bytes(entries), 'backup-entries.bin': bytes(entries)}
    for name, here in (('primary-header.bin', 1), ('backup-header.bin', LAST_DISK_LBA)):
        before = blobs[name]
        block = bytearray(before)
        struct.pack_into('<I', block, 88, zlib.crc32(entries))
        struct.pack_into('<I', block, 16, 0)
        size = struct.unpack_from('<I', block, 12)[0]
        struct.pack_into('<I', block, 16, zlib.crc32(block[:size]))
        parsed = parse_gpt_header(bytes(block), here, LAST_DISK_LBA)
        if parsed['array_crc'] != zlib.crc32(entries):
            raise ValueError('Proposed GPT CRC mismatch')
        for index, (old, new) in enumerate(zip(before, block)):
            if old != new and not (16 <= index < 20 or 88 <= index < 92):
                raise ValueError('Unexpected GPT header change')
        result[name] = bytes(block)
    return result


def inspect_fat(data):
    if len(data) != FAT_BLOCKS * BLOCK_BYTES or data[510:512] != b'\x55\xaa':
        raise ValueError('Incorrect bounded FAT geometry')
    sector, cluster, reserved, copies, root_entries, total16, media, fat_sectors = struct.unpack_from('<HBHBHHBH', data, 11)
    total = total16 or struct.unpack_from('<I', data, 32)[0]
    first_data = reserved + copies * fat_sectors + (root_entries * 32 + sector - 1) // sector
    clusters = (total - first_data) // cluster if cluster else 0
    if (sector, cluster, copies, total) != (BLOCK_BYTES, 1, 2, FAT_BLOCKS) or not (
            reserved > 0 and root_entries >= 128 and 0 < clusters < 4085
            and data[54:62] == b'FAT12   '
            and fat_sectors * sector * 2 // 3 >= clusters + 2):
        raise ValueError('Invalid FAT12 parameters')
    start, length = reserved * sector, fat_sectors * sector
    if data[start:start+length] != data[start+length:start+2*length]:
        raise ValueError('FAT copies differ')
    if data[start:start+3] != bytes((media, 255, 255)) or any(data[start+3:start+length]):
        raise ValueError('Expected an empty allocation table')
    if any(data[first_data * sector:]):
        raise ValueError('Expected empty data clusters')
    return {'sector_bytes': sector, 'blocks': total, 'clusters': clusters,
            'first_data_block': first_data, 'fat_blocks': fat_sectors, 'copies': copies}


def journal_slot(snapshot, identity):
    if len(snapshot) != SNAPSHOT_BYTES:
        raise ValueError('Expected complete 576KiB NV/FTW snapshot')
    payload_crc = zlib.crc32(snapshot)
    def make(commit, header_crc=0):
        block = bytearray(BLOCK_BYTES)
        block[:16] = (b'PIANO-NVCMIT-v1' if commit else b'PIANO-NVJRNL-v1').ljust(16, b'\0')
        struct.pack_into('<IIIIIIQIIII', block, 16, 1, 128, 0, 1, 0,
                         2 if commit else 1, 1, SNAPSHOT_BYTES, payload_crc, header_crc, 0)
        block[64:80] = identity.bytes_le
        struct.pack_into('<I', block, 24, zlib.crc32(block))
        return bytes(block)
    first = make(False)
    footer = make(True, struct.unpack_from('<I', first, 24)[0])
    data = bytearray(NV_BLOCKS * BLOCK_BYTES)
    data[:BLOCK_BYTES] = first
    data[BLOCK_BYTES:BLOCK_BYTES+len(snapshot)] = snapshot
    data[NV_COMMIT_BLOCK*BLOCK_BYTES:] = footer
    return bytes(data)


def prepare(snapshot_path, output, identity=None):
    output = Path(output)
    if output.exists() or output.is_symlink():
        raise ValueError('Proposal output exists; refusing to overwrite evidence')
    if not output.resolve().is_relative_to((ROOT/'private').resolve()):
        raise ValueError('Proposal belongs in private; contains device GPT metadata')
    from emit_nv_seed import emit
    snapshot = None
    if snapshot_path is not None:
        snapshot_path = Path(snapshot_path)
        if snapshot_path.is_symlink() or not snapshot_path.is_file():
            raise ValueError('Expected a regular NV seed snapshot')
        snapshot = snapshot_path.read_bytes()
    blobs = verify_capture()
    identity = identity or uuid.uuid4()
    if not isinstance(identity, uuid.UUID) or not identity.int:
        raise ValueError('Invalid storage UUID')
    gpt = proposed_gpt(blobs, identity)
    disk_guid = blobs['primary-header.bin'][56:72]
    header = volume_header(identity, disk_guid)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.product-storage-', dir=output.parent) as temporary:
        stage = Path(temporary)
        # Generate the real standard FV/variable/FTW structures and journal with
        # the actual C backend. Validate our host packing against its output.
        seed = stage/'nv-seed'
        nv_info = emit(identity, seed)
        expected_snapshot = (seed/'nv-ftw.bin').read_bytes()
        if snapshot is not None and snapshot != expected_snapshot:
            raise ValueError('Only the actual standard empty NV seed may be provisioned')
        snapshot = expected_snapshot
        initial_slot = journal_slot(snapshot, identity)
        if initial_slot != (seed/'nv-slot-a.bin').read_bytes() or any((seed/'nv-slot-b.bin').read_bytes()):
            raise ValueError('Host journal does not match the actual C wire format')
        fat = stage/'fat-child.img'
        with fat.open('xb') as stream:
            stream.truncate(FAT_BLOCKS * BLOCK_BYTES)
        args = ['mkfs.fat', '--invariant', '-g', '1/1', '-F', '12', '-S', str(BLOCK_BYTES), '-s', '1',
                '-f', '2', '-R', '1', '-r', '128', '-h', '0', '-n', 'PIANOSTATE', str(fat)]
        subprocess.run(args, check=True, capture_output=True, text=True, timeout=30)
        fat_bytes = fat.read_bytes()
        fat_info = inspect_fat(fat_bytes)
        image = bytearray(BLOCKS * BLOCK_BYTES)
        image[:2*BLOCK_BYTES] = header * 2
        image[FAT_FIRST*BLOCK_BYTES:(FAT_FIRST+FAT_BLOCKS)*BLOCK_BYTES] = fat_bytes
        image[NV_FIRST[0]*BLOCK_BYTES:NV_FIRST[1]*BLOCK_BYTES] = initial_slot
        files = {**gpt, 'volume-header.bin': header, 'PianoUEFI-storage.img': bytes(image),
                 'nv-snapshot.bin': snapshot}
        records = {}
        for name, data in files.items():
            (stage/name).write_bytes(data)
            if (stage/name).read_bytes() != data:
                raise ValueError('PC proposal readback mismatch')
            records[name] = metadata(data)
        # Re-read source snapshots/pinned GPT after generating the proposal.
        if (snapshot_path is not None and snapshot_path.read_bytes() != snapshot) or verify_capture() != blobs:
            raise ValueError('Source changed during proposal generation')
        plan = {'schema': 1, 'status': 'PC_PROPOSAL_REQUIRES_EXPLICIT_PERMANENT_RESERVATION_APPROVAL',
                'device_operations': False, 'device_writes_authorized': False,
                'volume_uuid': str(identity), 'type_guid': str(TYPE_GUID), 'gpt_entry_index': ENTRY_INDEX,
                'lun': 4, 'block_bytes': BLOCK_BYTES, 'first_lba': FIRST_LBA, 'last_lba': LAST_LBA,
                'bytes': len(image), 'original_active_partitions': 79, 'existing_entries_byte_equal': True,
                'gpt_header_changes_only_crc_fields': True, 'mbr_changes': False,
                'original_files': {name: metadata(data) for name, data in blobs.items()},
                'proposal_files': records, 'fat': fat_info, 'nv': nv_info,
                'layout': {'headers': [0, 1], 'fat': [FAT_FIRST, FAT_BLOCKS],
                           'journal_a': [NV_FIRST[0], NV_BLOCKS], 'journal_b': [NV_FIRST[1], NV_BLOCKS]},
                'limits': ['Permanent use and GPT changes exceed restore-after-test scope.',
                           'No device executor is included; fresh live checks and approval are still required.',
                           '14MiB stores firmware state and small files, not a Linux root filesystem.',
                           'Runtime NV writes and product device acceptance are not proved.']}
        (stage/'manifest.json').write_text(json.dumps(plan, indent=2)+'\n')
        # fat-child.img is already included in the container; retain it for inspection.
        stage.rename(output)
    return plan


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nv-snapshot', type=Path,
                        help='Optional existing seed; must equal the actual C-generated empty snapshot')
    parser.add_argument('--output', type=Path, default=ROOT/'private/provisioning/piano-storage-v1')
    parser.add_argument('--volume-uuid', type=uuid.UUID)
    args = parser.parse_args()
    try:
        result = prepare(args.nv_snapshot, args.output, args.volume_uuid)
    except (ValueError, OSError, ImportError) as error:
        parser.exit(1, str(error)+'\n')
    print(json.dumps({key: result[key] for key in ('status', 'device_operations', 'volume_uuid', 'bytes')}, indent=2))


if __name__ == '__main__':
    main()
