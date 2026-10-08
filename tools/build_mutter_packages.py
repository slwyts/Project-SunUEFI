#!/usr/bin/env python3
"""Build the fixed Debian ARM64 Mutter source with standard quilt/debhelper."""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
from email.utils import format_datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tarfile

import build_gsd_packages as debian

ROOT = Path(__file__).resolve().parents[1]
SOURCE_VERSION = '48.7-0+deb13u1'
PACKAGE_VERSION = SOURCE_VERSION + '+sunuefi1'
PATCHES = [ROOT / 'linux/desktops/gnome/patches/mutter' / name for name in (
    '0001-kms-empty-gamma-bypass.patch', '0002-color-no-edid-standard-profile.patch')]
RUNTIME = {'mutter': 'arm64', 'gir1.2-mutter-16': 'arm64',
           'libmutter-16-0': 'arm64', 'mutter-common': 'all', 'mutter-common-bin': 'arm64'}


def patch_records():
    return [{'path': str(path.relative_to(ROOT)), 'sha256': debian.sha(path)} for path in PATCHES]


def chroot(rootfs, args, **kwargs):
    return debian.chroot(rootfs, ['env', 'PATH=/usr/sbin:/usr/bin:/sbin:/bin', *args], **kwargs)


def align_development_cohorts(rootfs, log, execute=True):
    # A cloned product can contain a local +piano Mesa revision. Development
    # packages require the exact official runtime version. Only this isolated
    # builder is aligned; never downgrade the supplied product or the host.
    installed = {}
    text = debian.capture(['chroot', rootfs, 'dpkg-query', '-W',
                          '-f=${binary:Package}\t${source:Package}\t${Version}\n'])
    for line in text.splitlines():
        package, source, version = line.split('\t')
        installed[package.split(':', 1)[0]] = (package, source, version)
    targets, source_records = {}, []
    for development, anchor in [('libgbm-dev', 'libgbm1'),
                                 ('libpipewire-0.3-dev', 'libpipewire-0.3-0t64'),
                                 ('libspa-0.2-dev', 'libspa-0.2-modules')]:
        if anchor not in installed:
            continue  # A minimal builder lets normal build-dep choose its cohort.
        _, source, version = installed[anchor]
        official_version = re.sub(r'\+piano[0-9]+$', '', version)
        cohort = [(development, official_version)]
        cohort += [(package, official_version) for package, item_source, item_version in installed.values()
                   if item_source == source and item_version != official_version and
                   re.sub(r'\+piano[0-9]+$', '', item_version) == official_version]
        for package, item_version in cohort:
            index = debian.capture(['chroot', rootfs, 'apt-cache', 'show', package + '=' + item_version])
            rows = [row for row in debian.controls(index) if row.get('Version') == item_version and
                    row.get('Package') == package.split(':', 1)[0] and
                    row.get('Source', row.get('Package', '')).split(' ', 1)[0] == source]
            if not rows or any(row.get('SHA256') != rows[0].get('SHA256') for row in rows):
                raise ValueError('Signed APT has no unambiguous matching development cohort: ' + package)
            targets[package] = item_version
            source_records.append({'package': package, 'version': item_version, 'source': source,
                                   'sha256': rows[0]['SHA256'], 'filename': rows[0]['Filename']})
    if not targets:
        return {'targets': [], 'source_records': []}
    args = ['apt-get', 'install', '-y', '--allow-downgrades', '--no-install-recommends']
    args += [package + '=' + version for package, version in sorted(targets.items())]
    command = ['chroot', rootfs, 'env', 'LC_ALL=C', 'DEBIAN_FRONTEND=noninteractive',
               *args[:2], '-s', *args[2:]]
    result = subprocess.run(list(map(str, command)), text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    simulation = result.stdout
    log.write(simulation)
    log.flush()
    result.check_returncode()
    if any(line.startswith('Remv ') for line in simulation.splitlines()):
        raise ValueError('Builder development cohort would remove packages; no changes made')
    if execute:
        chroot(rootfs, args, stdout=log, stderr=subprocess.STDOUT)
    return {'targets': args[5:], 'installed': execute, 'source_records': source_records,
            'simulation_sha256': hashlib.sha256(simulation.encode()).hexdigest()}


@contextmanager
def procfs(rootfs):
    proc = rootfs / 'proc'
    proc.mkdir(exist_ok=True)
    mounted = not os.path.ismount(proc)
    created = []
    if mounted:
        debian.run(['mount', '-t', 'proc', '-o', 'nosuid,nodev,noexec', 'proc', proc])
    try:
        for name, target in [('fd', '/proc/self/fd'), ('stdin', '/proc/self/fd/0'),
                             ('stdout', '/proc/self/fd/1'), ('stderr', '/proc/self/fd/2')]:
            path = rootfs / 'dev' / name
            if not path.exists() and not path.is_symlink():
                path.symlink_to(target)
                created.append(path)
        yield
    finally:
        for path in created:
            path.unlink()
        if mounted:
            debian.run(['umount', proc])


def prepare(rootfs, cache, work, origin, log):
    source = work / 'source'
    if not source.exists():
        work.mkdir(parents=True, exist_ok=True)
        for archive in origin['archives']:
            if '.orig.tar.' in archive['file']:
                shutil.copy2(cache / archive['file'], work / archive['file'])
        pristine = work / 'pristine'
        chroot(rootfs, ['dpkg-source', '-x', debian.inside(rootfs, cache / origin['dsc']['file']),
                       debian.inside(rootfs, pristine)], stdout=log, stderr=subprocess.STDOUT)
        shutil.copytree(pristine, source, symlinks=True)
        series = source / 'debian/patches/series'
        names = []
        for patch in PATCHES:
            name = 'sunuefi-' + patch.name
            shutil.copy2(patch, series.parent / name)
            names.append(name)
        series.write_text(series.read_text().rstrip() + '\n' + '\n'.join(names) + '\n')
        changelog = source / 'debian/changelog'
        changelog.write_text(
            f'mutter ({PACKAGE_VERSION}) UNRELEASED; urgency=medium\n\n'
            '  * Treat empty KMS gamma as bypass before resampling.\n'
            '  * Enable the existing uncalibrated sRGB fallback for no-EDID displays.\n\n'
            f' -- Project SunUEFI <sunuefi@localhost>  {format_datetime(datetime.now(timezone.utc))}\n\n'
            + changelog.read_text())
    for patch in PATCHES:
        if debian.sha(source / 'debian/patches' / ('sunuefi-' + patch.name)) != debian.sha(patch):
            raise ValueError('Prepared source patch changed: ' + patch.name)
    if (source / 'debian/changelog').read_text().split('(', 1)[1].split(')', 1)[0] != PACKAGE_VERSION:
        raise ValueError('Prepared source version differs')
    archive = next(row for row in origin['archives'] if row['file'].endswith('.debian.tar.xz'))
    with tarfile.open(cache / archive['file'], 'r:xz') as packaging:
        for name in ['debian/rules', 'debian/control']:
            if (source / name).read_bytes() != packaging.extractfile(name).read():
                raise ValueError('Original Debian packaging changed: ' + name)
    # Quilt owns .pc and patch application; no manual source substitutions.
    chroot(rootfs, ['sh', '-c', 'cd ' + shlex.quote(debian.inside(rootfs, source)) +
                   ' && dpkg-source --before-build .'], stdout=log, stderr=subprocess.STDOUT)
    return source


def collect(output, source, origin, jobs):
    runtime, all_files = output / 'runtime', output / 'all'
    runtime.mkdir(exist_ok=True)
    all_files.mkdir(exist_ok=True)
    packages = {}
    for package, architecture in RUNTIME.items():
        path = source.parent / f'{package}_{PACKAGE_VERSION}_{architecture}.deb'
        fields = dict(line.split(': ', 1) for line in debian.capture(
            ['dpkg-deb', '-f', path, 'Package', 'Version', 'Architecture']).splitlines())
        if fields != {'Package': package, 'Version': PACKAGE_VERSION, 'Architecture': architecture}:
            raise ValueError('Unexpected runtime package: ' + path.name)
        shutil.copy2(path, runtime / path.name)
        packages[package] = {'file': path.name, 'version': PACKAGE_VERSION,
                             'architecture': architecture, 'sha256': debian.sha(path)}
    changes = source.parent / f'mutter_{PACKAGE_VERSION}_arm64.changes'
    buildinfo = source.parent / f'mutter_{PACKAGE_VERSION}_arm64.buildinfo'
    change = next(row for row in debian.controls(changes.read_text()) if row.get('Source') == 'mutter')
    if change.get('Version') != PACKAGE_VERSION:
        raise ValueError('Actual .changes version differs')
    for name, proof in debian.checksum_rows(change).items():
        path = source.parent / name
        if path.stat().st_size != proof['size'] or debian.sha(path) != proof['sha256']:
            raise ValueError('Built artifact differs from .changes: ' + name)
        shutil.copy2(path, all_files / name)
    for path in [changes, buildinfo]:
        shutil.copy2(path, all_files / path.name)
    origin['prepared_sources'] = {name: debian.sha(source / name) for name in (
        'src/backends/native/meta-kms-update.c', 'src/backends/meta-color-store.c',
        'src/backends/meta-color-device.c', 'debian/rules', 'debian/control')}
    record = {'schema_version': 1, 'source': origin, 'packages_built': True,
              'patches': patch_records(), 'package_version': PACKAGE_VERSION,
              'packages': packages, 'recipe': 'original Debian debian/rules and control',
              'build_command': ['dpkg-buildpackage', '-us', '-uc'],
              'build_options': 'nocheck parallel=' + str(jobs), 'build_profiles': 'nocheck',
              'buildinfo_sha256': debian.sha(buildinfo), 'changes_sha256': debian.sha(changes),
              'producer_sha256': debian.sha(__file__),
              'source_verifier_sha256': debian.sha(debian.__file__),
              'status': 'HOST_BUILT_MUTTER_PACKAGES_NOT_DEVICE_VERIFIED'}
    (output / 'SOURCE.json').write_text(json.dumps(record, indent=2) + '\n')
    (output / 'SHA256SUMS').write_text(''.join(row['sha256'] + '  runtime/' + row['file'] + '\n'
                                             for row in packages.values()))
    (output / '.incomplete').unlink(missing_ok=True)
    return record


def build(output, sysroot, builder_root, jobs, install_dependencies):
    output = output.resolve()
    if not output.is_relative_to(ROOT / 'build') or not 1 <= jobs <= 8:
        raise ValueError('Use a project build output and 1..8 jobs')
    if os.geteuid():
        raise ValueError('Run with root for the isolated native Debian chroot')
    if builder_root:
        rootfs = builder_root.resolve()
    else:
        rootfs = output / 'builder/rootfs'
        if not rootfs.exists():
            rootfs.parent.mkdir(parents=True, exist_ok=True)
            debian.run(['cp', '-a', '--reflink=auto', sysroot.resolve(), rootfs])
    if rootfs == Path('/') or not rootfs.is_relative_to(ROOT / 'build') or output.is_relative_to(rootfs):
        raise ValueError('Use an isolated project builder; output must be outside it')
    release = dict(line.split('=', 1) for line in (rootfs / 'etc/os-release').read_text().splitlines() if '=' in line)
    if release.get('ID', '').strip('"') != 'debian' or release.get('VERSION_CODENAME', '').strip('"') != 'trixie':
        raise ValueError('The builder must be Debian trixie')
    if debian.capture(['chroot', rootfs, 'dpkg', '--print-architecture']) != 'arm64':
        raise ValueError('The builder must use native ARM64 packages (QEMU binfmt is supported)')
    output.mkdir(parents=True, exist_ok=True)
    (output / '.incomplete').write_text('Mutter package build has not completed.\n')
    cache = rootfs / 'build/sunuefi-mutter/cache'
    identity = hashlib.sha256(json.dumps(patch_records(), sort_keys=True).encode()).hexdigest()[:12]
    work = rootfs / ('build/sunuefi-mutter/work-' + identity)
    dependency_alignment = None
    with debian.devices(rootfs), procfs(rootfs), (output / 'build.log').open('a') as log:
        if install_dependencies:
            dns = rootfs / 'etc/resolv.conf'
            if dns.is_symlink():
                dns.unlink()
            dns.write_bytes(Path('/etc/resolv.conf').read_bytes())
            (rootfs / 'etc/apt/sources.list.d/sunuefi-mutter.sources').write_text(
                'Types: deb-src\nURIs: https://deb.debian.org/debian\nSuites: trixie\nComponents: main\n'
                'Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg\n')
            chroot(rootfs, ['apt-get', '-oAcquire::AllowInsecureRepositories=false',
                           '-oAPT::Get::AllowUnauthenticated=false', 'update'], stdout=log, stderr=subprocess.STDOUT)
            chroot(rootfs, ['apt-get', 'install', '-y', '--no-install-recommends', 'dpkg-dev', 'build-essential'],
                   stdout=log, stderr=subprocess.STDOUT)
            dependency_alignment = align_development_cohorts(rootfs, log)
            chroot(rootfs, ['apt-get', 'build-dep', '-y', '--no-install-recommends', '--build-profiles=nocheck',
                           'mutter=' + SOURCE_VERSION], stdout=log, stderr=subprocess.STDOUT)
        cache.mkdir(parents=True, exist_ok=True)
        if not (cache / ('mutter_' + SOURCE_VERSION + '.dsc')).exists():
            chroot(rootfs, ['sh', '-c', 'cd ' + shlex.quote(debian.inside(rootfs, cache)) +
                           ' && apt-get source --download-only --only-source mutter=' + SOURCE_VERSION],
                   stdout=log, stderr=subprocess.STDOUT)
        origin = debian.verify_source(rootfs, cache, 'mutter', SOURCE_VERSION)
        if dependency_alignment is not None:
            origin['builder_dependency_alignment'] = dependency_alignment
        source = prepare(rootfs, cache, work, origin, log)
        chroot(rootfs, ['sh', '-c', 'cd ' + shlex.quote(debian.inside(rootfs, source)) +
                       ' && dpkg-checkbuilddeps -Pnocheck'], stdout=log, stderr=subprocess.STDOUT)
        chroot(rootfs, ['env', 'DEB_BUILD_OPTIONS=nocheck parallel=' + str(jobs), 'DEB_BUILD_PROFILES=nocheck',
                       'sh', '-c', 'cd ' + shlex.quote(debian.inside(rootfs, source)) +
                       ' && dpkg-buildpackage -us -uc'], stdout=log, stderr=subprocess.STDOUT)
        # Debian after-build un-applies quilt; restore the source view for receipts.
        chroot(rootfs, ['sh', '-c', 'cd ' + shlex.quote(debian.inside(rootfs, source)) +
                       ' && dpkg-source --before-build .'], stdout=log, stderr=subprocess.STDOUT)
        return collect(output, source, origin, jobs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/mutter')
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--sysroot', type=Path, help='Completed trixie ARM64 input; cloned before use')
    group.add_argument('--builder-root', type=Path, help='An existing isolated native Debian package builder')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--skip-install-dependencies', action='store_true', help='Use already verified builder dependencies')
    args = parser.parse_args()
    try:
        print(json.dumps(build(args.output, args.sysroot, args.builder_root, args.jobs,
                               not args.skip_install_dependencies), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError, StopIteration) as error:
        parser.exit(2, str(error) + '\n')
