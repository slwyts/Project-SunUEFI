#!/usr/bin/env python3
"""Rebuild the Debian FFmpeg packages with the shared stateful V4L2 patch."""
import argparse
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

import prepare_ffmpeg_source as source

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = {'ffmpeg', 'libavcodec61', 'libavdevice61', 'libavfilter10',
           'libavformat61', 'libavutil59', 'libpostproc58', 'libswresample5', 'libswscale8'}


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(args, **kwargs):
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def build(output, jobs, install_dependencies=False):
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
    if not output.is_relative_to(ROOT / 'build') or output.exists():
        raise ValueError('Use a new project build directory')
    if not 1 <= jobs <= 8:
        raise ValueError('Use 1 to 8 build jobs')
    if install_dependencies and os.geteuid():
        raise ValueError('--install-dependencies needs root in the build container')
    output.mkdir(parents=True)
    (output / '.incomplete').write_text('FFmpeg package build has not completed.\n')
    work = output / 'source'
    origin = source.prepare(work, apply_patches=False)
    archive = next(row for row in record['source']['archives']
                   if row['filename'].endswith('.debian.tar.xz'))
    cache = ROOT / 'build/source-cache/ffmpeg-7.1.5-source' / archive['filename']
    url = record['source']['dsc_url'].rsplit('/', 1)[0] + '/' + archive['filename']
    if not cache.exists():
        with urllib.request.urlopen(url, timeout=60) as response:
            cache.write_bytes(response.read())
    if cache.stat().st_size != archive['size'] or sha(cache) != archive['sha256']:
        raise ValueError('Debian FFmpeg packaging archive checksum mismatch')
    with tarfile.open(cache, 'r:xz') as stream:
        stream.extractall(work, filter='data')
    patch_folder = work / 'debian/patches'
    patch_folder.mkdir(exist_ok=True)
    patch_names = []
    for row in record['series']:
        shutil.copy2(source.SERIES.parent / row['path'], patch_folder / row['path'])
        patch_names.append(row['path'])
    series_path = patch_folder / 'series'
    original_series = series_path.read_text() if series_path.exists() else ''
    series_path.write_text(original_series + ('\n' if original_series and not original_series.endswith('\n') else '') +
                           ''.join(name + '\n' for name in patch_names))
    version = record['source']['debian_source_version'] + '+' + record['local_revision']
    changelog = work / 'debian/changelog'
    changelog.write_text('ffmpeg (' + version + ') trixie; urgency=medium\n\n'
                         '  * Follow the stateful V4L2 decoder source-change sequence.\n\n'
                         ' -- Project SunUEFI <sunuefi@users.noreply.github.com>  ' +
                         formatdate(usegmt=True) + '\n\n' + changelog.read_text())
    env = os.environ.copy()
    # Keep the original Debian codec/features recipe, including its extra variant.
    # Only the extensive upstream test suite is omitted for this candidate build.
    env['DEB_BUILD_OPTIONS'] = 'nocheck parallel=' + str(jobs)
    env.pop('DEB_BUILD_PROFILES', None)
    command = ['dpkg-buildpackage', '-B', '-us', '-uc', '-aarm64', '-j' + str(jobs)]
    with (output / 'build.log').open('w') as log:
        if install_dependencies:
            run(['apt-get', 'update'], stdout=log, stderr=subprocess.STDOUT)
            run(['apt-get', 'install', '-y', '--no-install-recommends',
                 'devscripts', 'equivs', 'dpkg-dev', 'build-essential'],
                stdout=log, stderr=subprocess.STDOUT)
            run(['mk-build-deps', '--install', '--remove', '--build-arch=arm64',
                 '--host-arch=arm64', '--arch=arm64',
                 '--tool=apt-get -y --no-install-recommends', 'debian/control'],
                cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT)
        run(['dpkg-source', '--before-build', '.'], cwd=work, env=env,
            stdout=log, stderr=subprocess.STDOUT)
        run(command, cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT)
    runtime = output / 'runtime'
    runtime.mkdir()
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
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--install-dependencies', action='store_true',
                        help='Install signed APT build dependencies inside the Debian builder')
    args = parser.parse_args()
    try:
        print(json.dumps(build(args.output, args.jobs, args.install_dependencies), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(2, str(error) + '\n')
