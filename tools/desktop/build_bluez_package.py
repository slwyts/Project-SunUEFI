#!/usr/bin/env python3
"""Build Debian's BlueZ package with the uncached HoG identity fix."""
import argparse
from functools import partial
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build_piano_runtime_helpers import source_patches
from desktop import debian_package, package_build

PACKAGE = 'bluez'
RUNTIME = {PACKAGE: 'arm64'}
REVISION = 1


def selection(sysroot, output):
    target = debian_package.target_identity(sysroot, PACKAGE, package_output=output)
    upstream = target['binary']['source_version'].split(':')[-1].rsplit('-', 1)[0]
    series = ROOT / 'patches/bluez' / upstream / 'series.json'
    if not series.is_file():
        raise ValueError('Check the HoG fix against the selected BlueZ source: ' + upstream)
    return target, series, source_patches(series)


def reusable(output, sysroot):
    output = Path(output).resolve()
    target, series, patches = selection(sysroot, output)
    proof = [{'path': path.relative_to(ROOT).as_posix(), 'sha256': debian_package.sha(path)}
             for path in patches]
    inputs = debian_package.input_records([Path(__file__), series, Path(debian_package.__file__),
                                           Path(package_build.__file__)])
    return debian_package.reusable(output, target, proof, RUNTIME, inputs, REVISION)


def test_hog(guest, source, settings, *, jobs):
    # dpkg-buildpackage unapplies quilt patches before returning.
    guest(['env', *package_build.arguments(settings), 'sh', '-ec',
           'cd "$1"; dpkg-source --before-build .; '
           'trap "dpkg-source --after-build ." EXIT; '
           'make -j"$2" unit/test-hog; ./unit/test-hog',
           'hog-tests', source, str(jobs)])


def verify_runtime(folder):
    binary = folder / 'usr/libexec/bluetooth/bluetoothd'
    elf = debian_package.capture(['readelf', '--file-header', binary])
    if (not re.search(r'(?m)^\s*Machine:\s*AArch64\s*$', elf) or
            not (folder / 'usr/lib/systemd/system/bluetooth.service').is_file()):
        raise ValueError('Expected the ARM64 BlueZ daemon and its original service')


def build(output, sysroot, jobs, *, builder_root=None):
    output = Path(output).resolve()
    _, series, patches = selection(sysroot, output)
    with debian_package.use_build_archive(debian_package.archive_inputs(sysroot)):
        return debian_package.build(
            output, sysroot, jobs, PACKAGE, patches, RUNTIME, [Path(__file__), series],
            revision=REVISION, builder_root=builder_root, validate_runtime=verify_runtime,
            test_build=partial(test_hog, jobs=jobs), build_environment=debian_package.gcc_environment)


def main():
    parser = argparse.ArgumentParser(prog='./build.sh bluez', description=__doc__)
    parser.add_argument('--sysroot', type=Path, required=True, help='Debian sid ARM64 target')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/bluez')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--builder-root', type=Path, help='Existing isolated sid ARM64 builder')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        return subprocess.run(['unshare', '--mount', '--propagation', 'private',
                               sys.executable, __file__, *sys.argv[1:], '--worker']).returncode
    print(json.dumps(build(args.output, args.sysroot, args.jobs,
                           builder_root=args.builder_root), indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
