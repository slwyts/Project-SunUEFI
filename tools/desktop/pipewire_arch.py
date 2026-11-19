"""Build the official Arch Linux ARM PipeWire splits with libcamera."""
import fcntl
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

import build_arch_package
from build_piano_ram_bootstrap import guest_resolve
from build_piano_runtime_helpers import source_patches
from build import overlay_packages, overlay_release
from desktop import libcamera_arch, package_build
from prepare_steamos_rootfs import download

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / 'config/pipewire.json'
RECIPE_PATCH = ROOT / 'linux/desktops/common/pipewire/PKGBUILD.patch'
REQUIRED = {'pipewire', 'libpipewire', 'pipewire-libcamera', 'gst-plugin-pipewire'}


def capture(args):
    return subprocess.check_output(args, text=True).strip()


def policy():
    return json.loads(CONFIG.read_text())['arch']


def patch_records(base, selected):
    series = ROOT / 'patches/pipewire' / selected['series'] / 'series.json'
    shared = {path.name: path.resolve() for path in source_patches(series)}
    paths = [shared[name] for name in selected['files']]
    return [*base, *[{'path': path.relative_to(ROOT).as_posix(), 'sha256': package_build.sha(path)}
                     for path in paths]]


def prepare(recipe, data, source, patches, version, inputs, jobs):
    cache = ROOT / 'build/source-cache/pipewire-integration'
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / ('pipewire-' + data['upstream_package_version'].split(':')[-1].split('-')[0]
                       + '.tar.gz')
    recipe.mkdir(parents=True, exist_ok=True)
    with (cache / 'download.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        download(source['url'], archive, source['sha256'])
        for name, digest in data['files'].items():
            cached = cache / (digest + '-' + name)
            download(data['recipe_url'] + '/' + name, cached, digest)
            shutil.copy2(cached, recipe / (name + '.upstream' if name == 'PKGBUILD' else name))
    shutil.copy2(archive, recipe / archive.name)
    patch_names = [Path(row['path']).name for row in patches]
    for row, name in zip(patches, patch_names, strict=True):
        shutil.copy2(ROOT / row['path'], recipe / name)
    sources = [archive.name, *(name for name in data['files'] if name != 'PKGBUILD'), *patch_names]
    text = capture(['patch', '--silent', '--batch', '--fuzz=0', '-o', '-',
                    recipe / 'PKGBUILD.upstream', '-i', RECIPE_PATCH])
    values = {'RELEASE': data['package_release'], 'SPANDSP': data['spandsp']['minimum_version'],
              'LIBCAMERA_VERSION': version, 'JOBS': str(jobs), 'PATCHES': shlex.join(patch_names),
              'SOURCES': shlex.join(sources), 'CHECKSUMS': shlex.join(
                  package_build.sha(recipe / name) for name in sources)}
    for key, value in values.items():
        text = text.replace('@' + key + '@', value)
    # libcamera archives are installed inputs, but a resume must match their bytes too.
    references = '\n'.join(f'# {row["file"]}: {row["sha256"]}' for row in inputs.values())
    (recipe / 'PKGBUILD').write_text(f'{text}\n\n{references}\n')


def verify_packages(runtime, archives, expected, version, audio_soname, camera_abi):
    packages = {}
    camera_dependencies = {f'pipewire={version}', *libcamera_arch.sonames(camera_abi)}
    for filename, row in archives.items():
        fields = overlay_packages.pacman_info(runtime / filename)
        name, = fields.get('pkgname', ('',))
        if (name in packages or name not in expected or fields.get('pkgver') != [version] or
                fields.get('arch') != ['aarch64']):
            raise ValueError('Unexpected native PipeWire package: ' + filename)
        depends = set(fields.get('depend', ()))
        for dependency in depends:
            other, sep, required = dependency.partition('=')
            if sep and other in expected and required != version:
                raise ValueError(f'{name} requires a different PipeWire version: {dependency}')
        if name == 'pipewire-libcamera' and not camera_dependencies <= depends:
            raise ValueError('The camera SPA must require the matching PipeWire and libcamera ABI')
        if name == 'pipewire-audio' and audio_soname not in depends:
            raise ValueError('The Bluetooth audio plugins do not match the ALARM spandsp ABI')
        packages[name] = {'file': filename, 'sha256': row['sha256'],
                          'version': version, 'architecture': 'aarch64'}
    if set(packages) != expected or not REQUIRED <= expected:
        raise ValueError('The build must produce all official PipeWire split packages')
    return packages


def install_names(splits, installed):
    """Upgrade installed splits and add the camera plugin, not optional providers."""
    return sorted(REQUIRED | (splits & installed))


def native_archives(packages):
    return {row['file']: {'sha256': row['sha256']} for row in packages}


def build(root, output, jobs, libcamera, data, source, patches):
    libcamera = Path(libcamera).resolve()
    if output.is_relative_to(libcamera) or libcamera.is_relative_to(output):
        raise ValueError('Use separate outputs for the PipeWire and libcamera builds')
    release = guest_resolve(root, '/etc/os-release').read_text().splitlines()
    distro = next((line[3:].strip('"\'') for line in release if line.startswith('ID=')), '')
    if distro not in ('archarm', 'archlinuxarm'):
        raise ValueError('The pacman libcamera package input requires an Arch Linux ARM build root')
    camera_runtime, camera_version, camera_abi, inputs = libcamera_arch.runtime_inputs(libcamera)
    base = data['upstream_package_version']
    version = base.rsplit('-', 1)[0] + '-' + overlay_release.pacman_release(data['package_release'])
    patches = patch_records(patches, data['libcamera_patches'])
    output.mkdir(parents=True, exist_ok=True)
    with (output / 'build.lock').open('a') as lock, (output / 'build.log').open('a') as log:
        fcntl.flock(lock, fcntl.LOCK_EX)
        recipe = output / 'recipe'
        prepare(recipe, data, source, patches, camera_version, inputs, jobs)
        expected = set(capture(['bash', '-ec', 'source "$1"; printf "%s\\n" "${pkgname[@]}"',
                                'pipewire-recipe', recipe / 'PKGBUILD']).splitlines())
        installed = dict(line.split(maxsplit=1)
                         for line in capture(['chroot', root, 'pacman', '-Q']).splitlines())
        selected = install_names(expected, installed.keys())
        if 'pipewire-v4l2-source' in installed:
            raise ValueError('Build the native PipeWire set in an Arch root without pipewire-v4l2-source')
        for name in selected:
            if name in installed and capture(['chroot', root, 'vercmp', installed[name], version]) == '1':
                raise ValueError(f'The installed {name} is newer than this PipeWire recipe: {installed[name]}')
        (output / 'SOURCE.json').unlink(missing_ok=True)
        build_arch_package.install_cached(root, camera_runtime,
                                          selected_archives=native_archives(inputs.values()))
        runtime = output / 'runtime'
        work = root / 'var/tmp/sunuefi-package-build'
        try:
            subprocess.run(
                [sys.executable, build_arch_package.__file__, '--rootfs', root, '--recipe', recipe,
                 '--output', runtime, '--jobs', str(jobs), '--build-only',
                 *(['--resume'] if work.is_dir() else [])],
                check=True, stdout=log, stderr=subprocess.STDOUT)
        except subprocess.CalledProcessError as exc:
            raise ValueError(f'Arch PipeWire packaging failed; see {log.name}') from exc
        manifest = json.loads((runtime / 'manifest.json').read_text())
        packages = verify_packages(runtime, manifest['packages'], expected, version,
                                   data['spandsp']['soname'], camera_abi)
        build_arch_package.install_cached(root, runtime,
                                          selected_archives=native_archives(packages[name] for name in selected))
        result = dict(line.split(maxsplit=1) for line in
                      capture(['chroot', root, 'pacman', '-Q', *selected]).splitlines())
        if result != dict.fromkeys(selected, version):
            raise ValueError('The installed PipeWire packages do not match the native build')
        record = {'packages_built': True, 'upstream_package_version': base,
                  'package_version': version, 'package_format': 'pacman', 'packages': packages,
                  'arch_recipe': data, 'archive_sha256': source['sha256'], 'patches': patches,
                  'build_dependencies': inputs, 'recipe_sha256': manifest['recipe_sha256'],
                  'scope': 'Native Arch Linux ARM PipeWire splits with libcamera'}
        (output / 'SOURCE.json').write_text(json.dumps(record, indent=2) + '\n')
        return record
