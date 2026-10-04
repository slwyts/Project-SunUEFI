#!/usr/bin/env python3
"""Inspect captured boot headers and FDTs offline; never connects to a device."""
import argparse
import gzip
import json
from pathlib import Path
import struct

def parse_fdt(data):
    if len(data) < 40:
        raise ValueError('Short FDT header')
    h = struct.unpack_from('>10I', data)
    magic, total, off_struct, off_strings, off_reserve, version, compat, _, size_strings, size_struct = h
    if magic != 0xd00dfeed or total > len(data) or version < 16:
        raise ValueError('Invalid FDT header')
    if off_struct + size_struct > total or off_strings + size_strings > total:
        raise ValueError('FDT block outside total size')
    strings = data[off_strings:off_strings + size_strings]
    nodes, stack = {}, []
    pos, end = off_struct, off_struct + size_struct
    def cstring(blob, start):
        finish = blob.find(b'\0', start)
        if start < 0 or finish < start:
            raise ValueError('Unterminated FDT string')
        return blob[start:finish].decode(errors='replace'), finish + 1
    while pos + 4 <= end:
        token = struct.unpack_from('>I', data, pos)[0]
        pos += 4
        if token == 1:
            name, pos = cstring(data[:end], pos)
            pos = (pos + 3) & ~3
            stack.append(name)
            path = '/' + '/'.join(stack[1:])
            nodes[path] = {}
        elif token == 2:
            if not stack:
                raise ValueError('FDT node stack underflow')
            stack.pop()
        elif token == 3:
            if not stack or pos + 8 > end:
                raise ValueError('Invalid FDT property')
            size, nameoff = struct.unpack_from('>2I', data, pos)
            pos += 8
            if nameoff >= len(strings) or pos + size > end:
                raise ValueError('Invalid FDT property range')
            name, _ = cstring(strings, nameoff)
            nodes['/' + '/'.join(stack[1:])][name] = data[pos:pos + size]
            pos = (pos + size + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            if stack:
                raise ValueError('Unbalanced FDT nodes')
            return nodes
        else:
            raise ValueError(f'Invalid FDT token {token}')
    raise ValueError('Missing FDT end token')

def cells(value):
    if len(value) % 4:
        raise ValueError('Non-cell-aligned value')
    return struct.unpack('>' + 'I' * (len(value) // 4), value)

def text(value):
    return value.rstrip(b'\0').decode(errors='replace').split('\0')

def reg_ranges(nodes, path):
    parent = path.rsplit('/', 1)[0] or '/'
    props = nodes[parent]
    ac = int.from_bytes(props.get('#address-cells', b'\0\0\0\2'), 'big')
    sc = int.from_bytes(props.get('#size-cells', b'\0\0\0\1'), 'big')
    if ac not in (1, 2) or sc not in (0, 1, 2):
        raise ValueError('Unsupported address/size cells')
    values = cells(nodes[path].get('reg', b''))
    if len(values) % (ac + sc):
        raise ValueError('Invalid reg size')
    def integer(v):
        result = 0
        for cell in v:
            result = (result << 32) | cell
        return result
    return [{'base': integer(values[i:i+ac]), 'size': integer(values[i+ac:i+ac+sc])}
            for i in range(0, len(values), ac + sc)]

def summarize(nodes):
    root = nodes['/']
    result = {'model': text(root.get('model', b'')),
              'compatible': text(root.get('compatible', b'')),
              'memory': [], 'reserved_memory': [], 'hardware': [], 'uefi': {}}
    for path, props in nodes.items():
        compatible = text(props.get('compatible', b''))
        if props.get('device_type') == b'memory\0':
            result['memory'].append({'path': path, 'ranges': reg_ranges(nodes, path)})
        if path.startswith('/reserved-memory/') and path.count('/') == 2:
            result['reserved_memory'].append({'path': path, 'ranges': reg_ranges(nodes, path),
                'no_map': 'no-map' in props, 'compatible': compatible})
        if any(s in ' '.join(compatible) for s in ('gic-v3', 'psci', 'ufshc', 'dwc3', 'gunyah', 'simple-framebuffer')):
            result['hardware'].append({'path': path, 'compatible': compatible,
                'reg_cells': [hex(v) for v in cells(props.get('reg', b''))],
                'method': text(props.get('method', b'')), 'status': text(props.get('status', b''))})
        if 'uefi' in path.lower() or path.startswith('/chosen'):
            result['uefi'][path] = {k: ([hex(v) for v in cells(b)] if b and len(b) % 4 == 0 and b'\0' in b[:4]
                                else text(b)) for k,b in props.items()
                                if k not in ('bootargs', 'rng-seed', 'kaslr-seed', 'serial-number')}
    return result

def scan_fdts(data):
    start = 0
    while True:
        offset = data.find(b'\xd0\x0d\xfe\xed', start)
        if offset < 0:
            return
        start = offset + 4
        if offset + 40 > len(data):
            continue
        size = struct.unpack_from('>I', data, offset + 4)[0]
        if 40 <= size <= len(data) - offset:
            blob = data[offset:offset + size]
            try:
                nodes = parse_fdt(blob)
                yield offset, blob, nodes
            except ValueError:
                continue

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {'boot_headers': {}, 'fdts': {}}
    live = (args.capture / 'live.dtb').read_bytes()
    report['live'] = summarize(parse_fdt(live))
    for name in ('boot_a', 'init_boot_a', 'vendor_boot_a', 'dtbo_a', 'xbl_config_a', 'uefi_a'):
        data = (args.capture / (name + '.img')).read_bytes()
        if data.startswith(b'ANDROID!'):
            ks, rs, version, hs = struct.unpack_from('<4I', data, 8)
            hv = struct.unpack_from('<I', data, 40)[0]
            report['boot_headers'][name] = {'header_version': hv, 'header_size': hs,
                                          'kernel_size': ks, 'ramdisk_size': rs, 'os_version': version}
            if hv in (3, 4) and ks:
                (args.output / (name + '-kernel')).write_bytes(data[4096:4096+ks])
        if data.startswith(b'VNDRBOOT'):
            hv, page, ka, ra, rs = struct.unpack_from('<5I', data, 8)
            hs, ds = struct.unpack_from('<2I', data, 2096)
            da = struct.unpack_from('<Q', data, 2104)[0]
            dtb_off = ((hs + page - 1)//page + (rs + page-1)//page)*page
            dtb = data[dtb_off:dtb_off+ds]
            (args.output / 'vendor-dtbs.bin').write_bytes(dtb)
            report['boot_headers'][name] = {'header_version': hv, 'page_size': page,
                'vendor_ramdisk_size': rs, 'dtb_size': ds, 'dtb_addr': hex(da), 'dtb_offset': dtb_off}
        for offset, blob, nodes in scan_fdts(data):
            key = f'{name}-0x{offset:x}'
            (args.output / (key + '.dtb')).write_bytes(blob)
            report['fdts'][key] = summarize(nodes)
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'boot_headers': report['boot_headers'],
                      'embedded_fdt_count': len(report['fdts']), 'live': report['live']}, indent=2))

if __name__ == '__main__':
    main()
