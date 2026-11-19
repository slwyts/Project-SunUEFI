#!/usr/bin/env python3
"""Build Debian desktop packages and product images without touching a device."""
import argparse
from contextlib import contextmanager, ExitStack
from datetime import datetime, timezone
import fcntl
import gzip
import json
import os
from pathlib import Path
import platform
import shutil
import stat
import subprocess
import sys
import tempfile
import time

import build_piano_disk_bootstrap as disk
import build_piano_runtime_helpers as runtime
import stage_piano_full_userspace as userspace
import stage_piano_kernel_modules as modules
import stage_piano_ram_hardware as hardware
from assemble_rootfs import product_fstab
from build import overlay_packages, overlay_release
from build.denial import image_manifest
from package_bsp import POWER_PROFILES_PACKAGE
from make_kernel_initramfs import make_newc, validate_static_arm64_elf
from package import camera
from prepare_debian_desktop import DESKTOPS, desktop_packages
from prepare_release_kernel import release_series

ROOT = Path(__file__).resolve().parents[1]
POLICY = ROOT / 'config/release.json'
DEBIAN_MIRROR = 'https://deb.debian.org/debian'
REGDB_PATH = '/lib/firmware/regulatory.db'


def package_build_manifest(desktop):
    return f'suite=sid arch=arm64 desktop={desktop} type=package-build'


def digest(path): return modules.sha(path)


def capture(args, cwd=None):
    return subprocess.check_output(list(map(str, args)), cwd=cwd, text=True).strip()


def debian_archive(rootfs, desktop='gnome'):
    """Check the actual APT indexes, including a resumed generated root."""
    release = userspace.guest_resolve(rootfs, '/etc/os-release').read_text()
    if 'ID=debian' not in release.splitlines():
        raise ValueError('The product base must be Debian')
    text = capture(['chroot', rootfs, 'apt-get', 'indextargets', '--format',
                    '$(CODENAME) $(SUITE) $(ORIGIN) $(IDENTIFIER)'])
    rows = [line.split() for line in text.splitlines() if line.strip()]
    sid = ('sid', 'unstable', 'Debian')
    origins = {sid, ('sid', 'sid', 'SunUEFI')}
    if desktop == 'denial':
        from build.denial import APTORIGIN
        origins.add(APTORIGIN)
    if (any(len(row) != 4 or tuple(row[:3]) not in origins for row in rows) or
            [*sid, 'Packages'] not in rows):
        raise ValueError('Debian product APT indexes must contain sid only; rebuild the base root')
    return {'suite': 'sid', 'indexes': rows}


def repository(path, commit=None, tree=None):
    head = capture(['git', '-C', path, 'rev-parse', 'HEAD'])
    if (commit and head != commit) or capture(['git', '-C', path, 'status', '--porcelain']):
        raise ValueError('Source HEAD/cleanliness mismatch: ' + str(path))
    if tree and capture(['git', '-C', path, 'rev-parse', 'HEAD^{tree}']) != tree:
        raise ValueError('Release kernel tree mismatch')
    return head


def image_status(desktop):
    return f'HOST_BUILT_RELEASE_{desktop.upper()}_ROOT_NOT_BOOT_VERIFIED'


def plan(kernel, source, output, runtime_dir=None, kernel_build=None,
         public_source=None, firmware_source=None, root=ROOT, resume=False,
         sensors_dir=None, ffmpeg_dir=None, boot_task_snapshot=False, desktop='gnome', mutter_dir=None,
         packages_only=None, published_packages=None, denial_packages=None):
    root = Path(root).resolve(); config = json.loads((root / 'config/release.json').read_text())
    if desktop not in DESKTOPS:
        raise ValueError('Release desktop must be gnome, kde or denial')
    config['debian']['extra_packages'] = desktop_packages(config['debian']['extra_packages'], desktop)
    if config['debian']['apt_policy'] != 'mutable-recorded' or config['debian']['snapshot'] is not None:
        raise ValueError('Only signed mutable APT with recorded metadata is implemented; snapshot mode is not configured')
    if config['debian']['suite'] != 'sid':
        raise ValueError('The Debian product builds only sid')
    kernel, source, output = [Path(p).resolve() for p in (kernel, source, output)]
    if packages_only:
        if desktop == 'denial':
            raise ValueError('Use --desktop gnome --packages-only to build the shared Debian overlays')
        package_output = Path(packages_only).resolve()
        if (package_output.exists() or not package_output.is_relative_to(root / 'artifacts/packages') or
                package_output == root / 'artifacts/packages'):
            raise ValueError('Use a new project artifacts/packages directory for the overlay packages')
        if output == root / 'build/distros/release':
            output = root / 'build/distros/overlays' / ('debian-' + desktop)
        if output == root / 'build/distros/overlays' or not output.is_relative_to(root / 'build/distros/overlays'):
            raise ValueError('Use a separate build/distros/overlays directory for package builds')
    if published_packages and (sensors_dir or ffmpeg_dir or mutter_dir or packages_only):
        raise ValueError('Published packages cannot be combined with local package selection')
    denial_packages = image_manifest(denial_packages, 'debian-denial', desktop=desktop,
                                     published=published_packages is not None)
    if published_packages:
        published_packages = Path(published_packages).resolve()
        overlay_packages.read_debian(published_packages, desktop)
    if not output.is_relative_to(root / 'build/distros') or output.is_symlink() or (output.exists() and not resume):
        raise ValueError('Output must be a new project build/distros directory')
    previous = None
    if resume:
        if not (output / 'COMPLETE').is_file():
            raise ValueError('Resume needs a completed upstream base root')
        base_manifest = output / 'build-manifest.txt'
        if packages_only:
            if (not base_manifest.is_file() or
                    base_manifest.read_text().splitlines()[:1] != [package_build_manifest(desktop)]):
                raise ValueError('Resume needs a completed sid package build root for the selected desktop')
        elif (output / 'manifest.json').is_file():
            previous = json.loads((output / 'manifest.json').read_text())
            if (previous.get('status') not in {image_status(name) for name in DESKTOPS} or
                    previous.get('debian_commit') != config['debian']['commit']):
                raise ValueError('Completed base root belongs to another source')
            if previous.get('desktop', 'gnome') != desktop:
                raise ValueError('Resume must keep the desktop used to build this root')
            if previous.get('debian_suite') != config['debian']['suite']:
                raise ValueError('Resume must keep the Debian suite; build a fresh sid base')
        else:
            expected = f'suite=sid arch=arm64 desktop={desktop} firmware=not-included'
            if not base_manifest.is_file() or base_manifest.read_text().splitlines()[:1] != [expected]:
                raise ValueError('Resume needs a completed sid base for the selected desktop')
            failure = output / 'FAILED.json'
            if failure.is_file() and json.loads(failure.read_text()).get('desktop', 'gnome') != desktop:
                raise ValueError('Failed base root belongs to another desktop')
    if not kernel.is_relative_to(root / 'artifacts/kernels') or not source.is_relative_to(root / 'build/kernel-worktrees'):
        raise ValueError('Use project release kernel source and artifacts')
    public = Path(public_source or root / config['debian']['source']).resolve()
    firmware = Path(firmware_source or root / config['firmware']['components']).resolve()
    missing = []
    inputs = [('kernel manifest', kernel / 'manifest.json'), ('kernel source', source / 'Makefile')]
    if not packages_only:
        inputs.append(('public rootfs builder', public / 'scripts/build-rootfs.sh'))
        inputs.append(('firmware', next(iter(sorted(firmware.glob('*/manifest.json'))), firmware / 'missing')))
    for name, path in inputs:
        if not path.is_file(): missing.append(name)
    sensors = Path(sensors_dir).resolve() if sensors_dir else None
    sensor_source = root / config['sensors']['source']
    if not (sensor_source / 'scripts/build-sensors-debs.sh').is_file(): missing.append('fixed Piano sensors source')
    if sensor_source.is_dir(): repository(sensor_source, config['sensors']['commit'])
    if sensors is not None:
        if not list(sensors.glob('*.deb')):
            missing.append('Piano sensors runtime .deb directory (--sensors-dir)')
        else:
            sensors_packages(sensors, config['sensors'], root=root)
    ffmpeg = Path(ffmpeg_dir).resolve() if ffmpeg_dir else None
    if ffmpeg:
        ffmpeg_packages(ffmpeg)
    mutter = Path(mutter_dir).resolve() if mutter_dir else None
    if mutter:
        if desktop != 'gnome':
            raise ValueError('--mutter-dir is only valid for the GNOME desktop')
        mutter_packages(mutter)
    if runtime_dir:
        if not (Path(runtime_dir) / 'manifest.json').is_file(): missing.append('matching runtime bundle manifest')
    if not packages_only and not published_packages and not (root / 'upstream/audioreach-topology/audioreach').exists():
        missing.append('AudioReach topology macros')
    record = None
    if (kernel / 'manifest.json').is_file():
        record, _ = modules.inspect(kernel)
        head = repository(source, tree=release_series(root, config['kernel'])['target_tree'])
        if record['source_commit'] != head or record.get('source_clean') is not True:
            raise ValueError('Kernel manifest does not match the prepared release source')
        words = record['command_line'].split()
        if record.get('root_policy') != 'LABEL=PIANOROOT' or [w for w in words if w.startswith('piano.root=')] != ['piano.root=LABEL=PIANOROOT'] or any(w.startswith('root=') for w in words) or 'userdata' in record['command_line']:
            raise ValueError('Release kernel must select LABEL=PIANOROOT, never Android userdata')
    if not packages_only and public.is_dir(): repository(public, config['debian']['commit'])
    if packages_only:
        steps = ['prepare a minimal signed Debian sid ARM64 package build root',
                 'build and verify the selected native desktop and hardware packages',
                 'export selected packages and SHA256 manifest without assembling a product image']
    else:
        steps = ['derive build-rootfs.sh and install the base desktop packages from signed sid APT',
                 'install the matching native kernel packages',
                 ('install exact desktop and camera packages from the signed SunUEFI APT source'
                  if published_packages
                  else 'build overlays in a separate signed Debian sid root and install the native packages')]
        if desktop == 'denial':
            steps.append('install exact Denial packages from the signed SunUEFI and Denial APT sources'
                         if denial_packages else
                         'install Denial from its signed APT source and enable its mobile session')
        steps.append('stage the hardware runtime, apply product defaults and build the initial initramfs')
    return {'status': 'PLAN_ONLY', 'resume': resume, 'desktop': desktop,
            'previous_kernel_release': previous.get('kernel_release') if previous else None,
            'root_policy': 'LABEL=PIANOROOT', 'kernel': str(kernel), 'source': str(source),
            'output': str(output), 'rootfs': str(output / 'rootfs'),
            'sensors_dir': str(sensors) if sensors else None,
            'ffmpeg_dir': str(ffmpeg) if ffmpeg else None,
            'mutter_dir': str(mutter) if mutter else None,
            'boot_task_snapshot': boot_task_snapshot,
            'packages_only': str(package_output) if packages_only else None,
            'published_packages': str(published_packages) if published_packages else None,
            'denial_packages': str(denial_packages) if denial_packages else None,
            'runtime_dir': str(Path(runtime_dir).resolve()) if runtime_dir else None,
            'kernel_build': str(Path(kernel_build or root / 'build/kernels/release').resolve()),
            'public_source': str(public), 'firmware_source': str(firmware), 'config': config,
            'missing_inputs': missing, 'steps': steps}


def require_host():
    if os.geteuid(): raise ValueError('--execute requires root; debootstrap/chroot/mount are real build dependencies')
    caps = next(line.split()[1] for line in Path('/proc/self/status').read_text().splitlines() if line.startswith('CapEff:'))
    if int(caps, 16) & ((1 << 21) | (1 << 18)) != ((1 << 21) | (1 << 18)):
        raise ValueError('Root runner also needs CAP_SYS_ADMIN and CAP_SYS_CHROOT')
    required = ['debootstrap', 'chroot', 'curl', 'ssh-keygen', 'openssl', 'mount', 'umount', 'unshare', 'sha256sum', 'dpkg-deb', 'gpgv']
    if platform.machine() not in ('aarch64', 'arm64'):
        required.append('qemu-aarch64-static')
        entry = Path('/proc/sys/fs/binfmt_misc/qemu-aarch64')
        if not entry.is_file() or 'enabled' not in entry.read_text() or 'F' not in entry.read_text().split('flags:')[-1].splitlines()[0]:
            raise ValueError('x86 runner needs enabled qemu-aarch64 binfmt with the F flag')
    absent = [name for name in required if not shutil.which(name)]
    if absent: raise ValueError('Missing build dependencies: ' + ', '.join(absent))


def own_root(rootfs, output):
    rootfs, output = Path(rootfs).resolve(), Path(output).resolve()
    if not output.is_relative_to(ROOT / 'build/distros') or rootfs != output / 'rootfs' or not rootfs.is_dir():
        raise ValueError('Modify only this new release build rootfs')
    return rootfs, output


def target(root, name):
    path = root / name
    if not path.is_relative_to(root) or '..' in Path(name).parts: raise ValueError('Guest path escapes root')
    for parent in path.parents:
        if parent == root: break
        if parent.is_symlink(): raise ValueError('Guest path traverses a symlink')
    return path


def put(root, name, text):
    path = target(root, name)
    if path.is_symlink(): path.unlink()
    path.parent.mkdir(parents=True, exist_ok=True); path.write_text(text)


def select_wireless_regdb(rootfs, run):
    # The shared kernel trusts the upstream signature, not Debian's.
    run(['chroot', rootfs, 'update-alternatives', '--set', 'regulatory.db', REGDB_PATH + '-upstream'])


def wireless_regdb_record(rootfs):
    files = {}
    for suffix in ('', '.p7s'):
        name = (REGDB_PATH + suffix).lstrip('/')
        selected = userspace.guest_resolve(rootfs, name)
        upstream = userspace.guest_resolve(rootfs, name + '-upstream')
        if selected != upstream or not selected.is_file() or not selected.stat().st_size:
            raise ValueError('Debian wireless-regdb upstream alternative is not installed: ' + name)
        files[name] = digest(selected)
    return files


def default_system_record(rootfs, installed, config, desktop='gnome'):
    """Require the shipped desktop and BSP, including runtime-bundle resumes."""
    required = {'systemd-sysv', 'pipewire', 'network-manager', 'wireless-regdb', POWER_PROFILES_PACKAGE,
                'tuned', 'tuned-ppd'}
    required.update({
        'gnome': ('gnome-shell', 'gdm3'),
        'kde': ('plasma-workspace', 'sddm', 'powerdevil'),
        'denial': ('denial', 'denial-flutter-engine', 'sddm'),
    }[desktop])
    required.update(config['debian']['extra_packages'])
    missing = sorted(required - installed.keys())
    if missing:
        raise ValueError('Required desktop packages are not installed: ' + ', '.join(missing))

    paths = (
        'etc/modules-load.d/piano-bluetooth.conf',
        'etc/udev/rules.d/79-piano-touch.rules',
        'usr/share/piano/pen/p81c-radio.json', 'usr/lib/piano/pen-control.py',
        'usr/bin/piano-pen-canvas',
    )
    paths += {
        'gnome': ('etc/dconf/db/local.d/00-sunuefi', 'etc/dconf/db/local', 'etc/dconf/profile/user'),
        'kde': ('etc/sddm.conf.d/10-sunuefi.conf',
                'usr/share/plasma/shells/org.kde.plasma.desktop/metadata.json'),
        'denial': ('etc/sddm.conf.d/10-sunuefi.conf', 'etc/denial/session.conf',
                   'usr/share/wayland-sessions/denial.desktop', 'usr/bin/denial-session'),
    }[desktop]
    files = wireless_regdb_record(rootfs)
    for name in paths:
        path = userspace.guest_resolve(rootfs, name)
        if not path.is_file() or not path.stat().st_size:
            raise ValueError('Required desktop/BSP file is missing: ' + name)
        files[name] = digest(path)
    defaults = ROOT / 'linux/desktops/gnome/defaults.ini'
    if desktop == 'gnome' and files['etc/dconf/db/local.d/00-sunuefi'] != digest(defaults):
        raise ValueError('Packaged GNOME defaults differ from the desktop source')
    if desktop == 'gnome' and 'system-db:local' not in (rootfs / 'etc/dconf/profile/user').read_text().splitlines():
        raise ValueError('GNOME dconf profile does not load the packaged defaults')
    return {'packages': {name: installed[name] for name in sorted(required)},
            'files': files, 'desktop': desktop}


def clock_epoch(rootfs):
    """Give systemd a build-time clock floor before RTC or NTP is available."""
    name = 'usr/lib/clock-epoch'
    put(rootfs, name, '')
    path = target(rootfs, name)
    path.chmod(0o644)
    timestamp = int(time.time())
    os.utime(path, (timestamp, timestamp))
    return {'path': '/' + name, 'mtime': timestamp,
            'utc': datetime.fromtimestamp(timestamp, timezone.utc).isoformat()}


@contextmanager
def build_resolver(rootfs):
    # NetworkManager is not running inside the new build chroot yet.
    path = target(rootfs, 'etc/resolv.conf')
    link = path.readlink() if path.is_symlink() else None
    original = path.read_bytes() if path.is_file() and link is None else None
    mode = path.stat().st_mode & 0o777 if original is not None else 0o644
    path.unlink(missing_ok=True)
    path.write_bytes(Path('/etc/resolv.conf').read_bytes())
    try:
        yield
    finally:
        path.unlink(missing_ok=True)
        if link is not None: path.symlink_to(link)
        elif original is not None:
            path.write_bytes(original); path.chmod(mode)


@contextmanager
def build_devices(rootfs):
    """Provide chroot nodes and procfs without exposing host device or sysfs trees."""
    created = []
    proc = target(rootfs, 'proc')
    mounted_proc = False
    try:
        if proc.is_symlink():
            raise ValueError('Build procfs destination must not be a symlink')
        proc.mkdir(exist_ok=True)
        if not os.path.ismount(proc):
            subprocess.run(['mount', '-t', 'proc', '-o', 'nosuid,nodev,noexec', 'proc', proc], check=True)
            mounted_proc = True
        for name, major, minor in (('null', 1, 3), ('zero', 1, 5), ('random', 1, 8), ('urandom', 1, 9)):
            path = target(rootfs, 'dev/' + name)
            if not path.exists():
                os.mknod(path, stat.S_IFCHR | 0o666, os.makedev(major, minor))
                created.append(path)
        yield
    finally:
        if mounted_proc:
            subprocess.run(['umount', proc], check=True)
        for path in created:
            path.unlink(missing_ok=True)


def prepare_package_build_root(output, desktop, run):
    from desktop.debian_package import KEYRING, prepare_build_indexes

    rootfs = output / 'rootfs'
    output.mkdir()
    run(['debootstrap', '--arch=arm64', '--variant=minbase', '--components=main',
         '--include=ca-certificates', '--force-check-sig', '--keyring=' + KEYRING,
         '--foreign', 'sid', rootfs, DEBIAN_MIRROR])
    run(['chroot', rootfs, '/debootstrap/debootstrap', '--second-stage'])
    put(rootfs, 'etc/apt/sources.list',
        f'deb [signed-by={KEYRING}] {DEBIAN_MIRROR} sid main contrib\n'
        f'deb-src [signed-by={KEYRING}] {DEBIAN_MIRROR} sid main contrib\n')
    put(rootfs, 'usr/sbin/policy-rc.d', '#!/bin/sh\nexit 101\n')
    target(rootfs, 'usr/sbin/policy-rc.d').chmod(0o755)
    with build_devices(rootfs), build_resolver(rootfs):
        def guest(args):
            return run(['chroot', rootfs, 'env', 'DEBIAN_FRONTEND=noninteractive', *args])

        prepare_build_indexes(rootfs, guest)
    (output / 'build-manifest.txt').write_text(package_build_manifest(desktop) + '\n')
    (output / 'COMPLETE').touch()


@contextmanager
def runtime_build_sysroot(rootfs, output, run):
    """Install helper build dependencies without adding them to the image."""
    staging = output / 'runtime-sysroot'
    staging.mkdir()
    for name in ('upper', 'work', 'root'):
        (staging / name).mkdir()
    sysroot = staging / 'root'
    options = f'lowerdir={rootfs},upperdir={staging / "upper"},workdir={staging / "work"}'
    run(['mount', '-t', 'overlay', 'overlay', '-o', options, sysroot])
    try:
        with build_devices(sysroot), build_resolver(sysroot):
            run(['chroot', sysroot, 'env', 'DEBIAN_FRONTEND=noninteractive',
                 'apt-get', 'install', '-y', '--no-install-recommends',
                  'build-essential', 'libc6-dev', 'libyuv-dev', 'libjpeg62-turbo-dev'])
            yield sysroot
    finally:
        run(['umount', sysroot])
        shutil.rmtree(staging)


def apply_policy(rootfs, output, config, *, pacman=False):
    rootfs, _ = own_root(rootfs, output)
    storage = config['storage']
    if (storage['root_label'], storage['root_partname'], storage['esp_label'], storage['root_fstype'], storage['esp_partname']) != ('PIANOROOT', 'sunuefi_root', 'SUNUEFI_ESP', 'ext4', 'sunuefi_esp'):
        raise ValueError('Unknown release storage identities')
    if set(storage['masked_units']) != {'piano-swapfile.service', 'qbootctl.service'}:
        raise ValueError('Only the two storage-policy units may be masked')
    put(rootfs, 'etc/fstab', product_fstab(pacman=pacman))
    # The shared ESP starts with the same entry token on all product images.
    put(rootfs, 'etc/kernel/entry-token', 'linux\n')
    # This runs only while assembling a new image, never on an installed root.
    localtime = target(rootfs, 'etc/localtime')
    localtime.unlink(missing_ok=True)
    localtime.symlink_to('/usr/share/zoneinfo/Asia/Shanghai')
    put(rootfs, 'etc/timezone', 'Asia/Shanghai\n')
    # The RAM overlay masks growfs; the dedicated disk root uses systemd's native service.
    growfs_mask = target(rootfs, 'etc/systemd/system/systemd-growfs-root.service')
    if growfs_mask.is_symlink() and growfs_mask.readlink() == Path('/dev/null'):
        growfs_mask.unlink()
    put(rootfs, 'etc/piano/root-policy.json', json.dumps({'version': 1, 'mode': 'label', 'root_label': 'PIANOROOT',
        'root_partlabel': 'sunuefi_root', 'esp_label': 'SUNUEFI_ESP'}, indent=2) + '\n')
    put(rootfs, 'etc/udev/rules.d/01-piano-protect-android.rules', '''SUBSYSTEM!="block", GOTO="release_end"
KERNELS!="1d84000.*", GOTO="release_end"
ENV{UDISKS_IGNORE}="1"
ENV{DEVTYPE}!="partition", GOTO="release_end"
IMPORT{builtin}="blkid"
ENV{ID_FS_LABEL}=="PIANOROOT", ENV{ID_PART_ENTRY_NAME}=="sunuefi_root", ENV{ID_FS_TYPE}=="ext4", GOTO="release_end"
ENV{ID_FS_LABEL}=="SUNUEFI_ESP", ENV{ID_PART_ENTRY_NAME}=="sunuefi_esp", ENV{ID_FS_TYPE}=="vfat", GOTO="release_end"
ATTR{ro}=="1", GOTO="release_end"
RUN+="/usr/sbin/blockdev --setro /dev/%k"
LABEL="release_end"
''')
    for unit in storage['masked_units']:
        path = target(rootfs, Path('etc/systemd/system') / unit)
        if path.exists() or path.is_symlink(): path.unlink()
        path.parent.mkdir(parents=True, exist_ok=True); path.symlink_to('/dev/null')
    debug = target(rootfs, Path('usr/local/sbin/piano-debug-bootstrap'))
    if debug.is_symlink(): debug.unlink()
    debug.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / 'linux/userspace/piano-debug-bootstrap', debug)
    put(rootfs, 'usr/local/sbin/piano-boot-request', (ROOT / 'linux/userspace/piano-boot-request').read_text())
    target(rootfs, 'usr/local/sbin/piano-boot-request').chmod(0o755)


def sanitize(rootfs, output):
    rootfs, output = own_root(rootfs, output)
    # These are populated by the running kernel after switch_root.
    for name in ('dev', 'proc', 'sys', 'run'):
        directory = target(rootfs, name)
        for child in directory.iterdir():
            if child.is_dir() and not child.is_symlink(): shutil.rmtree(child)
            else: child.unlink()
    for relative in ('root/.ssh/authorized_keys', 'home/piano/.ssh/authorized_keys'):
        path = target(rootfs, relative)
        if path.exists() or path.is_symlink(): path.unlink()
    for path in target(rootfs, 'etc/ssh/placeholder').parent.glob('ssh_host_*'):
        if path.is_file() or path.is_symlink(): path.unlink()
    for name in ('access-key', 'access-key.pub', 'login.txt'):
        path = output / name
        if path.exists() or path.is_symlink(): path.unlink()
    put(rootfs, 'etc/machine-id', '')
    path = target(rootfs, Path('var/lib/dbus/machine-id'))
    if path.exists() or path.is_symlink(): path.unlink()
    path.parent.mkdir(parents=True, exist_ok=True); path.symlink_to('/etc/machine-id')
    login = json.loads(POLICY.read_text())['login']
    subprocess.run(['chroot', str(rootfs), '/usr/sbin/chpasswd'],
                   input=login['user'] + ':' + login['password'] + '\n',
                   text=True, check=True)
    for name in ('etc/shadow-', 'etc/gshadow-'):
        backup = target(rootfs, name)
        if backup.exists() or backup.is_symlink(): backup.unlink()


def native_boot_request(rootfs, output):
    # Compile inside the same ARM64 builder/root ABI used for the release.
    import build_boot_request
    folder = output / 'native-boot-request'
    compiler = os.environ.get('SUNUEFI_BOOT_REQUEST_CC') or (
        'aarch64-linux-gnu-gcc' if platform.machine() not in ('aarch64', 'arm64')
        and shutil.which('aarch64-linux-gnu-gcc') else 'clang')
    record = build_boot_request.build(folder, 'aarch64', compiler, rootfs)
    binary = target(rootfs, 'usr/local/sbin/piano-boot-request')
    shutil.copy2(folder / 'piano-boot-request', binary)
    binary.chmod(0o755)
    installed = {}
    for name, destination, mode in (
        ('piano-next-boot', 'usr/bin/piano-next-boot', 0o755),
        ('piano-next-boot-helper', 'usr/libexec/piano-next-boot-helper', 0o755),
        ('org.sunuefi.boot-request.policy', 'usr/share/polkit-1/actions/org.sunuefi.boot-request.policy', 0o644),
        ('49-piano-boot-request.rules', 'usr/share/polkit-1/rules.d/49-piano-boot-request.rules', 0o644),
        ('org.sunuefi.ReturnAndroid.desktop', 'usr/share/applications/org.sunuefi.ReturnAndroid.desktop', 0o644),
        ('org.sunuefi.BootMenu.desktop', 'usr/share/applications/org.sunuefi.BootMenu.desktop', 0o644),
    ):
        source = ROOT / 'linux/userspace' / name
        path = userspace.guest_resolve(rootfs, destination)
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, path)
        path.chmod(mode)
        installed[destination] = digest(source)
    record['next_boot_tools'] = installed
    # Remove only obsolete project-owned configuration from reused roots.
    for name in ('etc/piano/boot-request.json', 'usr/sbin/piano-next-boot-configure',
                 'usr/share/doc/piano-next-boot/boot-request.example.json'):
        userspace.guest_resolve(rootfs, name).unlink(missing_ok=True)
    record['boot_device_detection'] = 'read-only Qualcomm GPT A/B attributes'
    return record


def runtime_packages(folder, names, version=None):
    folder = Path(folder).resolve()
    metadata = folder.parent / 'SOURCE.json'
    if not metadata.is_file() or (folder.parent / '.incomplete').exists():
        raise ValueError('Use a completed runtime package directory: ' + str(folder))
    source = json.loads(metadata.read_text())
    rows = {}
    for path in sorted(folder.glob('*.deb')):
        package, actual_version, arch = [capture(['dpkg-deb', '-f', path, field])
                                         for field in ('Package', 'Version', 'Architecture')]
        row = {'file': path.name, 'version': actual_version, 'architecture': arch, 'sha256': digest(path)}
        if (package in rows or arch not in ('arm64', 'all') or
                (version is not None and actual_version != version) or
                source.get('packages', {}).get(package) != row):
            raise ValueError('Runtime package identity differs: ' + path.name)
        rows[package] = row
    if set(rows) != set(names):
        raise ValueError('The complete runtime package set is required: ' + str(folder))
    return source, rows


def mesa_packages(folder, rootfs):
    from build import mesa as builder
    folder = Path(folder).resolve()
    source, rows = runtime_packages(folder, builder.RUNTIME, builder.package_version())
    if builder.reusable(folder.parent, builder.target_identity(rootfs)) is None:
        raise ValueError('Mesa runtime does not match the official source, native recipe or target ABI')
    return {'source_sha256': digest(folder.parent / 'SOURCE.json'),
            'official_source': source['official_source'], 'patches': source['patches'],
            'packages': rows, 'device_verified': False}


def sensors_packages(folder, config, root=ROOT, rootfs=None):
    folder = Path(folder).resolve()
    metadata = next((p for p in (folder.parent, folder)
                     if (p / 'SOURCE').is_file() and (p / 'SHA256SUMS').is_file()), None)
    if metadata is None:
        raise ValueError('Piano sensors packages need SOURCE and SHA256SUMS beside or above the runtime directory')
    source = json.loads((metadata / 'SOURCE').read_text())
    if (source.get('source_url'), source.get('source_commit')) != (config['source_url'], config['commit']):
        raise ValueError('Piano sensors SOURCE does not match the fixed release source')
    patches = [{'path': name, 'sha256': digest(Path(root) / name)} for name in config.get('patches', [])]
    from build_piano_runtime_helpers import source_patches
    series = Path(root) / 'patches/piano-sensors/5/series.json'
    ordered = [path.resolve().relative_to(Path(root).resolve()).as_posix()
               for path in source_patches(series)]
    if ordered != [row['path'] for row in patches]:
        raise ValueError('Sensors configuration differs from the selected patch series')
    if source.get('patches', []) != patches:
        raise ValueError('Piano sensors SOURCE does not match the release importer patches; rebuild with ./build.sh sensors')
    sums = {}
    for line in (metadata / 'SHA256SUMS').read_text().splitlines():
        if not line.strip(): continue
        checksum, name = line.split(maxsplit=1)
        name = str(Path(name.lstrip('*')))
        if len(checksum) != 64 or any(c not in '0123456789abcdef' for c in checksum.lower()) or name in sums:
            raise ValueError('Invalid or duplicate Piano sensors SHA256SUMS entry')
        sums[name] = checksum.lower()
    rows = {}
    for path in sorted(folder.glob('*.deb')):
        actual = digest(path)
        if sums.get(str(path.relative_to(metadata))) != actual:
            raise ValueError('Piano sensors package checksum mismatch: ' + path.name)
        package, version, arch = [capture(['dpkg-deb', '-f', path, field]) for field in ('Package', 'Version', 'Architecture')]
        if arch not in ('arm64', 'all') or package in rows:
            raise ValueError('Expected unique ARM64 Piano sensors runtime packages')
        if package in {'libssc2', 'libssc-bin', 'iio-sensor-proxy'} and '+piano' not in version:
            raise ValueError('Piano sensors needs the SSC-enabled Piano package versions')
        rows[package] = {'file': path.name, 'version': version, 'architecture': arch, 'sha256': actual}
    if set(rows) != {'piano-sensors', 'fastrpc-support', 'libfastrpc1', 'libssc2', 'libssc-bin', 'iio-sensor-proxy'}:
        raise ValueError('Expected exactly the six Piano sensors runtime packages')
    from build import debian, sensors as builder
    policy = json.loads((Path(root) / builder.POLICY.relative_to(ROOT)).read_text())
    versions = policy['source_versions']
    expected = builder.package_versions(policy)
    if any(rows[package]['version'] != version for package, version in expected.items()):
        raise ValueError('Sensors runtime versions differ from the sid package policy')
    target = debian.target_identity(rootfs, versions, {'libssc': 'libssc2',
                                    'iio-sensor-proxy': 'iio-sensor-proxy'}) if rootfs else None
    if (source.get('target', {}).get('sources') != versions or
            source.get('policy_sha256') != digest(Path(root) / builder.POLICY.relative_to(ROOT)) or
            debian.reusable(metadata, expected['piano-sensors'], patches, builder.RUNTIME, target) is None):
        raise ValueError('Sensors runtime does not match the sid source, patches or target ABI')
    return {'source_url': source['source_url'], 'source_commit': source['source_commit'],
            'source_sha256': digest(metadata / 'SOURCE'), 'sha256sums_sha256': digest(metadata / 'SHA256SUMS'),
            'patches': patches, 'packages': rows}


def ffmpeg_packages(folder, rootfs=None):
    import build_ffmpeg_packages as builder
    from build import debian
    folder = Path(folder).resolve()
    metadata = folder.parent / 'SOURCE.json'
    version = builder.package_version()
    patches = builder.patch_records()
    source, rows = runtime_packages(folder, builder.RUNTIME, version)
    selected = {'ffmpeg': json.loads(builder.POLICY.read_text())['source_version']}
    target = debian.target_identity(rootfs, selected, {'ffmpeg': 'ffmpeg'}) if rootfs else None
    if (source.get('target', {}).get('sources') != selected or
            builder.reusable(folder.parent, target) is None):
        raise ValueError('FFmpeg runtime does not match the sid source, patches or target ABI')
    return {'source_sha256': digest(metadata), 'patches': patches, 'packages': rows,
            'device_verified': False}


def gsd_packages(folder, rootfs=None):
    """Select the standard GNOME daemon packages with the device ALS policy."""
    folder = Path(folder).resolve()
    metadata = folder.parent / 'SOURCE.json'
    source = json.loads(metadata.read_text())
    import build_gsd_packages as builder
    patches = builder.patch_records()
    if rootfs is not None and builder.reusable(folder.parent, rootfs) is None:
        raise ValueError('GNOME power packages do not match the sid target ABI or source')
    if source.get('packages_built') is not True or source.get('patches') != patches:
        raise ValueError('GNOME power packages do not match the current ambient policy; rebuild with ./build.sh gsd')
    rows = {}
    for package_name, expected in sorted(source.get('packages', {}).items()):
        if Path(expected['file']).name != expected['file']:
            raise ValueError('Invalid GNOME power package filename')
        path = folder / expected['file']
        package, version, arch = [capture(['dpkg-deb', '-f', path, field])
                                   for field in ('Package', 'Version', 'Architecture')]
        row = {'file': path.name, 'version': version, 'architecture': arch, 'sha256': digest(path)}
        if (package != package_name or package in rows or arch not in ('arm64', 'all') or
                version != source.get('package_version') or '+sunuefi' not in version or
                source.get('packages', {}).get(package) != row):
            raise ValueError('GNOME power runtime package identity differs: ' + path.name)
        rows[package] = row
    if set(rows) != {'gnome-settings-daemon', 'gnome-settings-daemon-common'}:
        raise ValueError('Both standard GNOME settings daemon packages are required')
    return {'source_sha256': digest(metadata), 'patches': patches, 'packages': rows,
            'device_verified': False}


def mutter_packages(folder, rootfs=None):
    """Use the standard Mutter packages carrying the two desktop fixes."""
    import build_mutter_packages as builder
    folder = Path(folder).resolve()
    metadata = folder.parent / 'SOURCE.json'
    if (folder.parent / '.incomplete').exists():
        raise ValueError('Mutter package build has not completed')
    source = json.loads(metadata.read_text())
    if rootfs is not None and builder.reusable(folder.parent, rootfs) is None:
        raise ValueError('Mutter packages do not match the sid target ABI or source')
    if (source.get('packages_built') is not True or
            source.get('patches') != builder.patch_records() or
            source.get('source', {}).get('package') != 'mutter' or
            source.get('source', {}).get('suite') != 'sid' or
            source.get('package_version') != overlay_release.debian_version(
                source.get('source', {}).get('version', '') + '+sunuefi' + str(builder.REVISION))):
        raise ValueError('Mutter packages do not match the desktop patches; rebuild with ./build.sh mutter')
    rows = {}
    for path in sorted(folder.glob('*.deb')):
        package, version, arch = [capture(['dpkg-deb', '-f', path, field])
                                  for field in ('Package', 'Version', 'Architecture')]
        row = {'file': path.name, 'version': version, 'architecture': arch, 'sha256': digest(path)}
        if (package in rows or builder.RUNTIME.get(package) != arch or
                version != source['package_version'] or
                source.get('packages', {}).get(package) != row):
            raise ValueError('Mutter runtime package identity differs: ' + path.name)
        rows[package] = row
    if set(rows) != set(builder.RUNTIME):
        raise ValueError('The complete standard Mutter runtime package set is required')
    return {'source_sha256': digest(metadata), 'patches': source['patches'],
            'packages': rows, 'device_verified': False}


def bootstrap_tools():
    """Extract fixed tools without installing them into any target rootfs."""
    from prepare_steamos_rootfs import download
    packages = json.loads(POLICY.read_text())['bootstrap_packages']
    folder = ROOT / 'build/source-cache/bootstrap'
    folder.mkdir(parents=True, exist_ok=True)
    with (folder / 'download.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        archives = {}
        for name, row in packages.items():
            archive = folder / (row['sha256'] + '.deb')
            download(row['url'], archive, row['sha256'])
            archives[name] = archive
    return packages, archives


def bootstrap(kernel, output, release, boot_task_snapshot=False):
    packages, archives = bootstrap_tools()
    # The target distro must not determine the shared initramfs contents.
    with tempfile.TemporaryDirectory(prefix='bootstrap-', dir=ROOT / 'build') as temporary:
        rootfs = Path(temporary)
        for archive in archives.values():
            subprocess.run(['dpkg-deb', '-x', archive, rootfs], check=True)
        for name in ('bin', 'sbin', 'lib'):
            if not (rootfs / name).exists():
                (rootfs / name).symlink_to('usr/' + name)
        return pack_bootstrap(rootfs, packages, archives, kernel, output, release, boot_task_snapshot)


def pack_bootstrap(rootfs, packages, archives, kernel, output, release, boot_task_snapshot):
    busybox = disk.guest_resolve(rootfs, '/usr/bin/busybox')
    validate_static_arm64_elf(busybox.read_bytes())
    applets = set(capture(['chroot', rootfs, '/usr/bin/busybox', '--list']).splitlines())
    # Debian's static build omits mountpoint; use its util-linux executable
    # with the same ELF dependency closure already needed by blkid.
    bootstrap_applets = tuple(name for name in disk.APPLETS if name != 'mountpoint')
    if missing := set(bootstrap_applets) - applets:
        raise ValueError('Static BusyBox is missing bootstrap applets: ' + ', '.join(sorted(missing)))
    package = [capture(['dpkg-deb', '-f', archives['busybox-static'], field])
               for field in ('Package', 'Version', 'Architecture')]
    if package[0] != 'busybox-static' or package[2] != 'arm64':
        raise ValueError('Release bootstrap requires an ARM64 busybox-static package')
    busybox_source = output / 'initramfs/busybox-source.json'
    busybox_source.parent.mkdir(exist_ok=True)
    busybox_record = {'package': package[0], 'version': package[1],
        'architecture': package[2], 'sha256': digest(busybox),
        'source': packages['busybox-static']['url'], 'license': 'GPL-2.0-only'}
    busybox_source.write_text(json.dumps(busybox_record, indent=2) + '\n')
    files = disk.runtime_files(rootfs, ('/usr/sbin/blkid', '/usr/bin/mountpoint'))
    directory = Path(kernel) / 'modules/lib/modules' / release
    ordered, _ = disk.module_closure(directory)
    rescue, _ = disk.module_closure(directory, disk.SEEDS + disk.RESCUE_SEEDS)
    files.update({'bin/busybox': busybox, 'pianoinit': ROOT / 'linux/userspace/disk-bootstrap',
                  'usr/local/sbin/piano-debug-bootstrap': ROOT / 'linux/userspace/piano-debug-bootstrap'})
    files['init'] = files['pianoinit']
    for name in rescue: files[f'lib/modules/{release}/{name}'] = directory / name
    from stage_piano_firmware import components
    keyboard = [(manifest, data) for _, manifest, data in components()
                if data['component'] == 'piano-keyboard']
    if len(keyboard) != 1:
        raise ValueError('The shared bootstrap needs one keyboard firmware component')
    manifest, data = keyboard[0]
    firmware = manifest.parent / 'MCU_Upgrade.bin'
    if digest(firmware) != data['files']['MCU_Upgrade.bin']['sha256']:
        raise ValueError('The shared keyboard firmware checksum differs')
    files['lib/firmware/nanosic/MCU_Upgrade.bin'] = firmware
    generated = {'etc/piano/root-label': 'PIANOROOT\n', 'etc/piano/root-partname': 'sunuefi_root\n',
                 'etc/piano/busybox-source.json': busybox_source.read_text(),
                 'etc/piano/kernel-release': release + '\n',
                 'etc/piano/linux-debug.conf': 'usb=acm-ncm\nshell=1\nrecovery_seconds=0\n',
                 'etc/piano/modules-load-order': ''.join(f'{disk.module_name(p)} /lib/modules/{release}/{p}\n' for p in ordered),
                 'etc/piano/modules-rescue-order': ''.join(f'{disk.module_name(p)} /lib/modules/{release}/{p}\n' for p in rescue)}
    if boot_task_snapshot:
        generated['etc/piano/boot-task-snapshot'] = 'once\n'
    dirs = {'dev', 'proc', 'sys', 'run', 'tmp', 'sysroot'}
    for name in files.keys() | generated.keys(): dirs.update(p.as_posix() for p in Path(name).parents if p.as_posix() != '.')
    rows = [{'name': n, 'mode': stat.S_IFDIR | (0o1777 if n == 'tmp' else 0o755)} for n in sorted(dirs)]
    rows += [{'name': n, 'mode': stat.S_IFREG | 0o755, 'data': p.read_bytes()} for n, p in sorted(files.items())]
    rows += [{'name': n, 'mode': stat.S_IFREG | 0o644, 'data': v.encode()} for n, v in generated.items()]
    rows += [{'name': 'bin/' + n, 'mode': stat.S_IFLNK | 0o777, 'data': b'busybox'} for n in bootstrap_applets]
    rows += [{'name': 'dev/console', 'mode': stat.S_IFCHR | 0o600, 'major': 5, 'minor': 1},
             {'name': 'dev/null', 'mode': stat.S_IFCHR | 0o666, 'major': 1, 'minor': 3}]
    target = output / 'initramfs/initramfs.cpio.gz'; target.parent.mkdir(exist_ok=True)
    target.write_bytes(gzip.compress(make_newc(rows), mtime=0))
    record = {'status': 'HOST_BUILT_LABEL_BOOTSTRAP_NOT_BOOT_VERIFIED', 'kernel_release': release,
              'kernel_commit': json.loads((Path(kernel) / 'manifest.json').read_text())['source_commit'],
              'kernel_manifest_sha256': digest(Path(kernel) / 'manifest.json'), 'root_policy': 'LABEL=PIANOROOT',
              'initramfs_sha256': digest(target), 'entry_sha256': digest(files['pianoinit']),
              'busybox': busybox_record, 'packages': packages,
              'boot_task_snapshot': boot_task_snapshot,
              'boot_task_snapshot_source_sha256': digest(ROOT / 'linux/userspace/piano-boot-task-snapshot')}
    (target.parent / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def build_camera_overlays(rootfs, run, directory):
    """Build the shared Debian camera packages in dependency order."""
    from desktop import build_libcamera_packages as libcamera_builder
    from desktop import build_pipewire_packages as pipewire_builder
    from desktop import build_plasma_camera_package as plasma_camera_builder
    from desktop import build_qt_multimedia_packages as qt_multimedia_builder

    libcamera = directory / 'libcamera'
    run([ROOT / 'build.sh', 'libcamera', '--sysroot', rootfs, '--output', libcamera])
    libcamera_record = libcamera_builder.reusable(libcamera, rootfs)
    if libcamera_record is None:
        raise ValueError('libcamera packages do not match the signed Debian source and CAMSS patches')
    pipewire = directory / 'pipewire'
    run([ROOT / 'build.sh', 'pipewire', '--rootfs', rootfs, '--output', pipewire,
         '--libcamera-packages', libcamera])
    pipewire_record = pipewire_builder.reusable_debian(pipewire, rootfs, libcamera=libcamera)
    if pipewire_record is None:
        raise ValueError('PipeWire packages do not match the signed Debian source and libcamera packages')
    qt_multimedia = directory / 'qt-multimedia'
    run([ROOT / 'build.sh', 'qt-multimedia', '--sysroot', rootfs, '--output', qt_multimedia])
    qt_multimedia_record = qt_multimedia_builder.reusable(qt_multimedia, rootfs)
    if qt_multimedia_record is None:
        raise ValueError('Qt Multimedia packages do not match the signed Debian source and recording patch')
    plasma_camera = directory / 'plasma-camera'
    run([ROOT / 'build.sh', 'plasma-camera', '--sysroot', rootfs, '--output', plasma_camera,
         '--libcamera-packages', libcamera])
    plasma_camera_record = plasma_camera_builder.reusable(plasma_camera, rootfs, libcamera=libcamera)
    if plasma_camera_record is None:
        raise ValueError('Plasma Camera does not match the signed Debian source and libcamera packages')
    return {'libcamera': (libcamera / 'runtime', libcamera_record),
            'pipewire': (pipewire / 'runtime', pipewire_record),
            'qt_multimedia': (qt_multimedia / 'runtime', qt_multimedia_record),
            'plasma_camera': (plasma_camera / 'runtime', plasma_camera_record)}


def build_overlays(record, rootfs, kernel, run, directory=None, *, uapi=None):
    """Produce the same native packages for publishing and local builds."""
    sources = {}
    directory = directory or ROOT / 'build'
    mesa = directory / 'mesa/runtime'
    run([ROOT / 'build.sh', 'mesa', mesa.parent, '--sysroot', rootfs])
    sources['mesa'] = mesa, mesa_packages(mesa, rootfs)
    sensors = Path(record.get('sensors_dir') or directory / 'sensors/runtime')
    if record.get('sensors_dir') is None:
        run([ROOT / 'build.sh', 'sensors', sensors.parent, '--sysroot', rootfs])
    sources['sensors'] = sensors, sensors_packages(sensors, record['config']['sensors'], rootfs=rootfs)
    ffmpeg = Path(record.get('ffmpeg_dir') or directory / 'ffmpeg/runtime')
    if record.get('ffmpeg_dir') is None:
        run([ROOT / 'build.sh', 'ffmpeg', '--output', ffmpeg.parent, '--sysroot', rootfs])
    sources['ffmpeg'] = ffmpeg, ffmpeg_packages(ffmpeg, rootfs)
    sources.update(build_camera_overlays(rootfs, run, directory))
    from desktop import build_libwacom_packages as libwacom_builder
    libwacom = directory / 'libwacom'
    run([ROOT / 'build.sh', 'libwacom', '--sysroot', rootfs, '--output', libwacom])
    libwacom_record = libwacom_builder.reusable(libwacom, rootfs)
    if libwacom_record is None:
        raise ValueError('libwacom packages do not match the shared SPI digitizer support')
    sources['libwacom'] = libwacom / 'runtime', libwacom_record
    from desktop import build_bluez_package as bluez_builder
    bluez = directory / 'bluez'
    run([ROOT / 'build.sh', 'bluez', '--sysroot', rootfs, '--output', bluez])
    bluez_record = bluez_builder.reusable(bluez, rootfs)
    if bluez_record is None:
        raise ValueError('BlueZ package does not match the signed Debian source and HoG patch')
    sources['bluez'] = bluez / 'runtime', bluez_record
    if record['desktop'] == 'kde' or record.get('packages_only'):
        from desktop import build_kwin_package as kwin_builder

        kwin = directory / 'kwin'
        target, series, patches = kwin_builder.selection(rootfs)
        if series is not None:
            run([ROOT / 'build.sh', 'kwin', '--sysroot', rootfs, '--output', kwin])
            kwin_record = kwin_builder.reusable(kwin, target, series, patches)
            if kwin_record is None:
                raise ValueError('KWin packages do not match the signed Debian source and HDR fix')
            sources['kwin'] = kwin / 'runtime', kwin_record
    if record['desktop'] == 'gnome':
        gsd = directory / 'gsd/runtime'
        run([ROOT / 'build.sh', 'gsd', '--output', gsd.parent, '--sysroot', rootfs])
        sources['gnome_power'] = gsd, gsd_packages(gsd, rootfs)
        mutter = Path(record.get('mutter_dir') or directory / 'mutter/runtime')
        if record.get('mutter_dir') is None:
            run([ROOT / 'build.sh', 'mutter', '--output', mutter.parent, '--sysroot', rootfs])
        sources['mutter'] = mutter, mutter_packages(mutter, rootfs)
        from desktop import build_gdm_packages as gdm_builder
        gdm = directory / 'gdm'
        run([ROOT / 'build.sh', 'gdm', '--sysroot', rootfs, '--output', gdm])
        gdm_record = gdm_builder.reusable(gdm, rootfs)
        if gdm_record is None:
            raise ValueError('GDM packages do not match the shared source patches')
        sources['gdm'] = gdm / 'runtime', gdm_record
    install_overlays(rootfs, run, overlay_packages.debian_packages(sources), desktop=record['desktop'])
    sources['fingerprint'] = build_fingerprint_overlay(
        record, rootfs, kernel, directory / 'fingerprint-packages', uapi)
    return sources


def install_overlays(rootfs, run, packages, *, desktop, extra=(), published=False):
    from desktop.build_pipewire_packages import DEBIAN_V4L2_TRANSITION
    from desktop.debian_package import prepare_build_indexes

    records = packages if published else {name: row for name, (_, row) in packages.items()}
    cohort = any(row.get('component') == 'pipewire' and
                 row.get('name') == 'libpipewire-0.3-modules' for row in records.values())
    installed = set()
    if cohort or any(row.get('component') == 'kwin' for row in records.values()):
        text = capture(['chroot', rootfs, 'dpkg-query', '-W',
                        '-f=${Package}\t${db:Status-Status}\n'])
        installed = {row.split()[0] for row in text.splitlines() if row.endswith('\tinstalled')}
    selection = overlay_packages.debian_package_selection(desktop, installed)
    remove = []
    if cohort:
        selection['pipewire'] -= {DEBIAN_V4L2_TRANSITION}
        if DEBIAN_V4L2_TRANSITION in installed:
            remove = [DEBIAN_V4L2_TRANSITION + '-']
    packages = {name: packages[name] for name, row in records.items()
                if row.get('component') not in selection or row['name'] in selection[row['component']]}

    apt = ['chroot', rootfs, 'env', 'DEBIAN_FRONTEND=noninteractive', 'apt-get']
    # Rebuilding a generated image can replace higher CI revisions with the
    # explicitly selected local archives.
    if not published:
        apt.append('--allow-downgrades')
    tmp = target(rootfs, 'var/tmp')
    tmp.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sunuefi-overlays-', dir=tmp) as folder, \
            build_devices(rootfs), build_resolver(rootfs):
        work = Path(folder)
        work.chmod(0o755)
        # Local builds already selected signed Debian indexes for their package
        # build. The newly installed boot config can add the project repository,
        # but the local archives do not depend on that repository being online.
        prepare_build_indexes(rootfs, lambda args: run(['chroot', rootfs, *args]),
                              refresh=published)
        if published:
            paths = overlay_packages.download_debian(rootfs, work, packages, run)
        else:
            paths = []
            for name, (archive, row) in packages.items():
                stage = work / name
                shutil.copyfile(archive, stage)
                if digest(stage) != row['sha256']:
                    raise ValueError('Overlay package changed during installation: ' + name)
                paths.append('/' + stage.relative_to(rootfs).as_posix())
        if published:
            # In particular, do not keep Debian's bytes when both archives
            # provide the same version of an explicitly requested package.
            run([*apt, 'install', '-y', '--no-install-recommends', '--reinstall', *paths, *remove])
            run([*apt, 'install', '-y', '--no-upgrade', '--no-install-recommends',
                 *extra, 'fprintd', 'libpam-fprintd'])
        else:
            run([*apt, 'install', '-y', '--no-install-recommends', *extra, *paths, *remove])
        if 'chromium' in extra:
            run([*apt, 'purge', '-y', 'firefox-esr'])


def build_fingerprint_overlay(record, rootfs, kernel, fingerprint, uapi):
    """Build libfprint against the same installed runtime as the other packages."""
    from build_piano_fingerprint import install_debian_desktop, prepare_desktop_backend
    from build_release_helpers import export_uapi

    if record['runtime_dir']:
        install_debian_desktop(rootfs, Path(record['runtime_dir']), fingerprint)
    else:
        with tempfile.TemporaryDirectory(prefix='sunuefi-fingerprint-', dir=Path(record['output'])) as temporary:
            work = Path(temporary)
            if uapi is None:
                uapi = work / 'uapi'
                export_uapi(Path(record['source']), Path(record['kernel_build']),
                            kernel['source_commit'], uapi)
            prepare_desktop_backend(work, uapi)
            install_debian_desktop(rootfs, work, fingerprint)
    metadata = json.loads((fingerprint / 'SOURCE.json').read_text())
    metadata['packages'] = {'libfprint-2-2': metadata['package']}
    folder = fingerprint / 'runtime' if (fingerprint / 'runtime').is_dir() else fingerprint
    return folder, metadata


def build_local_overlays(record, kernel, run):
    """Keep project repositories in the image out of build dependency selection."""
    from desktop.debian_package import archive_inputs, use_build_archive, verify_build_archive

    output = Path(record['output'])
    with tempfile.TemporaryDirectory(prefix='sunuefi-overlay-root-', dir=output) as temporary:
        builder = Path(temporary) / 'builder'
        prepare_package_build_root(builder, record['desktop'], run)
        rootfs = builder / 'rootfs'
        with use_build_archive(archive_inputs(rootfs)):
            sources = build_overlays(record, rootfs, kernel, run, output / 'packages')
            verify_build_archive(rootfs)
            return sources


def build_overlay_set(record, rootfs, kernel, run):
    """Build or reuse native packages independently of product image assembly."""
    from build import overlay_cache
    from build_release_helpers import export_uapi
    from desktop.debian_package import use_build_archive, verify_build_archive

    desktop = record['desktop']
    output = Path(record['output'])
    with ExitStack() as contexts:
        directory = uapi = inputs = None
        # CI starts from a fresh package build root and uses only the repository recipes.
        # A resumed build or an explicitly supplied bundle has different inputs.
        if (os.environ.get('SUNUEFI_BUILD_ENVIRONMENT_ID')
                and not any(record.get(key) for key in
                            ('resume', 'runtime_dir', 'sensors_dir', 'ffmpeg_dir', 'mutter_dir'))):
            temporary = contexts.enter_context(
                tempfile.TemporaryDirectory(prefix='sunuefi-overlay-uapi-', dir=output))
            uapi = Path(temporary)
            uapi_digest = export_uapi(Path(record['source']), Path(record['kernel_build']),
                                      kernel['source_commit'], uapi)
            inputs = overlay_cache.debian_inputs(ROOT, rootfs, uapi_digest)
            if cached := overlay_cache.restore_debian(
                    ROOT, Path(record['packages_only']), 'debian-' + desktop, inputs):
                return cached
            contexts.enter_context(use_build_archive(inputs['archive']))
            directory = output / 'packages'
        sources = build_overlays(record, rootfs, kernel, run, directory, uapi=uapi)
        if inputs is not None:
            verify_build_archive(rootfs)
            if (inputs['recipes'] != overlay_cache.debian_recipe_identity(ROOT) or
                    inputs['kernel_uapi_sha256'] != runtime.tree_digest(runtime.tree_files(uapi / 'include'))):
                raise ValueError('Debian overlay recipes or exported kernel headers changed during the build')
        packages = overlay_packages.debian_packages(sources)
        overlay_packages.verify_debian_generation(packages)
        return overlay_packages.export(Path(record['packages_only']), 'debian-' + desktop,
                                       'apt', packages,
                                       {name: value for name, (_, value) in sources.items()},
                                       build_inputs=inputs)


def execute(record):
    if record['missing_inputs']: raise ValueError('Missing inputs: ' + ', '.join(record['missing_inputs']))
    require_host()
    out, rootfs, source, kernel = [Path(record[k]) for k in ('output', 'rootfs', 'source', 'kernel')]
    cfg = record['config']; public = Path(record['public_source']); m, kernel_hash = modules.inspect(kernel)
    desktop = record.get('desktop', 'gnome')
    abi = runtime.kernel_identity(Path(record['kernel_build']), source, m['source_commit'], m['kernel_release'])
    # Clang selects the target GCC libraries from the supplied Debian sysroot.
    # A host cross-GCC can otherwise mix its own libc linker scripts into it.
    helper_cc = 'clang' if shutil.which('clang') else 'aarch64-linux-gnu-gcc'
    if record['runtime_dir']:
        bundle = Path(record['runtime_dir'])
        saved = json.loads((bundle / 'manifest.json').read_text())
        if saved.get('status') != 'RUNTIME_COMPILED_NOT_DEVICE_TESTED' or saved.get('kernel') != abi or saved.get('public_commit') != cfg['debian']['commit']:
            raise ValueError('Runtime bundle does not belong to the actual new kernel build')
        from build_release_helpers import inputs_current
        if not inputs_current(bundle):
            raise ValueError('Runtime bundle sources differ; rebuild it for the current hardware implementation')
    elif not record.get('packages_only') and any(not shutil.which(name)
                                                 for name in (helper_cc, 'make', 'depmod')):
        raise ValueError('Install the release helper compiler, make and depmod first')
    if out.exists() and not record.get('resume'): raise ValueError('Output appeared after planning; preserve it')
    out.parent.mkdir(parents=True, exist_ok=True); log = out.with_name(out.name + '.build.log')
    if log.exists() and not record.get('resume'): raise ValueError('Build log exists; choose a new output name')
    if record.get('resume'):
        (out / 'manifest.json').unlink(missing_ok=True)
    def run(args, cwd=None, *, capture_output=False, text=False):
        command = list(map(str, args))
        if capture_output:
            return subprocess.run(command, cwd=cwd, capture_output=True, text=text, check=True)
        with log.open('a') as stream:
            return subprocess.run(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT, check=True)
    try:
        if not record.get('resume'):
            if record.get('packages_only'):
                prepare_package_build_root(out, desktop, run)
            else:
                from prepare_debian_desktop import prepare
                recipe = prepare(public, out.with_name(out.name + '-recipe'), desktop)
                run(['unshare', '--mount', '--propagation', 'private', 'bash', recipe / 'scripts/build-rootfs.sh',
                     '--suite', cfg['debian']['suite'], '--output', out])
        if not (out / 'COMPLETE').is_file() or rootfs.stat().st_uid:
            raise ValueError('Debian base build did not complete with native guest ownership')
        archive = debian_archive(rootfs, desktop)
        put(rootfs, 'usr/sbin/policy-rc.d', '#!/bin/sh\nexit 101\n'); (rootfs / 'usr/sbin/policy-rc.d').chmod(0o755)
        if record.get('resume') and not record.get('published_packages'):
            with build_devices(rootfs), build_resolver(rootfs):
                run(['chroot', rootfs, 'apt-get', '-o', 'APT::Update::Error-Mode=any', 'update'])
                archive = debian_archive(rootfs, desktop)
                run(['chroot', rootfs, 'env', 'DEBIAN_FRONTEND=noninteractive',
                     'apt-get', 'upgrade', '-y', '--with-new-pkgs', '--no-remove'])
        published = record.get('published_packages')
        if record.get('packages_only'):
            result = build_overlay_set(record, rootfs, m, run)
            (out / 'FAILED.json').unlink(missing_ok=True)
            return result
        select_wireless_regdb(rootfs, run)
        from package.kernel import install as install_kernel

        if published:
            bsp_archive, bsp = overlay_packages.shared_bsp('debian')
            topology_archive, topology = overlay_packages.shared_topology('debian')
            power_archive, power = overlay_packages.shared_bsp('debian', component='power-profiles')
            camera_archive, camera_record = camera.shared_package('debian')
            components, packages = overlay_packages.read_debian(Path(published), desktop)
            overlay_packages.check_topology_conflicts(rootfs, topology)
            install_kernel(rootfs, 'debian', kernel, published=True)
            install_overlays(rootfs, run, {
                **packages, bsp_archive.name: bsp, topology_archive.name: topology,
                camera_archive.name: camera_record, power_archive.name: power,
            }, desktop=desktop, extra=cfg['debian']['extra_packages'], published=True)
            archive = debian_archive(rootfs, desktop)
        else:
            camera_archive, camera_record = camera.ensure('debian')
            sources = build_local_overlays(record, m, run)
            packages = overlay_packages.debian_packages(sources)
            install_kernel(rootfs, 'debian', kernel)
            with tempfile.TemporaryDirectory(prefix='sunuefi-bsp-', dir=out) as temporary:
                bsp_archive, bsp = overlay_packages.build_bsp(Path(temporary) / 'package', 'debian', run)
                topology_archive, topology = overlay_packages.build_bsp(
                    Path(temporary) / 'topology', 'debian', run, component='audio-topology')
                power_archive, power = overlay_packages.build_bsp(
                    Path(temporary) / 'power', 'debian', run, component='power-profiles')
                overlay_packages.check_topology_conflicts(rootfs, topology)
                install_overlays(rootfs, run, {
                    **packages, bsp_archive.name: (bsp_archive, bsp),
                    topology_archive.name: (topology_archive, topology),
                    camera_archive.name: (camera_archive, camera_record),
                    power_archive.name: (power_archive, power),
                }, desktop=desktop, extra=(*cfg['debian']['extra_packages'], 'fprintd', 'libpam-fprintd'))
            components = {name: {'packages': {}} for name in overlay_packages.DEBIAN_GNOME}
            components.update({name: value for name, (_, value) in sources.items()})
        sensor_unit = target(rootfs, 'usr/lib/systemd/system/adsprpcd-sensorspd.service')
        sensor_unit.write_text(sensor_unit.read_text().replace(' piano-adsp.service', ''))
        if record.get('resume') and (out / 'adapters').is_dir(): shutil.rmtree(out / 'adapters')
        userspace.stage(rootfs, public, desktop=desktop); hardware.build(out / 'adapters', public, rootfs)
        if desktop == 'kde':
            from prepare_debian_desktop import configure
            configure(rootfs, subprocess.run)
        # Compile the staged local GNOME defaults after the public builder's
        # dconf update, so animation/power policy is actually present in root.
        if desktop == 'gnome':
            run(['chroot', rootfs, 'dconf', 'update'])
        if record['runtime_dir']:
            bundle = Path(record['runtime_dir']); rm = json.loads((bundle / 'manifest.json').read_text())
            if rm['kernel'] != runtime.kernel_identity(Path(record['kernel_build']), source, m['source_commit'], m['kernel_release']): raise ValueError('Runtime bundle kernel ABI provenance differs')
            runtime.stage(bundle, rootfs)
        else:
            bundle = out / 'runtime'
            if record.get('resume') and bundle.is_dir():
                shutil.rmtree(bundle)
            with runtime_build_sysroot(rootfs, out, run) as sysroot:
                run([sys.executable, ROOT / 'tools/build_release_helpers.py', '--kernel', kernel, '--source', source,
                     '--kernel-build', record['kernel_build'], '--output', bundle, '--sysroot', sysroot,
                     '--cc', helper_cc])
            runtime.stage(bundle, rootfs)
        from build_piano_fingerprint import debian_abi, desktop_service
        if not published and debian_abi(rootfs) != components['fingerprint']['target_abi']:
            raise ValueError('The image fingerprint ABI differs from the signed package build root')
        desktop_service(rootfs)
        from stage_piano_firmware import stage as stage_firmware, verify_display
        firmware = stage_firmware(Path(record['firmware_source']), rootfs, 'debian',
                                  shared=bool(published), run=run)
        apply_policy(rootfs, out, cfg)
        run(['chroot', rootfs, 'systemctl', 'enable', 'tuned-ppd.service'])
        if desktop == 'denial':
            from build.denial import install_debian

            denial_packages = record.get('denial_packages')
            denial_versions = install_debian(rootfs, run,
                                             manifest=Path(denial_packages) if denial_packages else None)
            archive = debian_archive(rootfs, desktop)
        boot_request = native_boot_request(rootfs, out)
        sanitize(rootfs, out)
        from package.v4l2loopback import verify_installed
        verify_installed(rootfs, 'debian', m['kernel_release'])
        (rootfs / 'usr/sbin/policy-rc.d').unlink()
        packages = capture(['chroot', rootfs, 'dpkg-query', '-W', '-f=${binary:Package}\t${Version}\t${Architecture}\t${db:Status-Status}\n'])
        (out / 'packages.tsv').write_text(packages + '\n')
        installed = {p[0].split(':')[0]: p[1] for line in packages.splitlines() if len(p := line.split('\t')) == 4 and p[3] == 'installed'}
        if installed.get(bsp['name']) != bsp['version']:
            raise ValueError('Installed Piano device configuration package version differs')
        overlay_packages.verify_bsp_files(rootfs, 'debian')
        if installed.get(topology['name']) != topology['version']:
            raise ValueError('Installed Piano audio topology package version differs')
        overlay_packages.verify_topology(rootfs, 'debian', topology)
        if installed.get(power['name']) != power['version']:
            raise ValueError('Installed Piano power profiles package version differs')
        overlay_packages.verify_bsp_files(rootfs, 'debian', component='power-profiles')
        verify_display(rootfs, 'debian', firmware['display_package'])
        if installed.get(camera_record['name']) != camera_record['version']:
            raise ValueError('Installed Piano camera package version differs')
        camera.verify_installed(rootfs, 'debian', camera_archive)
        defaults = default_system_record(rootfs, installed, cfg, desktop)
        selection = overlay_packages.debian_package_selection(desktop, installed)
        for component, metadata in components.items():
            for package, row in metadata['packages'].items():
                if component in selection and package not in selection[component]:
                    continue
                if installed.get(package) != row['version']:
                    raise ValueError(f'Installed {component} package version differs: {package}')
        metadata = {str(p.relative_to(rootfs)): digest(p) for p in (rootfs / 'var/lib/apt/lists').glob('*InRelease')}
        if not metadata: raise ValueError('Signed APT InRelease metadata is missing')
        metadata.update({str(p.relative_to(rootfs)): digest(p) for p in (rootfs / 'etc/apt').rglob('*') if p.is_file()})
        snapshot = target(rootfs, 'usr/lib/piano/piano-boot-task-snapshot')
        shutil.copy2(ROOT / 'linux/userspace/piano-boot-task-snapshot', snapshot)
        snapshot.chmod(0o755)
        boot = bootstrap(kernel, out, m['kernel_release'], record.get('boot_task_snapshot', False))
        clock = clock_epoch(rootfs)
        if modules.inspect(kernel)[1] != kernel_hash: raise ValueError('Kernel changed during rootfs build')
        result = {'status': image_status(desktop), 'rootfs': str(rootfs),
                  'root_policy': 'LABEL=PIANOROOT', 'kernel_release': m['kernel_release'], 'kernel_commit': m['source_commit'],
                  'kernel_manifest_sha256': kernel_hash, 'release_config_sha256': digest(POLICY), 'packages_sha256': digest(out / 'packages.tsv'),
                  'adapters_manifest_sha256': digest(out / 'adapters/manifest.json'), 'runtime_manifest_sha256': digest(bundle / 'manifest.json'),
                  'apt_policy': cfg['debian']['apt_policy'], 'apt_metadata': metadata,
                  'debian_archive': archive, 'debian_suite': cfg['debian']['suite'],
                  'debian_commit': cfg['debian']['commit'],
                  'device_configuration': bsp, 'camera_package': camera_record,
                  'audio_topology_package': topology, 'power_profiles_package': power,
                  'display_firmware_package': firmware['display_package'],
                  **components,
                  'boot_request': boot_request, 'firmware_components': cfg['firmware']['components'],
                  'clock_epoch': clock,
                  'default_system': defaults, 'desktop': desktop,
                  'initramfs': boot, 'password': cfg['login']['password'], 'autologin_retained': True,
                  'bit_reproducible': False, 'root_uid': rootfs.stat().st_uid, 'device_operation_performed': False}
        if published:
            result['overlay_manifest_sha256'] = digest(Path(published))
        if desktop == 'denial':
            result['denial'] = denial_versions
            if denial_packages:
                result['denial_manifest_sha256'] = digest(Path(denial_packages))
        (out / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
        (out / 'FAILED.json').unlink(missing_ok=True)
        return result
    except Exception as exc:
        if out.is_dir(): (out / 'FAILED.json').write_text(json.dumps({'status': 'BUILD_FAILED', 'desktop': desktop, 'error': str(exc)}) + '\n')
        raise
    finally:
        if rootfs.is_dir():
            target(rootfs, 'usr/sbin/policy-rc.d').unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('kernel', 'source', 'output'): parser.add_argument('--' + name, type=Path, required=True)
    for name in ('sensors-dir', 'ffmpeg-dir', 'mutter-dir', 'runtime-dir', 'kernel-build', 'public-source', 'firmware-source'): parser.add_argument('--' + name, type=Path)
    mode = parser.add_mutually_exclusive_group(required=True); mode.add_argument('--plan', action='store_true'); mode.add_argument('--execute', action='store_true')
    packages = parser.add_mutually_exclusive_group()
    packages.add_argument('--packages-only', type=Path, metavar='DIRECTORY',
                          help='Build and export overlay packages without assembling a product image')
    packages.add_argument('--published-packages', type=Path, metavar='MANIFEST',
                          help='Install exact overlay packages from the signed SunUEFI APT source')
    parser.add_argument('--denial-packages', type=Path, metavar='MANIFEST',
                        help='Install the published Denial package set for --desktop denial')
    parser.add_argument('--resume', action='store_true', help='Reuse the completed upstream base and refresh release assembly')
    parser.add_argument('--desktop', choices=DESKTOPS, default='gnome')
    parser.add_argument('--boot-task-snapshot', action='store_true',
                        help='Enable one delayed PID1/blocked-task snapshot in the diagnostic initramfs')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = vars(parser.parse_args()); execute_flag = args.pop('execute'); args.pop('plan')
    if execute_flag and not args.pop('worker'):
        raise SystemExit(subprocess.run(['unshare', '--mount', '--propagation', 'private',
                                        sys.executable, __file__, *sys.argv[1:], '--worker']).returncode)
    args.pop('worker', None)
    try:
        record = plan(**args); print(json.dumps(execute(record) if execute_flag else record, indent=2))
    except (OSError, ValueError, subprocess.SubprocessError) as exc: parser.exit(2, str(exc) + '\n')


if __name__ == '__main__': main()
