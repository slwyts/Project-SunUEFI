#!/usr/bin/env python3
"""Build Nyarch GNOME from current ARM64 packages and the shared Piano BSP."""
import argparse
import json
import os
from pathlib import Path
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
from build import arch_mesa, overlay_cache, overlay_packages, overlay_release
import stage_arch_piano
from build_steamos_rootfs import kde_package_is_current, sensor_sources, sensor_recipes
from prepare_steamos_rootfs import download

ROOT = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, **kwargs)


def build(args):
    target = 'nyarch-gnome'
    output, package_output, published = overlay_packages.pacman_build_mode(args, target, ROOT)
    if os.geteuid() or not output.is_relative_to(ROOT / 'build/distros'):
        raise ValueError('Use root and an output below build/distros')
    cfg = json.loads((ROOT / 'config/nyarch.json').read_text())
    policy = json.loads((ROOT / 'config/release.json').read_text())
    inputs = ROOT / 'build/nyarch-inputs'
    inputs.mkdir(parents=True, exist_ok=True)
    archive = inputs / 'ArchLinuxARM-aarch64.tar.gz'
    if not archive.exists():
        partial = archive.with_suffix('.partial')
        with urllib.request.urlopen(cfg['base_url'], timeout=90) as source, \
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
            '--distro', 'arch', '--desktop', 'gnome', '--execute',
            '--output', str(output), '--base-file', str(archive),
            '--base-url', cfg['base_url'], '--base-sha256', base_hash])
        assemble_rootfs.execute(assemble_rootfs.plan(options))
    elif not (output / 'manifest.json').exists():
        raise ValueError('The base desktop assembly did not complete')

    sensors = inputs / 'sensors'
    if not sensors.exists():
        sensor_sources(sensors, policy['sensors'])
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
    if published:
        # These are also product runtime dependencies, including user-facing
        # packages normally installed while building the four Nyarch apps.
        with release.build_resolver(root), build_nyarch_apps.guest_environment(root) as guest:
            guest(['pacman', '-Syu', '--needed', '--noconfirm', *build_nyarch_apps.PACKAGES])
        records = overlay_packages.install_arch_overlays(root, kernel, target, published)
    else:
        records, packages = build_overlays(root, output, inputs, sensors, applications,
                                           refresh=not package_output)
        if package_output:
            commit = json.loads((kernel / 'manifest.json').read_text())['source_commit']
            packages.update(overlay_packages.fingerprint_pacman_packages(
                root, output / 'fingerprint-packages', source, kernel_build, commit))
            overlay_cache.check_pacman_inputs(ROOT, root, cache_inputs)
            return overlay_packages.export(package_output, target, overlay_packages.ARCH_REPOSITORIES[target],
                                           overlay_packages.pacman_packages(root, packages), records,
                                           build_inputs=cache_inputs)

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

    runtime = output / 'runtime'
    stage_arch_piano.hardware.stage_pen_calibration(root)
    if not (runtime / 'manifest.json').exists():
        run(sys.executable, ROOT / 'tools/build_release_helpers.py',
            '--kernel', kernel, '--source', source,
            '--kernel-build', kernel_build,
            '--output', runtime, '--sysroot', args.bootstrap_sysroot.resolve(),
            '--firmware-root', root)
    stage_arch_piano.stage(argparse.Namespace(
        rootfs=root, kernel=kernel, runtime=runtime, sensor_source=sensors,
        bootstrap_sysroot=args.bootstrap_sysroot, desktop='nyarch',
        desktop_source=desktop, extensions=extensions,
        published_packages=published[1] if published else None))
    manifest = output / 'manifest.json'
    record = json.loads(manifest.read_text())
    record.update(base_archive_sha256=base_hash, nyarch_inputs=cfg, **records)
    if published:
        record['overlay_manifest_sha256'] = release.digest(args.published_packages)
    manifest.write_text(json.dumps(record, indent=2) + '\n')


def build_overlays(root, output, inputs, sensors, apps, *, refresh):
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
    recipes = inputs / 'recipes'
    if not recipes.exists():
        sensor_recipes(recipes, sensors)
    mesa = recipes / 'mesa'
    arch_mesa.recipe(mesa)
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
    # SteamOS's protobuf compatibility rebuild is intentionally not selected
    # for Arch: its rolling packages already share the current library ABI.
    for name in ('mutter', 'control-center', 'fastrpc', 'libssc', 'iio-sensor-proxy'):
        directory = output / 'packages' / name
        if not kde_package_is_current(directory, recipes / name):
            build_arch_package.build(root, recipes / name, directory)
        else:
            build_arch_package.install_cached(root, directory)
        packages[name] = overlay_packages.pacman_outputs(directory)
    mesa_packages = ROOT / 'build/mesa-pacman/nyarch-gnome'
    if not arch_mesa.is_current(mesa_packages, mesa):
        if mesa_packages.exists():
            shutil.rmtree(mesa_packages)
        build_arch_package.build(root, mesa, mesa_packages, validate=arch_mesa.validate)
        shutil.copy2(mesa / 'SOURCE.json', mesa_packages / 'SOURCE.json')
    else:
        arch_mesa.install_cached(root, mesa_packages)
    packages['mesa'] = overlay_packages.pacman_outputs(mesa_packages)
    gsd_packages = output / 'packages/gnome-settings-daemon'
    gsd_manifest = gsd_packages / 'manifest.json'
    if not kde_package_is_current(gsd_packages, gsd):
        build_arch_package.build(root, gsd, gsd_packages)
    else:
        build_arch_package.install_cached(root, gsd_packages)
    packages['gnome_power'] = overlay_packages.pacman_outputs(gsd_packages)

    ffmpeg = output / 'packages/ffmpeg'
    if not build_arch_ffmpeg.is_current(ffmpeg):
        if ffmpeg.exists():
            shutil.rmtree(ffmpeg)
        build_arch_ffmpeg.build(root, ffmpeg)
    else:
        build_arch_package.install_cached(root, ffmpeg)
    packages['ffmpeg'] = overlay_packages.pacman_outputs(ffmpeg)
    pipewire = output / 'packages/pipewire'
    run(ROOT / 'build.sh', 'pipewire', '--rootfs', root, '--output', pipewire)
    libwacom = output / 'packages/libwacom'
    run(ROOT / 'build.sh', 'libwacom', '--rootfs', root, '--output', libwacom)
    gdm = output / 'packages/gdm'
    run(ROOT / 'build.sh', 'gdm', '--rootfs', root, '--output', gdm)

    for name, directory in (('pipewire', pipewire), ('libwacom', libwacom), ('gdm', gdm)):
        packages[name] = overlay_packages.pacman_outputs(directory / 'runtime')
    records = {name: json.loads((directory / 'SOURCE.json').read_text())
               for name, directory in (('ffmpeg', ffmpeg), ('pipewire', pipewire),
                                       ('libwacom', libwacom), ('gdm', gdm))}
    records['gnome_power'] = json.loads(gsd_manifest.read_text())
    records['nyarch_applications'] = app_records
    return records, packages


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/distros/nyarch-gnome-piano')
    parser.add_argument('--bootstrap-sysroot', type=Path, default=Path('/'))
    parser.add_argument('--resume', action='store_true')
    overlay_packages.pacman_arguments(parser)
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        raise SystemExit(run('unshare', '--mount', '--propagation', 'private',
                             sys.executable, __file__, *sys.argv[1:], '--worker').returncode)
    build(args)
