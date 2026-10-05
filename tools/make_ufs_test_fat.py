#!/usr/bin/env python3
"""Create a local FAT12 image for the bounded UFS test window; no device access."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BLOCK_BYTES, BLOCKS = 4096, 3584


def inspect(data):
    if len(data) != BLOCK_BYTES * BLOCKS or data[510:512] != b'\x55\xaa':
        raise ValueError('Wrong image geometry or boot signature')
    sector, cluster, reserved, fats, entries, total16, media, fat_sectors = struct.unpack_from('<HBHBHHBH', data, 11)
    if sector != BLOCK_BYTES or cluster != 1:
        raise ValueError('Expected 4096-byte sectors and one sector per cluster')
    total32 = struct.unpack_from('<I', data, 32)[0]
    hidden = struct.unpack_from('<I', data, 28)[0]
    root_sectors = (entries * 32 + sector - 1) // sector
    first_data = reserved + fats * fat_sectors + root_sectors
    clusters = ((total16 or total32) - first_data) // cluster if cluster else 0
    if (sector, cluster, fats, hidden, total16 or total32) != (4096, 1, 2, 0, BLOCKS) or not (
            reserved > 0 and entries >= 128 and 0 < clusters < 4085 and
            fat_sectors * sector * 2 // 3 >= clusters + 2 and data[54:62] == b'FAT12   '):
        raise ValueError('Expected a bounded FAT12 superfloppy with 4 KiB sectors')
    fat_offset = reserved * sector
    fat_bytes = fat_sectors * sector
    if data[fat_offset:fat_offset+fat_bytes] != data[fat_offset+fat_bytes:fat_offset+fat_bytes*2]:
        raise ValueError('FAT copies differ')
    if data[fat_offset:fat_offset+3] != bytes((media, 0xff, 0xff)) or any(data[fat_offset+3:fat_offset+fat_bytes]):
        raise ValueError('Expected an empty FAT allocation table')
    if any(data[first_data * sector:]):
        raise ValueError('Expected empty zeroed data clusters')
    extents = []
    for lba in range(BLOCKS):
        block = data[lba*sector:(lba+1)*sector]
        if any(block):
            extents.append({'logical_lba': lba, 'physical_lba': 375040 + lba,
                            'bytes': sector, 'sha256': hashlib.sha256(block).hexdigest()})
    if not extents or len(extents) > 64 or any(item['logical_lba'] >= first_data for item in extents):
        raise ValueError('Unexpected nonzero format extents')
    return {'sector_bytes': sector, 'sectors': BLOCKS, 'cluster_sectors': cluster,
            'reserved_sectors': reserved, 'fat_copies': fats, 'fat_sectors': fat_sectors,
            'root_entries': entries, 'first_data_sector': first_data, 'clusters': clusters,
            'nonzero_blocks': extents}


def main():
    out = ROOT / 'artifacts/ufs/test-window'
    if out.exists() or out.is_symlink():
        raise SystemExit('Local test-window output already exists; refusing overwrite')
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.fat12-', dir=out.parent) as temporary:
        stage = Path(temporary)
        image = stage / 'fat12.img'
        with image.open('xb') as stream:
            stream.truncate(BLOCK_BYTES * BLOCKS)
        arguments = ['mkfs.fat', '--invariant', '-F', '12', '-S', '4096', '-s', '1',
                     '-f', '2', '-R', '1', '-r', '128', '-h', '0', '-n', 'PIANO_TEST', str(image)]
        process = subprocess.run(arguments, capture_output=True, text=True, timeout=30, check=True)
        data = image.read_bytes()
        geometry = inspect(data)
        metadata = {'status': 'HOST_IMAGE_ONLY_NO_DEVICE_FORMAT', 'format': 'FAT12-superfloppy',
                    'lun': 4, 'first_physical_lba': 375040, 'last_physical_lba': 378623,
                    'image': {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()},
                    'geometry': geometry, 'mkfs_arguments': arguments[:-1] + ['fat12.img'],
                    'mkfs_output': process.stdout + process.stderr,
                    'device_operations': False,
                    'limitations': ['PC image creation only; UEFI formatting/file writes not verified.',
                                    'Only an explicitly gated bounded provider may use these extents.',
                                    'The experiment must restore and independently verify the whole original gap.']}
        (stage / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
        stage.rename(out)
    print(json.dumps({'path': str(out/'fat12.img'), 'image': metadata['image'],
                      'geometry': geometry, 'device_operations': False}, indent=2))


if __name__ == '__main__':
    main()
