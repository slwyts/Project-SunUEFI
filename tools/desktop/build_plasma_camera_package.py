#!/usr/bin/env python3
"""Build Debian's native ARM64 Plasma Camera against the packaged libcamera."""
import argparse
from contextlib import contextmanager
from functools import partial
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build_piano_runtime_helpers import source_patches
from desktop import debian_package, package_build
from desktop.build_pipewire_packages import libcamera_inputs

PACKAGE = 'plasma-camera'
RUNTIME = {PACKAGE: 'arm64'}
REVISION = 1


@contextmanager
def debian_gcc_environment(rootfs):
    # Kirigami's CMake config needs OpenMP. Use Debian's GCC and libgomp.
    with package_build.environment(rootfs) as settings:
        settings['PATH'] = settings['PATH'].removeprefix(package_build.GUEST_TOOLS + '/bin:')
        settings.pop('CCACHE_EXTRAFILES', None)
        settings.pop('MAKE', None)
        yield dict(settings, CC='gcc', CXX='g++', CCACHE_COMPILERTYPE='gcc')


def selection(sysroot, output):
    target = debian_package.target_identity(sysroot, PACKAGE, package_output=output)
    upstream = target['binary']['source_version'].split(':')[-1].rsplit('-', 1)[0]
    series = ROOT / 'patches/plasma-camera' / upstream / 'series.json'
    if not series.is_file():
        raise ValueError('Check the libcamera backport against the selected Plasma Camera source: ' + upstream)
    return target, series, source_patches(series)


def verify_runtime(output, record, library):
    archive = output / 'runtime' / record['packages'][PACKAGE]['file']
    depends = debian_package.capture(['dpkg-deb', '-f', archive, 'Depends'])
    if not any(re.fullmatch(rf'{re.escape(library)}(?: \([^)]+\))?', relation.strip())
               for relation in depends.split(',')):
        raise ValueError('Plasma Camera does not depend on the selected libcamera package')
    with tempfile.TemporaryDirectory(prefix='plasma-camera-runtime-') as directory:
        debian_package.run(['dpkg-deb', '-x', archive, directory])
        binary = Path(directory) / 'usr/bin/plasma-camera'
        elf = debian_package.capture(['readelf', '--file-header', '--dynamic', '--wide', binary])
    abi = library.removeprefix('libcamera')
    soname = f'libcamera.so.{abi}'
    linked = set(re.findall(r'\(NEEDED\).*?\[(libcamera[^]]+)\]', elf))
    if (not re.search(r'(?m)^\s*Machine:\s*AArch64\s*$', elf) or
            soname not in linked or linked - {soname, f'libcamera-base.so.{abi}'} or
            re.search(r'\((?:RPATH|RUNPATH)\)', elf)):
        raise ValueError('Plasma Camera must be ARM64 and link the selected libcamera ABI from the system')


def camera_dependencies(libcamera):
    archives, library = libcamera_inputs(libcamera)
    if not library:
        raise ValueError('Plasma Camera needs a completed Debian libcamera build')
    return archives, library


def reusable(output, sysroot, *, libcamera):
    output = Path(output).resolve()
    target, series, patches = selection(sysroot, output)
    archives, library = camera_dependencies(libcamera)
    proof = [{'path': path.relative_to(ROOT).as_posix(), 'sha256': debian_package.sha(path)}
             for path in patches]
    inputs = debian_package.input_records([Path(__file__), series, Path(debian_package.__file__),
                                           Path(package_build.__file__)])
    return debian_package.reusable(output, target, proof, RUNTIME, inputs, REVISION,
                                   build_dependencies=debian_package.build_dependency_records(archives),
                                   validate_packages=partial(verify_runtime, library=library))


def build(output, sysroot, jobs=None, *, libcamera):
    output, sysroot = Path(output).resolve(), Path(sysroot).resolve()
    _, series, patches = selection(sysroot, output)
    archives, library = camera_dependencies(libcamera)
    jobs = package_build.default_jobs() if jobs is None else jobs
    with debian_package.use_build_archive(debian_package.archive_inputs(sysroot)):
        return debian_package.build(output, sysroot, jobs, PACKAGE, patches, RUNTIME,
                                    [Path(__file__), series], revision=REVISION,
                                    build_dependencies=archives, build_environment=debian_gcc_environment,
                                    validate_packages=partial(verify_runtime, library=library))


def main():
    parser = argparse.ArgumentParser(prog='./build.sh plasma-camera', description=__doc__)
    parser.add_argument('--sysroot', type=Path, required=True, help='Debian sid ARM64 target')
    parser.add_argument('--libcamera-packages', type=Path, required=True, help='Completed Debian libcamera build')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/plasma-camera')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        return subprocess.run(['unshare', '--mount', '--propagation', 'private',
                               sys.executable, __file__, *sys.argv[1:], '--worker']).returncode
    print(json.dumps(build(args.output, args.sysroot, args.jobs, libcamera=args.libcamera_packages), indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
