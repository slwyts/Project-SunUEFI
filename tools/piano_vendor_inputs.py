"""Load the fixed, minimal Piano firmware inputs without consulting private/.

These bytes describe captured firmware. Validation is host provenance checking,
never live hardware ownership or write authorization.
"""
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import uuid

from compose_piano_dtb import read_fdt

ROOT = Path(__file__).resolve().parents[1]
BUNDLE = 'vendor/piano'
MANIFEST_SHA256 = '2fe5b050b4843b23185082fe6816ea889f84521aecedfb4b6b10cbf654c9d35a'
BOARD_DTB = BUNDLE + '/dtb/piano.dtb'
BOARD_DTB_SHA256 = '82b2404bc872c1f60eb4c54c62eb9f2094b97586bdc2aeccae9765f5f258aff1'
BOARD_SOURCE_SHA256 = 'a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7'
XBL_DTB = BUNDLE + '/dtb/xbl-config.dtb'
XBL_DTB_SHA256 = '634ec73dc6d69a07b5af8246e03b0d2ae84dfb9ce9901135121c220443c9cd10'
NATIVE_NAMES = ('SmemDxe', 'DALSys', 'ChipInfo', 'PlatformInfoDxeDriver',
                'HWIODxeDriver', 'ULogDxe', 'CmdDbDxe', 'PwrUtilsDxe',
                'RpmhDxe', 'NpaDxe', 'VcsDxe', 'ClockDxe', 'HALIOMMU')
SNAPSHOT_PROPERTIES = frozenset((
    'linux,initrd-start', 'linux,initrd-end', 'kaslr-seed', 'rng-seed',
    'serial-number', 'bootargs', 'linux,elfcorehdr', 'linux,usable-memory-range',
    'linux,uefi-system-table', 'linux,uefi-mmap-start', 'linux,uefi-mmap-size',
    'linux,uefi-mmap-desc-size', 'linux,uefi-mmap-desc-ver',
))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def _unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate vendor manifest key: ' + key)
        result[key] = value
    return result


def _relative(name):
    if not isinstance(name, str) or not name or '\\' in name or ':' in name:
        raise ValueError('Unsafe vendor input path')
    path = PurePosixPath(name)
    if path.is_absolute() or any(part in ('', '.', '..') for part in name.split('/')):
        raise ValueError('Unsafe vendor input path: ' + name)
    return path


def _path(base, name):
    path = base
    for part in _relative(name).parts:
        path = path / part
        if path.is_symlink():
            raise ValueError('Vendor input is a symlink: ' + name)
    if not path.is_file():
        raise ValueError('Missing vendor input: ' + name)
    return path


def _manifest(raw):
    manifest = json.loads(raw, object_pairs_hook=_unique_pairs)
    if not isinstance(manifest, dict) or type(manifest.get('schema_version')) is not int or manifest['schema_version'] != 1:
        raise ValueError('Unsupported vendor manifest')
    required = {'schema_version', 'target', 'scope', 'native_drivers', 'board_dtb',
                'xbl_config_dtb', 'provenance', 'files'}
    if set(manifest) != required:
        raise ValueError('Vendor manifest field set differs')
    for field, keys in (('board_dtb', {'path', 'source_sha256', 'hardware_scope_unchanged', 'removed_properties'}),
                        ('xbl_config_dtb', {'path', 'source_sha256'})):
        if not isinstance(manifest[field], dict) or set(manifest[field]) != keys:
            raise ValueError('Vendor manifest field set differs: ' + field)
    drivers, files = manifest.get('native_drivers'), manifest.get('files')
    if not isinstance(drivers, dict) or set(drivers) != set(NATIVE_NAMES):
        raise ValueError('Vendor native driver set differs from the fixed product')
    if not isinstance(files, dict):
        raise ValueError('Vendor file inventory missing')
    referenced = [manifest['board_dtb']['path'], manifest['xbl_config_dtb']['path']]
    for name in NATIVE_NAMES:
        row = drivers[name]
        if not isinstance(row, dict) or set(row) != {'file_guid', 'pe', 'depex'}:
            raise ValueError('Malformed vendor native identity: ' + name)
        if not isinstance(row['file_guid'], str):
            raise ValueError('Malformed vendor native GUID: ' + name)
        uuid.UUID(row['file_guid'])
        referenced.append(row['pe'])
        if row['depex'] is not None:
            referenced.append(row['depex'])
    for name in referenced:
        _relative(name)
    if len(set(referenced)) != len(referenced) or set(files) != set(referenced):
        raise ValueError('Vendor file inventory has missing, extra or aliased inputs')
    for name, item in files.items():
        _relative(name)
        if not isinstance(item, dict) or set(item) != {'bytes', 'sha256'}:
            raise ValueError('Malformed vendor file record: ' + name)
        if type(item['bytes']) is not int or not 0 < item['bytes'] <= 8 * 1024 * 1024:
            raise ValueError('Invalid vendor file length: ' + name)
        if not isinstance(item['sha256'], str) or len(item['sha256']) != 64 or any(c not in '0123456789abcdef' for c in item['sha256']):
            raise ValueError('Invalid vendor file hash: ' + name)
    if sha(raw) != MANIFEST_SHA256:
        raise ValueError('Fixed vendor manifest SHA256 mismatch')
    return manifest


def validate_pe(data):
    """Check ARM64 PE headers, raw sections and an executable entry point."""
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError('Native PE DOS header invalid')
    pe = struct.unpack_from('<I', data, 60)[0]
    if pe < 64 or pe + 24 > len(data) or data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Native PE header bounds invalid')
    machine, count = struct.unpack_from('<HH', data, pe + 4)
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    optional, table = pe + 24, pe + 24 + optional_size
    if machine != 0xaa64 or not 1 <= count <= 96 or optional_size < 112 or table + count * 40 > len(data):
        raise ValueError('Native PE machine/section table invalid')
    if struct.unpack_from('<H', data, optional)[0] != 0x20b:
        raise ValueError('Native PE must be PE32+')
    directories = struct.unpack_from('<I', data, optional + 108)[0]
    if directories > 16 or 112 + directories * 8 > optional_size:
        raise ValueError('Native PE data directory bounds invalid')
    entry = struct.unpack_from('<I', data, optional + 16)[0]
    image_size, header_size = struct.unpack_from('<II', data, optional + 56)
    subsystem = struct.unpack_from('<H', data, optional + 68)[0]
    if not entry or entry & 3 or entry >= image_size or not table + count * 40 <= header_size <= len(data) or subsystem not in (11, 12):
        raise ValueError('Native PE image/header/entry bounds invalid')
    ranges, executable_entry = [], False
    for index in range(count):
        at = table + index * 40
        virtual_size, virtual, raw_size, raw = struct.unpack_from('<IIII', data, at + 8)
        flags = struct.unpack_from('<I', data, at + 36)[0]
        if virtual + max(virtual_size, raw_size) > image_size:
            raise ValueError('Native PE virtual section bounds invalid')
        if raw_size:
            if raw < header_size or raw + raw_size > len(data):
                raise ValueError('Native PE raw section bounds invalid')
            if any(raw < end and start < raw + raw_size for start, end in ranges):
                raise ValueError('Native PE raw sections overlap')
            ranges.append((raw, raw + raw_size))
        if flags & 0x20000000 and virtual <= entry and entry + 4 <= virtual + raw_size:
            executable_entry = True
    if not executable_entry:
        raise ValueError('Native PE entry is not backed by executable code')


def validate_depex(data):
    depth, offset = 0, 0
    while offset < len(data):
        opcode = data[offset]
        offset += 1
        if opcode in (0, 1):
            if offset != 1 or len(data) != 18 or data[-1] != 8:
                raise ValueError('Native DEPEX BEFORE/AFTER bounds invalid')
            return
        if opcode == 2:
            if offset + 16 > len(data):
                raise ValueError('Native DEPEX GUID truncated')
            offset += 16
            depth += 1
        elif opcode in (3, 4):
            if depth < 2:
                raise ValueError('Native DEPEX binary stack underflow')
            depth -= 1
        elif opcode == 5:
            if not depth:
                raise ValueError('Native DEPEX unary stack underflow')
        elif opcode in (6, 7):
            depth += 1
        elif opcode == 8:
            if offset != len(data) or depth != 1:
                raise ValueError('Native DEPEX end/stack invalid')
            return
        elif opcode != 9 or offset != 1:
            raise ValueError('Native DEPEX opcode invalid')
    raise ValueError('Native DEPEX missing END')


def validate_board_dtb(data):
    parsed = read_fdt(data)
    if any(SNAPSHOT_PROPERTIES & parsed['tree'].get(node, {}).keys() for node in ('/', '/chosen')):
        raise ValueError('Board DTB contains stale/private snapshot fields')
    return parsed


def load(root=ROOT):
    root = Path(root)
    base = root / BUNDLE
    if base.is_symlink() or (root / 'vendor').is_symlink():
        raise ValueError('Vendor bundle directory is a symlink')
    manifest_path = _path(base, 'manifest.json')
    if manifest_path.stat().st_size > 256 * 1024:
        raise ValueError('Vendor manifest exceeds bounded size')
    raw = manifest_path.read_bytes()
    manifest = _manifest(raw)
    allowed = set(manifest['files']) | {'manifest.json', 'README.md'}
    # Inventory only the consumed firmware trees. Ancillary subdirectories are
    # not build inputs; their contents cannot block firmware preparation.
    inventory = list(base.iterdir())
    for folder in {PurePosixPath(name).parts[0] for name in manifest['files']}:
        inventory.extend((base / folder).rglob('*'))
    for path in inventory:
        if path.is_symlink():
            raise ValueError('Vendor input is a symlink: ' + str(path.relative_to(base)))
        if path.is_file() and path.relative_to(base).as_posix() not in allowed:
            raise ValueError('Unlisted vendor input: ' + str(path.relative_to(base)))
    data, paths = {}, {}
    for name, expected in manifest['files'].items():
        path = _path(base, name)
        if path.stat().st_size != expected['bytes']:
            raise ValueError('Vendor input length differs: ' + name)
        value = path.read_bytes()
        if sha(value) != expected['sha256']:
            raise ValueError('Vendor input SHA256 mismatch: ' + name)
        data[name], paths[name] = value, path
    drivers = {}
    for name, row in manifest['native_drivers'].items():
        validate_pe(data[row['pe']])
        if row['depex'] is not None:
            validate_depex(data[row['depex']])
        drivers[name] = {'file_guid': row['file_guid'], 'pe_path': paths[row['pe']],
                         'pe_sha256': manifest['files'][row['pe']]['sha256'],
                         'depex_path': paths[row['depex']] if row['depex'] else None}
    board = data[manifest['board_dtb']['path']]
    validate_board_dtb(board)
    read_fdt(data[manifest['xbl_config_dtb']['path']])
    # Independent final reads catch changes during validation.
    if _path(base, 'manifest.json').read_bytes() != raw or any(_path(base, name).read_bytes() != value for name, value in data.items()):
        raise ValueError('Vendor inputs changed during validation')
    return {'manifest': manifest, 'native_drivers': drivers, 'board_dtb': board,
            'xbl_config_dtb': data[manifest['xbl_config_dtb']['path']], 'paths': paths}


def source_files(root=ROOT):
    root = Path(root)
    bundle = load(root)
    return (root / 'tools/piano_vendor_inputs.py', root / BUNDLE / 'manifest.json',
            *bundle['paths'].values())


def c_array(symbol, data):
    rows = [f'STATIC CONST UINT8 {symbol}[{len(data)}] = {{']
    rows.extend('  ' + ', '.join(f'0x{byte:02X}' for byte in data[offset:offset + 16]) + ','
                for offset in range(0, len(data), 16))
    rows.append('};')
    return '\n'.join(rows)


def record(root=ROOT, bundle=None):
    root = Path(root)
    bundle = load(root) if bundle is None else bundle
    return {'manifest': BUNDLE + '/manifest.json', 'manifest_sha256': MANIFEST_SHA256,
            'loader_sha256': sha((root / 'tools/piano_vendor_inputs.py').read_bytes()),
            'files': {BUNDLE + '/' + name: item['sha256'] for name, item in bundle['manifest']['files'].items()},
            'board_original_scope_sha256': BOARD_SOURCE_SHA256,
            'board_snapshot_properties_removed': bundle['manifest']['board_dtb']['removed_properties'],
            'runtime_write_authorized': False}


if __name__ == '__main__':
    loaded = load()
    print(json.dumps({'status': 'FIXED_VENDOR_INPUTS_VERIFIED_NOT_RUNTIME_AUTHORITY',
                      'manifest_sha256': MANIFEST_SHA256,
                      'native_drivers': len(loaded['native_drivers']),
                      'files': len(loaded['paths']),
                      'bytes': sum(item['bytes'] for item in loaded['manifest']['files'].values()),
                      'scope': loaded['manifest']['scope'],
                      'runtime_write_authorized': False}, indent=2))
