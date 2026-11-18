#!/usr/bin/env python3
"""Install the shared Piano hardware sources into a completed Arch desktop.

The native Mesa, sensor and Nyarch application packages must already be
installed through pacman. This assembles firmware, kernel modules and the same
static runtime used by Debian; it never reads the host's live device state.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

import build_piano_runtime_helpers as runtime
import build_release_rootfs as release
from build_nyarch_apps import guest_environment
import stage_piano_full_userspace as userspace
import stage_piano_kernel_modules as modules
import stage_piano_ram_hardware as hardware
import stage_nyarch_desktop as nyarch
from build import overlay_packages, sensors
from package import camera

ROOT = Path(__file__).resolve().parents[1]


def put(root, name, text, mode=0o644):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(mode)


def pacman_configuration(text, steamos):
    """Enable the signed repositories before stock repositories, once each."""
    from package.boot import PACMAN_REPOSITORIES

    generated = {f'/etc/pacman.d/{name}.conf' for name in PACMAN_REPOSITORIES}
    lines = []
    sections = []
    for line in text.splitlines(keepends=True):
        uncommented = line.split('#', 1)[0].strip()
        include = re.fullmatch(r'Include\s*=\s*(\S+)', uncommented)
        if include and include[1] in generated:
            continue
        section = re.fullmatch(r'\[([^\]]+)\]', uncommented)
        if section:
            sections.append((len(lines), section[1]))
        lines.append(line)
    names = [name for _, name in sections]
    if len(names) < 2 or names[0] != 'options' or names.count('options') != 1:
        raise ValueError('pacman.conf must start with options and contain a repository')
    if any(name in PACMAN_REPOSITORIES for name in names):
        raise ValueError('pacman.conf has an unmanaged SunUEFI repository section')
    insertion = sections[1][0]
    selected = PACMAN_REPOSITORIES[::-1] if steamos else PACMAN_REPOSITORIES[:1]
    lines[insertion:insertion] = [f'Include = /etc/pacman.d/{name}.conf\n'
                                  for name in selected]
    return ''.join(lines)


def install_repository(root, steamos):
    from package.boot import PACMAN_REPOSITORIES, pacman_repository

    root = Path(root).resolve()
    builds = ROOT / 'build/distros'
    if os.geteuid() or root == builds or not root.is_relative_to(builds):
        raise ValueError('Configure pacman repositories only in a project build root')
    for name in PACMAN_REPOSITORIES if steamos else PACMAN_REPOSITORIES[:1]:
        fragment = release.target(root, f'etc/pacman.d/{name}.conf')
        if fragment.is_symlink() or fragment.read_text() != pacman_repository(name):
            raise ValueError('Install the matching Piano repository configuration: ' + name)
    policy = release.target(root, 'etc/pacman.conf')
    if policy.is_symlink():
        raise ValueError('Expected a regular pacman.conf in the build root')
    configured = pacman_configuration(policy.read_text(), steamos)
    with guest_environment(root) as guest:
        guest(['pacman-key', '--populate', 'sunuefi'])
    policy.write_text(configured)


def install_camera(root, guest, *, published):
    """Install the same camera archive in Nyarch and SteamOS."""
    archive, metadata = (camera.shared_package('pacman') if published
                         else camera.ensure('pacman'))
    tmp = release.target(root, 'var/tmp')
    tmp.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sunuefi-camera-', dir=tmp) as directory:
        work = Path(directory)
        work.chmod(0o755)
        if published:
            overlay_packages.install_pacman(root, work, 'sunuefi',
                                            {archive.name: metadata}, guest)
        else:
            staged = work / archive.name
            shutil.copyfile(archive, staged)
            if release.digest(staged) != metadata['sha256']:
                raise ValueError('Camera package changed during installation: ' + archive.name)
            guest(['pacman', '-U', '--noconfirm', '/' + staged.relative_to(root).as_posix()])
    return archive, metadata


def install_sensors(root, guest, *, published):
    """Install the device package after the target's SSC provider."""
    guest(['pacman', '-T', sensors.SSC_DEPENDENCY])
    tmp = release.target(root, 'var/tmp')
    tmp.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sunuefi-sensors-', dir=tmp) as folder:
        work = Path(folder)
        work.chmod(0o755)
        if published:
            archive, metadata = sensors.pacman_package(ROOT / 'artifacts/packages/bsp/sensors')
            overlay_packages.install_pacman(root, work, 'sunuefi', {archive.name: metadata}, guest)
        else:
            output = work / 'package'
            sensors.build_pacman(output)
            archive, metadata = sensors.pacman_package(output)
            guest(['pacman', '-U', '--noconfirm', '/' + archive.relative_to(root).as_posix()])
    return metadata


def stage(args):
    root, output = args.rootfs.resolve(), args.rootfs.resolve().parent
    if os.geteuid() or not root.is_relative_to(ROOT / 'build/distros'):
        raise ValueError('Stage only a privileged project build rootfs')
    cfg = json.loads((ROOT / 'config/release.json').read_text())
    kernel, kernel_hash = modules.inspect(args.kernel)
    helper_kernel = json.loads((args.runtime / 'manifest.json').read_text())['kernel']
    if (helper_kernel['commit'], helper_kernel['release']) != (
            kernel['source_commit'], kernel['kernel_release']):
        raise ValueError('Rebuild the runtime helpers for the selected kernel')
    steamos = getattr(args, 'desktop', 'nyarch') == 'steamos'
    published_packages = getattr(args, 'published_packages', None)
    device_configuration = getattr(args, 'device_configuration', None)
    topology = getattr(args, 'audio_topology_package', None)
    if device_configuration is None and topology is None:
        device_configuration, topology = overlay_packages.install_local_arch_bsp(root)
    if device_configuration is None or topology is None:
        raise ValueError('Both shared device packages must be installed')
    userspace.stage(root, ROOT / cfg['debian']['source'],
                    desktop='kde' if steamos else 'gnome')
    # These are generated files in the build root; refresh them on --resume.
    adapters = output / 'piano-adapters'
    if adapters.exists():
        shutil.rmtree(adapters)
    hardware.build(adapters, ROOT / cfg['debian']['source'], root)
    if published_packages is None:
        from package.kernel import install as install_kernel
        install_kernel(root, 'pacman', args.kernel)
    runtime.stage(args.runtime.resolve(), root)
    if published_packages is not None:
        from build_piano_fingerprint import desktop_service
        desktop_service(root)
    else:
        from build_piano_fingerprint import install_desktop
        install_desktop(root, args.runtime.resolve(), output / 'fingerprint-packages')
    fw = ROOT / cfg['firmware']['components']
    from stage_piano_firmware import stage as stage_firmware, verify_display
    firmware = stage_firmware(fw, root, 'pacman', shared=published_packages is not None)
    # Upstream base selects GDM via Debian paths; use Arch's installed names.
    (root / 'etc/gdm3/daemon.conf').unlink(missing_ok=True)
    put(root, 'usr/lib/systemd/system/piano-hostkeys.service.d/10-sshd.conf',
        '[Unit]\nBefore=sshd.service\n')
    put(root, 'usr/lib/systemd/system/sshd.service.d/10-piano.conf',
        '[Unit]\nRequires=piano-hostkeys.service\nAfter=piano-hostkeys.service\n')
    (root / 'usr/lib/systemd/system/ssh.service.d/piano.conf').unlink(missing_ok=True)
    # Standard libinput hardware defaults; no desktop-specific name spoofing.
    release.apply_policy(root, output, cfg)
    if not steamos:
        put(root, 'usr/share/gnome-settings-daemon/ambient-profiles/piano.ini',
            (ROOT / 'linux/desktops/gnome/ambient-policy/piano.ini').read_text())
    put(root, 'etc/piano/linux-debug.conf', 'usb=acm-ncm\nshell=1\nrecovery_seconds=0\n')
    put(root, 'etc/chromium-flags.conf',
        (ROOT / 'linux/bsp/arch/chromium-flags.conf').read_text())
    with release.build_resolver(root), guest_environment(root) as guest:
        sensor_record = install_sensors(root, guest, published=published_packages is not None)
        camera_archive, camera_record = install_camera(
            root, guest, published=published_packages is not None)
        required = ('erofs-utils', 'device-mapper', 'rsync')
        missing = [name for name in required if subprocess.run(
            ['chroot', root, 'pacman', '-Q', name], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL).returncode]
        if missing:
            guest(['pacman', '-S', '--needed', '--noconfirm', *missing])
        # The upstream demo account occupies UID 1000 in bootstrap tarballs.
        if subprocess.run(['chroot', root, 'id', 'alarm'], capture_output=True).returncode == 0:
            guest(['userdel', '-r', 'alarm'])
        if subprocess.check_output(['chroot', root, 'id', '-u', 'piano'], text=True).strip() != '1000':
            guest(['usermod', '-u', '1000', 'piano'])
        guest(['chown', '-R', 'piano:piano', '/home/piano'])
        guest(['usermod', '-aG', 'audio,video,input,render', 'piano'])
        guest(['chpasswd'], input='piano:' + cfg['login']['password'] + '\n', text=True)
        guest(['systemctl', 'unmask', 'sleep.target', 'suspend.target'])
        guest(['systemctl', 'mask', 'hibernate.target',
               'hybrid-sleep.target', 'suspend-then-hibernate.target'])
        guest(['systemctl', 'enable', 'sddm' if steamos else 'gdm', 'NetworkManager', 'sshd', 'bluetooth',
               'piano-camerad', 'piano-hostkeys', 'adsprpcd-sensorspd'])
        guest(['systemctl', 'set-default', 'graphical.target'])
    if steamos:
        from stage_steamos_desktop import configure
        desktop = configure(root, published_packages=published_packages is not None)
    else:
        from prepare_nyarch_material import prepare, UUID
        extension_bundle = output / 'nyarch-shell-extensions'
        if extension_bundle.exists():
            shutil.rmtree(extension_bundle)
        shutil.copytree(args.extensions, extension_bundle)
        prepare(args.desktop_source.resolve(), root, extension_bundle / UUID)
        desktop = nyarch.stage(root, args.desktop_source, extension_bundle)
    clock = release.clock_epoch(root)
    release.native_boot_request(root, output)
    # Fixed Debian tools build the shared bootstrap outside the Arch root.
    boot = release.bootstrap(args.kernel.resolve(), output, kernel['kernel_release'])
    release.sanitize(root, output)
    # These rolling packages moved their dist-info license directory without
    # updating the /usr/share/licenses symlink. Preserve the actual licenses.
    for package, pattern, filename in (
            ('python-jaraco.collections', 'jaraco_collections-*.dist-info/licenses/LICENSE', 'LICENSE'),
            ('python-numpy', 'numpy-*.dist-info/licenses/LICENSE.txt', 'LICENSE.txt')):
        link = root / 'usr/share/licenses' / package / filename
        if link.is_symlink() and not userspace.guest_resolve(root, str(link.relative_to(root))).exists():
            matches = list((root / 'usr/lib').glob('python*/site-packages/' + pattern))
            if len(matches) != 1:
                raise ValueError('Cannot locate installed license: ' + package)
            link.unlink()
            link.symlink_to(os.path.relpath(matches[0], link.parent))
    for name in ('var/cache/pacman/pkg', 'var/tmp', 'tmp'):
        folder = root / name
        for item in folder.iterdir():
            if item.is_dir() and not item.is_symlink():
                shutil.rmtree(item)
            else:
                item.unlink()
    packages = subprocess.check_output(['chroot', root, 'pacman', '-Q'], text=True)
    overlay_packages.verify_pacman_versions(
        {**(published_packages or {}), 'device_configuration': device_configuration,
         'audio_topology_package': topology, 'camera_package': camera_record,
         'sensor_package': sensor_record}, packages)
    overlay_packages.verify_bsp_files(root, 'pacman')
    overlay_packages.verify_topology(root, 'pacman', topology)
    verify_display(root, 'pacman', firmware['display_package'])
    sensors.verify_arch_files(root)
    camera.verify_installed(root, 'pacman', camera_archive)
    from package.v4l2loopback import verify_installed
    verify_installed(root, 'pacman', kernel['kernel_release'])
    (output / 'packages.tsv').write_text(packages)
    record = {'status': 'HOST_BUILT_ROOT_NOT_BOOT_VERIFIED',
              'distro': 'steamos' if steamos else 'nyarch',
              'base': 'Valve SteamOS ARM64' if steamos else 'Arch Linux ARM rolling',
              'desktop': 'kde' if steamos else 'gnome', 'kernel_release': kernel['kernel_release'],
              'kernel_commit': kernel['source_commit'], 'kernel_manifest_sha256': kernel_hash,
              'root_policy': 'LABEL=PIANOROOT', 'login': cfg['login'],
              'desktop_configuration': desktop, 'initramfs': boot, 'clock_epoch': clock,
              'firmware_components': cfg['firmware']['components'],
              'display_firmware_package': firmware['display_package'],
              'packages_sha256': release.digest(output / 'packages.tsv'),
              'camera_package': camera_record, 'audio_topology_package': topology,
              'sensor_package': sensor_record,
              'runtime_manifest_sha256': release.digest(args.runtime / 'manifest.json')}
    (output / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('rootfs', 'kernel', 'runtime', 'bootstrap-sysroot'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--desktop', choices=('nyarch', 'steamos'), default='nyarch')
    parser.add_argument('--desktop-source', type=Path)
    parser.add_argument('--extensions', type=Path, default=ROOT / 'build/nyarch-extensions')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.desktop == 'nyarch' and args.desktop_source is None:
        parser.error('--desktop-source is required for Nyarch')
    if not args.worker:
        raise SystemExit(subprocess.run(['unshare', '--mount', '--propagation', 'private',
                                        sys.executable, __file__, *sys.argv[1:], '--worker']).returncode)
    stage(args)
