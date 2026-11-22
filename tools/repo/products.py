#!/usr/bin/env python3
"""Verify and publish native package generations built by product CI."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace

from publish import (KERNEL_META_PACKAGES, KERNEL_META_VERSION, PACMAN_V4L2_LEGACY,
                     REPOSITORIES, ROOT, Store, check_kernel_epoch, pacman_metadata,
                     prepare_pacman_v4l2_withdrawal, prepare_publication, sha, upload_publication)
from retention import History
from verify_public import PublicIndexError, published_kernel_versions, verify_public

sys.path.insert(0, str(ROOT / 'tools'))
from build import denial_manifest, sensors
from build.overlay_packages import (ARCH_REPOSITORIES, PACMAN_CAMERA, bsp_package,
                                    read_debian, read_pacman, topology_package)
from package import camera
from package.archive import debian_identity, pacman_identity
from package.boot import PUBLIC_REPOSITORY_URL


KERNEL_REPOSITORIES = {'debian': 'apt', 'pacman': 'sunuefi'}
PUBLICATION_ORDER = ('apt', 'sunuefi-steamos', 'sunuefi')
OVERLAY_REPOSITORIES = {
    'debian-gnome': 'apt',
    'debian-kde': 'apt',
    'nyarch-gnome': 'sunuefi',
    'steamos': 'sunuefi-steamos',
    **{target: repo for target, (_, repo, _) in denial_manifest.TARGETS.items()},
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


def verify_kernel_abi(directory, public_url=PUBLIC_REPOSITORY_URL):
    """Stop a proven version conflict before building distribution overlays."""
    for package_format, repository in KERNEL_REPOSITORIES.items():
        name = KERNEL_META_PACKAGES[repository]
        archives = packages(directory / package_format, format=package_format)
        if package_format == 'debian':
            candidates = [path for filename, (path, _) in archives.items()
                          if filename.startswith(name + '_')]
            read_identity = debian_identity
        else:
            candidates = [path for filename, (path, _) in archives.items()
                          if filename.partition('.pkg.tar.')[0].rsplit('-', 3)[0] == name]
            read_identity = pacman_identity
        if len(candidates) != 1:
            raise ValueError(f'Expected one native {name} metapackage in {directory / package_format}')
        package, version, _ = read_identity(candidates[0])
        if package != name or not KERNEL_META_VERSION.fullmatch(version):
            raise ValueError(f'Unexpected kernel metapackage: {package} {version}')
        try:
            published = published_kernel_versions(public_url, repository, name)
        except PublicIndexError as error:
            print(f'Could not read the signed {repository} index: {error}. '
                  'The publication job will check the R2 index.', flush=True)
            continue
        check_kernel_epoch(name, version, published)
        print(f'Checked {name} {version} against the signed {repository} index.', flush=True)


def overlay_set(directory, target):
    if target in denial_manifest.TARGETS:
        return denial_manifest.verify_archives(directory, target)
    repo = OVERLAY_REPOSITORIES[target]
    archives = packages(directory, schema_version=1, target=target, repo=repo)
    if repo == 'apt':
        _, entries = read_debian(directory / 'manifest.json', target.removeprefix('debian-'))
        if set(archives) != set(entries):
            raise ValueError('Debian overlay files differ from the package set')
    else:
        _, entries, _ = read_pacman(directory / 'manifest.json', target)
        for name, (path, _) in archives.items():
            row = entries[name]
            if pacman_identity(path) != (row['name'], row['version'], row['architecture']):
                raise ValueError('Pacman overlay archive differs from the package set: ' + name)
    return archives


def check_pacman_camera_coverage(arch, valve):
    """Do not let a SteamOS lookup fall through to an Arch media runtime."""
    def names(record):
        return {row['name'] for row in record['packages'].values() if row['component'] in PACMAN_CAMERA}

    missing = names(arch) - names(valve) - {'libcamera-docs'}
    if missing:
        raise ValueError('SteamOS must supply its own media packages: ' + ', '.join(sorted(missing)))


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
    if arch_targets and arch_targets != ARCH_REPOSITORIES.keys():
        raise ValueError('Publish both Arch overlay sets so SteamOS has matching packages')
    if kernel:
        for package_format, repository in KERNEL_REPOSITORIES.items():
            groups[repository].update(packages(root / 'kernel' / package_format, format=package_format))
            for path, metadata in (bsp_package(root / 'bsp' / package_format, package_format),
                                   topology_package(root / 'bsp', package_format),
                                   bsp_package(root / 'bsp/power-profiles' / package_format,
                                               package_format, component='power-profiles'),
                                   bsp_package(root / 'bsp/keyboard-firmware' / package_format,
                                               package_format, component='keyboard-firmware'),
                                   bsp_package(root / 'bsp/touch-firmware' / package_format,
                                               package_format, component='touch-firmware'),
                                   camera.shared_package(package_format, root / 'camera' / package_format)):
                if path.name in groups[repository]:
                    raise ValueError('Common package duplicated in ' + repository + ': ' + path.name)
                groups[repository][path.name] = (path, metadata['sha256'])
                shared.add(path.name)
        if arch_targets:
            path, metadata = sensors.pacman_package(root / 'bsp/sensors')
            groups['sunuefi'][path.name] = (path, metadata['sha256'])
            shared.add(path.name)
    arch_records = {}
    for target in overlays:
        repository = OVERLAY_REPOSITORIES[target]
        directory = root / 'overlays' / target
        archives = overlay_set(directory, target)
        if target in arch_targets:
            arch_records[target] = json.loads((directory / 'manifest.json').read_text())
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
    if arch_targets:
        check_pacman_camera_coverage(arch_records['nyarch-gnome'], arch_records['steamos'])
    return {repository: dict(groups[repository][name] for name in sorted(groups[repository]))
            for repository in PUBLICATION_ORDER if groups[repository]}


def publish_products(batches, key, output, store, *, publication_id=None):
    """Verify every selected repository before switching any public index."""
    publications = {}
    histories = {}
    started_at = int(time.time())
    for repository in PUBLICATION_ORDER:
        if repository not in batches:
            continue
        packages = batches[repository]
        options = SimpleNamespace(repo=repository, key=key, output=output / repository,
                                  packages=packages)
        digests = {path.name: digest for path, digest in packages.items()}
        if publication_id is not None:
            histories[repository] = History(repository, store, publication_id, started_at, digests)
        publications[repository] = prepare_publication(
            options, store, digests=digests, history=histories.get(repository))

    withdrawal = None
    if 'sunuefi-steamos' in publications and 'sunuefi' in publications:
        valve = publications['sunuefi-steamos']
        names, _ = pacman_metadata(valve.output / 'sunuefi-steamos.db')
        if 'pipewire' in names and PACMAN_V4L2_LEGACY not in names:
            common, _ = pacman_metadata(publications['sunuefi'].output / 'sunuefi.db')
            if PACMAN_V4L2_LEGACY in common:
                raise ValueError('The shared index still offers the old Arch V4L2 plugin to SteamOS')
            # On retry, the withdrawal may be committed but not yet verified
            # at the client URLs. Check it before removing the Valve split.
            withdrawal = prepare_pacman_v4l2_withdrawal(
                output / 'sunuefi-v4l2-withdrawal', key, store,
                check_existing=PACMAN_V4L2_LEGACY in valve.retired)

    def upload_and_check(publication, **fields):
        result = upload_publication(publication, store)
        verify_public(PUBLIC_REPOSITORY_URL, publication.repo, publication.output)
        print(json.dumps({**result, **fields, 'public_indexes_verified': True}, indent=2), flush=True)

    for repository, publication in publications.items():
        if repository == 'sunuefi-steamos' and withdrawal is not None:
            if withdrawal.mutable:
                upload_and_check(withdrawal, withdrawn=PACMAN_V4L2_LEGACY)
            else:
                verify_public(PUBLIC_REPOSITORY_URL, withdrawal.repo, withdrawal.output)
        upload_and_check(publication)

    # A run that stopped partway through publishing cannot age out a verified
    # generation, even if one of its repositories is already visible.
    for repository, history in histories.items():
        history.record(publications[repository].output)
    for repository, history in histories.items():
        removed = history.prune()
        if removed:
            print(f'Removed {removed} expired package and index objects from {repository}.',
                  flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', action='store_true')
    parser.add_argument('--verify-kernel-abi', type=Path, metavar='PACKAGE_DIRECTORY')
    parser.add_argument('--verify-target', choices=OVERLAY_REPOSITORIES)
    parser.add_argument('--verify-bsp', action='store_true')
    parser.add_argument('--overlay-matrix', type=overlay_matrix, default='{"target":[]}')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.verify_kernel_abi:
        if args.kernel or args.overlay_matrix or args.verify_target or args.verify_bsp or args.output:
            parser.error('Use --verify-kernel-abi without other operations')
        verify_kernel_abi(args.verify_kernel_abi)
        return
    if args.output is None:
        parser.error('--output is required')
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
            'keyboard_firmware': bsp_package(args.output / 'keyboard-firmware' / package_format,
                                             package_format, component='keyboard-firmware')[1],
            'touch_firmware': bsp_package(args.output / 'touch-firmware' / package_format,
                                          package_format, component='touch-firmware')[1],
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
            publish_products(batches, keys[0], args.output, Store(),
                             publication_id=os.environ.get('GITHUB_RUN_ID'))
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
