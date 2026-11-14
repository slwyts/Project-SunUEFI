"""Build standard ARM64 packages from authenticated Debian sid source indexes."""
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from email.utils import format_datetime, parsedate_to_datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile

from desktop import package_build

ROOT = Path(__file__).resolve().parents[2]
KEYRING = '/usr/share/keyrings/debian-archive-keyring.gpg'
ABI_PACKAGES = ('libc6', 'libglib2.0-0t64', 'libsystemd0', 'libgtk-4-1',
                'libgtk-3-0t64', 'libwayland-client0', 'libgbm1',
                'libpipewire-0.3-0t64', 'libupower-glib3', 'gnome-shell')


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def capture(args, **kwargs):
    return subprocess.check_output(list(map(str, args)), text=True, **kwargs).strip()


def chroot(rootfs, args, **kwargs):
    return run(['chroot', rootfs, 'env', 'DEBIAN_FRONTEND=noninteractive',
                'PATH=/usr/sbin:/usr/bin:/sbin:/bin', 'LC_ALL=C.UTF-8', *args], **kwargs)


def inside(rootfs, path):
    return '/' + path.resolve().relative_to(rootfs.resolve()).as_posix()


def controls(text):
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
    return result


def checksum_rows(row, field='Checksums-Sha256'):
    return {name: {'sha256': digest, 'size': int(size)}
            for digest, size, name in (line.split() for line in row[field].splitlines() if line)}


@contextmanager
def devices(rootfs):
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


def signed_records(rootfs, identifier, packages):
    """Verify InRelease and the actual uncompressed index, not a receipt."""
    targets = controls(capture(['chroot', rootfs, 'apt-get', 'indextargets']))
    if any((row.get('Codename'), row.get('Suite'), row.get('Origin'), row.get('Trusted')) !=
           ('sid', 'unstable', 'Debian', 'yes') for row in targets):
        raise ValueError('Only signed Debian sid/unstable APT indexes are supported')
    targets = [row for row in targets if row.get('Identifier') == identifier]
    if not targets:
        raise ValueError('The target needs authenticated sid ' + identifier + ' indexes')
    records, proofs = [], []
    package_lines = {'Package: ' + package for package in packages}
    for target in targets:
        if (target.get('Signed-By') or KEYRING) != KEYRING:
            raise ValueError('Debian indexes must use the Debian archive keyring')
        filename = target['Filename']
        suffix = '_' + target['MetaKey'].replace('/', '_')
        if suffix not in filename:
            raise ValueError('Unexpected APT index filename')
        release = Path(filename.split(suffix, 1)[0] + '_InRelease')
        verified = subprocess.run(
            ['gpgv', '--keyring', str(rootfs / KEYRING.lstrip('/')), '--output', '-',
             str(rootfs / release.relative_to('/'))], text=True, capture_output=True, check=True)
        row = controls(verified.stdout)[0]
        if (row.get('Codename'), row.get('Suite'), row.get('Origin')) != ('sid', 'unstable', 'Debian'):
            raise ValueError('InRelease is not Debian sid/unstable')
        now = datetime.now(timezone.utc)
        if (parsedate_to_datetime(row['Date']) > now + timedelta(minutes=10) or
                ('Valid-Until' in row and parsedate_to_datetime(row['Valid-Until']) < now)):
            raise ValueError('The signed sid InRelease is not current; refresh target APT indexes')
        expected = checksum_rows(row, 'SHA256')[target['MetaKey']]
        process = subprocess.Popen(['chroot', str(rootfs), '/usr/lib/apt/apt-helper', 'cat-file', filename],
                                   stdout=subprocess.PIPE)
        digest, size, paragraph = hashlib.sha256(), 0, []
        selected = []
        try:
            for line in process.stdout:
                digest.update(line)
                size += len(line)
                if line.strip():
                    paragraph.append(line)
                else:
                    if paragraph and paragraph[0].decode().strip() in package_lines:
                        selected.extend(controls(b''.join(paragraph).decode()))
                    paragraph = []
            if paragraph and paragraph[0].decode().strip() in package_lines:
                selected.extend(controls(b''.join(paragraph).decode()))
        finally:
            process.stdout.close()
            status = process.wait()
        if status or size != expected['size'] or digest.hexdigest() != expected['sha256']:
            raise ValueError('APT index differs from its signed InRelease: ' + filename)
        records.extend(selected)
        proofs.append({'uri': target['URI'], 'sha256': expected['sha256'],
                       'inrelease_sha256': sha(rootfs / release.relative_to('/'))})
    return records, proofs


def source_identity(row):
    source = row.get('Source', row['Package'])
    match = re.fullmatch(r'(\S+)(?: \(([^)]+)\))?', source)
    if not match:
        raise ValueError('Unexpected Debian binary Source field')
    return match[1], match[2] or row['Version']


def target_identity(rootfs, package):
    rootfs = Path(rootfs).resolve()
    release = dict(line.split('=', 1) for line in (rootfs / 'etc/os-release').read_text().splitlines() if '=' in line)
    if release.get('ID', '').strip('"') != 'debian':
        raise ValueError('The target must be Debian; sid identifies itself as forky in os-release')
    if capture(['chroot', rootfs, 'dpkg', '--print-architecture']) != 'arm64':
        raise ValueError('The target must use ARM64 Debian packages')
    rows, _ = signed_records(rootfs, 'Packages', (package, *ABI_PACKAGES))
    installed = {}
    text = capture(['chroot', rootfs, 'dpkg-query', '-W',
                    '-f=${Package}\t${db:Status-Status}\t${Version}\n'])
    for line in text.splitlines():
        name, status, version = line.split('\t')
        if status == 'installed':
            installed[name] = version

    def select(name):
        candidates = [row for row in rows if row['Package'] == name and row['Architecture'] in ('arm64', 'all')]
        if not candidates:
            raise ValueError('Signed sid APT index has no target package: ' + name)
        policy = capture(['chroot', rootfs, 'env', 'LC_ALL=C', 'apt-cache', 'policy', name])
        version = re.search(r'^  Candidate: (\S+)$', policy, re.M)[1]
        selected = [row for row in candidates if row['Version'] == version]
        if not selected and '+sunuefi' in version:
            # Local packages sort above their original version. Only the signed
            # archive candidate, never the installed local copy, selects source.
            version = candidates[0]['Version']
            for row in candidates[1:]:
                if subprocess.run(['dpkg', '--compare-versions', row['Version'], 'gt', version]).returncode == 0:
                    version = row['Version']
            selected = [row for row in candidates if row['Version'] == version]
        if not selected or len({row['SHA256'] for row in selected}) != 1:
            raise ValueError('APT candidate must unambiguously match signed sid: ' + name)
        row = selected[0]
        if name in ABI_PACKAGES and name in installed and installed[name] != row['Version']:
            raise ValueError('Target ABI differs from signed sid candidate; update ' + name + ' first')
        source, source_version = source_identity(row)
        return {'package': name, 'version': row['Version'], 'source': source,
                'source_version': source_version, 'sha256': row['SHA256']}

    binary = select(package)
    sources, _ = signed_records(rootfs, 'Sources', (binary['source'],))
    sources = [row for row in sources if row['Version'] == binary['source_version']]
    if not sources or any(checksum_rows(row) != checksum_rows(sources[0]) for row in sources):
        raise ValueError('Signed sid Sources must match the selected binary source: ' + package)
    return {'suite': 'sid', 'architecture': 'arm64', 'binary': binary,
            'source_checksums': checksum_rows(sources[0]),
            'abi': {name: select(name) for name in ABI_PACKAGES}}


def verify_source(rootfs, cache, package, source_version):
    rows, proofs = signed_records(rootfs, 'Sources', (package,))
    candidates = [row for row in rows if row.get('Package') == package and row.get('Version') == source_version]
    if not candidates or any(checksum_rows(row) != checksum_rows(candidates[0]) for row in candidates):
        raise ValueError('Signed sid Sources must unambiguously identify ' + package + ' ' + source_version)
    row = candidates[0]
    expected = checksum_rows(row)
    dsc_name = package + '_' + source_version.split(':')[-1] + '.dsc'
    if dsc_name not in expected:
        raise ValueError('Signed Sources has no checksum for the selected .dsc')
    for name, proof in expected.items():
        path = cache / name
        if Path(name).name != name or path.stat().st_size != proof['size'] or sha(path) != proof['sha256']:
            raise ValueError('APT source checksum mismatch: ' + name)
    dsc = next(row for row in controls((cache / dsc_name).read_text()) if row.get('Source') == package)
    if dsc.get('Version') != source_version or dsc.get('Format') != '3.0 (quilt)':
        raise ValueError('Unexpected Debian source identity')
    if any(expected.get(name) != proof for name, proof in checksum_rows(dsc).items()):
        raise ValueError('.dsc archive checksums differ from signed Sources')
    files = [{'file': name, **proof} for name, proof in expected.items()]
    return {'package': package, 'version': source_version, 'suite': 'sid',
            'directory': row['Directory'], 'dsc': next(item for item in files if item['file'] == dsc_name),
            'archives': [item for item in files if item['file'] != dsc_name],
            'apt_source_record_sha256': hashlib.sha256(json.dumps(row, sort_keys=True).encode()).hexdigest(),
            'apt_indexes': proofs, 'verification': 'Debian InRelease signature and exact source SHA256'}


def input_records(paths):
    return {path.relative_to(ROOT).as_posix(): sha(path) for path in paths}


def reusable(output, target, patches, runtime, inputs, revision):
    try:
        record = json.loads((output / 'SOURCE.json').read_text())
        version = target['binary']['source_version'] + '+sunuefi' + str(revision)
        if (record.get('packages_built') is not True or record.get('target') != target or
                record.get('patches') != patches or record.get('inputs') != inputs or
                record.get('package_version') != version or set(record.get('packages', {})) != set(runtime) or
                record.get('source', {}).get('version') != target['binary']['source_version'] or
                (output / '.incomplete').exists()):
            return None
        source = record['source']
        if source.get('package') != target['binary'].get('source') or source.get('suite') != 'sid':
            return None
        checksums = {row['file']: {'sha256': row['sha256'], 'size': row['size']}
                     for row in [source['dsc'], *source['archives']]}
        if checksums != target['source_checksums']:
            return None
        for name, row in record['packages'].items():
            if (Path(row['file']).name != row['file'] or row['version'] != version or
                    row['architecture'] != runtime[name] or sha(output / 'runtime' / row['file']) != row['sha256']):
                return None
        return record
    except (OSError, ValueError, KeyError):
        return None


def build(output, sysroot, jobs, package, patches, runtime, inputs, revision=1,
          validate=None, validate_runtime=None, builder_root=None, install_dependencies=True):
    output = Path(output).resolve()
    sysroot = Path(sysroot or builder_root).resolve()
    if (not output.is_relative_to(ROOT / 'build') or not sysroot.is_relative_to(ROOT / 'build') or
            output.is_relative_to(sysroot) or sysroot.is_relative_to(output) or jobs < 1):
        raise ValueError('Use separate project build directories and at least one build job')
    if os.geteuid():
        raise ValueError('Use root for an isolated ARM64 Debian package build')
    target = target_identity(sysroot, package)
    patch_proof = [{'path': path.relative_to(ROOT).as_posix(), 'sha256': sha(path)} for path in patches]
    inputs = input_records([Path(__file__), Path(package_build.__file__), *inputs])
    if cached := reusable(output, target, patch_proof, runtime, inputs, revision):
        return cached
    output.mkdir(parents=True, exist_ok=True)
    (output / '.incomplete').write_text(package + ' package build has not completed.\n')
    rootfs = Path(builder_root).resolve() if builder_root else output / 'builder/rootfs'
    if builder_root and (not rootfs.is_relative_to(ROOT / 'build') or output.is_relative_to(rootfs) or
                         rootfs.is_relative_to(output)):
        raise ValueError('Use an isolated project builder, not a product rootfs')
    if not rootfs.exists():
        rootfs.parent.mkdir(parents=True, exist_ok=True)
        run(['cp', '-a', '--reflink=auto', sysroot, rootfs])
    version = target['binary']['source_version']
    source_package = target['binary']['source']
    if source_package != package:
        raise ValueError('Unexpected Debian source package for ' + package)
    package_version = version + '+sunuefi' + str(revision)
    identity = hashlib.sha256(json.dumps({'target': target, 'patches': patch_proof, 'inputs': inputs},
                                        sort_keys=True).encode()).hexdigest()[:16]
    work = rootfs / 'build' / package / identity
    cache, source = rootfs / 'build' / package / 'cache', work / 'source'
    with devices(rootfs), (output / 'build.log').open('a') as log:
        def guest(args):
            return chroot(rootfs, args, stdout=log, stderr=subprocess.STDOUT)

        dns = rootfs / 'etc/resolv.conf'
        if dns.is_symlink():
            dns.unlink()
        dns.write_bytes(Path('/etc/resolv.conf').read_bytes())
        policy = rootfs / 'usr/sbin/policy-rc.d'
        policy.write_text('#!/bin/sh\nexit 101\n')
        policy.chmod(0o755)
        # Reuse the target's deb-src configuration rather than adding duplicates.
        if install_dependencies:
            guest(['apt-get', '-oAcquire::AllowInsecureRepositories=false',
                   '-oAPT::Get::AllowUnauthenticated=false', 'update'])
            if target_identity(rootfs, package) != target:
                raise ValueError('The builder and target sid indexes differ; refresh the target APT indexes')
            guest(['apt-get', 'install', '-y', '--no-install-recommends', 'dpkg-dev', 'build-essential', 'ccache'])
            guest(['apt-get', 'build-dep', '-y', '--no-install-recommends', '--build-profiles=nocheck',
                   package + '=' + version])
        elif target_identity(rootfs, package) != target:
            raise ValueError('The existing builder does not match the supplied target ABI/source')
        cache.mkdir(parents=True, exist_ok=True)
        guest(['sh', '-ec', 'cd "$1"; apt-get source --download-only --only-source "$2"',
               'source', inside(rootfs, cache), package + '=' + version])
        origin = verify_source(rootfs, cache, package, version)
        if not source.exists():
            work.mkdir(parents=True, exist_ok=True)
            guest(['dpkg-source', '-x', inside(rootfs, cache / origin['dsc']['file']), inside(rootfs, source)])
            patch_dir = source / 'debian/patches'
            patch_dir.mkdir(exist_ok=True)
            series = patch_dir / 'series'
            names = []
            for patch in patches:
                name = 'sunuefi-' + patch.name
                shutil.copy2(patch, patch_dir / name)
                names.append(name)
            series.write_text((series.read_text().rstrip() if series.exists() else '') + '\n' +
                              '\n'.join(names) + '\n')
            changelog = source / 'debian/changelog'
            changelog.write_text(
                f'{package} ({package_version}) UNRELEASED; urgency=medium\n\n'
                '  * Apply the shared SunUEFI desktop source patches.\n\n'
                f' -- Project SunUEFI <sunuefi@users.noreply.github.com>  {format_datetime(datetime.now(timezone.utc))}\n\n'
                + changelog.read_text())
        for patch in patches:
            if sha(source / 'debian/patches' / ('sunuefi-' + patch.name)) != sha(patch):
                raise ValueError('Prepared patch differs: ' + patch.name)
        if (source / 'debian/changelog').read_text().split('(', 1)[1].split(')', 1)[0] != package_version:
            raise ValueError('Prepared Debian package version differs')
        archive = next(row for row in origin['archives'] if '.debian.tar.' in row['file'])
        with tarfile.open(cache / archive['file']) as packaging:
            for name in ('debian/rules', 'debian/control'):
                if (source / name).read_bytes() != packaging.extractfile(name).read():
                    raise ValueError('Original Debian packaging changed: ' + name)
        directory = inside(rootfs, source)
        guest(['sh', '-ec', 'cd "$1"; dpkg-source --before-build .; dpkg-checkbuilddeps -Pnocheck',
               'prepare', directory])
        if validate:
            validate(source)
        with package_build.environment(rootfs) as settings:
            guest(['env', *package_build.arguments(settings), f'DEB_BUILD_OPTIONS=nocheck parallel={jobs}',
                   'DEB_BUILD_PROFILES=nocheck', 'sh', '-ec', 'cd "$1"; dpkg-buildpackage -b -us -uc',
                   'build', directory])
        changes = work / f'{package}_{package_version.split(":")[-1]}_arm64.changes'
        buildinfo = changes.with_suffix('.buildinfo')
        change = next(row for row in controls(changes.read_text()) if row.get('Source') == package)
        if change.get('Version') != package_version:
            raise ValueError('Actual .changes has a different package version')
        checksums = checksum_rows(change)
        runtime_dir = output / 'runtime'
        runtime_dir.mkdir(exist_ok=True)
        wanted_files = {f'{name}_{package_version.split(":")[-1]}_{architecture}.deb'
                        for name, architecture in runtime.items()}
        for path in runtime_dir.glob('*.deb'):
            if path.name not in wanted_files:
                path.unlink()
        packages = {}
        for name, architecture in runtime.items():
            path = work / f'{name}_{package_version.split(":")[-1]}_{architecture}.deb'
            fields = dict(line.split(': ', 1) for line in capture(
                ['dpkg-deb', '-f', path, 'Package', 'Version', 'Architecture']).splitlines())
            if fields != {'Package': name, 'Version': package_version, 'Architecture': architecture}:
                raise ValueError('Unexpected runtime package identity: ' + path.name)
            if checksums.get(path.name) != {'sha256': sha(path), 'size': path.stat().st_size}:
                raise ValueError('Runtime package differs from actual .changes: ' + path.name)
            shutil.copy2(path, runtime_dir / path.name)
            packages[name] = {'file': path.name, 'version': package_version,
                              'architecture': architecture, 'sha256': sha(path)}
        if validate_runtime:
            with tempfile.TemporaryDirectory(prefix=package + '-runtime-', dir=output) as temporary:
                folder = Path(temporary)
                for row in packages.values():
                    run(['dpkg-deb', '-x', runtime_dir / row['file'], folder], stdout=log, stderr=log)
                validate_runtime(folder)
        # Keep the build's dependency versions and artifact checksums with runtime.
        for path in (changes, buildinfo):
            shutil.copy2(path, output / path.name)
        record = {'schema_version': 2, 'packages_built': True, 'target': target, 'source': origin,
                  'package_version': package_version, 'packages': packages, 'patches': patch_proof,
                  'inputs': inputs, 'recipe': 'original Debian rules and control; standard quilt/debhelper',
                  'build_command': ['dpkg-buildpackage', '-b', '-us', '-uc'],
                  'build_options': f'nocheck parallel={jobs}', 'build_profiles': 'nocheck',
                  'changes_sha256': sha(changes), 'buildinfo_sha256': sha(buildinfo)}
        (output / 'SOURCE.json').write_text(json.dumps(record, indent=2) + '\n')
        (output / 'SHA256SUMS').write_text(''.join(row['sha256'] + '  runtime/' + row['file'] + '\n'
                                                 for row in packages.values()))
        (output / '.incomplete').unlink()
        return record
