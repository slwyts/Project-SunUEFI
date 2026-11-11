#!/usr/bin/env python3
"""Rebuild the Debian FFmpeg packages with the shared stateful V4L2 patch."""
import argparse
from contextlib import nullcontext
from email.utils import formatdate
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import urllib.request
from host import package_build

import prepare_ffmpeg_source as source

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = {'ffmpeg', 'libavcodec61', 'libavdevice61', 'libavfilter10',
           'libavformat61', 'libavutil59', 'libpostproc58', 'libswresample5', 'libswscale8'}


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(args, **kwargs):
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def reusable(output):
    try:
        record = json.loads((output / 'SOURCE.json').read_text())
        policy = json.loads(source.SERIES.read_text())
        archive = next(row for row in policy['source']['archives'] if row['filename'].endswith('.orig.tar.xz'))
        patches = [{'path': str((source.SERIES.parent / row['path']).relative_to(ROOT)),
                    'sha256': source.sha(source.SERIES.parent / row['path'])} for row in policy['series']]
        version = policy['source']['debian_source_version'] + '+' + policy['local_revision']
        if (not record.get('packages_built') or record.get('patches') != patches or
                record.get('archive_sha256') != archive['sha256'] or
                record.get('package_version') != version or set(record.get('packages', {})) != RUNTIME or
                (output / '.incomplete').exists()):
            return None
        for row in record['packages'].values():
            if (Path(row['file']).name != row['file'] or row['version'] != version or
                    row['architecture'] != 'arm64' or sha(output / 'runtime' / row['file']) != row['sha256']):
                return None
        return record
    except (OSError, ValueError, KeyError, StopIteration):
        return None


def align_gbm_development(log):
    """SDL2 needs the GBM headers matching the runtime already in the builder."""
    installed = subprocess.run(
        ['dpkg-query', '-W', '-f=${db:Status-Status}\t${Version}', 'libgbm1'],
        text=True, capture_output=True)
    if installed.returncode or not installed.stdout.startswith('installed\t'):
        return  # A clean builder can resolve the ordinary stable pair.
    version = installed.stdout.split('\t', 1)[1].strip()
    run(['apt-get', 'install', '-y', '--no-remove', '--no-install-recommends',
         'libgbm-dev=' + version], stdout=log, stderr=subprocess.STDOUT)


def build_isolated(output, sysroot, jobs, resume=False):
    """Build ARM64 Debian packages in a disposable copy, never in the tablet."""
    import build_gsd_packages as debian
    output, sysroot = output.resolve(), sysroot.resolve()
    if cached := reusable(output):
        return cached
    if os.geteuid() or not sysroot.is_relative_to(ROOT / 'build') or not sysroot.is_dir():
        raise ValueError('--sysroot requires root and an existing project build sysroot')
    if not output.is_relative_to(ROOT / 'build') or (output.exists() and not resume):
        raise ValueError('Use a new project build output directory or --resume')
    builder = output.with_name(output.name + '-builder')
    if (builder.exists() and not resume) or builder.is_relative_to(sysroot):
        raise ValueError('Use a fresh isolated builder outside the input sysroot')
    builder.mkdir(exist_ok=resume)
    guest_root = builder / 'rootfs'
    if not guest_root.exists():
        run(['cp', '-a', '--reflink=auto', sysroot, guest_root])
    resolver = guest_root / 'etc/resolv.conf'
    resolver.unlink(missing_ok=True)
    resolver.write_bytes(Path('/etc/resolv.conf').read_bytes())
    workspace = guest_root / 'workspace'
    workspace.mkdir(exist_ok=True)
    # The CLI enters a private mount namespace before taking this branch.
    with debian.devices(guest_root):
        # Runtime roots contain the local +piano Mesa version; dev packages
        # require exact official versions. Align only this disposable clone.
        import build_mutter_packages
        if not resume:
            with (builder / 'dependencies.log').open('a') as log:
                build_mutter_packages.chroot(guest_root, ['apt-get', 'update'],
                                            stdout=log, stderr=subprocess.STDOUT)
                build_mutter_packages.align_development_cohorts(guest_root, log)
        run(['mount', '--bind', ROOT, workspace])
        try:
            with package_build.environment(guest_root) as env:
                run(['chroot', guest_root, '/usr/bin/env',
                     'DEBIAN_FRONTEND=noninteractive', 'SUNUEFI_PACKAGE_ENVIRONMENT=1',
                     *package_build.arguments(env), '/usr/bin/python3',
                     '/workspace/tools/build_ffmpeg_packages.py',
                     '--output', '/workspace/' + str(output.relative_to(ROOT)),
                     '--jobs', str(jobs), *(['--resume'] if resume else ['--install-dependencies'])])
        finally:
            run(['umount', workspace])
    return json.loads((output / 'SOURCE.json').read_text())


def build(output, jobs, install_dependencies=False, resume=False):
    if cached := reusable(output.resolve()):
        return cached
    if platform.machine() not in ('aarch64', 'arm64'):
        raise ValueError('Run in a Debian trixie ARM64 builder; no host or reduced-feature package is substituted')
    release = subprocess.check_output(['dpkg', '--print-architecture'], text=True).strip()
    if release != 'arm64':
        raise ValueError('The Debian builder architecture must be arm64')
    os_release = dict(line.split('=', 1) for line in Path('/etc/os-release').read_text().splitlines()
                      if '=' in line)
    if os_release.get('ID', '').strip('"') != 'debian' or os_release.get('VERSION_CODENAME', '').strip('"') != 'trixie':
        raise ValueError('This pinned Debian recipe targets Debian trixie')
    record = json.loads(source.SERIES.read_text())
    output = output.resolve()
    if not output.is_relative_to(ROOT / 'build') or (output.exists() and not resume):
        raise ValueError('Use a new project build directory or --resume')
    if jobs < 1:
        raise ValueError('Use at least one build job')
    expected_patches = [{'path': str((source.SERIES.parent / row['path']).relative_to(ROOT)),
                         'sha256': source.sha(source.SERIES.parent / row['path'])}
                        for row in record['series']]
    if install_dependencies and os.geteuid():
        raise ValueError('--install-dependencies needs root in the build container')
    output.mkdir(parents=True, exist_ok=resume)
    (output / '.incomplete').write_text('FFmpeg package build has not completed.\n')
    work = output / 'source'
    origin = (json.loads((work / 'SOURCE.json').read_text()) if resume else
              source.prepare(work, apply_patches=False))
    archive = next(row for row in record['source']['archives']
                   if row['filename'].endswith('.debian.tar.xz'))
    cache = ROOT / 'build/source-cache/ffmpeg-7.1.5-source' / archive['filename']
    url = record['source']['dsc_url'].rsplit('/', 1)[0] + '/' + archive['filename']
    if not cache.exists():
        with urllib.request.urlopen(url, timeout=60) as response:
            cache.write_bytes(response.read())
    if cache.stat().st_size != archive['size'] or sha(cache) != archive['sha256']:
        raise ValueError('Debian FFmpeg packaging archive checksum mismatch')
    if not resume:
        with tarfile.open(cache, 'r:xz') as stream:
            stream.extractall(work, filter='data')
    patch_folder = work / 'debian/patches'
    patch_folder.mkdir(exist_ok=True)
    patch_names = []
    for row in record['series']:
        if resume:
            if sha(patch_folder / row['path']) != row['sha256']:
                raise ValueError('Resume requires the same FFmpeg patches')
        else:
            shutil.copy2(source.SERIES.parent / row['path'], patch_folder / row['path'])
        patch_names.append(row['path'])
    series_path = patch_folder / 'series'
    original_series = series_path.read_text() if series_path.exists() else ''
    if not resume:
        series_path.write_text(original_series + ('\n' if original_series and not original_series.endswith('\n') else '') +
                               ''.join(name + '\n' for name in patch_names))
    version = record['source']['debian_source_version'] + '+' + record['local_revision']
    changelog = work / 'debian/changelog'
    if resume:
        if (changelog.read_text().split('(', 1)[1].split(')', 1)[0] != version or
                origin.get('patches') != expected_patches or
                origin.get('archive_sha256') != next(row['sha256'] for row in record['source']['archives']
                                                   if row['filename'].endswith('.orig.tar.xz'))):
            raise ValueError('Resume requires the same FFmpeg source and package version')
    else:
        changelog.write_text('ffmpeg (' + version + ') trixie; urgency=medium\n\n'
                             '  * Follow the stateful V4L2 decoder source-change sequence.\n\n'
                             ' -- Project SunUEFI <sunuefi@users.noreply.github.com>  ' +
                             formatdate(localtime=False) + '\n\n' + changelog.read_text())
    env = os.environ.copy()
    # Keep the original Debian codec/features recipe, including its extra variant.
    # Tests run separately; package compilation retains every codec variant.
    env['DEB_BUILD_OPTIONS'] = 'nocheck parallel=' + str(jobs)
    env.pop('DEB_BUILD_PROFILES', None)
    command = ['dpkg-buildpackage', '-B', '-us', '-uc', '-aarm64', '-j' + str(jobs)]
    if resume:
        command.append('-nc')
    context = (nullcontext({}) if os.environ.get('SUNUEFI_PACKAGE_ENVIRONMENT') else
               package_build.environment(Path('/')))
    with context as settings, (output / 'build.log').open('a') as log:
        env.update(settings)
        if install_dependencies:
            run(['apt-get', 'update'], stdout=log, stderr=subprocess.STDOUT)
            run(['apt-get', 'install', '-y', '--no-install-recommends',
                  'devscripts', 'equivs', 'dpkg-dev', 'build-essential', 'ccache'],
                stdout=log, stderr=subprocess.STDOUT)
            align_gbm_development(log)
            run(['mk-build-deps', '--install', '--remove', '--build-arch=arm64',
                 '--host-arch=arm64', '--arch=arm64',
                 '--tool=apt-get -y --no-remove --no-install-recommends '
                 '-o Debug::pkgProblemResolver=yes', 'debian/control'],
                cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT)
        run(['dpkg-source', '--before-build', '.'], cwd=work, env=env,
            stdout=log, stderr=subprocess.STDOUT)
        run(command, cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT)
    runtime = output / 'runtime'
    runtime.mkdir(exist_ok=True)
    packages = {}
    for path in sorted(output.glob('*.deb')):
        fields = subprocess.check_output(['dpkg-deb', '-f', path, 'Package', 'Version', 'Architecture'], text=True)
        meta = dict(line.split(': ', 1) for line in fields.splitlines())
        if meta['Package'] not in RUNTIME:
            continue
        if meta['Package'] in packages or meta['Version'] != version or meta['Architecture'] != 'arm64':
            raise ValueError('Unexpected FFmpeg runtime package identity: ' + path.name)
        shutil.copy2(path, runtime / path.name)
        packages[meta['Package']] = {'file': path.name, 'version': version,
                                     'architecture': 'arm64', 'sha256': sha(path)}
    if packages.keys() != RUNTIME:
        raise ValueError('The full nine-package FFmpeg runtime set was not produced')
    origin.update({'debian_archive_sha256': archive['sha256'], 'package_version': version,
                   'packages_built': True, 'patches_applied': True,
                   'recipe': 'original Debian debian/rules',
                   'build_command': command, 'build_options': env['DEB_BUILD_OPTIONS'],
                   'status': 'HOST_BUILT_FFMPEG_PACKAGES_NOT_DEVICE_VERIFIED', 'packages': packages})
    (output / 'SOURCE.json').write_text(json.dumps(origin, indent=2) + '\n')
    (output / 'SHA256SUMS').write_text(''.join(row['sha256'] + '  runtime/' + row['file'] + '\n'
                                            for row in packages.values()))
    (output / '.incomplete').unlink()
    return origin


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/ffmpeg')
    parser.add_argument('--jobs', type=int, default=package_build.default_jobs(768))
    parser.add_argument('--resume', action='store_true', help='Continue the same source build without cleaning objects')
    parser.add_argument('--sysroot', type=Path,
                        help='Clone a completed Debian ARM64 sysroot into an isolated package builder')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--install-dependencies', action='store_true',
                        help='Install signed APT build dependencies inside the Debian builder')
    args = parser.parse_args()
    try:
        if args.sysroot and not args.worker:
            import sys
            raise SystemExit(subprocess.run(['unshare', '--mount', '--propagation', 'private',
                                            sys.executable, __file__, *sys.argv[1:], '--worker']).returncode)
        result = (build_isolated(args.output, args.sysroot, args.jobs, args.resume) if args.sysroot else
                  build(args.output, args.jobs, args.install_dependencies, args.resume))
        print(json.dumps(result, indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(2, str(error) + '\n')
