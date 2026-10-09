#!/usr/bin/env python3
"""Inspect current-product module inputs; package only a tested native interface."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import sys
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
COMMANDS = ('status', 'probe', 'repack', 'restore', 'request')
TESTS = ('lossless_roundtrip', 'current_source_guard', 'active_slot_guard',
         'nested_wrapper_rejected', 'ota_restore_refused', 'write_readback',
         'payload_tamper_rejected', 'request_owned_wrapper_only', 'request_persistent_reselection_crc',
         'missing_request_stock_passthrough')
DEVICE_PROOFS = ('device_passthrough_verified', 'request_handling_verified', 'standard_recovery_preserved')
SOURCE_FILES = ('module.prop', 'common.sh', 'customize.sh', 'uninstall.sh', 'manager.sh', 'storage.sh', 'storage-operations.sh', 'service.sh',
                'native-interface.json', 'skip_mount', 'action.sh',
                'webroot/index.html', 'webroot/style.css', 'webroot/main.js', 'webroot/sun.svg',
                'META-INF/com/google/android/update-binary',
                'META-INF/com/google/android/updater-script')


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def pairs(items):
    result = {}
    for key, value in items:
        require(key not in result, 'Duplicate JSON key: ' + key)
        result[key] = value
    return result


def read_file(path, limit=128 * 1024 * 1024):
    path = Path(path)
    require(not path.is_symlink() and path.is_file(), 'Missing or symlink input: ' + str(path))
    require(path.stat().st_size <= limit, 'Oversized input: ' + str(path))
    return path.read_bytes()


def read_json(path):
    return json.loads(read_file(path, 1024 * 1024), object_pairs_hook=pairs)


def child(folder, name):
    require(isinstance(name, str), 'Unsafe relative path')
    part = PurePosixPath(name)
    require(not part.is_absolute() and name and
            part.parts and all(x not in ('', '.', '..') for x in part.parts) and
            '\\' not in name and '\0' not in name, 'Unsafe relative path')
    folder = Path(folder).resolve()
    path = folder.joinpath(*part.parts)
    require(path.resolve().is_relative_to(folder) and
            not any(p.is_symlink() for p in [path, *path.parents] if p != folder and p.is_relative_to(folder)),
            'Symlink/path escape')
    return path


def record(data):
    return {'bytes': len(data), 'sha256': digest(data)}


def checked(data, meta, label):
    require(isinstance(meta, dict) and meta.get('sha256') == digest(data) and
            type(meta.get('bytes')) is int and meta['bytes'] == len(data), label + ' hash/size mismatch')


def product_payload(folder):
    folder = Path(folder)
    require(not (folder / '.incomplete').exists(), 'Product bundle is incomplete')
    meta = read_json(folder / 'manifest.json')
    files = meta.get('files', {})
    image = read_file(child(folder, 'PianoUEFI-product.img'))
    fd = read_file(child(folder, 'PianoUEFI-product.fd'))
    shim = read_file(child(folder, 'BootShim.bin'))
    for name, data in (('PianoUEFI-product.img', image), ('PianoUEFI-product.fd', fd), ('BootShim.bin', shim)):
        checked(data, files.get(name), name)
    require(len(image) >= 4096 and image[:8] == b'ANDROID!', 'Invalid product Android header')
    kernel_bytes, ramdisk_bytes = struct.unpack_from('<II', image, 8)
    version = struct.unpack_from('<I', image, 40)[0]
    require(version in (3, 4) and meta.get('android_header_version') == version,
            'Product must have a recorded v3/v4 Android header')
    ramdisk_at = 4096 + ((kernel_bytes + 4095) // 4096) * 4096
    require(kernel_bytes > 0 and ramdisk_bytes >= 64 and
            ramdisk_at + ramdisk_bytes <= len(image), 'Product kernel/APP bounds differ')
    app = image[ramdisk_at:ramdisk_at + ramdisk_bytes]
    magic, app_version, header, size, app_sha = struct.unpack_from('<16sIIQ32s', app)
    require(magic == b'SUNUEFI-APPv1\0'.ljust(16, b'\0') and app_version == 1 and header == 64 and
            size == len(app) - 64 and app_sha == hashlib.sha256(app[64:]).digest(), 'Invalid APPv1 envelope')
    require(len(fd) == 0x300000 and len(shim) >= 64 and shim[56:60] == b'ARM\x64', 'Unexpected FD/BootShim')
    stream = zlib.decompressobj(16 + zlib.MAX_WBITS)
    raw = stream.decompress(image[4096:4096 + kernel_bytes], len(shim) + len(fd) + 1)
    require(stream.eof and raw == shim + fd, 'Product gzip does not contain the recorded shared core')
    return {'fd.bin': fd, 'app.bin': app, 'shim.bin': shim}, {'product_sha256': digest(image),
            'product_header_version': version, 'fd': record(fd), 'app': record(app), 'shim': record(shim)}


def selector_payload(path, manifest, payloads):
    data, meta = read_file(path, 1024 * 1024), read_json(manifest)
    require(meta.get('schema_version') == 1 and meta.get('interface_version') == 1,
            'Unsupported selector manifest/interface')
    require(meta.get('selector_sha256') == digest(data) and meta.get('selector_bytes') == len(data),
            'Selector hash/size mismatch')
    require(meta.get('product_fd_sha256') == digest(payloads['fd.bin']) and
            meta.get('app_payload_sha256') == digest(payloads['app.bin']), 'Selector belongs to a different FD/APP')
    require(meta.get('supported_boot_headers') == [4], 'Selector must declare current-ROM BOOT v4 support')
    require(meta.get('app_abi') == 1 and meta.get('wrapper_version') == 1,
            'Unsupported selector APP/wrapper ABI')
    require(meta.get('entry_policy') == 'explicit-request-only' and
            meta.get('request_bootarg') == 'sunuefi.boot=uefi', 'Selector must preserve ordinary Android/Recovery entry')
    require(meta.get('persistent_uefi_request') is False, 'A diagnostic persistent UEFI request must not ship in a module')
    require(len(data) >= 4 and len(data) % 4 == 0, 'Selector must contain aligned ARM64 instructions')
    memory, offset = meta.get('selector_memory_bytes'), meta.get('metadata_offset')
    require(type(memory) is int and type(offset) is int and 8192 < memory < 65536 and
            len(data) <= memory and offset >= 0 and offset % 16 == 0 and offset + 128 <= len(data) and
            data[offset:offset + 128] == bytes(128), 'Selector memory/metadata layout differs')
    return data, {'memory_bytes': memory, 'metadata_offset': offset}


def arm64_executable(data):
    require(len(data) >= 64 and data[:7] == b'\x7fELF\x02\x01\x01', 'Native tool must be little-endian ELF64')
    kind, machine = struct.unpack_from('<HH', data, 16)
    entry, table = struct.unpack_from('<QQ', data, 24)
    header_size, width, count = struct.unpack_from('<HHH', data, 52)
    require(kind in (2, 3) and machine == 183 and header_size == 64 and width == 56 and
            1 <= count <= 128 and table + width * count <= len(data), 'Native tool must be an ARM64 executable')
    executable = False
    for index in range(count):
        tag, flags, offset, address, _, size, memory, _ = struct.unpack_from('<IIQQQQQQ', data, table + index * width)
        require(offset + size <= len(data) and size <= memory, 'ELF segment bounds differ')
        require(tag != 3, 'Native tool must be static (no host/Android interpreter dependency)')
        if tag == 1 and flags & 1 and size and address <= entry < address + size:
            executable = True
    require(executable, 'ELF entry has no file-backed executable segment')


def native_proof(tool, manifest, payloads, selector):
    data, proof = read_file(tool, 32 * 1024 * 1024), read_json(manifest)
    arm64_executable(data)
    require(proof.get('schema_version') == 1 and proof.get('interface_version') == 1,
            'Unsupported native proof/interface')
    checked(data, proof.get('executable'), 'Native tool')
    require(set(COMMANDS).issubset(proof.get('commands', [])), 'Native interface commands are incomplete')
    require(proof.get('request_policy') == 'persistent-until-changed', 'Native proof must describe persistent requests')
    expected = {name: digest(raw) for name, raw in {**payloads, 'selector.bin': selector}.items()}
    require(proof.get('payload_sha256') == expected, 'Native proof belongs to different payloads')
    require(proof.get('supported_boot_headers', proof.get('tested_boot_headers')) == [4],
            'Native tool must support current-ROM BOOT v4')
    require(proof.get('stock_kernel_bundled') is False and proof.get('full_partition_backup') is False,
            'Native proof must preserve runtime extraction and no whole-partition backup')
    blockers = []
    if proof.get('tested_boot_headers') != [4]:
        blockers.append('native BOOT v4 tests are missing')
    tests = proof.get('tests', {})
    for name in TESTS:
        if tests.get(name) is not True:
            blockers.append('native test not passed: ' + name)
    evidence = proof.get('evidence', {})
    for name in DEVICE_PROOFS:
        if proof.get(name) is not True:
            blockers.append(name + '=false')
        else:
            item = evidence.get(name, {})
            evidence_data = read_file(child(Path(manifest).parent, item.get('path', '')), 4 * 1024 * 1024)
            checked(evidence_data, item, name + ' evidence')
            require(evidence_data.strip(), 'Empty device evidence')
    return data, proof, blockers


def inspect(args):
    payloads, identity = product_payload(args.product)
    blockers = []
    selector = selector_meta = None
    if args.selector and args.selector_manifest:
        selector, selector_meta = selector_payload(args.selector, args.selector_manifest, payloads)
        payloads['selector.bin'] = selector
    else:
        blockers.append('current-product selector and selector manifest are required')
    native = proof = None
    if args.native_tool and args.native_manifest and selector is not None:
        native, proof, failures = native_proof(args.native_tool, args.native_manifest,
                                              {key: value for key, value in payloads.items() if key != 'selector.bin'}, selector)
        blockers.extend(failures)
    else:
        blockers.append('validated ARM64 piano-boot-repack and native proof are required')
    report = {'schema_version': 1, 'status': 'READY_FOR_MODULE_PACKAGE' if not blockers else 'MODULE_NOT_READY',
              'zip_ready': not blockers, 'blockers': blockers, 'product': identity,
              'payloads': {name: record(data) for name, data in payloads.items()},
              'selector': selector_meta,
              'native_tests': proof.get('tests', {}) if proof else {},
              'device_passthrough_verified': bool(proof and proof.get('device_passthrough_verified') is True),
              'request_handling_verified': bool(proof and proof.get('request_handling_verified') is True),
              'standard_recovery_preserved': bool(proof and proof.get('standard_recovery_preserved') is True),
              'partition_execution_ready': False, 'stock_kernel_bundled': False,
              'ota_automatic': False, 'supported_managers': ['Magisk', 'KernelSU'],
              'automatic_partition_operations': False,
              'root_preservation': 'current BOOT GKI bytes retained; init_boot/vendor_boot untouched',
              'supported_source_layout': 'raw ARM64 BOOT v4, separate init_boot, valid Piano AVB hash/fingerprint'}
    report['entry_policy'] = 'explicit-request-only'
    report['request_policy'] = 'persistent-until-changed'
    report['request_bootarg'] = 'sunuefi.boot=uefi'
    report['webui_bridge_verified'] = False
    return report, payloads, native


def package_installed(args):
    """Package a manager for an already installed, validated shared core."""
    folder = args.installed_core.resolve()
    descriptor = read_json(folder / 'payload-descriptor.json')
    native = read_file(args.native_tool)
    request = read_file(args.request_tool)
    arm64_executable(native)
    arm64_executable(request)
    policy = read_json(args.native_manifest)
    checked(native, policy['tool'], 'Native tool')
    entries = {name: read_file(ROOT / 'android/module' / name) for name in SOURCE_FILES}
    for name in ('selector.bin', 'shim.bin', 'fd.bin', 'app.bin'):
        data = read_file(folder / name)
        checked(data, policy['payloads'][name], name)
        entries['payload/' + name] = data
    entries['bin/piano-boot-repack'] = native
    entries['bin/piano-boot-request'] = request
    for option, name in (('storage_tool', 'piano-storage'), ('resize_tool', 'piano-resize-f2fs')):
        path = getattr(args, option, None)
        if path:
            data = read_file(path)
            arm64_executable(data)
            entries['bin/' + name] = data
    if getattr(args, 'e2fs_tools', None):
        for name in ('e2fsck','resize2fs'):
            data = read_file(args.e2fs_tools / name)
            arm64_executable(data)
            entries['bin/' + name] = data
        entries['licenses/NOTICE.e2fsprogs'] = read_file(args.e2fs_tools / 'NOTICE.e2fsprogs')
        entries['licenses/e2fsprogs-build.json'] = read_file(args.e2fs_tools / 'manifest.json')
    entries['policy.json'] = (json.dumps(policy, indent=2) + '\n').encode()
    entries['installed-core.json'] = (json.dumps(descriptor, indent=2) + '\n').encode()
    entries['licenses/COPYING.libmd'] = read_file(ROOT / 'upstream/simple-init/libs/libmd/COPYING')
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'x', zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(entries.items()):
            item = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            item.create_system = 3
            executable = name.endswith('.sh') or name.startswith('bin/') or name.endswith('/update-binary')
            item.external_attr = (0o100755 if executable else 0o100644) << 16
            item.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(item, data)
    return {'artifact_kind': 'android-product-module' if descriptor.get('generic_core') else 'installed-core-module', 'zip': str(output),
            'zip_sha256': digest(output.read_bytes()), 'stock_kernel_bundled': False,
            'installation': 'current-ROM installation or core upgrade' if descriptor.get('generic_core') else 'read-only adoption of matching current BOOT',
            'ota': 'repack current active-slot stock BOOT using the same core'}


def package(args):
    if getattr(args, 'installed_core', None):
        require(args.native_tool and args.native_manifest and args.request_tool and args.output,
                'Installed core needs native tool/policy, request tool and output')
        return package_installed(args)
    report, payloads, native = inspect(args)
    if args.inspect:
        return report
    candidate = getattr(args, 'candidate', False)
    require(report['zip_ready'] or candidate,
            'MODULE_NOT_READY: ' + '; '.join(report['blockers']))
    require(native is not None and report['selector'] is not None,
            'A candidate also requires the real native tool and same-product selector')
    require(args.output is not None, '--output is required for a ZIP')
    output = Path(args.output)
    require(output.suffix == '.zip' and not output.exists() and not output.is_symlink(), 'Use a new .zip output path')
    source = ROOT / 'android/module'
    entries = {name: read_file(source / name, 128 * 1024) for name in SOURCE_FILES}
    if candidate and not report['zip_ready']:
        entries['module.prop'] = entries['module.prop'].replace(
            b'name=SunUEFI for Piano\n', b'name=SunUEFI for Piano (validation candidate)\n').replace(
            b'description=Current-ROM BOOT selector with explicit UEFI requests; ordinary Android and stock Recovery stay available.\n',
            b'description=Validation candidate; installer refuses BOOT writes until the recorded native and device checks pass.\n')
    entries.update({'payload/' + name: data for name, data in payloads.items()})
    entries['bin/piano-boot-repack'] = native
    entries['licenses/COPYING.libmd'] = read_file(ROOT / 'upstream/simple-init/libs/libmd/COPYING')
    entries['licenses/jsmn.h'] = read_file(ROOT / 'android/native/jsmn.h')
    entries['policy.json'] = (json.dumps({**report, 'interface_version': 1,
        'supported_boot_headers': [4], 'wrapper_version': 1, 'app_abi': 1,
        'commands': list(COMMANDS), 'tool': record(native),
        'native_proof_sha256': digest(read_file(args.native_manifest)),
        'selector_manifest_sha256': digest(read_file(args.selector_manifest))}, sort_keys=True, indent=2) + '\n').encode()
    fields = {'PIANO_POLICY_SCHEMA': '1', 'PIANO_INTERFACE_VERSION': '1',
              'PIANO_DEVICE_PASSTHROUGH_VERIFIED': str(report['device_passthrough_verified']).lower(),
              'PIANO_REQUEST_HANDLING_VERIFIED': str(report['request_handling_verified']).lower(),
              'PIANO_STANDARD_RECOVERY_PRESERVED': str(report['standard_recovery_preserved']).lower()}
    entries['module-policy.sh'] = ''.join(key + '=' + value + '\n' for key, value in fields.items()).encode()
    entries['webroot/status.json'] = (json.dumps({key: report[key] for key in
        ('status', 'device_passthrough_verified', 'request_handling_verified', 'standard_recovery_preserved',
         'webui_bridge_verified', 'entry_policy', 'request_policy', 'request_bootarg')}, indent=2) + '\n').encode()
    # Managers do not retain META-INF and may remove customize.sh or rewrite
    # module.prop. Check the installed runtime/payload files, not installer-only
    # files that are legitimately absent after installation.
    installed = {name: data for name, data in entries.items()
                 if not name.startswith('META-INF/') and name not in ('customize.sh', 'module.prop')}
    entries['payload.sha256'] = ''.join(digest(data) + '  ' + name + '\n'
                                      for name, data in sorted(installed.items())).encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    created = False
    try:
        with output.open('xb') as stream:
            created = True
            with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
                for name, data in sorted(entries.items()):
                    item = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
                    item.create_system = 3
                    executable = name.endswith('.sh') or name.startswith('bin/') or name.endswith('/update-binary')
                    item.external_attr = (0o100755 if executable else 0o100644) << 16
                    item.compress_type = zipfile.ZIP_DEFLATED
                    archive.writestr(item, data)
        with zipfile.ZipFile(output) as archive:
            require(archive.testzip() is None and set(archive.namelist()) == set(entries), 'ZIP readback failed')
    except BaseException:
        if created:
            output.unlink(missing_ok=True)
        raise
    return {**report, 'artifact_kind': 'installable-module' if report['zip_ready'] else 'validation-candidate',
            'install_ready': report['zip_ready'], 'zip': str(output),
            'zip_sha256': digest(output.read_bytes()), 'zip_bytes': output.stat().st_size,
            'zip_entries': len(entries)}


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument('--product', type=Path, default=ROOT / 'artifacts/product')
    result.add_argument('--installed-core', type=Path, help='Validated adopt-exported payload directory')
    result.add_argument('--storage-tool', type=Path, help='Static ARM64 partition/image tool')
    result.add_argument('--e2fs-tools', type=Path, help='Static ARM64 e2fsck/resize2fs build directory with NOTICE')
    result.add_argument('--resize-tool', type=Path, help='Static ARM64 online F2FS shrink helper')
    result.add_argument('--request-tool', type=Path)
    result.add_argument('--selector', type=Path)
    result.add_argument('--selector-manifest', type=Path)
    result.add_argument('--native-tool', type=Path)
    result.add_argument('--native-manifest', type=Path)
    result.add_argument('--output', type=Path)
    result.add_argument('--inspect', action='store_true')
    result.add_argument('--candidate', action='store_true',
                        help='Export a clearly labelled validation ZIP; keep install/write readiness checks intact')
    return result


def main():
    try:
        result = package(parser().parse_args())
        print(json.dumps(result, indent=2))
    except (ValueError, OSError, KeyError, TypeError, struct.error, zlib.error, json.JSONDecodeError) as error:
        print('android-module: ' + str(error), file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
