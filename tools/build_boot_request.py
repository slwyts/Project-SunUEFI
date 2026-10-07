#!/usr/bin/env python3
"""Build the shared-CRC native request command; never access a device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(output, arch, compiler=None, sysroot=None):
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    bundled = ROOT / 'build/host-tools/usr'
    env = os.environ.copy()
    if (bundled / 'bin').is_dir():
        env['PATH'] = str(bundled / 'bin') + os.pathsep + env.get('PATH', '')
        env['LD_LIBRARY_PATH'] = str(bundled / 'lib') + os.pathsep + env.get('LD_LIBRARY_PATH', '')
    cc = compiler or ('cc' if arch == 'host' else 'clang')
    cc = shutil.which(cc, path=env['PATH'])
    if not cc:
        raise ValueError('Native request compiler is unavailable')
    sources = [ROOT / 'android/native/piano-boot-request.c', ROOT / 'uefi/handoff/bootselect/BootRequest.c']
    header = ROOT / 'uefi/handoff/bootselect/BootRequest.h'
    flags = ['-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(header.parent)]
    if arch == 'aarch64':
        flags += ['-static']
        if 'clang' in Path(cc).name:
            flags += ['--target=aarch64-linux-gnu', '-fuse-ld=lld']
            if sysroot:
                flags += ['--sysroot=' + str(sysroot.resolve()), '--gcc-toolchain=' + str(sysroot.resolve() / 'usr')]
        elif sysroot:
            flags += ['--sysroot=' + str(sysroot.resolve())]
    binary = output / 'piano-boot-request'
    subprocess.run([cc, *flags, *map(str, sources), '-o', str(binary)], env=env, check=True)
    data = binary.read_bytes()
    if data[:4] != b'\x7fELF' or (arch == 'aarch64' and int.from_bytes(data[18:20], 'little') != 183):
        raise ValueError('Unexpected native request ELF architecture')
    record = {'status': 'HOST_BUILT_NATIVE_REQUEST_NOT_DEVICE_VERIFIED', 'arch': arch,
              'compiler': cc, 'compiler_version': subprocess.check_output([cc, '--version'], env=env, text=True).splitlines()[0],
              'sources': {str(path.relative_to(ROOT)): sha(path) for path in [*sources, header]},
              'binary': {'file': binary.name, 'bytes': binary.stat().st_size, 'sha256': sha(binary)},
              'device_operation_performed': False, 'repacker_implemented': False}
    (output / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'artifacts/boot-request')
    parser.add_argument('--arch', choices=('host', 'aarch64'), default='aarch64')
    parser.add_argument('--cc')
    parser.add_argument('--sysroot', type=Path)
    args = parser.parse_args()
    print(json.dumps(build(args.output, args.arch, args.cc, args.sysroot), indent=2))
