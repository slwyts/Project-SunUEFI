#!/usr/bin/env python3
"""Rebuild sid FFmpeg with the shared FFmpeg 9 stateful V4L2 fixes."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys

from build import debian, overlay_release
from desktop import package_build

ROOT = Path(__file__).resolve().parents[1]
SERIES = ROOT / 'patches/ffmpeg/9.0.2/series.json'
POLICY = SERIES.with_name('debian.json')
RUNTIME = {'ffmpeg', 'ffplay', 'libavcodec63', 'libavdevice63', 'libavfilter12',
           'libavformat63', 'libavutil61', 'libswresample7', 'libswscale10'}


def sha(path):
    return package_build.sha(path)


def patch_records():
    series = json.loads(SERIES.read_text())
    result = []
    for row in series['series']:
        patch = (SERIES.parent / row['path']).resolve()
        if sha(patch) != row['sha256']:
            raise ValueError('FFmpeg patch checksum mismatch: ' + row['path'])
        result.append({'path': str(patch.relative_to(ROOT)), 'sha256': row['sha256']})
    return result


def package_version():
    policy = json.loads(POLICY.read_text())
    return overlay_release.debian_version(policy['source_version'] + '+' +
                                          json.loads(SERIES.read_text())['local_revision'])


def reusable(output, target=None):
    record = debian.reusable(Path(output), package_version(), patch_records(), RUNTIME, target)
    return record if record and record.get('policy_sha256') == sha(POLICY) else None


def build_isolated(output, sysroot, jobs, resume=False):
    return build(output, jobs, True, resume, sysroot)


def build(output, jobs, install_dependencies=False, resume=False, sysroot=None):
    output = output.resolve()
    if jobs < 1:
        raise ValueError('Use at least one build job')
    policy = json.loads(POLICY.read_text())
    series = json.loads(SERIES.read_text())
    patches = patch_records()
    version = package_version()
    producer_sha256 = sha(__file__)
    builder_sha256 = sha(debian.__file__)
    target = debian.target_identity(sysroot or Path('/'), {'ffmpeg': policy['source_version']},
                                    {'ffmpeg': 'ffmpeg'})
    if cached := reusable(output, target):
        return cached
    if output.exists() and not resume:
        raise ValueError('Use a new project build output directory or --resume')
    if resume:
        debian.validate_resume(output, target, patches, version)
    with debian.builder(output, sysroot) as (root, env), (output / 'build.log').open('a') as log:
        (output / '.incomplete').write_text('FFmpeg package build has not completed.\n')
        if install_dependencies:
            debian.dependencies(root, log, resume)
        else:
            debian.verify_sid(root)
        target['source_checksums'] = debian.source_checksums(root, target['sources'])
        work, origin = debian.source(root, output, 'ffmpeg', policy['source_version'], log)
        archive = next(row for row in origin['archives'] if row['file'].endswith('.orig.tar.xz'))
        # The Debian orig must match the upstream archive in this patch series.
        shared_archive = (next(row for row in series['source']['archives']
                               if row['filename'].endswith('.orig.tar.xz'))
                          if 'archives' in series['source'] else series['source'])
        if archive['sha256'] != shared_archive['sha256']:
            raise ValueError('Debian FFmpeg orig differs from the shared FFmpeg 9 source')
        debian.add_patches(work, [ROOT / row['path'] for row in patches], 'ffmpeg', version,
                           'Follow the stateful V4L2 source-change and encoder buffer sequence.')
        build_env = dict(env, DEB_BUILD_OPTIONS='nocheck parallel=' + str(jobs), DEB_BUILD_PROFILES='')
        dependencies_satisfied = subprocess.run(
            ['chroot', str(root), 'sh', '-c', 'cd ' +
             shlex.quote(debian.inside(root, work)) + ' && dpkg-checkbuilddeps'],
            stdout=log, stderr=subprocess.STDOUT).returncode == 0
        if install_dependencies and not dependencies_satisfied:
            debian.command(root, build_env, work,
                           ['mk-build-deps', '--install', '--remove',
                             '--tool=apt-get -y --no-remove --no-install-recommends', 'debian/control'], log)
        if env.get('CC') == 'clang':
            libraries = sorted((root / 'usr/lib').glob('llvm-*/lib/libomp.so'))
            if not libraries and install_dependencies:
                # libvidstab uses -fopenmp; link the target, not host, runtime.
                debian.guest(root, ['apt-get', 'install', '-y', '--no-remove',
                                    '--no-install-recommends', 'libomp-dev'],
                             stdout=log, stderr=subprocess.STDOUT)
                libraries = sorted((root / 'usr/lib').glob('llvm-*/lib/libomp.so'))
            if len(libraries) != 1:
                raise ValueError('The Clang builder needs one sid libomp development library')
            build_env['LIBRARY_PATH'] = '/' + str(libraries[0].parent.relative_to(root))
        command = ['dpkg-buildpackage', '-B', '-us', '-uc', '-j' + str(jobs)]
        if resume:
            command.append('-nc')
        changes = work.parent / f'ffmpeg_{version.split(":")[-1]}_arm64.changes'
        identity = {'target': target, 'patches': patches, 'package_version': version,
                    'recipe': origin['recipe']}
        marker = work / 'SOURCE.json'
        previous = json.loads(marker.read_text())
        if previous.get('build_identity') not in (None, identity):
            raise ValueError('Resume requires the same FFmpeg source, patches, recipe and target ABI')
        previous['build_identity'] = identity
        marker.write_text(json.dumps(previous, indent=2) + '\n')
        if not resume or not changes.is_file():
            debian.command(root, build_env, work, command, log)
        packages = debian.collect(output, work.parent, RUNTIME, version)
        if sha(__file__) != producer_sha256 or sha(debian.__file__) != builder_sha256:
            raise ValueError('The FFmpeg build recipe changed during compilation; rerun --resume')
        record = {'component': series['component'], 'upstream_version': series['upstream_version'],
                  'local_revision': series['local_revision'], 'source': origin, 'target': target,
                  'archive_sha256': archive['sha256'], 'patches': patches,
                  'package_version': version, 'packages_built': True, 'patches_applied': True,
                  'recipe': 'original Debian debian/rules and control',
                  'build_command': command, 'build_options': build_env['DEB_BUILD_OPTIONS'],
                  'packages': packages, 'producer': str(Path(__file__).relative_to(ROOT)),
                  'producer_sha256': producer_sha256, 'builder_sha256': builder_sha256,
                  'build_packages': debian.build_packages(root),
                  'runtime_dependencies': debian.runtime_dependencies(output, packages),
                  'compiler_environment_sha256': sha(package_build.__file__),
                  'verifier_sha256': debian.verifier_sha(),
                  'policy_sha256': sha(POLICY)}
        debian.write_manifest(output, record)
        (output / '.incomplete').unlink()
        return record


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/ffmpeg')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs(768))
    parser.add_argument('--resume', action='store_true', help='Continue the same source build without cleaning objects')
    parser.add_argument('--sysroot', type=Path, help='Clone a signed sid ARM64 sysroot into an isolated builder')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--install-dependencies', action='store_true',
                        help='Install signed sid build dependencies inside the Debian builder')
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.sysroot and not args.worker:
        return subprocess.run(['unshare', '--mount', '--propagation', 'private', sys.executable,
                               __file__, *argv, '--worker']).returncode
    try:
        result = (build_isolated(args.output, args.sysroot, args.jobs, args.resume) if args.sysroot else
                  build(args.output, args.jobs, args.install_dependencies, args.resume))
        print(json.dumps(result, indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(2, str(error) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
