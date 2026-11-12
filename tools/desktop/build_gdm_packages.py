#!/usr/bin/env python3
"""Build Debian or Arch GDM packages with native KMS readiness detection."""
import argparse
from datetime import datetime, timezone
from email.utils import format_datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_gsd_packages as debian
import build_arch_package
from build_mutter_packages import align_development_cohorts
from build_chroot import guest_command
from build_piano_runtime_helpers import source_patches
from desktop import package_build

SERIES = ROOT / 'patches/gdm/48.0/series.json'
RUNTIME = {'gdm3', 'libgdm1', 'gir1.2-gdm-1.0'}


def selection():
    data = json.loads(SERIES.read_text())
    return data, [{'path': path.relative_to(ROOT).as_posix(), 'sha256': debian.sha(path)}
                  for path in source_patches(SERIES)]


def reusable(output):
    try:
        data, patches = selection()
        record = json.loads((output / 'SOURCE.json').read_text())
        if (record.get('packages_built') is not True or
                record.get('package_version') != data['package_version'] or
                record.get('patches') != patches or set(record.get('packages', {})) != RUNTIME or
                (output / '.incomplete').exists()):
            return None
        for row in record['packages'].values():
            if (Path(row['file']).name != row['file'] or row['architecture'] != 'arm64' or
                    row['version'] != data['package_version'] or
                    debian.sha(output / 'runtime' / row['file']) != row['sha256']):
                return None
        return record
    except (OSError, ValueError, KeyError):
        return None


def build(output, sysroot, jobs):
    output, sysroot = output.resolve(), sysroot.resolve()
    if (not output.is_relative_to(ROOT / 'build') or not sysroot.is_relative_to(ROOT / 'build') or
            output.is_relative_to(sysroot) or jobs < 1):
        raise ValueError('Use separate project build directories and at least one build job')
    if cached := reusable(output):
        return cached
    if os.geteuid():
        raise ValueError('Use root for the isolated Debian package builder')
    data, patches = selection()
    version = data['package_version']
    builder = output / 'builder/rootfs'
    if not builder.is_dir():
        builder.parent.mkdir(parents=True, exist_ok=True)
        debian.run(['cp', '-a', '--reflink=auto', sysroot, builder])
    if debian.capture(['chroot', builder, 'dpkg', '--print-architecture']) != 'arm64':
        raise ValueError('The GDM builder must use ARM64 Debian packages')
    output.mkdir(parents=True, exist_ok=True)
    (output / '.incomplete').write_text('GDM package build has not completed.\n')
    cache = builder / 'build/sunuefi-gdm/cache'
    identity = hashlib.sha256(json.dumps(patches, sort_keys=True).encode()).hexdigest()[:12]
    work = builder / ('build/sunuefi-gdm/work-' + identity)
    source = work / 'source'
    with debian.devices(builder), (output / 'build.log').open('a') as log:
        def guest(args):
            debian.run(guest_command(builder, ['env', 'DEBIAN_FRONTEND=noninteractive',
                                              'PATH=/usr/sbin:/usr/bin:/sbin:/bin', *args]),
                       stdout=log, stderr=log)

        dns = builder / 'etc/resolv.conf'
        if dns.is_symlink():
            dns.unlink()
        dns.write_bytes(Path('/etc/resolv.conf').read_bytes())
        (builder / 'etc/apt/sources.list.d/sunuefi-gdm.sources').write_text(
            'Types: deb-src\nURIs: https://deb.debian.org/debian\nSuites: trixie\n'
            'Components: main\nSigned-By: /usr/share/keyrings/debian-archive-keyring.gpg\n')
        guest(['apt-get', 'update'])
        guest(['apt-get', 'install', '-y', '--no-install-recommends', 'dpkg-dev', 'build-essential', 'ccache'])
        align_development_cohorts(builder, log)
        guest(['apt-get', 'build-dep', '-y', '--no-install-recommends', '--build-profiles=nocheck',
               'gdm3=' + data['source_version']])
        cache.mkdir(parents=True, exist_ok=True)
        guest(['sh', '-ec', 'cd "$1"; apt-get source --download-only --only-source gdm3="$2"',
               'source', debian.inside(builder, cache), data['source_version']])
        origin = debian.verify_source(builder, cache, 'gdm3', data['source_version'])
        if not source.is_dir():
            work.mkdir(parents=True, exist_ok=True)
            guest(['dpkg-source', '-x', debian.inside(builder, cache / origin['dsc']['file']),
                   debian.inside(builder, source)])
            series = source / 'debian/patches/series'
            names = []
            for patch in source_patches(SERIES):
                name = 'sunuefi-' + patch.name
                shutil.copy2(patch, series.parent / name)
                names.append(name)
            series.write_text(series.read_text().rstrip() + '\n' + '\n'.join(names) + '\n')
            changelog = source / 'debian/changelog'
            changelog.write_text(
                f'gdm3 ({version}) UNRELEASED; urgency=medium\n\n'
                '  * Wait for native KMS connectors before starting a local display.\n\n'
                f' -- slwyts <slwyts@foxmail.com>  {format_datetime(datetime.now(timezone.utc))}\n\n'
                + changelog.read_text())
        for patch in source_patches(SERIES):
            if debian.sha(source / 'debian/patches' / ('sunuefi-' + patch.name)) != debian.sha(patch):
                raise ValueError('Prepared GDM patch differs')
        if (source / 'debian/changelog').read_text().split('(', 1)[1].split(')', 1)[0] != version:
            raise ValueError('Prepared GDM version differs')
        with package_build.environment(builder) as settings:
            guest(['env', *package_build.arguments(settings),
                   f'DEB_BUILD_OPTIONS=nocheck parallel={jobs}', 'DEB_BUILD_PROFILES=nocheck',
                   'sh', '-ec', 'cd "$1"; dpkg-buildpackage -b -us -uc',
                   'build', debian.inside(builder, source)])
        changes = work / f'gdm3_{version}_arm64.changes'
        change = next(row for row in debian.controls(changes.read_text()) if row.get('Source') == 'gdm3')
        if change.get('Version') != version:
            raise ValueError('GDM build changes have a different version')
        checksums = debian.checksum_rows(change)
        runtime = output / 'runtime'
        runtime.mkdir(exist_ok=True)
        packages = {}
        for name in sorted(RUNTIME):
            path = work / f'{name}_{version}_arm64.deb'
            fields = dict(line.split(': ', 1) for line in debian.capture(
                ['dpkg-deb', '-f', path, 'Package', 'Version', 'Architecture']).splitlines())
            if fields != {'Package': name, 'Version': version, 'Architecture': 'arm64'}:
                raise ValueError('Unexpected GDM runtime identity')
            checksum = debian.sha(path)
            if checksums.get(path.name, {}).get('sha256') != checksum:
                raise ValueError('GDM package differs from its build changes')
            shutil.copy2(path, runtime / path.name)
            packages[name] = {'file': path.name, 'version': version,
                              'architecture': 'arm64', 'sha256': checksum}
        record = {'packages_built': True, 'source': origin, 'package_version': version,
                  'patches': patches, 'packages': packages, 'changes_sha256': debian.sha(changes),
                  'producer_sha256': debian.sha(__file__)}
        (output / 'SOURCE.json').write_text(json.dumps(record, indent=2) + '\n')
        (output / '.incomplete').unlink()
        return record


def build_arch(root, output):
    root, output = root.resolve(), output.resolve()
    if (os.geteuid() or not root.is_relative_to(ROOT / 'build/distros') or
            not output.is_relative_to(ROOT / 'build') or output.is_relative_to(root) or
            root.is_relative_to(output)):
        raise ValueError('Use root and separate project Arch rootfs and output directories')
    installed = debian.capture(['chroot', root, 'pacman', '-Q', 'gdm']).split()[1]
    version = installed.rsplit('-', 1)[0]
    series = ROOT / 'patches/gdm' / version / 'series.json'
    data = json.loads(series.read_text())
    patches = source_patches(series)
    template = ROOT / 'linux/desktops/gnome/arch/gdm/PKGBUILD'
    inputs = {path.relative_to(ROOT).as_posix(): debian.sha(path)
              for path in (series, *patches, template, Path(__file__))}
    metadata = output / 'SOURCE.json'
    if metadata.is_file():
        record = json.loads(metadata.read_text())
        packages = record.get('packages', {})
        if (record.get('inputs') == inputs and packages and
                all(Path(name).name == name and (output / 'runtime' / name).is_file() and
                    debian.sha(output / 'runtime' / name) == row['sha256']
                    for name, row in packages.items())):
            build_arch_package.install_cached(root, output / 'runtime')
            return record
    output.mkdir(parents=True, exist_ok=True)
    recipe = output / 'recipe'
    recipe.mkdir(exist_ok=True)
    for patch in patches:
        shutil.copy2(patch, recipe / patch.name)
    sources = [data['source']['url'], *[path.name for path in patches]]
    checksums = [data['source']['sha256'], *[debian.sha(path) for path in patches]]
    text = template.read_text().replace('@VERSION@', data['version'])
    text = text.replace('@RELEASE@', data['package_release'])
    text = text.replace('@SOURCES@', ' '.join(map(shlex.quote, sources)))
    text = text.replace('@CHECKSUMS@', ' '.join(map(shlex.quote, checksums)))
    (recipe / 'PKGBUILD').write_text(text)
    runtime = output / 'runtime'
    with (output / 'build.log').open('a') as log:
        subprocess.run([sys.executable, ROOT / 'tools/build_arch_package.py', '--rootfs', root,
                        '--recipe', recipe, '--output', runtime],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    record = json.loads((runtime / 'manifest.json').read_text())
    record.update(inputs=inputs, source=data['source'])
    metadata.write_text(json.dumps(record, indent=2) + '\n')
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--sysroot', type=Path, help='Debian ARM64 input; cloned before use')
    mode.add_argument('--rootfs', type=Path, help='Arch ARM64 target; installs the built packages')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/gdm')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        raise SystemExit(subprocess.run(['unshare', '--mount', '--propagation', 'private',
                                        sys.executable, __file__, *sys.argv[1:], '--worker']).returncode)
    if args.rootfs:
        print(json.dumps(build_arch(args.rootfs, args.output), indent=2))
    else:
        print(json.dumps(build(args.output, args.sysroot, args.jobs), indent=2))
