#!/usr/bin/env python3
"""Read pinned Env machine code and test95 snapshots; never execute a provider."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]
ENV_SHA = '593d9e766c0070e01d4c6684b7bb8050f32f77ea3ca300c6a110ad3903de17e3'
RAM_SHA = '16aed6a815309af4109f34000f43de0058d310e293b2aecc29c6d4d9a7ed828c'
SIII_SHA = '313b218fd801ca2d9cba0450e8e4b97c046ef77443e24909c0d66234959053bc'
OPCODES = {
    0x8b58: 0x7100051f,  # version == 1 selects the separate 64-byte path
    0x8ba8: 0x9100c2bb,  # cursor = payload + 0x30 = entry + 0x18
    0x8bc0: 0xb9401776,  # type = entry + 0x2c
    0x8bc4: 0xf85f8377,  # base = entry + 0x10
    0x8bc8: 0x710006df,  # type == 1
    0x8bd0: 0xb9400f68,  # category = entry + 0x24
    0x8bd4: 0x7100391f,  # category == 14
    0x8be8: 0xf9401776,  # current length = entry + 0x40
    0x8c14: 0xa9025937,  # cache base/current at +0x20/+0x28
    0x8c3c: 0x510016c8,  # preloaded type - 5
    0x8c40: 0x7100111f,  # unsigned difference <= 4: type5..9
    0x8c6c: 0xf940037c,  # preloaded length = entry + 0x18
    0x8ccc: 0x9101237b,  # next entry +0x48
    0x233c: 0xb9400289,  # public bank count pointer is UINT32
    0x23bc: 0xf801054b,  # bank public output stride16
    0x2450: 0xf94002a8,  # public preloaded count pointer is UINT64
    0x24e0: 0xb9401d2b,  # preloaded public type from cache+0x44
    0x24ec: 0x9100614a,  # preloaded public output stride24
    0x8738: 0xf844856c,  # total-like method sums entry+0x18 at stride72
}


def pinned(path, digest):
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != digest:
        raise ValueError('Pinned evidence changed: ' + str(path))
    return raw


def analyze():
    native = ROOT / 'upstream/Mu-Silicium/Binaries/piano/Stage0/EnvDxeEnhanced/EnvDxeEnhanced.efi'
    image = pinned(native, ENV_SHA)
    pe = struct.unpack_from('<I', image, 0x3c)[0]
    count, optional = struct.unpack_from('<H', image, pe + 6)[0], struct.unpack_from('<H', image, pe + 20)[0]
    for i in range(count):
        row = pe + 24 + optional + 40 * i
        _, rva, _, raw_offset = struct.unpack_from('<4I', image, row + 8)
        if rva != raw_offset:
            raise ValueError('RVA is no longer the pinned PE file offset')
    table = struct.unpack_from('<7Q', image, 0x102e0)
    if table != (0x10002, 0x2268, 0x228c, 0x22f4, 0x22a8, 0x23d4, 0x2420):
        raise ValueError('Native protocol table changed')
    for rva, word in OPCODES.items():
        if struct.unpack_from('<I', image, rva)[0] != word:
            raise ValueError(f'Native instruction changed at RVA {rva:X}')
    capture = ROOT / 'private/analysis/usb-live-test95'
    raw = pinned(capture / 'ram402.bin', RAM_SHA)
    siii = pinned(capture / 'siii.bin', SIII_SHA)
    if len(raw) != 2328 or zlib.crc32(raw) != 0x7c271814:
        raise ValueError('RAM402 length/CRC mismatch')
    if len(siii) != 20 or zlib.crc32(siii) != 0x2776a43d:
        raise ValueError('SIII length/CRC mismatch')
    header = struct.unpack_from('<6I', raw)
    if header != (0x9da5e0a8, 0xaf9ec4e2, 3, 0, 15, 0):
        raise ValueError('Captured RAM header changed')
    records = []
    for i in range(header[4]):
        at = 24 + 72 * i
        base, declared, current = (struct.unpack_from('<Q', raw, at + off)[0] for off in (16, 24, 64))
        category, kind = (struct.unpack_from('<I', raw, at + off)[0] for off in (36, 44))
        records.append({'source_index': i, 'base': base, 'declared_length': declared,
                        'current_length': current, 'category': category, 'raw_type': kind})
    current = [r for r in records if r['category'] == 14 and r['raw_type'] == 1]
    positive = [r for r in current if r['current_length']]
    containers = [r for r in current if r['declared_length']]
    for row in positive:
        row['container_candidates'] = [r['source_index'] for r in containers
            if r['base'] <= row['base'] and row['base'] + row['current_length'] <= r['base'] + r['declared_length']]
    overlapping = [(a['source_index'], b['source_index']) for i, a in enumerate(positive) for b in positive[i + 1:]
        if a['base'] < b['base'] + b['current_length'] and b['base'] < a['base'] + a['current_length']]
    return {
        'scope': 'STATIC_MACHINE_CODE_AND_CAPTURED_CPU_BYTES_ONLY',
        'native_sha256': ENV_SHA, 'ram_sha256': RAM_SHA, 'siii_sha256': SIII_SHA,
        'instruction_rvas': [f'{rva:04X}' for rva in OPCODES],
        'native_vtable': [f'{value:X}' for value in table],
        'ram_capacity': (len(raw) - 24) // 72, 'raw_count': header[4],
        'unused_records_zero': not any(raw[24 + 72 * header[4]:]),
        'native_current_count': len(current), 'positive_current_count': len(positive),
        'empty_current_indices': [r['source_index'] for r in current if not r['current_length']],
        'current_overlap_pairs': overlapping,
        'declared_container_field_sum': f"{sum(r['declared_length'] for r in containers):X}",
        'current_span_sum': f"{sum(r['current_length'] for r in positive):X}",
        'native_total_like_field_sum_all_records': f"{sum(r['declared_length'] for r in records):X}",
        'preloaded_indices': [r['source_index'] for r in records if r['category'] == 14 and 5 <= r['raw_type'] <= 9],
        'unknown_category14_indices': [r['source_index'] for r in records if r['category'] == 14 and r['raw_type'] != 1 and not 5 <= r['raw_type'] <= 9],
        'other_category_indices': [r['source_index'] for r in records if r['category'] != 14],
        'records': records,
        'siii_fields': dict(zip(('signature', 'smem_bytes', 'smem_base', 'item_count', 'tlv_count'), struct.unpack('<IIQHH', siii))),
        'authority': 'NONE; provider output and report booleans are not memory ownership or allocation permission',
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disassemble', action='store_true', help='also print only the bounded audited instruction ranges')
    args = parser.parse_args()
    print(json.dumps(analyze(), indent=2))
    if args.disassemble:
        native = ROOT / 'upstream/Mu-Silicium/Binaries/piano/Stage0/EnvDxeEnhanced/EnvDxeEnhanced.efi'
        for start, stop in ((0x22f4, 0x251c), (0x8638, 0x8880), (0x8a74, 0x8cd8)):
            subprocess.run([str(ROOT / 'build/host-tools/usr/bin/llvm-objdump'), '-d',
                            f'--start-address={start}', f'--stop-address={stop}', str(native)], check=True)


if __name__ == '__main__':
    main()
