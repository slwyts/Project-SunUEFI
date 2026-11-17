#!/usr/bin/env python3
"""Build libwacom with SPI digitizer recognition for native tablet settings."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_arch_package
from build_piano_runtime_helpers import source_patches
from desktop import debian_package, package_build

SERIES = ROOT / 'patches/libwacom/2.20/series.json'
PATCHES = source_patches(SERIES)
RUNTIME = {'libwacom9': 'arm64', 'libwacom-common': 'all', 'libwacom-bin': 'arm64'}
REVISION = 1
ARCH_ABI_PACKAGES = ('glibc', 'glib2', 'libevdev', 'libgudev', 'systemd-libs', 'libgcc')


def arch_target(root):
    packages = debian_package.capture(['chroot', root, 'pacman', '-Q', *ARCH_ABI_PACKAGES])
    return {'os_release': (root / 'etc/os-release').read_text(),
            'packages': dict(line.split(maxsplit=1) for line in packages.splitlines())}


def verify_arch_runtime(root):
    library = root / 'usr/lib/libwacom.so.9.0.0'
    header = library.read_bytes()[:20]
    if header[:6] != b'\x7fELF\x02\x01' or header[18:20] != b'\xb7\x00':
        raise ValueError('The pacman libwacom runtime must be ARM64')
    for path in ('/usr/lib/libwacom.so.9.0.0', '/usr/bin/libwacom-list-local-devices'):
        process = subprocess.run(['chroot', root, 'ldd', '-r', path], text=True, capture_output=True)
        if process.returncode or any(word in process.stdout + process.stderr
                                     for word in ('not found', 'undefined symbol')):
            raise ValueError('The libwacom runtime has unresolved target dependencies: ' + path)
    return debian_package.sha(library)


def selection(rootfs):
    target = debian_package.target_identity(rootfs, 'libwacom9')
    version = target['binary']['source_version'].split('-', 1)[0]
    if version not in json.loads(SERIES.read_text())['versions']:
        raise ValueError('Port the SPI digitizer support to the selected libwacom source: ' + version)
    return target


def reusable(output, rootfs):
    patches = [{'path': path.relative_to(ROOT).as_posix(), 'sha256': debian_package.sha(path)}
               for path in PATCHES]
    inputs = debian_package.input_records([Path(__file__), SERIES, Path(debian_package.__file__),
                                          Path(package_build.__file__)])
    return debian_package.reusable(Path(output), selection(rootfs), patches, RUNTIME, inputs, REVISION)


def validate_runtime(folder):
    library = (folder / 'usr/lib/aarch64-linux-gnu/libwacom.so.9.0.0').read_bytes()
    if library[:6] != b'\x7fELF\x02\x01' or library[18:20] != b'\xb7\x00':
        raise ValueError('libwacom must be an ARM64 ELF library')
    tablet = folder / 'usr/share/libwacom/xiaomi-pad-8-pro.tablet'
    if 'spi|0603|6532|Novatek Focus Pen Pro' not in tablet.read_text():
        raise ValueError('The libwacom runtime is missing the physical SPI digitizer description')


def build(output, sysroot, jobs, builder_root=None, install_dependencies=True):
    selection(sysroot or builder_root)
    return debian_package.build(output, sysroot, jobs, 'libwacom', PATCHES, RUNTIME,
                                [Path(__file__), SERIES], revision=REVISION,
                                builder_root=builder_root, install_dependencies=install_dependencies,
                                validate_runtime=validate_runtime, binary_package='libwacom9')


def build_arch(root, output, jobs):
    root, output = root.resolve(), output.resolve()
    if (os.geteuid() or not root.is_relative_to(ROOT / 'build/distros') or
            not output.is_relative_to(ROOT / 'build') or output.is_relative_to(root) or
            root.is_relative_to(output)):
        raise ValueError('Use root and separate project Arch rootfs and output directories')
    data = json.loads(SERIES.read_text())
    installed = debian_package.capture(['chroot', root, 'pacman', '-Q', 'libwacom']).split()[1]
    installed_version = installed.rsplit('-', 1)[0]
    if installed_version not in data['installed_versions']:
        raise ValueError('Check the selected libwacom ABI before replacing version ' + installed_version)
    version = data['version']
    target = arch_target(root)
    template = ROOT / 'linux/desktops/common/libwacom/PKGBUILD'
    inputs = debian_package.input_records([Path(__file__), SERIES, template, *PATCHES,
                                          Path(package_build.__file__)])
    metadata = output / 'SOURCE.json'
    if metadata.is_file():
        record = json.loads(metadata.read_text())
        if (record.get('inputs') == inputs and record.get('target') == target and record.get('packages') and
                build_arch_package.matches_release(output / 'runtime') and
                all(Path(name).name == name and
                    debian_package.sha(output / 'runtime' / name) == row['sha256']
                    for name, row in record['packages'].items())):
            build_arch_package.install_cached(root, output / 'runtime')
            if verify_arch_runtime(root) != record.get('library_sha256'):
                raise ValueError('The installed libwacom library differs from its package cache')
            return record
    output.mkdir(parents=True, exist_ok=True)
    recipe = output / 'recipe'
    recipe.mkdir(exist_ok=True)
    sources = [data['source']['url'], *[path.name for path in PATCHES]]
    checksums = [data['source']['sha256'], *[debian_package.sha(path) for path in PATCHES]]
    text = template.read_text().replace('@VERSION@', version)
    text = text.replace('@SOURCES@', ' '.join(map(shlex.quote, sources)))
    text = text.replace('@CHECKSUMS@', ' '.join(map(shlex.quote, checksums)))
    (recipe / 'PKGBUILD').write_text(text)
    for patch in PATCHES:
        (recipe / patch.name).write_bytes(patch.read_bytes())
    runtime = output / 'runtime'
    previous = debian_package.library_exports(root / 'usr/lib/libwacom.so.9')
    work = root / 'var/tmp/sunuefi-package-build'
    resume = work.is_dir() and (work / 'PKGBUILD').read_bytes() == (recipe / 'PKGBUILD').read_bytes()
    with (output / 'build.log').open('a') as log:
        subprocess.run([sys.executable, ROOT / 'tools/build_arch_package.py', '--rootfs', root,
                        '--recipe', recipe, '--output', runtime, '--jobs', str(jobs),
                        *(['--resume'] if resume else [])],
                       check=True, stdout=log, stderr=subprocess.STDOUT)
    record = json.loads((runtime / 'manifest.json').read_text())
    current = debian_package.library_exports(root / 'usr/lib/libwacom.so.9')
    if previous[0] != 'libwacom.so.9' or current[0] != previous[0] or not previous[1] <= current[1]:
        raise ValueError('The libwacom upgrade must preserve the target library ABI')
    record.update(inputs=inputs, source=data['source'], target=target,
                  library_sha256=verify_arch_runtime(root))
    metadata.write_text(json.dumps(record, indent=2) + '\n')
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--sysroot', type=Path, help='Debian ARM64 target; cloned before use')
    mode.add_argument('--builder-root', type=Path, help='Existing isolated sid ARM64 builder')
    mode.add_argument('--rootfs', type=Path, help='Arch ARM64 target; installs the built package')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/libwacom')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--skip-install-dependencies', action='store_true')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        raise SystemExit(subprocess.run(['unshare', '--mount', '--propagation', 'private',
                                        sys.executable, __file__, *sys.argv[1:], '--worker']).returncode)
    if args.rootfs:
        print(json.dumps(build_arch(args.rootfs, args.output, args.jobs), indent=2))
    else:
        print(json.dumps(build(args.output, args.sysroot, args.jobs, args.builder_root,
                               not args.skip_install_dependencies), indent=2))
