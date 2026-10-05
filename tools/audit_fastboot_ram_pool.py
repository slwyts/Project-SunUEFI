#!/usr/bin/env python3
"""Offline DRAM candidate audit; no mapping, allocation, device or hardware I/O."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from compose_piano_dtb import read_fdt

U64 = 1 << 64
GIB = 1 << 30


def interval(base, size):
    if base < 0 or size < 0 or base >= U64 or size > U64 - base:
        raise ValueError('Invalid/overflowing address interval')
    return (base, base + size)


def merge(ranges):
    out = []
    for start, end in sorted(ranges):
        if start == end:
            continue
        if start > end:
            raise ValueError('Inverted range')
        if out and start <= out[-1][1]:
            out[-1] = (out[-1][0], max(end, out[-1][1]))
        else:
            out.append((start, end))
    return out


def subtract(ranges, excluded):
    out = []
    for start, end in merge(ranges):
        cursor = start
        for low, high in merge(excluded):
            if high <= cursor or low >= end:
                continue
            if low > cursor:
                out.append((cursor, low))
            cursor = max(cursor, min(high, end))
        if cursor < end:
            out.append((cursor, end))
    return out


def cells(data):
    if len(data) % 4:
        raise ValueError('Misaligned FDT cells')
    return [int.from_bytes(data[pos:pos + 4], 'big') for pos in range(0, len(data), 4)]


def number(data):
    return int.from_bytes(data, 'big')


def tuples(props, key, parent):
    ac = number(parent.get('#address-cells', b'\0\0\0\2'))
    sc = number(parent.get('#size-cells', b'\0\0\0\2'))
    if ac not in (1, 2) or sc not in (1, 2):
        raise ValueError('Unsupported address/size cells')
    values = cells(props.get(key, b''))
    if len(values) % (ac + sc):
        raise ValueError('Invalid range property ' + key)
    out = []
    for pos in range(0, len(values), ac + sc):
        base = 0
        size = 0
        for value in values[pos:pos + ac]:
            base = base << 32 | value
        for value in values[pos + ac:pos + ac + sc]:
            size = size << 32 | value
        out.append(interval(base, size))
    return out


def records(ranges):
    return [{'start': f'0x{start:x}', 'end': f'0x{end:x}', 'bytes': end - start}
            for start, end in merge(ranges)]


def trace(log):
    pattern = re.compile(r'PIANO_EFI_TRACE MAP index=(\d+) type=(\d+) phys=0x([\da-fA-F]+) virt=0x[\da-fA-F]+ pages=0x([\da-fA-F]+) attr=0x([\da-fA-F]+)')
    entered = list(re.finditer(r'PIANO_EFI_TRACE EBS_ENTER[^\n]+', log))
    if not entered:
        raise ValueError('No complete EFI map trace')
    block = log[entered[-1].start():]
    line = entered[-1].group()
    if 'known=1' not in line:
        raise ValueError('Unknown EFI map')
    size = int(re.search(r'bytes=(\d+)', line).group(1))
    stride = int(re.search(r'stride=(\d+)', line).group(1))
    if not stride or size % stride:
        raise ValueError('Invalid EFI map size/stride')
    rows = []
    for index, type_, physical, pages, attr in pattern.findall(block):
        if int(index) != len(rows):
            raise ValueError('Nonsequential/repeated EFI descriptors')
        region = interval(int(physical, 16), int(pages, 16) * 4096)
        rows.append({'type': int(type_), 'range': region, 'attribute': int(attr, 16)})
    if len(rows) != size // stride:
        raise ValueError('Truncated EFI descriptor trace')
    cpu = re.findall(r'PIANO_EFI_TRACE CPU[^\n]+', log)
    return rows, cpu[-1] if cpu else None


def audit(dtb, native, efi, runtime=None):
    parsed = read_fdt(dtb)
    tree = parsed['tree']
    root = tree['/']
    dram = []
    for path, props in tree.items():
        if path.count('/') == 1 and props.get('device_type', b'').rstrip(b'\0') == b'memory':
            dram.extend(tuples(props, 'reg', root))
    dram = merge(dram)
    if not dram:
        raise ValueError('No declared DRAM')
    fixed = [interval(base, size) for base, size in parsed['reservations']]
    reusable = []
    dynamic = []
    parent = tree.get('/reserved-memory', root)
    for path, props in tree.items():
        if not path.startswith('/reserved-memory/') or path.count('/') != 2:
            continue
        regions = tuples(props, 'reg', parent)
        fixed.extend(regions)
        if 'reusable' in props:
            reusable.append({'node': path, 'status': props.get('status', b'okay\0').rstrip(b'\0').decode(),
                             'ranges': records(regions), 'no_map': 'no-map' in props})
        requested = number(props.get('size', b''))
        if requested and not any(high > low for low, high in regions):
            allowed = tuples(props, 'alloc-ranges', parent) if 'alloc-ranges' in props else dram
            dynamic.append({'node': path, 'requested_bytes': requested, 'alloc_ranges': allowed,
                            'reusable': 'reusable' in props, 'no_map': 'no-map' in props})
    exclusions = fixed[:]
    for row in native:
        exclusions.append(interval(row['base'], row['size']))
    rows, cpu = trace(efi)
    exclusions.extend(row['range'] for row in rows) # Preserve heap, FD, runtime and existing allocations.
    chosen = tree.get('/chosen', {})
    if 'linux,initrd-start' in chosen and 'linux,initrd-end' in chosen:
        low, high = number(chosen['linux,initrd-start']), number(chosen['linux,initrd-end'])
        exclusions.append(interval(low, high - low))
    fixed_free = subtract(dram, exclusions)
    dynamic_free = subtract(fixed_free, [region for pool in dynamic for region in pool['alloc_ranges']])
    observed = []
    if runtime is not None:
        if not isinstance(runtime.get('phase'), str) or not runtime['phase'] or not isinstance(runtime.get('regions'), list):
            raise ValueError('Runtime evidence must have an explicit phase and regions')
        known = {pool['node']: pool for pool in dynamic}
        seen = set()
        for region in runtime['regions']:
            node = region['node']
            if node not in known or node in seen:
                raise ValueError('Unknown/duplicate dynamic runtime reserve ' + node)
            seen.add(node)
            start = int(region['start'], 0) if isinstance(region['start'], str) else region['start']
            actual = interval(start, region['bytes'])
            pool = known[node]
            if region['bytes'] != pool['requested_bytes'] or not any(low <= actual[0] and actual[1] <= high for low, high in pool['alloc_ranges']):
                raise ValueError('Runtime reserve size or range differs from DT request')
            observed.append({'node': node, 'phase': runtime['phase'], 'range': actual})
    candidates = []
    for low, high in fixed_free:
        start = (low + (2 << 20) - 1) & ~((2 << 20) - 1)
        if high - start < GIB:
            continue
        while start + GIB <= high:
            conflicts = [pool['node'] for pool in dynamic if any(start < end and base < start + GIB for base, end in pool['alloc_ranges'])]
            candidates.append({'start': f'0x{start:x}', 'end': f'0x{start + GIB:x}', 'bytes': GIB,
                               'fixed_exclusions_clear': True, 'unresolved_dynamic_owners': conflicts,
                               'observed_phase_conflicts': [item['node'] for item in observed if start < item['range'][1] and item['range'][0] < start + GIB],
                               'efi_ram_descriptor_cover': False, 'cpu_translation': 'UNKNOWN_NOT_PROBED',
                               'safe_to_use': False})
            start += GIB
    linux_types = {1, 2, 3, 4, 7, 9, 14}
    linux_ram = [row['range'] for row in rows if row['type'] in linux_types and row['attribute'] & 8]
    conventional = [row['range'] for row in rows if row['type'] == 7 and row['attribute'] & 8]
    for item in reusable:
        item_ranges = [(int(row['start'], 16), int(row['end'], 16)) for row in item['ranges']]
        item['outside_efi_linux_usable'] = records(subtract(item_ranges, linux_ram))
    tcr = int(re.search(r'tcr=0x([\da-fA-F]+)', cpu).group(1), 16) if cpu else None
    return {'status': 'OFFLINE_CANDIDATES_ONLY_NO_ALLOCATOR_CHANGE',
            'declared_dram': records(dram), 'declared_dram_bytes': sum(end - start for start, end in dram),
            'fixed_and_existing_owner_exclusions': records(exclusions),
            'fixed_free_ranges': records(fixed_free), 'one_gib_candidates': candidates,
            'unresolved_dynamic_pools': [{**pool, 'alloc_ranges': records(pool['alloc_ranges'])} for pool in dynamic],
            'strict_free_after_all_dynamic_alloc_windows': records(dynamic_free),
            'dynamic_alloc_window_note': 'Allowed placement windows are uncertainty bounds, not actual occupied bytes.',
            'runtime_reserve_evidence': [{'node': item['node'], 'phase': item['phase'], 'ranges': records([item['range']])} for item in observed],
            'free_after_observed_phase_reserves': records(subtract(fixed_free, [item['range'] for item in observed])) if runtime is not None else None,
            'runtime_phase_note': 'Observed ranges apply only to their labelled boot phase; they do not verify UEFI ownership or mapping.',
            'fixed_reusable_cma_contract': reusable,
            'efi_descriptor_count': len(rows),
            'efi_linux_usable_bytes': sum(end - start for start, end in merge(linux_ram)),
            'efi_conventional_bytes': sum(end - start for start, end in merge(conventional)),
            'largest_efi_conventional_bytes': max((end - start for start, end in merge(conventional)), default=0),
            'cpu_snapshot': cpu, 'cpu_va_bits': 64 - (tcr & 63) if tcr is not None else None,
            'cpu_pa_bits': [32, 36, 40, 42, 44, 48, 52, None][(tcr >> 32) & 7] if tcr is not None else None,
            'mapping_claim': 'TCR width permits addresses; no page tables/AT probe captured. Missing EFI descriptors do not prove unmapped DRAM.',
            'linux_contract_note': 'Fixed reusable CMA is excluded from download ownership but must retain valid System RAM/vmemmap at Linux handoff; no-map/secure exclusions differ.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dtb', type=Path, default=Path('private/captures/2026-10-03-piano/live.dtb'))
    parser.add_argument('--native', type=Path, default=Path('platforms/pianoPkg/native-memory-map.json'))
    parser.add_argument('--efi-log', type=Path, default=Path('private/analysis/ramlog-test-85/console.txt'))
    parser.add_argument('--json', type=Path)
    parser.add_argument('--runtime-reserves', type=Path, help='Explicit phase-labelled observed dynamic allocations; never UEFI proof by inference')
    args = parser.parse_args()
    inputs = [args.dtb, args.native, args.efi_log]
    runtime = json.loads(args.runtime_reserves.read_text()) if args.runtime_reserves else None
    if args.runtime_reserves:
        inputs.append(args.runtime_reserves)
    result = audit(inputs[0].read_bytes(), json.loads(inputs[1].read_text()), inputs[2].read_bytes().decode(errors='replace'), runtime)
    result['inputs_sha256'] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    output = json.dumps(result, indent=2) + '\n'
    if args.json:
        args.json.write_text(output)
    print(output, end='')


if __name__ == '__main__':
    main()
