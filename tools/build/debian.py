"""Authenticated sid sources and isolated ARM64 package builds."""
from contextlib import contextmanager, ExitStack
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import stat
import subprocess
import tarfile
import tempfile

from build import overlay_release
from desktop import package_build

ROOT = Path(__file__).resolve().parents[2]
ABI_PACKAGES = ('libc6', 'libglib2.0-0t64', 'libstdc++6', 'libgcc-s1',
                'libsystemd0', 'libwayland-client0', 'libgbm1')
# Version 1 also describes manifests produced before this field was recorded.
# Bump for source preparation or compilation changes, not APT verification.
BUILD_RECIPE_VERSION = 1


class TargetIdentity(dict):
    """Carry the read-only sysroot for revalidation without serialising its path."""
    def __init__(self, values, root):
        super().__init__(values)
        self.root = root


def run(args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def capture(args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()


def controls(text):
    rows, row, key = [], {}, None
    for line in text.splitlines() + ['']:
        if line.startswith('-----BEGIN PGP SIGNATURE-----'):
            break
        if not line.strip():
            if row:
                rows.append(row)
            row, key = {}, None
        elif line[0].isspace() and key:
            row[key] += '\n' + line.strip()
        elif ':' in line:
            key, value = line.split(':', 1)
            row[key] = value.strip()
    return rows


def checksum_rows(row):
    return {name: {'sha256': digest, 'size': int(size)}
            for digest, size, name in (line.split() for line in row['Checksums-Sha256'].splitlines() if line)}


def library_versions(root):
    installed = controls(capture(['chroot', root, 'dpkg-query', '-W',
                                  '-f=Package: ${Package}\nVersion: ${Version}\n'
                                  'Status: ${db:Status-Status}\nSource: ${source:Package}\n\n']))
    return {row['Package']: {'version': row['Version'], 'source': row.get('Source')}
            for row in installed if row.get('Status') == 'installed' and row['Package'].startswith('lib')}


def build_packages(root):
    runtime = controls(capture(['chroot', root, 'dpkg-query', '-W',
                                '-f=Package: ${Package}\nVersion: ${Version}\n'
                                'Status: ${db:Status-Status}\n\n']))
    return {row['Package']: row['Version'] for row in runtime if row.get('Status') == 'installed'}


def runtime_dependencies(output, packages):
    names = set(ABI_PACKAGES)
    for row in packages.values():
        text = capture(['dpkg-deb', '-f', output / 'runtime' / row['file'], 'Depends'])
        for dependency in re.split(r'[,|]', text):
            if match := re.match(r'\s*([a-z0-9][a-z0-9+.-]*)', dependency):
                names.add(match[1])
    return sorted(names)


def validate_resume(output, target, patches, version):
    manifest = output / 'SOURCE.json'
    if not manifest.is_file():
        return
    record = json.loads(manifest.read_text())
    if record.get('patches') != patches or record.get('package_version') != version:
        raise ValueError('Resume requires the same package version and patches')
    before = record.get('target')
    if before is not None and any(before.get(key) != target.get(key) for key in ('suite', 'architecture', 'sources', 'binaries')):
        raise ValueError('Resume requires the same target ABI and source selection')
    if record.get('build_packages'):
        for name in record.get('runtime_dependencies', ABI_PACKAGES):
            if name in record.get('packages', {}) or name not in target['libraries']:
                continue
            if record['build_packages'].get(name) != target['libraries'][name]['version']:
                raise ValueError('Resume requires the same runtime dependency ABI: ' + name)


def guest(root, args, **kwargs):
    return run(['chroot', root, '/usr/bin/env', 'DEBIAN_FRONTEND=noninteractive',
                'LC_ALL=C.UTF-8', 'LANG=C.UTF-8',
                'DEBFULLNAME=Project SunUEFI', 'DEBEMAIL=sunuefi@users.noreply.github.com',
                *args], **kwargs)


def verify_sid(root):
    release = dict(line.split('=', 1) for line in (root / 'etc/os-release').read_text().splitlines()
                   if '=' in line)
    if release.get('ID', '').strip('"') != 'debian':
        raise ValueError('Use a Debian sid ARM64 builder')
    if capture(['chroot', root, 'dpkg', '--print-architecture']) != 'arm64':
        raise ValueError('The package builder must use ARM64 packages')
    # sid's base-files uses the next release codename, not VERSION_CODENAME=sid.
    indexes = list((root / 'var/lib/apt/lists').glob('*InRelease'))
    if not indexes or any('\nCodename: sid\n' not in path.read_text() for path in indexes):
        raise ValueError('Use only authenticated Debian sid APT indexes in this builder')


def project_source_version(source, version):
    """Recognise only the exact local revisions selected by this repository."""
    if source == 'mesa':
        return None
    elif source == 'ffmpeg':
        directory = ROOT / 'patches/ffmpeg/9.0.2'
        original = json.loads((directory / 'debian.json').read_text())['source_version']
        revision = json.loads((directory / 'series.json').read_text())['local_revision']
    elif source in ('libssc', 'iio-sensor-proxy'):
        policy = json.loads((ROOT / 'patches/piano-sensors/5/build.json').read_text())
        original = policy['source_versions'][source]
        revision = policy['local_revisions'][source]
    else:
        return None
    return original if overlay_release.base_debian_version(version) == original + '+' + revision else None


def apt_candidates(root, packages, archive_only=False):
    arguments = ['chroot', root, 'env', 'LC_ALL=C', 'apt-cache']
    if archive_only:
        # Hide only installed packages; repository pins and APT policy stay intact.
        arguments += ['-o', 'Dir::State::status=/dev/null']
    text = capture([*arguments, 'policy', *sorted(packages)])
    candidates, package = {}, None
    for line in text.splitlines():
        if line and not line[0].isspace() and line.endswith(':'):
            package = line[:-1].split(':', 1)[0]
        elif line.startswith('  Candidate: ') and package is not None:
            candidates[package] = line.split(': ', 1)[1]
    return candidates


def official_candidate(package, version, records, libraries, archive_version=None):
    from desktop import debian_package as signed
    candidates = [row for row in records if row['Package'] == package and
                  row['Architecture'] in ('arm64', 'all')]
    if not version or version == '(none)':
        raise ValueError('APT has no candidate for the target package: ' + package)
    selected = [row for row in candidates if row['Version'] == version]
    if not selected:
        installed = libraries.get(package, {})
        if installed.get('version') != version:
            raise ValueError('APT candidate does not match signed sid: ' + package)
        source = installed.get('source')
        original = project_source_version(source, version)
        if original is None:
            raise ValueError('APT candidate is not a known project revision: ' + package + '=' + version)
        selected = [row for row in candidates if row['Version'] == archive_version and
                    signed.source_identity(row) == (source, original)]
    if not selected or len({row['SHA256'] for row in selected}) != 1:
        raise ValueError('APT candidate must unambiguously match signed sid: ' + package)
    row = selected[0]
    source, original = signed.source_identity(row)
    installed = libraries.get(package)
    if (package in ABI_PACKAGES and installed is not None and installed['version'] != row['Version'] and
            (installed.get('source') != source or
             project_source_version(source, installed['version']) != original)):
        raise ValueError('Refresh the target sid ABI before building: ' + package)
    return row


def verify_local_gbm(root, official):
    from desktop import debian_package as signed
    fields = controls(capture(['chroot', root, 'dpkg-query', '-W',
        '-f=version: ${Version}\nsource: ${source:Package}\n'
        'source_version: ${source:Version}\narchitecture: ${Architecture}\n', 'libgbm1']))[0]
    source, version = signed.source_identity(official)
    rows, _ = signed.signed_records(root, 'Sources', (source,))
    selected = [row for row in rows if row.get('Package') == source and row.get('Version') == version]
    if not selected or any(checksum_rows(row) != checksum_rows(selected[0]) for row in selected):
        raise ValueError('Signed sid Sources must identify the GBM packaging source')
    proof = signed.verify_local_package(root, 'libgbm1', fields, official, selected[0])
    proof['verified_baseline_sha256'] = signed.verify_gbm_abi(root, official, proof)
    return proof


def target_identity(root, sources, anchors):
    """Bind a completed-package cache to signed source selection and target libs."""
    from desktop import debian_package as signed
    root = Path(root).resolve()
    verify_sid(root)
    libraries = library_versions(root)
    packages = set(anchors.values()) | set(ABI_PACKAGES)
    records, _ = signed.signed_records(root, 'Packages', tuple(sorted(packages)))
    policies = apt_candidates(root, packages)
    installed = build_packages(root)
    local = {name for name in packages if name in installed and policies.get(name) == installed[name] and
             not any(row['Package'] == name and row['Version'] == policies.get(name) for row in records)}
    archive_policies = apt_candidates(root, local, archive_only=True) if local else {}
    identities = dict(libraries)
    for name in local - libraries.keys():
        possible_sources = {signed.source_identity(row)[0] for row in records if row['Package'] == name}
        if len(possible_sources) == 1:
            identities[name] = {'version': installed[name], 'source': possible_sources.pop()}
    selected, installed_local = {}, {}
    for package in sorted(packages):
        if not any(row['Package'] == package for row in records):
            if package not in anchors.values() and package not in libraries:
                continue
            raise ValueError('Signed sid index has no target package: ' + package)
        if (package == 'libgbm1' and package in libraries and
                (package in local or libraries[package]['version'] != policies.get(package))):
            row = official_candidate(package, archive_policies.get(package, policies.get(package)), records, {})
            if libraries[package]['version'] != row['Version']:
                installed_local[package] = verify_local_gbm(root, row)
        else:
            row = official_candidate(package, policies.get(package), records, identities, archive_policies.get(package))
        source_name, version = signed.source_identity(row)
        if package in anchors.values() and sources.get(source_name) != version:
            raise ValueError('The configured source differs from the target sid ABI: ' + source_name)
        selected[package] = {'version': row['Version'], 'source': source_name,
                             'source_version': version, 'sha256': row['SHA256']}
    target = TargetIdentity({'suite': 'sid', 'architecture': 'arm64', 'sources': sources,
                             'binaries': selected, 'libraries': libraries}, root)
    if installed_local:
        target['installed_local'] = installed_local
    if list((root / 'var/lib/apt/lists').glob('*_source_Sources*')):
        target['source_checksums'] = source_checksums(root, sources)
    return target


def source_checksums(root, selected):
    from desktop import debian_package as signed
    records, _ = signed.signed_records(root, 'Sources', tuple(selected))
    result = {}
    for package, version in selected.items():
        rows = [row for row in records if row.get('Package') == package and row.get('Version') == version]
        if not rows or any(checksum_rows(row) != checksum_rows(rows[0]) for row in rows):
            raise ValueError('Signed sid source version is unavailable or ambiguous: ' + package)
        result[package] = checksum_rows(rows[0])
    return result


def verify_original_recipe(work, origin):
    for name, digest in origin.get('recipe', {}).items():
        if package_build.sha(work / name) != digest:
            raise ValueError('The original Debian package recipe changed: ' + origin['package'])


def build_sources(root):
    """Add missing build-only components without replacing the target's policy."""
    folder = root / 'etc/apt/sources.list.d'
    folder.mkdir(parents=True, exist_ok=True)
    destination = folder / 'sunuefi-build.sources'
    configured = [root / 'etc/apt/sources.list', *folder.glob('*.list'), *folder.glob('*.sources')]
    enabled = {'deb': set(), 'deb-src': set()}
    for path in configured:
        if path == destination or not path.is_file():
            continue
        text = path.read_text()
        if path.suffix == '.sources':
            for row in controls(text):
                if row.get('Enabled', 'yes').lower() == 'no' or 'sid' not in row.get('Suites', '').split():
                    continue
                for kind in row.get('Types', '').split():
                    if kind in enabled:
                        enabled[kind].update(row.get('Components', '').split())
        else:
            for line in text.splitlines():
                match = re.match(r'^\s*(deb(?:-src)?)\s+(?:\[[^]]*\]\s+)?\S+\s+sid\s+([^#]+)', line)
                if match:
                    enabled[match[1]].update(match[2].split())
    sections = []
    for kind, needed in [('deb-src', {'main', 'contrib'}), ('deb', {'contrib'})]:
        missing = needed - enabled[kind]
        if missing:
            sections.append(f'Types: {kind}\nURIs: https://deb.debian.org/debian\nSuites: sid\n'
                            'Components: ' + ' '.join(sorted(missing)) + '\n'
                            'Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg\n')
    destination.write_text('\n'.join(sections))


@contextmanager
def builder(output, sysroot=None):
    from desktop.debian_package import verify_build_archive

    output = output.resolve()
    if os.geteuid() or not output.is_relative_to(ROOT / 'build'):
        raise ValueError('Run as root with an output below the project build directory')
    output.mkdir(parents=True, exist_ok=True)
    root = output / 'builder/rootfs' if sysroot else Path('/')
    if sysroot:
        sysroot = sysroot.resolve()
        if root.is_relative_to(sysroot) or not sysroot.is_dir():
            raise ValueError('Use an existing sysroot outside the output directory')
        if not root.exists():
            root.parent.mkdir(parents=True, exist_ok=True)
            run(['cp', '-a', '--reflink=auto', sysroot, root])
        resolver = root / 'etc/resolv.conf'
        resolver.unlink(missing_ok=True)
        resolver.write_bytes(Path('/etc/resolv.conf').read_bytes())
        # Build-only sources live in the disposable clone, not the supplied root.
        # Use the official sid archive for source and contrib FastRPC dependencies.
        build_sources(root)
    elif platform.machine() not in ('arm64', 'aarch64'):
        raise ValueError('On non-ARM64 hosts use --sysroot; host dependencies are never installed')
    workspace = root / 'workspace' if sysroot else ROOT
    mounts = []
    with packaging_devices(root), ExitStack() as stack:
        # Concurrent package builds share one prepared host-tool directory.
        # Serialize its preparation, not the independent package compilation.
        with (ROOT / 'build/host-package-tools.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            env = stack.enter_context(package_build.environment(root))
        if sysroot:
            workspace.mkdir(exist_ok=True)
            run(['mount', '--bind', ROOT, workspace])
            mounts.append(workspace)
        policy = root / 'usr/sbin/policy-rc.d'
        saved_policy = policy.read_bytes() if policy.exists() else None
        saved_mode = policy.stat().st_mode if policy.exists() else None
        policy.write_text('#!/bin/sh\nexit 101\n')
        policy.chmod(0o755)
        try:
            yield root, env
            verify_build_archive(root)
        finally:
            if saved_policy is None:
                policy.unlink()
            else:
                policy.write_bytes(saved_policy)
                policy.chmod(saved_mode)
            for mount in reversed(mounts):
                run(['umount', mount])


@contextmanager
def packaging_devices(root):
    links, nodes = [], []
    try:
        for name, major, minor in [('null', 1, 3), ('zero', 1, 5), ('random', 1, 8), ('urandom', 1, 9)]:
            path = root / 'dev' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists():
                os.mknod(path, stat.S_IFCHR | 0o666, os.makedev(major, minor))
                nodes.append(path)
        for name, target in [('fd', '/proc/self/fd'), ('stdin', '/proc/self/fd/0'),
                             ('stdout', '/proc/self/fd/1'), ('stderr', '/proc/self/fd/2')]:
            path = root / 'dev' / name
            if not path.exists() and not path.is_symlink():
                path.symlink_to(target)
                links.append(path)
        yield
    finally:
        for path in links + nodes:
            path.unlink()


def inside(root, path):
    path = Path(path).resolve()
    return str(path) if root == Path('/') else '/workspace/' + str(path.relative_to(ROOT))


def command(root, env, path, args, log):
    guest(root, ['env', 'QUILT_PATCH_OPTS=--fuzz=0', *package_build.arguments(env), 'sh', '-c',
                 'cd ' + shlex.quote(inside(root, path)) + ' && exec ' + shlex.join(list(map(str, args)))],
          stdout=log, stderr=subprocess.STDOUT)


def source(root, output, package, version, log):
    from desktop import debian_package as signed
    cache = output / 'archives' / package
    cache.mkdir(parents=True, exist_ok=True)
    records, proofs = signed.signed_records(root, 'Sources', (package,))
    rows = [row for row in records if row.get('Package') == package and row.get('Version') == version]
    if not rows or any(checksum_rows(row) != checksum_rows(rows[0]) for row in rows):
        raise ValueError('Signed sid source index must unambiguously identify ' + package + ' ' + version)
    expected = checksum_rows(rows[0])
    expected = dict(sorted(expected.items()))
    if not all((cache / name).is_file() and (cache / name).stat().st_size == proof['size'] and
               package_build.sha(cache / name) == proof['sha256'] for name, proof in expected.items()):
        command(root, {}, cache, ['apt-get', 'source', '--download-only', '--only-source',
                                 package + '=' + version], log)
    for name, proof in expected.items():
        if Path(name).name != name:
            raise ValueError('Invalid source archive filename')
        path = cache / name
        if path.stat().st_size != proof['size'] or package_build.sha(path) != proof['sha256']:
            raise ValueError('Signed APT source checksum mismatch: ' + name)
    dsc = cache / (package + '_' + version.split(':')[-1] + '.dsc')
    metadata = next(row for row in controls(dsc.read_text()) if row.get('Source') == package)
    if metadata.get('Version') != version or metadata.get('Format') != '3.0 (quilt)':
        raise ValueError('Unexpected Debian source identity')
    if any(expected.get(name) != proof for name, proof in checksum_rows(metadata).items()):
        raise ValueError('Source .dsc differs from the authenticated sid index')
    origin = {'package': package, 'version': version, 'suite': 'sid',
              'archives': [{'file': name, **proof} for name, proof in expected.items()],
              'apt_source_record_sha256': hashlib.sha256(json.dumps(rows[0], sort_keys=True).encode()).hexdigest(),
              'apt_inrelease': [{'file': p.name, 'sha256': package_build.sha(p)}
                                 for p in sorted((root / 'var/lib/apt/lists').glob('*InRelease'))]}
    archive = next(name for name in expected if '.debian.tar.' in name)
    with tarfile.open(cache / archive) as packaging:
        recipe = {name: packaging.extractfile(name).read() for name in ('debian/rules', 'debian/control')}
    origin['recipe'] = {name: hashlib.sha256(data).hexdigest() for name, data in recipe.items()}
    origin['apt_indexes'] = proofs
    work = output / 'sources' / package
    if not work.exists():
        work.parent.mkdir(parents=True, exist_ok=True)
        command(root, {}, work.parent, ['dpkg-source', '-x', inside(root, dsc), package], log)
        (work / 'SOURCE.json').write_text(json.dumps(origin, indent=2) + '\n')
    else:
        previous = json.loads((work / 'SOURCE.json').read_text())
        before = {row['file']: (row['sha256'], row['size']) for row in previous.get('archives', [])}
        after = {row['file']: (row['sha256'], row['size']) for row in origin['archives']}
        if before != after or previous.get('version') != version:
            raise ValueError('The prepared source differs from the current signed source index')
    verify_original_recipe(work, origin)
    return work, origin


def binaries(root, cache, names, version, log):
    from desktop import debian_package as signed
    rows, proofs = signed.signed_records(root, 'Packages', names)
    selected = {}
    cache.mkdir(exist_ok=True)
    for package in names:
        candidates = [row for row in rows if row['Package'] == package and
                      row['Version'] == version and row['Architecture'] == 'arm64']
        if not candidates or len({row['SHA256'] for row in candidates}) != 1:
            raise ValueError('Signed sid binary version is unavailable or ambiguous: ' + package)
        row = candidates[0]
        path = cache / Path(row['Filename']).name
        if not path.is_file() or package_build.sha(path) != row['SHA256']:
            command(root, {}, cache, ['apt-get', 'download', package + '=' + version], log)
        if path.stat().st_size != int(row['Size']) or package_build.sha(path) != row['SHA256']:
            raise ValueError('Binary package differs from signed sid Packages: ' + path.name)
        selected[package] = {'file': path.name, 'version': version, 'sha256': row['SHA256']}
    return {'packages': selected, 'apt_indexes': proofs}


def build(root, env, source, jobs, log, architecture_only=False, resume=False):
    options = 'nocheck parallel=' + str(jobs)
    env = dict(env, DEB_BUILD_OPTIONS=options, DEB_BUILD_PROFILES='nocheck')
    if subprocess.run(['chroot', str(root), 'sh', '-c',
                       'cd ' + shlex.quote(inside(root, source)) + ' && dpkg-checkbuilddeps -Pnocheck'],
                      stdout=log, stderr=subprocess.STDOUT).returncode:
        command(root, env, source, ['mk-build-deps', '--install', '--remove',
                                   '--tool=apt-get -y --no-remove --no-install-recommends',
                                   'debian/control'], log)
    command(root, env, source, ['dpkg-buildpackage', '-B' if architecture_only else '-b',
                               '-us', '-uc', '-j' + str(jobs), *(['-nc'] if resume else [])], log)


def align_development_cohorts(root, log):
    """Replace local runtime revisions only in the disposable package builder."""
    installed = controls(capture(['chroot', root, 'dpkg-query', '-W',
                                  '-f=Package: ${binary:Package}\nVersion: ${Version}\n'
                                  'Source: ${source:Package}\n\n']))
    from desktop import debian_package as signed
    local = [row for row in installed if project_source_version(row.get('Source'), row.get('Version'))]
    names = [row['Package'] for row in local if row.get('Source') in ('mesa', 'pipewire', 'wayland')]
    if not names:
        return
    rows, _ = signed.signed_records(root, 'Packages', [name.split(':', 1)[0] for name in names])
    policies = apt_candidates(root, names, archive_only=True)
    targets = []
    for row in installed:
        if row['Package'] not in names:
            continue
        package = row['Package']
        original = project_source_version(row['Source'], row['Version'])
        candidates = [item for item in rows if item['Package'] == package.split(':', 1)[0] and
                      item['Version'] == policies.get(package.split(':', 1)[0]) and
                      signed.source_identity(item) == (row['Source'], original)]
        if not candidates or len({item['SHA256'] for item in candidates}) != 1:
            raise ValueError('Signed sid has no development cohort for ' + package)
        targets.append(package + '=' + candidates[0]['Version'])
    if targets:
        guest(root, ['apt-get', 'install', '-y', '--allow-downgrades', '--no-remove',
                     '--no-install-recommends', *targets], stdout=log, stderr=subprocess.STDOUT)


def dependencies(root, log, resume=False):
    from desktop import debian_package as signed
    signed.prepare_build_indexes(root, lambda args: guest(root, args, stdout=log,
                                 stderr=subprocess.STDOUT), refresh=not resume)
    verify_sid(root)
    signed.signed_records(root, 'Sources', ())
    if root != Path('/'):
        align_development_cohorts(root, log)
    guest(root, ['apt-get', 'install', '-y', '--no-install-recommends', 'build-essential',
                 'ca-certificates', 'dpkg-dev', 'devscripts', 'equivs', 'ccache', 'patch'],
          stdout=log, stderr=subprocess.STDOUT)


def add_patches(work, patches, package, version, message):
    """Let dpkg-source apply local changes through the original quilt recipe."""
    from email.utils import formatdate
    directory = work / 'debian/patches'
    directory.mkdir(exist_ok=True)
    series = directory / 'series'
    original = series.read_text() if series.exists() else ''
    names = original.splitlines()
    for patch in patches:
        target = directory / ('sunuefi-' + patch.name)
        if target.exists() and target.read_bytes() != patch.read_bytes():
            raise ValueError('Prepared patch changed: ' + patch.name)
        if not target.exists():
            target.write_bytes(patch.read_bytes())
        if target.name not in names:
            names.append(target.name)
    contents = '\n'.join(names) + '\n'
    if not series.exists() or series.read_text() != contents:
        series.write_text(contents)
    changelog = work / 'debian/changelog'
    first = changelog.read_text().splitlines()[0]
    if first.split('(', 1)[1].split(')', 1)[0] != version or ') sid;' not in first:
        changelog.write_text(f'{package} ({version}) sid; urgency=medium\n\n'
                             f'  * {message}\n\n'
                             ' -- Project SunUEFI <sunuefi@users.noreply.github.com>  ' +
                             formatdate(localtime=False) + '\n\n' + changelog.read_text())


def write_manifest(output, record):
    record.setdefault('build_recipe_version', BUILD_RECIPE_VERSION)
    (output / 'SOURCE.json').write_text(json.dumps(record, indent=2) + '\n')
    (output / 'SHA256SUMS').write_text(''.join(row['sha256'] + '  runtime/' + row['file'] + '\n'
                                            for row in record['packages'].values()))


def reusable(output, version, patches, runtime, target=None):
    try:
        record = json.loads((output / 'SOURCE.json').read_text())
        if (record.get('package_version') != version or record.get('patches') != patches or
                record.get('recipe') != 'original Debian debian/rules and control' or
                not record.get('target') or not record.get('build_packages') or (target is not None and
                     {key: value for key, value in record['target'].items()
                      if key not in ('source_checksums', 'libraries', 'installed_local')} !=
                     {key: value for key, value in target.items()
                      if key not in ('source_checksums', 'libraries', 'installed_local')}) or
                record.get('producer_sha256') != package_build.sha(ROOT / record['producer']) or
                record.get('build_recipe_version', 1) != BUILD_RECIPE_VERSION or
                record.get('compiler_environment_sha256') != package_build.sha(package_build.__file__) or
                set(record.get('packages', {})) != set(runtime) or
                not record.get('packages_built') or (output / '.incomplete').exists()):
            return None
        root = getattr(target, 'root', None)
        if root is None and (output / 'builder/rootfs').is_dir():
            root = output / 'builder/rootfs'
        if root is None:
            return None
        # Re-run today's verifier. Old verifier hashes remain build provenance.
        current_sources = source_checksums(root, record['target']['sources'])
        if target is not None:
            local_gbm = target.get('installed_local', {}).get('libgbm1')
            previous_gbm = record['target'].get('installed_local', {}).get('libgbm1')
            if previous_gbm and previous_gbm != local_gbm:
                return None
            if local_gbm:
                from desktop import debian_package as signed
                if not signed.verify_gbm_dependencies(output, record, {'abi': {'libgbm1': local_gbm}}):
                    return None
            local_packages = set(record['packages'])
            for name in record.get('runtime_dependencies', ABI_PACKAGES):
                if name in local_packages or name not in target['libraries']:
                    continue
                if name == 'libgbm1' and local_gbm:
                    baseline = target['binaries']['libgbm1']
                    if (local_gbm['verified_baseline_sha256'] == baseline['sha256'] and
                            record['build_packages'].get(name) == baseline['version']):
                        continue
                if record['build_packages'].get(name) != target['libraries'][name]['version']:
                    return None
        origins = record.get('sources') or {record['source']['package']: record['source']}
        for name, source_version in record['target']['sources'].items():
            if name == 'fastrpc':
                continue  # The unchanged archive binary is authenticated separately by APT.
            origin = origins.get(name, {})
            if (origin.get('suite') != 'sid' or origin.get('version') != source_version or
                    set(origin.get('recipe', {})) != {'debian/rules', 'debian/control'} or
                    not origin.get('apt_indexes')):
                return None
            checksums = {row['file']: {'sha256': row['sha256'], 'size': row['size']}
                         for row in origin['archives']}
            if checksums != current_sources[name]:
                return None
            if target is not None and target.get('source_checksums'):
                checksums = {row['file']: {'sha256': row['sha256'], 'size': row['size']}
                             for row in origin['archives']}
                if checksums != target['source_checksums'][name]:
                    return None
        if 'fastrpc' in record['target']['sources']:
            from desktop import debian_package as signed
            rows, _ = signed.signed_records(root, 'Packages', ('fastrpc-support', 'libfastrpc1'))
            binary = record.get('fastrpc', {})
            if not binary.get('apt_indexes') or set(binary.get('packages', {})) != {'libfastrpc1', 'fastrpc-support'}:
                return None
            for name, row in binary['packages'].items():
                runtime_row = record['packages'][name]
                if any(row[key] != runtime_row[key] for key in ('file', 'version', 'sha256')):
                    return None
                matches = [item for item in rows if item['Package'] == name and
                           item['Version'] == row['version'] and item['SHA256'] == row['sha256']]
                if not matches:
                    return None
        for row in record['packages'].values():
            if (Path(row['file']).name != row['file'] or row['architecture'] not in ('all', 'arm64') or
                    package_build.sha(output / 'runtime' / row['file']) != row['sha256']):
                return None
        return record
    except (ValueError, KeyError, OSError):
        return None


def verifier_sha():
    from desktop import debian_package
    return package_build.sha(debian_package.__file__)


def collect(output, source_dir, runtime_names, version=None, built=True):
    runtime = output / 'runtime'
    runtime.mkdir(exist_ok=True)
    packages = {}
    for path in sorted(source_dir.glob('*.deb')):
        fields = dict(line.split(': ', 1) for line in capture(
            ['dpkg-deb', '-f', path, 'Package', 'Version', 'Architecture']).splitlines())
        name = fields['Package']
        if name not in runtime_names:
            continue
        if (name in packages or fields['Architecture'] not in ('arm64', 'all') or
                (version and fields['Version'] != version)):
            raise ValueError('Unexpected package identity: ' + path.name)
        if built:
            proofs = [checksum_rows(row).get(path.name) for changes in source_dir.glob('*.changes')
                      for row in controls(changes.read_text()) if row.get('Version') == fields['Version']]
            expected = {'sha256': package_build.sha(path), 'size': path.stat().st_size}
            if expected not in proofs:
                raise ValueError('Runtime package differs from the actual build .changes: ' + path.name)
        with tempfile.TemporaryDirectory(prefix='package-', dir=output) as temporary:
            run(['dpkg-deb', '-x', path, temporary])
            for binary in Path(temporary).rglob('*'):
                if not binary.is_file() or binary.is_symlink():
                    continue
                with binary.open('rb') as stream:
                    header = stream.read(20)
                if header.startswith(b'\x7fELF') and (
                        header[:6] != b'\x7fELF\x02\x01' or int.from_bytes(header[18:20], 'little') != 183):
                    raise ValueError('Runtime package contains a non-ARM64 ELF: ' + path.name)
        shutil.copy2(path, runtime / path.name)
        packages[name] = {'file': path.name, 'version': fields['Version'],
                          'architecture': fields['Architecture'], 'sha256': package_build.sha(path)}
    if packages.keys() != set(runtime_names):
        raise ValueError('Incomplete runtime package set: ' + ', '.join(set(runtime_names) - packages.keys()))
    return packages
