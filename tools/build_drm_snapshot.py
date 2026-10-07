#!/usr/bin/env python3
"""Build a static ARM64 read-only DRM snapshot tool for Android and Linux."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from make_kernel_initramfs import validate_static_arm64_elf

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def build(output, kernel_source, sysroot, cc):
    output = output.resolve()
    kernel_source = kernel_source.resolve()
    if not output.is_relative_to(ROOT / 'build') or output.exists():
        raise ValueError('Use a new project build output directory')
    if not kernel_source.is_relative_to(ROOT / 'build/kernel-worktrees'):
        raise ValueError('Export UAPI from a prepared kernel build worktree')
    env = os.environ.copy()
    bundled = ROOT / 'build/host-tools/usr'
    if (bundled / 'bin').is_dir():
        env['PATH'] = str(bundled / 'bin') + os.pathsep + env.get('PATH', '')
        env['LD_LIBRARY_PATH'] = str(bundled / 'lib') + os.pathsep + env.get('LD_LIBRARY_PATH', '')
    compiler = shutil.which(cc, path=env.get('PATH'))
    if not compiler:
        raise ValueError('ARM64 static compiler is missing')
    output.mkdir(parents=True)
    uapi = output / 'uapi'
    subprocess.run(['make', '-s', '-C', kernel_source, 'O=' + str(output / 'uapi-build'),
                    'ARCH=arm64', 'headers_install', 'INSTALL_HDR_PATH=' + str(uapi)],
                   env=env, check=True)
    binary = output / 'piano-drm-snapshot'
    source = ROOT / 'tools/android/piano_drm_snapshot.c'
    flags = ['-static', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(uapi / 'include')]
    if 'clang' in Path(compiler).name:
        flags += ['--target=aarch64-linux-gnu', '-fuse-ld=lld']
        if sysroot:
            flags += ['--sysroot=' + str(sysroot.resolve()), '--gcc-toolchain=' + str(sysroot.resolve() / 'usr')]
    elif sysroot:
        flags += ['--sysroot=' + str(sysroot.resolve())]
    subprocess.run([compiler, *flags, source, '-o', binary], env=env, check=True)
    validate_static_arm64_elf(binary.read_bytes())
    metadata = {'status': 'HOST_BUILT_DRM_SNAPSHOT_NOT_DEVICE_VERIFIED',
                'source_sha256': digest(source),
                'kernel_commit': subprocess.check_output(['git', '-C', kernel_source, 'rev-parse', 'HEAD'], text=True).strip(),
                'uapi': {str(p.relative_to(uapi)): digest(p) for p in sorted((uapi / 'include/drm').glob('*.h'))},
                'compiler': subprocess.check_output([compiler, '--version'], env=env, text=True).splitlines()[0],
                'flags': flags, 'binary': {'file': binary.name, 'bytes': binary.stat().st_size, 'sha256': digest(binary)},
                'device_operation_performed': False, 'modeset_implemented': False}
    (output / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return metadata


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/android-drm-snapshot')
    parser.add_argument('--kernel-source', type=Path, required=True)
    parser.add_argument('--sysroot', type=Path)
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args()
    try:
        print(json.dumps(build(args.output, args.kernel_source, args.sysroot, args.cc), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(2, str(error) + '\n')
