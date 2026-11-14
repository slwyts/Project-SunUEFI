#!/usr/bin/env python3
"""Build the fixed F2FS tools used by Android early expansion and image tests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SERIES = ROOT / 'patches/f2fs-tools/1.17.0/series.json'


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def fetch_commit(source, policy):
    commit = policy['base_commit']
    present = ['git', '-C', str(source), 'cat-file', '-e', commit + '^{commit}']
    if not subprocess.run(present, capture_output=True).returncode:
        return
    urls = [policy['source_url'], *policy.get('source_mirrors', [])]
    for _ in range(2):
        for url in urls:
            print('Fetching fixed F2FS tools from ' + url, flush=True)
            try:
                subprocess.run(['git', '-C', str(source), '-c', 'http.lowSpeedLimit=1024',
                                '-c', 'http.lowSpeedTime=30', 'fetch', '--no-tags',
                                '--depth=1', url, commit], check=True, timeout=90)
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
                continue
            if not subprocess.run(present, capture_output=True).returncode:
                return
    raise ValueError('Cannot fetch the fixed F2FS commit from its source or mirrors: ' + commit)


def build(output, sysroot=None, arch='aarch64', jobs=4):
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    policy = json.loads(SERIES.read_text())
    source = ROOT / 'build/source-cache/f2fs-tools'
    if not source.exists():
        source.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'init', str(source)], check=True)
    if subprocess.check_output(['git', '-C', str(source), 'rev-parse', '--show-toplevel'], text=True).strip() != str(source):
        raise ValueError('F2FS source cache is not an independent repository')
    if subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True):
        raise ValueError('F2FS source cache contains local changes')
    commit = policy['base_commit']
    fetch_commit(source, policy)
    patch_rows = []
    for row in policy['patches']:
        path = SERIES.parent / row['file']
        if path.resolve().parent != SERIES.parent or digest(path) != row['sha256']:
            raise ValueError('F2FS patch path or digest mismatch')
        patch_rows.append((path, row))
    workspace = output / ('source-' + digest(SERIES)[:12])
    if not workspace.exists():
        subprocess.run(['git', '-C', str(source), 'worktree', 'add', '--detach', str(workspace), commit], check=True)
        for path, _ in patch_rows:
            subprocess.run(['git', '-C', str(workspace), 'apply', '--check', '--index', str(path)], check=True)
            subprocess.run(['git', '-C', str(workspace), 'apply', '--index', str(path)], check=True)
        subprocess.run(['bash', 'autogen.sh'], cwd=workspace, check=True)
    if (subprocess.check_output(['git', '-C', str(workspace), 'rev-parse', 'HEAD'], text=True).strip() != commit or
        subprocess.check_output(['git', '-C', str(workspace), 'write-tree'], text=True).strip() != policy['target_tree'] or
        subprocess.run(['git', '-C', str(workspace), 'diff', '--quiet']).returncode):
        raise ValueError('Prepared F2FS source tree changed; preserve it for inspection')
    env = os.environ.copy()
    bundled = Path(os.environ.get('SUNUEFI_TOOLCHAIN_ROOT', ROOT / 'build/host-tools/usr'))
    env['PATH'] = str(bundled / 'bin') + os.pathsep + env.get('PATH', '')
    env['LD_LIBRARY_PATH'] = str(bundled / 'lib') + os.pathsep + env.get('LD_LIBRARY_PATH', '')
    arguments = ['--without-selinux', '--without-blkid', '--without-lzo2', '--without-lz4', '--disable-shared']
    if arch == 'aarch64':
        if sysroot is None:
            raise ValueError('ARM64 F2FS tools need the module development sysroot')
        sysroot = sysroot.resolve()
        compiler = shutil.which('clang', path=env['PATH'])
        if not compiler:
            raise ValueError('Clang is required for the ARM64 F2FS tools')
        env.update(CC=f'{compiler} --target=aarch64-linux-gnu -fuse-ld=lld '
                      f'--sysroot={sysroot} --gcc-toolchain={sysroot / "usr"}',
                   AR='aarch64-linux-gnu-ar', RANLIB='aarch64-linux-gnu-ranlib',
                   CFLAGS='-O2', LDFLAGS='-static',
                   PKG_CONFIG_SYSROOT_DIR=str(sysroot),
                   PKG_CONFIG_LIBDIR=str(sysroot / 'usr/lib/aarch64-linux-gnu/pkgconfig'))
        host = {'x86_64': 'x86_64', 'aarch64': 'aarch64'}.get(platform.machine())
        if not host:
            raise ValueError('Unsupported F2FS build host')
        arguments += ['--host=aarch64-linux-gnu', f'--build={host}-linux-gnu']
    objects = output / ('objects-' + digest(SERIES)[:12])
    objects.mkdir(exist_ok=True)
    with (output / 'build.log').open('w') as log:
        make = ['make', f'-j{jobs}']
        if arch == 'aarch64':
            make += ['LDFLAGS=-all-static']
            for name in ('fsck/fsck.f2fs', 'mkfs/mkfs.f2fs'):
                (objects / name).unlink(missing_ok=True)
        for command in ([str(workspace / 'configure'), *arguments], make):
            subprocess.run(command, cwd=objects, env=env, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    binaries = {}
    for name, relative in (('fsck.f2fs', 'fsck/fsck.f2fs'), ('mkfs.f2fs', 'mkfs/mkfs.f2fs')):
        executable = objects / relative
        if arch == 'aarch64':
            import sys
            sys.path.insert(0, str(ROOT / 'tools'))
            from build_android_module import arm64_executable
            arm64_executable(executable.read_bytes())
        shutil.copy2(executable, output / name)
        binaries[name] = {'bytes': executable.stat().st_size, 'sha256': digest(executable)}
    for name in ('resize.f2fs', 'sload.f2fs', 'dump.f2fs'):
        shutil.copy2(output / 'fsck.f2fs', output / name)
    shutil.copy2(workspace / 'COPYING', output / 'COPYING.f2fs-tools')
    record = {'source_url': policy['source_url'], 'source_commit': commit,
              'version': policy['version'], 'arch': arch, 'binaries': binaries,
              'series_sha256': digest(SERIES), 'patches': policy['patches']}
    (output / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/android-f2fs-tools')
    parser.add_argument('--sysroot', type=Path)
    parser.add_argument('--arch', choices=('aarch64', 'host'), default='aarch64')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    print(json.dumps(build(args.output, args.sysroot, args.arch, args.jobs), indent=2))
