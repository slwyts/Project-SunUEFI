#!/usr/bin/env python3
"""Package the configured kernel with upstream bindeb-pkg or pacman-pkg."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build_piano_full_kernel import (kernel_build_environment, kernel_release_matches,
                                     release_kernel_policy, toolchain)
from stage_piano_kernel_modules import inspect

DEBIAN_REVISION = 2


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def kernel_source(source, manifest):
    """Bind packaging to the source in the sealed build, not a moving release tip."""
    commit, tree = subprocess.check_output([
        'git', '-C', source, 'show', '-s', '--format=%H%n%T', 'HEAD'],
        text=True).splitlines()
    if (commit, tree) != (manifest['source_commit'], manifest['source_tree']):
        raise ValueError('Packaging source differs from the sealed kernel worktree')
    if subprocess.check_output([
            'git', '-C', source, 'status', '--porcelain=v1', '--untracked-files=all']):
        raise ValueError('Kernel packaging source has local changes')


def package_versions(kernel_version, release, package_format, epoch):
    """Keep the binary ABI and independently updated boot configuration distinct."""
    revision = re.search(r'-b([1-9]\d*)-g([0-9a-f]{12})$', release)
    if not revision:
        raise ValueError('Rebuild the kernel with the configured ABI epoch before packaging')
    abi_epoch, digest = revision.groups()
    # Order first by kernel ABI generation, then by packaging revision.
    if epoch < int(abi_epoch):
        raise ValueError('Package epoch precedes the sealed ABI epoch')
    base = re.sub(r'^(\d+\.\d+\.\d+)-rc',
                  r'\1~rc' if package_format == 'debian' else r'\1rc', kernel_version)
    if package_format == 'pacman':
        base = base.replace('-', '_')
    return (base + '+' + abi_epoch + '.' + digest,
            base + '+' + '.'.join((abi_epoch, str(epoch), digest)))


def debian_image_dependencies(kernel_version, release, abi_epoch):
    from package.boot import dtb_package_name

    # Do not change the image package when only package_epoch advances.
    _, boot_version = package_versions(kernel_version, release, 'debian', abi_epoch)
    return (f'{dtb_package_name(release)}, '
            f'piano-boot-config (>= {boot_version}-{DEBIAN_REVISION})')


def package_plan(kernel, build, output, package_format, jobs):
    manifest, identity = inspect(kernel)
    source = Path(manifest['compiler_worktree']).resolve()
    build, output = build.resolve(), output.resolve()
    if not build.is_relative_to(ROOT / 'build/kernels') or not output.is_relative_to(ROOT / 'artifacts/packages'):
        raise ValueError('Use project kernel build and artifacts/packages output directories')
    if not source.is_relative_to(ROOT / 'build/kernel-worktrees'):
        raise ValueError('Use the kernel compiler worktree owned by the build')
    if sha(build / '.config') != manifest['config_sha256'] or (
            sha(build / 'Module.symvers') != manifest['module_symvers_sha256']):
        raise ValueError('Build configuration/symbols differ from the sealed kernel')
    image_digest = manifest['image']['sha256']
    if sha(build / 'arch/arm64/boot/Image') != image_digest:
        raise ValueError('Build Image differs from the sealed kernel')
    release = manifest['kernel_release']
    # Preserve the full release including the source/config ABI suffix.
    if not kernel_release_matches(manifest, build):
        raise ValueError('Build release differs from the sealed ABI inputs; rebuild the kernel')
    config = (build / '.config').read_text()
    configured = re.search(r'^CONFIG_LOCALVERSION="(.*)"$', config, re.M)
    configured = configured[1] if configured else ''
    kernel_version = subprocess.check_output(
        ['make', '-s', '-C', source, 'kernelversion'], text=True).strip()
    suffix = release.removeprefix(kernel_version + configured)
    if not release.startswith(kernel_version + configured):
        raise ValueError('Kernel release differs from source/configuration')
    release_epoch = release_kernel_policy()['package_epoch']
    abi_version, package_version = package_versions(
        kernel_version, release, package_format, release_epoch)
    command = ['make', '-C', str(source), 'O=' + str(build), 'ARCH=arm64',
               'LLVM=1', 'LLVM_IAS=1', 'KBUILD_IMAGE=arch/arm64/boot/Image',
               'LOCALVERSION=' + suffix, '-j' + str(jobs)]
    abi_epoch = manifest['kernel_policy']['abi_epoch']
    environment = kernel_build_environment(abi_epoch)
    environment.update(DEBFULLNAME='Project SunUEFI', DEBEMAIL='slwyts@tuta.io',
                       PACKAGER='Project SunUEFI <slwyts@tuta.io>', LC_ALL='C', TZ='UTC')
    if platform.machine() not in ('aarch64', 'arm64'):
        command += ['CROSS_COMPILE=aarch64-linux-gnu-']
    revision = DEBIAN_REVISION if package_format == 'debian' else 1
    if package_format == 'debian':
        command += [f'KDEB_PKGVERSION={abi_version}-{revision}', 'KDEB_SOURCENAME=linux-piano',
                    'KDEB_CHANGELOG_DIST=unstable', 'KBUILD_BUILD_VERSION=1', 'bindeb-pkg']
        environment['SUNUEFI_DEBIAN_IMAGE_DEPENDS'] = debian_image_dependencies(
            kernel_version, release, abi_epoch)
        # Do not build a redundant multi-gigabyte debug image package.
        profiles = os.environ.get('DEB_BUILD_PROFILES', '').split()
        environment['DEB_BUILD_PROFILES'] = ' '.join(dict.fromkeys(
            [*profiles, 'pkg.linux-piano.nokerneldbg']))
    else:
        environment.update(PACMAN_PKGBASE='linux-piano-' + release.lower().replace('_', '-'),
                           PACMAN_EXTRAPACKAGES='headers',
                           PKGDEST=str(output), PKGEXT='.pkg.tar.zst')
        command += ['MAKEPKGOPTS=--noconfirm --nodeps --force', 'pacman-pkg']
    return {'format': package_format, 'kernel_release': release, 'manifest_sha256': identity,
            'kernel': str(Path(kernel).resolve()), 'config_sha256': manifest['config_sha256'],
            'module_symvers_sha256': manifest['module_symvers_sha256'],
            'image_sha256': image_digest,
            'source_commit': manifest['source_commit'], 'source_tree': manifest['source_tree'],
            'source': str(source), 'build': str(build), 'output': str(output),
            'command': command, 'environment': environment, 'version': package_version,
            'abi_version': abi_version, 'revision': revision, 'package_epoch': release_epoch}


def verify_headers(archive, package_format, *, release=None, symbols=None):
    command = (['dpkg-deb', '--fsys-tarfile', archive] if package_format == 'debian'
               else ['zstd', '-dc', archive])
    release_header, packaged_symbols = None, None
    with subprocess.Popen(command, stdout=subprocess.PIPE) as process:
        with tarfile.open(fileobj=process.stdout, mode='r|') as stream:
            members = {}
            for row in stream:
                if not row.isfile():
                    continue
                payload = stream.extractfile(row)
                members[row.name] = payload.read(20)
                if row.name.endswith('/Module.symvers'):
                    packaged_symbols = hashlib.sha256(members[row.name] + payload.read()).hexdigest()
                elif row.name.endswith('/include/generated/utsrelease.h'):
                    release_header = members[row.name] + payload.read()
    if process.returncode:
        raise subprocess.CalledProcessError(process.returncode, command)
    for suffix in ('/include/config/auto.conf', '/Module.symvers', '/include/generated/utsrelease.h',
                   '/scripts/basic/fixdep', '/scripts/mod/modpost'):
        if not any(name.endswith(suffix) for name in members):
            raise ValueError('Headers are missing external-module build input: ' + suffix)
    for name, header in members.items():
        if header.startswith(b'\x7fELF') and header[18:20] != b'\xb7\x00':
            raise ValueError('Headers contain a host executable rather than ARM64: ' + name)
    if symbols and packaged_symbols != symbols:
        raise ValueError('Headers Module.symvers differs from the sealed build')
    if release and release_header != f'#define UTS_RELEASE "{release}"\n'.encode():
        raise ValueError('Headers release differs from the sealed build')


def verify_image(archive, package_format, release, expected, modules=()):
    """Verify the packaged EFI image and every release module against the sealed build."""
    prefix = 'lib/modules/' + release + '/'
    relative = 'boot/vmlinuz-' + release if package_format == 'debian' else prefix + 'vmlinuz'
    command = (['dpkg-deb', '--fsys-tarfile', archive] if package_format == 'debian'
               else ['zstd', '-dc', archive])
    missing = {row['path']: row['sha256'] for row in modules}
    data = b''
    with subprocess.Popen(command, stdout=subprocess.PIPE) as process:
        with tarfile.open(fileobj=process.stdout, mode='r|') as stream:
            for member in stream:
                path = member.name.removeprefix('./').removeprefix('usr/')
                if not member.isfile():
                    continue
                if path == relative:
                    if data:
                        raise ValueError('Packaged kernel contains more than one Image')
                    data = stream.extractfile(member).read()
                elif path.startswith(prefix) and re.search(r'\.ko(?:\.[a-z0-9]+)?$', path):
                    digest = missing.pop(path, None)
                    if digest is None or hashlib.file_digest(
                            stream.extractfile(member), 'sha256').hexdigest() != digest:
                        raise ValueError('Packaged module differs from the sealed build: ' + path)
    if process.returncode:
        raise subprocess.CalledProcessError(process.returncode, command)
    if missing:
        raise ValueError('Packaged kernel is missing sealed modules: ' + ', '.join(sorted(missing)[:3]))
    pe = int.from_bytes(data[60:64], 'little') if len(data) >= 64 else 0
    if (not data.startswith(b'MZ') or data[56:60] != b'ARM\x64' or
            data[pe:pe + 6] != b'PE\0\0\x64\xaa' or
            hashlib.sha256(data).hexdigest() != expected):
        raise ValueError('Packaged kernel differs from the sealed uncompressed EFI Image')


def download_pacman(root, work, packages, guest):
    """Fetch exactly the published kernel generation without refreshing the base repos."""
    from build import overlay_packages

    database, cache = work / 'database', work / 'published'
    database.mkdir()
    cache.mkdir()
    config = work / 'pacman.conf'
    config.write_text('[options]\nArchitecture = aarch64\n'
                      'SigLevel = Required DatabaseRequired\n'
                      'Include = /etc/pacman.d/sunuefi.conf\n')
    records = {}
    for archive, row in packages.items():
        name, version, architecture = overlay_packages.pacman_identity(work / archive)
        if architecture not in ('aarch64', 'any') or not re.fullmatch(r'[a-z0-9][a-z0-9.+_-]*', name):
            raise ValueError('Unexpected native pacman package name: ' + name)
        records[archive] = {'name': name, 'version': version, 'sha256': row['sha256']}

    def relative(path):
        return '/' + path.relative_to(root).as_posix()

    def isolated_pacman(arguments):
        return guest(['pacman', '--config', relative(config), '--dbpath', relative(database),
                      '--nodeps', '--nodeps', *arguments[1:]])

    guest(['pacman-key', '--populate', 'sunuefi'])
    isolated_pacman(['pacman', '-Sy', '--noconfirm'])
    overlay_packages.download_pacman(root, cache, 'sunuefi', records, isolated_pacman)
    return [relative(cache / name) for name in packages]


def install(root, package_format, kernel, *, published=False):
    """Install into a disposable product root using its native package manager."""
    import tempfile
    from build_release_rootfs import build_devices, build_resolver
    from build_chroot import guest_command
    from build_nyarch_apps import guest_environment
    root = Path(root).resolve()
    if not root.is_relative_to(ROOT / 'build/distros'):
        raise ValueError('Install only into a generated product root')
    manifest, kernel_manifest_sha = inspect(kernel)
    directory = ROOT / 'artifacts/packages/kernel' / package_format
    record = json.loads((directory / 'manifest.json').read_text())
    if (record.get('format') != package_format or not record.get('packages') or
            (directory / '.incomplete').exists() or
            record.get('kernel_release') != manifest['kernel_release'] or
            record.get('manifest_sha256') != kernel_manifest_sha):
        raise ValueError('Native kernel packages do not match the shared kernel')
    (root / 'var/tmp').mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='kernel-packages-', dir=root / 'var/tmp') as folder:
        work = Path(folder)
        work.chmod(0o755)
        paths = []
        for name, row in record['packages'].items():
            source = directory / name
            if Path(name).name != name or source.is_symlink() or sha(source) != row['sha256']:
                raise ValueError('Native kernel package changed: ' + name)
            shutil.copy2(source, work / name)
            if sha(work / name) != row['sha256']:
                raise ValueError('Native kernel package changed while staging: ' + name)
            paths.append('/' + (work / name).relative_to(root).as_posix())
        with build_resolver(root):
            if package_format == 'debian':
                with build_devices(root):
                    def apt(*arguments):
                        subprocess.run(guest_command(root, [
                            '/usr/bin/env', 'SYSTEMD_OFFLINE=1', 'SYSTEMD_IN_CHROOT=1',
                            'PATH=/usr/sbin:/usr/bin:/sbin:/bin',
                            'DEBIAN_FRONTEND=noninteractive', 'apt-get', *arguments]), check=True)

                    if published:
                        from build.overlay_packages import debian_identity, download_debian
                        # The validated config is the initial public key and APT source.
                        config, = [path for path in paths if Path(path).name.startswith('piano-boot-config_')]
                        apt('install', '-y', '--no-install-recommends', config)
                        apt('-o', 'APT::Update::Error-Mode=any', '-o', 'APT::Get::List-Cleanup=false',
                            '-o', 'Dir::Etc::sourcelist=sources.list.d/sunuefi.sources',
                            '-o', 'Dir::Etc::sourceparts=-', 'update')
                        expected = {}
                        for name, row in record['packages'].items():
                            package, version, architecture = debian_identity(work / name)
                            expected[name] = {'name': package, 'version': version,
                                              'architecture': architecture, 'sha256': row['sha256']}
                        incoming = work / 'published'
                        incoming.mkdir(mode=0o755)
                        paths = download_debian(root, incoming, expected,
                                                lambda args, **kwargs: subprocess.run(args, check=True, **kwargs))
                    apt('install', '-y', '--no-install-recommends',
                        *(['--reinstall'] if published else []), *paths)
            else:
                with guest_environment(root) as guest:
                    # Rootfs construction has no ESP; the shared startup image
                    # is built separately. Leave installed hooks active on the device.
                    env = dict(os.environ, SUNUEFI_OFFLINE_ROOTFS='1')
                    if published:
                        config, = [path for path in paths if Path(path).name.startswith('piano-boot-config-')]
                        guest(['pacman', '-U', '--noconfirm', config], env=env)
                        paths = download_pacman(root, work, record['packages'], guest)
                    guest(['pacman', '-U', '--noconfirm', *paths], env=env)


def build_packages(plan):
    source, build, output = (Path(plan[name]) for name in ('source', 'build', 'output'))
    if output.exists():
        raise ValueError('Use a fresh package output directory')
    # The build tree is shared with kernel compilation and external modules.
    lock_root = ROOT / 'build/locks'
    lock_root.mkdir(parents=True, exist_ok=True)
    handles = []
    for directory in sorted({source, build}):
        handle = (lock_root / (hashlib.sha256(str(directory).encode()).hexdigest() + '.lock')).open('a')
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        handles.append(handle)
    kernel_source(source, plan)
    kernel_manifest = (Path(plan['kernel']) / 'manifest.json').read_bytes()
    if hashlib.sha256(kernel_manifest).hexdigest() != plan['manifest_sha256']:
        raise ValueError('Sealed kernel changed while preparing packages')
    manifest = json.loads(kernel_manifest)
    output.mkdir(parents=True)
    incomplete = output / '.incomplete'
    incomplete.touch()
    image_digest = plan['image_sha256']
    env, tools = toolchain()
    env.update(plan['environment'])
    env.update(GIT_CONFIG_COUNT='1', GIT_CONFIG_KEY_0='safe.directory',
               GIT_CONFIG_VALUE_0=str(source))
    # Use the compiler identity of the original build to avoid reconfiguring
    # the kernel simply because packaging has a different CC command string.
    command = plan['command'].copy()
    if plan['format'] == 'debian':
        # Add the device's package relationships to upstream's generated metadata.
        shutil.copyfile(Path(__file__).with_name('kernel-debian.mk'), build / 'sunuefi-debian.mk')
        flags = env.get('DPKG_FLAGS', '')
        command.insert(-1, 'DPKG_FLAGS=' + flags +
                       " -R'$(MAKE) -f sunuefi-debian.mk'")
    sealed_tools = manifest['toolchain']
    if (tools['sha256'], tools['kernel_cc']) != (sealed_tools['sha256'], sealed_tools['kernel_cc']):
        raise ValueError('Kernel packaging tools differ from the sealed build; rebuild the shared kernel')
    command.insert(-1, 'CC=' + shlex.join(tools['kernel_cc']))
    if plan['format'] == 'pacman' and os.geteuid() == 0:
        # makepkg deliberately refuses root. These are disposable build trees,
        # not source pins under upstream/ or an installed system.
        import pwd
        try:
            account = pwd.getpwnam('sunuefi-build')
        except KeyError:
            if not Path('/.dockerenv').is_file():
                raise ValueError('Run pacman packaging as a regular user or in the project builder')
            subprocess.run(['useradd', '--system', '--create-home', 'sunuefi-build'], check=True)
            account = pwd.getpwnam('sunuefi-build')
        if tools['ccache_enabled']:
            cache = build / 'package-ccache'
            cache.mkdir(exist_ok=True)
            env['CCACHE_DIR'] = str(cache)
        for path in (source, build, output):
            shutil.chown(path, account.pw_uid, account.pw_gid)
            for directory, folders, files in os.walk(path):
                for name in folders + files:
                    os.lchown(Path(directory) / name, account.pw_uid, account.pw_gid)
        command = ['runuser', '-u', 'sunuefi-build', '--', *command]
    if plan['format'] == 'pacman':
        # Upstream pacman-pkg increments this generated counter for pkgrel.
        # ABI releases already identify source/config; keep #1 in the binary
        # as in the shared kernel rather than recompiling it as build #2.
        (build / '.version').write_text('0\n')
    subprocess.run(command, env=env, check=True)
    if sha(build / 'arch/arm64/boot/Image') != image_digest:
        raise ValueError('Packaging rebuilt a different kernel; rebuild the shared kernel before publishing')
    if sha(build / '.config') != plan['config_sha256'] or (
            sha(build / 'Module.symvers') != plan['module_symvers_sha256']):
        raise ValueError('Packaging changed kernel configuration or symbols; use the same builder as the shared kernel')
    kernel_source(source, plan)
    if plan['format'] == 'debian':
        expected = {'linux-image-' + plan['kernel_release'].lower().replace('_', '-'),
                    'linux-headers-' + plan['kernel_release'].lower().replace('_', '-')}
        selected = []
        for path in build.parent.glob('*.deb'):
            if not any(path.name.startswith(name + '_') for name in expected):
                continue
            name, version = subprocess.check_output(
                ['dpkg-deb', '-f', path, 'Package', 'Version'], text=True).splitlines()
            name = name.removeprefix('Package: ')
            version = version.removeprefix('Version: ')
            # Do not publish linux-libc-dev or replace distro UAPI headers.
            if name in expected and version == f"{plan['abi_version']}-{plan['revision']}":
                shutil.copy2(path, output / path.name)
                selected.append(output / path.name)
        if len(selected) != 2:
            raise ValueError('bindeb-pkg did not produce the matching image and headers')
    else:
        selected = sorted(output.glob('*.pkg.tar.zst'))
        if len(selected) != 2:
            raise ValueError('pacman-pkg did not produce only the image and headers packages')
    image_archive, = (path for path in selected if 'headers' not in path.name)
    if plan['format'] == 'debian':
        depends = subprocess.check_output(['dpkg-deb', '-f', image_archive, 'Depends'], text=True).strip()
        required = plan['environment']['SUNUEFI_DEBIAN_IMAGE_DEPENDS'].split(', ')
        if not all(dependency in depends.split(', ') for dependency in required):
            raise ValueError('Debian image is missing its device tree or boot configuration dependency')
    for path in selected:
        if 'headers' in path.name:
            verify_headers(path, plan['format'], release=plan['kernel_release'],
                           symbols=plan['module_symvers_sha256'])
        else:
            verify_image(path, plan['format'], plan['kernel_release'], image_digest,
                         manifest['modules'])
    from package.boot import add_packages
    from package.v4l2loopback import build_package as build_camera_module
    archive, name, version = build_camera_module(plan, image_archive, env)
    selected.append(archive)
    selected += add_packages(plan, output, manifest, sha, image_modules=((name, version),))
    if sha(Path(plan['kernel']) / 'manifest.json') != plan['manifest_sha256']:
        raise ValueError('Sealed kernel changed while building packages')
    record = {**plan, 'packages': {path.name: {'sha256': sha(path)} for path in selected}}
    (output / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    incomplete.unlink()
    return record


def main():
    from desktop.package_build import default_jobs
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--format', choices=('debian', 'pacman'), required=True)
    parser.add_argument('--kernel', type=Path, default=ROOT / 'artifacts/kernels/release')
    parser.add_argument('--build', type=Path, default=ROOT / 'build/kernels/release')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=default_jobs())
    parser.add_argument('--plan', action='store_true')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    try:
        plan = package_plan(args.kernel, args.build, args.output, args.format, args.jobs)
        print(json.dumps(plan if args.plan else build_packages(plan), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(2, str(error) + '\n')


if __name__ == '__main__':
    main()
