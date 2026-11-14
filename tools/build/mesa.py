#!/usr/bin/env python3
"""Rebuild sid Mesa with the missing Adreno 830v1 device ID."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build import debian
from desktop import package_build

SERIES = ROOT / 'patches/mesa/26.2.6/series.json'
RUNTIME = {'mesa-libgallium', 'libgbm1', 'libegl-mesa0', 'libglx-mesa0',
           'libgl1-mesa-dri', 'mesa-vulkan-drivers'}


def build(output, sysroot, jobs):
    policy = json.loads(SERIES.read_text())
    version = policy['source_version'] + '+' + policy['local_revision']
    patches = [SERIES.parent / name for name in policy['series']]
    output = output.resolve()
    records = [{'path': str(p.relative_to(ROOT)), 'sha256': package_build.sha(p)} for p in patches]
    if cached := debian.reusable(output, version, records, RUNTIME):
        return cached
    with debian.builder(output, sysroot) as (root, env), (output / 'build.log').open('a') as log:
        (output / '.incomplete').write_text('Mesa package build has not completed.\n')
        debian.dependencies(root, log)
        work, origin = debian.source(root, output, 'mesa', policy['source_version'], log)
        debian.add_patches(work, patches, 'mesa', version,
                           'Recognise the Adreno 830v1 chip ID on drm/msm.')
        debian.build(root, env, work, jobs, log, architecture_only=True)
        packages = debian.collect(output, work.parent, RUNTIME, version)
        result = {'source': origin, 'packages_built': True, 'package_version': version,
                  'recipe': 'original Debian debian/rules and control', 'packages': packages,
                  'patches': records, 'producer': str(Path(__file__).relative_to(ROOT)),
                  'producer_sha256': package_build.sha(__file__),
                  'builder_sha256': package_build.sha(debian.__file__)}
        debian.write_manifest(output, result)
        (output / '.incomplete').unlink()
        return result


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, nargs='?', default=ROOT / 'build/mesa')
    parser.add_argument('--sysroot', type=Path, help='Clone a signed sid ARM64 sysroot below build/')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs())
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.sysroot and not args.worker:
        return subprocess.run(['unshare', '--mount', '--propagation', 'private', sys.executable,
                               __file__, *argv, '--worker']).returncode
    try:
        print(json.dumps(build(args.output, args.sysroot, args.jobs), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(2, str(error) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
