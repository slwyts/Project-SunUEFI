#!/usr/bin/env python3
"""Build Debian sid ARM64 Mutter with the empty KMS Gamma fix."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

import build_gsd_packages as debian
from desktop import debian_package
from desktop import package_build

ROOT = Path(__file__).resolve().parents[1]
PATCHES = [ROOT / 'linux/desktops/gnome/patches/mutter/0001-kms-empty-gamma-bypass.patch']
RUNTIME = {'mutter': 'arm64', 'gir1.2-mutter-51': 'arm64', 'libmutter-51-0': 'arm64',
           'mutter-common': 'all', 'mutter-common-bin': 'arm64'}
REVISION = 1


def patch_records():
    return [{'path': path.relative_to(ROOT).as_posix(), 'sha256': debian.sha(path)} for path in PATCHES]


def selection(rootfs):
    target = debian_package.target_identity(rootfs, 'mutter')
    if target['binary']['source_version'].split('-', 1)[0] != '51.0':
        raise ValueError('The selected Mutter patches and runtime packages require signed sid Mutter 51.0')
    return target


def reusable(output, rootfs):
    inputs = debian_package.input_records([Path(__file__), Path(debian_package.__file__),
                                          Path(package_build.__file__)])
    return debian_package.reusable(Path(output), selection(rootfs), patch_records(), RUNTIME, inputs, REVISION)


def chroot(rootfs, args, **kwargs):
    return debian_package.chroot(rootfs, args, **kwargs)


def align_development_cohorts(rootfs, log, execute=True):
    # Align only the disposable builder when a product carries local libraries.
    installed = {}
    text = debian.capture(['chroot', rootfs, 'dpkg-query', '-W',
                          '-f=${binary:Package}\t${source:Package}\t${Version}\n'])
    for line in text.splitlines():
        package, source, version = line.split('\t')
        installed[package.split(':', 1)[0]] = (package, source, version)
    targets, source_records = {}, []
    for development, anchor in [('libgbm-dev', 'libgbm1'),
                                 ('libpipewire-0.3-dev', 'libpipewire-0.3-0t64'),
                                 ('libspa-0.2-dev', 'libspa-0.2-modules'),
                                 ('libwayland-dev', 'libwayland-client0'),
                                 ('libwayland-bin', 'libwayland-client0')]:
        if anchor not in installed:
            continue
        _, source, version = installed[anchor]
        official_version = re.sub(r'\+piano[0-9]+$', '', version)
        cohort = [(development, official_version)]
        cohort += [(package, official_version) for package, item_source, item_version in installed.values()
                   if item_source == source and item_version != official_version and
                   re.sub(r'\+piano[0-9]+$', '', item_version) == official_version]
        for package, item_version in cohort:
            index = debian.capture(['chroot', rootfs, 'apt-cache', 'show', package + '=' + item_version])
            rows = [row for row in debian.controls(index) if row.get('Version') == item_version and
                    row.get('Package') == package.split(':', 1)[0] and
                    row.get('Source', row.get('Package', '')).split(' ', 1)[0] == source]
            if not rows or any(row.get('SHA256') != rows[0].get('SHA256') for row in rows):
                raise ValueError('Signed APT has no unambiguous matching development cohort: ' + package)
            targets[package] = item_version
            source_records.append({'package': package, 'version': item_version, 'source': source,
                                   'sha256': rows[0]['SHA256'], 'filename': rows[0]['Filename']})
    if not targets:
        return {'targets': [], 'source_records': []}
    args = ['apt-get', 'install', '-y', '--allow-downgrades', '--no-install-recommends']
    args += [package + '=' + version for package, version in sorted(targets.items())]
    command = ['chroot', rootfs, 'env', 'LC_ALL=C', 'DEBIAN_FRONTEND=noninteractive',
               *args[:2], '-s', *args[2:]]
    result = subprocess.run(list(map(str, command)), text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    simulation = result.stdout
    log.write(simulation)
    log.flush()
    result.check_returncode()
    if any(line.startswith('Remv ') for line in simulation.splitlines()):
        raise ValueError('Builder development cohort would remove packages; no changes made')
    if execute:
        chroot(rootfs, args, stdout=log, stderr=subprocess.STDOUT)
    return {'targets': args[5:], 'installed': execute, 'source_records': source_records,
            'simulation_sha256': hashlib.sha256(simulation.encode()).hexdigest()}


def build(output, sysroot, builder_root, jobs, install_dependencies=True):
    selection(sysroot or builder_root)
    return debian_package.build(output, sysroot, jobs, 'mutter', PATCHES, RUNTIME, [Path(__file__)],
                                revision=REVISION, builder_root=builder_root,
                                install_dependencies=install_dependencies)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/mutter')
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--sysroot', type=Path, help='Completed sid ARM64 input; cloned before use')
    mode.add_argument('--builder-root', type=Path, help='Existing isolated sid ARM64 package builder')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--skip-install-dependencies', action='store_true',
                        help='Use an already provisioned sid package builder')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        raise SystemExit(subprocess.run(['unshare', '--mount', '--propagation', 'private',
                                        sys.executable, __file__, *sys.argv[1:], '--worker']).returncode)
    try:
        print(json.dumps(build(args.output, args.sysroot, args.builder_root, args.jobs,
                               not args.skip_install_dependencies), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError, StopIteration) as error:
        parser.exit(2, str(error) + '\n')
