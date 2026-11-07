#!/usr/bin/env python3
"""Build shared FFmpeg BSP patches as native SteamOS or Arch packages.

Preserve the target's codec configuration and library ABI. FFmpeg 7.0/7.1
use the shared Debian source; FFmpeg 8 and 9 use their matching patch ports.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

import prepare_ffmpeg_source as source
from build_nyarch_apps import guest_environment
from build_release_rootfs import build_resolver

ROOT = Path(__file__).resolve().parents[1]


def package_recipe(version, epoch, pkgrel, options, deps, provides):
    quote_array = lambda values: '(' + ' '.join(shlex.quote(v) for v in values) + ')'
    return (
        f"pkgname=ffmpeg\npkgver={version}\npkgrel={pkgrel}\n"
        f"epoch={epoch}\npkgdesc='FFmpeg with Piano stateful V4L2 fixes'\n"
        "arch=(aarch64)\nurl=https://ffmpeg.org\nlicense=(GPL-3.0-or-later)\n"
        "options=(!debug !lto)\n"
        f"depends={quote_array(deps)}\nprovides={quote_array(provides)}\n"
        "build() {\n  cd \"$startdir/ffmpeg-source\"\n  ./configure " +
        shlex.join(options) + "\n  make -j4\n}\n"
        "package() {\n  cd \"$startdir/ffmpeg-source\"\n"
        "  make DESTDIR=\"$pkgdir\" install\n"
        "  install -Dm644 COPYING.GPLv3 \"$pkgdir/usr/share/licenses/ffmpeg/COPYING.GPLv3\"\n}\n")


def build(root, output, resume=False):
    root, output = root.resolve(), output.resolve()
    if os.geteuid() or not root.is_relative_to(ROOT / 'build/distros'):
        raise ValueError('Use root and a disposable project rootfs')
    if (output.exists() and not resume) or not output.is_relative_to(ROOT / 'build'):
        raise ValueError('Use a new output below build/')
    installed = subprocess.check_output(['chroot', root, 'ffmpeg', '-version'],
                                        text=True, stderr=subprocess.STDOUT)
    # Diagnostics may precede the banner on native ARM hosts. Version/ABI
    # decisions belong to pacman's package record, not banner formatting.
    installed = re.sub(r'\x1b\[[0-9;]*m', '', installed)
    line = next(line for line in installed.splitlines() if line.startswith('configuration:'))
    options = shlex.split(line.split(':', 1)[1])
    dbpath = subprocess.check_output(['chroot', root, 'pacman-conf', 'DBPath'], text=True).strip()
    records = list((root / dbpath.lstrip('/') / 'local').glob('ffmpeg-*/desc'))
    records = [p for p in records if '%NAME%\nffmpeg\n' in p.read_text()]
    if len(records) != 1:
        raise ValueError('Expected one installed FFmpeg package record')
    text = records[0].read_text()
    def field(name):
        return text.split('%' + name + '%\n', 1)[1].split('\n\n', 1)[0].splitlines()
    deps, provides = field('DEPENDS'), field('PROVIDES')
    package_version = field('VERSION')[0]
    upstream_version = package_version.split(':')[-1]
    if re.match(r'^7\.[01](?:[.\s-])', upstream_version):
        series_path = source.SERIES
    elif re.match(r'^8\.1(?:\.\d+)?-', upstream_version):
        series_path = source.SERIES.with_name('series-8.json')
    elif re.match(r'^9\.0\.2-', upstream_version):
        series_path = ROOT / 'patches/ffmpeg/9.0.2/series.json'
    else:
        raise ValueError('Port the BSP fixes to the installed FFmpeg ABI before replacing ' + package_version)
    series = json.loads(series_path.read_text())
    version = series['upstream_version']
    # Retain Arch's package release ordering while identifying our rebuild.
    release = upstream_version.rsplit('-', 1)[1]
    if not re.fullmatch(r'\d+(?:\.\d+)?', release):
        raise ValueError('Unsupported pacman package release: ' + release)
    major, _, rebuild = release.partition('.')
    pkgrel = major + '.' + str(int(rebuild or '0') + 1)
    epoch = package_version.split(':')[0] if ':' in package_version else '0'
    output.mkdir(parents=True, exist_ok=resume)
    provenance = (json.loads((output / 'source/SOURCE.json').read_text())
                  if resume else source.prepare(output / 'source', series=series_path))
    expected_patches = [
        {'path': str((series_path.parent / row['path']).relative_to(ROOT)),
         'sha256': row['sha256']} for row in series['series']]
    if (provenance.get('patches') != expected_patches or
            provenance.get('upstream_version') != series['upstream_version']):
        raise ValueError('Cached FFmpeg source differs from the configured patch series')
    # Compile inside the target ABI with its complete installed codec options.
    guest_work = root / 'var/tmp/sunuefi-ffmpeg'
    if guest_work.exists() and not resume:
        raise ValueError('Existing FFmpeg build directory must be inspected before retry')
    guest_work.mkdir(parents=True, exist_ok=resume)
    if not (guest_work / 'ffmpeg-source').exists():
        shutil.copytree(output / 'source', guest_work / 'ffmpeg-source')
    recipe = package_recipe(version, epoch, pkgrel, options, deps, provides)
    (guest_work / 'PKGBUILD').write_text(recipe)
    with build_resolver(root), guest_environment(root) as guest:
        dependencies = ['base-devel', 'clang',
               'nasm', 'amf-headers', 'avisynthplus', 'ffnvcodec-headers',
               'frei0r-plugins', 'ladspa', 'opencl-headers', 'vulkan-headers',
               *deps]
        # SteamOS retains some base-only packages and virtual soname
        # dependencies. Do not ask the repository to reinstall satisfied ones.
        missing = subprocess.run(['chroot', root, 'pacman', '-T', *dependencies],
                                 capture_output=True, text=True)
        if missing.returncode not in (0, 127):
            raise RuntimeError(missing.stderr)
        if missing.returncode:
            guest(['pacman', '-S', '--needed', '--noconfirm',
                   *missing.stdout.splitlines()])
        if subprocess.run(['chroot', root, 'id', 'sunuefi-ffmpeg'],
                          capture_output=True).returncode:
            guest(['useradd', '-m', 'sunuefi-ffmpeg'])
        guest(['chown', '-R', 'sunuefi-ffmpeg:sunuefi-ffmpeg', '/var/tmp/sunuefi-ffmpeg'])
        guest(['runuser', '-u', 'sunuefi-ffmpeg', '--', 'bash', '-c',
               'cd /var/tmp/sunuefi-ffmpeg && makepkg --noconfirm --nocheck --nodeps'])
        packages = [p for p in guest_work.glob('ffmpeg-*.pkg.tar.*')
                    if p.is_file() and p.suffix in ('.xz', '.zst', '.gz', '.bz2', '.lz4', '.lzo')]
        if len(packages) != 1:
            raise ValueError('FFmpeg package was not produced')
        package = packages[0]
        guest(['pacman', '-U', '--noconfirm', '/var/tmp/sunuefi-ffmpeg/' + package.name])
        version = guest(['ffmpeg', '-version'], capture_output=True, text=True).stdout
        if series['upstream_version'] not in version:
            raise ValueError('Installed FFmpeg differs from the built package')
        guest(['userdel', '-r', 'sunuefi-ffmpeg'])
    shutil.copy2(package, output / package.name)
    provenance.update(package_format='pacman', packages_built=True, original_configuration=options,
                      package=package.name, package_sha256=source.sha(package),
                      installed_version=version, device_verified=False)
    (output / 'SOURCE.json').write_text(json.dumps(provenance, indent=2) + '\n')
    shutil.rmtree(guest_work)
    return provenance


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rootfs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--resume', action='store_true')
    args = parser.parse_args()
    if not args.worker:
        raise SystemExit(subprocess.run(
            ['unshare', '--mount', '--propagation', 'private',
             sys.executable, __file__, *sys.argv[1:], '--worker']).returncode)
    print(json.dumps(build(args.rootfs, args.output, args.resume), indent=2))
