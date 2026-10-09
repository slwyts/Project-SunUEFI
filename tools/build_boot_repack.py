#!/usr/bin/env python3
"""Build the native BOOT4 tool; optionally roundtrip one real stock file without a device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TESTS = ('lossless_roundtrip', 'current_source_guard', 'active_slot_guard',
         'nested_wrapper_rejected', 'ota_restore_refused', 'write_readback',
         'payload_tamper_rejected', 'request_owned_wrapper_only',
         'request_persistent_reselection_crc', 'missing_request_stock_passthrough')


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def same_file_bytes(left, right):
    with left.open('rb') as a, right.open('rb') as b:
        while True:
            x, y = a.read(1024 * 1024), b.read(1024 * 1024)
            if x != y:
                return False
            if not x:
                return True


def run_json(command):
    return json.loads(subprocess.check_output(list(map(str, command)), text=True))


def roundtrip(binary, stock, selector_dir, product, app, output, runner=None):
    if not stock or not selector_dir:
        raise ValueError('--roundtrip-stock and --selector-dir are required together')
    stock, selector_dir, product, app = (p.resolve() for p in (stock, selector_dir, product, app))
    info = json.loads((selector_dir / 'selector.json').read_text())
    selector = selector_dir / 'selector.bin'
    if selector.stat().st_size != info['selector_bytes'] or sha(selector) != info['selector_sha256']:
        raise ValueError('Selector manifest hash/size mismatch')
    payloads = {'selector.bin': selector, 'BootShim.bin': product / 'BootShim.bin',
                'PianoUEFI-product.fd': product / 'PianoUEFI-product.fd', 'app-payload.bin': app}
    product_meta = json.loads((product / 'manifest.json').read_text())
    if product_meta.get('target') != 'product':
        raise ValueError('Use the current product FD/BootShim')
    for name in ('BootShim.bin', 'PianoUEFI-product.fd'):
        p = payloads[name]
        if product_meta['files'][name] != {'bytes': p.stat().st_size, 'sha256': sha(p)}:
            raise ValueError('Product component differs from its manifest: ' + name)
    command = ([runner] if runner else []) + [binary]
    probe = run_json([*command, 'probe', '--input', stock])
    if probe['wrapped']:
        raise ValueError('Roundtrip requires the actual unwrapped stock BOOT')
    wrapped = output / 'roundtrip-wrapped.img'
    if wrapped.exists():
        raise ValueError('Roundtrip output already exists')
    packed = run_json([*command, 'repack', '--input', stock, '--output', wrapped,
                       '--selector', selector, '--selector-memory-bytes', info['selector_memory_bytes'],
                       '--selector-metadata-offset', info['metadata_offset'],
                       '--shim', payloads['BootShim.bin'], '--fd', payloads['PianoUEFI-product.fd'], '--app', app])
    wrapped_probe = run_json([*command, 'probe', '--input', wrapped])
    # The restored full image is a temporary comparison result, never a retained backup.
    with tempfile.TemporaryDirectory(prefix='restore-check-', dir=output) as temporary:
        restored = Path(temporary) / 'restored.img'
        result = run_json([*command, 'restore', '--input', wrapped, '--output', restored])
        restored_sha = sha(restored)
        equal = same_file_bytes(stock, restored)
        if not equal or restored_sha != sha(stock):
            raise ValueError('Real stock BOOT roundtrip did not reproduce all original bytes')
        # Real wrapper bytes, shared NEXTv1 CRC/reader, persistent re-selection,
        # and lossless restore after the legitimate request changes its AVB hash.
        requested = Path(temporary) / 'request-linux.img'
        reselected = Path(temporary) / 'request-android.img'
        first = run_json([*command, 'request', '--input', wrapped, '--output', requested, '--target', 'linux'])
        first_preview = run_json([*command, 'request', '--input', requested, '--target', 'linux', '--preview'])
        second = run_json([*command, 'request', '--input', requested, '--output', reselected, '--target', 'android'])
        preview = run_json([*command, 'request', '--input', reselected, '--target', 'android', '--preview'])
        if (first['target'], first['sequence'], second['target'], second['sequence'],
                preview['target'], preview['sequence'], first_preview['target'], first_preview['sequence']) != (2, 1, 0, 2, 0, 2, 2, 1):
            raise ValueError('Real persistent request CRC/reselection differed')
        if first_preview['file_sha256'] != first['file_sha256'] or preview['file_sha256'] != second['file_sha256']:
            raise ValueError('Read-only request preview changed the real wrapper bytes')
        changed_probe = run_json([*command, 'probe', '--input', reselected])
        if changed_probe['avb_hash_matches']:
            raise ValueError('Expected NEXTv1 request to change the unsigned wrapper AVB data hash')
        restored.unlink()
        changed_restore = run_json([*command, 'restore', '--input', reselected, '--output', restored])
        if not same_file_bytes(stock, restored) or sha(restored) != sha(stock):
            raise ValueError('Real BOOT did not restore exactly after persistent request changes')
    record = {'status': 'REAL_STOCK_FILE_ROUNDTRIP_BYTE_EQUAL_NOT_DEVICE_VERIFIED',
              'source_boot_sha256': sha(stock), 'source_boot_bytes': stock.stat().st_size,
              'restored_sha256': restored_sha, 'byte_equal': equal,
              'source_probe': probe, 'repack_result': packed, 'wrapped_probe': wrapped_probe,
              'restore_result': result, 'wrapped_file': wrapped.name, 'wrapped_sha256': sha(wrapped),
              'persistent_request': {'first': first, 'first_read_only_preview': first_preview,
                                     'reselected': second, 'read_only_preview': preview,
                                     'changed_wrapper_probe': changed_probe, 'restore_result': changed_restore,
                                     'restored_byte_equal': True},
              'payloads': {name: {'sha256': sha(path), 'bytes': path.stat().st_size} for name, path in payloads.items()},
              'selector_source_files': info.get('source_files', {}),
              'full_stock_backup_retained': False, 'device_operation_performed': False,
              'oem_signature_verified': False, 'new_wrapper_oem_signed': False}
    (output / 'roundtrip.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def build(output, arch, compiler=None, sysroot=None):
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    bundled = ROOT / 'build/host-tools/usr'
    env = os.environ.copy()
    if (bundled / 'bin').is_dir():
        env['PATH'] = str(bundled / 'bin') + os.pathsep + env.get('PATH', '')
        env['LD_LIBRARY_PATH'] = str(bundled / 'lib') + os.pathsep + env.get('LD_LIBRARY_PATH', '')
    cc = shutil.which(compiler or ('cc' if arch == 'host' else 'clang'), path=env['PATH'])
    if not cc:
        raise ValueError('Native compiler is unavailable')
    libmd = ROOT / 'upstream/simple-init/libs/libmd'
    request = ROOT / 'uefi/handoff/bootselect'
    sources = [ROOT / 'android/native/piano-boot-repack.c', request / 'BootRequest.c', libmd / 'src/sha2.c']
    inputs = [*sources, request / 'BootRequest.h', libmd / 'include/sha2.h', libmd / 'src/local-link.h', libmd / 'COPYING',
              ROOT / 'android/native/jsmn.h', ROOT / 'android/native/jsmn-source.json']
    flags = ['-O2', '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror',
             '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
             '-I' + str(request), '-I' + str(libmd / 'include')]
    if arch == 'aarch64':
        flags += ['-static']
        if 'clang' in Path(cc).name:
            if not sysroot:
                raise ValueError('Cross clang requires an existing AArch64 libc/GCC --sysroot')
            flags += ['--target=aarch64-linux-gnu', '-fuse-ld=lld',
                      '--sysroot=' + str(sysroot.resolve()), '--gcc-toolchain=' + str(sysroot.resolve() / 'usr')]
        elif sysroot:
            flags += ['--sysroot=' + str(sysroot.resolve())]
    binary = output / 'piano-boot-repack'
    subprocess.run([cc, *flags, *map(str, sources), '-o', str(binary)], env=env, check=True)
    data = binary.read_bytes()
    if len(data) < 64 or data[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('Unexpected native ELF format')
    static = True
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phsize, phcount = struct.unpack_from('<HH', data, 54)
    for i in range(phcount):
        if struct.unpack_from('<I', data, phoff + i * phsize)[0] == 3:
            static = False
    if arch == 'aarch64' and (struct.unpack_from('<H', data, 18)[0] != 183 or not static):
        raise ValueError('AArch64 native tool must be a static ELF without PT_INTERP')
    shutil.copy2(libmd / 'COPYING', output / 'COPYING.libmd')
    shutil.copy2(ROOT / 'android/native/jsmn.h', output / 'jsmn.h')
    shutil.copy2(ROOT / 'android/native/jsmn-source.json', output / 'jsmn-source.json')
    record = {'schema_version': 1, 'interface_version': 1,
              'status': 'NATIVE_ONLINE_INTERFACE_BUILT_NOT_DEVICE_VERIFIED', 'arch': arch,
              'commands': ['status', 'probe', 'adopt', 'repack', 'upgrade', 'restore', 'request'], 'file_mode_only': False,
              'read_only_adoption': True, 'adoption_policy_payload_matching': True,
              'adoption_full_stock_reconstruction': True, 'adoption_payload_export': True,
              'core_upgrade_implemented': True, 'upgrade_preserves_stock_and_request': True,
              'wrapper': 'SPLITv1+RSTRv1', 'supported_boot_headers': [4],
              'compiler': cc, 'compiler_version': subprocess.check_output([cc, '--version'], env=env, text=True).splitlines()[0],
              'compile_command': [cc, *flags, *map(str, sources), '-o', str(binary)],
              'sources': {str(path.relative_to(ROOT)): sha(path) for path in inputs},
              'builder_sha256': sha(Path(__file__).resolve()),
              'libmd_upstream_commit': subprocess.check_output(['git', '-C', ROOT / 'upstream/simple-init', 'rev-parse', 'HEAD'], text=True).strip(),
              'executable': {'file': binary.name, 'bytes': binary.stat().st_size, 'sha256': sha(binary), 'static': static},
              'stock_kernel_bundled': False, 'full_partition_backup': False,
              'request_policy': 'persistent-until-changed', 'tests': dict.fromkeys(TESTS, False),
              'device_operation_performed': False, 'device_passthrough_verified': False,
              'request_handling_verified': False, 'standard_recovery_preserved': False,
              'device_execution_ready': False, 'ota_automatic': False,
              'module_policy_interface_implemented': True}
    (output / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    return binary, record


def prototype_policy(binary, result, selector_dir, product, app, output):
    """Deployable input records with every unperformed device proof still false."""
    meta = json.loads((selector_dir / 'selector.json').read_text())
    paths = {'selector.bin': selector_dir / 'selector.bin', 'shim.bin': product / 'BootShim.bin',
             'fd.bin': product / 'PianoUEFI-product.fd', 'app.bin': app}
    payloads = {name: {'sha256': sha(path), 'bytes': path.stat().st_size} for name, path in paths.items()}
    result['payload_sha256'] = {name: item['sha256'] for name, item in payloads.items()}
    selector_meta = {**meta, 'schema_version': 1, 'interface_version': 1,
                     'supported_boot_headers': [4], 'wrapper_version': 1, 'app_abi': 1,
                     'product_fd_sha256': payloads['fd.bin']['sha256'],
                     'app_payload_sha256': payloads['app.bin']['sha256'],
                     'entry_policy': 'explicit-request-only', 'request_bootarg': 'sunuefi.boot=uefi',
                     'persistent_uefi_request': False}
    # The diagnostic cmdline override is a BOOT packaging option, not a flag
    # in selector.bin. The runtime repacker keeps the real stock BOOT header.
    (output / 'selector-module.json').write_text(json.dumps(selector_meta, indent=2) + '\n')
    policy = {'schema_version': 1, 'interface_version': 1, 'wrapper_version': 1, 'app_abi': 1,
              'zip_ready': False, 'stock_kernel_bundled': False, 'ota_automatic': False,
              'entry_policy': 'explicit-request-only', 'request_policy': 'persistent-until-changed',
              'device_passthrough_verified': False, 'request_handling_verified': False,
              'standard_recovery_preserved': False, 'webui_bridge_verified': False, 'native_tests': result['tests'],
              'tool': {'sha256': sha(binary), 'bytes': binary.stat().st_size}, 'payloads': payloads,
              'selector': {'memory_bytes': meta['selector_memory_bytes'], 'metadata_offset': meta['metadata_offset']}}
    (output / 'policy-prototype.json').write_text(json.dumps(policy, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/android-boot-repack')
    parser.add_argument('--arch', choices=('host', 'aarch64'), default='aarch64')
    parser.add_argument('--cc')
    parser.add_argument('--sysroot', type=Path)
    parser.add_argument('--roundtrip-stock', type=Path)
    parser.add_argument('--selector-dir', type=Path)
    parser.add_argument('--product', type=Path, default=ROOT / 'artifacts/product')
    parser.add_argument('--app', type=Path, default=ROOT / 'artifacts/simpleinit/product/app-payload.bin')
    parser.add_argument('--runner', help='Existing user-mode emulator, e.g. qemu-aarch64; no device runner')
    args = parser.parse_args()
    try:
        binary, result = build(args.output, args.arch, args.cc, args.sysroot)
        if args.roundtrip_stock or args.selector_dir:
            result['roundtrip'] = roundtrip(binary, args.roundtrip_stock, args.selector_dir,
                                            args.product, args.app, args.output.resolve(), args.runner)
            result['tested_boot_headers'] = [4]
            result['tests']['lossless_roundtrip'] = True
            result['tests']['request_persistent_reselection_crc'] = True
            prototype_policy(binary, result, args.selector_dir.resolve(), args.product.resolve(),
                             args.app.resolve(), args.output.resolve())
            (args.output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, str(error) + '\n')
