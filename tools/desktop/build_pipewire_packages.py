#!/usr/bin/env python3
"""Build native PipeWire packages for Debian, Arch Linux ARM and SteamOS."""
import argparse
from functools import partial
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build_piano_runtime_helpers import source_patches
from build import overlay_release
from desktop import debian_package as debian, package_build

DEBIAN_REVISION = 7
DEBIAN_LIBCAMERA_REVISION = 10
DEBIAN_V4L2_TRANSITION = 'pipewire-v4l2-source'
DEBIAN_DEFAULT = frozenset((
    'gstreamer1.0-pipewire', 'libpipewire-0.3-0t64', 'libpipewire-0.3-common',
    'libpipewire-0.3-modules', 'libspa-0.2-bluetooth', 'libspa-0.2-modules',
    'pipewire', 'pipewire-alsa', 'pipewire-audio', 'pipewire-bin', 'pipewire-pulse',
))
# Existing optional packages still need the same version during an APT upgrade.
DEBIAN_PACKAGES = {
    **dict.fromkeys((
        'gstreamer1.0-pipewire', 'libpipewire-0.3-0t64', 'libpipewire-0.3-dev',
        'libpipewire-0.3-modules', 'libpipewire-0.3-modules-x11',
        'libspa-0.2-bluetooth', 'libspa-0.2-dev', 'libspa-0.2-jack',
        'libspa-0.2-libcamera', 'libspa-0.2-modules', 'pipewire', 'pipewire-alsa',
        'pipewire-bin', 'pipewire-jack', 'pipewire-pulse', 'pipewire-tests', 'pipewire-v4l2',
    ), 'arm64'),
    **dict.fromkeys((
        'libpipewire-0.3-common', 'pipewire-audio', 'pipewire-audio-client-libraries',
        'pipewire-doc', 'pipewire-libcamera', 'pipewire-system-services', DEBIAN_V4L2_TRANSITION,
    ), 'all'),
}


def patch_series(version):
    upstream = version.split(':')[-1].split('-', 1)[0]
    series = ROOT / 'patches/pipewire' / upstream / 'series.json'
    if not series.is_file():
        raise ValueError('Check the patches against the selected PipeWire version: ' + version)
    return series


def arch_policy(version):
    series = patch_series(version)
    data = json.loads(series.read_text())
    if {'steamos', 'nyarch-gnome'}.isdisjoint(data['targets']):
        raise ValueError('Select the PipeWire patch series for pacman')
    patches = source_patches(series)
    return data, [{'path': p.relative_to(ROOT).as_posix(), 'sha256': debian.sha(p)} for p in patches]


def debian_series(version):
    series = patch_series(version)
    data = json.loads(series.read_text())
    if not {'debian-gnome', 'debian-kde'} <= set(data['targets']):
        raise ValueError('Select the PipeWire patch series for Debian')
    return series, data, [path.resolve() for path in source_patches(series)]


def debian_inputs(root, output):
    # The server package has systemd units; the shared library carries the ABI.
    target = debian.target_identity(root, debian.PIPEWIRE_ABI_PACKAGE, package_output=output)
    series, data, patches = debian_series(target['binary']['source_version'])
    archive = next(row for name, row in target['source_checksums'].items() if '.orig.tar.' in name)
    if archive['sha256'] != data['source']['sha256']:
        raise ValueError('PipeWire patches are not based on the signed Debian source')
    return target, series, patches


def libcamera_inputs(directory):
    if directory is None:
        return (), None
    directory = Path(directory).resolve()
    record = json.loads((directory / 'SOURCE.json').read_text())
    packages = record.get('packages', {})
    libraries = [name for name in packages if re.fullmatch(r'libcamera[0-9]+\.[0-9]+', name)]
    version = record.get('package_version', '')
    if (record.get('packages_built') is not True or (directory / '.incomplete').exists() or
            record.get('source', {}).get('package') != 'libcamera' or len(libraries) != 1 or
            not re.fullmatch(r'[0-9][a-zA-Z0-9:.+~-]*', version)):
        raise ValueError('Use a completed Debian libcamera build')
    library, = libraries
    ipa = 'libcamera-ipa-' + library.removeprefix('libcamera')
    archives = []
    checksums = {}
    for name in (library, 'libcamera-dev', ipa):
        row = packages.get(name, {})
        filename = f'{name}_{version.split(":")[-1]}_arm64.deb'
        if (row.get('file'), row.get('version'), row.get('architecture')) != (filename, version, 'arm64'):
            raise ValueError('Missing matching Debian libcamera package: ' + name)
        archive = directory / 'runtime' / filename
        if archive.is_symlink() or not archive.is_file():
            raise ValueError('Missing Debian libcamera archive: ' + filename)
        archives.append(archive)
        checksums[name] = {'version': version, 'architecture': 'arm64', 'sha256': row['sha256']}
    if debian.build_dependency_records(archives) != checksums:
        raise ValueError('Debian libcamera archive differs from its build record')
    return archives, library


def verify_libcamera_plugin(output, record, library):
    plugin = 'libspa-0.2-libcamera'
    archive = output / 'runtime' / record['packages'][plugin]['file']
    depends = debian.capture(['dpkg-deb', '-f', archive, 'Depends'])
    if not any(re.fullmatch(rf'{re.escape(library)}(?: \([^)]+\))?', relation.strip())
               for relation in depends.split(',')):
        raise ValueError('The PipeWire camera plugin depends on a different libcamera package')
    with tempfile.TemporaryDirectory(prefix='pipewire-libcamera-') as directory:
        debian.run(['dpkg-deb', '-x', archive, directory])
        binary = Path(directory) / 'usr/lib/aarch64-linux-gnu/spa-0.2/libcamera/libspa-libcamera.so'
        dynamic = debian.capture(['readelf', '-d', binary])
    linked = set(re.findall(r'\(NEEDED\).*?\[(libcamera[^]]+)\]', dynamic))
    abi = library.removeprefix('libcamera')
    if (f'libcamera.so.{abi}' not in linked or
            linked - {f'libcamera.so.{abi}', f'libcamera-base.so.{abi}'}):
        raise ValueError('The PipeWire camera plugin links a different libcamera ABI')


def reusable_debian(output, root, *, libcamera=None):
    target, series, patches = debian_inputs(root, output)
    proof = [{'path': path.relative_to(ROOT).as_posix(), 'sha256': debian.sha(path)} for path in patches]
    inputs = debian.input_records([Path(__file__), series, Path(debian.__file__),
                                   Path(package_build.__file__)])
    archives, library = libcamera_inputs(libcamera)
    revision = DEBIAN_LIBCAMERA_REVISION if library else DEBIAN_REVISION
    verify = partial(verify_libcamera_plugin, library=library) if library else None
    return debian.reusable(output, target, proof, DEBIAN_PACKAGES, inputs, revision,
                           build_dependencies=debian.build_dependency_records(archives),
                           validate_packages=verify)


def verify_debian_packages(record):
    version = record.get('package_version')
    packages = record['packages']
    if (not version or set(packages) != set(DEBIAN_PACKAGES) or
            any(row.get('version') != version or row.get('architecture') != DEBIAN_PACKAGES[name]
                for name, row in packages.items())):
        raise ValueError('The Debian PipeWire set must contain all native packages at the same version')


def test_debian_build(guest, source, _settings):
    # Keep the focused GStreamer tests from the previous plugin build.
    guest(['sh', '-ec', 'cd "$1"; build="obj-$(dpkg-architecture -qDEB_HOST_GNU_TYPE)"; '
           'GST_PLUGIN_SYSTEM_PATH_1_0= GST_PLUGIN_PATH_1_0= GST_REGISTRY_FORK=no '
           'GST_REGISTRY_1_0="$PWD/$build/test-registry.bin" '
           'meson test -C "$build" --num-processes 1 --no-rebuild --print-errorlogs '
           '--suite gstreamer', 'test', source])


def build_debian(root, output, jobs, *, libcamera=None):
    _, series, patches = debian_inputs(root, output)
    archives, library = libcamera_inputs(libcamera)
    revision = DEBIAN_LIBCAMERA_REVISION if library else DEBIAN_REVISION
    verify = partial(verify_libcamera_plugin, library=library) if library else None
    return debian.build(output, root, jobs, 'pipewire', patches, DEBIAN_PACKAGES,
                        [Path(__file__), series], revision=revision,
                        binary_package=debian.PIPEWIRE_ABI_PACKAGE,
                        test_build=test_debian_build, build_dependencies=archives,
                        validate_packages=verify)


def build(root, output, jobs=None, *, libcamera=None):
    jobs = package_build.default_jobs() if jobs is None else jobs
    root, output = root.resolve(), output.resolve()
    if (os.geteuid() or not root.is_relative_to(ROOT / 'build') or not root.is_dir() or
            not output.is_relative_to(ROOT / 'build') or output.is_relative_to(root) or
            root.is_relative_to(output) or jobs < 1):
        raise ValueError('Use root, project build directories outside each other, and at least one job')
    if (root / 'var/lib/dpkg/status').is_file():
        output.mkdir(parents=True, exist_ok=True)
        return build_debian(root, output, jobs, libcamera=libcamera)
    if libcamera is None:
        raise ValueError('--libcamera-packages is required for Arch Linux ARM and SteamOS')
    from desktop import pipewire_arch

    return pipewire_arch.build(root, output, jobs, libcamera)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rootfs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--libcamera-packages', type=Path, metavar='DIR',
                        help='Build against completed libcamera packages for the same distribution')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        raise SystemExit(subprocess.run(['unshare', '--mount', '--propagation', 'private',
                                        sys.executable, __file__, *sys.argv[1:], '--worker']).returncode)
    print(json.dumps(build(args.rootfs, args.output, args.jobs,
                           libcamera=args.libcamera_packages), indent=2))
