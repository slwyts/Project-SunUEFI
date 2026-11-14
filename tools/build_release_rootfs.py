#!/usr/bin/env python3
"""Build a new public-source GNOME root and label bootstrap; never write a device."""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import gzip
import json
import os
from pathlib import Path
import platform
import shutil
import stat
import subprocess
import sys
import time

import build_piano_disk_bootstrap as disk
import build_piano_runtime_helpers as runtime
import stage_piano_full_userspace as userspace
import stage_piano_kernel_modules as modules
import stage_piano_ram_hardware as hardware
from make_kernel_initramfs import make_newc, validate_static_arm64_elf

ROOT = Path(__file__).resolve().parents[1]
POLICY = ROOT / 'config/release.json'


def digest(path): return modules.sha(path)


def capture(args, cwd=None):
    return subprocess.check_output(list(map(str, args)), cwd=cwd, text=True).strip()


def debian_archive(rootfs):
    """Check the actual APT indexes, including a resumed generated root."""
    release = userspace.guest_resolve(rootfs, '/etc/os-release').read_text()
    if 'ID=debian' not in release.splitlines():
        raise ValueError('The product base must be Debian')
    text = capture(['chroot', rootfs, 'apt-get', 'indextargets', '--format',
                    '$(CODENAME) $(SUITE) $(ORIGIN) $(IDENTIFIER)'])
    rows = [line.split() for line in text.splitlines() if line.strip()]
    if not rows or any(len(row) != 4 or row[:3] != ['sid', 'unstable', 'Debian']
                       for row in rows) or not any(row[3] == 'Packages' for row in rows):
        raise ValueError('Debian product APT indexes must contain sid only; rebuild the base root')
    return {'suite': 'sid', 'indexes': rows}


def repository(path, commit=None, tree=None):
    head = capture(['git', '-C', path, 'rev-parse', 'HEAD'])
    if (commit and head != commit) or capture(['git', '-C', path, 'status', '--porcelain']):
        raise ValueError('Source HEAD/cleanliness mismatch: ' + str(path))
    if tree and capture(['git', '-C', path, 'rev-parse', 'HEAD^{tree}']) != tree:
        raise ValueError('Release kernel tree mismatch')
    return head


def plan(kernel, source, output, runtime_dir=None, kernel_build=None,
         public_source=None, firmware_source=None, macros=None, v4l2_source=None, root=ROOT, resume=False,
         sensors_dir=None, ffmpeg_dir=None, boot_task_snapshot=False, desktop='gnome', mutter_dir=None):
    root = Path(root).resolve(); config = json.loads((root / 'config/release.json').read_text())
    if desktop not in ('gnome', 'kde'):
        raise ValueError('Release desktop must be gnome or kde')
    if desktop == 'kde':
        from prepare_debian_desktop import GNOME_PACKAGES, KDE_PACKAGES
        config['debian']['extra_packages'] = [
            p for p in config['debian']['extra_packages'] if p not in GNOME_PACKAGES
        ] + KDE_PACKAGES
    if config['debian']['apt_policy'] != 'mutable-recorded' or config['debian']['snapshot'] is not None:
        raise ValueError('Only signed mutable APT with recorded metadata is implemented; snapshot mode is not configured')
    if config['debian']['suite'] != 'sid':
        raise ValueError('The Debian product builds only sid')
    kernel, source, output = [Path(p).resolve() for p in (kernel, source, output)]
    if not output.is_relative_to(root / 'build/distros') or output.is_symlink() or (output.exists() and not resume):
        raise ValueError('Output must be a new project build/distros directory')
    previous = None
    if resume:
        if not (output / 'COMPLETE').is_file():
            raise ValueError('Resume needs a completed upstream base root')
        if (output / 'manifest.json').is_file():
            previous = json.loads((output / 'manifest.json').read_text())
            if previous.get('status') not in ('HOST_BUILT_RELEASE_GNOME_ROOT_NOT_BOOT_VERIFIED', 'HOST_BUILT_RELEASE_KDE_ROOT_NOT_BOOT_VERIFIED') or previous.get('debian_commit') != config['debian']['commit']:
                raise ValueError('Completed base root belongs to another source')
            if previous.get('desktop', 'gnome') != desktop:
                raise ValueError('Resume must keep the desktop used to build this root')
            if previous.get('debian_suite') != config['debian']['suite']:
                raise ValueError('Resume must keep the Debian suite; build a fresh sid base')
        else:
            base_manifest = output / 'build-manifest.txt'
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
    macros = Path(macros or root / 'upstream/audioreach-topology').resolve()
    loop = Path(v4l2_source or root / 'upstream/v4l2loopback').resolve()
    missing = []
    for name, path in (('kernel manifest', kernel / 'manifest.json'), ('kernel source', source / 'Makefile'),
                       ('public rootfs builder', public / 'scripts/build-rootfs.sh'), ('firmware', next(iter(sorted(firmware.glob('*/manifest.json'))), firmware / 'missing'))):
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
    else:
        for name, path in (('AudioReach topology macros', macros / 'audioreach'), ('v4l2loopback source', loop / 'Makefile')):
            if not path.exists(): missing.append(name)
    record = None
    if (kernel / 'manifest.json').is_file():
        record, _ = modules.inspect(kernel)
        head = repository(source, tree=config['kernel']['target_tree'])
        if record['source_commit'] != head or record.get('source_clean') is not True:
            raise ValueError('Kernel manifest does not match the prepared release source')
        words = record['command_line'].split()
        if record.get('root_policy') != 'LABEL=PIANOROOT' or [w for w in words if w.startswith('piano.root=')] != ['piano.root=LABEL=PIANOROOT'] or any(w.startswith('root=') for w in words) or 'userdata' in record['command_line']:
            raise ValueError('Release kernel must select LABEL=PIANOROOT, never Android userdata')
    if public.is_dir(): repository(public, config['debian']['commit'])
    return {'status': 'PLAN_ONLY', 'resume': resume, 'desktop': desktop,
            'previous_kernel_release': previous.get('kernel_release') if previous else None,
            'root_policy': 'LABEL=PIANOROOT', 'kernel': str(kernel), 'source': str(source),
            'output': str(output), 'rootfs': str(output / 'rootfs'),
            'sensors_dir': str(sensors) if sensors else None,
            'ffmpeg_dir': str(ffmpeg) if ffmpeg else None,
            'mutter_dir': str(mutter) if mutter else None,
            'boot_task_snapshot': boot_task_snapshot,
            'runtime_dir': str(Path(runtime_dir).resolve()) if runtime_dir else None,
            'kernel_build': str(Path(kernel_build or root / 'build/kernels/release').resolve()),
            'public_source': str(public), 'firmware_source': str(firmware), 'config': config, 'missing_inputs': missing,
            'macros': str(macros), 'v4l2_source': str(loop),
            'steps': ['require root + mount/chroot capability; ARM64 or enabled qemu-aarch64 binfmt',
                      'derive build-rootfs.sh for sid and install the selected desktop from signed APT in a private mount namespace',
                      'install extra packages with signed APT defaults; record mutable repository metadata',
                      'verify sensors SOURCE/SHA256SUMS; install six runtime packages and signed APT dependencies',
                      'stage public overlay/adapters/modules; build_release_helpers uses actual compiler, alsatplg/m4 and matching kernel ABI',
                      'verify/stage firmware; apply label storage policy; set the documented login password and remove generated access keys',
                      'record actual packages/source hashes; build release label initramfs; no archive or device writes']}


def require_host():
    if os.geteuid(): raise ValueError('--execute requires root; debootstrap/chroot/mount are real build dependencies')
    caps = next(line.split()[1] for line in Path('/proc/self/status').read_text().splitlines() if line.startswith('CapEff:'))
    if int(caps, 16) & ((1 << 21) | (1 << 18)) != ((1 << 21) | (1 << 18)):
        raise ValueError('Root runner also needs CAP_SYS_ADMIN and CAP_SYS_CHROOT')
    required = ['debootstrap', 'chroot', 'curl', 'ssh-keygen', 'openssl', 'mount', 'umount', 'unshare', 'sha256sum', 'dpkg-deb']
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


def default_system_record(rootfs, installed, config, desktop='gnome'):
    """Require the shipped desktop and BSP, including runtime-bundle resumes."""
    if desktop == 'kde' and {'tuned', 'tuned-ppd'} & installed.keys():
        raise ValueError('KDE must not retain TuneD without its GNOME profile configuration')
    required = {'systemd-sysv', 'pipewire', 'network-manager'}
    required.update({'gnome-shell', 'gdm3'} if desktop == 'gnome' else {'plasma-workspace', 'sddm', 'powerdevil'})
    required.update(config['debian']['extra_packages'])
    missing = sorted(required - installed.keys())
    if missing:
        raise ValueError('Required desktop packages are not installed: ' + ', '.join(missing))

    paths = (
        'etc/modules-load.d/piano-bluetooth.conf',
        'etc/udev/rules.d/79-piano-touch.rules',
        'usr/share/piano/pen/p81c-radio.json', 'usr/lib/piano/pen-bluetooth',
        'usr/lib/piano/touch-input', 'usr/bin/piano-pen-canvas',
    )
    paths += (('etc/dconf/db/local.d/00-sunuefi', 'etc/dconf/db/local', 'etc/dconf/profile/user',
               'usr/lib/tuned/profiles/piano-power-saver/tuned.conf',
               'usr/lib/tuned/profiles/piano-balanced/tuned.conf',
               'usr/lib/tuned/profiles/piano-performance/tuned.conf',
               'etc/tuned/piano-ppd.conf',
               'usr/lib/systemd/system/tuned-ppd.service.d/50-piano-ppd.conf')
              if desktop == 'gnome' else ('etc/sddm.conf.d/10-sunuefi.conf',))
    files = {}
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


def apply_policy(rootfs, output, config):
    rootfs, _ = own_root(rootfs, output)
    storage = config['storage']
    if (storage['root_label'], storage['root_partname'], storage['esp_label'], storage['root_fstype'], storage['esp_partname']) != ('PIANOROOT', 'sunuefi_root', 'SUNUEFI_ESP', 'ext4', 'sunuefi_esp'):
        raise ValueError('Unknown release storage identities')
    if set(storage['masked_units']) != {'piano-swapfile.service', 'qbootctl.service'}:
        raise ValueError('Only the two storage-policy units may be masked')
    put(rootfs, 'etc/fstab', 'LABEL=PIANOROOT / ext4 defaults,noatime,x-systemd.growfs 0 1\nLABEL=SUNUEFI_ESP /boot/efi vfat umask=0077,nofail 0 2\n')
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
    series = Path(root) / 'patches/piano-sensors/sid/series.json'
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
    expected = {'piano-sensors': policy['native_version'],
                'fastrpc-support': versions['fastrpc'], 'libfastrpc1': versions['fastrpc'],
                'libssc2': versions['libssc'] + '+' + policy['local_revisions']['libssc'],
                'libssc-bin': versions['libssc'] + '+' + policy['local_revisions']['libssc'],
                'iio-sensor-proxy': versions['iio-sensor-proxy'] + '+' +
                                    policy['local_revisions']['iio-sensor-proxy']}
    if any(rows[package]['version'] != version for package, version in expected.items()):
        raise ValueError('Sensors runtime versions differ from the sid package policy')
    target = debian.target_identity(rootfs, versions, {'libssc': 'libssc2',
                                    'iio-sensor-proxy': 'iio-sensor-proxy'}) if rootfs else None
    if (source.get('target', {}).get('sources') != versions or
            source.get('policy_sha256') != digest(Path(root) / builder.POLICY.relative_to(ROOT)) or
            debian.reusable(metadata, policy['native_version'], patches, builder.RUNTIME, target) is None):
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
            source.get('package_version') != source.get('source', {}).get('version', '') +
            '+sunuefi' + str(builder.REVISION)):
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


def bootstrap(rootfs, kernel, output, release, boot_task_snapshot=False, firmware_root=None):
    # Use the same authenticated distro input on fresh CI and local builds.
    # The early diagnostic BusyBox capture is not a release dependency.
    busybox = disk.guest_resolve(rootfs, '/usr/bin/busybox')
    validate_static_arm64_elf(busybox.read_bytes())
    applets = set(capture(['chroot', rootfs, '/usr/bin/busybox', '--list']).splitlines())
    # Debian's static build omits mountpoint; use its util-linux executable
    # with the same ELF dependency closure already needed by blkid.
    bootstrap_applets = tuple(name for name in disk.APPLETS if name != 'mountpoint')
    if missing := set(bootstrap_applets) - applets:
        raise ValueError('Static BusyBox is missing bootstrap applets: ' + ', '.join(sorted(missing)))
    package = capture(['chroot', rootfs, 'dpkg-query', '-W',
                       '-f=${Package}\t${Version}\t${Architecture}', 'busybox-static']).split('\t')
    if len(package) != 3 or package[0] != 'busybox-static' or package[2] != 'arm64':
        raise ValueError('Release bootstrap requires the installed ARM64 busybox-static package')
    busybox_source = output / 'initramfs/busybox-source.json'
    busybox_source.parent.mkdir(exist_ok=True)
    busybox_record = {'package': package[0], 'version': package[1],
        'architecture': package[2], 'sha256': digest(busybox),
        'source': 'signed Debian APT in the release rootfs', 'license': 'GPL-2.0-only'}
    busybox_source.write_text(json.dumps(busybox_record, indent=2) + '\n')
    files = disk.runtime_files(rootfs, ('/usr/sbin/blkid', '/usr/bin/mountpoint'))
    directory = Path(kernel) / 'modules/lib/modules' / release
    ordered, _ = disk.module_closure(directory)
    rescue, _ = disk.module_closure(directory, disk.SEEDS + disk.RESCUE_SEEDS)
    files.update({'bin/busybox': busybox, 'pianoinit': ROOT / 'linux/userspace/disk-bootstrap',
                  'usr/local/sbin/piano-debug-bootstrap': ROOT / 'linux/userspace/piano-debug-bootstrap'})
    files['init'] = files['pianoinit']
    for name in rescue: files[f'lib/modules/{release}/{name}'] = directory / name
    files['lib/firmware/nanosic/MCU_Upgrade.bin'] = disk.guest_resolve(
        Path(firmware_root or rootfs), '/lib/firmware/nanosic/MCU_Upgrade.bin')
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
              'busybox': busybox_record,
              'boot_task_snapshot': boot_task_snapshot,
              'boot_task_snapshot_source_sha256': digest(ROOT / 'linux/userspace/piano-boot-task-snapshot')}
    (target.parent / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


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
    elif any(not shutil.which(name) for name in (helper_cc, 'alsatplg', 'm4', 'make', 'depmod')):
        raise ValueError('Install the release helper compiler, alsatplg, m4, make and depmod first')
    if out.exists() and not record.get('resume'): raise ValueError('Output appeared after planning; preserve it')
    out.parent.mkdir(parents=True, exist_ok=True); log = out.with_name(out.name + '.build.log')
    if log.exists() and not record.get('resume'): raise ValueError('Build log exists; choose a new output name')
    if record.get('resume'):
        (out / 'manifest.json').unlink(missing_ok=True)
    def run(args, cwd=None):
        with log.open('a') as stream: subprocess.run(list(map(str, args)), cwd=cwd, stdout=stream, stderr=subprocess.STDOUT, check=True)
    try:
        if not record.get('resume'):
            from prepare_debian_desktop import prepare
            recipe = prepare(public, out.with_name(out.name + '-recipe'), desktop)
            run(['unshare', '--mount', '--propagation', 'private', 'bash', recipe / 'scripts/build-rootfs.sh',
                 '--suite', cfg['debian']['suite'], '--output', out])
        if not (out / 'COMPLETE').is_file() or rootfs.stat().st_uid: raise ValueError('Base desktop build did not complete with native guest ownership')
        archive = debian_archive(rootfs)
        put(rootfs, 'usr/sbin/policy-rc.d', '#!/bin/sh\nexit 101\n'); (rootfs / 'usr/sbin/policy-rc.d').chmod(0o755)
        if record.get('resume'):
            with build_devices(rootfs), build_resolver(rootfs):
                run(['chroot', rootfs, 'apt-get', 'update'])
                run(['chroot', rootfs, 'env', 'DEBIAN_FRONTEND=noninteractive',
                     'apt-get', 'upgrade', '-y', '--with-new-pkgs', '--no-remove'])
        sensor_folder = Path(record.get('sensors_dir') or ROOT / 'build/sensors/runtime')
        if record.get('sensors_dir') is None:
            run([ROOT / 'build.sh', 'sensors', sensor_folder.parent, '--sysroot', rootfs])
        sensors = sensors_packages(sensor_folder, cfg['sensors'], rootfs=rootfs)
        ffmpeg_folder = Path(record.get('ffmpeg_dir') or ROOT / 'build/ffmpeg/runtime')
        if record.get('ffmpeg_dir') is None:
            run([ROOT / 'build.sh', 'ffmpeg', '--output', ffmpeg_folder.parent, '--sysroot', rootfs])
        ffmpeg = ffmpeg_packages(ffmpeg_folder, rootfs)
        from desktop import build_pipewire_packages
        pipewire_folder = ROOT / 'build/pipewire'
        run([ROOT / 'build.sh', 'pipewire', '--rootfs', rootfs, '--output', pipewire_folder])
        pipewire = build_pipewire_packages.reusable(
            pipewire_folder, build_pipewire_packages.debian_version(rootfs), 'deb')
        if pipewire is None:
            raise ValueError('PipeWire GStreamer plugin does not match the target ABI')
        gsd_folder = ROOT / 'build/gsd/runtime'
        if desktop == 'gnome':
            run([ROOT / 'build.sh', 'gsd', '--output', gsd_folder.parent, '--sysroot', rootfs])
        gsd = gsd_packages(gsd_folder, rootfs) if desktop == 'gnome' else {'packages': {}}
        mutter_folder = Path(record.get('mutter_dir') or ROOT / 'build/mutter/runtime')
        if desktop == 'gnome' and not record.get('mutter_dir'):
            run([ROOT / 'build.sh', 'mutter', '--output', mutter_folder.parent, '--sysroot', rootfs])
        mutter = mutter_packages(mutter_folder, rootfs) if desktop == 'gnome' else {'packages': {}}
        from desktop import build_gdm_packages as gdm_builder
        gdm_folder = ROOT / 'build/gdm'
        gdm = {'packages': {}}
        if desktop == 'gnome':
            run([ROOT / 'build.sh', 'gdm', '--sysroot', rootfs, '--output', gdm_folder])
            gdm = gdm_builder.reusable(gdm_folder, rootfs)
            if gdm is None:
                raise ValueError('GDM packages do not match the display readiness patch')
        gsd_stage = target(rootfs, 'tmp/piano-gnome-packages')
        gsd_stage.mkdir(parents=True, exist_ok=True)
        for row in gsd['packages'].values():
            shutil.copy2(gsd_folder / row['file'], gsd_stage / row['file'])
        for row in mutter['packages'].values():
            destination = gsd_stage / row['file']
            shutil.copy2(mutter_folder / row['file'], destination)
            if digest(destination) != row['sha256']:
                raise ValueError('Mutter package changed while staging: ' + row['file'])
        for row in gdm['packages'].values():
            destination = gsd_stage / row['file']
            shutil.copy2(gdm_folder / 'runtime' / row['file'], destination)
            if digest(destination) != row['sha256']:
                raise ValueError('GDM package changed while staging')
        for row in pipewire['packages'].values():
            destination = gsd_stage / row['file']
            shutil.copy2(pipewire_folder / 'runtime' / row['file'], destination)
            if digest(destination) != row['sha256']:
                raise ValueError('PipeWire package changed while staging')
        sensor_stage = target(rootfs, 'tmp/piano-sensors-packages')
        sensor_stage.mkdir(parents=True, exist_ok=True)
        for row in sensors['packages'].values():
            shutil.copy2(sensor_folder / row['file'], sensor_stage / row['file'])
            if digest(sensor_stage / row['file']) != row['sha256']:
                raise ValueError('Piano sensors package changed while staging: ' + row['file'])
        ffmpeg_stage = target(rootfs, 'tmp/piano-ffmpeg-packages')
        if ffmpeg:
            ffmpeg_stage.mkdir(parents=True, exist_ok=True)
            for row in ffmpeg['packages'].values():
                shutil.copy2(ffmpeg_folder / row['file'], ffmpeg_stage / row['file'])
                if digest(ffmpeg_stage / row['file']) != row['sha256']:
                    raise ValueError('FFmpeg package changed while staging: ' + row['file'])
        with build_devices(rootfs), build_resolver(rootfs):
            run(['chroot', rootfs, 'apt-get', 'update'])
            local_packages = ['/tmp/piano-sensors-packages/' + row['file']
                              for row in sensors['packages'].values()]
            local_packages.extend('/tmp/piano-gnome-packages/' + row['file']
                                  for row in gsd['packages'].values())
            local_packages.extend('/tmp/piano-gnome-packages/' + row['file']
                                  for row in mutter['packages'].values())
            local_packages.extend('/tmp/piano-gnome-packages/' + row['file']
                                  for row in gdm['packages'].values())
            local_packages.extend('/tmp/piano-gnome-packages/' + row['file']
                                  for row in pipewire['packages'].values())
            if ffmpeg:
                local_packages.extend('/tmp/piano-ffmpeg-packages/' + row['file']
                                      for row in ffmpeg['packages'].values())
            run(['chroot', rootfs, 'env', 'DEBIAN_FRONTEND=noninteractive', 'apt-get', 'install', '-y',
                 '--no-install-recommends', *cfg['debian']['extra_packages'], *local_packages])
            if desktop == 'kde':
                # A resumed generated KDE root may retain the old GNOME backend.
                run(['chroot', rootfs, 'env', 'DEBIAN_FRONTEND=noninteractive',
                     'apt-get', 'purge', '-y', 'tuned-ppd', 'tuned'])
            if 'chromium' in cfg['debian']['extra_packages']:
                # The selected browser is installed before removing the old
                # base's Firefox package. User browser profiles are untouched.
                run(['chroot', rootfs, 'env', 'DEBIAN_FRONTEND=noninteractive',
                     'apt-get', 'purge', '-y', 'firefox-esr'])
        shutil.rmtree(sensor_stage)
        shutil.rmtree(gsd_stage)
        if ffmpeg:
            shutil.rmtree(ffmpeg_stage)
        sensor_unit = target(rootfs, 'usr/lib/systemd/system/adsprpcd-sensorspd.service')
        sensor_unit.write_text(sensor_unit.read_text().replace(' piano-adsp.service', ''))
        if record.get('resume') and (out / 'adapters').is_dir(): shutil.rmtree(out / 'adapters')
        if record.get('resume'):
            staged_modules = target(rootfs, 'usr/lib/modules') / m['kernel_release']
            if staged_modules.is_dir(): shutil.rmtree(staged_modules)
        userspace.stage(rootfs, public, desktop=desktop); hardware.build(out / 'adapters', public, rootfs)
        if desktop == 'kde':
            from prepare_debian_desktop import configure
            configure(rootfs, subprocess.run)
        # Compile the staged local GNOME defaults after the public builder's
        # dconf update, so animation/power policy is actually present in root.
        if desktop == 'gnome':
            run(['chroot', rootfs, 'dconf', 'update'])
        previous_release = record.get('previous_kernel_release')
        remove_releases = ([previous_release] if previous_release and previous_release != m['kernel_release']
                           and (rootfs / 'usr/lib/modules' / previous_release).is_dir() else [])
        modules.stage(rootfs, kernel, remove_releases)
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
                     '--cc', helper_cc, '--macros', record['macros'], '--v4l2-source', record['v4l2_source']])
            runtime.stage(bundle, rootfs)
        from build_piano_fingerprint import install_desktop
        install_desktop(rootfs, bundle, out / 'fingerprint-packages')
        from stage_piano_firmware import stage as stage_firmware
        stage_firmware(Path(record['firmware_source']), rootfs)
        apply_policy(rootfs, out, cfg)
        boot_request = native_boot_request(rootfs, out)
        sanitize(rootfs, out)
        run(['chroot', rootfs, '/usr/sbin/depmod', '-a', m['kernel_release']])
        (rootfs / 'usr/sbin/policy-rc.d').unlink()
        packages = capture(['chroot', rootfs, 'dpkg-query', '-W', '-f=${binary:Package}\t${Version}\t${Architecture}\t${db:Status-Status}\n'])
        (out / 'packages.tsv').write_text(packages + '\n')
        installed = {p[0].split(':')[0]: p[1] for line in packages.splitlines() if len(p := line.split('\t')) == 4 and p[3] == 'installed'}
        defaults = default_system_record(rootfs, installed, cfg, desktop)
        for package, row in sensors['packages'].items():
            if installed.get(package) != row['version']:
                raise ValueError('Installed Piano sensors version differs: ' + package)
        if ffmpeg:
            for package, row in ffmpeg['packages'].items():
                if installed.get(package) != row['version']:
                    raise ValueError('Installed FFmpeg version differs: ' + package)
        for package, row in gsd['packages'].items():
            if installed.get(package) != row['version']:
                raise ValueError('Installed GNOME power package version differs: ' + package)
        for package, row in mutter['packages'].items():
            if installed.get(package) != row['version']:
                raise ValueError('Installed Mutter package version differs: ' + package)
        for package, row in gdm['packages'].items():
            if installed.get(package) != row['version']:
                raise ValueError('Installed GDM package version differs: ' + package)
        for package, row in pipewire['packages'].items():
            if installed.get(package) != row['version']:
                raise ValueError('Installed PipeWire plugin version differs: ' + package)
        metadata = {str(p.relative_to(rootfs)): digest(p) for p in (rootfs / 'var/lib/apt/lists').glob('*InRelease')}
        if not metadata: raise ValueError('Signed APT InRelease metadata is missing')
        metadata.update({str(p.relative_to(rootfs)): digest(p) for p in (rootfs / 'etc/apt').rglob('*') if p.is_file()})
        snapshot = target(rootfs, 'usr/lib/piano/piano-boot-task-snapshot')
        shutil.copy2(ROOT / 'linux/userspace/piano-boot-task-snapshot', snapshot)
        snapshot.chmod(0o755)
        boot = bootstrap(rootfs, kernel, out, m['kernel_release'], record.get('boot_task_snapshot', False))
        clock = clock_epoch(rootfs)
        if modules.inspect(kernel)[1] != kernel_hash: raise ValueError('Kernel changed during rootfs build')
        result = {'status': 'HOST_BUILT_RELEASE_' + desktop.upper() + '_ROOT_NOT_BOOT_VERIFIED', 'rootfs': str(rootfs),
                  'root_policy': 'LABEL=PIANOROOT', 'kernel_release': m['kernel_release'], 'kernel_commit': m['source_commit'],
                  'kernel_manifest_sha256': kernel_hash, 'release_config_sha256': digest(POLICY), 'packages_sha256': digest(out / 'packages.tsv'),
                  'adapters_manifest_sha256': digest(out / 'adapters/manifest.json'), 'runtime_manifest_sha256': digest(bundle / 'manifest.json'),
                  'apt_policy': cfg['debian']['apt_policy'], 'apt_metadata': metadata,
                  'debian_archive': archive, 'debian_suite': cfg['debian']['suite'],
                  'debian_commit': cfg['debian']['commit'],
                  'sensors': sensors, 'ffmpeg': ffmpeg, 'gnome_power': gsd,
                  'mutter': mutter, 'gdm': gdm, 'pipewire': pipewire,
                  'boot_request': boot_request, 'firmware_components': cfg['firmware']['components'],
                  'clock_epoch': clock,
                  'default_system': defaults, 'desktop': desktop,
                  'initramfs': boot, 'password': cfg['login']['password'], 'autologin_retained': True,
                  'bit_reproducible': False, 'root_uid': rootfs.stat().st_uid, 'device_operation_performed': False}
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
    for name in ('sensors-dir', 'ffmpeg-dir', 'mutter-dir', 'runtime-dir', 'kernel-build', 'public-source', 'firmware-source', 'macros', 'v4l2-source'): parser.add_argument('--' + name, type=Path)
    mode = parser.add_mutually_exclusive_group(required=True); mode.add_argument('--plan', action='store_true'); mode.add_argument('--execute', action='store_true')
    parser.add_argument('--resume', action='store_true', help='Reuse the completed upstream base and refresh release assembly')
    parser.add_argument('--desktop', choices=('gnome', 'kde'), default='gnome')
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
