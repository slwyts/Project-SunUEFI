#!/usr/bin/env python3
"""Build the native Debian Qt Multimedia package set for V4L2 recording."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build_piano_runtime_helpers import source_patches
from desktop import debian_package, package_build

SOURCE = 'qt6-multimedia'
LIBRARY = 'libqt6multimedia6'
REVISION = 1
DEBIAN_DEFAULT = frozenset((LIBRARY, 'qml6-module-qtmultimedia'))
# Debian's Architecture: all documentation does not share these version locks.
DEBIAN_PACKAGES = dict.fromkeys((
    LIBRARY, 'libqt6multimediawidgets6', 'libqt6spatialaudio6',
    'qml6-module-qtmultimedia', 'qml6-module-qtquick3d-spatialaudio',
    'qt6-multimedia-dev', 'qt6-multimedia-private-dev', 'qt6-multimedia-examples',
), 'arm64')


def selection(sysroot, output):
    target = debian_package.target_identity(sysroot, LIBRARY, package_output=output)
    upstream = target['binary']['source_version'].split(':')[-1].rsplit('-', 1)[0]
    series = ROOT / 'patches' / SOURCE / upstream / 'series.json'
    if not series.is_file():
        raise ValueError('Check the V4L2 recording patch against Qt Multimedia ' + upstream)
    return target, series, source_patches(series)


def verify_debian_packages(record):
    version, packages = record.get('package_version'), record['packages']
    if (not version or set(packages) != set(DEBIAN_PACKAGES) or any(
            row.get('version') != version or row.get('architecture') != DEBIAN_PACKAGES[name]
            for name, row in packages.items())):
        raise ValueError('Debian Qt Multimedia packages must all have the same version')


def verify_runtime(root):
    plugin = list(root.rglob('libffmpegmediaplugin.so'))
    if len(plugin) != 1:
        raise ValueError('The Debian Qt Multimedia set must include one FFmpeg backend')
    elf = debian_package.capture(['readelf', '--file-header', '--dynamic', '--wide', plugin[0]])
    libraries = ('libQt6Multimedia.so.6', 'libQt6Core.so.6',
                 'libavcodec.so.63', 'libavformat.so.63')
    if (not re.search(r'(?m)^\s*Machine:\s*AArch64\s*$', elf)
            or re.search(r'\((RPATH|RUNPATH)\)', elf)
            or any(f'[{name}]' not in elf for name in libraries)):
        raise ValueError('The Qt Multimedia plugin must use the system ARM64 Qt and FFmpeg ABI')


def reusable(output, sysroot):
    output = Path(output).resolve()
    target, series, patches = selection(sysroot, output)
    proof = [{'path': path.relative_to(ROOT).as_posix(), 'sha256': debian_package.sha(path)}
             for path in patches]
    inputs = debian_package.input_records([
        Path(__file__), series, Path(debian_package.__file__), Path(package_build.__file__),
    ])
    return debian_package.reusable(output, target, proof, DEBIAN_PACKAGES, inputs, REVISION,
                                   validate_packages=lambda _, record: verify_debian_packages(record),
                                   architecture_only=True)


def build(output, sysroot, jobs=None):
    output, sysroot = Path(output).resolve(), Path(sysroot).resolve()
    _, series, patches = selection(sysroot, output)
    jobs = package_build.default_jobs() if jobs is None else jobs
    with debian_package.use_build_archive(debian_package.archive_inputs(sysroot)):
        return debian_package.build(
            output, sysroot, jobs, SOURCE, patches, DEBIAN_PACKAGES,
            [Path(__file__), series], revision=REVISION, binary_package=LIBRARY,
            # Match Debian's C++ symbols and LTO rules with the target GCC.
            build_environment=debian_package.gcc_environment, validate_runtime=verify_runtime,
            validate_packages=lambda _, record: verify_debian_packages(record),
            architecture_only=True,
        )


def main():
    parser = argparse.ArgumentParser(prog='./build.sh qt-multimedia', description=__doc__)
    parser.add_argument('--sysroot', type=Path, required=True, help='Debian sid ARM64 target')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/qt-multimedia')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        return subprocess.run(['unshare', '--mount', '--propagation', 'private',
                               sys.executable, __file__, *sys.argv[1:], '--worker']).returncode
    print(json.dumps(build(args.output, args.sysroot, args.jobs), indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
