#!/usr/bin/env python3
"""Export Android sparse images for standard Fastboot from a completed bundle."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

MIB = 1024 * 1024
MAGIC = 0xed26ff3a
RAW, FILL = 0xcac1, 0xcac2


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def convert(source, destination, block_size=4096):
    """Use FILL for zero blocks, preserving exact readback on reused partitions."""
    size = source.stat().st_size
    if not size or size % block_size or size // block_size > 0xffffffff:
        raise ValueError('Raw image must be a nonempty block-aligned file')
    chunks = blocks = 0
    expanded = hashlib.sha256()
    zero = bytes(block_size)
    with source.open('rb') as src, destination.open('xb+') as dst:
        dst.write(bytes(28))
        kind = None
        start = count = 0
        def finish():
            nonlocal chunks
            if not count:
                return
            end = dst.tell()
            payload_size = count * block_size if kind == RAW else 4
            dst.seek(start)
            dst.write(struct.pack('<HHII', kind, 0, count, 12 + payload_size))
            dst.seek(end)
            chunks += 1
        while batch := src.read(4 * MIB):
            expanded.update(batch)
            for offset in range(0, len(batch), block_size):
                data = batch[offset:offset + block_size]
                next_kind = FILL if data == zero else RAW
                # Keep each RAW chunk under Fastboot's ordinary transfer cap.
                if next_kind != kind or (kind == RAW and count * block_size >= 64 * MIB):
                    finish()
                    kind, count, start = next_kind, 0, dst.tell()
                    dst.write(bytes(12))
                    if kind == FILL:
                        dst.write(bytes(4))
                if kind == RAW:
                    dst.write(data)
                count += 1
                blocks += 1
        finish()
        dst.seek(0)
        dst.write(struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, block_size, blocks, chunks, 0))
    return {'bytes': destination.stat().st_size, 'expanded_bytes': size,
            'sha256': digest(destination), 'expanded_sha256': expanded.hexdigest(),
            'format': 'android-sparse', 'block_bytes': block_size}


def package(bundle, output):
    record = json.loads((bundle / 'manifest.json').read_text())
    if (bundle / '.incomplete').exists():
        raise ValueError('Input bundle is incomplete')
    if 'esp.img' not in record['files']:
        raise ValueError('Input bundle must include esp.img')
    names = [name for name in ('esp.img', 'root.ext4.img') if name in record['files']]
    for name in names:
        if not (bundle / name).is_file():
            raise ValueError('Missing source image declared by bundle: ' + name)
    output.mkdir(parents=True, exist_ok=False)
    result = {'schema_version': 1, 'format': 'android-sparse', 'files': {},
              'kernel_release': record['kernel_release'],
              'source_manifest_sha256': digest(bundle / 'manifest.json')}
    for name in names:
        source = bundle / name
        expected = record['files'][name]
        row = convert(source, output / name)
        if row['expanded_sha256'] != expected['sha256'] or row['expanded_bytes'] != expected['bytes']:
            raise ValueError('Source image does not match its bundle: ' + name)
        result['files'][name] = row
    (output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    (output / 'SHA256SUMS').write_text(''.join(
        row['sha256'] + '  ' + name + '\n' for name, row in result['files'].items()))
    (output / 'README.txt').write_text(
        'Android sparse images for factory Fastboot. Do not write with dd.\n'
        + ''.join('fastboot -s SERIAL -S 128M flash ' + partition + ' ' + name + '\n'
                  for name, partition in (('root.ext4.img', 'sunuefi_root'), ('esp.img', 'sunuefi_esp'))
                  if name in result['files']) +
        'SunUEFI assistant 0.5.2 and newer also accept these images directly.\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(package(args.bundle, args.output), indent=2))
