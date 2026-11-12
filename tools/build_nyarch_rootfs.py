#!/usr/bin/env python3
"""Build Nyarch GNOME from current ARM64 packages and the shared Piano BSP."""
import argparse
import hashlib
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
import stage_arch_piano
from build_steamos_rootfs import sensor_sources, sensor_recipes
from prepare_steamos_rootfs import download

ROOT = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, **kwargs)


def build(args):
    output = args.output.resolve()
    if os.geteuid() or not output.is_relative_to(ROOT / 'build/distros'):
        raise ValueError('Use root and an output below build/distros')
    cfg = json.loads((ROOT / 'config/nyarch.json').read_text())
    policy = json.loads((ROOT / 'config/release.json').read_text())
    inputs = ROOT / 'build/nyarch-inputs'
    inputs.mkdir(parents=True, exist_ok=True)
    archive = inputs / 'ArchLinuxARM-aarch64.tar.gz'
    if not archive.exists():
        partial = archive.with_suffix('.partial')
        with urllib.request.urlopen(cfg['base_url'], timeout=90) as source, partial.open('wb') as target:
            shutil.copyfileobj(source, target)
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

    apps = inputs / 'applications'
    if not apps.exists():
        build_nyarch_apps.prepare_sources(apps)
    if not (output / 'packages/nyarch/SOURCE.json').exists():
        with release.build_resolver(root):
            build_nyarch_apps.build(root, apps, output / 'packages/nyarch')
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

    sensors = inputs / 'sensors'
    if not sensors.exists():
        sensor_sources(sensors, policy['sensors'])
    recipes = inputs / 'recipes'
    if not recipes.exists():
        sensor_recipes(recipes, sensors)
        mesa = recipes / 'mesa'
        shutil.copytree(ROOT / 'linux/bsp/arch/mesa', mesa)
        shutil.copy2(ROOT / 'upstream/piano-mesa-current/patches/0001-freedreno-Add-chip-id-support-for-A830v1.patch', mesa)
    gsd = recipes / 'gnome-settings-daemon'
    if gsd.exists():
        shutil.rmtree(gsd)
    run(sys.executable, ROOT / 'linux/desktops/gnome/ambient-policy/packaging/prepare_package.py',
        'arch', gsd)
    mutter = recipes / 'mutter'
    shutil.copytree(ROOT / 'linux/desktops/gnome/arch/mutter', mutter,
                    dirs_exist_ok=True)
    for name in ('0001-kms-empty-gamma-bypass.patch',
                 '0003-wayland-touch-ignore-cancelled-grab.patch'):
        shutil.copy2(ROOT / 'linux/desktops/gnome/patches/mutter' / name, mutter)
    control_center = recipes / 'control-center'
    shutil.copytree(ROOT / 'linux/desktops/gnome/arch/control-center',
                    control_center, dirs_exist_ok=True)
    shutil.copy2(ROOT / 'linux/desktops/gnome/patches/control-center/0001-about-gunyah-primary-domain.patch',
                 control_center)
    # SteamOS's protobuf compatibility rebuild is intentionally not selected
    # for Arch: its rolling packages already share the current library ABI.
    for name in ('mutter', 'control-center', 'mesa', 'fastrpc', 'libssc', 'iio-sensor-proxy'):
        packages = output / 'packages' / name
        if not (packages / 'manifest.json').exists():
            build_arch_package.build(root, recipes / name, packages)
    gsd_packages = output / 'packages/gnome-settings-daemon'
    gsd_manifest = gsd_packages / 'manifest.json'
    if (not gsd_manifest.is_file() or
            json.loads(gsd_manifest.read_text()).get('recipe_sha256') !=
            release.digest(gsd / 'PKGBUILD')):
        build_arch_package.build(root, gsd, gsd_packages)
    else:
        build_arch_package.install_cached(root, gsd_packages)

    ffmpeg = output / 'packages/ffmpeg'
    if not build_arch_ffmpeg.is_current(ffmpeg):
        if ffmpeg.exists():
            shutil.rmtree(ffmpeg)
        build_arch_ffmpeg.build(root, ffmpeg)
    else:
        build_arch_package.install_cached(root, ffmpeg)
    pipewire = output / 'packages/pipewire'
    run(ROOT / 'build.sh', 'pipewire', '--rootfs', root, '--output', pipewire)
    gdm = output / 'packages/gdm'
    run(ROOT / 'build.sh', 'gdm', '--rootfs', root, '--output', gdm)

    kernel = ROOT / 'artifacts/kernels/release'
    source = Path(json.loads((ROOT / 'build/release/source-manifest.json').read_text())['worktree'])
    runtime = output / 'runtime'
    if not (runtime / 'manifest.json').exists():
        run(sys.executable, ROOT / 'tools/build_release_helpers.py',
            '--kernel', kernel, '--source', source,
            '--kernel-build', ROOT / 'build/kernels/release',
            '--output', runtime, '--sysroot', args.bootstrap_sysroot.resolve())
    stage_arch_piano.stage(argparse.Namespace(
        rootfs=root, kernel=kernel, runtime=runtime, sensor_source=sensors,
        bootstrap_sysroot=args.bootstrap_sysroot, desktop='nyarch',
        desktop_source=desktop, extensions=extensions))
    manifest = output / 'manifest.json'
    record = json.loads(manifest.read_text())
    record.update(base_archive_sha256=base_hash, nyarch_inputs=cfg,
                  gnome_power=json.loads(gsd_manifest.read_text()),
                  ffmpeg=json.loads((ffmpeg / 'SOURCE.json').read_text()),
                  pipewire=json.loads((pipewire / 'SOURCE.json').read_text()),
                  gdm=json.loads((gdm / 'SOURCE.json').read_text()))
    manifest.write_text(json.dumps(record, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/distros/nyarch-gnome-piano')
    parser.add_argument('--bootstrap-sysroot', type=Path, default=Path('/'))
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.worker:
        raise SystemExit(run('unshare', '--mount', '--propagation', 'private',
                             sys.executable, __file__, *sys.argv[1:], '--worker').returncode)
    build(args)
