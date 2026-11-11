#!/usr/bin/env python3
"""Build the Debian UPower daemon with standard keyboard LED hotplug support."""
import argparse
from datetime import datetime, timezone
from email.utils import format_datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_gsd_packages as debian
from build_chroot import guest_command
from build_piano_runtime_helpers import source_patches

SOURCE_VERSION = '1.90.9-1'
PACKAGE_VERSION = SOURCE_VERSION + '+sunuefi1'
SERIES = ROOT / 'patches/upower/1.90.9/series.json'
RUNTIME = {'upower', 'libupower-glib3'}


def patch_records():
    return [{'path': path.relative_to(ROOT).as_posix(), 'sha256': debian.sha(path)}
            for path in source_patches(SERIES)]


def reusable(output):
    metadata = output / 'SOURCE.json'
    if not metadata.is_file():
        return None
    data = json.loads(metadata.read_text())
    if data.get('patches') != patch_records() or data.get('package_version') != PACKAGE_VERSION:
        return None
    if data.get('packages_built') is not True or set(data.get('packages', {})) != RUNTIME:
        return None
    for row in data['packages'].values():
        if (Path(row['file']).name != row['file'] or row.get('version') != PACKAGE_VERSION or
                row.get('architecture') != 'arm64'):
            return None
        path = output / 'runtime' / row['file']
        if not path.is_file() or debian.sha(path) != row['sha256']:
            return None
    return data


def build(output, sysroot, jobs=2, collect_only=False):
    output, sysroot = output.resolve(), sysroot.resolve()
    if (not output.is_relative_to(ROOT / 'build') or not sysroot.is_relative_to(ROOT / 'build') or
            output.is_relative_to(sysroot)):
        raise ValueError('Use project build directories for the builder and output')
    cached = reusable(output)
    if cached:
        return cached
    if os.geteuid() or not 1 <= jobs <= 8:
        raise ValueError('Use root and 1..8 package build jobs')
    output.mkdir(parents=True, exist_ok=True)
    builder = output / 'builder/rootfs'
    if collect_only and not builder.is_dir():
        raise ValueError('Collection requires an existing completed UPower builder')
    if not builder.exists():
        builder.parent.mkdir(parents=True, exist_ok=True)
        debian.run(['cp', '-a', '--reflink=auto', sysroot, builder])
    cache = builder / 'build/upower/cache'
    identity = hashlib.sha256(json.dumps(patch_records(), sort_keys=True).encode()).hexdigest()[:12]
    source = builder / 'build/upower' / identity / 'source'
    with debian.devices(builder), (output / 'build.log').open('a') as log:
        def guest(args):
            debian.run(guest_command(builder, ['env', 'DEBIAN_FRONTEND=noninteractive',
                                              'PATH=/usr/sbin:/usr/bin:/sbin:/bin', *args]),
                       stdout=log, stderr=log)

        dns = builder / 'etc/resolv.conf'
        if dns.is_symlink():
            dns.unlink()
        dns.write_bytes(Path('/etc/resolv.conf').read_bytes())
        (builder / 'etc/apt/sources.list.d/sunuefi-upower.sources').write_text(
            'Types: deb-src\nURIs: https://deb.debian.org/debian\nSuites: trixie\n'
            'Components: main\nSigned-By: /usr/share/keyrings/debian-archive-keyring.gpg\n')
        if not collect_only:
            guest(['apt-get', 'update'])
            guest(['apt-get', 'install', '-y', '--no-install-recommends', 'dpkg-dev', 'build-essential'])
            guest(['apt-get', 'build-dep', '-y', '--build-profiles=nocheck', 'upower=' + SOURCE_VERSION])
        cache.mkdir(parents=True, exist_ok=True)
        (builder / 'proc').mkdir(exist_ok=True)
        if not collect_only:
            guest(['sh', '-c', 'cd "$1"; apt-get source --download-only --only-source upower="$2"',
                   'source', debian.inside(builder, cache), SOURCE_VERSION])
        origin = debian.verify_source(builder, cache, 'upower', SOURCE_VERSION)
        if collect_only and not source.is_dir():
            raise ValueError('Completed UPower package source is missing')
        if not source.exists():
            source.parent.mkdir(parents=True, exist_ok=True)
            guest(['dpkg-source', '-x', debian.inside(builder, cache / origin['dsc']['file']),
                   debian.inside(builder, source)])
            patches = source / 'debian/patches'
            patches.mkdir(exist_ok=True)
            for path in source_patches(SERIES):
                shutil.copy2(path, patches / path.name)
            series = patches / 'series'
            series.write_text((series.read_text().rstrip() if series.exists() else '') + '\n' +
                              ''.join(path.name + '\n' for path in source_patches(SERIES)))
            changelog = source / 'debian/changelog'
            changelog.write_text(
                f'upower ({PACKAGE_VERSION}) UNRELEASED; urgency=medium\n\n'
                '  * Refresh the legacy keyboard backlight after LED hotplug.\n\n'
                f' -- slwyts <slwyts@foxmail.com>  {format_datetime(datetime.now(timezone.utc))}\n\n' +
                changelog.read_text())
        for patch in source_patches(SERIES):
            if debian.sha(source / 'debian/patches' / patch.name) != debian.sha(patch):
                raise ValueError('Prepared UPower patch differs')
        if not collect_only:
            guest(['env', f'DEB_BUILD_OPTIONS=nocheck parallel={jobs}', 'DEB_BUILD_PROFILES=nocheck',
                   'sh', '-c', 'cd "$1"; dpkg-buildpackage -b -us -uc',
                   'build', debian.inside(builder, source)])
        changes = source.parent / f'upower_{PACKAGE_VERSION}_arm64.changes'
        change = next(row for row in debian.controls(changes.read_text()) if row.get('Source') == 'upower')
        if change.get('Version') != PACKAGE_VERSION:
            raise ValueError('UPower build changes have a different version')
        checksums = debian.checksum_rows(change)
        runtime = output / 'runtime'
        runtime.mkdir(exist_ok=True)
        packages = {}
        for name in sorted(RUNTIME):
            path = source.parent / f'{name}_{PACKAGE_VERSION}_arm64.deb'
            fields = dict(line.split(': ', 1) for line in debian.capture(
                ['dpkg-deb', '-f', path, 'Package', 'Version', 'Architecture']).splitlines())
            if fields != {'Package': name, 'Version': PACKAGE_VERSION, 'Architecture': 'arm64'}:
                raise ValueError('Unexpected UPower runtime package identity')
            checksum = debian.sha(path)
            if checksums.get(path.name, {}).get('sha256') != checksum:
                raise ValueError('UPower runtime package differs from its build changes')
            shutil.copy2(path, runtime / path.name)
            packages[name] = {'file': path.name, 'sha256': checksum,
                              'version': PACKAGE_VERSION, 'architecture': 'arm64'}
        with tempfile.TemporaryDirectory(dir=output, prefix='upower-runtime-') as temporary:
            debian.run(['dpkg-deb', '-x', runtime / packages['upower']['file'], temporary])
            binaries = [Path(temporary) / 'usr/libexec/upowerd',
                        Path(temporary) / 'usr/lib/upower/upowerd']
            binaries = [path for path in binaries if path.is_file()]
            if len(binaries) != 1:
                raise ValueError('UPower daemon is missing from its runtime package')
            binary = binaries[0].read_bytes()
            if binary[:6] != b'\x7fELF\x02\x01' or int.from_bytes(binary[18:20], 'little') != 183:
                raise ValueError('UPower daemon is not an ARM64 ELF')
        data = {'packages_built': True, 'package_version': PACKAGE_VERSION, 'source': origin,
                'patches': patch_records(), 'packages': packages,
                'changes_sha256': debian.sha(changes),
                'producer_sha256': debian.sha(__file__)}
        (output / 'SOURCE.json').write_text(json.dumps(data, indent=2) + '\n')
        return data


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sysroot', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/upower')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--collect-only', action='store_true',
                        help='Seal an existing completed package build without rebuilding it')
    args = parser.parse_args()
    print(json.dumps(build(args.output, args.sysroot, args.jobs, args.collect_only), indent=2))
