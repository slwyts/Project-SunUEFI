#!/usr/bin/env python3
"""Build the bounded Android storage helper; never open a device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def build(output, arch, compiler=None, sysroot=None):
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    bundled = ROOT / 'build/host-tools/usr'
    if (bundled / 'bin').is_dir():
        env['PATH'] = str(bundled / 'bin') + os.pathsep + env.get('PATH', '')
        env['LD_LIBRARY_PATH'] = str(bundled / 'lib') + os.pathsep + env.get('LD_LIBRARY_PATH', '')
    cc = shutil.which(compiler or ('cc' if arch == 'host' else 'clang'), path=env['PATH'])
    if not cc:
        raise ValueError('Native storage compiler is unavailable')
    libmd = ROOT / 'upstream/simple-init/libs/libmd'
    sources = [ROOT / 'android/native/piano-storage.c', libmd / 'src/sha2.c']
    flags = ['-O2', '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror',
             '-I' + str(libmd / 'include')]
    if arch == 'aarch64':
        flags += ['-static']
        if 'clang' in Path(cc).name:
            if not sysroot:
                raise ValueError('Cross clang requires an AArch64 --sysroot')
            flags += ['--target=aarch64-linux-gnu', '-fuse-ld=lld',
                      '--sysroot=' + str(sysroot.resolve()),
                      '--gcc-toolchain=' + str(sysroot.resolve() / 'usr')]
        elif sysroot:
            flags += ['--sysroot=' + str(sysroot.resolve())]
    binary = output / 'piano-storage'
    subprocess.run([cc, *flags, *map(str, sources), '-o', str(binary)], env=env, check=True)
    data = binary.read_bytes()
    if data[:7] != b'\x7fELF\x02\x01\x01' or (arch == 'aarch64' and struct.unpack_from('<H', data, 18)[0] != 183):
        raise ValueError('Unexpected storage executable architecture')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phsize, phcount = struct.unpack_from('<HH', data, 54)
    if arch == 'aarch64' and any(struct.unpack_from('<I', data, phoff + i * phsize)[0] == 3 for i in range(phcount)):
        raise ValueError('Android storage executable must be static')
    shutil.copy2(libmd / 'COPYING', output / 'COPYING.libmd')
    meta = {'schema_version': 1, 'arch': arch, 'device_write_performed': False,
            'commands': ['select', 'sources', 'plan', 'describe', 'validate', 'facts', 'apply', 'flash', 'check-project','check-root'],
            'binary': {'file': binary.name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()},
            'sources': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}}
    (output / 'manifest.json').write_text(json.dumps(meta, indent=2) + '\n')
    return meta


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/android-storage')
    parser.add_argument('--arch', choices=('host', 'aarch64'), default='aarch64')
    parser.add_argument('--cc')
    parser.add_argument('--sysroot', type=Path)
    args = parser.parse_args()
    print(json.dumps(build(args.output, args.arch, args.cc, args.sysroot), indent=2))
