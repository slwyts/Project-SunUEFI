#!/usr/bin/env python3
"""Inspect, stage and package Piano device support offline."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import pwd
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

from build_piano_ram_bootstrap import guest_resolve
from build import overlay_release

ROOT = Path(__file__).resolve().parents[1]
BSP = ROOT / 'linux/bsp'
NAME = 'piano-device-config'
TOUCH_PACKAGE = 'piano-firmware-touch'
TOUCH_MANIFEST = ROOT / 'vendor/firmware/piano-touch/manifest.json'
FIRMWARE_ROOT = 'usr/lib/firmware/'


def package_description(package):
    if package == TOUCH_PACKAGE:
        return 'Xiaomi Pad 8 Pro Novatek touchscreen firmware'
    return 'Xiaomi Pad 8 Pro device configuration'


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_json(path):
    return json.loads(path.read_text())


def payload(features):
    manifest = read_json(BSP / 'manifest.json')
    rows = []
    for row in manifest['files']:
        if row['group'] != 'common' and row['group'] not in features:
            continue
        name = row['path']
        require(not PurePosixPath(name).is_absolute() and '..' not in PurePosixPath(name).parts, 'Unsafe BSP path')
        source = BSP / ('common' if row['group'] == 'common' else 'optional/' + row['group']) / name
        require(source.is_symlink() == (row['type'] == 'symlink'), 'BSP file type changed: ' + name)
        data = os.readlink(source).encode() if source.is_symlink() else source.read_bytes()
        require(sha(data) == row['sha256'], 'BSP payload drift: ' + name)
        if source.is_symlink():
            target = posixpath.normpath(str(PurePosixPath(name).parent / data.decode()))
            require(not data.startswith(b'/') and not target.startswith('../'), 'BSP link escapes payload')
        else:
            script = row['mode'] == '0755' and data.startswith(b'#!')
            require(not data.startswith(b'\x7fELF') and
                    (row['mode'] == '0644' or script),
                    'BSP requires text configuration or an interpreted script: ' + name)
        if row.get('patch'):
            require(sha((BSP / row['patch']).read_bytes()) == row['patch_sha256'], 'BSP patch drift')
        rows.append((row, data))
    return manifest, rows


def touch_payload(source):
    manifest = read_json(TOUCH_MANIFEST)
    require(manifest['directory'] == 'novatek' and len(manifest['files']) == 2,
            'Expected the two Novatek panel firmware files')
    rows = []
    for filename, record in manifest['files'].items():
        require(Path(filename).name == filename and filename not in ('', '.', '..'),
                'Invalid touch firmware filename: ' + filename)
        path = source / filename
        require(path.is_file() and not path.is_symlink(),
                'Missing regular touch firmware file: ' + str(path))
        require(path.stat().st_size == record['bytes'],
                'Touch firmware size mismatch: ' + filename)
        data = path.read_bytes()
        require(len(data) == record['bytes'] and sha(data) == record['sha256'],
                'Touch firmware checksum mismatch: ' + filename)
        rows.append(({
            'path': FIRMWARE_ROOT + manifest['directory'] + '/' + filename,
            'type': 'file',
            'mode': '0644',
            'sha256': record['sha256'],
        }, data))
    return manifest, rows


def owners(root):
    result = {}
    info = guest_resolve(root, '/var/lib/dpkg/info')
    if info.is_dir():
        for path in info.glob('*.list'):
            require(not path.is_symlink(), 'Symlink in package ownership database')
            for line in path.read_text().splitlines():
                result.setdefault(line.lstrip('/'), set()).add(path.name[:-5].split(':')[0])
    for database in ('/var/lib/pacman/local', '/usr/lib/holo/pacmandb/local'):
        db = guest_resolve(root, database)
        for path in db.glob('*/files'):
            require(not path.is_symlink(), 'Symlink in package ownership database')
            section = ''
            for line in path.read_text().splitlines():
                if line.startswith('%'):
                    section = line
                elif section == '%FILES%' and line:
                    result.setdefault(line.rstrip('/'), set()).add(path.parent.name.rsplit('-', 2)[0])
    return result


def conflict_check(root, rows, package=NAME):
    root = root.resolve()
    require(root.is_dir(), 'Conflict-check root does not exist')
    ownership, conflicts = owners(root), []
    for row, _ in rows:
        name = row['path']
        names = [name]
        if package == TOUCH_PACKAGE and name.startswith(FIRMWARE_ROOT):
            names = [base + suffix for base in (name, name.removeprefix('usr/'))
                     for suffix in ('', '.xz', '.zst')]
        resolved = {}
        for candidate in names:
            relative = PurePosixPath(candidate)
            path = guest_resolve(root, '/' + str(relative.parent)) / relative.name
            resolved.setdefault(path, set()).update(ownership.get(candidate, ()))
        for path, assigned in resolved.items():
            if assigned - {package} or ((path.exists() or path.is_symlink()) and assigned != {package}):
                conflicts.append({'path': path.relative_to(root).as_posix(),
                                  'owners': sorted(assigned) or ['unowned-existing-file']})
    require(not conflicts, 'Existing target files must not be overwritten: ' + json.dumps(conflicts))
    return {'performed': True, 'root_was_modified': False, 'conflicts': []}


def inspect_touch_firmware(args, suite):
    require(args.target in ('debian', 'arch'), 'Use --target debian or arch for touch firmware')
    require(not args.feature, 'Configuration features cannot be added to the firmware package')
    require(overlay_release.ENVIRONMENT not in os.environ,
            'Local touch firmware cannot be built as a publication generation')
    require(args.command != 'package' or args.against,
            'Specify --against with the target root to check touch firmware ownership')
    manifest, rows = touch_payload(args.touch_firmware)
    report = {
        'schema_version': 1,
        'component': 'touch-firmware',
        'package': TOUCH_PACKAGE,
        'version': args.version,
        'target': {'distro': args.target, 'suite': suite, 'arch': args.arch},
        'package_architecture': 'any' if args.target == 'arch' else 'all',
        'source': {'rom': manifest['rom'], 'origin': manifest['source']},
        'public_distribution': False,
        'depends': [],
        'package_ready': False,
        'conflict_check': (conflict_check(args.against, rows, TOUCH_PACKAGE) if args.against
                           else {'performed': False, 'required_before_install': True}),
        'payload_files': {row['path']: {**row, 'bytes': len(data)} for row, data in rows},
        'files': {},
    }
    return report, rows


def inspect(args):
    require(re.fullmatch('[0-9][A-Za-z0-9.+]*', args.version), 'Invalid package version')
    profile = read_json(BSP / 'profiles.json')['profiles'][args.target]
    suite = args.suite or profile['default_suite']
    require(suite and re.fullmatch('[A-Za-z0-9_.-]+', suite), 'Specify an explicit Deepin/target suite')
    if args.touch_firmware is not None:
        return inspect_touch_firmware(args, suite)
    source, rows = payload(args.feature)
    ledger = read_json(BSP / 'layers.json')
    layers = ledger['required_layers']
    version = args.version
    if args.format == 'deb':
        version = overlay_release.debian_version(version)
    report = {'schema_version': 1, 'component': 'config', 'package': NAME, 'version': version,
              'status': 'HOST_CONFIG_NOT_DEVICE_TESTED', 'target': {'distro': args.target, 'suite': suite, 'arch': args.arch},
              'package_architecture': 'any' if args.target == 'arch' else 'all',
              'features': args.feature, 'source': source['source'], 'required_layers': layers,
              'selected_optional_layers': ledger['optional_layers'] if 'camera' in args.feature else {},
              'depends': profile['depends'] + (profile['camera_depends'] if 'camera' in args.feature else []),
              'dependency_resolution_tested': False, 'native_layer_abi_verified': False, 'device_tested': False,
              'auto_enable_services': False, 'package_ready': False,
              'conflict_check': conflict_check(args.against, rows) if args.against else {'performed': False, 'required_before_install': True},
              'payload_files': {row['path']: {**{k: v for k, v in row.items() if k != 'group'}, 'bytes': len(data)} for row, data in rows}, 'files': {}}
    return report, rows


def stage(args):
    report, rows = inspect(args)
    require(args.output is not None and not args.output.exists() and not args.output.is_symlink(), 'Use a fresh output directory')
    output = args.output.absolute()
    output.mkdir(parents=True)
    (output / '.incomplete').write_text('Not a complete BSP package.\n')
    tree = output / 'payload'
    tree.mkdir()
    for row, data in rows:
        path = tree / row['path']
        path.parent.mkdir(parents=True, exist_ok=True)
        if row['type'] == 'symlink':
            path.symlink_to(data.decode())
        else:
            path.write_bytes(data)
            path.chmod(int(row['mode'], 8))
    # Installed provenance records source data without local build paths.
    docs = tree / 'usr/share/doc' / report['package']
    if report['package'] == TOUCH_PACKAGE:
        docs.mkdir(parents=True)
        (docs / 'copyright').write_text(
            'Novatek / Xiaomi proprietary firmware from the stock device ROM.\n'
            'The SunUEFI source license does not cover these binaries.\n'
            'No permission for public redistribution is documented.\n')
    provenance = docs / 'bsp-manifest.json'
    provenance.write_text(json.dumps({k: v for k, v in report.items() if k != 'files'}, indent=2) + '\n')
    for path in [tree, *tree.rglob('*')]:
        if path.is_dir() and not path.is_symlink():
            path.chmod(0o755)
    return output, tree, report, rows


def preinst(paths, package=NAME):
    guard = '#!/bin/sh\nset -eu\n'
    if package == TOUCH_PACKAGE:
        firmware = ' '.join(shlex.quote('/' + name) for name in paths if name.startswith(FIRMWARE_ROOT))
        guard += (f'for path in {firmware}; do\n'
                  '  for suffix in .xz .zst; do\n'
                  '    if [ -e "$path$suffix" ] || [ -L "$path$suffix" ]; then\n'
                  '      printf "%s\\n" "BSP file conflict: $path$suffix" >&2; exit 1\n'
                  '    fi\n'
                  '  done\n'
                  'done\n')
    quoted = ' '.join(shlex.quote('/' + name) for name in paths)
    return (guard +
            '# Refuse cross-package ownership and unowned existing files; no force-overwrite.\n'
            f'for path in {quoted}; do\n'
            '  if [ -e "$path" ] || [ -L "$path" ]; then\n'
            '    owned=$(dpkg-query --search "$path" 2>/dev/null || true)\n'
            f'    case "$owned" in "{package}: $path"|"{package}:all: $path") ;;\n'
            '      *) printf "%s\\n" "BSP file conflict: $path ($owned)" >&2; exit 1 ;;\n'
            '    esac\n'
            '  fi\ndone\n')


def debian_package(output, tree, report, configuration):
    package = report['package']
    control = tree / 'DEBIAN'
    control.mkdir()
    control.chmod(0o755)
    section = 'non-free-firmware/kernel' if package == TOUCH_PACKAGE else 'misc'
    depends = 'Depends: ' + ', '.join(report['depends']) + '\n' if report['depends'] else ''
    detail = (' Built from locally supplied stock ROM files.\n' if package == TOUCH_PACKAGE
              else ' Native runtime, Mesa, firmware and modules are distributed separately.\n')
    (control / 'control').write_text(
        f"Package: {package}\nVersion: {report['version']}\nArchitecture: all\n"
        f'Maintainer: Project SunUEFI <slwyts@tuta.io>\nSection: {section}\nPriority: optional\n'
        f'{depends}Description: {package_description(package)}\n{detail}')
    if configuration:
        (control / 'conffiles').write_text(''.join('/' + name + '\n' for name in configuration))
    docs = 'usr/share/doc/' + package + '/'
    paths = [*report['payload_files'], docs + 'bsp-manifest.json']
    if package == TOUCH_PACKAGE:
        paths.append(docs + 'copyright')
    (control / 'preinst').write_text(preinst(paths, package))
    (control / 'preinst').chmod(0o755)
    subprocess.run(['/bin/sh', '-n', control / 'preinst'], check=True)
    artifact = output / f"{package}_{report['version']}_all.deb"
    subprocess.run(['dpkg-deb', '--root-owner-group', '--build', tree, artifact],
                   check=True, stdout=sys.stderr, env={**os.environ, 'SOURCE_DATE_EPOCH': '0'})
    shutil.rmtree(control)
    return artifact


def pacman_sources(directory, tree, report, configuration):
    package = report['package']
    archive = directory / 'payload.tar'
    subprocess.run(['tar', '--sort=name', '--numeric-owner', '--owner=0', '--group=0',
                    '--mtime=@0', '--format=pax', '--pax-option=delete=atime,delete=ctime',
                    '-C', tree, '-cf', archive, '.'], check=True)
    depends = ' '.join(map(shlex.quote, report['depends']))
    backup = ' '.join(map(shlex.quote, configuration))
    license_ids = 'LicenseRef-Proprietary' if package == TOUCH_PACKAGE else 'MIT BSD-2-Clause-Patent'
    (directory / 'PKGBUILD').write_text(
        f"pkgname={package}\npkgver={report['version']}\npkgrel={overlay_release.pacman_release('1')}\n"
        f'pkgdesc={shlex.quote(package_description(package))}\n'
        "url='https://github.com/slwyts/Project-SunUEFI'\n"
        f'arch=(any)\nlicense=({license_ids})\noptions=(!strip !debug)\n'
        f'depends=({depends})\nbackup=({backup})\n'
        'source=(payload.tar)\nnoextract=(payload.tar)\n'
        f"sha256sums=('{sha(archive.read_bytes())}')\n"
        'package() {\n'
        '  bsdtar --no-same-owner -xf "$srcdir/payload.tar" -C "$pkgdir"\n'
        '  install -d "$pkgdir/usr/share/licenses/$pkgname"\n'
        '  ln -s "../../doc/$pkgname/copyright" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"\n'
        '}\n')


def pacman_builder_account():
    if os.geteuid() != 0:
        return None
    try:
        return pwd.getpwnam('sunuefi-build')
    except KeyError:
        if Path('/.dockerenv').is_file():
            subprocess.run(['useradd', '--system', '--create-home', 'sunuefi-build'], check=True)
            return pwd.getpwnam('sunuefi-build')
        uid = os.environ.get('SUDO_UID', '')
        if uid.isdecimal() and int(uid) > 0:
            return pwd.getpwuid(int(uid))
        raise ValueError('Run makepkg as a regular user or in the project builder')


def pacman_package(output, tree, report, configuration):
    # Only the packed sources and recipe are exposed to the unprivileged
    # builder, not the rootfs or repo signing files.
    account = pacman_builder_account()
    with tempfile.TemporaryDirectory(prefix='sunuefi-bsp-', dir='/tmp' if account else output) as folder:
        work = Path(folder)
        pacman_sources(work, tree, report, configuration)
        # This package only copies checked data. Its runtime dependencies are
        # resolved on installation, not installed into the build machine.
        env = {**os.environ, 'SOURCE_DATE_EPOCH': '0', 'LC_ALL': 'C',
               'BUILDDIR': str(work), 'SRCDEST': str(work), 'PKGDEST': str(work),
               'LOGDEST': str(work), 'PKGEXT': '.pkg.tar.zst',
               'PACKAGER': 'Project SunUEFI <slwyts@tuta.io>'}
        command = ['makepkg', '--noconfirm', '--nodeps']
        if account is not None:
            for path in (work, *work.iterdir()):
                os.chown(path, account.pw_uid, account.pw_gid, follow_symlinks=False)
            command = ['runuser', '-u', account.pw_name, '--', *command]
        subprocess.run(command, cwd=work, env=env, stdout=sys.stderr, check=True)
        version = report['version'] + '-' + overlay_release.pacman_release('1')
        artifact = work / f"{report['package']}-{version}-any.pkg.tar.zst"
        require(artifact.is_file() and not artifact.is_symlink(),
                'makepkg did not produce the Piano package')
        target = output / artifact.name
        shutil.copyfile(artifact, target)
        return target, version


def build(args):
    if args.command == 'inspect':
        return inspect(args)[0]
    if args.command == 'package':
        require(args.format, 'Select --format deb or pacman')
        require((args.format == 'pacman') == (args.target == 'arch'),
                'Use --format pacman for Arch and --format deb for Debian derivatives')
        backend = 'makepkg' if args.format == 'pacman' else 'dpkg-deb'
        require(shutil.which(backend), 'Missing package backend: ' + backend)
        if args.format == 'pacman':
            require(shutil.which('tar'), 'Missing tar')
    output, tree, report, rows = stage(args)
    if args.command == 'package':
        configuration = [row['path'] for row, _ in rows if row['path'].startswith('etc/')]
        if args.format == 'pacman':
            artifact, version = pacman_package(output, tree, report, configuration)
        else:
            artifact = debian_package(output, tree, report, configuration)
            version = report['version']
        report['package_ready'] = True
        report['files'][artifact.name] = {'bytes': artifact.stat().st_size,
                                          'sha256': sha(artifact.read_bytes()), 'format': args.format,
                                          'version': version}
    for path in [tree, *tree.rglob('*')]:
        if path.is_dir():
            path.chmod(0o755)
    (output / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    (output / '.incomplete').unlink()
    return report


def parser():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('command', choices=('inspect', 'stage', 'package'))
    ap.add_argument('--target', choices=('debian', 'ubuntu', 'deepin', 'arch'), default='debian')
    ap.add_argument('--suite')
    ap.add_argument('--arch', choices=('aarch64',), default='aarch64')
    ap.add_argument('--feature', choices=('camera',), action='append', default=[])
    ap.add_argument('--format', choices=('deb', 'pacman'))
    ap.add_argument('--against', type=Path, help='Readonly target-root conflict/ownership check')
    ap.add_argument('--touch-firmware', type=Path,
                    help='Use the two local stock touch firmware files instead of packaging device configuration')
    ap.add_argument('--output', type=Path)
    ap.add_argument('--version', default='0.1.0')
    return ap


if __name__ == '__main__':
    try:
        print(json.dumps(build(parser().parse_args()), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
