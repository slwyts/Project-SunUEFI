#!/usr/bin/env python3
"""Build Debian sid ARM64 GSD with the shared GNOME 51 ambient and power fixes."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

from build_piano_runtime_helpers import source_patches
from desktop import debian_package as debian
from desktop import package_build

# Existing package tools import these Debian helpers from this module.
from desktop.debian_package import capture, chroot, checksum_rows, controls, devices, inside, run, sha, verify_source

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'patches/gnome-settings-daemon/0001-power-optional-device-ambient-profile-51.patch'
SERIES = ROOT / 'patches/gnome-settings-daemon/51.0/series.json'
PREPARE = ROOT / 'linux/desktops/gnome/ambient-policy/packaging/prepare_package.py'
POLICY = PREPARE.parent.parent
PATCHES = [PATCH, *source_patches(SERIES)]
RUNTIME = {'gnome-settings-daemon': 'arm64', 'gnome-settings-daemon-common': 'all'}
REVISION = 1


def patch_records():
    return [{'path': path.relative_to(ROOT).as_posix(), 'sha256': sha(path)} for path in PATCHES]


def inputs():
    return [Path(__file__), SERIES, *[POLICY / name for name in
            ('gsd-ambient-profile.c', 'gsd-ambient-profile.h', 'piano.ini')]]


def selection(rootfs):
    target = debian.target_identity(rootfs, 'gnome-settings-daemon')
    if target['binary']['source_version'].split('-', 1)[0] != '51.0':
        raise ValueError('The shared GSD patches require the signed sid GNOME 51.0 source')
    return target


def reusable(output, rootfs):
    target = selection(rootfs)
    records = debian.input_records([Path(debian.__file__), Path(package_build.__file__), *inputs()])
    return debian.reusable(Path(output), target, patch_records(), RUNTIME, records, REVISION)


def validate_prepared(source):
    for name in ('gsd-ambient-profile.c', 'gsd-ambient-profile.h'):
        if (source / 'plugins/power' / name).read_bytes() != (POLICY / name).read_bytes():
            raise ValueError('Prepared ambient policy source differs: ' + name)
    if (source / 'plugins/power/ambient-profiles/piano.ini').read_bytes() != (POLICY / 'piano.ini').read_bytes():
        raise ValueError('Prepared Piano ambient profile differs')


def validate_runtime(folder):
    binary = (folder / 'usr/libexec/gsd-power').read_bytes()
    if binary[:6] != b'\x7fELF\x02\x01' or binary[18:20] != b'\xb7\x00':
        raise ValueError('Actual gsd-power runtime must be little-endian ELF64 AArch64')
    if b'Using ambient-light profile %s for %s' not in binary:
        raise ValueError('Actual gsd-power binary is missing the ambient profile implementation')
    profile = folder / 'usr/share/gnome-settings-daemon/ambient-profiles/piano.ini'
    if profile.read_bytes() != (POLICY / 'piano.ini').read_bytes():
        raise ValueError('Actual runtime package has no matching Piano profile')


def build(output, sysroot, jobs, builder_root=None, install_dependencies=True):
    selection(sysroot or builder_root)
    return debian.build(output, sysroot, jobs, 'gnome-settings-daemon', PATCHES, RUNTIME, inputs(),
                        revision=REVISION, validate=validate_prepared, builder_root=builder_root,
                        validate_runtime=validate_runtime,
                        install_dependencies=install_dependencies)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/gsd')
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
        print(json.dumps(build(args.output, args.sysroot, args.jobs, args.builder_root,
                               not args.skip_install_dependencies), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError, StopIteration) as error:
        parser.exit(2, str(error) + '\n')
