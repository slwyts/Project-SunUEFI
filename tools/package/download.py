"""Fetch exact repository archives with the target package manager."""
import os

from build.cache import sha
from package.archive import debian_identity


def download_debian(rootfs, download, packages, run, *, source='sources.list.d/sunuefi.sources'):
    """Use APT's archive authentication and compare to the producer's bytes."""
    account = run(['chroot', rootfs, 'getent', 'passwd', '_apt'],
                  capture_output=True, text=True).stdout.split(':')
    os.chown(download, int(account[2]), int(account[3]))
    guest = '/' + download.relative_to(rootfs).as_posix()
    # Fetch from the selected signed source even if another has the same version.
    # Installation still resolves dependencies against the normal source list.
    run(['chroot', rootfs, 'sh', '-ec',
         'cd "$1"; package_source=$2; shift 2; exec apt-get -o "Dir::Etc::sourcelist=$package_source" '
         '-o Dir::Etc::sourceparts=- download "$@"',
         'sunuefi-package-download', guest, source,
         *[row['name'] + '=' + row['version'] for row in packages.values()]])
    expected = {(row['name'], row['version'], row['architecture']): row['sha256']
                for row in packages.values()}
    paths = []
    for archive in sorted(download.glob('*.deb')):
        identity = debian_identity(archive)
        if archive.is_symlink() or sha(archive) != expected.pop(identity, None):
            raise ValueError('Downloaded package differs from the published generation: ' + archive.name)
        paths.append(guest + '/' + archive.name)
    if expected:
        raise ValueError('Published APT source did not return the complete package set')
    return paths


def download_pacman(rootfs, folder, repository, packages, guest, *, install_options=()):
    """Download from the named repository and verify every expected payload."""
    cache = '/' + folder.relative_to(rootfs).as_posix()
    targets = [repository + '/' + row['name'] + '=' + row['version'] for row in packages.values()]
    options = ['--noconfirm', *install_options, '--cachedir', cache, *targets]
    guest(['pacman', '-Sw', *options])
    for name, row in packages.items():
        path = folder / name
        if path.is_symlink() or not path.is_file() or sha(path) != row['sha256']:
            raise ValueError('Downloaded pacman package differs from the published generation: ' + name)
    return options
