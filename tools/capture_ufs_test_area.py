#!/usr/bin/env python3
"""Read and durably back up Piano's proposed LUN4 test gap. No device writes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import zlib

LUN = 4
BLOCK = 4096
FIRST = 375040
LAST = 378623
CAPACITY = 1551892480

def sha(data):
    return hashlib.sha256(data).hexdigest()

def header(data, here, last):
    if len(data) != BLOCK or data[:8] != b'EFI PART':
        raise ValueError('Incomplete GPT header')
    revision, size, crc, reserved = struct.unpack_from('<IIII', data, 8)
    current, alternate, first, final = struct.unpack_from('<QQQQ', data, 24)
    entry_lba, count, width, array_crc = struct.unpack_from('<QIII', data, 72)
    if revision != 0x10000 or not 92 <= size <= BLOCK or reserved:
        raise ValueError('Unsupported GPT header')
    clean = bytearray(data[:size]); struct.pack_into('<I', clean, 16, 0)
    if zlib.crc32(clean) != crc or current != here or alternate != (last if here == 1 else 1):
        raise ValueError('GPT header CRC/location mismatch')
    if not 2 <= first <= final < last or not count or width < 128 or width & (width - 1) or count * width > 65536:
        raise ValueError('GPT array bounds invalid')
    array_blocks = (count * width + BLOCK - 1) // BLOCK
    if here == 1:
        valid = 2 <= entry_lba and entry_lba + array_blocks <= first
    else:
        valid = final < entry_lba and entry_lba + array_blocks <= last
    if not valid:
        raise ValueError('GPT array overlaps usable disk area')
    return dict(first=first, last=final, entry_lba=entry_lba, entries=count,
                width=width, bytes=count * width, blocks=array_blocks,
                array_crc=array_crc, header_crc=crc, disk_guid=data[56:72].hex())

def validate_gap(primary, backup, entries, other_entries):
    keys = ('first', 'last', 'entries', 'width', 'bytes', 'array_crc', 'disk_guid')
    if any(primary[k] != backup[k] for k in keys) or entries != other_entries:
        raise ValueError('Primary/backup GPT mismatch')
    if len(entries) != primary['bytes'] or zlib.crc32(entries) != primary['array_crc']:
        raise ValueError('GPT entry array CRC mismatch')
    if not primary['first'] <= FIRST <= LAST <= primary['last']:
        raise ValueError('Proposed test area outside usable GPT area')
    active = 0
    ranges = []
    for offset in range(0, len(entries), primary['width']):
        entry = entries[offset:offset + primary['width']]
        if not any(entry[:16]):
            continue
        begin, end = struct.unpack_from('<QQ', entry, 32)
        if not primary['first'] <= begin <= end <= primary['last']:
            raise ValueError('Invalid active partition range')
        if begin <= LAST and end >= FIRST:
            raise ValueError('Proposed test area overlaps an active partition')
        ranges.append((begin, end))
        active += 1
    ranges.sort()
    if any(after[0] <= before[1] for before, after in zip(ranges, ranges[1:])):
        raise ValueError('Active GPT partitions overlap each other')
    if active != 79:
        raise ValueError('Unexpected LUN4 active partition count')
    return active

def durable_save(path, data):
    with path.open('xb') as stream:
        stream.write(data); stream.flush(); os.fsync(stream.fileno())
    if path.read_bytes() != data:
        raise ValueError('External backup readback mismatch')
    return dict(bytes=len(data), sha256=sha(data))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--capture-id', type=int, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    adb = ['adb', '-s', args.serial, 'exec-out']
    def run(command, timeout=30):
        return subprocess.check_output(adb + [command], timeout=timeout)
    if run('getprop ro.product.device').strip() != b'piano' or run('getprop sys.boot_completed').strip() != b'1':
        raise SystemExit('Expected authorized Piano restored to Android')
    name = run(f"su -c 'ls /sys/class/scsi_device/0:0:0:{LUN}/device/block'").decode().strip()
    if not re.fullmatch(r'sd[a-z]+', name):
        raise SystemExit('Unexpected LUN4 device mapping')
    size = int(run(f"su -c 'cat /sys/block/{name}/size'").strip()) * 512
    logical = int(run(f"su -c 'cat /sys/block/{name}/queue/logical_block_size'").strip())
    if size != CAPACITY or logical != BLOCK:
        raise SystemExit('LUN4 capacity/block size differs from audited snapshot')
    last = size // BLOCK - 1
    def read(lba, count):
        if not 0 <= lba <= last or not 0 < count <= last + 1 - lba:
            raise ValueError('Read outside LUN')
        data = run(f"su -c 'dd if=/dev/block/{name} bs={BLOCK} skip={lba} count={count} 2>/dev/null'", 90)
        if len(data) != count * BLOCK:
            raise ValueError('Incomplete raw block read')
        return data
    first_header, backup_header = read(1, 1), read(last, 1)
    primary, backup = header(first_header, 1, last), header(backup_header, last, last)
    first_array = read(primary['entry_lba'], primary['blocks'])[:primary['bytes']]
    backup_array = read(backup['entry_lba'], backup['blocks'])[:backup['bytes']]
    active = validate_gap(primary, backup, first_array, backup_array)
    baseline = root / 'private/analysis/ufs-gpt-android-test-57'
    if (baseline / 'lun-4-entries.bin').read_bytes() != first_array or (baseline / 'lun-4-mbr-header.bin').read_bytes()[BLOCK:] != first_header:
        raise ValueError('Live GPT changed from audited test57 baseline')
    gap = read(FIRST, LAST - FIRST + 1)
    if gap != read(FIRST, LAST - FIRST + 1):
        raise ValueError('Proposed gap changed between independent reads')
    out = root / f'private/captures/ufs-test-area-{args.capture_id}'
    out.mkdir(exist_ok=False)
    blobs = {'mbr.bin': read(0, 1), 'primary-header.bin': first_header,
             'primary-entries.bin': first_array, 'backup-header.bin': backup_header,
             'backup-entries.bin': backup_array, 'gap-original.bin': gap,
             'first-block-original.bin': gap[:BLOCK],
             'neighbor-before.bin': read(FIRST - 1, 1), 'neighbor-after.bin': read(LAST + 1, 1)}
    records = {filename: durable_save(out / filename, data) for filename, data in blobs.items()}
    manifest = dict(status='READ_ONLY_BACKUP_NO_WRITE_TEST', lun=LUN, block_device=name,
                    block_bytes=BLOCK, capacity_bytes=size, first_lba=FIRST, last_lba=LAST,
                    original_reads_match=True, all_zero=not any(gap), active_partitions=active,
                    primary_gpt=primary, backup_gpt=backup, files=records, device_writes=False,
                    limitations=['GPT gap is not a persistent reservation.',
                                 'Raw vendor LBA usage and FUA/cache support still require separate checks.',
                                 'This backup tool neither installs nor authorizes a writable BlockIO provider.'])
    durable_save(out / 'manifest.json', (json.dumps(manifest, indent=2) + '\n').encode())
    descriptor = os.open(out, os.O_DIRECTORY)
    try: os.fsync(descriptor)
    finally: os.close(descriptor)
    descriptor = os.open(out.parent, os.O_DIRECTORY)
    try: os.fsync(descriptor)
    finally: os.close(descriptor)
    print(json.dumps(manifest, indent=2))

if __name__ == '__main__':
    main()
