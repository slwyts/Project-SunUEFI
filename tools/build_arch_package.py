#!/usr/bin/env python3
"""Build a project PKGBUILD in the selected ARM64 root, then install its packages."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from build_nyarch_apps import ROOT, guest_environment, run
from build_release_rootfs import build_resolver
from build import overlay_release
from desktop import package_build


def prepared_recipe(path):
    recipe = path.read_bytes()
    if overlay_release.generation() is None:
        return recipe
    return overlay_release.pacman_recipe(recipe.decode()).encode()


def recipe_sha256(path):
    return hashlib.sha256(prepared_recipe(path)).hexdigest()


def matches_release(output):
    try:
        record = json.loads((output / 'manifest.json').read_text())
        return record.get('overlay_release') == overlay_release.generation()
    except (OSError, ValueError):
        return False


def install_cached(root, output, *, install_options=(), selected_archives=None):
    """Install the selected cached archives, or the complete manifest by default."""
    root, output = root.resolve(), output.resolve()
    if os.geteuid() or not root.is_relative_to(ROOT / 'build/distros'):
        raise ValueError('Use root and a project-built ARM64 rootfs')
    record = json.loads((output / 'manifest.json').read_text())
    with tempfile.TemporaryDirectory(prefix='sunuefi-package-install-',
                                     dir=root / 'var/tmp') as folder:
        work = Path(folder)
        packages = []
        archives = record['packages']
        selected = archives if selected_archives is None else selected_archives
        for name, row in selected.items():
            if Path(name).name != name or row != archives.get(name):
                raise ValueError('Invalid cached package: ' + name)
            staged = work / name
            shutil.copy2(output / name, staged)
            if package_build.sha(staged) != row['sha256']:
                raise ValueError('Invalid cached package: ' + name)
            packages.append('/' + str((work / name).relative_to(root)))
        if not packages:
            raise ValueError('The cache contains no binary packages')
        with build_resolver(root), guest_environment(root) as guest:
            guest(['pacman', '-U', '--noconfirm', *install_options, *packages])


def verify_sources(guest, folder):
    for attempt in range(3):
        try:
            guest(['runuser', '-u', 'sunuefi-package', '--', 'bash', '-c',
                   'cd "$1"; makepkg --verifysource --noconfirm --nodeps', 'sources', folder])
            return
        except subprocess.CalledProcessError:
            if attempt == 2:
                raise
            wait = 15 * (attempt + 1)
            print(f'Package source download or verification failed; retrying in {wait}s '
                  f'({attempt + 2}/3).', flush=True)
            time.sleep(wait)


def build(root, recipe, output, resume=False, jobs=None, validate=None, *,
          check=False, install_options=(), install=True):
    root, recipe, output = root.resolve(), recipe.resolve(), output.resolve()
    if os.geteuid() or not root.is_relative_to(ROOT / 'build/distros'):
        raise ValueError('Use root and a project-built ARM64 rootfs')
    if not (recipe / 'PKGBUILD').is_file():
        raise ValueError('The recipe directory must include PKGBUILD and local sources')
    jobs = package_build.default_jobs() if jobs is None else jobs
    if jobs < 1:
        raise ValueError('Use at least one build job')
    folder = '/var/tmp/sunuefi-package-build'
    work = root / folder.lstrip('/')
    pkgbuild = prepared_recipe(recipe / 'PKGBUILD')
    if work.exists() and not resume:
        raise ValueError('A package build already exists; use --resume for the same recipe')
    if work.exists():
        if (work / 'PKGBUILD').read_bytes() != pkgbuild:
            raise ValueError('Resume requires the same package recipe')
    else:
        shutil.copytree(recipe, work, symlinks=True)
        if pkgbuild != (recipe / 'PKGBUILD').read_bytes():
            if (work / 'PKGBUILD').is_symlink():
                (work / 'PKGBUILD').unlink()
            (work / 'PKGBUILD').write_bytes(pkgbuild)
    with build_resolver(root), guest_environment(root) as guest:
        dependencies = guest(
            ['bash', '-c', 'source "$1/PKGBUILD"; printf "%s\\n" "${makedepends[@]}" "${depends[@]}"',
             'dependencies', folder], capture_output=True, text=True).stdout.splitlines()
        dependencies = list(dict.fromkeys(['base-devel', 'ccache', *[p for p in dependencies if p]]))
        # A locally built dependency (for example libssc on SteamOS) need not
        # exist in the base's repositories. Resolve installed versions first.
        missing = subprocess.run(['chroot', root, 'pacman', '-T', *dependencies],
                                 capture_output=True, text=True)
        if missing.returncode not in (0, 127):
            raise RuntimeError(missing.stderr)
        if missing.returncode:
            guest(['pacman', '-S', '--needed', '--noconfirm', *missing.stdout.splitlines()])
        if subprocess.run(['chroot', root, 'id', '-u', 'sunuefi-package'],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
            guest(['useradd', '-m', '-u', '60002', 'sunuefi-package'])
        guest(['chown', '-R', 'sunuefi-package:sunuefi-package', folder])
        if not (work / '.sunuefi-built').exists():
            verify_sources(guest, folder)
            source = work / 'src'
            prepared = source.is_dir() and any(source.iterdir())
            flags = ['--force', '--noconfirm', '--nodeps', '--check' if check else '--nocheck']
            if resume and prepared:
                flags.append('--noextract')
            with package_build.environment(root, user='sunuefi-package') as settings:
                settings['MAKEFLAGS'] = '-j' + str(jobs)
                guest(['runuser', '-u', 'sunuefi-package', '--', 'env',
                       *package_build.arguments(settings), 'bash', '-c',
                       'cd "$1"; shift; exec makepkg "$@"', 'makepkg', folder, *flags])
            (work / '.sunuefi-built').write_text('makepkg completed\n')
        packages = [p for p in work.glob('*.pkg.tar.*') if p.suffix in ('.xz', '.zst', '.gz')]
        if not packages:
            raise ValueError('makepkg produced no binary packages')
        output.mkdir(parents=True, exist_ok=True)
        rows = {}
        for path in packages:
            shutil.copy2(path, output / path.name)
            rows[path.name] = {'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
        if install:
            guest(['pacman', '-U', '--noconfirm', *install_options,
                   *[folder + '/' + p.name for p in packages]])
        if validate is not None:
            validate(root, work, output)
        guest(['userdel', '-r', 'sunuefi-package'])
    (output / 'manifest.json').write_text(json.dumps({
        'package_format': 'pacman', 'architecture': 'aarch64',
        'recipe_sha256': hashlib.sha256(pkgbuild).hexdigest(),
        'overlay_release': overlay_release.generation(),
        'packages': rows}, indent=2) + '\n')
    shutil.rmtree(work)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('rootfs', 'recipe', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--jobs', type=int)
    parser.add_argument('--build-only', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        raise SystemExit(run(['unshare', '--mount', '--propagation', 'private',
                              sys.executable, __file__, *sys.argv[1:], '--worker']).returncode)
    build(args.rootfs, args.recipe, args.output, args.resume, args.jobs,
          install=not args.build_only)
