#!/usr/bin/env python3
"""Read Piano ABL failure logs from blackbox; never write device storage.

The captured firmware uses 4-KiB headers and a bounded ring of 64-KiB UEFI
records. Validate each offset before reading. Do not collect the Android
minidump or userdata portions of blackbox.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

BLOCK = 4096
LOG_BYTES = 65536
RECORD_BYTES = BLOCK + LOG_BYTES


def require(ok, message):
    if not ok:
        raise ValueError(message)


def header(data, magic):
    require(len(data) == BLOCK and struct.unpack_from('<Q', data)[0] == magic,
            'Unsupported blackbox header magic or size')
    version, size = struct.unpack_from('<II', data, 8)
    require(version == 0x1000 and size in (0x400, BLOCK), 'Unsupported blackbox header version')


def bootfail_location(data, capacity, failure_base):
    header(data, 0x56775AF41BCDE0F0)
    # Offset 32 is the 64-bit control-content location, NOT a boot-failure
    # pointer. Its high word happened to contain stale data in an old capture.
    # This firmware's separate boot-failure base is audited from WriteUefiLog.
    location = failure_base
    require(location >= BLOCK and location % BLOCK == 0 and location + BLOCK <= capacity,
            'Boot-failure header exceeds partition')
    return location


def log_location(data, base, capacity):
    header(data, 0x627759541BCDE0F0)
    relative = struct.unpack_from('<I', data, 20)[0]
    location = base + relative
    require(relative >= BLOCK and relative % BLOCK == 0 and location + BLOCK <= capacity,
            'UEFI ring header exceeds partition')
    return location


def ring_geometry(data, base, capacity):
    header(data, 0x94682550DD25E1F0)
    count, current = struct.unpack_from('<II', data, 20)
    require(1 <= count <= 32 and current < count and base + BLOCK + count * RECORD_BYTES <= capacity,
            'Unsupported/out-of-bounds UEFI ring')
    return count, current


def collect(serial, output, failure_base=0x9A00000):
    require(re.fullmatch(r'[A-Za-z0-9_-]+', serial), 'Invalid ADB serial')
    output = Path(output)
    require(not output.exists(), 'Use a new output directory')
    adb = ['adb', '-s', serial]
    def command(text, binary=False):
        p = subprocess.run(adb + ['exec-out', "su -c '" + text + "'"],
                           capture_output=True, timeout=20)
        require(p.returncode == 0 and not p.stdout.startswith((b'su:', b'dd:', b'cat:')),
                'Rooted Android read failed: ' + p.stderr.decode(errors='replace'))
        return p.stdout if binary else p.stdout.decode().strip()
    require(subprocess.check_output(adb + ['shell', 'getprop', 'ro.product.device'], text=True).strip() == 'piano',
            'This collector supports only piano')
    capacity = int(command('blockdev --getsize64 /dev/block/by-name/blackbox'))
    def read(offset, length):
        require(offset % BLOCK == length % BLOCK == 0 and 0 <= offset < capacity and offset + length <= capacity,
                'Unaligned/out-of-bounds blackbox read')
        result = command(f'dd if=/dev/block/by-name/blackbox bs=4096 skip={offset // BLOCK} count={length // BLOCK} 2>/dev/null', True)
        require(len(result) == length, 'Short blackbox read')
        return result
    primary = read(0, BLOCK)
    failure_base = bootfail_location(primary, capacity, failure_base)
    failure = read(failure_base, BLOCK)
    ring_base = log_location(failure, failure_base, capacity)
    ring = read(ring_base, BLOCK)
    count, current = ring_geometry(ring, ring_base, capacity)
    output.mkdir(parents=True)
    entries = []
    for name, data in [('blackbox-header.bin', primary), ('failure-header.bin', failure), ('ring-header.bin', ring)]:
        (output / name).write_bytes(data)
    for index in range(count):
        offset = ring_base + BLOCK + index * RECORD_BYTES
        data = read(offset, RECORD_BYTES)
        if not any(data):
            continue
        header(data[:BLOCK], 0x768B480DD48A6592)
        boot = struct.unpack_from('<I', data, 16)[0]
        name = f'boot-{boot}-slot-{index}.bin'
        (output / name).write_bytes(data)
        text = data[BLOCK:].decode(errors='replace').replace('\0', '')
        (output / name.replace('.bin', '.txt')).write_text(text)
        errors = [line for line in text.splitlines() if
                  re.search(r'Err: line:|ERROR: BootLinux:|LoadImageAndAuth failed|AvbSlotVerify returned', line)]
        entries.append({'slot': index, 'boot_index': boot, 'offset': offset, 'file': name,
                        'sha256': hashlib.sha256(data).hexdigest(), 'errors': errors})
    result = {'device': 'piano', 'device_writes_performed': False,
              'partition_bytes': capacity, 'boot_index': struct.unpack_from('<I', primary, 16)[0],
              'ring_base': ring_base, 'ring_count': count, 'ring_current': current,
              'records': entries}
    (output / 'manifest.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--failure-base', type=lambda value: int(value, 0), default=0x9A00000,
                        help='Audited boot-failure base for this firmware; following headers are independently validated')
    args = parser.parse_args()
    try:
        result = collect(args.serial, args.output, args.failure_base)
        print(json.dumps({key: result[key] for key in ('boot_index', 'ring_count', 'ring_current', 'records')}, ensure_ascii=False, indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
