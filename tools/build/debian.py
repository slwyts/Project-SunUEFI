"""Authenticated sid sources and isolated ARM64 package builds."""
from contextlib import contextmanager, ExitStack
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import stat
import subprocess
import tempfile

from desktop import package_build

ROOT = Path(__file__).resolve().parents[2]


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


def guest(root, args, **kwargs):
    return run(['chroot', root, '/usr/bin/env', 'DEBIAN_FRONTEND=noninteractive',
                'LC_ALL=C.UTF-8', 'LANG=C.UTF-8', *args], **kwargs)


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


@contextmanager
def builder(output, sysroot=None):
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
        sources = root / 'etc/apt/sources.list.d/sunuefi-build.sources'
        sources.parent.mkdir(parents=True, exist_ok=True)
        configured = [root / 'etc/apt/sources.list',
                      *sorted(sources.parent.glob('*.list')), *sorted(sources.parent.glob('*.sources'))]
        text = '\n'.join(p.read_text() for p in configured if p != sources and p.is_file())
        extra = []
        if not any(line.startswith('deb-src ') or (line.startswith('Types:') and 'deb-src' in line)
                   for line in text.splitlines()):
            extra.append('Types: deb-src\nURIs: https://deb.debian.org/debian\nSuites: sid\n'
                         'Components: main contrib non-free non-free-firmware\n'
                         'Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg\n')
        if 'contrib' not in text:
            extra.append('Types: deb\nURIs: https://deb.debian.org/debian\nSuites: sid\n'
                         'Components: contrib non-free non-free-firmware\n'
                         'Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg\n')
        sources.write_text('\n'.join(extra))
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
        try:
            yield root, env
        finally:
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
    guest(root, ['env', *package_build.arguments(env), 'sh', '-c',
                 'cd ' + shlex.quote(inside(root, path)) + ' && exec ' + shlex.join(list(map(str, args)))],
          stdout=log, stderr=subprocess.STDOUT)


def source(root, output, package, version, log):
    cache = output / 'archives' / package
    cache.mkdir(parents=True, exist_ok=True)
    command(root, {}, cache, ['apt-get', 'source', '--download-only', '--only-source',
                             package + '=' + version], log)
    rows = [row for row in controls(capture(
        ['chroot', root, 'apt-cache', 'showsrc', '--only-source', package]))
            if row.get('Package') == package and row.get('Version') == version]
    if not rows or any(checksum_rows(row) != checksum_rows(rows[0]) for row in rows):
        raise ValueError('Signed sid source index must unambiguously identify ' + package + ' ' + version)
    expected = checksum_rows(rows[0])
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
    work = output / 'sources' / package
    if not work.exists():
        work.parent.mkdir(parents=True, exist_ok=True)
        command(root, {}, work.parent, ['dpkg-source', '-x', inside(root, dsc), package], log)
        (work / 'SOURCE.json').write_text(json.dumps(origin, indent=2) + '\n')
    elif (json.loads((work / 'SOURCE.json').read_text()).get('archives') != origin['archives'] or
          json.loads((work / 'SOURCE.json').read_text()).get('version') != version):
        raise ValueError('The prepared source differs from the current signed source index')
    return work, origin


def build(root, env, source, jobs, log, architecture_only=False):
    options = 'nocheck parallel=' + str(jobs)
    env = dict(env, DEB_BUILD_OPTIONS=options, DEB_BUILD_PROFILES='nocheck')
    command(root, env, source, ['mk-build-deps', '--install', '--remove',
                               '--tool=apt-get -y --no-remove --no-install-recommends',
                               'debian/control'], log)
    command(root, env, source, ['dpkg-buildpackage', '-B' if architecture_only else '-b',
                               '-us', '-uc', '-j' + str(jobs)], log)


def dependencies(root, log):
    guest(root, ['apt-get', 'update'], stdout=log, stderr=subprocess.STDOUT)
    verify_sid(root)
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
        target.write_bytes(patch.read_bytes())
        if target.name not in names:
            names.append(target.name)
    series.write_text('\n'.join(names) + '\n')
    changelog = work / 'debian/changelog'
    first = changelog.read_text().splitlines()[0]
    if first.split('(', 1)[1].split(')', 1)[0] != version or ') sid;' not in first:
        changelog.write_text(f'{package} ({version}) sid; urgency=medium\n\n'
                             f'  * {message}\n\n'
                             ' -- Project SunUEFI <sunuefi@users.noreply.github.com>  ' +
                             formatdate(localtime=False) + '\n\n' + changelog.read_text())


def write_manifest(output, record):
    (output / 'SOURCE.json').write_text(json.dumps(record, indent=2) + '\n')
    (output / 'SHA256SUMS').write_text(''.join(row['sha256'] + '  runtime/' + row['file'] + '\n'
                                            for row in record['packages'].values()))


def reusable(output, version, patches, runtime):
    try:
        record = json.loads((output / 'SOURCE.json').read_text())
        if (record.get('package_version') != version or record.get('patches') != patches or
                record.get('producer_sha256') != package_build.sha(ROOT / record['producer']) or
                record.get('builder_sha256') != package_build.sha(__file__) or
                set(record.get('packages', {})) != set(runtime) or
                not record.get('packages_built') or (output / '.incomplete').exists()):
            return None
        for row in record['packages'].values():
            if (Path(row['file']).name != row['file'] or row['architecture'] not in ('all', 'arm64') or
                    package_build.sha(output / 'runtime' / row['file']) != row['sha256']):
                return None
        return record
    except (ValueError, KeyError, OSError):
        return None


def collect(output, source_dir, runtime_names, version=None):
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
