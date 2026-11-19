#!/usr/bin/env python3
"""Build the Debian PipeWire package set or plugins for the installed pacman ABI."""
import argparse
from functools import partial
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_arch_package
from build_piano_runtime_helpers import source_patches
from build import overlay_release
from prepare_steamos_rootfs import download
from desktop import debian_package as debian, package_build

GST_VIDEO_MIN_VERSION = '1.22.0'
MODULES = ('glib-2.0', 'gobject-2.0', 'gmodule-2.0', 'gio-2.0', 'gio-unix-2.0',
           'gstreamer-1.0', 'gstreamer-base-1.0',
           'gstreamer-audio-1.0', 'gstreamer-allocators-1.0')
DEBIAN_REVISION = 3
DEBIAN_LIBCAMERA_REVISION = 4
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


def pacman_release(version):
    release = version.rsplit('-', 1)[1]
    major, _, minor = release.partition('.')
    return f'{major}.{int(minor or 0) + 2}'


def package_version(version, data):
    epoch = version.split(':', 1)[0] if ':' in version else '0'
    return ((epoch + ':' if epoch != '0' else '') + data['version'] + '-' +
            overlay_release.pacman_release(pacman_release(version)))


def reusable_arch(output, version):
    try:
        record = json.loads((output / 'SOURCE.json').read_text())
        data, patches = arch_policy(version)
        if (record['upstream_package_version'] != version or record['patches'] != patches or
                record['archive_sha256'] != data['source']['sha256'] or
                record.get('producer_sha256') != debian.sha(__file__) or
                record['package_format'] != 'pacman' or not record['packages_built']):
            return None
        if set(record['packages']) != {'gst-plugin-pipewire', 'pipewire-v4l2-source'}:
            return None
        expected_version = package_version(version, data)
        for row in record['packages'].values():
            if (Path(row['file']).name != row['file'] or row['version'] != expected_version
                    or debian.sha(output / 'runtime' / row['file']) != row['sha256']):
                return None
        return record
    except (OSError, KeyError, ValueError):
        return None


def prepare(output, version):
    data, patches = arch_policy(version)
    identity = hashlib.sha256(json.dumps(
        [data, patches, debian.sha(__file__)], sort_keys=True).encode()).hexdigest()[:12]
    work = output / ('source-' + identity)
    if work.exists():
        record = json.loads((work / 'prepared.json').read_text())
        actual = {str(p.relative_to(work / 'source')): debian.sha(p)
                  for p in sorted((work / 'source').rglob('*')) if p.is_file()}
        if not actual or record != actual:
            raise ValueError('Prepared PipeWire source differs; preserve the existing work directory')
        return work, data, patches
    cache = ROOT / 'build/source-cache/pipewire-integration'
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / ('pipewire-' + data['version'] + '.tar.gz')
    for attempt in range(3):
        try:
            download(data['source']['url'], archive, data['source']['sha256'])
            break
        except OSError:
            if attempt == 2:
                raise
            time.sleep(5 * (attempt + 1))
    # Publish sources only after extraction, patches and the recipe are complete.
    with tempfile.TemporaryDirectory(prefix='pipewire-source-', dir=output) as temporary:
        staging = Path(temporary)
        extracted = staging / 'upstream'
        extracted.mkdir()
        with tarfile.open(archive) as stream:
            stream.extractall(extracted, filter='data')
        roots = list(extracted.iterdir())
        if len(roots) != 1 or not (roots[0] / 'src/gst/meson.build').is_file():
            raise ValueError('Unexpected PipeWire release layout')
        for patch in patches:
            debian.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', ROOT / patch['path']], cwd=roots[0])
        prepared = staging / 'prepared'
        source = prepared / 'source'
        source.mkdir(parents=True)
        shutil.copytree(roots[0] / 'src/gst', source / 'gst')
        shutil.copytree(roots[0] / 'spa/plugins/v4l2', source / 'v4l2')
        prepare_meson(source, data['version'])
        record = {str(p.relative_to(source)): debian.sha(p)
                  for p in sorted(source.rglob('*')) if p.is_file()}
        (prepared / 'prepared.json').write_text(json.dumps(record, indent=2) + '\n')
        prepared.rename(work)
    return work, data, patches


def prepare_meson(source, version):
    # Use the installed server and SPA ABI; audio and Bluetooth stay untouched.
    # PipeWire uses the first factory regex, before drop-in overrides. Give the
    # companion source its own name so the server cannot select its stock copy.
    for name in ('v4l2-device.c', 'v4l2-source.c'):
        path = source / 'v4l2' / name
        text = path.read_text()
        if text.count('SPA_NAME_API_V4L2_SOURCE') != 1:
            raise ValueError('Check the upstream V4L2 source factory before packaging')
        path.write_text(text.replace('SPA_NAME_API_V4L2_SOURCE', '"api.camera.v4l2.source"'))
    (source / 'meson.build').write_text(
        "project('pipewire-gstreamer', 'c', version: '" + version + "')\n"
         "cc = meson.get_compiler('c')\nconfiginc = include_directories('.')\n"
        "pipewire_dep = dependency('libpipewire-0.3', version: '==" + version + "')\n"
        "spa_dep = dependency('libspa-0.2')\nmathlib = cc.find_library('m')\n"
        "gst_dep = [" + ', '.join("dependency('" + name + "')" for name in MODULES) + ",\n"
        f"           dependency('gstreamer-video-1.0', version: '>={GST_VIDEO_MIN_VERSION}')]\n"
        "config = configuration_data()\nconfig.set_quoted('PACKAGE', 'pipewire')\n"
        "config.set_quoted('PACKAGE_VERSION', meson.project_version())\n"
        "config.set('HAVE_GSTREAMER_DEVICE_PROVIDER', true)\n"
        "config.set('HAVE_LIBUDEV', true)\nconfig.set('HAVE_LOGIND', true)\n"
        "new_allocators = dependency('gstreamer-allocators-1.0').version().version_compare('>=1.23.1')\n"
        "config.set('HAVE_GSTREAMER_DMA_DRM', new_allocators)\n"
        "config.set('HAVE_GSTREAMER_SHM_ALLOCATOR', new_allocators)\n"
        "configure_file(output: 'config.h', configuration: config)\nsubdir('gst')\n"
        "shared_library('spa-v4l2-source', ['v4l2/v4l2.c', 'v4l2/v4l2-device.c', "
        "'v4l2/v4l2-source.c', 'v4l2/v4l2-udev.c'], include_directories: configinc, "
        "dependencies: [spa_dep, mathlib, dependency('libudev'), dependency('libsystemd')], "
        "install: true, install_dir: get_option('libdir') / 'spa-0.2/v4l2')\n"
        "install_data('90-v4l2-source.conf', install_dir: get_option('datadir') / 'pipewire/pipewire.conf.d')\n"
        "install_data('90-v4l2-source.conf', install_dir: get_option('datadir') / 'wireplumber/wireplumber.conf.d')\n")
    (source / '90-v4l2-source.conf').write_text(
        '# Use the V4L2 copy-buffer fix with the installed PipeWire server.\n'
        'context.spa-libs = {\n'
        '  api.v4l2.* = v4l2/libspa-v4l2-source\n'
        '  api.camera.v4l2.source = v4l2/libspa-v4l2-source\n'
        '}\n')
    (source / 'meson_options.txt').write_text(
        "option('gstreamer-device-provider', type: 'feature', value: 'enabled')\n"
        "option('tests', type: 'feature', value: 'enabled')\n")


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


def test_debian_build(guest, source):
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


def build_arch(root, output, jobs):
    versions = debian.capture(['chroot', root, 'pacman', '-Q',
                              'libpipewire', 'pipewire', 'pipewire-audio']).splitlines()
    versions = [line.split()[1] for line in versions]
    if len(versions) != 3 or len(set(versions)) != 1:
        raise ValueError('PipeWire server, audio and library package versions must match')
    version = versions[0]
    cached = reusable_arch(output, version)
    if cached:
        build_arch_package.install_cached(root, output / 'runtime')
        installed = debian.capture(['chroot', root, 'pacman', '-Q', 'gst-plugin-pipewire']).split()[1]
        if installed != cached['packages']['gst-plugin-pipewire']['version']:
            raise ValueError('Installed PipeWire plugin version differs')
        return cached
    work, data, patches = prepare(output, version)
    recipe = work / 'recipe'
    recipe.mkdir(exist_ok=True)
    shutil.copytree(work / 'source', recipe / 'source', dirs_exist_ok=True)
    epoch = version.split(':')[0] if ':' in version else '0'
    pkgrel = pacman_release(version)
    (recipe / 'PKGBUILD').write_text(
        'pkgbase=pipewire-camera-plugins\npkgname=(gst-plugin-pipewire pipewire-v4l2-source)\npkgver=' + data['version'] + '\npkgrel=' + pkgrel + '\nepoch=' + epoch + '\n'
        'pkgdesc="PipeWire GStreamer plugin with camera format and buffer fixes"\n'
        'arch=(aarch64)\nurl=https://pipewire.org\nlicense=(MIT)\n'
        'depends=(' + ' '.join(shlex.quote(v) for v in (
            'libpipewire=' + version, 'pipewire=' + version, 'pipewire-audio=' + version,
            'glib2', 'glibc', 'gst-plugins-base-libs>=' + GST_VIDEO_MIN_VERSION,
            'gstreamer', 'pipewire-session-manager')) + ')\n'
        'makedepends=(meson pkgconf systemd)\noptions=(!debug !lto)\n'
        'build() {\n  meson setup "$startdir/build" "$startdir/source" --prefix=/usr --libdir=lib --buildtype=release\n'
        f'  meson compile -C "$startdir/build" -j{jobs}\n'
        '  GST_PLUGIN_SYSTEM_PATH_1_0= GST_PLUGIN_PATH_1_0= GST_REGISTRY_FORK=no '
        'GST_REGISTRY_1_0="$startdir/build/test-registry.bin" '
        'meson test -C "$startdir/build" --num-processes 1 --no-rebuild --print-errorlogs\n}\n'
        'package_gst-plugin-pipewire() {\n'
        '  install -Dm755 "$startdir/build/gst/libgstpipewire.so" "$pkgdir/usr/lib/gstreamer-1.0/libgstpipewire.so"\n}\n'
        'package_pipewire-v4l2-source() {\n  depends+=(systemd-libs)\n'
        '  install -Dm755 "$startdir/build/libspa-v4l2-source.so" "$pkgdir/usr/lib/spa-0.2/v4l2/libspa-v4l2-source.so"\n'
        '  for context in pipewire/pipewire.conf.d wireplumber/wireplumber.conf.d; do\n'
        '    install -Dm644 "$startdir/source/90-v4l2-source.conf" "$pkgdir/usr/share/$context/90-v4l2-source.conf"\n'
        '  done\n}\n')
    runtime = output / 'runtime'
    with (output / 'build.log').open('a') as log:
        subprocess.run([sys.executable, ROOT / 'tools/build_arch_package.py', '--rootfs', root,
                        '--recipe', recipe, '--output', runtime],
                       check=True, stdout=log, stderr=subprocess.STDOUT)
    manifest = json.loads((runtime / 'manifest.json').read_text())
    expected_version = package_version(version, data)
    packages = {}
    for name, row in manifest['packages'].items():
        info = debian.capture(['bsdtar', '-xOf', runtime / name, '.PKGINFO'])
        fields = dict(line.split(' = ', 1) for line in info.splitlines() if ' = ' in line)
        package_name = fields.get('pkgname')
        if (package_name not in ('gst-plugin-pipewire', 'pipewire-v4l2-source') or
                fields.get('arch') != 'aarch64' or fields.get('pkgver') != expected_version):
            raise ValueError('Unexpected PipeWire plugin package identity')
        installed = debian.capture(['chroot', root, 'pacman', '-Q', package_name]).split()[1]
        if installed != expected_version:
            raise ValueError('Installed PipeWire plugin version differs')
        packages[package_name] = {'file': name, 'sha256': row['sha256'],
                                  'version': expected_version, 'architecture': 'aarch64'}
    if set(packages) != {'gst-plugin-pipewire', 'pipewire-v4l2-source'}:
        raise ValueError('The PipeWire build must contain both camera plugins')
    return seal(output, version, data, patches, packages)


def seal(output, version, data, patches, packages):
    record = {'packages_built': True, 'upstream_package_version': version,
              'producer_sha256': debian.sha(__file__),
              'archive_sha256': data['source']['sha256'], 'patches': patches,
              'package_format': 'pacman', 'packages': packages,
              'scope': 'GStreamer and V4L2 capture plugins; server, audio and Bluetooth libraries unchanged'}
    (output / 'SOURCE.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def build(root, output, jobs=None, *, libcamera=None):
    jobs = package_build.default_jobs() if jobs is None else jobs
    root, output = root.resolve(), output.resolve()
    if (os.geteuid() or not root.is_relative_to(ROOT / 'build') or not root.is_dir() or
            not output.is_relative_to(ROOT / 'build') or output.is_relative_to(root) or
            root.is_relative_to(output) or jobs < 1):
        raise ValueError('Use root, project build directories outside each other, and at least one job')
    output.mkdir(parents=True, exist_ok=True)
    if (root / 'var/lib/dpkg/status').is_file():
        return build_debian(root, output, jobs, libcamera=libcamera)
    if libcamera is not None:
        raise ValueError('The libcamera package input requires Debian')
    return build_arch(root, output, jobs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rootfs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--libcamera-packages', type=Path, metavar='DIR',
                        help='Build against the packages from a completed Debian libcamera build')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        raise SystemExit(subprocess.run(['unshare', '--mount', '--propagation', 'private',
                                        sys.executable, __file__, *sys.argv[1:], '--worker']).returncode)
    print(json.dumps(build(args.rootfs, args.output, args.jobs,
                           libcamera=args.libcamera_packages), indent=2))
