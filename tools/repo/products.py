#!/usr/bin/env python3
"""Publish validated product packages using an isolated CI signing key."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from publish import REPOSITORIES, ROOT, Store, publish, sha


KERNEL_REPOSITORIES = {'debian': 'apt', 'pacman': 'sunuefi'}
OVERLAY_REPOSITORIES = {
    'debian-gnome': 'apt',
    'debian-kde': 'apt',
    'nyarch-gnome': 'sunuefi',
    'steamos': 'sunuefi-steamos',
}


def stage_target(target, output):
    """Collect only successful native package outputs, never builder caches."""
    import shutil
    repository = OVERLAY_REPOSITORIES[target]
    if repository == 'apt':
        patterns = [
            'build/' + name + '/runtime/*.deb' for name in
            ('mesa', 'ffmpeg', 'sensors', 'gsd', 'mutter', 'gdm', 'pipewire',
             'libwacom', 'fingerprint-packages')]
    else:
        patterns = [f'build/distros/{target}-piano/packages/*/*.pkg.tar.*',
                    f'build/mesa-pacman/{target}/*.pkg.tar.*']
    output.mkdir(parents=True, exist_ok=False)
    rows = {}
    for pattern in patterns:
        for path in sorted(ROOT.glob(pattern)):
            if path.suffix not in ('.deb', '.zst', '.xz', '.gz'):
                continue
            if (path.parent / '.incomplete').exists() or (path.parent.parent / '.incomplete').exists():
                raise ValueError('Incomplete native package output: ' + str(path))
            digest = sha(path)
            if path.name in rows and rows[path.name]['sha256'] != digest:
                raise ValueError('Different builds share a package filename: ' + path.name)
            shutil.copy2(path, output / path.name)
            rows[path.name] = {'sha256': digest}
    if not rows:
        raise ValueError('No completed native packages for ' + target)
    (output / 'manifest.json').write_text(json.dumps(
        {'repo': repository, 'packages': rows}, indent=2) + '\n')


def packages(directory, field, value):
    record = json.loads((directory / 'manifest.json').read_text())
    if record.get(field) != value:
        raise ValueError('Package manifest does not match its build target: ' + str(directory))
    paths = {}
    for name, row in record['packages'].items():
        path = directory / name
        if Path(name).name != name or not path.is_file() or sha(path) != row['sha256']:
            raise ValueError('Package manifest differs: ' + name)
        paths[name] = (path, row['sha256'])
    if not paths or (directory / '.incomplete').exists():
        raise ValueError('Package generation has not completed')
    return paths


def publication_batches(root, *, kernel, overlays):
    """Merge shared and distribution packages before updating each signed index."""
    groups = {name: {} for name in REPOSITORIES}
    if kernel:
        for package_format, repository in KERNEL_REPOSITORIES.items():
            groups[repository].update(packages(root / 'kernel' / package_format,
                                               'format', package_format))
    if overlays:
        directories = sorted(path for path in (root / 'overlays').glob('*') if path.is_dir())
        if not directories:
            raise ValueError('No validated distribution package generations were restored')
        for directory in directories:
            if directory.name not in OVERLAY_REPOSITORIES:
                raise ValueError('Unknown distribution package target: ' + directory.name)
            repository = OVERLAY_REPOSITORIES[directory.name]
            for name, (path, digest) in packages(directory, 'repo', repository).items():
                old = groups[repository].get(name)
                if old is not None and old[1] != digest:
                    raise ValueError('Different builds share a package filename: ' + name)
                groups[repository][name] = (path, digest)
    return {repository: [group[name][0] for name in sorted(group)]
            for repository, group in groups.items() if group}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', action='store_true')
    parser.add_argument('--stage-target', choices=OVERLAY_REPOSITORIES)
    parser.add_argument('--overlays', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.stage_target:
        stage_target(args.stage_target, args.output)
        return
    if not args.kernel and not args.overlays:
        parser.error('Select --kernel, --overlays or --stage-target')
    batches = publication_batches(ROOT / 'artifacts/packages', kernel=args.kernel, overlays=args.overlays)
    from types import SimpleNamespace
    with tempfile.TemporaryDirectory(prefix='sunuefi-signing-') as folder:
        previous = os.environ.get('GNUPGHOME')
        os.environ['GNUPGHOME'] = folder
        secret = os.environ.pop('SUNUEFI_REPO_GPG_KEY')
        try:
            subprocess.run(['gpg', '--batch', '--import'], input=secret.encode(), check=True)
            record = subprocess.check_output(['gpg', '--batch', '--with-colons',
                                              '--list-secret-keys'], text=True)
            keys = [line.split(':')[9] for line in record.splitlines() if line.startswith('fpr:')]
            if not keys:
                raise ValueError('No signing subkey was imported')
            store = Store()
            for repository, paths in batches.items():
                options = SimpleNamespace(
                    repo=repository, key=keys[0], output=args.output / repository, packages=paths)
                print(json.dumps(publish(options, store), indent=2))
        finally:
            subprocess.run(['gpgconf', '--kill', 'gpg-agent'], check=False)
            if previous is None:
                os.environ.pop('GNUPGHOME', None)
            else:
                os.environ['GNUPGHOME'] = previous


if __name__ == '__main__':
    main()
