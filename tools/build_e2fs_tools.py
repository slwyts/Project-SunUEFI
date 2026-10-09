#!/usr/bin/env python3
"""Build static ARM64 e2fsck/resize2fs for the Android module; never open a device.

The sysroot needs libc development files, uuid-dev and libblkid-dev. The
e2fsprogs source is fixed to the official 1.47.2 archive and checksum.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VERSION = '1.47.2'
ARCHIVE = f'e2fsprogs-{VERSION}.tar.xz'
URL = f'https://www.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v{VERSION}/{ARCHIVE}'
SHA256 = '08242e64ca0e8194d9c1caad49762b19209a06318199b63ce74ae4ef2d74e63c'


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def build(output, sysroot, cache, jobs):
    output, sysroot, cache = output.resolve(), sysroot.resolve(), cache.resolve()
    for name in ('libc.a', 'libuuid.a', 'libblkid.a'):
        if not (sysroot / 'usr/lib/aarch64-linux-gnu' / name).is_file():
            raise ValueError(f'Sysroot is missing {name}; install libc6-dev, uuid-dev and libblkid-dev')
    output.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / ARCHIVE
    if not archive.exists():
        temporary = archive.with_suffix(archive.suffix + '.download')
        with urllib.request.urlopen(URL, timeout=60) as source, temporary.open('wb') as target:
            shutil.copyfileobj(source, target)
        if digest(temporary) != SHA256:
            temporary.unlink()
            raise ValueError('Official e2fsprogs archive checksum differs')
        temporary.rename(archive)
    if digest(archive) != SHA256:
        raise ValueError('Cached e2fsprogs archive checksum differs')
    source = cache / f'e2fsprogs-{VERSION}'
    if not source.exists():
        with tarfile.open(archive) as contents:
            contents.extractall(cache, filter='data')
    workspace = output / 'workspace'
    workspace.mkdir(exist_ok=True)
    env = os.environ.copy()
    bundled = ROOT / 'build/host-tools/usr'
    env['PATH'] = str(bundled / 'bin') + os.pathsep + env.get('PATH', '')
    env['LD_LIBRARY_PATH'] = str(bundled / 'lib') + os.pathsep + env.get('LD_LIBRARY_PATH', '')
    compiler = shutil.which('clang', path=env['PATH'])
    if not compiler:
        raise ValueError('clang is required for the static ARM64 tools')
    env['CC'] = (f'{compiler} --target=aarch64-linux-gnu -fuse-ld=lld '
                 f'--sysroot={sysroot} --gcc-toolchain={sysroot / "usr"}')
    env['AR'] = 'aarch64-linux-gnu-ar'
    env['RANLIB'] = 'aarch64-linux-gnu-ranlib'
    env['CFLAGS'] = '-O2'
    env['LDFLAGS'] = '-static'
    env['PKG_CONFIG_SYSROOT_DIR'] = str(sysroot)
    env['PKG_CONFIG_LIBDIR'] = str(sysroot / 'usr/lib/aarch64-linux-gnu/pkgconfig')
    host_arch = {'x86_64': 'x86_64', 'aarch64': 'aarch64'}.get(platform.machine())
    if not host_arch:
        raise ValueError('Unsupported build host architecture')
    commands = [
        [str(source / 'configure'), '--host=aarch64-linux-gnu', f'--build={host_arch}-linux-gnu',
         '--disable-nls', '--disable-elf-shlibs', '--disable-bsd-shlibs', '--disable-fsck',
         '--disable-e2initrd-helper', '--without-crond-dir', '--without-systemd-unit-dir'],
        ['make', f'-j{jobs}', 'libs'],
        ['make', '-C', 'e2fsck', f'-j{jobs}', 'e2fsck.static'],
        ['make', '-C', 'resize', f'-j{jobs}', 'resize2fs.static'],
    ]
    with (output / 'build.log').open('w') as log:
        for command in commands:
            subprocess.run(command, cwd=workspace, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    binaries = {}
    for name, folder in (('e2fsck', 'e2fsck'), ('resize2fs', 'resize')):
        executable = workspace / folder / f'{name}.static'
        data = executable.read_bytes()
        if data[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
            raise ValueError(f'{name} is not an ARM64 ELF executable')
        offset = struct.unpack_from('<Q', data, 32)[0]
        size, count = struct.unpack_from('<HH', data, 54)
        if any(struct.unpack_from('<I', data, offset + i * size)[0] == 3 for i in range(count)):
            raise ValueError(f'{name} requires a dynamic loader')
        shutil.copy2(executable, output / name)
        binaries[name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    shutil.copy2(source / 'NOTICE', output / 'NOTICE.e2fsprogs')
    meta = {'schema_version': 1, 'version': VERSION, 'source': URL,
            'source_sha256': SHA256, 'arch': 'aarch64', 'static': True,
            'device_write_performed': False, 'binaries': binaries,
            'sysroot_libraries': {
                name: digest(sysroot / 'usr/lib/aarch64-linux-gnu' / name)
                for name in ('libc.a', 'libuuid.a', 'libblkid.a')
            }}
    (output / 'manifest.json').write_text(json.dumps(meta, indent=2) + '\n')
    return meta


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sysroot', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/android-e2fs-tools')
    parser.add_argument('--cache', type=Path, default=ROOT / 'build/source-cache/e2fsprogs')
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 2, 8))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    print(json.dumps(build(args.output, args.sysroot, args.cache, args.jobs), indent=2))
