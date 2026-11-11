"""Run package compilation natively while keeping the target's ABI and packaging."""
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
GUEST_TOOLS = '/opt/sunuefi-package-tools'


def default_jobs(memory_mib=1024):
    cpus = len(os.sched_getaffinity(0))
    quota = Path('/sys/fs/cgroup/cpu.max')
    if quota.is_file():
        value, period = quota.read_text().split()
        if value != 'max':
            cpus = min(cpus, max(1, (int(value) + int(period) - 1) // int(period)))
    meminfo = Path('/proc/meminfo')
    memory = (int(re.search(r'^MemAvailable:\s+(\d+)', meminfo.read_text(), re.M)[1]) * 1024
              if meminfo.is_file() else os.sysconf('SC_AVPHYS_PAGES') * os.sysconf('SC_PAGE_SIZE'))
    limit = Path('/sys/fs/cgroup/memory.max')
    used = Path('/sys/fs/cgroup/memory.current')
    if limit.is_file() and used.is_file() and limit.read_text().strip() != 'max':
        memory = min(memory, max(0, int(limit.read_text()) - int(used.read_text())))
    return max(1, min(cpus, (memory - 1024 ** 3) // (memory_mib * 1024 ** 2)))


def arguments(settings):
    return [key + '=' + value for key, value in settings.items()]


def run_cached(output, inputs, command, env=None, selection=None):
    """Reuse only complete binary outputs whose recipes and checksums still match."""
    output = Path(output)
    identity = {'files': {str(Path(path).relative_to(ROOT)): sha(path) for path in inputs},
                'selection': selection}
    marker = output / 'package-cache.json'
    if marker.is_file():
        record = json.loads(marker.read_text())
        if record.get('inputs') == identity and record.get('packages') and all(
                not Path(name).is_absolute() and '..' not in Path(name).parts and
                (output / name).is_file() and sha(output / name) == digest
                for name, digest in record['packages'].items()):
            print('Reusing completed packages: ' + str(output), flush=True)
            return
    marker.unlink(missing_ok=True)
    subprocess.run(list(map(str, command)), env=env, check=True)
    runtime = output / 'runtime' if (output / 'runtime').is_dir() else output
    packages = {str(path.relative_to(output)): sha(path)
                for path in runtime.rglob('*.deb') if path.is_file()}
    if not packages:
        raise ValueError('Package build produced no Debian packages: ' + str(output))
    marker.write_text(json.dumps({'inputs': identity, 'packages': packages}, indent=2) + '\n')


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def native_tools():
    """Copy native executables and their loader, never host headers or target libs."""
    names = ('clang', 'ld.lld', 'llvm-ar', 'llvm-ranlib', 'llvm-nm',
             'llvm-strip', 'llvm-objcopy', 'aarch64-linux-gnu-as', 'aarch64-linux-gnu-ld', 'make')
    selected = {name: Path(shutil.which(name) or '') for name in names}
    missing = [name for name, path in selected.items() if not path.is_file()]
    if missing:
        raise ValueError('Install native package build tools: ' + ', '.join(missing))
    if not shutil.which('patchelf'):
        raise ValueError('Install patchelf for the native package tool environment')
    for name in ('ccache', 'ninja'):
        if path := shutil.which(name):
            selected[name] = Path(path)
    resource = Path(subprocess.check_output(
        [selected['clang'], '-print-resource-dir'], text=True).strip()) / 'include'
    files, loader = {}, None
    for path in selected.values():
        text = subprocess.check_output(['ldd', path.resolve()], text=True)
        for line in text.splitlines():
            match = re.search(r'(?:=>\s+)?(/\S+)\s+\(', line)
            if not match:
                continue
            library = Path(match[1])
            if library.name.startswith(('ld-linux', 'ld-musl')):
                loader = library
            else:
                if library.name in files and files[library.name].resolve() != library.resolve():
                    raise ValueError('Conflicting native tool libraries: ' + library.name)
                files[library.name] = library
    if loader is None:
        raise ValueError('Cannot find the native executable loader')
    identity = {name: sha(path.resolve()) for name, path in selected.items()}
    identity['loader'] = sha(loader.resolve())
    identity['libraries'] = {name: sha(path.resolve()) for name, path in files.items()}
    identity['headers'] = {p.relative_to(resource).as_posix(): sha(p)
                           for p in resource.rglob('*') if p.is_file()}
    identity['builder'] = sha(__file__)
    key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:16]
    destination = ROOT / 'build/host-package-tools' / key
    if (destination / 'identity.json').is_file():
        saved = json.loads((destination / 'identity.json').read_text())
        if not saved.get('outputs') or any(not (destination / name).is_file() or sha(destination / name) != digest
               for name, digest in saved.get('outputs', {}).items()):
            raise ValueError('Prepared native package tools changed')
        return destination
    destination.mkdir(parents=True, exist_ok=True)
    for folder in ('bin', 'tools', 'lib'):
        (destination / folder).mkdir(exist_ok=True)
    for name, path in selected.items():
        shutil.copy2(path.resolve(), destination / 'tools' / name)
    shutil.copy2(loader.resolve(), destination / 'lib/loader')
    for name, path in files.items():
        shutil.copy2(path.resolve(), destination / 'lib' / name)
    shutil.copytree(resource, destination / 'include', dirs_exist_ok=True)
    prefix = Path(GUEST_TOOLS)
    for name in selected:
        subprocess.run(['patchelf', '--set-interpreter', str(prefix / 'lib/loader'),
                        '--force-rpath', '--set-rpath', str(prefix / 'lib'),
                        str(destination / 'tools' / name)], check=True)

    def wrapper(name, args):
        path = destination / 'bin' / name
        path.write_text('#!/bin/sh\nexec ' + shlex.join(args) + ' "$@"\n')
        path.chmod(0o755)

    flags = ['--target=aarch64-linux-gnu', '--sysroot=/', '--gcc-toolchain=/usr',
             '-resource-dir=' + str(prefix), '-fuse-ld=bfd', '-fno-integrated-as',
             '-B' + str(prefix / 'bin'), '-Wno-unused-command-line-argument', '-gdwarf-4']
    wrapper('clang-driver', [str(prefix / 'tools/clang'), *flags])
    for name, cxx in [('clang', False), ('clang++', True)]:
        command = [str(prefix / 'bin/clang-driver')]
        if cxx:
            command += ['--driver-mode=g++']
        if 'ccache' in selected:
            command = [str(prefix / 'tools/ccache'), *command]
        wrapper(name, command)
    for name in selected.keys() - {'clang', 'ccache'}:
        wrapper(name, [str(prefix / 'tools' / name)])
    aliases = {'cc': 'clang', 'gcc': 'clang', 'c++': 'clang++', 'g++': 'clang++',
               'ar': 'llvm-ar', 'ranlib': 'llvm-ranlib', 'nm': 'llvm-nm',
                'strip': 'llvm-strip', 'objcopy': 'llvm-objcopy'}
    for name, target in aliases.items():
        (destination / 'bin' / name).symlink_to(target)
        (destination / 'bin' / ('aarch64-linux-gnu-' + name)).symlink_to(target)
    for name in ('ld', 'ld.bfd', 'aarch64-linux-gnu-ld.bfd'):
        (destination / 'bin' / name).symlink_to('aarch64-linux-gnu-ld')
    identity['outputs'] = {str(path.relative_to(destination)): sha(path)
                           for path in destination.rglob('*') if path.is_file() and not path.is_symlink()}
    (destination / 'identity.json').write_text(json.dumps(identity, sort_keys=True) + '\n')
    return destination


@contextmanager
def environment(rootfs):
    """Only compiler/linker/make run as host ELF; target programs may use binfmt."""
    rootfs = Path(rootfs).resolve()
    cross = platform.machine() not in ('aarch64', 'arm64')
    if cross and rootfs == Path('/'):
        raise ValueError('ARM64 package builds on x86 need --sysroot; QEMU compilation is not used')
    cache = Path(os.environ.get('CCACHE_DIR', ROOT / 'build/ccache')).resolve()
    cache.mkdir(parents=True, exist_ok=True)
    # makepkg runs as an unprivileged build user; no product files use this mode.
    cache.chmod(0o1777)
    mounts = []

    def bind(source, target):
        target.mkdir(parents=True, exist_ok=True)
        subprocess.run(['mount', '--bind', str(source), str(target)], check=True)
        mounts.append(target)

    try:
        if rootfs != Path('/') and not os.path.ismount(rootfs / 'proc'):
            proc = rootfs / 'proc'
            proc.mkdir(exist_ok=True)
            subprocess.run(['mount', '-t', 'proc', '-o', 'nosuid,nodev,noexec', 'proc', str(proc)], check=True)
            mounts.append(proc)
        cache_path = str(cache)
        path = '/usr/lib/ccache:/usr/lib/ccache/bin:/usr/sbin:/usr/bin:/sbin:/bin'
        result = {'CCACHE_MAXSIZE': os.environ.get('CCACHE_MAXSIZE', '8G'),
                  'CCACHE_UMASK': '000', 'CCACHE_COMPILERCHECK': 'content'}
        if rootfs != Path('/'):
            cache_path = '/var/cache/sunuefi-ccache'
            bind(cache, rootfs / cache_path.lstrip('/'))
        result['CCACHE_DIR'] = cache_path
        if cross:
            bind(native_tools(), rootfs / GUEST_TOOLS.lstrip('/'))
            path = GUEST_TOOLS + '/bin:' + path
            result.update(CC='clang', CXX='clang++', CCACHE_COMPILERTYPE='clang',
                          CCACHE_EXTRAFILES=GUEST_TOOLS + '/identity.json',
                          MAKE=GUEST_TOOLS + '/bin/make')
        result['PATH'] = path
        yield result
    finally:
        for target in reversed(mounts):
            subprocess.run(['umount', str(target)], check=True)
