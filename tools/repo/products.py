#!/usr/bin/env python3
"""Verify and publish native package generations built by product CI."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace

from publish import REPOSITORIES, ROOT, Store, prepare_publication, sha, upload_publication
from verify_public import verify_public

sys.path.insert(0, str(ROOT / 'tools'))
from build import sensors
from build.overlay_packages import ARCH_REPOSITORIES, bsp_package, read_debian, topology_package
from package import camera
from package.boot import PUBLIC_REPOSITORY_URL


KERNEL_REPOSITORIES = {'debian': 'apt', 'pacman': 'sunuefi'}
OVERLAY_REPOSITORIES = {
    'debian-gnome': 'apt',
    'debian-kde': 'apt',
    'nyarch-gnome': 'sunuefi',
    'steamos': 'sunuefi-steamos',
}


def packages(directory, **identity):
    manifest = directory / 'manifest.json'
    if directory.is_symlink() or manifest.is_symlink() or (directory / '.incomplete').exists():
        raise ValueError('Package generation has not completed: ' + str(directory))
    record = json.loads(manifest.read_text())
    if any(record.get(field) != value for field, value in identity.items()):
        raise ValueError('Package manifest does not match its build target: ' + str(directory))
    paths = {}
    for name, row in record['packages'].items():
        path = directory / name
        if Path(name).name != name or path.is_symlink() or not path.is_file() or sha(path) != row['sha256']:
            raise ValueError('Package manifest differs: ' + name)
        paths[name] = (path, row['sha256'])
    if not paths:
        raise ValueError('Package generation has no packages: ' + str(directory))
    return paths


def overlay_set(directory, target):
    repo = OVERLAY_REPOSITORIES[target]
    archives = packages(directory, schema_version=1, target=target, repo=repo)
    if repo == 'apt':
        _, entries = read_debian(directory / 'manifest.json', target.removeprefix('debian-'))
        if set(archives) != set(entries):
            raise ValueError('Debian overlay files differ from the package set')
    return archives


def overlay_matrix(value):
    matrix = json.loads(value)
    if not isinstance(matrix, dict) or set(matrix) != {'target'}:
        raise ValueError('Expected one overlay target axis')
    targets = matrix['target']
    if not isinstance(targets, list) or any(
            not isinstance(name, str) or name not in OVERLAY_REPOSITORIES for name in targets):
        raise ValueError('Invalid overlay target list')
    if len(set(targets)) != len(targets):
        raise ValueError('Overlay targets must be unique')
    return targets


def publication_batches(root, *, kernel, overlays):
    """Merge shared and distribution packages before updating each signed index."""
    groups = {name: {} for name in REPOSITORIES}
    shared = set()
    arch_targets = set(overlays) & ARCH_REPOSITORIES.keys()
    if kernel and arch_targets and arch_targets != ARCH_REPOSITORIES.keys():
        raise ValueError('Publish both Arch overlay sets with the shared sensor package')
    if kernel:
        for package_format, repository in KERNEL_REPOSITORIES.items():
            groups[repository].update(packages(root / 'kernel' / package_format, format=package_format))
            for path, metadata in (bsp_package(root / 'bsp' / package_format, package_format),
                                   topology_package(root / 'bsp', package_format),
                                   bsp_package(root / 'bsp/power-profiles' / package_format,
                                               package_format, component='power-profiles'),
                                   camera.shared_package(package_format, root / 'camera' / package_format)):
                if path.name in groups[repository]:
                    raise ValueError('Common package duplicated in ' + repository + ': ' + path.name)
                groups[repository][path.name] = (path, metadata['sha256'])
                shared.add(path.name)
        if arch_targets:
            path, metadata = sensors.pacman_package(root / 'bsp/sensors')
            groups['sunuefi'][path.name] = (path, metadata['sha256'])
            shared.add(path.name)
    for target in overlays:
        repository = OVERLAY_REPOSITORIES[target]
        archives = overlay_set(root / 'overlays' / target, target)
        if target in arch_targets:
            provider = [path for name, (path, _) in archives.items()
                        if name.startswith('iio-sensor-proxy-')]
            if len(provider) != 1:
                raise ValueError(f'{target} must supply one native SensorProxy package')
            sensors.check_ssc_provider(provider[0])
        for name, (path, digest) in archives.items():
            if name in shared or (target in ARCH_REPOSITORIES and
                                  name.startswith(sensors.ARCH_PACKAGE + '-')):
                raise ValueError('Common package duplicated in ' + target + ': ' + name)
            old = groups[repository].get(name)
            if old is not None and old[1] != digest:
                raise ValueError('Different builds share a package filename: ' + name)
            groups[repository][name] = (path, digest)
    # Expose the SteamOS provider before the common package that requires it.
    order = ('apt', 'sunuefi-steamos', 'sunuefi') if kernel and arch_targets else REPOSITORIES
    return {repository: dict(groups[repository][name] for name in sorted(groups[repository]))
            for repository in order if groups[repository]}


def publish_products(batches, key, output, store):
    """Verify every selected repository before switching any public index."""
    publications = []
    for repository, packages in batches.items():
        options = SimpleNamespace(repo=repository, key=key, output=output / repository,
                                  packages=packages)
        digests = {path.name: digest for path, digest in packages.items()}
        publications.append(prepare_publication(options, store, digests=digests))
    for publication in publications:
        result = upload_publication(publication, store)
        verify_public(PUBLIC_REPOSITORY_URL, publication.repo, publication.output)
        print(json.dumps({**result, 'public_indexes_verified': True}, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', action='store_true')
    parser.add_argument('--verify-target', choices=OVERLAY_REPOSITORIES)
    parser.add_argument('--verify-bsp', action='store_true')
    parser.add_argument('--overlay-matrix', type=overlay_matrix, default='{"target":[]}')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.verify_bsp:
        if args.kernel or args.overlay_matrix or args.verify_target:
            parser.error('Use --verify-bsp without other operations')
        rows = {package_format: {
            'device_configuration': bsp_package(args.output / package_format, package_format)[1],
            'audio_topology': topology_package(args.output, package_format)[1],
            'power_profiles': bsp_package(args.output / 'power-profiles' / package_format,
                                         package_format, component='power-profiles')[1],
            'display_firmware': bsp_package(args.output / 'display-firmware' / package_format,
                                            package_format, component='display-firmware')[1],
        }
                for package_format in KERNEL_REPOSITORIES}
        rows['pacman']['sensors'] = sensors.pacman_package(args.output / 'sensors')[1]
        print(json.dumps(rows, indent=2))
        return
    if args.verify_target:
        if args.kernel or args.overlay_matrix:
            parser.error('Use --verify-target without publication options')
        paths = overlay_set(args.output, args.verify_target)
        print(f'Verified {len(paths)} packages for {args.verify_target}')
        return
    if not args.kernel and not args.overlay_matrix:
        parser.error('Select --kernel, --overlay-matrix, --verify-target or --verify-bsp')
    batches = publication_batches(ROOT / 'artifacts/packages', kernel=args.kernel,
                                  overlays=args.overlay_matrix)
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
            publish_products(batches, keys[0], args.output, Store())
        finally:
            subprocess.run(['gpgconf', '--kill', 'gpg-agent'], check=False)
            if previous is None:
                os.environ.pop('GNUPGHOME', None)
            else:
                os.environ['GNUPGHOME'] = previous


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
