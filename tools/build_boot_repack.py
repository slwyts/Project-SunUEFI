#!/usr/bin/env python3
"""Build the native regular-file BOOT4 repacker; optionally roundtrip one real stock file."""
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
    record = {'status': 'REAL_STOCK_FILE_ROUNDTRIP_BYTE_EQUAL_NOT_DEVICE_VERIFIED',
              'source_boot_sha256': sha(stock), 'source_boot_bytes': stock.stat().st_size,
              'restored_sha256': restored_sha, 'byte_equal': equal,
              'source_probe': probe, 'repack_result': packed, 'wrapped_probe': wrapped_probe,
              'restore_result': result, 'wrapped_file': wrapped.name, 'wrapped_sha256': sha(wrapped),
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
    inputs = [*sources, request / 'BootRequest.h', libmd / 'include/sha2.h', libmd / 'src/local-link.h', libmd / 'COPYING']
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
    record = {'schema_version': 1, 'interface_version': 1,
              'status': 'NATIVE_FILE_MODE_BUILT_NOT_DEVICE_VERIFIED', 'arch': arch,
              'commands': ['status', 'probe', 'repack', 'restore'], 'file_mode_only': True,
              'wrapper': 'SPLITv1+RSTRv1', 'supported_boot_headers': [4],
              'compiler': cc, 'compiler_version': subprocess.check_output([cc, '--version'], env=env, text=True).splitlines()[0],
              'compile_command': [cc, *flags, *map(str, sources), '-o', str(binary)],
              'sources': {str(path.relative_to(ROOT)): sha(path) for path in inputs},
              'libmd_upstream_commit': subprocess.check_output(['git', '-C', ROOT / 'upstream/simple-init', 'rev-parse', 'HEAD'], text=True).strip(),
              'executable': {'file': binary.name, 'bytes': binary.stat().st_size, 'sha256': sha(binary), 'static': static},
              'stock_kernel_bundled': False, 'full_partition_backup': False,
              'device_operation_performed': False, 'device_passthrough_verified': False,
              'request_handling_verified': False, 'standard_recovery_preserved': False,
              'device_execution_ready': False, 'ota_automatic': False,
              'module_policy_interface_implemented': False}
    (output / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    return binary, record


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
            (args.output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, str(error) + '\n')
