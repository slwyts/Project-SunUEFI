#!/usr/bin/env python3
"""Build Debian KWin with the upstream HDR brightness restoration fix."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build_piano_runtime_helpers import source_patches
from desktop import debian_package, package_build

RUNTIME = {
    'kwin-common': 'arm64',
    'kwin-data': 'all',
    'kwin-dev': 'arm64',
    'kwin-wayland': 'arm64',
    'libkwin6': 'arm64',
}
DEFAULT = RUNTIME.keys() - {'kwin-dev'}
QT_ABI = ('libqt6core6t64', 'libqt6qml6', 'libqt6svg6')
REVISION = 1
FIXED_UPSTREAM = '6.8.4'  # Plasma/6.8 backport in KWin !10407.


def selection(sysroot):
    target = debian_package.target_identity(sysroot, 'kwin-wayland', extra_abi=QT_ABI)
    version = target['binary']['source_version'].split(':')[-1].rsplit('-', 1)[0]
    if subprocess.run(['dpkg', '--compare-versions', version, 'ge', FIXED_UPSTREAM]).returncode == 0:
        return target, None, ()
    # Plasma 6.8 already links the two settings modules to Qt::Qml.
    series_version = '6.6.5' if version in ('6.8.0', '6.8.1', '6.8.2', '6.8.3') else version
    series = ROOT / 'patches/kwin' / series_version / 'series.json'
    if not series.is_file():
        raise ValueError('Check the KWin patches against the selected source: ' + version)
    return target, series, tuple(path.resolve() for path in source_patches(series))


def reusable(output, target, series, patches):
    patch_records = [{'path': path.relative_to(ROOT).as_posix(),
                      'sha256': debian_package.sha(path)} for path in patches]
    inputs = debian_package.input_records([Path(__file__), series, Path(debian_package.__file__),
                                           Path(package_build.__file__)])
    return debian_package.reusable(output, target, patch_records, RUNTIME, inputs, REVISION,
                                   keep_binary_revision=True)


def build(output, sysroot, jobs, *, builder_root=None, install_dependencies=True):
    _, series, patches = selection(sysroot)
    if series is None:
        return None
    with debian_package.use_build_archive(debian_package.archive_inputs(sysroot)):
        return debian_package.build(
            output, sysroot, jobs, 'kwin', patches, RUNTIME,
            [Path(__file__), series], revision=REVISION, binary_package='kwin-wayland',
            builder_root=builder_root, install_dependencies=install_dependencies,
            extra_abi=QT_ABI, keep_binary_revision=True,
        )


def main():
    parser = argparse.ArgumentParser(prog='./build.sh kwin', description=__doc__)
    parser.add_argument('--sysroot', type=Path, required=True, help='Debian sid ARM64 target')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/kwin')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs(memory_mib=1536))
    parser.add_argument('--builder-root', type=Path, help='Existing isolated sid ARM64 builder')
    parser.add_argument('--skip-install-dependencies', action='store_true',
                        help='Use build dependencies already installed in the isolated builder')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        return subprocess.run(['unshare', '--mount', '--propagation', 'private',
                               sys.executable, __file__, *sys.argv[1:], '--worker']).returncode
    print(json.dumps(build(args.output, args.sysroot, args.jobs,
                           builder_root=args.builder_root,
                           install_dependencies=not args.skip_install_dependencies), indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
