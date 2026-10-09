#!/usr/bin/env python3
"""Build the Android online F2FS helper without running a device operation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def build(output, compiler=None, sysroot=None, arch='aarch64'):
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    bundled = ROOT / 'build/host-tools/usr'
    if (bundled / 'bin').is_dir():
        env['PATH'] = str(bundled / 'bin') + os.pathsep + env.get('PATH', '')
        env['LD_LIBRARY_PATH'] = str(bundled / 'lib') + os.pathsep + env.get('LD_LIBRARY_PATH', '')
    cc = shutil.which(compiler or ('cc' if arch == 'host' else 'clang'), path=env['PATH'])
    if not cc:
        raise ValueError('F2FS helper compiler is unavailable')
    source = ROOT / 'tools/android/piano_resize_f2fs.c'
    flags = ['-O2', '-std=c11', '-Wall', '-Wextra', '-Werror']
    if arch == 'aarch64':
        flags += ['-static']
        if 'clang' in Path(cc).name:
            if sysroot is None:
                raise ValueError('Cross clang needs an AArch64 libc/GCC --sysroot')
            flags += ['--target=aarch64-linux-gnu', '-fuse-ld=lld',
                      '--sysroot=' + str(sysroot.resolve()),
                      '--gcc-toolchain=' + str(sysroot.resolve() / 'usr')]
        elif sysroot is not None:
            flags += ['--sysroot=' + str(sysroot.resolve())]
    binary = output / 'piano-resize-f2fs'
    command = [cc, *flags, str(source), '-o', str(binary)]
    subprocess.run(command, env=env, check=True)
    data = binary.read_bytes()
    if data[:4] != b'\x7fELF' or (arch == 'aarch64' and int.from_bytes(data[18:20], 'little') != 183):
        raise ValueError('Unexpected F2FS helper ELF architecture')
    record = {'arch': arch, 'compile_command': command,
              'source': str(source.relative_to(ROOT)),
              'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'binary': {'file': binary.name, 'bytes': len(data),
                         'sha256': hashlib.sha256(data).hexdigest()},
              'commands': ['status [--expect-bytes BYTES]', 'shrink --target-bytes BYTES --execute'],
              'device_operation_performed': False, 'writes_gpt': False}
    (output / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/android-storage/resize-f2fs')
    parser.add_argument('--arch', choices=('host', 'aarch64'), default='aarch64')
    parser.add_argument('--cc')
    parser.add_argument('--sysroot', type=Path)
    args = parser.parse_args()
    print(json.dumps(build(args.output, args.cc, args.sysroot, args.arch), indent=2))
