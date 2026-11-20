"""Build and verify the optional ARM64 Droidloom native packages."""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
from email.utils import format_datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / 'tools') not in sys.path:
    sys.path.insert(0, str(ROOT / 'tools'))

from build.cache import atomic_json, sha
from desktop.package_build import default_jobs
from package.archive import debian_identity, pacman_info


RECIPE = ROOT / 'linux/packaging/droidloom'
POLICY = ROOT / 'config/droidloom.json'
MAINTAINER = 'Project SunUEFI <slwyts@tuta.io>'
COMPONENTS = ('runtime', 'image', 'base')
ARCHITECTURES = {'debian': 'arm64', 'pacman': 'aarch64'}
LIB = 'usr/lib/droidloom'
SHARE = 'usr/share/droidloom'
PARTITION_DIR = LIB + '/images/images'
LICENSES = 'usr/share/licenses/droidloom-'
HOST_PROGRAMS = {
    *('usr/bin/droidloom-' + name for name in (
        'applications', 'audio', 'devices', 'doctor', 'gpu-server', 'supervisor', 'wayland')),
    'usr/bin/droidloomctl', 'usr/bin/droidloomd', LIB + '/droidloom-package-helper',
}
HOST_SYSTEMD = 'usr/lib/systemd'
HOOKS = 'usr/share/libalpm/hooks'
HOOK_FILES = ('60-droidloom-upgrade.hook', '60-droidloom-remove.hook', '90-droidloom-installed.hook')
POLKIT = 'usr/share/polkit-1/actions/org.droidloom.setup.policy'
CURRENT = LIB + '/current'
PARTITIONS = {
    'image': {PARTITION_DIR + '/vendor.raw.img': (1080, b'\x53\xef')},
    'base': {PARTITION_DIR + '/' + name + '.img': (1024, b'\xe2\xe1\xf5\xe0')
             for name in ('system', 'system_ext', 'product')},
}
GUEST_PROGRAMS = {LIB + '/' + name for name in (
    'runtime/bin/droidloom-android-init', 'runtime/bin/surfaceflinger', 'storage/vold')}
PACMAN_METADATA = {'.PKGINFO', '.BUILDINFO', '.MTREE'}
CHUNK_SIZE = 1024 * 1024
ELF64 = b'\x7fELF\x02\x01\x01'


def read_policy():
    record = json.loads(POLICY.read_text())
    if (record.get('schema_version') != 1 or
            not re.fullmatch(r'\d+\.\d+\.\d+', record['version']) or
            not re.fullmatch(r'[a-f0-9]{40}', record['commit']) or
            any(type(record[field]) is not int or record[field] < 1
                for field in ('release', 'package_revision', 'source_date_epoch')) or
            set(record['arm64_archives']) != set(COMPONENTS) or
            any(not is_sha256(value) for value in (
                record['source_sha256'], *record['arm64_archives'].values()))):
        raise ValueError('Invalid Droidloom source or package version')
    return record


def is_sha256(value):
    return isinstance(value, str) and re.fullmatch('[a-f0-9]{64}', value) is not None


def version(policy, package_format):
    revision = policy['package_revision']
    suffix = f'+sunuefi{revision}' if package_format == 'debian' else f'.{revision}'
    return f"{policy['version']}-{policy['release']}{suffix}"


def upstream_version(policy):
    return f"{policy['version']}-{policy['release']}"


def filename(component, package_version, package_format):
    name = 'droidloom-' + component
    if package_format == 'debian':
        return f'{name}_{package_version}_arm64.deb'
    return f'{name}-{package_version}-aarch64.pkg.tar.zst'


def upstream_identity(policy):
    return {'version': policy['version'], 'release': policy['release'], 'architecture': 'aarch64'}


def patches():
    return sorted(RECIPE.glob('*.patch'))


def take_source(path, destination, policy):
    if path.is_dir():
        lookup = ['git', '-C', path]
        commit = subprocess.check_output([*lookup, 'rev-parse', 'HEAD'], text=True).strip()
        root = subprocess.check_output([*lookup, 'rev-parse', '--show-toplevel'], text=True).strip()
        if path != Path(root) or commit != policy['commit']:
            raise ValueError('Use the pinned Droidloom Git checkout')
        with tempfile.TemporaryFile() as snapshot:
            subprocess.run([*lookup, 'archive', 'HEAD'], stdout=snapshot, check=True)
            snapshot.seek(0)
            with tarfile.open(fileobj=snapshot) as archive:
                archive.extractall(destination, filter='data')
    else:
        if not path.is_file() or sha(path) != policy['source_sha256']:
            raise ValueError('Droidloom source archive does not match the pinned SHA-256')
        with tarfile.open(path, 'r:gz') as archive:
            archive.extractall(destination.parent, filter='data')
        (destination.parent / 'droidloom').rename(destination)
    if json.loads((destination / 'packaging/arch/version.json').read_text()) != {
            'version': policy['version'], 'release': policy['release']}:
        raise ValueError('Droidloom package version does not match the source')
    files = patches()
    subprocess.run(['git', 'apply', '--check', *files], cwd=destination, check=True)
    subprocess.run(['git', 'apply', *files], cwd=destination, check=True)


@contextmanager
def archive_members(path, package_format):
    command = ['dpkg-deb', '--fsys-tarfile'] if package_format == 'debian' else ['zstd', '-dcq']
    with subprocess.Popen([*command, path], stdout=subprocess.PIPE) as process:
        with tarfile.open(fileobj=process.stdout, mode='r|', bufsize=CHUNK_SIZE) as members:
            yield members
        while process.stdout.read(CHUNK_SIZE):
            pass
        if process.wait():
            raise ValueError('Cannot read package data: ' + path.name)


def allowed_paths(component, package_format):
    licenses = LICENSES + component
    docs = {'usr/share/doc/droidloom-' + component} if package_format == 'debian' else set()
    if component == 'runtime':
        return HOST_PROGRAMS | {HOST_SYSTEMD, SHARE, POLKIT, licenses,
                                'usr/share/doc/droidloom'} | docs | (
                                    {HOOKS} if package_format == 'pacman' else set())
    return set(PARTITIONS[component]) | {licenses} | docs | (
        {CURRENT, LIB + '/runtime', LIB + '/storage'} if component == 'image' else set())


def member_name(raw):
    name = raw.removeprefix('./').rstrip('/')
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or str(path) != (name or '.'):
        raise ValueError('Invalid package path: ' + raw)
    return name


def check_path(name, allowed, directory):
    if not name or name == '.':
        return directory
    if name.startswith(CURRENT + '/'):
        return False
    return any(name == prefix or name.startswith(prefix + '/') or
               (directory and prefix.startswith(name + '/')) for prefix in allowed)


def check_content(name, prefix, size, component):
    if name in HOST_PROGRAMS | GUEST_PROGRAMS:
        if len(prefix) < 64 or prefix[:7] != ELF64 or prefix[18:20] != b'\xb7\x00':
            raise ValueError('Not an ARM64 ELF: ' + name)
    elif name in PARTITIONS.get(component, {}):
        offset, magic = PARTITIONS[component][name]
        if size < 64 * CHUNK_SIZE or prefix[offset:offset + len(magic)] != magic:
            raise ValueError('Missing a real Android partition image: ' + name)


def content(name, stream, size, component, destination=None):
    """Hash and optionally extract a member, preserving holes in partition images."""
    prefix = stream.read(4096)
    check_content(name, prefix, size, component)
    digest = hashlib.sha256(prefix)
    if destination is None:
        for block in iter(lambda: stream.read(CHUNK_SIZE), b''):
            digest.update(block)
    else:
        zeros = bytes(CHUNK_SIZE)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open('xb') as target:
            target.write(prefix)
            for block in iter(lambda: stream.read(CHUNK_SIZE), b''):
                digest.update(block)
                if block == zeros:
                    target.seek(len(block), os.SEEK_CUR)
                else:
                    target.write(block)
            target.truncate(size)
    return digest.hexdigest()


def archive_payload(path, component, package_format, *, destination=None, notices_only=False):
    """Validate package paths and content without materializing the archive in RAM."""
    allowed = allowed_paths(component, package_format)
    seen, files = set(), {}
    with archive_members(path, package_format) as members:
        for item in members:
            name = member_name(item.name)
            if name in seen or not name and not item.isdir():
                raise ValueError('Repeated or invalid archive entry: ' + item.name)
            seen.add(name)
            if package_format == 'pacman' and name in PACMAN_METADATA and item.isfile():
                continue
            if not check_path(name, allowed, item.isdir()) or item.uid or item.gid:
                raise ValueError('Unexpected package path or owner: ' + name)
            if notices_only and not name.startswith(LICENSES + 'runtime/'):
                continue
            if item.isdir():
                continue
            target = destination / name if destination is not None else None
            if item.issym() and component == 'image' and name == CURRENT and item.linkname == 'runtime':
                files[name] = {'link': item.linkname}
                if target is not None:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.symlink_to(item.linkname)
                continue
            if not item.isfile() or item.mode & 0o7022:
                raise ValueError('Unsupported archive file type or mode: ' + name)
            if name in (SHARE + '/cell-template.json', SHARE + '/package.json'):
                if item.size > CHUNK_SIZE:
                    raise ValueError('Invalid Droidloom package metadata: ' + name)
                value = json.load(members.extractfile(item))
                if not isinstance(value, dict):
                    raise ValueError('Invalid Droidloom package metadata: ' + name)
                if name.endswith('/cell-template.json') and (
                        value.get('share_user_folders') is not False or
                        value.get('shared_storage_directories') != []):
                    raise ValueError('Packaged Droidloom must not share personal folders by default')
                files[name] = {'json': value}
                continue
            files[name] = {'sha256': content(name, members.extractfile(item), item.size, component, target),
                           'bytes': item.size, 'mode': item.mode & 0o777}
            if target is not None:
                target.chmod(item.mode & 0o777)
    return files


def require_payload(component, files, package_format):
    required = {LICENSES + component + '/LICENSE'}
    if component == 'runtime':
        required |= HOST_PROGRAMS | {SHARE + '/cell-template.json', SHARE + '/package.json',
                                     HOST_SYSTEMD + '/system/droidloomd.service',
                                     HOST_SYSTEMD + '/user/droidloom.service', POLKIT,
                                     LICENSES + 'runtime/rust/index.json'}
        if package_format == 'pacman':
            required |= {HOOKS + '/' + name for name in HOOK_FILES}
    else:
        required |= set(PARTITIONS[component]) | {LICENSES + component + '/Android-NOTICE.xml.gz'}
        if component == 'image':
            required |= GUEST_PROGRAMS | {CURRENT, LIB + '/runtime/framework/services.jar'}
    missing = required - files.keys()
    if missing:
        raise ValueError('Incomplete Droidloom ' + component + ': ' + ', '.join(sorted(missing)))
    for name in required & (HOST_PROGRAMS | GUEST_PROGRAMS):
        if not files[name]['mode'] & 0o111:
            raise ValueError('Droidloom program is not executable: ' + name)


def pacman_fields(path):
    info = pacman_info(path)
    keys = ('pkgname', 'pkgver', 'arch')
    if any(len(info.get(key, ())) != 1 for key in keys):
        raise ValueError('Invalid pacman package identity: ' + path.name)
    return (tuple(info[key][0] for key in keys), info.get('depend', []))


def package_metadata(path, component, package_format, package_version):
    if package_format == 'debian':
        actual = debian_identity(path)
        depends = subprocess.check_output(['dpkg-deb', '-f', path, 'Depends'], text=True).split(',')
    else:
        actual, depends = pacman_fields(path)
    if actual != ('droidloom-' + component, package_version, ARCHITECTURES[package_format]):
        raise ValueError('Package identity does not match its generation: ' + path.name)
    next_component = {'runtime': 'image', 'image': 'base'}.get(component)
    expected = ([] if next_component is None else [
        f'droidloom-{next_component} (= {package_version})' if package_format == 'debian'
        else f'droidloom-{next_component}={package_version}'])
    if [entry.strip() for entry in depends if 'droidloom-' in entry] != expected:
        raise ValueError('Droidloom package does not require the matching generation: ' + path.name)


def stage_tree(source, destination, component, notices_only):
    if not source.is_dir() or source.is_symlink():
        raise ValueError('Missing upstream staged ' + component + ' tree: ' + str(source))
    licenses = LICENSES + 'runtime'
    if notices_only:
        source = source / licenses
        if not source.is_dir() or source.is_symlink() or source.resolve() != source:
            raise ValueError('Missing upstream runtime license directory')
    files = {}
    for item in sorted(source.rglob('*')):
        name = item.relative_to(source).as_posix()
        if notices_only:
            name = licenses + '/' + name
        mode = item.lstat().st_mode
        if not check_path(name, allowed_paths(component, 'pacman'), stat.S_ISDIR(mode)):
            raise ValueError('Unexpected upstream staged path: ' + name)
        if stat.S_ISDIR(mode):
            continue
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if stat.S_ISLNK(mode) and component == 'image' and name == CURRENT and os.readlink(item) == 'runtime':
            target.symlink_to('runtime')
            files[name] = {'link': 'runtime'}
        elif stat.S_ISREG(mode) and not mode & 0o7022:
            # Package only the portable files; do not copy the Android source tree.
            with item.open('rb') as stream:
                check_content(name, stream.read(4096), item.stat().st_size, component)
            os.link(item, target, follow_symlinks=False)
            files[name] = {'sha256': sha(item), 'bytes': item.stat().st_size, 'mode': mode & 0o777}
        else:
            raise ValueError('Unsupported upstream staged file type or mode: ' + name)
    return files


def guest_files(component, files):
    """Record Android bytes separately from distribution-added documentation."""
    return {name: value for name, value in files.items()
            if name.startswith((LIB + '/', LICENSES + component + '/'))}


def stage_guest(payload, archives, destination, policy):
    if payload:
        label = payload / 'runtime' / SHARE / 'package.json'
        if label.resolve() != label or json.loads(label.read_text()) != upstream_identity(policy):
            raise ValueError('Upstream staged output must be the pinned ARM64 release')
        source = {'kind': 'upstream-stage'}
    else:
        source = {'kind': 'release-archives', 'archives': {}}
    for component in COMPONENTS:
        notices_only = component == 'runtime'
        output = destination / component
        output.mkdir(parents=True)
        if payload:
            files = stage_tree(payload / component, output, component, notices_only)
        else:
            name = filename(component, upstream_version(policy), 'pacman')
            archive = archives / name
            digest = policy['arm64_archives'][component]
            if not archive.is_file() or sha(archive) != digest:
                raise ValueError('Droidloom release archive does not match its pin: ' + str(archive))
            package_metadata(archive, component, 'pacman', upstream_version(policy))
            files = archive_payload(archive, component, 'pacman', destination=output,
                                    notices_only=notices_only)
            source['archives'][name] = {'sha256': digest, 'url': policy['release_url'] + '/' + name}
        if notices_only:
            if not {LICENSES + 'runtime/LICENSE', LICENSES + 'runtime/rust/index.json'} <= files.keys():
                raise ValueError('Missing upstream runtime license notices')
        else:
            require_payload(component, files, 'pacman')
            source[component] = guest_files(component, files)
    return source


def host_abi(sysroot, package_format):
    libraries = sysroot / 'usr/lib'
    if package_format == 'debian':
        libraries /= 'aarch64-linux-gnu'
    pkgconf = libraries / 'pkgconfig'
    env = {**os.environ, 'PKG_CONFIG_PATH': '', 'PKG_CONFIG_SYSROOT_DIR': str(sysroot),
           'PKG_CONFIG_LIBDIR': f'{pkgconf}:{sysroot / "usr/share/pkgconfig"}'}
    stats = {}
    for module in ('gbm', 'wayland-server', 'xkbcommon', 'libpipewire-0.3', 'libdrm'):
        path = pkgconf / (module + '.pc')
        if not path.is_file():
            raise ValueError('Not a complete ' + package_format + ' ARM64 development sysroot: ' + str(path))
        stats[module] = {
            'version': subprocess.check_output(['pkg-config', '--modversion', module],
                                               env=env, text=True).strip(),
            'sha256': sha(path),
        }
    libc = libraries / 'libc.so.6'
    with libc.open('rb') as stream:
        check_content('usr/bin/droidloomd', stream.read(64), libc.stat().st_size, 'runtime')
    stats['libc'] = {'sha256': sha(libc)}
    return stats


def stage_runtime(source, runtime, work, sysroot, package_format, policy, jobs, env):
    target = 'aarch64-unknown-linux-gnu'
    host_work = work / 'host'
    host_work.mkdir()
    build_env = {**env, 'CARGO_TARGET_DIR': str(work / 'cargo-tools')}
    stages = ['cargo', 'run', '--locked', '--release', '-j', str(jobs), '-p', 'droidloom-update',
              '--', '--jobs', str(jobs), 'host-build', '--work', host_work, '--sysroot', sysroot]
    subprocess.run(stages, cwd=source, env=build_env, check=True)
    binaries = host_work / ('cargo-' + target) / target / 'release'
    for path in HOST_PROGRAMS:
        executable = binaries / Path(path).name
        with executable.open('rb') as stream:
            check_content(path, stream.read(64), executable.stat().st_size, 'runtime')
        copy_file(executable, runtime / path)
    packing = source / 'packaging'
    for group in ('system', 'user'):
        for unit in (packing / 'systemd' / group).iterdir():
            if unit.suffix in ('.service', '.socket'):
                copy_file(unit, runtime / HOST_SYSTEMD / group / unit.name)
    copy_file(packing / 'arch/droidloom.service', runtime / HOST_SYSTEMD / 'user/droidloom.service')
    copy_file(packing / 'arch' / Path(POLKIT).name, runtime / POLKIT)
    if package_format == 'pacman':
        for name in HOOK_FILES:
            copy_file(packing / 'arch' / name, runtime / HOOKS / name)
    template = json.loads((packing / 'cell-spec-arm64.json').read_text())
    for key in ('host_uid', 'render_node', 'data_dir', 'runtime_dir', 'denial_socket'):
        template.pop(key, None)
    template.update(image_dir='/' + LIB + '/images', vendor_image='/' + PARTITION_DIR + '/vendor.raw.img',
                    share_user_folders=False, shared_storage_directories=[])
    atomic_json(runtime / SHARE / 'cell-template.json', template)
    atomic_json(runtime / SHARE / 'package.json', upstream_identity(policy))


def copy_file(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)


def build_debian(work, sysroot, package_version, epoch, env):
    recipe = work / 'source-package'
    recipe.mkdir()
    debian = recipe / 'debian'
    shutil.copytree(RECIPE / 'debian', debian)
    (debian / 'rules').chmod(0o755)
    for component in ('image', 'base'):
        copy_file(debian / 'guest.prerm', debian / ('droidloom-' + component + '.prerm'))
    (debian / 'guest.prerm').unlink()
    timestamp = format_datetime(datetime.fromtimestamp(epoch, timezone.utc))
    (debian / 'changelog').write_text(
        f'droidloom ({package_version}) unstable; urgency=medium\n\n'
        '  * Package Droidloom and its ARM64 Android images.\n\n'
        f' -- {MAINTAINER}  {timestamp}\n')
    command = ['dpkg-buildpackage', '-T', 'binary', '-us', '-uc']
    if sysroot == Path('/'):
        if subprocess.check_output(['dpkg', '--print-architecture'], text=True).strip() != 'arm64':
            raise ValueError('Use a Debian ARM64 package builder')
        subprocess.run(command, cwd=recipe, env={**env, 'DROIDLOOM_PAYLOAD': str(work / 'payload')}, check=True)
    else:
        if not (sysroot / 'usr/bin/dh').is_file() or not (sysroot / 'dev/null').exists():
            raise ValueError('Use a Debian ARM64 build root with debhelper, dpkg-dev and /dev/null')
        sandbox = ['unshare', '--mount', '--propagation', 'private']
        if os.geteuid() != 0:
            sandbox += ['--map-root-user']
        # The bind mount only exists in the child namespace; the target root is unchanged.
        subprocess.run([*sandbox, 'sh', '-ec',
                        'work=$1 sysroot=$2 epoch=$3; shift 3\n'
                        'mount --bind "$work" "$sysroot/tmp"\n'
                        'exec chroot "$sysroot" /usr/bin/env LC_ALL=C TZ=UTC '
                        'SOURCE_DATE_EPOCH="$epoch" DROIDLOOM_PAYLOAD=/tmp/payload '
                        'sh -ec \'test "$(dpkg --print-architecture)" = arm64; '
                        'cd /tmp/source-package; exec "$@"\' sh "$@"',
                        'droidloom-debian', work, sysroot, str(epoch), *command],
                       env=env, check=True)
    return work


def build_pacman(work, policy, env):
    if os.geteuid() == 0:
        raise ValueError('Run the pacman package build as an ordinary user')
    recipe = work / 'pacman'
    recipe.mkdir()
    shutil.copy2(RECIPE / 'PKGBUILD', recipe)
    config = recipe / 'makepkg.conf'
    config.write_text('. /etc/makepkg.conf\nCARCH=aarch64\n')
    subprocess.run(['makepkg', '--noconfirm', '--nodeps', '--config', config], cwd=recipe,
                   env={**env, 'DROIDLOOM_PAYLOAD': str(work / 'payload'),
                        'DROIDLOOM_VERSION': policy['version'],
                        'DROIDLOOM_PKGREL': f"{policy['release']}.{policy['package_revision']}",
                        'PKGEXT': '.pkg.tar.zst', 'PACKAGER': MAINTAINER,
                        **{name: str(recipe) for name in ('BUILDDIR', 'SRCDEST', 'PKGDEST', 'LOGDEST')}},
                   check=True)
    return recipe


def check_packages(directory, package_format, policy, provenance):
    package_version = version(policy, package_format)
    result, owned = {}, set()
    for component in COMPONENTS:
        name = filename(component, package_version, package_format)
        path = directory / name
        if path.is_symlink() or not path.is_file():
            raise ValueError('Missing native Droidloom package: ' + str(path))
        package_metadata(path, component, package_format, package_version)
        files = archive_payload(path, component, package_format)
        require_payload(component, files, package_format)
        if owned & files.keys():
            raise ValueError('Droidloom packages install overlapping files')
        owned.update(files)
        if component == 'runtime':
            if files[SHARE + '/package.json']['json'] != upstream_identity(policy):
                raise ValueError('Native runtime identity does not match the pinned source')
        elif guest_files(component, files) != provenance[component]:
            raise ValueError('Native package rewrote Android files or notices: ' + name)
        result[name] = {'name': 'droidloom-' + component, 'version': package_version,
                        'architecture': ARCHITECTURES[package_format], 'sha256': sha(path)}
    return result


def verify_archives(directory, package_format=None):
    """Read and independently inspect all three native archives before publication."""
    directory = Path(directory)
    receipt = directory / 'manifest.json'
    if directory.is_symlink() or receipt.is_symlink():
        raise ValueError('Droidloom package generation must not be a symlink')
    record = json.loads(receipt.read_text())
    policy = read_policy()
    fmt = record['format']
    source = record['source']
    if (record['schema_version'] != 1 or fmt not in ARCHITECTURES or
            (package_format and package_format != fmt) or source['commit'] != policy['commit'] or
            source['archive_sha256'] not in (None, policy['source_sha256']) or
            not is_sha256(source['cargo_lock_sha256']) or
            source['patches'] != {path.name: sha(path) for path in patches()}):
        raise ValueError('Droidloom package generation differs from its source or target')
    provenance = record['guest']
    if provenance['kind'] == 'release-archives':
        archive_rows = provenance['archives']
        if archive_rows != {
            filename(component, upstream_version(policy), 'pacman'): {
                'sha256': digest,
                'url': policy['release_url'] + '/' + filename(component, upstream_version(policy), 'pacman'),
            } for component, digest in policy['arm64_archives'].items()
        }:
            raise ValueError('Unrecognized upstream Droidloom archive provenance')
    elif provenance['kind'] != 'upstream-stage':
        raise ValueError('Unrecognized upstream Droidloom guest source')
    packages = check_packages(directory, fmt, policy, provenance)
    if record['packages'] != packages or set(directory.iterdir()) != {
            receipt, *(directory / name for name in packages)}:
        raise ValueError('Droidloom package generation does not match its manifest')
    return {name: (directory / name, row['sha256']) for name, row in packages.items()}


def build(args):
    policy = read_policy()
    fmt = args.format
    sysroot = args.sysroot.resolve(strict=True)
    metadata = host_abi(sysroot, fmt)
    epoch = policy['source_date_epoch']
    env = {**os.environ, 'SOURCE_DATE_EPOCH': str(epoch), 'LC_ALL': 'C', 'TZ': 'UTC'}
    output = args.output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError('Use an unused Droidloom package output directory')
    output.parent.mkdir(parents=True, exist_ok=True)
    payload = args.payload.resolve(strict=True) if args.payload else None
    if payload and payload.stat().st_dev != output.parent.stat().st_dev:
        raise ValueError('Put --output on the same filesystem as --payload to avoid copying the images')
    with tempfile.TemporaryDirectory(prefix='droidloom-', dir=output.parent) as temporary:
        work = Path(temporary).resolve()
        source = work / 'source'
        take_source(args.source.resolve(strict=True), source, policy)
        provenance = stage_guest(payload, args.archives, work / 'payload', policy)
        stage_runtime(source, work / 'payload/runtime', work, sysroot, fmt, policy, args.jobs, env)
        package_version = version(policy, fmt)
        archives = (build_debian(work, sysroot, package_version, epoch, env) if fmt == 'debian'
                    else build_pacman(work, policy, env))
        packages = check_packages(archives, fmt, policy, provenance)
        record = {
            'schema_version': 1, 'format': fmt, 'source': {
                'commit': policy['commit'],
                'archive_sha256': policy['source_sha256'] if args.source.is_file() else None,
                'cargo_lock_sha256': sha(source / 'Cargo.lock'),
                'patches': {path.name: sha(path) for path in patches()},
            }, 'guest': provenance, 'packages': packages,
            'build': {**metadata, **{
                name: subprocess.check_output([name, '--version'], cwd=source, text=True).splitlines()[0]
                for name in ('rustc', 'cargo', 'aarch64-linux-gnu-gcc')}},
        }
        finished = work / 'packages'
        finished.mkdir()
        for name in packages:
            (archives / name).rename(finished / name)
        atomic_json(finished / 'manifest.json', record)
        if output.exists() or output.is_symlink():
            raise ValueError('Droidloom package output appeared during the build')
        finished.rename(output)
    return packages


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--format', choices=ARCHITECTURES)
    parser.add_argument('--source', type=Path, help='pinned upstream source archive or Git checkout')
    parser.add_argument('--sysroot', type=Path, help='target ARM64 development root')
    guest = parser.add_mutually_exclusive_group()
    guest.add_argument('--payload', type=Path, help='upstream ARM64 package-stage directory')
    guest.add_argument('--archives', type=Path, help='directory with the three pinned upstream ARM64 archives')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--verify', action='store_true', help='only verify the completed --output generation')
    parser.add_argument('--jobs', type=int, default=max(1, default_jobs() - 2))
    args = parser.parse_args()
    if args.verify:
        if any((args.source, args.sysroot, args.payload, args.archives)):
            parser.error('--verify does not take build inputs')
        names = verify_archives(args.output, args.format)
    else:
        if not all((args.format, args.source, args.sysroot, args.payload or args.archives)) or args.jobs < 1:
            parser.error('building requires --format, --source, --sysroot and --payload or --archives')
        names = build(args)
    for name in names:
        print(name)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError, tarfile.TarError) as error:
        raise SystemExit(str(error))
