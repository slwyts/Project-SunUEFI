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
import shutil
import subprocess
import sys

import build_piano_runtime_helpers as runtime
import build_release_rootfs as release
from build_nyarch_apps import guest_environment
import stage_piano_full_userspace as userspace
import stage_piano_kernel_modules as modules
import stage_piano_ram_hardware as hardware
import stage_nyarch_desktop as nyarch

ROOT = Path(__file__).resolve().parents[1]


def put(root, name, text, mode=0o644):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(mode)


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
    userspace.stage(root, ROOT / cfg['debian']['source'],
                    desktop='kde' if steamos else 'gnome')
    # These are generated files in the build root; refresh them on --resume.
    adapters = output / 'piano-adapters'
    if adapters.exists():
        shutil.rmtree(adapters)
    hardware.build(adapters, ROOT / cfg['debian']['source'], root)
    old_modules = ([p.name for p in (root / 'usr/lib/modules').iterdir()
                    if p.is_dir() and p.name != kernel['kernel_release']]
                   if steamos and (root / 'usr/lib/modules').is_dir() else [])
    current_modules = modules.safe_target(
        root, Path('usr/lib/modules') / kernel['kernel_release'])
    if current_modules.exists():
        shutil.rmtree(current_modules)
    modules.stage(root, args.kernel, old_modules)
    runtime.stage(args.runtime.resolve(), root)
    from build_piano_fingerprint import install_desktop
    install_desktop(root, args.runtime.resolve(), output / 'fingerprint-packages')
    fw = ROOT / cfg['firmware']['source']
    from stage_piano_firmware import stage as stage_firmware
    stage_firmware(fw, root)
    # Use the reviewed, patched sensor importer source; Debian packaging itself
    # is not installed in Arch.
    sensor = args.sensor_source.resolve() / 'piano-sensors'
    sensor_record = json.loads((args.sensor_source / 'SOURCE').read_text())
    expected = [{'path': p, 'sha256': release.digest(ROOT / p)} for p in cfg['sensors']['patches']]
    if sensor_record.get('patches') != expected:
        raise ValueError('Sensor importer does not match the configured patch series')
    for name in ('piano-sensors-import', 'piano-sensors-lp-table', 'piano-sensors-wait'):
        put(root, 'usr/libexec/piano-sensors/' + name, (sensor / name).read_text(), 0o755)
    for name in ('piano-sensors-import', 'adsprpcd-sensorspd'):
        text = (sensor / ('debian/piano-sensors.' + name + '.service')).read_text()
        if name == 'adsprpcd-sensorspd':
            text = text.replace(' piano-adsp.service', '')
        put(root, 'usr/lib/systemd/system/' + name + '.service', text)
    put(root, 'usr/lib/systemd/system/iio-sensor-proxy.service.d/piano.conf',
        (sensor / 'iio-sensor-proxy-piano.conf').read_text())
    put(root, 'etc/udev/rules.d/59-fastrpc-remoteproc.rules',
        (sensor / '59-fastrpc-remoteproc.rules').read_text())
    put(root, 'etc/udev/rules.d/81-piano-sensors.rules',
        (sensor / 'debian/piano-sensors.udev').read_text())
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
        guest(['pacman', '-S', '--needed', '--noconfirm', 'erofs-utils', 'device-mapper', 'rsync'])
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
               'hybrid-sleep.target', 'suspend-then-hibernate.target',
               'adsprpcd.service', 'adsprpcd_audiopd.service'])
        guest(['systemctl', 'enable', 'sddm' if steamos else 'gdm', 'NetworkManager', 'sshd', 'bluetooth',
               'piano-camerad', 'piano-hostkeys', 'adsprpcd-sensorspd'])
        guest(['systemctl', 'set-default', 'graphical.target'])
        guest(['depmod', '-a', kernel['kernel_release']])
    if steamos:
        from stage_steamos_desktop import configure
        desktop = configure(root)
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
    # Shared static bootstrap is built from a signed Debian busybox input,
    # separately from the Arch root. It switches into Arch /sbin/init.
    boot = release.bootstrap(args.bootstrap_sysroot.resolve(), args.kernel.resolve(),
                             output, kernel['kernel_release'], firmware_root=root)
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
    (output / 'packages.tsv').write_text(packages)
    record = {'status': 'HOST_BUILT_ROOT_NOT_BOOT_VERIFIED',
              'distro': 'steamos' if steamos else 'nyarch',
              'base': 'Valve SteamOS ARM64' if steamos else 'Arch Linux ARM rolling',
              'desktop': 'kde' if steamos else 'gnome', 'kernel_release': kernel['kernel_release'],
              'kernel_commit': kernel['source_commit'], 'kernel_manifest_sha256': kernel_hash,
              'root_policy': 'LABEL=PIANOROOT', 'login': cfg['login'],
              'desktop_configuration': desktop, 'initramfs': boot, 'clock_epoch': clock,
              'firmware_commit': cfg['firmware']['commit'],
              'packages_sha256': release.digest(output / 'packages.tsv'),
              'runtime_manifest_sha256': release.digest(args.runtime / 'manifest.json')}
    (output / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('rootfs', 'kernel', 'runtime', 'sensor-source', 'bootstrap-sysroot'):
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
