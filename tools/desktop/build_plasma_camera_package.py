#!/usr/bin/env python3
"""Build native ARM64 Plasma Camera against the packaged libcamera."""
import argparse
import fcntl
from functools import partial
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_arch_package
from build import overlay_packages, overlay_release
from build_piano_ram_bootstrap import guest_resolve
from build_piano_runtime_helpers import source_patches
from desktop import debian_package, libcamera_arch, package_build
from desktop.build_pipewire_packages import libcamera_inputs
from prepare_steamos_rootfs import download

PACKAGE = 'plasma-camera'
RUNTIME = {PACKAGE: 'arm64'}
REVISION = 4
CONFIG = ROOT / 'config/plasma-camera.json'
ARCH_RECIPE_PATCH = ROOT / 'linux/desktops/common/plasma-camera/PKGBUILD.patch'


def selection(sysroot, output):
    target = debian_package.target_identity(sysroot, PACKAGE, package_output=output)
    upstream = target['binary']['source_version'].split(':')[-1].rsplit('-', 1)[0]
    series = ROOT / 'patches/plasma-camera' / upstream / 'series.json'
    if not series.is_file():
        raise ValueError('Check the patches against the selected Plasma Camera source: ' + upstream)
    return target, series, source_patches(series)


def verify_runtime(output, record, library):
    archive = output / 'runtime' / record['packages'][PACKAGE]['file']
    depends = debian_package.capture(['dpkg-deb', '-f', archive, 'Depends'])
    if not any(re.fullmatch(rf'{re.escape(library)}(?: \([^)]+\))?', relation.strip())
               for relation in depends.split(',')):
        raise ValueError('Plasma Camera does not depend on the selected libcamera package')
    with tempfile.TemporaryDirectory(prefix='plasma-camera-runtime-') as directory:
        debian_package.run(['dpkg-deb', '-x', archive, directory])
        verify_camera_elf(Path(directory) / 'usr/bin/plasma-camera', library.removeprefix('libcamera'))


def verify_camera_elf(binary, abi):
    elf = debian_package.capture(['readelf', '--file-header', '--dynamic', '--wide', binary])
    soname = f'libcamera.so.{abi}'
    linked = set(re.findall(r'\(NEEDED\).*?\[(libcamera[^]]+)\]', elf))
    if (not re.search(r'(?m)^\s*Machine:\s*AArch64\s*$', elf) or
            soname not in linked or linked - {soname, f'libcamera-base.so.{abi}'} or
            re.search(r'\((?:RPATH|RUNPATH)\)', elf)):
        raise ValueError('Plasma Camera must be ARM64 and link the selected libcamera ABI from the system')


def camera_dependencies(libcamera):
    archives, library = libcamera_inputs(libcamera)
    if not library:
        raise ValueError('Plasma Camera needs a completed Debian libcamera build')
    return archives, library


def reusable(output, sysroot, *, libcamera):
    output = Path(output).resolve()
    target, series, patches = selection(sysroot, output)
    archives, library = camera_dependencies(libcamera)
    proof = [{'path': path.relative_to(ROOT).as_posix(), 'sha256': debian_package.sha(path)}
             for path in patches]
    inputs = debian_package.input_records([Path(__file__), series, Path(debian_package.__file__),
                                           Path(package_build.__file__)])
    return debian_package.reusable(output, target, proof, RUNTIME, inputs, REVISION,
                                   build_dependencies=debian_package.build_dependency_records(archives),
                                   validate_packages=partial(verify_runtime, library=library))


def build(output, sysroot, jobs=None, *, libcamera):
    output, sysroot = Path(output).resolve(), Path(sysroot).resolve()
    _, series, patches = selection(sysroot, output)
    archives, library = camera_dependencies(libcamera)
    jobs = package_build.default_jobs() if jobs is None else jobs
    with debian_package.use_build_archive(debian_package.archive_inputs(sysroot)):
        return debian_package.build(output, sysroot, jobs, PACKAGE, patches, RUNTIME,
                                    [Path(__file__), series], revision=REVISION,
                                    build_dependencies=archives,
                                    # Kirigami's CMake config needs Debian's libgomp.
                                    build_environment=debian_package.gcc_environment,
                                    validate_packages=partial(verify_runtime, library=library))


def verify_installed_arch_camera(root, archives, proof):
    result = subprocess.run(['chroot', root, 'pacman', '-Q', *proof], capture_output=True, text=True)
    versions = dict(line.split(maxsplit=1) for line in result.stdout.splitlines())
    if result.returncode or versions != {name: row['version'] for name, row in proof.items()}:
        raise ValueError('Install the selected Arch libcamera and IPA together in the build root')
    with tempfile.TemporaryDirectory(prefix='plasma-camera-libcamera-') as directory:
        tree = Path(directory)
        for archive in archives:
            debian_package.run(['bsdtar', '-xf', archive, '-C', tree])
        # Two builds at the same package version may have different IPA keys.
        for expected in (tree / 'usr').rglob('*'):
            name = expected.relative_to(tree).as_posix()
            actual = root / name
            if expected.is_symlink():
                matches = actual.is_symlink() and actual.readlink() == expected.readlink()
            elif expected.is_file():
                actual = guest_resolve(root, name)
                matches = actual.is_file() and package_build.sha(actual) == package_build.sha(expected)
            else:
                continue
            if not matches:
                raise ValueError('Installed Arch libcamera differs from its selected build: /' + name)


def prepare_arch(recipe, data, patches):
    source = data['source']
    cache = ROOT / 'build/source-cache/plasma-camera'
    cache.mkdir(parents=True, exist_ok=True)
    name = source['url'].rsplit('/', 1)[-1]
    archive = cache / (source['sha256'] + '.tar.xz')
    signature = cache / (source['signature_sha256'] + '.sig')
    key = cache / (source['key_sha256'] + '.asc')
    upstream = cache / (data['recipe_sha256'] + '-PKGBUILD')
    with (cache / 'download.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        download(data['recipe_url'], upstream, data['recipe_sha256'])
        download(source['url'], archive, source['sha256'])
        download(source['url'] + '.sig', signature, source['signature_sha256'])
        download(source['key_url'], key, source['key_sha256'])
    with tempfile.TemporaryDirectory(prefix='plasma-camera-signature-') as directory:
        keyring = Path(directory) / 'release.gpg'
        debian_package.run(['gpg', '--batch', '--homedir', directory,
                            '--dearmor', '--output', keyring, key])
        status = debian_package.capture(['gpgv', '--homedir', directory, '--status-fd', '1',
                                         '--keyring', keyring, signature, archive])
        if not any(line.startswith(f"[GNUPG:] VALIDSIG {source['release_signer']} ")
                   for line in status.splitlines()):
            raise ValueError('Plasma Camera source signature is not from the selected KDE release key')

    recipe.mkdir(parents=True, exist_ok=True)
    shutil.copy2(archive, recipe / name)
    for path in patches:
        shutil.copy2(path, recipe / path.name)
    text = debian_package.capture(['patch', '--silent', '--batch', '--fuzz=0', '-o', '-', upstream,
                                   '-i', ARCH_RECIPE_PATCH])
    values = {'RELEASE': data['package_release'], 'SOURCE': shlex.quote(name),
              'PATCHES': shlex.join([path.name for path in patches]),
              'CHECKSUMS': shlex.join([source['sha256'], *(package_build.sha(path) for path in patches)]),
              'PACKAGE_TOOLS': package_build.GUEST_TOOLS}
    for token, value in values.items():
        text = text.replace('@' + token + '@', value)
    (recipe / 'PKGBUILD').write_text(text + '\n')


def verify_arch_runtime(runtime, manifest, version, abi):
    packages = manifest['packages']
    if (manifest.get('package_format') != 'pacman' or manifest.get('architecture') != 'aarch64' or
            len(packages) != 1):
        raise ValueError('The Arch build must produce one Plasma Camera package')
    filename, = packages
    archive = runtime / filename
    row = packages[filename]
    if (Path(filename).name != filename or archive.is_symlink() or
            package_build.sha(archive) != row['sha256'] or
            overlay_packages.pacman_identity(archive) != (PACKAGE, version, 'aarch64')):
        raise ValueError('Unexpected Arch Plasma Camera package: ' + filename)
    dependencies = set(overlay_packages.pacman_info(archive).get('depend', ()))
    if not {*libcamera_arch.sonames(abi), 'qt6-multimedia-ffmpeg'} <= dependencies:
        raise ValueError('Arch Plasma Camera must require the selected libcamera ABI and FFmpeg backend')
    with tempfile.TemporaryDirectory(prefix='plasma-camera-runtime-') as directory:
        debian_package.run(['bsdtar', '-xf', archive, '-C', directory, 'usr/bin/plasma-camera',
                            'usr/share/applications/org.kde.plasma.camera.desktop',
                            'usr/share/metainfo/org.kde.plasma.camera.appdata.xml'])
        verify_camera_elf(Path(directory) / 'usr/bin/plasma-camera', abi)
    return {PACKAGE: {'file': filename, 'sha256': row['sha256'],
                      'architecture': 'aarch64', 'version': version}}


def build_arch(output, root, jobs, *, libcamera):
    root, output, libcamera = (Path(path).resolve() for path in (root, output, libcamera))
    if (os.geteuid() or jobs < 1 or not root.is_relative_to(ROOT / 'build/distros') or
            not output.is_relative_to(ROOT / 'build') or root.is_relative_to(output) or
            output.is_relative_to(root) or output.is_relative_to(libcamera) or
            libcamera.is_relative_to(output)):
        raise ValueError('Use root, an isolated Arch root, a separate output and at least one job')
    release = guest_resolve(root, '/etc/os-release').read_text().splitlines()
    distro = next((line[3:].strip('"\'') for line in release if line.startswith('ID=')), '')
    if distro not in ('archarm', 'archlinuxarm'):
        raise ValueError('The Plasma Camera recipe is for Arch Linux ARM')
    data = json.loads(CONFIG.read_text())['arch']
    upstream, _ = data['upstream_package_version'].rsplit('-', 1)
    index = debian_package.capture(['chroot', root, 'env', 'LC_ALL=C', 'pacman', '-Si',
                                    'extra/' + PACKAGE])
    if re.findall(r'(?m)^Version\s*:\s*(\S+)\s*$', index) != [data['upstream_package_version']]:
        raise ValueError('Check the Plasma Camera patch series against the selected Arch archive')
    series = ROOT / 'patches/plasma-camera' / upstream / 'series.json'
    patches = [path.resolve() for path in source_patches(series)]
    camera_runtime, _, abi, proof = libcamera_arch.runtime_inputs(libcamera)
    archives = [camera_runtime / row['file'] for row in proof.values()]
    verify_installed_arch_camera(root, archives, proof)
    output.mkdir(parents=True, exist_ok=True)
    with (output / 'build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        incomplete = output / '.incomplete'
        incomplete.write_text('Arch Plasma Camera build has not completed.\n')
        recipe, runtime = output / 'recipe', output / 'runtime'
        prepare_arch(recipe, data, patches)
        version = upstream + '-' + overlay_release.pacman_release(data['package_release'])

        build_arch_package.build(root, recipe, runtime, jobs=jobs, install=False)
        verify_installed_arch_camera(root, archives, proof)
        manifest = json.loads((runtime / 'manifest.json').read_text())
        packages = verify_arch_runtime(runtime, manifest, version, abi)
        build_arch_package.install_cached(root, runtime)
        if debian_package.capture(['chroot', root, 'pacman', '-Q', PACKAGE]) != f'{PACKAGE} {version}':
            raise ValueError('Installed Plasma Camera does not match the built package')
        record = {'packages_built': True, 'package_version': version, 'package_format': 'pacman',
                  'source': data, 'packages': packages, 'build_dependencies': proof,
                  'patches': [{'path': path.relative_to(ROOT).as_posix(),
                               'sha256': package_build.sha(path)} for path in patches],
                  'recipe_sha256': manifest['recipe_sha256']}
        (output / 'SOURCE.json').write_text(json.dumps(record, indent=2) + '\n')
        incomplete.unlink()
        return record


def main():
    parser = argparse.ArgumentParser(prog='./build.sh plasma-camera', description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--sysroot', type=Path, help='Debian sid ARM64 target')
    mode.add_argument('--rootfs', type=Path, help='Arch Linux ARM build root with the selected libcamera and IPA')
    parser.add_argument('--libcamera-packages', type=Path, required=True, help='Completed matching libcamera build')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        return subprocess.run(['unshare', '--mount', '--propagation', 'private',
                               sys.executable, __file__, *sys.argv[1:], '--worker']).returncode
    if args.rootfs:
        try:
            record = build_arch(args.output or ROOT / 'build/plasma-camera-arch', args.rootfs, args.jobs,
                                libcamera=args.libcamera_packages)
        except (OSError, ValueError, subprocess.CalledProcessError) as exc:
            parser.exit(1, f'{exc}\n')
    else:
        record = build(args.output or ROOT / 'build/plasma-camera', args.sysroot, args.jobs,
                       libcamera=args.libcamera_packages)
    print(json.dumps(record, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
