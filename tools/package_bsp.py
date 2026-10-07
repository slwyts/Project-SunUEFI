#!/usr/bin/env python3
"""Inspect/stage/package the tracked, configuration-only Piano BSP offline."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import shlex
import shutil
import subprocess

from build_piano_ram_bootstrap import guest_resolve

ROOT = Path(__file__).resolve().parents[1]
BSP = ROOT / 'linux/bsp'
NAME = 'piano-device-config'


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
            require(not data.startswith(b'\x7fELF') and row['mode'] == '0644', 'Native helper is not a configuration file')
        if row.get('patch'):
            require(sha((BSP / row['patch']).read_bytes()) == row['patch_sha256'], 'BSP patch drift')
        rows.append((row, data))
    return manifest, rows


def owners(root):
    result = {}
    info = guest_resolve(root, '/var/lib/dpkg/info')
    if info.is_dir():
        for path in info.glob('*.list'):
            require(not path.is_symlink(), 'Symlink in package ownership database')
            for line in path.read_text().splitlines():
                result.setdefault(line.lstrip('/'), set()).add(path.name[:-5].split(':')[0])
    db = guest_resolve(root, '/var/lib/pacman/local')
    if db.is_dir():
        for path in db.glob('*/files'):
            require(not path.is_symlink(), 'Symlink in package ownership database')
            section = ''
            for line in path.read_text().splitlines():
                if line.startswith('%'):
                    section = line
                elif section == '%FILES%' and line:
                    result.setdefault(line.rstrip('/'), set()).add(path.parent.name.rsplit('-', 2)[0])
    return result


def conflict_check(root, rows):
    root = root.resolve()
    require(root.is_dir(), 'Conflict-check root does not exist')
    ownership, conflicts = owners(root), []
    for row, _ in rows:
        name = row['path']
        path = guest_resolve(root, '/' + str(PurePosixPath(name).parent)) / PurePosixPath(name).name
        assigned = ownership.get(name, set())
        if assigned - {NAME} or ((path.exists() or path.is_symlink()) and assigned != {NAME}):
            conflicts.append({'path': name, 'owners': sorted(assigned) or ['unowned-existing-file']})
    require(not conflicts, 'Existing target files must not be overwritten: ' + json.dumps(conflicts))
    return {'performed': True, 'root_was_modified': False, 'conflicts': []}


def inspect(args):
    require(re.fullmatch('[0-9][A-Za-z0-9.+]*', args.version), 'Invalid package version')
    profile = read_json(BSP / 'profiles.json')['profiles'][args.target]
    suite = args.suite or profile['default_suite']
    require(suite and re.fullmatch('[A-Za-z0-9_.-]+', suite), 'Specify an explicit Deepin/target suite')
    source, rows = payload(args.feature)
    ledger = read_json(BSP / 'layers.json')
    layers = ledger['required_layers']
    report = {'schema_version': 1, 'component': 'config', 'package': NAME, 'version': args.version,
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
    # Installed provenance contains public source/metadata, never host paths.
    provenance = tree / 'usr/share/doc' / NAME / 'bsp-manifest.json'
    provenance.write_text(json.dumps({k: v for k, v in report.items() if k != 'files'}, indent=2) + '\n')
    for path in [tree, *tree.rglob('*')]:
        if path.is_dir() and not path.is_symlink():
            path.chmod(0o755)
    return output, tree, report, rows


def preinst(paths):
    quoted = ' '.join(shlex.quote('/' + name) for name in paths)
    return ('#!/bin/sh\nset -eu\n'
            '# Refuse cross-package ownership and unowned existing files; no force-overwrite.\n'
            f'for path in {quoted}; do\n'
            '  if [ -e "$path" ] || [ -L "$path" ]; then\n'
            '    owned=$(dpkg-query --search "$path" 2>/dev/null || true)\n'
            f'    case "$owned" in "{NAME}: $path"|"{NAME}:all: $path") ;;\n'
            '      *) printf "%s\\n" "BSP file conflict: $path ($owned)" >&2; exit 1 ;;\n'
            '    esac\n'
            '  fi\ndone\n')


def package(args, output, tree, report, rows):
    require(args.format in ('deb', 'tar'), 'Select --format deb or tar')
    if args.format == 'deb':
        require(args.target != 'arch', 'Arch requires a native recipe/package, not a Debian deb')
        require(shutil.which('dpkg-deb'), 'Missing dpkg-deb; no simulated .deb is generated')
        control = tree / 'DEBIAN'
        control.mkdir()
        control.chmod(0o755)
        (control / 'control').write_text(f'Package: {NAME}\nVersion: {args.version}\nArchitecture: all\n'
                                       f'Maintainer: Project SunUEFI\nSection: misc\nPriority: optional\n'
                                       'Depends: ' + ', '.join(report['depends']) + '\n'
                                       'Description: Piano device configuration (target profile candidate)\n'
                                       ' Configuration only; native runtime, Mesa, firmware and modules are separate.\n')
        (control / 'conffiles').write_text(''.join('/' + row['path'] + '\n' for row, _ in rows if row['path'].startswith('etc/')))
        (control / 'preinst').write_text(preinst(report['payload_files']))
        (control / 'preinst').chmod(0o755)
        subprocess.run(['/bin/sh', '-n', control / 'preinst'], check=True)
        artifact = output / f'{NAME}_{args.version}_all.deb'
        subprocess.run(['dpkg-deb', '--root-owner-group', '--build', tree, artifact], check=True,
                       env={**os.environ, 'SOURCE_DATE_EPOCH': '0'})
        shutil.rmtree(control)
        report['package_ready'] = True
        report['files'][artifact.name] = {'bytes': artifact.stat().st_size, 'sha256': sha(artifact.read_bytes()), 'format': 'deb'}
    else:
        require(shutil.which('tar'), 'Missing tar')
        artifact = output / 'payload.tar'
        subprocess.run(['tar', '--sort=name', '--numeric-owner', '--owner=0', '--group=0', '--mtime=@0',
                        '--format=pax', '--pax-option=delete=atime,delete=ctime', '-C', tree, '-cf', artifact, '.'], check=True)
        report['files'][artifact.name] = {'bytes': artifact.stat().st_size, 'sha256': sha(artifact.read_bytes()), 'format': 'tar'}
        if args.archrecipe:
            require(args.target == 'arch', '--archrecipe requires --target arch')
            recipe = output / 'PKGBUILD'
            depends = ' '.join(shlex.quote(dep) for dep in report['depends'])
            recipe.write_text(f'pkgname={NAME}\npkgver={args.version}\npkgrel=1\narch=(any)\nlicense=(MIT)\n'
                              f'pkgdesc="Piano device configuration; native layers separate"\ndepends=({depends})\n'
                              'source=(payload.tar)\nnoextract=(payload.tar)\n'
                              f"sha256sums=('{report['files']['payload.tar']['sha256']}')\n"
                              'package() {\n  bsdtar --no-same-owner -xf "$srcdir/payload.tar" -C "$pkgdir"\n}\n')
            subprocess.run(['/bin/bash', '-n', recipe], check=True)
            report['files'][recipe.name] = {'bytes': recipe.stat().st_size, 'sha256': sha(recipe.read_bytes()), 'format': 'arch-recipe'}


def build(args):
    if args.command == 'inspect':
        return inspect(args)[0]
    if args.command == 'package':
        require(args.format, 'Select --format')
        require(shutil.which('dpkg-deb' if args.format == 'deb' else 'tar'), 'Missing package backend: ' + str(args.format))
    output, tree, report, rows = stage(args)
    if args.command == 'package':
        package(args, output, tree, report, rows)
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
    ap.add_argument('--format', choices=('deb', 'tar'))
    ap.add_argument('--archrecipe', action='store_true')
    ap.add_argument('--against', type=Path, help='Readonly target-root conflict/ownership check')
    ap.add_argument('--output', type=Path)
    ap.add_argument('--version', default='0.1.0')
    return ap


if __name__ == '__main__':
    try:
        print(json.dumps(build(parser().parse_args()), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
