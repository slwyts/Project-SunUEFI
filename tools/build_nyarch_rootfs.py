#!/usr/bin/env python3
"""Build Arch Linux ARM images with Nyarch GNOME or Denial and the Piano BSP."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

import assemble_rootfs
import build_arch_package
import build_arch_ffmpeg
import build_nyarch_apps
import build_release_rootfs as release
from build_piano_runtime_helpers import source_patches
from build import arch_mesa, overlay_cache, overlay_packages, overlay_release, sensors
from build.denial import image_manifest
from build_chroot import arch_mirror_url
import stage_arch_piano
from build_steamos_rootfs import kde_package_is_current, sensor_recipes
from prepare_steamos_rootfs import download

ROOT = Path(__file__).resolve().parents[1]
OVERLAY_TARGET = 'nyarch-gnome'
SHARED_OVERLAYS = frozenset(('mesa', 'fastrpc', 'libssc', 'iio-sensor-proxy', 'ffmpeg',
                             *overlay_packages.PACMAN_CAMERA, 'libwacom', 'libfprint', 'fprintd',
                             'bluez'))


def run(*args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, **kwargs)


def shared_overlays(published):
    repository, packages, components = published
    return (repository,
            {name: row for name, row in packages.items() if row['component'] in SHARED_OVERLAYS},
            {name: row for name, row in components.items() if name in SHARED_OVERLAYS})


def build(args):
    desktop_name = getattr(args, 'desktop', 'nyarch')
    denial = desktop_name == 'denial'
    target = 'arch-denial' if denial else OVERLAY_TARGET
    if denial and getattr(args, 'packages_only', None):
        raise ValueError('Build the shared Arch packages with nyarch-gnome-rootfs --packages-only')
    if args.output is None:
        args.output = ROOT / 'build/distros' / (target + '-piano')
    denial_packages = image_manifest(
        getattr(args, 'denial_packages', None), 'arch-denial', desktop=desktop_name,
        published=getattr(args, 'published_packages', None) is not None)
    output, package_output, published = overlay_packages.pacman_build_mode(args, OVERLAY_TARGET, ROOT)
    if denial and published:
        published = shared_overlays(published)
    if os.geteuid() or not output.is_relative_to(ROOT / 'build/distros'):
        raise ValueError('Use root and an output below build/distros')
    cfg = json.loads((ROOT / 'config/nyarch.json').read_text())
    mirror = arch_mirror_url(cfg['pacman_build_mirror'])
    base_url = mirror + '/os/ArchLinuxARM-aarch64-latest.tar.gz'
    policy = json.loads((ROOT / 'config/release.json').read_text())
    inputs = ROOT / 'build/nyarch-inputs'
    inputs.mkdir(parents=True, exist_ok=True)
    archive = inputs / 'ArchLinuxARM-aarch64.tar.gz'
    if not archive.exists():
        partial = archive.with_suffix('.partial')
        with urllib.request.urlopen(base_url, timeout=90) as source, \
                partial.open('wb') as output_stream:
            shutil.copyfileobj(source, output_stream)
        partial.replace(archive)
    # This bootstrap is rolling, like the pacman repositories. Record the
    # actual digest; the resulting root records repository/package versions.
    base_hash = release.digest(archive)
    root = output / 'rootfs'
    if output.exists() and not args.resume:
        raise ValueError('Output already exists; use --resume for this build')
    if not output.exists():
        options = assemble_rootfs.parser().parse_args([
            '--distro', 'arch', '--desktop', 'denial' if denial else 'gnome', '--execute',
            '--output', str(output), '--base-file', str(archive),
            '--base-url', base_url, '--base-sha256', base_hash])
        plan = assemble_rootfs.plan(options)
        # The saved plan also selects the mirror for subsequent package builds.
        plan['pacman_build_mirror'] = mirror
        assemble_rootfs.execute(plan, defer_denial=denial)
    else:
        manifest = output / 'manifest.json'
        if not manifest.is_file():
            raise ValueError('The base desktop assembly did not complete')
        record = json.loads(manifest.read_text())
        if record.get('desktop') != ('denial' if denial else 'gnome'):
            raise ValueError('Resume must use the same desktop as the existing build')
        if record.get('pacman_build_mirror') != mirror:
            raise ValueError('The Arch build mirror has changed; choose a new output directory')

    kernel = ROOT / 'artifacts/kernels/release'
    source = Path(json.loads((ROOT / 'build/release/source-manifest.json').read_text())['worktree'])
    kernel_build = ROOT / 'build/kernels/release'
    applications = (output if package_output else inputs) / 'applications'
    cache_inputs = None
    if package_output:
        if not applications.exists():
            build_nyarch_apps.prepare_sources(applications)
        # Refresh once before reading the cohort. Build dependencies below use
        # the already selected sync databases rather than switching mid-build.
        with release.build_resolver(root), build_nyarch_apps.guest_environment(root) as guest:
            guest(['pacman', '-Syu', '--needed', '--noconfirm', *build_nyarch_apps.PACKAGES])
        cache_inputs = overlay_cache.pacman_inputs(
            ROOT, root, kernel / 'manifest.json', base_hash,
            json.loads((applications / 'SOURCE.json').read_text()))
        if reused := overlay_cache.restore_pacman(ROOT, package_output, target, cache_inputs):
            return reused
    installed_overlays = None
    if published:
        # These are also product runtime dependencies, including user-facing
        # packages normally installed while building the four Nyarch apps.
        if not denial:
            with release.build_resolver(root), build_nyarch_apps.guest_environment(root) as guest:
                guest(['pacman', '-Syu', '--needed', '--noconfirm', *build_nyarch_apps.PACKAGES])
        records, installed_overlays = overlay_packages.install_arch_overlays(root, kernel, target, published)
    else:
        sensor_source, _ = sensors.source_workspace(inputs / 'sensors', policy['sensors'])
        records, packages = build_overlays(root, output, inputs, sensor_source, applications,
                                           refresh=not package_output, nyarch_desktop=not denial,
                                           plasma=package_output is not None)
        if package_output:
            commit = json.loads((kernel / 'manifest.json').read_text())['source_commit']
            packages.update(overlay_packages.fingerprint_pacman_packages(
                root, output / 'fingerprint-packages', source, kernel_build, commit))
            overlay_cache.check_pacman_inputs(ROOT, root, cache_inputs)
            return overlay_packages.export(package_output, target, overlay_packages.ARCH_REPOSITORIES[target],
                                           overlay_packages.pacman_packages(root, target, packages), records,
                                           build_inputs=cache_inputs)
        records['device_configuration'], records['audio_topology_package'], \
            records['power_profiles_package'] = overlay_packages.install_local_arch_bsp(root)

    desktop, extensions = nyarch_material(cfg, inputs) if not denial else (None, None)
    runtime = output / 'runtime'
    from build_release_helpers import inputs_current
    if (runtime / 'manifest.json').is_file() and not inputs_current(runtime):
        shutil.rmtree(runtime)
    if not (runtime / 'manifest.json').exists():
        run(sys.executable, ROOT / 'tools/build_release_helpers.py',
            '--kernel', kernel, '--source', source,
            '--kernel-build', kernel_build,
            '--output', runtime, '--sysroot', args.bootstrap_sysroot.resolve(),
            '--firmware-root', root)
    stage_arch_piano.stage(argparse.Namespace(
        rootfs=root, kernel=kernel, runtime=runtime,
        bootstrap_sysroot=args.bootstrap_sysroot, desktop='denial' if denial else 'nyarch',
        desktop_source=desktop, extensions=extensions,
        published_packages=installed_overlays,
        denial_packages=denial_packages,
        device_configuration=records['device_configuration'],
        audio_topology_package=records['audio_topology_package'],
        power_profiles_package=records['power_profiles_package']))
    manifest = output / 'manifest.json'
    record = json.loads(manifest.read_text())
    record.update(base_archive_sha256=base_hash, **records)
    if not denial:
        record['nyarch_inputs'] = cfg
    if published:
        record['overlay_manifest_sha256'] = release.digest(args.published_packages)
    manifest.write_text(json.dumps(record, indent=2) + '\n')


def nyarch_material(cfg, inputs):
    desktop = inputs / 'desktop'
    if not desktop.exists():
        run('git', 'clone', '--filter=blob:none', '--no-checkout',
            cfg['desktop_repository'], desktop)
        run('git', '-C', desktop, 'checkout', '--detach', cfg['desktop_commit'])
    if subprocess.check_output(['git', '-C', desktop, 'rev-parse', 'HEAD'], text=True).strip() != cfg['desktop_commit']:
        raise ValueError('Nyarch desktop source differs from the selected commit')

    extensions = inputs / 'shell-extensions'
    extensions.mkdir(exist_ok=True)
    for item in cfg['extensions']:
        bundle = inputs / (item['uuid'] + '.zip')
        download(item['url'], bundle, item['sha256'])
        destination = extensions / item['uuid']
        if not destination.exists():
            with zipfile.ZipFile(bundle) as archive_file:
                for member in archive_file.infolist():
                    name = Path(member.filename)
                    if name.is_absolute() or '..' in name.parts:
                        raise ValueError('Invalid extension archive path')
                archive_file.extractall(destination)
    return desktop, extensions


def build_nyarch_overlays(root, output, recipes, apps, *, refresh):
    if not apps.exists():
        build_nyarch_apps.prepare_sources(apps)
    nyarch = output / 'packages/nyarch'
    marker = nyarch / 'SOURCE.json'
    app_records = json.loads(marker.read_text()) if marker.is_file() else []
    if (not app_records or any(row.get('overlay_release') != overlay_release.generation()
                               for row in app_records)):
        with release.build_resolver(root):
            app_records = build_nyarch_apps.build(root, apps, nyarch, refresh=refresh)
    packages = {'nyarch': (nyarch, {row['package']: {'sha256': row['sha256']}
                                   for row in app_records})}
    gsd = recipes / 'gnome-settings-daemon'
    if gsd.exists():
        shutil.rmtree(gsd)
    run(sys.executable, ROOT / 'linux/desktops/gnome/ambient-policy/packaging/prepare_package.py',
        'arch', gsd, '--rootfs', root)
    mutter = recipes / 'mutter'
    shutil.copytree(ROOT / 'linux/desktops/gnome/arch/mutter', mutter,
                    dirs_exist_ok=True)
    for path in source_patches(ROOT / 'patches/mutter/51.0/series.json'):
        shutil.copy2(path, mutter / path.name)
    control_center = recipes / 'control-center'
    shutil.copytree(ROOT / 'linux/desktops/gnome/arch/control-center',
                    control_center, dirs_exist_ok=True)
    shutil.copy2(ROOT / 'linux/desktops/gnome/patches/control-center/0001-about-gunyah-primary-domain.patch',
                 control_center)
    for name in ('mutter', 'control-center'):
        directory = output / 'packages' / name
        if not kde_package_is_current(directory, recipes / name):
            build_arch_package.build(root, recipes / name, directory)
        else:
            build_arch_package.install_cached(root, directory)
        packages[name] = overlay_packages.pacman_outputs(directory)
    return {'nyarch_applications': app_records}, packages


def build_bluez(root, recipe, output):
    """Build the daemon on Arch Linux ARM without overriding Valve's package."""
    package = ROOT / 'linux/bsp/arch' / overlay_packages.ARCH_BLUEZ
    version = re.search(r'(?m)^pkgver=(\S+)$', (package / 'PKGBUILD').read_text())[1]
    metadata = subprocess.check_output(
        ['chroot', root, 'env', 'LC_ALL=C', 'pacman', '-Si', 'extra/bluez'], text=True)
    upstream = re.search(r'(?m)^Version\s*:\s*(\S+)$', metadata)[1].split(':')[-1].rsplit('-', 1)[0]
    if version != upstream:
        raise ValueError('Check the HoG fix against Arch Linux ARM BlueZ ' + upstream)
    shutil.copytree(package, recipe, dirs_exist_ok=True)
    for patch in source_patches(ROOT / 'patches/bluez' / version / 'series.json'):
        shutil.copy2(patch, recipe / patch.name)
    options = overlay_packages.ARCH_BLUEZ_INSTALL_OPTIONS
    if kde_package_is_current(output, recipe):
        build_arch_package.install_cached(root, output, install_options=options)
    else:
        build_arch_package.build(root, recipe, output, check=True, install_options=options)
    return overlay_packages.pacman_outputs(output)


def build_overlays(root, output, inputs, sensor_source, apps, *, refresh, nyarch_desktop, plasma):
    records, packages = {}, {}
    recipes = inputs / 'recipes'
    if nyarch_desktop:
        records, packages = build_nyarch_overlays(root, output, recipes, apps, refresh=refresh)
    sensor_recipes(recipes, sensor_source)
    # SteamOS needs a separate protobuf compatibility rebuild. Arch uses
    # its rolling repository's existing library ABI.
    for name in ('fastrpc', 'libssc', 'iio-sensor-proxy'):
        directory = output / 'packages' / name
        if not kde_package_is_current(directory, recipes / name):
            build_arch_package.build(root, recipes / name, directory)
        else:
            build_arch_package.install_cached(root, directory)
        packages[name] = overlay_packages.pacman_outputs(directory)
    mesa = recipes / 'mesa'
    arch_mesa.recipe(mesa)
    mesa_packages = ROOT / 'build/mesa-pacman' / OVERLAY_TARGET
    if not arch_mesa.is_current(mesa_packages, mesa):
        if mesa_packages.exists():
            shutil.rmtree(mesa_packages)
        build_arch_package.build(root, mesa, mesa_packages, validate=arch_mesa.validate)
        shutil.copy2(mesa / 'SOURCE.json', mesa_packages / 'SOURCE.json')
    else:
        arch_mesa.install_cached(root, mesa_packages)
    packages['mesa'] = overlay_packages.pacman_outputs(mesa_packages)
    if nyarch_desktop:
        gsd = recipes / 'gnome-settings-daemon'
        gsd_packages = output / 'packages/gnome-settings-daemon'
        if not kde_package_is_current(gsd_packages, gsd):
            build_arch_package.build(root, gsd, gsd_packages)
        else:
            build_arch_package.install_cached(root, gsd_packages)
        packages['gnome_power'] = overlay_packages.pacman_outputs(gsd_packages)
        records['gnome_power'] = json.loads((gsd_packages / 'manifest.json').read_text())
    ffmpeg = output / 'packages/ffmpeg'
    if not build_arch_ffmpeg.is_current(ffmpeg):
        if ffmpeg.exists():
            shutil.rmtree(ffmpeg)
        build_arch_ffmpeg.build(root, ffmpeg)
    else:
        build_arch_package.install_cached(root, ffmpeg)
    packages['ffmpeg'] = overlay_packages.pacman_outputs(ffmpeg)
    camera_records, camera_packages = overlay_packages.build_pacman_camera(
        root, output / 'packages', plasma=plasma)
    records.update(camera_records)
    packages.update(camera_packages)
    libwacom = output / 'packages/libwacom'
    run(ROOT / 'build.sh', 'libwacom', '--rootfs', root, '--output', libwacom)
    packages['libwacom'] = overlay_packages.pacman_outputs(libwacom / 'runtime')
    packages['bluez'] = build_bluez(root, recipes / 'bluez', output / 'packages/bluez')
    records.update({name: json.loads((directory / 'SOURCE.json').read_text())
                    for name, directory in (('ffmpeg', ffmpeg), ('libwacom', libwacom))})
    if nyarch_desktop:
        gdm = output / 'packages/gdm'
        run(ROOT / 'build.sh', 'gdm', '--rootfs', root, '--output', gdm)
        packages['gdm'] = overlay_packages.pacman_outputs(gdm / 'runtime')
        records['gdm'] = json.loads((gdm / 'SOURCE.json').read_text())
    return records, packages


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--desktop', choices=('nyarch', 'denial'), default='nyarch')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--bootstrap-sysroot', type=Path, default=Path('/'))
    parser.add_argument('--resume', action='store_true')
    overlay_packages.pacman_arguments(parser)
    parser.add_argument('--denial-packages', type=Path, metavar='MANIFEST',
                        help='Install the published Denial package set for the Arch Denial image')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        raise SystemExit(subprocess.run(['unshare', '--mount', '--propagation', 'private',
                                        sys.executable, __file__, *sys.argv[1:], '--worker']).returncode)
    try:
        build(args)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(2, str(error) + '\n')
