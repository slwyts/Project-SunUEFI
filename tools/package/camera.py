"""Build and verify the independent Piano camera userspace packages."""
import argparse
from datetime import datetime, timezone
from email.utils import format_datetime
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / 'tools') not in sys.path:
    sys.path.insert(0, str(ROOT / 'tools'))

from build.cache import atomic_json, source_identity
from build.overlay_packages import debian_identity, pacman_identity, sha
from build_piano_ram_bootstrap import guest_resolve
from build_piano_runtime_helpers import CAMERA_RUNTIME_PATHS, build_camera, verify_elf
import package_bsp


NAME = 'piano-camera'
DESCRIPTION = 'Camera service and controls for the Xiaomi Pad 8 Pro'
MAINTAINER = 'Project SunUEFI <slwyts@tuta.io>'
URL = 'https://github.com/slwyts/Project-SunUEFI'
UNIT = 'piano-camerad.service'
COPYRIGHT = 'usr/share/doc/piano-camera/copyright'
OUTPUT = ROOT / 'artifacts/packages/camera'
FORMATS = {'debian': (debian_identity, 'arm64'), 'pacman': (pacman_identity, 'aarch64')}


def policy():
    return json.loads((ROOT / 'config/release.json').read_text())['camera_package']


def cache_digest():
    inputs = source_identity(ROOT, (
        'tools/package/camera.py', 'tools/build_piano_runtime_helpers.py',
        'tools/build/cache.py', 'tools/build/overlay_packages.py', 'tools/package_bsp.py',
        'linux/userspace/camera', 'linux/bsp/optional/camera', 'patches/demosaic',
    ))
    manifest = json.loads((ROOT / 'linux/bsp/manifest.json').read_text())
    inputs['camera_configuration'] = [row for row in manifest['files'] if row['group'] == 'camera']
    image_id = os.environ.get('SUNUEFI_BUILD_ENVIRONMENT_ID')
    if not image_id:
        # Standalone builds run in the same Debian builder, without a CI image ID.
        image_id = hashlib.sha256(subprocess.check_output(['dpkg-query', '-W'])).hexdigest()
    record = {'policy': policy(), 'inputs': inputs, 'architecture': platform.machine(),
              'builder': image_id, 'workspace': os.environ.get('SUNUEFI_CACHE_WORKSPACE', str(ROOT))}
    return hashlib.sha256(json.dumps(record, sort_keys=True).encode()).hexdigest()


def filename(package_format, version):
    if package_format == 'debian':
        return f'{NAME}_{version}_arm64.deb'
    return f'{NAME}-{version}-aarch64.pkg.tar.zst'


def configuration():
    _, rows = package_bsp.payload(['camera'])
    return {row['path']: (data, int(row['mode'], 8)) for row, data in rows
            if row['group'] == 'camera'}


def package_contents(build, files):
    """Check the builder's exact output and add the existing configuration."""
    if set(files) != CAMERA_RUNTIME_PATHS:
        raise ValueError('Camera package requires the complete camera build output')
    build = build.resolve()
    contents = configuration()
    for name, item in sorted(files.items()):
        relative = Path(item['file'])
        source = build / relative
        if (relative.is_absolute() or '..' in relative.parts or
                not source.resolve().is_relative_to(build) or
                source.is_symlink() or not source.is_file()):
            raise ValueError('Invalid camera build file: ' + name)
        mode = 0o644 if name.startswith('usr/share/licenses/') else 0o755
        data = source.read_bytes()
        if ((len(data), package_bsp.sha(data), item['mode']) !=
                (item['bytes'], item['sha256'], mode)):
            raise ValueError('Camera build file differs from the manifest: ' + name)
        if mode == 0o755:
            verify_elf(source)
        contents[name] = (data, mode)
    texts = [data.strip() for name, (data, _) in sorted(contents.items())
             if name.startswith('usr/share/licenses/')]
    contents[COPYRIGHT] = (f'Source: {URL}\n\n'.encode() +
                           b'\n\n'.join(texts) + b'\n', 0o644)
    return contents


def stage(tree, contents):
    tree.mkdir(parents=True)
    for name, (data, mode) in contents.items():
        destination = tree / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        destination.chmod(mode)
    for directory in (tree, *[path for path in tree.rglob('*') if path.is_dir()]):
        directory.chmod(0o755)


def debian_preinst(paths):
    quoted = ' '.join(shlex.quote('/' + name) for name in sorted(paths))
    return (
        '#!/bin/sh\nset -eu\n'
        f'for path in {quoted}; do\n'
        '  if [ -e "$path" ] || [ -L "$path" ]; then\n'
        '    owner=$(dpkg-query --search "$path" 2>/dev/null || true)\n'
        f'    case "$owner" in "{NAME}: $path"|"{NAME}:arm64: $path") ;;\n'
        '      *) printf "%s\\n" "Camera file already exists: $path ($owner)" >&2\n'
        '         printf "%s\\n" "Check ownership before moving it aside." >&2\n'
        '         exit 1 ;;\n'
        '    esac\n'
        '  fi\n'
        'done\n'
    )


def debian(work, contents, version, kernel_version, epoch, env):
    source = work / 'debian'
    tree = source / NAME
    stage(tree, contents)
    (source / 'control').write_text(
        f'Source: {NAME}\nSection: video\nPriority: optional\nMaintainer: {MAINTAINER}\n'
        'Build-Depends: debhelper-compat (= 13)\nRules-Requires-Root: no\n'
        f'Homepage: {URL}\n\n'
        f'Package: {NAME}\nArchitecture: arm64\n'
        'Depends: ${misc:Depends}, init-system-helpers, systemd, udev, kmod, piano-device-config, '
        f'wireplumber (>= 0.5), linux-image-piano (>= {kernel_version})\n'
        f'Description: {DESCRIPTION}\n'
        ' Provides the current V4L2 loopback camera streams and controls.\n')
    (source / 'changelog').write_text(
        f'{NAME} ({version}) unstable; urgency=medium\n\n'
        '  * Package the Piano camera service and control tool.\n\n'
        f' -- {MAINTAINER}  {format_datetime(datetime.fromtimestamp(epoch, timezone.utc))}\n')
    preinst = source / (NAME + '.preinst')
    preinst.write_text(debian_preinst(contents))
    preinst.chmod(0o755)
    env = {**env, 'DEB_HOST_ARCH': 'arm64', 'DEB_RULES_REQUIRES_ROOT': 'no'}
    for command in (['dh_installsystemd', '-p' + NAME, UNIT],
                    ['dh_installdeb', '-p' + NAME],
                    ['dh_gencontrol', '-p' + NAME]):
        subprocess.run(command, cwd=work, env=env, check=True)
    archive = work / f'{NAME}_{version}_arm64.deb'
    subprocess.run(['dpkg-deb', '--root-owner-group', '--build', tree, archive],
                   env=env, check=True)
    return archive


def pacman(work, contents, version, revision, kernel_version, epoch, env):
    payload = work / 'payload.tar'
    with tarfile.open(payload, 'w') as archive:
        for name, (data, mode) in sorted(contents.items()):
            member = tarfile.TarInfo(name)
            member.size, member.mode, member.mtime = len(data), mode, epoch
            member.uname = member.gname = 'root'
            archive.addfile(member, io.BytesIO(data))
    configuration_paths = ' '.join(shlex.quote(name) for name in sorted(contents)
                                   if name.startswith('etc/'))
    recipe = work / 'PKGBUILD'
    recipe.write_text(
        f'pkgname={NAME}\npkgver={version}\npkgrel={revision}\n'
        f'pkgdesc={shlex.quote(DESCRIPTION)}\nurl={shlex.quote(URL)}\n'
        'arch=(aarch64)\nlicense=(MIT)\n'
        f"depends=(systemd kmod piano-device-config 'wireplumber>=0.5' 'linux-piano>={kernel_version}')\n"
        f'backup=({configuration_paths})\n'
        'options=(!strip !debug)\nsource=(payload.tar)\nnoextract=(payload.tar)\n'
        f'sha256sums=({sha(payload)})\n'
        'package() { bsdtar --no-same-owner -xf "$srcdir/payload.tar" -C "$pkgdir"; }\n')
    config = work / 'makepkg.conf'
    config.write_text('. /etc/makepkg.conf\nCARCH=aarch64\n')
    command = ['makepkg', '--noconfirm', '--nodeps', '--config', config]
    if account := package_bsp.pacman_builder_account():
        for path in (work, payload, recipe, config):
            shutil.chown(path, account.pw_uid, account.pw_gid)
        command = ['runuser', '-u', account.pw_name, '--', *command]
    env = {**env, 'PACKAGER': MAINTAINER, 'PKGEXT': '.pkg.tar.zst',
           **{name: str(work) for name in ('BUILDDIR', 'SRCDEST', 'PKGDEST', 'LOGDEST')}}
    subprocess.run(command, cwd=work, env=env, check=True)
    return work / f'{NAME}-{version}-{revision}-aarch64.pkg.tar.zst'


def archive_contents(package_format, archive):
    """Check the native payload has no duplicate paths and only regular files."""
    data = subprocess.check_output(['dpkg-deb', '--fsys-tarfile', archive]
                                   if package_format == 'debian' else ['zstd', '-dq', '-c', archive])
    result = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:') as members:
        for member in members:
            name = member.name.removeprefix('./')
            if member.isdir() or (package_format == 'pacman' and
                                  name in ('.PKGINFO', '.BUILDINFO', '.MTREE')):
                continue
            if not member.isfile() or member.uid or member.gid or name in result:
                raise ValueError(f'Invalid camera package payload: {member.name}')
            result[name] = (members.extractfile(member).read(), member.mode)
    return result


def build_package(package_format, output, recipe, build, files, *, version, revision, epoch,
                  kernel_version):
    """Build one native archive from the verified ``build_camera`` files."""
    if package_format not in FORMATS:
        raise ValueError('Use Debian or pacman camera packaging')
    if (not re.fullmatch(r'[0-9][0-9A-Za-z.+]*', version) or
            not re.fullmatch(r'[1-9][0-9]*(?:\.[0-9]+)?', revision) or
            type(epoch) is not int or epoch < 0 or
            not re.fullmatch(r'[0-9][0-9A-Za-z.+:~_-]*', kernel_version)):
        raise ValueError('Invalid camera package or kernel dependency version')
    output, recipe, build = map(Path, (output, recipe, build))
    if recipe.is_symlink() or recipe.exists() or output.resolve().is_relative_to(recipe.resolve()):
        raise ValueError('Use a fresh recipe directory outside the camera package output')
    identity, arch = FORMATS[package_format]
    full_version = f'{version}-{revision}'
    destination = output / filename(package_format, full_version)
    if destination.exists() or destination.is_symlink():
        raise ValueError('Camera package already exists: ' + str(destination))
    contents = package_contents(build, files)
    env = {**os.environ, 'SOURCE_DATE_EPOCH': str(epoch), 'LC_ALL': 'C', 'TZ': 'UTC'}
    recipe.mkdir(parents=True)
    try:
        if package_format == 'debian':
            archive = debian(recipe.resolve(), contents, full_version, kernel_version, epoch, env)
        else:
            archive = pacman(recipe.resolve(), contents, version, revision, kernel_version, epoch, env)
        if (archive.is_symlink() or identity(archive) != (NAME, full_version, arch) or
                archive_contents(package_format, archive) != contents):
            raise ValueError('Native camera archive differs from the verified payload')
        output.mkdir(parents=True, exist_ok=True)
        with archive.open('rb') as source, destination.open('xb') as target:
            shutil.copyfileobj(source, target)
    finally:
        shutil.rmtree(recipe)
    return destination, NAME, full_version


def shared_package(package_format, directory=None, *, digest=None):
    """Read the exact package and its metadata from a verified generation."""
    directory = OUTPUT / package_format if directory is None else Path(directory)
    receipt = directory / 'manifest.json'
    if directory.is_symlink() or receipt.is_symlink():
        raise ValueError('Camera package cache must not contain symlinks')
    record = json.loads(receipt.read_text())
    settings = policy()
    version = settings['version'] + '-' + settings['revision']
    read_identity, arch = FORMATS[package_format]
    metadata = record['package']
    archive = directory / filename(package_format, version)
    if (record.get('schema_version') != 1 or record.get('build') != (digest or cache_digest()) or
            (metadata['name'], metadata['version'], metadata['architecture']) != (NAME, version, arch) or
            archive.is_symlink() or not archive.is_file() or sha(archive) != metadata['sha256'] or
            read_identity(archive) != (NAME, version, arch)):
        raise ValueError('Camera package cache differs from the current build: ' + str(directory))
    return archive, metadata


def compile_userspace(work, epoch):
    native = platform.machine() in ('aarch64', 'arm64')
    cc, cxx = ('cc', 'c++') if native else ('aarch64-linux-gnu-gcc', 'aarch64-linux-gnu-g++')
    emulator = None if native else shutil.which('qemu-aarch64') or shutil.which('qemu-aarch64-static')
    if not native and not emulator:
        raise ValueError('Cross-building the camera package requires qemu-aarch64')
    env = {**os.environ, 'SOURCE_DATE_EPOCH': str(epoch), 'LC_ALL': 'C', 'TZ': 'UTC'}
    if not native:
        env['PKG_CONFIG_LIBDIR'] = '/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig'
    try:
        files, _ = build_camera(work, [cc, '-O2', '-Wall', '-Wextra'], [cxx],
                                env=env, emulator=emulator)
    except subprocess.CalledProcessError:
        log = work / 'camera/build.log'
        if log.is_file():
            print('\n'.join(log.read_text().splitlines()[-60:]), file=sys.stderr)
        raise
    return files


def ensure_packages(formats, *, verify_only=False):
    work = ROOT / 'build/camera-package'
    work.mkdir(parents=True, exist_ok=True)
    with (work / 'build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        digest, settings = cache_digest(), policy()
        results, missing = {}, []
        for package_format in formats:
            try:
                results[package_format] = shared_package(package_format, digest=digest)
            except (FileNotFoundError, KeyError, ValueError):
                if verify_only:
                    raise
                missing.append(package_format)
        if not missing:
            return results
        build = work / 'userspace'
        if build.exists():
            shutil.rmtree(build)
        epoch = settings['source_date_epoch']
        files = compile_userspace(build, epoch)
        if OUTPUT.is_symlink():
            raise ValueError('Use a regular camera package output directory')
        OUTPUT.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='camera-', dir=OUTPUT) as temporary:
            for package_format in missing:
                stage = Path(temporary) / package_format
                archive, name, version = build_package(
                    package_format, stage, work / ('recipe-' + package_format), build, files,
                    version=settings['version'], revision=settings['revision'], epoch=epoch,
                    kernel_version=settings['minimum_kernel_package_version'])
                record = {'name': name, 'version': version,
                          'architecture': FORMATS[package_format][1], 'sha256': sha(archive)}
                atomic_json(stage / 'manifest.json',
                            {'schema_version': 1, 'build': digest, 'package': record})
                shared_package(package_format, stage, digest=digest)
            if cache_digest() != digest:
                raise ValueError('Camera package inputs changed while building')
            for package_format in missing:
                destination = OUTPUT / package_format
                if destination.is_symlink():
                    raise ValueError('Use a regular package directory: ' + str(destination))
                if destination.exists():
                    shutil.rmtree(destination)
                (Path(temporary) / package_format).rename(destination)
                results[package_format] = shared_package(package_format, digest=digest)
        return results


def ensure(package_format):
    return ensure_packages((package_format,))[package_format]


def verify_installed(root, package_format, archive):
    """Verify that the product kept the package's files, not a staged copy."""
    if package_format == 'debian':
        arguments = ('dpkg-query', '-L', NAME)
    else:
        arguments = ('pacman', '-Qlq', NAME)
    owned = set(subprocess.check_output(['chroot', root, *arguments], text=True).splitlines())
    for name, (data, mode) in archive_contents(package_format, archive).items():
        relative = Path(name)
        path = guest_resolve(root, relative.parent.as_posix()) / relative.name
        if '/' + name not in owned or path.is_symlink() or not path.is_file():
            raise ValueError(f'{NAME} does not own /{name} in the product root')
        if not name.startswith('etc/') and (path.read_bytes() != data or
                                            stat.S_IMODE(path.stat().st_mode) != mode):
            raise ValueError('Packaged camera file was overwritten: /' + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--format', choices=(*FORMATS, 'both'), default='both')
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--cache-key', action='store_true')
    action.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    if args.cache_key:
        print('camera-packages-v1-' + cache_digest())
        return
    formats = tuple(FORMATS) if args.format == 'both' else (args.format,)
    results = ensure_packages(formats, verify_only=args.verify)
    print(json.dumps({fmt: metadata for fmt, (_, metadata) in results.items()}, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
