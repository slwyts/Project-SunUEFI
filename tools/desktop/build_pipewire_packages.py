#!/usr/bin/env python3
"""Rebuild the GStreamer plugin against the target's installed PipeWire ABI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
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
import build_gsd_packages as debian
import build_mutter_packages
from build_chroot import guest_command
from build_piano_runtime_helpers import source_patches
from prepare_steamos_rootfs import download
from desktop import package_build

MODULES = ('glib-2.0', 'gobject-2.0', 'gmodule-2.0', 'gio-2.0', 'gio-unix-2.0',
           'gstreamer-1.0', 'gstreamer-base-1.0', 'gstreamer-video-1.0',
           'gstreamer-audio-1.0', 'gstreamer-allocators-1.0')


def policy(version):
    upstream = version.split(':')[-1].split('-', 1)[0]
    series = ROOT / 'patches/pipewire' / upstream / 'series.json'
    if not series.is_file():
        raise ValueError('Port the GStreamer fixes to the installed PipeWire version: ' + version)
    data = json.loads(series.read_text())
    patches = source_patches(series)
    return data, [{'path': p.relative_to(ROOT).as_posix(), 'sha256': debian.sha(p)} for p in patches]


def reusable(output, version, package_format):
    try:
        record = json.loads((output / 'SOURCE.json').read_text())
        data, patches = policy(version)
        if (record['upstream_package_version'] != version or record['patches'] != patches or
                record['archive_sha256'] != data['source']['sha256'] or
                record.get('producer_sha256') != debian.sha(__file__) or
                record['package_format'] != package_format or not record['packages_built']):
            return None
        expected = 'gstreamer1.0-pipewire' if package_format == 'deb' else 'gst-plugin-pipewire'
        if set(record['packages']) != {expected}:
            return None
        for row in record['packages'].values():
            if Path(row['file']).name != row['file'] or debian.sha(output / 'runtime' / row['file']) != row['sha256']:
                return None
        return record
    except (OSError, KeyError, ValueError):
        return None


def prepare(output, version):
    data, patches = policy(version)
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
        prepare_meson(source, data['version'])
        record = {str(p.relative_to(source)): debian.sha(p)
                  for p in sorted(source.rglob('*')) if p.is_file()}
        (prepared / 'prepared.json').write_text(json.dumps(record, indent=2) + '\n')
        prepared.rename(work)
    return work, data, patches


def prepare_meson(source, version):
    # Compile every upstream GStreamer source and its format tests, but link
    # against the installed server library. Audio and server packages are not replaced.
    (source / 'meson.build').write_text(
        "project('pipewire-gstreamer', 'c', version: '" + version + "')\n"
         "cc = meson.get_compiler('c')\nconfiginc = include_directories('.')\n"
        "pipewire_dep = dependency('libpipewire-0.3', version: '==" + version + "')\n"
        "spa_dep = dependency('libspa-0.2')\nmathlib = cc.find_library('m')\n"
        "gst_dep = [" + ', '.join("dependency('" + name + "')" for name in MODULES) + "]\n"
        "config = configuration_data()\nconfig.set_quoted('PACKAGE', 'pipewire')\n"
        "config.set_quoted('PACKAGE_VERSION', meson.project_version())\n"
        "config.set('HAVE_GSTREAMER_DEVICE_PROVIDER', true)\n"
        "new_allocators = dependency('gstreamer-allocators-1.0').version().version_compare('>=1.23.1')\n"
        "config.set('HAVE_GSTREAMER_DMA_DRM', new_allocators)\n"
        "config.set('HAVE_GSTREAMER_SHM_ALLOCATOR', new_allocators)\n"
        "configure_file(output: 'config.h', configuration: config)\nsubdir('gst')\n")
    (source / 'meson_options.txt').write_text(
        "option('gstreamer-device-provider', type: 'feature', value: 'enabled')\n"
        "option('tests', type: 'feature', value: 'enabled')\n")


def debian_version(root):
    versions = debian.capture(['chroot', root, 'dpkg-query', '-W', '-f=${Version}\n',
                              'pipewire', 'libpipewire-0.3-0t64']).splitlines()
    if len(versions) != 2 or len(set(versions)) != 1:
        raise ValueError('PipeWire server and library package versions must match')
    return versions[0]


def build_debian(root, output, jobs):
    version = debian_version(root)
    cached = reusable(output, version, 'deb')
    if cached:
        return cached
    work, data, patches = prepare(output, version)
    builder = output / 'builder/rootfs'
    if not builder.is_dir():
        builder.parent.mkdir(exist_ok=True)
        debian.run(['cp', '-a', '--reflink=auto', root, builder])
    if debian_version(builder) != version:
        raise ValueError('The isolated PipeWire builder no longer matches the target ABI')
    guest_work = builder / 'build' / ('pipewire-gstreamer-' + work.name)
    if guest_work.exists():
        prepared = json.loads((work / 'prepared.json').read_text())
        if any(debian.sha(guest_work / 'source' / name) != checksum
               for name, checksum in prepared.items()):
            raise ValueError('Existing PipeWire builder has different sources')
    else:
        guest_work.mkdir(parents=True)
        shutil.copytree(work / 'source', guest_work / 'source')
    (builder / 'proc').mkdir(exist_ok=True)
    with debian.devices(builder), (output / 'build.log').open('a') as log:
        def guest(args, **kwargs):
            return subprocess.run(list(map(str, guest_command(builder, [
                'env', 'DEBIAN_FRONTEND=noninteractive', 'PATH=/usr/sbin:/usr/bin:/sbin:/bin', *args]))),
                check=True, stdout=log, stderr=subprocess.STDOUT, **kwargs)

        dns = builder / 'etc/resolv.conf'
        if dns.is_symlink():
            dns.unlink()
        dns.write_bytes(Path('/etc/resolv.conf').read_bytes())
        guest(['apt-get', 'update'])
        build_mutter_packages.align_development_cohorts(builder, log)
        guest(['apt-get', 'install', '-y', '--no-remove', '--no-install-recommends',
               'build-essential', 'meson', 'ninja-build', 'dpkg-dev',
               'libpipewire-0.3-dev=' + version, 'libspa-0.2-dev=' + version,
               'libgstreamer1.0-dev', 'libgstreamer-plugins-base1.0-dev'])
        directory = debian.inside(builder, guest_work)
        with package_build.environment(builder) as settings:
            guest(['env', *package_build.arguments(settings),
                   'sh', '-ec', 'cd "$1"; test -d build || meson setup build source --prefix=/usr '
                   '--libdir=lib/aarch64-linux-gnu --buildtype=release; '
                    f'meson compile -C build -j{jobs}; '
                    'GST_PLUGIN_SYSTEM_PATH_1_0= GST_PLUGIN_PATH_1_0= GST_REGISTRY_FORK=no '
                    'GST_REGISTRY_1_0="$PWD/build/test-registry.bin" '
                    'meson test -C build --num-processes 1 --no-rebuild --print-errorlogs; '
                   'DESTDIR="$PWD/package" meson install -C build --no-rebuild', 'build', directory])
        package = guest_work / 'package'
        control = package / 'DEBIAN'
        control.mkdir(exist_ok=True)
        binary = package / 'usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstpipewire.so'
        elf = binary.read_bytes()
        if elf[:6] != b'\x7fELF\x02\x01' or elf[18:20] != b'\xb7\x00':
            raise ValueError('GStreamer plugin must be an ARM64 ELF')
        deps = debian.capture(['chroot', builder, 'sh', '-ec', 'cd "$1/source"; '
                              'mkdir -p debian; touch debian/control; '
                              'dpkg-shlibdeps -O ../package/usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstpipewire.so',
                              'dependencies', directory])
        dependencies = deps.split('shlibs:Depends=', 1)[1].strip()
        package_version = version + '+sunuefi1'
        (control / 'control').write_text(
            'Package: gstreamer1.0-pipewire\nVersion: ' + package_version + '\nArchitecture: arm64\n'
            'Multi-Arch: same\nSection: libs\nPriority: optional\n'
            'Maintainer: Project SunUEFI <sunuefi@users.noreply.github.com>\n'
            'Depends: pipewire (= ' + version + '), ' + dependencies + '\n'
            'Description: GStreamer plugin for PipeWire with format and buffer fixes\n'
            ' Keeps preferred camera formats and safely copies borrowed MemPtr buffers.\n')
        runtime = output / 'runtime'
        runtime.mkdir(exist_ok=True)
        path = runtime / ('gstreamer1.0-pipewire_' + package_version + '_arm64.deb')
        debian.run(['dpkg-deb', '--build', '--root-owner-group', package, path], stdout=log)
        packages = {'gstreamer1.0-pipewire': {'file': path.name, 'version': package_version,
                    'architecture': 'arm64', 'sha256': debian.sha(path)}}
    return seal(output, version, data, patches, 'deb', packages)


def build_arch(root, output, jobs):
    versions = debian.capture(['chroot', root, 'pacman', '-Q',
                              'libpipewire', 'pipewire', 'pipewire-audio']).splitlines()
    versions = [line.split()[1] for line in versions]
    if len(versions) != 3 or len(set(versions)) != 1:
        raise ValueError('PipeWire server, audio and library package versions must match')
    version = versions[0]
    cached = reusable(output, version, 'pacman')
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
    pkgrel = version.rsplit('-', 1)[1] + '.1'
    (recipe / 'PKGBUILD').write_text(
        'pkgname=gst-plugin-pipewire\npkgver=' + data['version'] + '\npkgrel=' + pkgrel + '\nepoch=' + epoch + '\n'
        'pkgdesc="PipeWire GStreamer plugin with camera format and buffer fixes"\n'
        'arch=(aarch64)\nurl=https://pipewire.org\nlicense=(MIT)\n'
        'depends=(' + ' '.join(shlex.quote(v) for v in (
            'libpipewire=' + version, 'pipewire=' + version, 'pipewire-audio=' + version,
            'glib2', 'glibc', 'gst-plugins-base-libs', 'gstreamer', 'pipewire-session-manager')) + ')\n'
        'makedepends=(meson pkgconf)\noptions=(!debug !lto)\n'
        'build() {\n  meson setup "$startdir/build" "$startdir/source" --prefix=/usr --libdir=lib --buildtype=release\n'
        f'  meson compile -C "$startdir/build" -j{jobs}\n'
        '  GST_PLUGIN_SYSTEM_PATH_1_0= GST_PLUGIN_PATH_1_0= GST_REGISTRY_FORK=no '
        'GST_REGISTRY_1_0="$startdir/build/test-registry.bin" '
        'meson test -C "$startdir/build" --num-processes 1 --no-rebuild --print-errorlogs\n}\n'
        'package() {\n  DESTDIR="$pkgdir" meson install -C "$startdir/build" --no-rebuild\n}\n')
    runtime = output / 'runtime'
    with (output / 'build.log').open('a') as log:
        subprocess.run([sys.executable, ROOT / 'tools/build_arch_package.py', '--rootfs', root,
                        '--recipe', recipe, '--output', runtime],
                       check=True, stdout=log, stderr=subprocess.STDOUT)
    manifest = json.loads((runtime / 'manifest.json').read_text())
    if len(manifest['packages']) != 1:
        raise ValueError('The PipeWire build must produce only its GStreamer plugin')
    name, row = next(iter(manifest['packages'].items()))
    info = debian.capture(['bsdtar', '-xOf', runtime / name, '.PKGINFO'])
    fields = dict(line.split(' = ', 1) for line in info.splitlines() if ' = ' in line)
    expected_version = (epoch + ':' if epoch != '0' else '') + data['version'] + '-' + pkgrel
    if (fields.get('pkgname') != 'gst-plugin-pipewire' or fields.get('arch') != 'aarch64' or
            fields.get('pkgver') != expected_version):
        raise ValueError('Unexpected PipeWire plugin package identity')
    installed = debian.capture(['chroot', root, 'pacman', '-Q', 'gst-plugin-pipewire']).split()[1]
    if installed != expected_version:
        raise ValueError('Installed PipeWire plugin version differs')
    packages = {'gst-plugin-pipewire': {'file': name, 'sha256': row['sha256'],
                'version': expected_version, 'architecture': 'aarch64'}}
    return seal(output, version, data, patches, 'pacman', packages)


def seal(output, version, data, patches, package_format, packages):
    record = {'packages_built': True, 'upstream_package_version': version,
              'producer_sha256': debian.sha(__file__),
              'archive_sha256': data['source']['sha256'], 'patches': patches,
              'package_format': package_format, 'packages': packages,
              'scope': 'GStreamer plugin only; installed server and audio libraries unchanged'}
    (output / 'SOURCE.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def build(root, output, jobs=None):
    jobs = package_build.default_jobs() if jobs is None else jobs
    root, output = root.resolve(), output.resolve()
    if (os.geteuid() or not root.is_relative_to(ROOT / 'build') or not root.is_dir() or
            not output.is_relative_to(ROOT / 'build') or output.is_relative_to(root) or
            root.is_relative_to(output) or jobs < 1):
        raise ValueError('Use root, project build directories outside each other, and at least one job')
    output.mkdir(parents=True, exist_ok=True)
    if (root / 'var/lib/dpkg/status').is_file():
        return build_debian(root, output, jobs)
    return build_arch(root, output, jobs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rootfs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        raise SystemExit(subprocess.run(['unshare', '--mount', '--propagation', 'private',
                                        sys.executable, __file__, *sys.argv[1:], '--worker']).returncode)
    print(json.dumps(build(args.rootfs, args.output, args.jobs), indent=2))
