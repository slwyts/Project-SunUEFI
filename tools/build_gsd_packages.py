#!/usr/bin/env python3
"""Build normal Debian ARM64 GSD packages in an isolated trixie sysroot."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import shlex
import stat
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'patches/gnome-settings-daemon/0001-power-optional-device-ambient-profile.patch'
PREPARE = ROOT / 'linux/desktops/gnome/ambient-policy/packaging/prepare_package.py'
POLICY = PREPARE.parent.parent
SOURCE_VERSION = '48.1-1'
PACKAGE_VERSION = SOURCE_VERSION + '+sunuefi1'
RUNTIME = {'gnome-settings-daemon': 'arm64', 'gnome-settings-daemon-common': 'all'}


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def capture(args, **kwargs):
    return subprocess.check_output(list(map(str, args)), text=True, **kwargs).strip()


def controls(text):
    """Read Debian control paragraphs, including the clear-signed .dsc body."""
    result, row, key = [], {}, None
    for line in text.splitlines() + ['']:
        if line.startswith('-----BEGIN PGP SIGNATURE-----'):
            break
        if not line.strip():
            if row:
                result.append(row)
            row, key = {}, None
        elif line[0].isspace() and key:
            row[key] += '\n' + line.strip()
        elif ':' in line:
            key, value = line.split(':', 1)
            row[key] = value.strip()
    if row:
        result.append(row)
    return result


def checksum_rows(row):
    return {name: {'sha256': digest, 'size': int(size)}
            for digest, size, name in (line.split() for line in row['Checksums-Sha256'].splitlines() if line)}


@contextmanager
def devices(rootfs):
    # Sealed release sysroots have no live /dev. These standard build-only
    # nodes are removed afterward; no host device, /sys or storage is mounted.
    created = []
    try:
        for name, major, minor in [('null', 1, 3), ('zero', 1, 5), ('random', 1, 8), ('urandom', 1, 9)]:
            path = rootfs / 'dev' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists():
                os.mknod(path, stat.S_IFCHR | 0o666, os.makedev(major, minor))
                created.append(path)
        yield
    finally:
        for path in created:
            path.unlink(missing_ok=True)


def inside(rootfs, path):
    return '/' + str(path.resolve().relative_to(rootfs))


def chroot(rootfs, args, **kwargs):
    return run(['chroot', rootfs, 'env', 'DEBIAN_FRONTEND=noninteractive', *args], **kwargs)


def verify_source(rootfs, cache, package='gnome-settings-daemon', source_version=SOURCE_VERSION):
    # APT's source index is authenticated by its InRelease signature. Check
    # cached archives and .dsc against that exact index, not a caller receipt.
    index = capture(['chroot', rootfs, 'apt-cache', 'showsrc', '--only-source', package])
    candidates = [row for row in controls(index)
                  if row.get('Package') == package and row.get('Version') == source_version]
    if not candidates or any(checksum_rows(row) != checksum_rows(candidates[0]) for row in candidates):
        raise ValueError('Signed APT source index must unambiguously identify ' + package + ' ' + source_version)
    row = candidates[0]
    expected = checksum_rows(row)
    dsc_name = package + '_' + source_version + '.dsc'
    if dsc_name not in expected:
        raise ValueError('APT source index has no checksum for the selected .dsc')
    files = {}
    for name, proof in expected.items():
        if Path(name).name != name:
            raise ValueError('Unexpected source archive filename')
        path = cache / name
        if path.stat().st_size != proof['size'] or sha(path) != proof['sha256']:
            raise ValueError('APT source checksum mismatch: ' + name)
        files[name] = {'file': name, **proof}
    dsc = next(row for row in controls((cache / dsc_name).read_text()) if row.get('Source') == package)
    if dsc.get('Version') != source_version or dsc.get('Format') != '3.0 (quilt)':
        raise ValueError('Unexpected Debian source identity')
    for name, proof in checksum_rows(dsc).items():
        if expected.get(name) != proof:
            raise ValueError('.dsc archive checksums differ from the authenticated APT index')
    apt_files = []
    for path in sorted((rootfs / 'var/lib/apt/lists').glob('*InRelease')):
        apt_files.append({'file': path.name, 'sha256': sha(path)})
    return {'package': package, 'version': source_version, 'suite': 'trixie',
            'directory': row.get('Directory'), 'dsc': files.pop(dsc_name), 'archives': list(files.values()),
            'apt_source_record_sha256': hashlib.sha256(json.dumps(row, sort_keys=True).encode()).hexdigest(),
            'apt_inrelease': apt_files, 'verification': 'APT authenticated source index; exact archive and .dsc SHA256'}


def validate_prepared(source):
    local = source / 'debian/patches/sunuefi-ambient-device-profile.patch'
    if sha(local) != sha(PATCH):
        raise ValueError('Prepared Debian patch differs from the current canonical patch')
    for name in ['gsd-ambient-profile.c', 'gsd-ambient-profile.h']:
        if (source / 'plugins/power' / name).read_bytes() != (POLICY / name).read_bytes():
            raise ValueError('Prepared ambient policy source differs: ' + name)
    if (source / 'plugins/power/ambient-profiles/piano.ini').read_bytes() != (POLICY / 'piano.ini').read_bytes():
        raise ValueError('Prepared Piano ambient profile differs')
    version = (source / 'debian/changelog').read_text().split('(', 1)[1].split(')', 1)[0]
    if version != PACKAGE_VERSION:
        raise ValueError('Prepared Debian package version differs')
    return {'debian_rules_sha256': sha(source / 'debian/rules'),
            'prepared_sources': {name: sha(source / 'plugins/power' / name)
                                 for name in ['gsd-power-manager.c', 'gsd-ambient-profile.c',
                                              'gsd-ambient-profile.h', 'meson.build', 'ambient-profiles/piano.ini']}}


def collect(output, rootfs, source, origin, jobs):
    runtime = output / 'runtime'
    runtime.mkdir(exist_ok=True)
    packages = {}
    for package in sorted(RUNTIME):
        path = source.parent / f'{package}_{PACKAGE_VERSION}_{RUNTIME[package]}.deb'
        meta = capture(['dpkg-deb', '-f', path, 'Package', 'Version', 'Architecture']).splitlines()
        fields = dict(line.split(': ', 1) for line in meta)
        if (fields.get('Package'), fields.get('Version'), fields.get('Architecture')) != (
                package, PACKAGE_VERSION, RUNTIME[package]):
            raise ValueError('Unexpected GSD runtime package identity')
        shutil.copy2(path, runtime / path.name)
        packages[package] = {'file': path.name, 'version': PACKAGE_VERSION,
                             'architecture': RUNTIME[package], 'sha256': sha(path)}
    # Inspect the actual package, including the target ELF and installed profile.
    with tempfile.TemporaryDirectory(prefix='gsd-runtime-', dir=output) as temporary:
        folder = Path(temporary)
        for row in packages.values():
            run(['dpkg-deb', '-x', runtime / row['file'], folder])
        binary = (folder / 'usr/libexec/gsd-power').read_bytes()
        if binary[:6] != b'\x7fELF\x02\x01' or int.from_bytes(binary[18:20], 'little') != 183:
            raise ValueError('Actual gsd-power runtime must be little-endian ELF64 AArch64')
        if b'Using ambient-light profile %s for %s' not in binary:
            raise ValueError('Actual gsd-power binary is missing the ambient profile implementation')
        profile = folder / 'usr/share/gnome-settings-daemon/ambient-profiles/piano.ini'
        if profile.read_bytes() != (POLICY / 'piano.ini').read_bytes():
            raise ValueError('Actual runtime package has no matching Piano profile')
    proof = validate_prepared(source)
    changes = source.parent / f'gnome-settings-daemon_{PACKAGE_VERSION}_arm64.changes'
    buildinfo = source.parent / f'gnome-settings-daemon_{PACKAGE_VERSION}_arm64.buildinfo'
    change = next(row for row in controls(changes.read_text()) if row.get('Source') == 'gnome-settings-daemon')
    if change.get('Version') != PACKAGE_VERSION:
        raise ValueError('Actual .changes version differs')
    built_checksums = checksum_rows(change)
    for row in packages.values():
        if built_checksums.get(row['file'], {}).get('sha256') != row['sha256']:
            raise ValueError('Runtime package checksum differs from actual .changes')
    origin.update(proof)
    record = {'schema_version': 1, 'source': origin, 'packages_built': True,
              'patches': [{'path': str(PATCH.relative_to(ROOT)), 'sha256': sha(PATCH)}],
              'package_version': PACKAGE_VERSION, 'packages': packages,
              'recipe': 'original Debian debian/rules',
              'build_command': ['dpkg-buildpackage', '-b', '-us', '-uc'],
              'build_options': 'nocheck parallel=' + str(jobs), 'build_profiles': 'nocheck',
              'buildinfo_sha256': sha(buildinfo), 'changes_sha256': sha(changes),
              'producer_sha256': sha(__file__), 'status': 'HOST_BUILT_GSD_PACKAGES_NOT_DEVICE_VERIFIED'}
    (output / 'SOURCE.json').write_text(json.dumps(record, indent=2) + '\n')
    (output / 'SHA256SUMS').write_text(''.join(row['sha256'] + '  runtime/' + row['file'] + '\n'
                                             for row in packages.values()))
    (output / '.incomplete').unlink(missing_ok=True)
    return record


def reusable(output):
    path = output / 'SOURCE.json'
    if not path.exists():
        return None
    record = json.loads(path.read_text())
    if (record.get('packages_built') is not True or record.get('package_version') != PACKAGE_VERSION or
        record.get('patches') != [{'path': str(PATCH.relative_to(ROOT)), 'sha256': sha(PATCH)}] or
        set(record.get('packages', {})) != set(RUNTIME)):
        return None
    for name, row in record['packages'].items():
        if row.get('architecture') != RUNTIME[name] or row.get('version') != PACKAGE_VERSION:
            return None
        path = output / 'runtime' / row['file']
        if not path.is_file() or sha(path) != row['sha256']:
            return None
    sources = record.get('source', {}).get('prepared_sources', {})
    for name in ['gsd-ambient-profile.c', 'gsd-ambient-profile.h', 'piano.ini']:
        key = 'ambient-profiles/piano.ini' if name == 'piano.ini' else name
        if sources.get(key) != sha(POLICY / name):
            return None
    return record


def build(output, sysroot, jobs, source_cache=None, prepared_source=None,
          collect_only=False, install_dependencies=True):
    output, sysroot = output.resolve(), sysroot.resolve()
    if not output.is_relative_to(ROOT / 'build') or sysroot == Path('/') or not sysroot.is_relative_to(ROOT / 'build'):
        raise ValueError('Use project build directories for output and the completed input sysroot')
    if not 1 <= jobs <= 8:
        raise ValueError('Use 1 to 8 package build jobs')
    cached = reusable(output)
    if cached:
        return cached
    if os.geteuid():
        raise ValueError('A real isolated chroot package build needs root')
    release = dict(line.split('=', 1) for line in (sysroot / 'etc/os-release').read_text().splitlines() if '=' in line)
    if release.get('ID', '').strip('"') != 'debian' or release.get('VERSION_CODENAME', '').strip('"') != 'trixie':
        raise ValueError('The input sysroot must be completed Debian trixie ARM64')
    output.mkdir(parents=True, exist_ok=True)
    (output / '.incomplete').write_text('GSD runtime packages have not completed.\n')
    if prepared_source is not None:
        # Reuse the existing isolated job without downloading or rebuilding it.
        rootfs, source = sysroot, prepared_source.resolve()
        if source_cache is None or not source.is_relative_to(rootfs):
            raise ValueError('Existing prepared source needs its original cache inside the isolated sysroot')
        cache = source_cache.resolve()
        if not cache.is_relative_to(rootfs):
            raise ValueError('Existing source archive cache must be inside the same isolated sysroot')
        if collect_only and (not source.is_dir() or not cache.is_dir()):
            raise ValueError('Collection requires an already completed source job and archive cache')
    else:
        if collect_only:
            raise ValueError('--collect-only requires --prepared-source and --source-cache')
        if output.is_relative_to(sysroot):
            raise ValueError('Package output cannot be inside the sysroot being cloned')
        rootfs = output / 'builder/rootfs'
        if not rootfs.exists():
            rootfs.parent.mkdir(parents=True, exist_ok=True)
            run(['cp', '-a', '--reflink=auto', sysroot, rootfs])
        cache = rootfs / 'build/sunuefi-gsd/cache'
        source = rootfs / ('build/sunuefi-gsd/work-' + sha(PATCH)[:12] + '/source')
    with devices(rootfs), (output / 'build.log').open('a') as log:
        if capture(['chroot', rootfs, 'dpkg', '--print-architecture']) != 'arm64':
            raise ValueError('The isolated Debian builder must use arm64 packages')
        if not collect_only:
            # The release base has binary APT sources and a sealed DNS symlink.
            # Restore DNS only in the clone; never follow the symlink into /run.
            dns = rootfs / 'etc/resolv.conf'
            if dns.is_symlink():
                dns.unlink()
            dns.write_bytes(Path('/etc/resolv.conf').read_bytes())
            apt_source = rootfs / 'etc/apt/sources.list.d/sunuefi-gsd.sources'
            apt_source.parent.mkdir(parents=True, exist_ok=True)
            apt_source.write_text('Types: deb-src\nURIs: https://deb.debian.org/debian\n'
                                  'Suites: trixie\nComponents: main\n'
                                  'Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg\n')
            chroot(rootfs, ['apt-get', '-oAcquire::AllowInsecureRepositories=false',
                           '-oAPT::Get::AllowUnauthenticated=false', 'update'], stdout=log, stderr=subprocess.STDOUT)
            if install_dependencies:
                chroot(rootfs, ['apt-get', 'install', '-y', '--no-install-recommends', 'dpkg-dev', 'build-essential'],
                       stdout=log, stderr=subprocess.STDOUT)
                chroot(rootfs, ['apt-get', 'build-dep', '-y', '--build-profiles=nocheck',
                               'gnome-settings-daemon=' + SOURCE_VERSION], stdout=log, stderr=subprocess.STDOUT)
        if not source.exists():
            cache.mkdir(parents=True, exist_ok=True)
            chroot(rootfs, ['sh', '-c', 'cd ' + shlex.quote(inside(rootfs, cache)) +
                           ' && apt-get source --download-only --only-source gnome-settings-daemon=' + SOURCE_VERSION],
                   stdout=log, stderr=subprocess.STDOUT)
        origin = verify_source(rootfs, cache)
        if not source.exists():
            source.parent.mkdir(parents=True, exist_ok=True)
            pristine = source.parent / 'pristine'
            chroot(rootfs, ['dpkg-source', '-x', inside(rootfs, cache / origin['dsc']['file']), inside(rootfs, pristine)],
                   stdout=log, stderr=subprocess.STDOUT)
            mirror = rootfs / 'opt/sunuefi-gsd-source'
            for path in [PATCH, PREPARE]:
                destination = mirror / path.relative_to(ROOT)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, destination)
            chroot(rootfs, ['python3', inside(rootfs, mirror / PREPARE.relative_to(ROOT)),
                           'debian', inside(rootfs, source), '--source', inside(rootfs, pristine)],
                   stdout=log, stderr=subprocess.STDOUT)
        # dpkg-buildpackage legitimately un-applies quilt patches after building.
        # Restore the standard prepared view before recording source fingerprints;
        # this does not compile or alter the completed .deb files.
        chroot(rootfs, ['sh', '-c', 'cd ' + shlex.quote(inside(rootfs, source)) +
                       ' && dpkg-source --before-build .'], stdout=log, stderr=subprocess.STDOUT)
        proof = validate_prepared(source)
        archive = cache / 'gnome-settings-daemon_48.1-1.debian.tar.xz'
        with tarfile.open(archive, 'r:xz') as packaging:
            original_rules = packaging.extractfile('debian/rules').read()
            if proof['debian_rules_sha256'] != hashlib.sha256(original_rules).hexdigest():
                raise ValueError('Prepared build changed the original Debian rules')
        if not collect_only:
            env = ['env', 'DEB_BUILD_OPTIONS=nocheck parallel=' + str(jobs), 'DEB_BUILD_PROFILES=nocheck',
                   'sh', '-c', 'cd ' + shlex.quote(inside(rootfs, source)) + ' && dpkg-buildpackage -b -us -uc']
            chroot(rootfs, env, stdout=log, stderr=subprocess.STDOUT)
        return collect(output, rootfs, source, origin, jobs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/gsd')
    parser.add_argument('--sysroot', type=Path, required=True, help='Completed Debian trixie ARM64 base; cloned for a fresh build')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--install-dependencies', action='store_true', default=True,
                        help='Install original Debian build dependencies (default)')
    parser.add_argument('--skip-install-dependencies', dest='install_dependencies', action='store_false',
                        help='Only for an already provisioned Debian package builder')
    parser.add_argument('--source-cache', type=Path, help='Authenticated .dsc/archive cache of an existing isolated job')
    parser.add_argument('--prepared-source', type=Path, help='Existing patched Debian job source inside --sysroot')
    parser.add_argument('--collect-only', action='store_true', help='Seal a completed existing job; no download or compilation')
    args = parser.parse_args()
    try:
        print(json.dumps(build(args.output, args.sysroot, args.jobs, args.source_cache,
                               args.prepared_source, args.collect_only, args.install_dependencies), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError, StopIteration) as error:
        parser.exit(2, str(error) + '\n')
