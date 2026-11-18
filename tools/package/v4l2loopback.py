"""Build and package the camera loopback module for one sealed Linux ABI."""
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tarfile

from prepare_linux_modules import modinfo

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'upstream/v4l2loopback'
PATCH_SERIES = ROOT / 'patches/v4l2loopback/0.15.4/series.json'
DESCRIPTION = 'Piano camera loopback module for a matching kernel'


def source_pin():
    return json.loads((ROOT / 'sources.lock.json').read_text())['v4l2loopback']


def module_path(release):
    if not re.fullmatch(r'[0-9][a-zA-Z0-9_.+-]{0,63}', release):
        raise ValueError('Invalid kernel release for the camera module')
    return Path('usr/lib/modules') / release / 'updates/v4l2loopback.ko'


def package_name(release):
    return 'linux-piano-v4l2loopback-' + release.lower().replace('_', '-')


def module_info(module, release):
    info = modinfo(module)
    if (info.get('name') != 'v4l2loopback' or
            not info.get('vermagic', '').startswith(release + ' ') or
            not re.fullmatch(r'[0-9]+(?:\.[0-9]+)+', info.get('version', ''))):
        raise ValueError('v4l2loopback module does not match kernel release ' + release)
    return info


def compile_module(build, source, output, release, env, *, strip_debug=False):
    """Apply the common source patches and compile against the selected Kbuild."""
    from build_piano_runtime_helpers import repository, source_patches

    module_path(release)
    pin = source_pin()['commit']
    repository(source, pin)
    output.mkdir()
    names = subprocess.check_output(['git', '-C', source, 'ls-files', '-z'], text=True)
    for name in filter(None, names.split('\0')):
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / name, target)
    for patch in source_patches(PATCH_SERIES):
        subprocess.run(['patch', '--batch', '--forward', '--fuzz=0',
                        '--no-backup-if-mismatch', '-p1', '-i', patch],
                       cwd=output, check=True)
    subprocess.run(['make', '-C', build, 'M=' + str(output), 'ARCH=arm64', 'LLVM=1',
                    'LLVM_IAS=1', 'LOCALVERSION=',
                    f'KCFLAGS=-ffile-prefix-map={output}=/usr/src/v4l2loopback', 'modules'],
                   env=env, check=True)
    module = output / 'v4l2loopback.ko'
    if strip_debug:
        subprocess.run(['llvm-strip', '--strip-debug', module], env=env, check=True)
    repository(source, pin)
    return module, module_info(module, release)


def package_debian(work, output, module, payload, name, version, release, kernel, env):
    tree = work / 'debian'
    destination = tree / payload
    destination.parent.mkdir(parents=True)
    shutil.copyfile(module, destination)
    destination.chmod(0o644)
    copyright_file = tree / 'usr/share/doc' / name / 'copyright'
    copyright_file.parent.mkdir(parents=True)
    pin = source_pin()
    copyright_file.write_text(
        f"Source: {pin['url']} at {pin['commit']}\n\n"
        + (module.parent / 'COPYING').read_text())
    # Match dpkg-gencontrol's per-file rounding and per-directory allowance.
    installed_size = 1 + sum(1 if path.is_dir() else (path.stat().st_size + 1023) // 1024
                             for path in tree.rglob('*'))
    control = tree / 'DEBIAN'
    control.mkdir()
    (control / 'control').write_text(
        f'Package: {name}\nVersion: {version}\nArchitecture: arm64\n'
        'Maintainer: Project SunUEFI <slwyts@tuta.io>\nSection: kernel\nPriority: optional\n'
        f'Installed-Size: {installed_size}\nDepends: {kernel[0]} (= {kernel[1]}), kmod\n'
        f'Description: {DESCRIPTION}\n')
    refresh = f'[ ! -d /lib/modules/{release} ] || depmod -a {release}\n'
    for script, test in (
            ('postinst', 'case "$1" in\n'
             '  configure|abort-upgrade|abort-remove|abort-deconfigure)\n'
             f'    {refresh}    ;;\nesac\n'),
            ('postrm', '[ "$1" = remove ] || exit 0\n'
             'command -v depmod >/dev/null || exit 0\n' + refresh)):
        path = control / script
        path.write_text('#!/bin/sh\nset -e\n' + test)
        path.chmod(0o755)
        subprocess.run(['sh', '-n', path], check=True)
    archive = output / f'{name}_{version}_arm64.deb'
    subprocess.run(['dpkg-deb', '--root-owner-group', '--build', tree, archive],
                   env=env, check=True)
    data = subprocess.check_output(['dpkg-deb', '--fsys-tarfile', archive])
    with tarfile.open(fileobj=io.BytesIO(data)) as package:
        member = package.getmember('./' + payload.as_posix())
        if not member.isfile() or package.extractfile(member).read() != module.read_bytes():
            raise ValueError('Debian package changed the camera module')
    return archive


def package_pacman(work, output, module, payload, name, version, kernel, env):
    from build_piano_runtime_helpers import sha

    recipe = work / 'pacman'
    recipe.mkdir()
    upstream_version, revision = version.rsplit('-', 1)
    license_file = module.parent / 'COPYING'
    shutil.copyfile(module, recipe / module.name)
    shutil.copyfile(license_file, recipe / license_file.name)
    (recipe / 'PKGBUILD').write_text(
        f'pkgname={name}\npkgver={upstream_version}\npkgrel={revision}\narch=(aarch64)\n'
        f'pkgdesc={shlex.quote(DESCRIPTION)}\nurl={shlex.quote(source_pin()["url"])}\n'
        'license=(GPL-2.0-or-later)\noptions=(!strip !debug)\n'
        f"depends=('{kernel[0]}={kernel[1]}' kmod)\n"
        'source=(v4l2loopback.ko COPYING)\n'
        f'sha256sums=({sha(module)} {sha(license_file)})\n'
        'package() {\n'
        f'  install -Dm644 "$srcdir/v4l2loopback.ko" "$pkgdir"/{shlex.quote(payload.as_posix())}\n'
        '  install -Dm644 "$srcdir/COPYING" "$pkgdir/usr/share/licenses/$pkgname/COPYING"\n'
        '}\n')
    config = recipe / 'makepkg.conf'
    config.write_text('. /etc/makepkg.conf\nCARCH=aarch64\n')
    command = ['makepkg', '--noconfirm', '--nodeps', '--config', config]
    if os.geteuid() == 0:
        # The kernel packager already creates this account and owns the output.
        import pwd
        account = pwd.getpwnam('sunuefi-build')
        for path in [recipe, *recipe.iterdir()]:
            shutil.chown(path, account.pw_uid, account.pw_gid)
        command = ['runuser', '-u', account.pw_name, '--', *command]
    subprocess.run(command, cwd=recipe, check=True,
                   env={**env, 'PKGDEST': str(output), 'PKGEXT': '.pkg.tar.zst'})
    archive = output / f'{name}-{version}-aarch64.pkg.tar.zst'
    if subprocess.check_output(['bsdtar', '-xOf', archive, payload]) != module.read_bytes():
        raise ValueError('Pacman package changed the camera module')
    return archive


def build_package(plan, kernel_archive, env):
    """Return the native archive, package name and full version."""
    from build.overlay_packages import debian_identity, pacman_identity
    from build_piano_runtime_helpers import kernel_identity

    release = plan['kernel_release']
    payload = module_path(release)
    suffix = release.lower().replace('_', '-')
    name = package_name(release)
    package_format = plan['format']
    if package_format == 'debian':
        read_identity, image_name, architecture = debian_identity, 'linux-image-' + suffix, 'arm64'
    elif package_format == 'pacman':
        read_identity, image_name, architecture = pacman_identity, 'linux-piano-' + suffix, 'aarch64'
    else:
        raise ValueError('Unknown camera module package format: ' + package_format)
    kernel, kernel_version, actual_arch = read_identity(kernel_archive)
    if ((kernel, actual_arch) != (image_name, architecture) or
            not re.fullmatch(r'[0-9][a-zA-Z0-9_.+~:-]*', kernel_version)):
        raise ValueError('Kernel archive does not match the camera module ABI: ' + release)
    revision = plan['package_epoch']
    if type(revision) is not int or revision <= 0:
        raise ValueError('Set package_epoch to a positive Unix time')
    build, source, output = (Path(plan[key]) for key in ('build', 'source', 'output'))
    commit = plan['source_commit']
    identity = kernel_identity(build, source, commit, release)
    if any(identity['inputs'][str(build / filename)] != plan[key]
           for filename, key in (('.config', 'config_sha256'),
                                 ('Module.symvers', 'module_symvers_sha256'))):
        raise ValueError('Kernel build configuration or symbols differ from the sealed ABI')
    # makepkg records the recipe path in .BUILDINFO. Use the canonical kernel
    # build directory, not a randomly named scratch directory.
    work = build / 'v4l2loopback-package'
    work.mkdir()
    try:
        module, info = compile_module(build, SOURCE, work / 'source', release, env)
        if kernel_identity(build, source, commit, release) != identity:
            raise ValueError('Kernel build inputs changed while compiling the camera module')
        version = f"{info['version']}-{revision}"
        package_env = {**env, 'SOURCE_DATE_EPOCH': str(revision), 'LC_ALL': 'C', 'TZ': 'UTC'}
        image_identity = (kernel, kernel_version)
        if package_format == 'debian':
            version += f".{plan['revision']}"
            archive = package_debian(work, output, module, payload, name, version, release,
                                     image_identity, package_env)
        else:
            archive = package_pacman(work, output, module, payload, name, version,
                                     image_identity, package_env)
        if read_identity(archive) != (name, version, architecture):
            raise ValueError('Camera module package metadata differs from the build')
        return archive, name, version
    finally:
        shutil.rmtree(work)


def verify_installed(root, package_format, release):
    """Check the product retains the installed module and its depmod entry."""
    from build_piano_ram_bootstrap import guest_resolve

    payload = module_path(release)
    name = package_name(release)
    if package_format == 'debian':
        command = ('dpkg-query', '-L', name)
    elif package_format == 'pacman':
        command = ('pacman', '-Qlq', name)
    else:
        raise ValueError('Unknown camera module package format: ' + package_format)
    owned = subprocess.check_output(['chroot', root, *command], text=True).splitlines()
    module = guest_resolve(root, payload.parent.as_posix()) / payload.name
    if '/' + payload.as_posix() not in owned or module.is_symlink() or not module.is_file():
        raise ValueError(f'{name} does not own /{payload.as_posix()} in the product root')
    module_info(module, release)
    index = module.parent.parent / 'modules.dep'
    if not index.is_file() or not any(line.startswith('updates/v4l2loopback.ko:')
                                      for line in index.read_text().splitlines()):
        raise ValueError('depmod did not index the camera module for ' + release)
