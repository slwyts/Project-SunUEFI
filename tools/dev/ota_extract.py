#!/usr/bin/env python3
"""Extract one partition image from an Android A/B OTA payload.bin.

Reads the protobuf manifest, copies only the partition's REPLACE extents into a
sparse output, then prints the hashes of that partition. No device access and no
full-payload copy: the caller supplies an already downloaded package.
"""
import argparse
import bz2
import lzma
import hashlib
import json
from pathlib import Path
import struct
import sys
import zipfile

MAGIC = b'CrAU'
BLOCK = 4096


def varint(data, offset):
    result = shift = 0
    while True:
        byte = data[offset]
        offset += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, offset
        shift += 7


def fields(data):
    """Yield (field_number, wire_type, value) for a protobuf message."""
    offset = 0
    while offset < len(data):
        key, offset = varint(data, offset)
        number, wire = key >> 3, key & 7
        if wire == 0:
            value, offset = varint(data, offset)
        elif wire == 2:
            length, offset = varint(data, offset)
            value = data[offset:offset + length]
            offset += length
        elif wire == 5:
            value = data[offset:offset + 4]
            offset += 4
        elif wire == 1:
            value = data[offset:offset + 8]
            offset += 8
        else:
            raise ValueError('Unsupported protobuf wire type: %d' % wire)
        yield number, wire, value


def message_list(data, number):
    return [value for field, wire, value in fields(data) if field == number and wire == 2]


def message_one(data, number, default=b''):
    for field, wire, value in fields(data):
        if field == number and wire == 2:
            return value
    return default


def uint_one(data, number, default=0):
    """Read a scalar varint field; the manifest mixes varints and messages."""
    for field, wire, value in fields(data):
        if field == number and wire == 0:
            return value
    return default


def repeated_int(data, number):
    for field, wire, value in fields(data):
        if field != number:
            continue
        if wire == 0:
            yield value
        elif wire == 2:
            offset = 0
            while offset < len(value):
                item, offset = varint(value, offset)
                yield item


def read_header(archive):
    """Return (metadata, data_start) for the payload."""
    header = archive.read(24)
    if header[:4] != MAGIC:
        raise ValueError('Not an update payload: '+repr(header[:4]))
    version = struct.unpack('>Q', header[4:12])[0]
    if version != 2:
        raise ValueError('Unsupported payload version: '+str(version))
    size = struct.unpack('>Q', header[12:20])[0]
    signature_size = struct.unpack('>I', header[20:24])[0]
    if not 0 < size < 64 * 1024 * 1024:
        raise ValueError('Implausible manifest size: '+str(size))
    metadata = archive.read(size)
    data_start = 24 + size + signature_size
    archive.seek(data_start)
    return metadata, data_start


def read_manifest(archive):
    """Backward-compatible helper returning only the manifest bytes."""
    return read_header(archive)[0]


# InstallOperation.Type values from update_metadata.proto.
REPLACE, REPLACE_BZ, MOVE, BSDIFF, SOURCE_COPY, SOURCE_BSDIFF, ZERO, DISCARD, REPLACE_XZ = range(9)
SUPPORTED = {REPLACE, REPLACE_BZ, ZERO, DISCARD, REPLACE_XZ}


def partitions(manifest):
    """Yield (name, blocks, operations) with absolute destination extents."""
    for raw in message_list(manifest, 13):
        name = message_one(raw, 1).decode()
        blocks = uint_one(raw, 7) or 0
        operations = []
        for update in message_list(raw, 8):
            kind = uint_one(update, 1, default=-1)
            if kind not in SUPPORTED:
                raise ValueError('Unsupported operation type %d in partition %s' % (kind, name))
            data_offset = uint_one(update, 2)
            data_length = uint_one(update, 3)
            extents = []
            # Field 6 is dst_extents; field 8 is the SHA256 of the stored data.
            for encoded in message_list(update, 6):
                extents.append((uint_one(encoded, 1), uint_one(encoded, 2)))
            operations.append((kind, data_offset, data_length, extents))
        # This payload omits the declared size; the highest extent defines it.
        for _, _, _, extents in operations:
            for start, count in extents:
                blocks = max(blocks, start + count)
        yield name, blocks, operations


def decompress(kind, payload):
    if kind == REPLACE:
        return payload
    if kind == REPLACE_BZ:
        return bz2.decompress(payload)
    if kind == REPLACE_XZ:
        return lzma.decompress(payload)
    return None


def extract(archive, blocks, operations, destination, data_start=0):
    """Write a partition image; returns (sha256 of stored data, bytes written)."""
    digest = hashlib.sha256()
    written = 0
    destination.truncate(blocks * BLOCK)
    for kind, offset, length, extents in operations:
        stored = b''
        if kind in (REPLACE, REPLACE_BZ, REPLACE_XZ):
            archive.seek(data_start + offset)
            stored = archive.read(length)
            if len(stored) != length:
                raise ValueError('Payload shorter than its manifest claims')
            digest.update(stored)
            data = decompress(kind, stored)
        else:
            data = b'\0' * (sum(count for _, count in extents) * BLOCK)
        cursor = 0
        for start, count in extents:
            size = count * BLOCK
            piece = data[cursor:cursor + size]
            if len(piece) != size:
                raise ValueError('Operation data does not fill its extents')
            destination.seek(start * BLOCK)
            destination.write(piece)
            written += size
            cursor += size
    return digest.hexdigest(), written


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path, help='Full OTA zip containing payload.bin')
    parser.add_argument('--list', action='store_true', help='List partitions and sizes')
    parser.add_argument('--partition', action='append', default=[], help='Extract this partition')
    parser.add_argument('--output', type=Path, help='Directory for extracted images')
    args = parser.parse_args(argv)

    with zipfile.ZipFile(args.package) as package:
        if 'payload.bin' not in package.namelist():
            raise ValueError('Package has no payload.bin (not a full A/B OTA)')
        with package.open('payload.bin') as archive:
            manifest, data_start = read_header(archive)
            table = {}
            for name, blocks, operations in partitions(manifest):
                table[name] = (blocks, operations)
            if args.list or not args.partition:
                for name in sorted(table):
                    blocks, operations = table[name]
                    data = sum(length for kind, _, length, _ in operations
                               if kind in (REPLACE, REPLACE_BZ, REPLACE_XZ))
                    print(f'{name:24} {blocks * BLOCK:>14} bytes   stored data {data:>12}')
                return 0
            if not args.output:
                parser.error('--output is required with --partition')
            args.output.mkdir(parents=True, exist_ok=True)
            results = {}
            for name in args.partition:
                if name not in table:
                    raise SystemExit('Unknown partition: '+name)
                destination = args.output / (name + '.img')
                with destination.open('wb') as stream:
                    digest, size = extract(archive, *table[name], stream, data_start)
                results[name] = {'path': str(destination), 'bytes': size,
                                 'sha256_of_operations': digest}
                print(json.dumps({name: results[name]}, indent=2), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
