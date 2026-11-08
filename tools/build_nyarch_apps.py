#!/usr/bin/env python3
"""Build current Nyarch application sources as native pacman packages.

Run after the Arch Linux ARM desktop assembly. Sources are resolved once per
invocation, recorded, and pinned in the generated PKGBUILDs. No ISO is used.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import urllib.request

from build_chroot import remove_pacman_sockets, stop_helpers

ROOT = Path(__file__).resolve().parents[1]
APPS = ('NyarchCustomize', 'NyarchTour', 'NyarchWizard', 'NyarchScript')
PACKAGES = (
    'base-devel', 'git', 'meson', 'python', 'python-gobject', 'gtk3', 'gtk4',
    'libadwaita', 'dconf', 'gdk-pixbuf2', 'pango', 'hicolor-icon-theme',
    'chromium', 'noto-fonts-cjk', 'ibus', 'ibus-libpinyin', 'bluez', 'bluez-utils',
    'alsa-utils', 'alsa-ucm-conf', 'pipewire-audio', 'pipewire-pulse',
    'gst-plugin-pipewire', 'gst-plugins-good', 'gst-plugins-bad', 'ffmpeg',
    'v4l-utils', 'python-pillow', 'python-numpy', 'zram-generator',
    'gnome-console', 'gnome-text-editor', 'gnome-tweaks', 'gnome-keyring', 'snapshot',
    'vulkan-freedreno', 'mesa-utils', 'vulkan-tools', 'polkit', 'busybox',
    'gnome-calculator', 'gnome-calendar', 'gnome-clocks', 'gnome-weather',
    'gnome-maps', 'gnome-software', 'gnome-system-monitor', 'loupe', 'papers',
    'file-roller', 'mpv', 'fragments', 'lollypop', 'geary', 'kitty', 'flatpak',
    'flatseal', 'extension-manager', 'shortwave', 'komikku', 'fastfetch',
    'nyancat', 'webapp-manager', 'pacman-contrib',
)


def run(args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def revision(url):
    return subprocess.check_output(['git', 'ls-remote', url, 'HEAD'], text=True).split()[0]


def prepare_sources(output):
    output.mkdir(parents=True)
    repo = 'https://github.com/NyarchLinux/nyarch-repo.git'
    pin = revision(repo)
    records = []
    for app in APPS:
        folder = output / app
        folder.mkdir()
        url = 'https://github.com/NyarchLinux/' + app + '.git'
        run(['git', 'clone', '--depth', '1', url, folder / 'source'])
        commit = subprocess.check_output(['git', '-C', folder / 'source',
                                          'rev-parse', 'HEAD'], text=True).strip()
        address = ('https://raw.githubusercontent.com/NyarchLinux/nyarch-repo/' +
                   pin + '/PkgBuilds/' + app + '/PKGBUILD')
        with urllib.request.urlopen(address, timeout=30) as response:
            original = response.read().decode()
        if "arch=('any')" not in original or 'source=("git+${url}.git")' not in original:
            raise ValueError('Nyarch package recipe changed: ' + app)
        (folder / 'PKGBUILD.upstream').write_text(original)
        (folder / 'PKGBUILD').write_text(original.replace(
            'source=("git+${url}.git")', 'source=("git+${url}.git#commit=' + commit + '")'))
        records.append({'name': app, 'recipe_commit': pin, 'source_commit': commit,
                        'recipe_sha256': hashlib.sha256(original.encode()).hexdigest()})
    (output / 'SOURCE.json').write_text(json.dumps(records, indent=2) + '\n')


@contextmanager
def guest_environment(root):
    """Temporary build mounts; keep pacman's runtime security defaults intact."""
    mounted = []
    try:
        for name, kind in (('proc', 'proc'), ('dev', 'tmpfs'), ('run', 'tmpfs')):
            path = root / name
            path.mkdir(exist_ok=True)
            run(['mount', '-t', kind, kind, path])
            mounted.append(path)
        for name, major, minor in (('null', 1, 3), ('zero', 1, 5),
                                    ('random', 1, 8), ('urandom', 1, 9)):
            run(['mknod', '-m', '666', root / 'dev' / name, 'c', major, minor])
        (root / 'dev/fd').symlink_to('/proc/self/fd')
        for number, name in enumerate(('stdin', 'stdout', 'stderr')):
            (root / 'dev' / name).symlink_to('/proc/self/fd/' + str(number))
        text = (root / 'etc/pacman.conf').read_text()
        (root / 'run/pacman-build.conf').write_text(
            re.sub(r'^\s*CheckSpace\s*$', '# CheckSpace (build chroot)', text, flags=re.M))
        help_text = subprocess.check_output(
            ['chroot', root, 'pacman', '-S', '--help'], text=True)
        sandbox_args = ['--disable-sandbox'] if '--disable-sandbox' in help_text else []
        def guest(args, **kwargs):
            if args[0] == 'pacman':
                # The build may run under QEMU or an older device kernel
                # without Landlock. Do not change the installed policy.
                args = [args[0], '--config', '/run/pacman-build.conf',
                        *sandbox_args, *args[1:]]
            # Package hooks can daemonize, change root or retain an fd into
            # /dev. Give each command its own process namespace so those
            # descendants cannot outlive the command and pin the build mounts.
            # systemd must still treat PID 1 in this namespace as a build chroot.
            return run(['unshare', '--pid', '--fork', '--kill-child',
                        '--mount-proc=' + str(root / 'proc'),
                        'chroot', root, '/usr/bin/env',
                        'SYSTEMD_OFFLINE=1', 'SYSTEMD_IN_CHROOT=1',
                        'PATH=/usr/bin:/usr/sbin:/bin:/sbin', *args], **kwargs)

        yield guest
    finally:
        subprocess.run(['chroot', root, 'gpgconf', '--homedir',
                        '/etc/pacman.d/gnupg', '--kill', 'all'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        stop_helpers(root)
        # PID namespace teardown may SIGKILL a daemon before it unlinks its
        # sockets. The files can also be inherited from Valve's base image.
        remove_pacman_sockets(root)
        for path in reversed(mounted):
            # Package hooks may mount devpts/shm below the temporary /dev.
            # Remove children first; do not lazily detach a busy build tree.
            try:
                run(['umount', '--recursive', path])
            except subprocess.CalledProcessError:
                subprocess.run(['findmnt', '--submounts', '--target', path],
                               check=False)
                raise


def build(root, sources, output, resume=False):
    root = root.resolve()
    if os.geteuid() or not root.is_relative_to(ROOT / 'build/distros'):
        raise ValueError('Use root and a project-built ARM64 rootfs')
    if not (root.parent / 'manifest.json').is_file():
        raise ValueError('Complete the Arch desktop base first')
    rows = json.loads((sources / 'SOURCE.json').read_text())
    if {row['name'] for row in rows} != set(APPS):
        raise ValueError('Unexpected Nyarch source set')
    output.mkdir(parents=True, exist_ok=True)
    work = root / 'var/tmp/sunuefi-nyarch-build'
    if work.exists() and not resume:
        raise ValueError('Previous application build remains; use a fresh build directory')
    work.mkdir(exist_ok=resume)
    # Use the package sources selected above, without another moving branch fetch.
    for row in rows:
        folder = work / row['name']
        if folder.exists():
            if (folder / 'PKGBUILD.upstream').read_bytes() != (sources / row['name'] / 'PKGBUILD.upstream').read_bytes():
                raise ValueError('Cannot resume with a different package recipe')
            actual = subprocess.check_output(
                ['git', '-c', 'safe.directory=*', '-C', folder / 'source', 'rev-parse', 'HEAD'],
                text=True).strip()
            if actual != row['source_commit']:
                raise ValueError('Cannot resume with a different application source')
        else:
            shutil.copytree(sources / row['name'], folder, symlinks=True)
        pkg = folder / 'PKGBUILD'
        pkg.write_text(pkg.read_text().replace(
            'git+${url}.git#commit=', row['name'] + '::git+file:///var/tmp/sunuefi-nyarch-build/' +
            row['name'] + '/source#commit='))
    with guest_environment(root) as guest:
        guest(['pacman', '-Syu', '--needed', '--noconfirm', *PACKAGES])
        if subprocess.run(['chroot', root, 'id', '-u', 'sunuefi-build'],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
            guest(['useradd', '-m', '-u', '60001', 'sunuefi-build'])
        guest(['chown', '-R', 'sunuefi-build:sunuefi-build', '/var/tmp/sunuefi-nyarch-build'])
        packages = []
        for row in rows:
            folder = '/var/tmp/sunuefi-nyarch-build/' + row['name']
            built = [p for p in (work / row['name']).glob('*.pkg.tar.*')
                     if p.suffix in ('.zst', '.xz', '.gz')]
            if not built:
                guest(['runuser', '-u', 'sunuefi-build', '--', 'env', 'CARCH=aarch64',
                       'bash', '-c', 'cd "$1"; makepkg --noconfirm --nocheck --nodeps', 'makepkg', folder])
                built = [p for p in (work / row['name']).glob('*.pkg.tar.*')
                         if p.suffix in ('.zst', '.xz', '.gz')]
            if len(built) != 1:
                raise ValueError('Expected one package for ' + row['name'])
            target = output / built[0].name
            shutil.copy2(built[0], target)
            packages.append('/var/tmp/sunuefi-nyarch-build/' + row['name'] + '/' + target.name)
            row['package'] = target.name
            row['sha256'] = hashlib.sha256(target.read_bytes()).hexdigest()
        guest(['pacman', '-U', '--noconfirm', *packages])
        guest(['userdel', '-r', 'sunuefi-build'])
    shutil.rmtree(work)
    (output / 'SOURCE.json').write_text(json.dumps(rows, indent=2) + '\n')
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sources', type=Path, required=True)
    parser.add_argument('--prepare-sources', action='store_true')
    parser.add_argument('--rootfs', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.prepare_sources:
        prepare_sources(args.sources)
        return
    if not args.rootfs or not args.output:
        parser.error('--rootfs and --output are required for packaging')
    if not args.worker:
        raise SystemExit(run(['unshare', '--mount', '--propagation', 'private',
                              sys.executable, __file__, *sys.argv[1:], '--worker']).returncode)
    build(args.rootfs, args.sources, args.output, args.resume)


if __name__ == '__main__':
    main()
