"""Build native PipeWire splits for Arch Linux ARM and SteamOS."""
import fcntl
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

import build_arch_package
from build_piano_runtime_helpers import source_patches
from build import overlay_packages, overlay_release
from desktop import libcamera_arch, package_build
from prepare_steamos_rootfs import download

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / 'config/pipewire.json'
RECIPE_DIR = ROOT / 'linux/desktops/common/pipewire'
REQUIRED = {'pipewire', 'libpipewire', 'pipewire-libcamera', 'gst-plugin-pipewire'}
V4L2_PACKAGE = 'pipewire-v4l2-source'
V4L2_OWNER = {'arch': 'libpipewire', 'steamos': 'pipewire'}
V4L2_MODULE = 'usr/lib/spa-0.2/v4l2/libspa-v4l2.so'
VALVE_SPA_LIBRARY = 'usr/lib/spa-0.2/libspa.so'


def capture(args):
    return subprocess.check_output(args, text=True).strip()


def policy(target='arch'):
    return json.loads(CONFIG.read_text())[target]


def v4l2_replacement(data):
    since = data.get('v4l2_source_replaces_since')
    return f'{V4L2_PACKAGE}>={since}' if since else V4L2_PACKAGE


def patch_records(base, selected):
    series = ROOT / 'patches/pipewire' / selected['series'] / 'series.json'
    shared = {path.name: path.resolve() for path in source_patches(series)}
    paths = [shared[name] for name in selected['files']]
    return [*base, *[{'path': path.relative_to(ROOT).as_posix(), 'sha256': package_build.sha(path)}
                     for path in paths]]


def recipe_inputs(cache, recipe, data):
    if upstream := data.get('recipe_archive'):
        archive = cache / (upstream['sha256'] + '.src.tar.gz')
        download(upstream['url'], archive, upstream['sha256'])
    for name, digest in data['files'].items():
        destination = recipe / (name + '.upstream' if name == 'PKGBUILD' else name)
        if upstream:
            content = subprocess.check_output(['bsdtar', '-xOf', archive, f'pipewire/{name}'])
            destination.write_bytes(content)
            if package_build.sha(destination) != digest:
                raise ValueError('Valve PipeWire source file differs from the recipe: ' + name)
        else:
            cached = cache / (digest + '-' + name)
            download(data['recipe_url'] + '/' + name, cached, digest)
            shutil.copy2(cached, destination)


def prepare(recipe, data, source, patches, version, inputs, jobs, *, target='arch'):
    cache = ROOT / 'build/source-cache/pipewire-integration'
    cache.mkdir(parents=True, exist_ok=True)
    upstream = data['upstream_package_version'].split(':')[-1].rsplit('-', 1)[0]
    archive = cache / f'pipewire-{upstream}.tar.gz'
    recipe.mkdir(parents=True, exist_ok=True)
    shared = data.get('shared_files', ())
    with (cache / 'download.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        download(source['url'], archive, source['sha256'])
        recipe_inputs(cache, recipe, data)
        if shared:
            arch = policy()
            recipe_inputs(cache, recipe, {'recipe_url': arch['recipe_url'],
                                          'files': {name: arch['files'][name] for name in shared}})
    shutil.copy2(archive, recipe / archive.name)
    patch_names = [Path(row['path']).name for row in patches]
    for row, name in zip(patches, patch_names, strict=True):
        shutil.copy2(ROOT / row['path'], recipe / name)
    sources = [archive.name, *(name for name in data['files'] if name != 'PKGBUILD'), *shared, *patch_names]
    recipe_patch = 'PKGBUILD.steamos.patch' if target == 'steamos' else 'PKGBUILD.patch'
    text = capture(['patch', '--silent', '--batch', '--fuzz=0', '-o', '-',
                    recipe / 'PKGBUILD.upstream', '-i', RECIPE_DIR / recipe_patch])
    values = {'VERSION': upstream, 'RELEASE': data['package_release'],
              'SPANDSP': data['spandsp'].get('minimum_version', ''),
              'V4L2_REPLACES': v4l2_replacement(data),
              'LIBCAMERA_VERSION': version, 'JOBS': str(jobs), 'PATCHES': shlex.join(patch_names),
              'SOURCES': shlex.join(sources), 'CHECKSUMS': shlex.join(
                  package_build.sha(recipe / name) for name in sources)}
    for key, value in values.items():
        text = text.replace('@' + key + '@', value)
    # libcamera archives are installed inputs, but a resume must match their bytes too.
    references = '\n'.join(f'# {row["file"]}: {row["sha256"]}' for row in inputs.values())
    (recipe / 'PKGBUILD').write_text(f'{text}\n\n{references}\n')


def verify_packages(runtime, archives, expected, version, audio_soname, camera_abi, *,
                    target='arch', webrtc_soname=None):
    packages = {}
    camera_dependencies = {f'pipewire={version}', *libcamera_arch.sonames(camera_abi)}
    for filename, row in archives.items():
        archive = runtime / filename
        fields = overlay_packages.pacman_info(archive)
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
        if name == V4L2_OWNER[target]:
            if (V4L2_PACKAGE not in fields.get('conflict', ()) or
                    v4l2_replacement(policy(target)) not in fields.get('replaces', ())):
                raise ValueError('The native V4L2 SPA must replace the standalone plugin')
            if V4L2_MODULE not in capture(['bsdtar', '-tf', archive]).splitlines():
                raise ValueError(f'{name} must contain the standard V4L2 SPA module')
        if target == 'steamos' and name == 'libpipewire':
            if VALVE_SPA_LIBRARY not in capture(['bsdtar', '-tf', archive]).splitlines():
                raise ValueError('SteamOS libpipewire must contain the Valve SPA library')
        if name == 'pipewire-audio':
            if audio_soname not in depends:
                raise ValueError('The Bluetooth audio plugins do not match the target spandsp ABI')
            if webrtc_soname and webrtc_soname not in depends:
                raise ValueError('The audio plugins must require the target WebRTC ABI')
            if target == 'steamos' and f'pipewire-netjack2={version}' not in depends:
                raise ValueError('SteamOS pipewire-audio must keep the matching netJACK2 modules')
        packages[name] = {'file': filename, 'sha256': row['sha256'],
                          'version': version, 'architecture': 'aarch64'}
    if set(packages) != expected or not set(install_names(expected, set(), target=target)) <= expected:
        raise ValueError('The build must produce all official PipeWire split packages')
    return packages


def install_names(splits, installed, *, target='arch'):
    """Upgrade installed splits and add the camera plugin, not optional providers."""
    required = REQUIRED | (overlay_packages.STEAMOS_PIPEWIRE_RUNTIME if target == 'steamos' else set())
    return sorted(required | (splits & installed))


def native_archives(packages):
    return {row['file']: {'sha256': row['sha256']} for row in packages}


def build(root, output, jobs, libcamera):
    from desktop.build_pipewire_packages import arch_policy

    libcamera = Path(libcamera).resolve()
    if output.is_relative_to(libcamera) or libcamera.is_relative_to(output):
        raise ValueError('Use separate outputs for the PipeWire and libcamera builds')
    target = libcamera_arch.build_target(root)
    data = policy(target)
    camera_runtime, camera_version, camera_abi, inputs = libcamera_arch.runtime_inputs(libcamera, target=target)
    base = data['upstream_package_version']
    upstream, patches = arch_policy(base)
    version = base.rsplit('-', 1)[0] + '-' + overlay_release.pacman_release(data['package_release'])
    if extra := data.get('libcamera_patches'):
        patches = patch_records(patches, extra)
    output.mkdir(parents=True, exist_ok=True)
    with (output / 'build.lock').open('a') as lock, (output / 'build.log').open('a') as log:
        fcntl.flock(lock, fcntl.LOCK_EX)
        recipe = output / 'recipe'
        prepare(recipe, data, upstream['source'], patches, camera_version, inputs, jobs, target=target)
        expected = set(capture(['bash', '-ec', 'source "$1"; printf "%s\\n" "${pkgname[@]}"',
                                'pipewire-recipe', recipe / 'PKGBUILD']).splitlines())
        installed = dict(line.split(maxsplit=1)
                         for line in capture(['chroot', root, 'pacman', '-Q']).splitlines())
        selected = install_names(expected, installed.keys(), target=target)
        if V4L2_PACKAGE in installed:
            raise ValueError(f'Build the native PipeWire set in an Arch root without {V4L2_PACKAGE}')
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
            raise ValueError(f'{target} PipeWire packaging failed; see {log.name}') from exc
        manifest = json.loads((runtime / 'manifest.json').read_text())
        packages = verify_packages(runtime, manifest['packages'], expected, version,
                                   data['spandsp']['soname'], camera_abi, target=target,
                                   webrtc_soname=data.get('webrtc_soname'))
        build_arch_package.install_cached(root, runtime,
                                          selected_archives=native_archives(packages[name] for name in selected))
        result = dict(line.split(maxsplit=1) for line in
                      capture(['chroot', root, 'pacman', '-Q', *selected]).splitlines())
        if result != dict.fromkeys(selected, version):
            raise ValueError('The installed PipeWire packages do not match the native build')
        record = {'packages_built': True, 'upstream_package_version': base,
                  'package_version': version, 'package_format': 'pacman', 'packages': packages,
                  'target': target, 'arch_recipe': data, 'archive_sha256': upstream['source']['sha256'],
                  'patches': patches, 'build_dependencies': inputs,
                  'recipe_sha256': manifest['recipe_sha256']}
        (output / 'SOURCE.json').write_text(json.dumps(record, indent=2) + '\n')
        return record
