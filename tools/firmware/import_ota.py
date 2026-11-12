#!/usr/bin/env python3
"""Import stock Piano firmware from an official OTA package into vendor/firmware.

The build never unpacks a payload: this maintainer-only helper reads the
extracted partitions, rebuilds the files whose stock form is not directly
loadable, and rewrites the tracked manifests the product builder consumes.
Run it once per stock release, then commit the result.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
FIRMWARE = ROOT / 'vendor/firmware'
# Component directory, stock partition tree, and the stock name prefix it takes.
COMPONENTS = (
    ('hyperos4-adsp', 'modem-tree', ('adsp',)),
    ('hyperos4-cdsp', 'modem-tree', ('cdsp',)),
)
NON_HLOS = 'stock modem /image (Android /vendor/firmware_mnt/image)'
# Goodix models live as exported arrays in the stock HAL; the offsets come from
# the ELF symbol table, so they are read rather than hardcoded.
GOODIX_MODELS = (
    ('piano-goodix-cap-s.bin', '__asp_CAP_S_2_bin'),
    ('piano-goodix-cap-t.bin', '__asp_CAP_T_2_bin'),
    ('piano-goodix-dubai-u4.bin', 'DLWeightLightDubaiU4_12_10_14MisTouch'),
)
GOODIX_PROGRAM = 'gfpia'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_elf(data, label, allow_external=False):
    """Return (index, offset, vaddr, filesz) for every program header."""
    if data[:4] != b'\x7fELF' or data[4] != 2:
        raise ValueError(label + ' is not a 64-bit ELF image')
    phoff, = struct.unpack_from('<Q', data, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
    if not 0 < phnum < 64 or phentsize < 56 or phoff + phnum * phentsize > len(data):
        raise ValueError(label + ' has an implausible program header table')
    headers = []
    for index in range(phnum):
        base = phoff + index * phentsize
        kind, _, offset, vaddr, _, filesz, _, _ = struct.unpack_from('<IIQQQQQQ', data, base)
        # A split program keeps its segment bodies in separate .bNN files, so
        # only whole images are required to hold them.
        if allow_external or offset + filesz <= len(data):
            headers.append((index, kind, offset, vaddr, filesz))
    if not headers:
        raise ValueError(label + ' has no program headers')
    return headers


def loaded_segments(headers):
    return [(index, offset, vaddr, filesz)
            for index, kind, offset, vaddr, filesz in headers if kind == 1]


def read_symbols(data, label):
    """Return {name: (offset, bytes)} for defined data symbols of a 64-bit ELF."""
    if data[:4] != b'\x7fELF' or data[4] != 2:
        raise ValueError(label + ' is not a 64-bit ELF')
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x3a)
    if not shnum or not shentsize or shoff + shnum * shentsize > len(data):
        raise ValueError(label + ' has no section header table')
    sections = []
    for index in range(shnum):
        base = shoff + index * shentsize
        _, kind, _, addr, offset, size, link, _, _, entsize = struct.unpack_from(
            '<IIQQQQIIQQ', data, base)
        sections.append((kind, addr, offset, size, link, entsize))
    segments = loaded_segments(read_elf(data, label))
    # SHT_SYMTAB or SHT_DYNSYM; stripped stock libraries keep only the latter.
    symbol_table, = [s for s in sections if s[0] in (2, 11)]
    kind, _, table_offset, table_size, link, entsize = symbol_table
    _, str_addr, str_offset, str_size, _, _ = sections[link]
    names = {}
    for base in range(table_offset, table_offset + table_size, entsize or 24):
        name_offset, info, _, shndx, value, size = struct.unpack_from('<IBBHQQ', data, base)
        if shndx == 0 or not size:
            continue
        end = str_offset + name_offset
        stop = data.index(b'\0', end)
        name = data[end:stop].decode()
        for _, offset, vaddr, filesz in segments:
            if vaddr <= value < vaddr + filesz and value - vaddr + size <= filesz:
                start = offset + (value - vaddr)
                names[name] = data[start:start + size]
                break
    return names


def reconstruct(program, trees, label):
    """Rebuild the loadable program from its .mdt header and .bNN segments.

    Stock split programs ship one .bNN file per program header, in the order
    the header table lists them: b00 is the header block itself.
    """
    head = (trees / (program + '.mdt')).read_bytes()
    headers = read_elf(head, label + '.mdt', allow_external=True)
    body = bytearray(head)
    for index, _, offset, _, filesz in headers:
        part = trees / f'{program}.b{index:02d}'
        if not part.is_file():
            raise ValueError(f'Missing segment file {part.name}')
        data = part.read_bytes()
        if len(data) != filesz:
            raise ValueError(f'{part.name} does not match header {index} size')
        if len(body) < offset + filesz:
            body.extend(b'\0' * (offset + filesz - len(body)))
        body[offset:offset + filesz] = data
    return bytes(body)


def files_for(trees, prefixes):
    wanted = {}
    for path in sorted(trees.iterdir()):
        if not path.is_file():
            continue
        # adsp.b09, adsp_dtb.mdt and adspr.jsn all belong to the same
        # component, so the prefix is matched without a separator.
        if any(path.name.startswith(p) for p in prefixes):
            wanted[path.name] = path.read_bytes()
    return wanted


def write_component(name, rom, source, payload, extra_notes=None):
    folder = FIRMWARE / name
    folder.mkdir(parents=True, exist_ok=True)
    for stale in folder.iterdir():
        if stale.is_file() and stale.name not in ('manifest.json', 'README.md'):
            stale.unlink()
    manifest = {'schema_version': 1, 'component': name,
                'rom': rom, 'source': source, 'files': {}}
    for filename, data in sorted(payload.items()):
        (folder / filename).write_bytes(data)
        manifest['files'][filename] = {'bytes': len(data), 'sha256': digest(data)}
    (folder / 'manifest.json').write_text(json.dumps(manifest, indent=1) + '\n')
    if extra_notes:
        (folder / 'README.md').write_text(extra_notes)
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extracted', type=Path, required=True,
                        help='Directory holding the OTA partition trees')
    parser.add_argument('--rom', required=True, help='Stock release, for example OS4.0.6.0.XPYCNXM')
    parser.add_argument('--dry-run', action='store_true', help='Report without writing files')
    args = parser.parse_args(argv)

    base = args.extracted.resolve()
    missing = [name for name in ('modem-tree', 'odm-tree') if not (base / name).is_dir()]
    if missing:
        parser.error('Missing extracted trees: ' + ', '.join(missing))
    modem, odm = base / 'modem-tree', base / 'odm-tree'

    results = {}
    for component, tree, prefixes in COMPONENTS:
        payload = files_for(base / tree, prefixes)
        if not payload:
            raise SystemExit(f'No stock files matched {component}')
        print(f'{component}: {len(payload)} files from {tree}')
        if not args.dry_run:
            write_component(component, args.rom, NON_HLOS, payload)
        results[component] = len(payload)

    # The Goodix secure program is shipped split; the stock MDT header names the
    # offsets, so the reconstructed image keeps every original byte in place.
    program = reconstruct(GOODIX_PROGRAM, modem, GOODIX_PROGRAM)
    hal = odm / 'lib64/libgf_hal.so'
    if not hal.is_file():
        raise SystemExit('Stock Goodix HAL is missing from the extracted odm tree')
    symbols = read_symbols(hal.read_bytes(), hal.name)
    payload = {'gfpia.elf': program}
    for filename, symbol in GOODIX_MODELS:
        if symbol not in symbols:
            raise SystemExit(f'Stock HAL no longer exports {symbol}')
        payload[filename] = symbols[symbol]
    print(f'hyperos4-fingerprint: {len(payload)} files, program from '
          f'{GOODIX_PROGRAM}.mdt, models from {hal.name}')
    if not args.dry_run:
        write_component('hyperos4-fingerprint', args.rom,
                        f'stock modem /image/{GOODIX_PROGRAM}.mdt and .bNN; reconstructed at their '
                        f'ELF file offsets; models read from stock /odm/lib64/{hal.name} by symbol',
                        payload)
    results['hyperos4-fingerprint'] = len(payload)

    print(json.dumps({'rom': args.rom, 'components': results}, indent=2))
    if args.dry_run:
        print('dry run: nothing written')
    return 0


if __name__ == '__main__':
    sys.exit(main())
