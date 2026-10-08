#!/usr/bin/env python3
"""Small distro/desktop dispatcher. Builds userspace, then installs target BSP packages."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import struct
import subprocess
import sys
import tarfile
from urllib.parse import urlsplit
from build_piano_ram_bootstrap import guest_resolve

ROOT = Path(__file__).resolve().parents[1]
LAYERS = {'mesa', 'runtime', 'modules', 'firmware'}


def sha(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''): value.update(chunk)
    return value.hexdigest()


def public_url(value):
    parsed = urlsplit(value)
    if parsed.scheme != 'https' or not parsed.hostname or parsed.username or parsed.password:
        raise ValueError('Use a public HTTPS artifact URL without embedded credentials')
    return value


def inside(root, path):
    if not path.parent.resolve().is_relative_to(root.resolve()): raise ValueError('Base guest symlink redirects a host write')
    return path


def apt_available(text):
    match = re.search(r'^\s*Candidate:\s*(\S+)', text, re.M)
    return bool(match and match[1] != '(none)')


def bundle(folder, target):
    folder = Path(folder).resolve(); data = json.loads((folder / 'manifest.json').read_text())
    if data.get('target') != target: raise ValueError('BSP/layer target distro, suite or architecture differs')
    if data.get('package_ready') is False: raise ValueError('BSP recipe is not a built target package')
    packages = []
    for name, row in data.get('files', {}).items():
        path = folder / name
        if path.is_symlink() or not path.resolve().is_relative_to(folder) or sha(path) != row['sha256']:
            raise ValueError('BSP/layer file identity changed')
        if name.endswith('.deb') or re.search(r'\.pkg\.tar\.(xz|zst|gz)$', name): packages.append(str(path))
    if not packages: raise ValueError('No built distro package; payload/PKGBUILD is a recipe only')
    if any(path.endswith('.deb') != (target['distro'] != 'arch') for path in packages):
        raise ValueError('Package format does not match the target distro')
    return {**data, 'packages': packages, 'manifest_sha256': sha(folder / 'manifest.json')}


def plan(args, root=ROOT):
    root = Path(root).resolve(); profiles = root / 'linux/rootfs'; catalog = json.loads((root / 'linux/desktops/catalog.json').read_text())
    profile_path = profiles / (args.distro + '.json')
    if args.distro not in catalog['matrix'] or args.desktop not in catalog['desktops']:
        raise ValueError('Unknown distro or desktop')
    profile = json.loads(profile_path.read_text()); desktop = catalog['desktops'][args.desktop]
    state = catalog['matrix'][args.distro][args.desktop]
    output = Path(args.output or root / 'build/distros' / (args.distro + '-' + args.desktop)).resolve()
    if output.exists() or not output.is_relative_to(root / 'build/distros'):
        raise ValueError('Choose a new output under project build/distros')
    result = {'status': 'HOST_RECIPE_CANDIDATE', 'distro': args.distro, 'desktop': args.desktop, 'output': str(output),
              'target': {'distro': args.distro, 'suite': args.suite or profile['suite'], 'arch': 'aarch64'},
              'profile': profile, 'desktop_profile': desktop, 'missing_inputs': [], 'device_tested': False,
              'scale_percent': args.scale_percent, 'bsp': None, 'layers': []}
    result['storage'] = json.loads((profiles / 'storage.json').read_text())
    if state == 'unavailable':
        return {**result, 'status': 'UNAVAILABLE', 'reason': 'No reviewed package recipe for this distro/desktop pair'}
    if args.distro != 'deepin' and result['target']['suite'] != profile['suite']:
        raise ValueError('Suite differs from the selected reviewed distro profile')
    if not 100 <= args.scale_percent <= 300 or (args.desktop == 'gnome' and args.scale_percent % 100):
        raise ValueError('Scale is 100..300%; GNOME lightweight integer defaults accept 100/200/300')
    for key in ('base_url', 'base_sha256', 'base_signature_url', 'base_keyring', 'base_keyring_sha256', 'keyring', 'sources_list'):
        result[key] = getattr(args, key, None) or profile.get(key)
    if profile['backend'] != 'debootstrap':
        required = ['base_url', 'base_sha256'] if args.distro == 'arch' else profile['required_inputs']
        for key in required:
            if not (result['target']['suite'] if key == 'suite' else result.get(key)): result['missing_inputs'].append(key)
        for key in ('base_url', 'base_signature_url'):
            if result.get(key): public_url(result[key])
        if result.get('base_sha256') and not re.fullmatch(r'[0-9a-f]{64}', result['base_sha256']): raise ValueError('Invalid base SHA256')
    if args.bsp:
        result['bsp'] = bundle(args.bsp, result['target'])
        result['layers'] = [bundle(path, result['target']) for path in args.layer]
        components = [item.get('component') for item in result['layers']]
        if len(set(components)) != len(components): raise ValueError('Duplicate compiled BSP layer')
        result['missing_inputs'] += sorted(LAYERS - set(components))
        if set(components) - LAYERS: raise ValueError('Unknown compiled BSP layer')
    elif args.layer: raise ValueError('Choose the matching BSP before supplying compiled layers')
    if result['missing_inputs']: result['status'] = 'INPUTS_REQUIRED'
    result['steps'] = ['authenticate/import the selected ARM64 base', 'check target package candidates then install the desktop',
                       'remove generic distro kernels; install only matching BSP/config/compiled packages when supplied',
                       'apply desktop defaults, lock factory passwords, clear generated identities, record actual packages/metadata']
    return result


def validate_archive(path):
    with tarfile.open(path, 'r:*') as archive:
        members = archive.getmembers()
    links = {PurePosixPath(item.name).as_posix().removeprefix('./') for item in members if item.issym()}
    for item in members:
        name = PurePosixPath(item.name)
        if name.is_absolute() or '..' in name.parts or any(p.as_posix().removeprefix('./') in links for p in name.parents):
            raise ValueError('Base archive path or symlink-parent escapes extraction')
        if item.islnk() and (PurePosixPath(item.linkname).is_absolute() or '..' in PurePosixPath(item.linkname).parts):
            raise ValueError('Base archive hardlink escapes extraction')


def deepin_sources(text, suite):
    rows = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith('#'): continue
        match = re.fullmatch(r'deb\s+(https://\S+)\s+(\S+)\s+(.+)', line)
        if not match or match[2] != suite or not urlsplit(match[1]).hostname.endswith(('.deepin.com', '.deepin.org')):
            raise ValueError('Deepin sources must be explicit Deepin-only HTTPS deb entries for this suite; no Debian/Ubuntu mixing')
        rows.append(f'deb [signed-by=/usr/share/keyrings/sunuefi-archive.gpg] {match[1]} {suite} {match[3]}')
    if not rows: raise ValueError('Deepin APT source list is empty')
    return rows


def clean_identity(root, guest):
    if not guest(['getent', 'passwd', 'piano'], check=False).returncode == 0:
        guest(['useradd', '-m', '-s', '/bin/bash', 'piano'])
    accounts = guest(['getent', 'passwd'], text=True)
    for line in accounts.splitlines():
        fields = line.split(':')
        if len(fields) > 2 and (fields[0] == 'root' or 1000 <= int(fields[2]) < 60000): guest(['usermod', '--password', '!', fields[0]])
    for path in inside(root, root / 'etc/ssh/placeholder').parent.glob('ssh_host_*'):
        if path.is_file() or path.is_symlink(): path.unlink()
    for home in [root / 'root', *(root / 'home').glob('*')]:
        ssh = inside(root, home / '.ssh')
        if ssh.is_symlink(): ssh.unlink()
        elif ssh.is_dir(): shutil.rmtree(ssh)
    for name in ('etc/shadow-', 'etc/gshadow-', 'etc/machine-id', 'var/lib/dbus/machine-id'):
        path = inside(root, root / name)
        if path.exists() or path.is_symlink(): path.unlink()
    (root / 'etc/machine-id').write_text('')
    fstab = inside(root, root / 'etc/fstab')
    if fstab.is_symlink(): fstab.unlink()
    fstab.write_text('LABEL=PIANOROOT / ext4 defaults,noatime,x-systemd.growfs 0 1\nLABEL=SUNUEFI_ESP /boot/efi vfat umask=0077,nofail 0 2\n')
    growfs_mask = inside(root, root / 'etc/systemd/system/systemd-growfs-root.service')
    if growfs_mask.is_symlink() and growfs_mask.readlink() == Path('/dev/null'):
        growfs_mask.unlink()
    setup = inside(root, root / 'home/piano/FIRST-SETUP.txt'); setup.parent.mkdir(parents=True, exist_ok=True)
    setup.write_text('No factory password is retained. Set your own piano password using the installer or a trusted root console before privileged desktop tasks.\n')


def configure_desktop(root, result, guest, optional):
    desktop, scale = result['desktop'], result['scale_percent']
    skeleton = inside(root, root / 'etc/skel')
    if skeleton.is_symlink(): raise ValueError('Guest skeleton redirects a host write')
    skeleton.mkdir(parents=True, exist_ok=True)
    if desktop == 'gnome':
        from stage_piano_full_userspace import stage_gnome_power
        result['gnome_power_files'] = stage_gnome_power(root)
        path = inside(root, root / 'etc/dconf/db/local.d/00-sunuefi'); path.parent.mkdir(parents=True, exist_ok=True)
        text = (ROOT / 'linux/desktops/gnome/defaults.ini').read_text().replace('uint32 2', 'uint32 ' + str(scale // 100))
        path.write_text(text); profile = inside(root, root / 'etc/dconf/profile/user'); profile.parent.mkdir(parents=True, exist_ok=True)
        profile.write_text('user-db:user\nsystem-db:local\n'); guest(['dconf', 'update'])
    elif desktop == 'kde':
        path = skeleton / '.config/plasma-workspace/env/sunuefi-scale.sh'; path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f'export QT_SCALE_FACTOR="${{QT_SCALE_FACTOR:-{scale / 100:g}}}"\n')
        entry = root / 'usr/share/applications/com.github.maliit.keyboard.desktop'
        if 'maliit-keyboard' in optional and entry.is_file():
            path = skeleton / '.config/kwinrc'; path.write_text('[Wayland]\nInputMethod=/usr/share/applications/com.github.maliit.keyboard.desktop\n')
    else:
        (skeleton / 'SUNUEFI-FIRST-SETUP.txt').write_text('Set display scaling in DDE Settings. OSK and sensor rotation have not been verified.\n')
    manager = 'arch' if result['distro'] == 'arch' else 'apt'; dm = result['desktop_profile']['display_manager'][manager]
    if dm in ('gdm', 'gdm3'):
        path = root / ('etc/gdm/custom.conf' if dm == 'gdm' else 'etc/gdm3/daemon.conf'); text = '[daemon]\nAutomaticLoginEnable=true\nAutomaticLogin=piano\n'
    elif dm == 'sddm': path = root / 'etc/sddm.conf.d/10-sunuefi.conf'; text = '[Autologin]\nUser=piano\nSession=plasmawayland\n'
    else: path = root / 'etc/lightdm/lightdm.conf.d/10-sunuefi.conf'; text = '[Seat:*]\nautologin-user=piano\n'
    inside(root, path).parent.mkdir(parents=True, exist_ok=True); path.write_text(text)


def execute(result):
    if result['status'] != 'HOST_RECIPE_CANDIDATE': raise ValueError(result.get('reason', 'Required explicit inputs/layers are missing'))
    if os.geteuid(): raise ValueError('--execute requires a root build runner with private mount/chroot capability')
    out = Path(result['output']); root = out / 'rootfs'; profile = result['profile']; arch = profile['package_arch']
    if out.exists(): raise ValueError('Output appeared after planning')
    out.mkdir(parents=True); root.mkdir(); mounted = []; log = out / 'build.log'
    def run(args, **kwargs):
        with log.open('a') as stream: return subprocess.run(list(map(str, args)), stdout=stream, stderr=subprocess.STDOUT, check=True, **kwargs)
    def guest(args, text=False, check=True):
        argv = ['chroot', str(root), '/usr/bin/env', 'PATH=/usr/sbin:/usr/bin:/sbin:/bin', 'SYSTEMD_OFFLINE=1', 'DEBIAN_FRONTEND=noninteractive', *args]
        if text: return subprocess.check_output(argv, text=True)
        if not check: return subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return run(argv)
    try:
        if profile['backend'] == 'debootstrap':
            key = Path(result['keyring']);
            if not key.is_file(): raise ValueError('Install the selected distro archive keyring first')
            run(['debootstrap', '--foreign', '--arch=' + arch, '--include=ca-certificates', '--components=' + ','.join(profile['components']),
                 '--force-check-gpg', '--keyring=' + str(key), result['target']['suite'], root, profile['mirror']])
        else:
            base = out / 'base.tar'; run(['curl', '-fL', '--proto', '=https', '--output', base, result['base_url']])
            if sha(base) != result['base_sha256']: raise ValueError('Downloaded base digest differs')
            if profile['id'] == 'deepin':
                key = Path(result['base_keyring'])
                if sha(key) != result['base_keyring_sha256']: raise ValueError('Base public keyring hash differs')
                sig = out / 'base.sig'; run(['curl', '-fL', '--proto', '=https', '--output', sig, result['base_signature_url']])
                run(['gpgv', '--keyring', key, sig, base])
            validate_archive(base); run(['bsdtar', '-xpf', base, '-C', root])
        cross = platform.machine() not in ('aarch64', 'arm64')
        if cross:
            qemu = shutil.which('qemu-aarch64-static'); entry = Path('/proc/sys/fs/binfmt_misc/qemu-aarch64')
            if not qemu or not entry.is_file() or 'enabled' not in entry.read_text(): raise ValueError('x86 build needs qemu-aarch64-static and enabled binfmt')
            (root / 'usr/bin').mkdir(parents=True, exist_ok=True); shutil.copy2(qemu, root / 'usr/bin/qemu-aarch64-static')
        if profile['backend'] == 'debootstrap': guest(['/debootstrap/debootstrap', '--second-stage'])
        release = dict(re.findall(r'^([A-Z_]+)=(.*)$', guest_resolve(root, '/etc/os-release').read_text(), re.M))
        allowed = {'arch', 'archarm', 'archlinuxarm'} if profile['id'] == 'arch' else {profile['id']}
        if release.get('ID', '').strip('"') not in allowed: raise ValueError('Imported base is not the selected distro')
        executable = guest_resolve(root, '/usr/bin/pacman' if profile['id'] == 'arch' else '/usr/bin/dpkg').read_bytes()[:20]
        if len(executable) < 20 or executable[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', executable, 18)[0] != 183:
            raise ValueError('Imported base package manager is not AArch64')
        for name, kind, options in (('proc', 'proc', ''), ('sys', 'sysfs', 'ro'), ('dev', 'tmpfs', ''), ('run', 'tmpfs', '')):
            path = root / name
            if path.is_symlink(): raise ValueError('Guest virtual mountpoint is a symlink')
            path.mkdir(exist_ok=True); run(['mount', '-t', kind, *(['-o', options] if options else []), kind, path]); mounted.append(path)
        for name, major, minor in (('null', 1, 3), ('zero', 1, 5), ('random', 1, 8), ('urandom', 1, 9), ('tty', 5, 0)):
            run(['mknod', '-m', '666', root / 'dev' / name, 'c', str(major), str(minor)])
        inside(root, root / 'etc/resolv.conf').unlink(missing_ok=True); shutil.copyfile('/etc/resolv.conf', root / 'etc/resolv.conf')
        policy = inside(root, root / 'usr/sbin/policy-rc.d'); policy.parent.mkdir(parents=True, exist_ok=True); policy.write_text('#!/bin/sh\nexit 101\n'); policy.chmod(0o755)
        manager = 'arch' if profile['id'] == 'arch' else 'apt'
        if manager == 'apt':
            rows = profile.get('sources') if profile['id'] != 'deepin' else deepin_sources(Path(result['sources_list']).read_text(), result['target']['suite'])
            for path in (root / 'etc/apt/sources.list.d').glob('*'):
                if path.suffix in ('.list', '.sources'): path.unlink()
            if profile['id'] == 'deepin':
                keyring = inside(root, root / 'usr/share/keyrings/sunuefi-archive.gpg'); keyring.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(result['keyring'], keyring)
            (root / 'etc/apt/sources.list').write_text('\n'.join(rows) + '\n'); guest(['apt-get', 'update'])
        else:
            conf = root / 'etc/pacman.conf'; text = conf.read_text()
            if not re.search(r'^\s*SigLevel\s*=\s*Required\b', text, re.M) or re.search(r'^\s*SigLevel\s*=.*Never', text, re.M): raise ValueError('Pacman repository signatures must remain required')
            conf.write_text(re.sub(r'^\s*Architecture\s*=.*$', 'Architecture = aarch64', text, flags=re.M))
            guest(['pacman-key', '--init']); guest(['pacman-key', '--populate', 'archlinuxarm']); guest(['pacman', '-Syu', '--noconfirm'])
        packages = profile['base_packages'] + result['desktop_profile'][manager + '_packages']; optional = []
        for package in packages + result['desktop_profile'].get('optional_packages', []):
            found = apt_available(guest(['apt-cache', 'policy', package], text=True)) if manager == 'apt' else guest(['pacman', '-Si', package], check=False).returncode == 0
            if not found and package in packages: raise ValueError('UNAVAILABLE target desktop package: ' + package)
            if found and package not in packages: optional.append(package)
        guest((['apt-get', 'install', '-y', '--no-install-recommends'] if manager == 'apt' else ['pacman', '-S', '--needed', '--noconfirm']) + packages + optional)
        inventory = guest(['dpkg-query', '-W', '-f=${binary:Package}\n'], text=True) if manager == 'apt' else guest(['pacman', '-Qq'], text=True)
        generic = [name for name in inventory.splitlines() if name.startswith(('linux-image', 'linux-headers', 'linux-aarch64'))]
        if generic: guest((['apt-get', 'purge', '-y'] if manager == 'apt' else ['pacman', '-R', '--noconfirm']) + generic)
        for item in ([result['bsp']] if result['bsp'] else []) + result['layers']:
            if item.get('component') in ('mesa', 'runtime') and item.get('libc') != 'static':
                actual = guest(['getconf', 'GNU_LIBC_VERSION'], text=True).strip().split()[-1]
                if item.get('glibc_version') != actual: raise ValueError('Compiled layer glibc does not match the target root')
            for package in item['packages']:
                if sha(package) != item['files'][Path(package).name]['sha256']: raise ValueError('Package changed after planning')
                if manager == 'apt':
                    pkg_arch = subprocess.check_output(['dpkg-deb', '-f', package, 'Architecture'], text=True).strip()
                    if pkg_arch not in ([arch, 'all'] if item.get('component') in ('config', 'firmware') else [arch]): raise ValueError('Compiled package architecture differs')
                    if not package.endswith('.deb'): raise ValueError('APT target requires its own .deb packages')
                else:
                    if package.endswith('.deb'): raise ValueError('Arch cannot use a Debian/Ubuntu .deb')
                    info = subprocess.check_output(['bsdtar', '-xOf', package, '.PKGINFO'], text=True)
                    pkg_arch = re.search(r'^arch = (\S+)$', info, re.M)
                    if not pkg_arch or pkg_arch[1] not in (['aarch64', 'any'] if item.get('component') in ('config', 'firmware') else ['aarch64']): raise ValueError('Arch package architecture differs')
            dest = root / 'tmp/bsp-packages'; dest.mkdir(parents=True, exist_ok=True)
            for package in item['packages']: shutil.copy2(package, dest / Path(package).name)
            files = ['/tmp/bsp-packages/' + Path(p).name for p in item['packages']]
            guest((['apt-get', 'install', '-y'] if manager == 'apt' else ['pacman', '-U', '--noconfirm']) + files); shutil.rmtree(dest)
        configure_desktop(root, result, guest, optional); clean_identity(root, guest)
        dm = result['desktop_profile']['display_manager'][manager]; guest(['systemctl', 'enable', dm, 'NetworkManager']); guest(['systemctl', 'set-default', 'graphical.target'])
        packages = guest(['dpkg-query', '-W', '-f=${binary:Package}\t${Version}\t${Architecture}\n'], text=True) if manager == 'apt' else guest(['pacman', '-Q'], text=True)
        (out / 'packages.tsv').write_text(packages); policy.unlink()
        if cross: (root / 'usr/bin/qemu-aarch64-static').unlink()
        result.update(status='HOST_ROOTFS_DESKTOP_BSP_BUILT_NOT_BOOT_VERIFIED' if result['bsp'] else 'HOST_ROOTFS_DESKTOP_BUILT_BSP_PENDING',
                      rootfs=str(root), packages_sha256=sha(out / 'packages.tsv'), optional_packages_installed=optional,
                      generic_kernels_removed=generic, password='factory passwords locked; owner must set their own', bit_reproducible=False)
        metadata = [*(root / 'var/lib/apt/lists').glob('*InRelease'), *(root / 'etc/apt').rglob('*.list'), *(root / 'etc/apt').rglob('*.sources')] if manager == 'apt' else list((root / 'var/lib/pacman/sync').glob('*'))
        result['repository_metadata'] = {str(p.relative_to(root)): sha(p) for p in metadata if p.is_file()}
    except Exception as exc:
        (out / 'FAILED.json').write_text(json.dumps({'status': 'BUILD_FAILED', 'error': str(exc)}) + '\n'); raise
    finally:
        for path in reversed(mounted): run(['umount', path])
    for name in ('dev', 'proc', 'sys', 'run'):
        for path in (root / name).iterdir():
            if path.is_dir() and not path.is_symlink(): shutil.rmtree(path)
            else: path.unlink()
    (out / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n'); return result


def parser():
    value = argparse.ArgumentParser(description=__doc__)
    value.add_argument('--distro', choices=['debian', 'ubuntu', 'arch', 'deepin'], required=True)
    value.add_argument('--desktop', choices=['gnome', 'kde', 'dde'], required=True)
    for name in ('output', 'base-keyring', 'keyring', 'sources-list', 'bsp'): value.add_argument('--' + name, type=Path)
    for name in ('suite', 'base-url', 'base-sha256', 'base-signature-url', 'base-keyring-sha256'): value.add_argument('--' + name)
    value.add_argument('--layer', action='append', type=Path, default=[]); value.add_argument('--scale-percent', type=int, default=200)
    mode = value.add_mutually_exclusive_group(required=True); mode.add_argument('--plan', action='store_true'); mode.add_argument('--execute', action='store_true')
    value.add_argument('--worker', action='store_true', help=argparse.SUPPRESS); return value


def main():
    args = parser().parse_args()
    try:
        result = plan(args)
        if args.execute and not args.worker:
            if os.geteuid(): raise ValueError('--execute requires a privileged root build runner')
            raise SystemExit(subprocess.run(['unshare', '--mount', '--propagation', 'private', sys.executable, __file__, *sys.argv[1:], '--worker']).returncode)
        print(json.dumps(execute(result) if args.execute else result, indent=2, default=str))
    except (OSError, ValueError, subprocess.SubprocessError) as exc: raise SystemExit(str(exc))


if __name__ == '__main__': main()
