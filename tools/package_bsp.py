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
from desktop import power_migration
from package import fingerprint_migration

ROOT = Path(__file__).resolve().parents[1]
BSP = ROOT / 'linux/bsp'
NAME = 'piano-device-config'
TOUCH_PACKAGE = 'piano-firmware-touch'
TOUCH_MANIFEST = ROOT / 'vendor/firmware/piano-touch/manifest.json'
DISPLAY_PACKAGE = 'piano-firmware-display'
DISPLAY_MANIFEST = ROOT / 'vendor/firmware/piano-display/manifest.json'
KEYBOARD_PACKAGE = 'piano-firmware-keyboard'
KEYBOARD_MANIFEST = ROOT / 'vendor/firmware/piano-keyboard/manifest.json'
FINGERPRINT_PACKAGE = 'piano-firmware-fingerprint'
FINGERPRINT_MANIFEST = ROOT / 'vendor/firmware/hyperos4-fingerprint/manifest.json'
FINGERPRINT_TOOLS = 'piano-fingerprint-tools'
STOCK_FIRMWARE = {
    TOUCH_PACKAGE: ('touch-firmware', TOUCH_MANIFEST),
    DISPLAY_PACKAGE: ('display-firmware', DISPLAY_MANIFEST),
    KEYBOARD_PACKAGE: ('keyboard-firmware', KEYBOARD_MANIFEST),
    FINGERPRINT_PACKAGE: ('fingerprint-firmware', FINGERPRINT_MANIFEST),
}
IMAGE_FIRMWARE = {component: (package, manifest)
                  for package, (component, manifest) in STOCK_FIRMWARE.items()
                  if package in (DISPLAY_PACKAGE, KEYBOARD_PACKAGE, FINGERPRINT_PACKAGE)}
PUBLISHED_STOCK_FIRMWARE = {KEYBOARD_PACKAGE}
TOPOLOGY_PACKAGE = 'piano-audio-topology'
POWER_PROFILES_PACKAGE = 'piano-power-profiles'
POWER_PROFILES = BSP / 'power-profiles'
TOPOLOGY_SOURCES = ('debian-piano-current', 'audioreach-topology')
FIRMWARE_PACKAGES = (*STOCK_FIRMWARE, TOPOLOGY_PACKAGE)
FIRMWARE_ROOT = 'usr/lib/firmware/'
OLD_FPRINTD_DROP_IN = 'usr/lib/systemd/system/fprintd.service.d/20-piano.conf'
OLD_FPRINTD_SHA256 = 'be5b439b5040f08ac2ed132dcf7179e2a2423b2357a11f8ffa3d04ad1190404b'


def package_description(package):
    if package == TOUCH_PACKAGE:
        return 'Xiaomi Pad 8 Pro Novatek touchscreen firmware'
    if package == DISPLAY_PACKAGE:
        return 'Xiaomi Pad 8 Pro display calibration firmware'
    if package == KEYBOARD_PACKAGE:
        return 'Xiaomi Pad 8 Pro Nanosic keyboard cover firmware'
    if package == FINGERPRINT_PACKAGE:
        return 'Xiaomi Pad 8 Pro Goodix fingerprint firmware'
    if package == FINGERPRINT_TOOLS:
        return 'Optional Goodix fingerprint diagnostics for the Xiaomi Pad 8 Pro'
    if package == TOPOLOGY_PACKAGE:
        return 'Xiaomi Pad 8 Pro AudioReach topology'
    if package == POWER_PROFILES_PACKAGE:
        return 'Xiaomi Pad 8 Pro CPU and GPU power profiles'
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


def local_firmware_payload(source, manifest_path, description):
    manifest = read_json(manifest_path)
    directory = PurePosixPath(manifest.get('directory', '.'))
    require(not directory.is_absolute() and '..' not in directory.parts,
            'Invalid firmware directory: ' + str(directory))
    require(manifest['files'], 'No files in the manifest for ' + description)
    rows = []
    for filename, record in manifest['files'].items():
        require(Path(filename).name == filename and filename not in ('', '.', '..'),
                'Invalid ' + description + ' filename: ' + filename)
        path = source / filename
        require(path.is_file() and not path.is_symlink(),
                'Missing regular ' + description + ' file: ' + str(path))
        require(path.stat().st_size == record['bytes'],
                description.capitalize() + ' size mismatch: ' + filename)
        data = path.read_bytes()
        require(len(data) == record['bytes'] and sha(data) == record['sha256'],
                description.capitalize() + ' checksum mismatch: ' + filename)
        rows.append(({
            'path': FIRMWARE_ROOT + (directory / filename).as_posix(),
            'type': 'file',
            'mode': '0644',
            'sha256': record['sha256'],
            **{key: record[key] for key in ('source', 'source_sha256') if key in record},
        }, data))
    return manifest, rows


def topology_payload():
    from build_piano_runtime_helpers import build_topology, repository

    policy = read_json(ROOT / 'config/release.json')['audio_topology']
    lock = read_json(ROOT / 'sources.lock.json')
    sources = {name: lock[name] for name in TOPOLOGY_SOURCES}
    checkouts = [ROOT / 'upstream' / name for name in TOPOLOGY_SOURCES]
    for path in checkouts:
        repository(path, sources[path.name]['commit'])
    for tool in ('m4', 'alsatplg'):
        require(shutil.which(tool), 'Missing audio topology build tool: ' + tool)
    env = {**os.environ, 'LC_ALL': 'C'}
    env.pop('M4PATH', None)
    with tempfile.TemporaryDirectory(prefix='sunuefi-topology-') as directory:
        work = Path(directory)
        topology = build_topology(*checkouts, work, env=env)
        data = topology.read_bytes()
        name = FIRMWARE_ROOT + topology.relative_to(work).as_posix()
    require(sha(data) == policy['sha256'], 'Audio topology checksum mismatch')
    return {
        'source': policy['source'],
        'repositories': sources,
    }, [({'path': name, 'type': 'file', 'mode': '0644', 'sha256': policy['sha256']}, data)]


def topology_copyright(source):
    macros = ROOT / 'upstream' / TOPOLOGY_SOURCES[1]
    sources = '\n'.join(f" {record['url']} ({record['commit']})"
                        for record in source['repositories'].values())
    return ('AudioReach topology for Xiaomi Pad 8 Pro, licensed BSD-3-Clause.\n'
            f'SunUEFI source: {source["source"]}\n'
            f'Upstream layout and macros:\n{sources}\n\n'
            + (macros / 'LICENSE.BSD-3-Clause').read_text())


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


def conflict_check(root, rows, package=NAME, *, adopt_identical=False):
    root = root.resolve()
    require(root.is_dir(), 'Conflict-check root does not exist')
    ownership, conflicts, adopted = owners(root), [], []
    for row, data in rows:
        name = row['path']
        names = [name]
        if package in FIRMWARE_PACKAGES and name.startswith(FIRMWARE_ROOT):
            names = [base + suffix for base in (name, name.removeprefix('usr/'))
                     for suffix in ('', '.xz', '.zst')]
        resolved = {}
        for candidate in names:
            relative = PurePosixPath(candidate)
            path = guest_resolve(root, '/' + str(relative.parent)) / relative.name
            if candidate == name:
                destination = path
            resolved.setdefault(path, set()).update(ownership.get(candidate, ()))
        for path, assigned in resolved.items():
            own_destination = path == destination and assigned == {package}
            existing = path.exists() or path.is_symlink()
            if (adopt_identical and path == destination and not assigned
                    and not path.is_symlink() and path.is_file() and path.stat().st_size == len(data)
                    and (path.stat().st_mode & 0o7777) == int(row['mode'], 8)
                    and path.read_bytes() == data):
                adopted.append(path.relative_to(root).as_posix())
            elif assigned - {package} or (existing and not own_destination):
                conflicts.append({'path': path.relative_to(root).as_posix(),
                                  'owners': sorted(assigned) or ['unowned-existing-file']})
    require(not conflicts, 'Existing target files must not be overwritten: ' + json.dumps(conflicts))
    report = {'performed': True, 'root_was_modified': False, 'conflicts': []}
    if adopt_identical:
        report['adopted'] = adopted
    return report


def inspect_stock_firmware(args, suite, package, source):
    component, manifest_path = STOCK_FIRMWARE[package]
    description = component.replace('-', ' ')
    published = package in PUBLISHED_STOCK_FIRMWARE
    require(args.target in ('debian', 'arch'), 'Use --target debian or arch for ' + description)
    require(published or overlay_release.ENVIRONMENT not in os.environ,
            f'Local {description} cannot be built as a publication generation')
    require(args.command != 'package' or args.against or component in IMAGE_FIRMWARE,
            f'Specify --against with the target root to check {description} ownership')
    manifest, rows = local_firmware_payload(source, manifest_path, description)
    provenance = {'rom': manifest['rom']}
    if 'source' in manifest:
        provenance['origin'] = manifest['source']
    version = overlay_release.debian_version(args.version) if published and args.format == 'deb' else args.version
    conflicts = (conflict_check(args.against, rows, package, adopt_identical=package == KEYBOARD_PACKAGE)
                 if args.against else {'performed': False, 'required_before_install': True})
    report = {
        'schema_version': 1,
        'component': component,
        'package': package,
        'version': version,
        'target': {'distro': args.target, 'suite': suite, 'arch': args.arch},
        'package_architecture': 'any' if args.target == 'arch' else 'all',
        'source': provenance,
        'public_distribution': published,
        'depends': [],
        'package_ready': False,
        'conflict_check': conflicts,
        'payload_files': {row['path']: {**row, 'bytes': len(data)} for row, data in rows},
        'files': {},
    }
    return report, rows


def inspect_audio_topology(args, suite):
    source, rows = topology_payload()
    report = {
        'schema_version': 1,
        'component': 'audio-topology',
        'package': TOPOLOGY_PACKAGE,
        'version': overlay_release.debian_version(args.version) if args.format == 'deb' else args.version,
        'target': {'distro': args.target, 'suite': suite, 'arch': args.arch},
        'package_architecture': 'any' if args.target == 'arch' else 'all',
        'source': source,
        'public_distribution': True,
        'depends': [],
        'package_ready': False,
        'conflict_check': (conflict_check(args.against, rows, TOPOLOGY_PACKAGE) if args.against
                           else {'performed': False, 'required_before_install': True}),
        'payload_files': {row['path']: {**row, 'bytes': len(data)} for row, data in rows},
        'files': {},
    }
    return report, rows


def inspect(args):
    firmware = next(((package, source)
                  for package, (component, _) in STOCK_FIRMWARE.items()
                  if (source := getattr(args, component.replace('-', '_'))) is not None), None)
    if args.version is None:
        if firmware and firmware[0] in (KEYBOARD_PACKAGE, FINGERPRINT_PACKAGE):
            _, manifest = STOCK_FIRMWARE[firmware[0]]
            args.version = read_json(manifest)['rom'].removeprefix('OS')
        else:
            separate = firmware is not None or args.audio_topology or args.power_profiles
            args.version = '0.1.0' if separate else read_json(BSP / 'manifest.json')['version']
    require(re.fullmatch('[0-9][A-Za-z0-9.+]*', args.version), 'Invalid package version')
    profile = read_json(BSP / 'profiles.json')['profiles'][args.target]
    suite = args.suite or profile['default_suite']
    require(suite and re.fullmatch('[A-Za-z0-9_.-]+', suite), 'Specify an explicit Deepin/target suite')
    if firmware is not None:
        return inspect_stock_firmware(args, suite, *firmware)
    if args.audio_topology:
        return inspect_audio_topology(args, suite)
    if args.power_profiles:
        return inspect_power_profiles(args, suite)
    source, rows = payload([])
    ledger = read_json(BSP / 'layers.json')
    layers = ledger['required_layers']
    version = args.version
    if args.format == 'deb':
        version = overlay_release.debian_version(version)
    report = {'schema_version': 1, 'component': 'config', 'package': NAME, 'version': version,
              'status': 'HOST_CONFIG_NOT_DEVICE_TESTED', 'target': {'distro': args.target, 'suite': suite, 'arch': args.arch},
              'package_architecture': 'any' if args.target == 'arch' else 'all',
              'source': source['source'], 'required_layers': layers,
              'depends': profile['depends'],
              'dependency_resolution_tested': False, 'native_layer_abi_verified': False, 'device_tested': False,
              'auto_enable_services': False, 'package_ready': False,
              'conflict_check': conflict_check(args.against, rows) if args.against else {'performed': False, 'required_before_install': True},
              'payload_files': {row['path']: {**{k: v for k, v in row.items() if k != 'group'}, 'bytes': len(data)} for row, data in rows}, 'files': {}}
    return report, rows


def power_profiles_payload():
    paths = ('etc/tuned/piano-ppd.conf',
             'usr/lib/systemd/system/tuned-ppd.service.d/50-piano-ppd.conf',
             *(f'usr/lib/tuned/profiles/piano-{mode}/tuned.conf'
               for mode in ('power-saver', 'balanced', 'performance')))
    rows = []
    for path in paths:
        source = POWER_PROFILES / path
        require(source.is_file() and not source.is_symlink(),
                'Missing regular power profile file: ' + path)
        data = source.read_bytes()
        rows.append(({'path': path, 'type': 'file', 'mode': '0644',
                      'sha256': sha(data)}, data))
    return rows


def inspect_power_profiles(args, suite):
    require(args.target in ('debian', 'arch'), 'Power profiles require Debian or Arch')
    rows = power_profiles_payload()
    package = POWER_PROFILES_PACKAGE
    version = overlay_release.debian_version(args.version) if args.format == 'deb' else args.version
    return {
        'schema_version': 1, 'component': 'power-profiles', 'package': package,
        'version': version, 'target': {'distro': args.target, 'suite': suite, 'arch': args.arch},
        'package_architecture': 'any' if args.target == 'arch' else 'all',
        'source': {'license': 'MIT', 'path': POWER_PROFILES.relative_to(ROOT).as_posix()},
        'depends': ['tuned-ppd'], 'package_ready': False,
        'conflict_check': (conflict_check(args.against, rows, package, adopt_identical=True)
                           if args.against
                           else {'performed': False, 'required_before_install': True}),
        'payload_files': {row['path']: {**row, 'bytes': len(data)} for row, data in rows},
        'files': {},
    }, rows


def stage(args, inspected=None):
    report, rows = inspect(args) if inspected is None else inspected
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
    if report['package'] in FIRMWARE_PACKAGES:
        docs.mkdir(parents=True)
        if report['package'] == TOUCH_PACKAGE:
            copyright = ('Novatek / Xiaomi proprietary firmware from the stock device ROM.\n'
                         'The SunUEFI source license does not cover these binaries.\n'
                         'No permission for public redistribution is documented.\n')
        elif report['package'] == DISPLAY_PACKAGE:
            copyright = ('Panel calibration derived from Qualcomm / Xiaomi stock QDCM data.\n'
                         'The SunUEFI source license does not cover the calibration data.\n')
        elif report['package'] == KEYBOARD_PACKAGE:
            copyright = ('Nanosic / Xiaomi keyboard cover firmware from the stock device ROM.\n'
                         'The SunUEFI source license does not cover this binary.\n')
        elif report['package'] == FINGERPRINT_PACKAGE:
            copyright = (FINGERPRINT_MANIFEST.parent / 'README.md').read_text()
        else:
            copyright = topology_copyright(report['source'])
        (docs / 'copyright').write_text(copyright)
    if report['package'] == POWER_PROFILES_PACKAGE:
        docs.mkdir(parents=True)
        shutil.copyfile(POWER_PROFILES / 'copyright', docs / 'copyright')
    provenance = docs / 'bsp-manifest.json'
    omitted = {'files'}
    if report['component'] in IMAGE_FIRMWARE or report['package'] == TOPOLOGY_PACKAGE:
        omitted.add('conflict_check')
    provenance.write_text(json.dumps({k: v for k, v in report.items() if k not in omitted}, indent=2) + '\n')
    for path in [tree, *tree.rglob('*')]:
        if path.is_dir() and not path.is_symlink():
            path.chmod(0o755)
    return output, tree, report, rows


def preinst(paths, package=NAME, *, architecture='all', adopt=None):
    guard = '#!/bin/sh\nset -eu\nroot=${DPKG_ROOT:-}\n'
    check_alias_owner = ''
    if package in FIRMWARE_PACKAGES:
        firmware = ' '.join(shlex.quote('/' + name) for name in paths if name.startswith(FIRMWARE_ROOT))
        guard += (f'for path in {firmware}; do\n'
                  '  alternate=${path#/usr}\n'
                  '  if ! [ "$root${path%/*}" -ef "$root${alternate%/*}" ]; then\n'
                  '    if [ -e "$root$alternate" ] || [ -L "$root$alternate" ]; then\n'
                  '      printf "%s\\n" "BSP file conflict: $alternate" >&2; exit 1\n'
                  '    fi\n'
                  '  fi\n'
                  '  for alias in "$path.xz" "$path.zst" "$alternate.xz" "$alternate.zst"; do\n'
                  '    if [ -e "$root$alias" ] || [ -L "$root$alias" ]; then\n'
                  '      printf "%s\\n" "BSP file conflict: $alias" >&2; exit 1\n'
                  '    fi\n'
                  '  done\n'
                  'done\n')
        # dpkg-query matches the path recorded in the package database.
        # A merged /lib may refer to the same file with a different owner entry.
        check_alias_owner = (
            '    case "$path" in\n'
            f'      /{FIRMWARE_ROOT}*)\n'
            '        alternate=${path#/usr}\n'
            '        if [ "$root${path%/*}" -ef "$root${alternate%/*}" ]; then\n'
            '          alias_owner=$(dpkg-query --search "$alternate" 2>/dev/null || true)\n'
            '          case "$alias_owner" in\n'
            '            "") ;;\n'
            f'            "{package}: $alternate"|"{package}:{architecture}: $alternate")\n'
            f'              [ -n "$owned" ] || owned="{package}: $path" ;;\n'
            '            *) printf "%s\\n" "BSP file conflict: $alternate ($alias_owner)" >&2; exit 1 ;;\n'
            '          esac\n'
            '        fi ;;\n'
            '    esac\n')
    quoted = ' '.join(shlex.quote('/' + name) for name in paths)
    adoption = ''
    if adopt:
        cases = ''.join(
            f'          {shlex.quote("/" + name)}) checksum={row["sha256"]} ;;\n'
            for name, row in adopt.items())
        adoption = (
            '      "")\n'
            '        checksum=\n'
            '        case "$path" in\n' + cases +
            '        esac\n'
            '        if [ -n "$checksum" ] && [ ! -L "$root$path" ] && [ -f "$root$path" ] &&\n'
            '           [ "$(stat -c %a -- "$root$path")" = 644 ] &&\n'
            '           printf "%s  %s\\n" "$checksum" "$root$path" | sha256sum --quiet -c -; then\n'
            '          continue\n'
            '        fi\n'
            '        printf "%s\\n" "BSP file conflict: $path" >&2; exit 1 ;;\n')
    return (guard +
            '# Refuse other packages and modified files from a generated image.\n'
            f'for path in {quoted}; do\n'
            '  if [ -e "$root$path" ] || [ -L "$root$path" ]; then\n'
            '    owned=$(dpkg-query --search "$path" 2>/dev/null || true)\n' +
            check_alias_owner +
            f'    case "$owned" in "{package}: $path"|"{package}:{architecture}: $path") ;;\n' +
            adoption +
            '      *) printf "%s\\n" "BSP file conflict: $path ($owned)" >&2; exit 1 ;;\n'
            '    esac\n'
            '  fi\n'
            'done\n')


def reload_systemd():
    return ('if [ -d /run/systemd/system ]; then\n'
            '    systemctl --system daemon-reload >/dev/null || true\n'
            'fi\n')


def config_install_script(package_format):
    owner = 'dpkg-query --search' if package_format == 'deb' else 'pacman -Qoq'
    body = power_migration.commands(package_format) + f'''\
legacy=/{OLD_FPRINTD_DROP_IN}
if [ ! -L "$legacy" ] && [ -f "$legacy" ] &&
   ! {owner} -- "$legacy" >/dev/null 2>&1 &&
   [ "$(stat -c '%a:%u:%g' -- "$legacy")" = 644:0:0 ] &&
   printf '%s  %s\\n' {OLD_FPRINTD_SHA256} "$legacy" | sha256sum --status -c -; then
    rm -- "$legacy"
fi
'''
    body += ("python3 -I -S - <<'SUNUEFI_FINGERPRINT_CLEANUP'\n"
             + Path(fingerprint_migration.__file__).read_text()
             + f"\nremove_on_upgrade({package_format!r})\n"
             + 'SUNUEFI_FINGERPRINT_CLEANUP\n')
    if package_format == 'deb':
        return ('#!/bin/sh\nset -eu\n[ "$1" = configure ] || exit 0\n' + body +
                reload_systemd())
    return ('post_install() (\n    set -eu\n' + body +
            ')\n\npost_upgrade() { post_install; }\n')


def debian_package(output, tree, report, configuration):
    package = report['package']
    architecture = report['package_architecture']
    section, detail = {
        TOUCH_PACKAGE: ('non-free-firmware/kernel', ' Built from locally supplied stock ROM files.\n'),
        DISPLAY_PACKAGE: ('non-free-firmware/kernel', ' Installs locally supplied panel calibration data.\n'),
        KEYBOARD_PACKAGE: ('non-free-firmware/kernel',
                           ' Installs the stock Nanosic keyboard and touchpad controller firmware.\n'),
        FINGERPRINT_PACKAGE: ('non-free-firmware/kernel',
                              ' Installs the stock signed Goodix application and models.\n'),
        FINGERPRINT_TOOLS: ('utils', ' Provides piano-fingerprint-ctl for explicit command-line diagnostics.\n'),
        TOPOLOGY_PACKAGE: ('kernel', ' Provides the topology loaded by the Qualcomm ASoC driver.\n'),
        POWER_PROFILES_PACKAGE: ('admin', ' Connects the three desktop power modes to TuneD.\n'),
    }.get(package, ('misc', ' Native runtime, Mesa, firmware and modules are distributed separately.\n'))
    depends = 'Depends: ' + ', '.join(report['depends']) + '\n' if report['depends'] else ''
    installed_size = ''
    if package in (*FIRMWARE_PACKAGES, POWER_PROFILES_PACKAGE, FINGERPRINT_TOOLS):
        # Match dpkg-gencontrol: 1 KiB per directory, round up each file.
        kib = 1 + sum(1 if path.is_dir() else (path.stat().st_size + 1023) // 1024
                      for path in tree.rglob('*'))
        installed_size = f'Installed-Size: {kib}\n'
    control = tree / 'DEBIAN'
    control.mkdir()
    control.chmod(0o755)
    (control / 'control').write_text(
        f"Package: {package}\nVersion: {report['version']}\nArchitecture: {architecture}\n"
        f'Maintainer: Project SunUEFI <slwyts@tuta.io>\nSection: {section}\nPriority: optional\n'
        f'{depends}{installed_size}Description: {package_description(package)}\n{detail}')
    if configuration:
        (control / 'conffiles').write_text(''.join('/' + name + '\n' for name in configuration))
    if package == KEYBOARD_PACKAGE:
        (control / 'triggers').write_text('activate-noawait update-initramfs\n')
    docs = 'usr/share/doc/' + package + '/'
    paths = [*report['payload_files'], docs + 'bsp-manifest.json']
    if package in FIRMWARE_PACKAGES or package == POWER_PROFILES_PACKAGE:
        paths.append(docs + 'copyright')
    adopt = report['payload_files'] if package in (POWER_PROFILES_PACKAGE, KEYBOARD_PACKAGE) else None
    (control / 'preinst').write_text(preinst(paths, package, architecture=architecture, adopt=adopt))
    (control / 'preinst').chmod(0o755)
    subprocess.run(['/bin/sh', '-n', control / 'preinst'], check=True)
    if package == NAME:
        migration = control / 'postinst'
        migration.write_text(config_install_script('deb'))
        migration.chmod(0o755)
        subprocess.run(['/bin/sh', '-n', migration], check=True)
        remove = control / 'postrm'
        remove.write_text('#!/bin/sh\nset -eu\n[ "$1" = remove ] || exit 0\n' +
                          reload_systemd())
        remove.chmod(0o755)
        subprocess.run(['/bin/sh', '-n', remove], check=True)
    artifact = output / f"{package}_{report['version']}_{architecture}.deb"
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
    license_ids = {name: 'LicenseRef-Proprietary' for name in STOCK_FIRMWARE}
    license_ids[TOPOLOGY_PACKAGE] = 'BSD-3-Clause'
    license_ids[POWER_PROFILES_PACKAGE] = 'MIT'
    license_ids[FINGERPRINT_TOOLS] = "BSD-3-Clause BSD-3-Clause-Clear 'GPL-2.0-only WITH Linux-syscall-note'"
    install = ''
    if package == NAME:
        migration = directory / (NAME + '.install')
        migration.write_text(config_install_script('pacman'))
        subprocess.run(['/bin/sh', '-n', migration], check=True)
        install = f'install={migration.name}\n'
    (directory / 'PKGBUILD').write_text(
        f"pkgname={package}\npkgver={report['version']}\npkgrel={overlay_release.pacman_release('1')}\n"
        f'pkgdesc={shlex.quote(package_description(package))}\n'
        "url='https://github.com/slwyts/Project-SunUEFI'\n"
        f'arch=({report["package_architecture"]})\n'
        f'license=({license_ids.get(package, "MIT BSD-2-Clause-Patent")})\noptions=(!strip !debug)\n'
        f'{install}'
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


def makepkg(work, architecture='any'):
    """Build a package of checked files without installing runtime dependencies."""
    env = {**os.environ, 'SOURCE_DATE_EPOCH': '0', 'LC_ALL': 'C',
           'BUILDDIR': str(work), 'SRCDEST': str(work), 'PKGDEST': str(work),
           'LOGDEST': str(work), 'PKGEXT': '.pkg.tar.zst',
           'PACKAGER': 'Project SunUEFI <slwyts@tuta.io>'}
    command = ['makepkg', '--noconfirm', '--nodeps']
    if architecture != 'any':
        config = work / 'makepkg.conf'
        config.write_text(f'. /etc/makepkg.conf\nCARCH={architecture}\n')
        command += ['--config', config]
    account = pacman_builder_account()
    if account is not None:
        for path in (work, *work.iterdir()):
            os.chown(path, account.pw_uid, account.pw_gid, follow_symlinks=False)
        command = ['runuser', '-u', account.pw_name, '--', *command]
    subprocess.run(command, cwd=work, env=env, stdout=sys.stderr, check=True)


def pacman_package(output, tree, report, configuration):
    # Only the packed sources and recipe are exposed to the unprivileged builder.
    with tempfile.TemporaryDirectory(prefix='sunuefi-bsp-', dir='/tmp') as folder:
        work = Path(folder)
        pacman_sources(work, tree, report, configuration)
        architecture = report['package_architecture']
        makepkg(work, architecture)
        version = report['version'] + '-' + overlay_release.pacman_release('1')
        artifact = work / f"{report['package']}-{version}-{architecture}.pkg.tar.zst"
        require(artifact.is_file() and not artifact.is_symlink(),
                'makepkg did not produce the Piano package')
        target = output / artifact.name
        shutil.copyfile(artifact, target)
        return target, version


def build(args):
    fingerprint_tools = args.fingerprint_tools
    if fingerprint_tools is not None:
        require(args.command == 'package', '--fingerprint-tools requires bsp package')
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
    inspected = None
    if fingerprint_tools is not None:
        from package import fingerprint_tools as tools

        inspected = tools.prepare(args)
    output, tree, report, rows = stage(args, inspected)
    if args.command == 'package':
        configuration = [row['path'] for row, _ in rows if row['path'].startswith('etc/')]
        if args.format == 'pacman':
            if report['package'] == KEYBOARD_PACKAGE:
                # Let ALPM adopt the previously unpackaged file and keep local replacements.
                configuration.extend(row['path'] for row, _ in rows)
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
    ap.add_argument('--format', choices=('deb', 'pacman'))
    ap.add_argument('--against', type=Path, help='Readonly target-root conflict/ownership check')
    component = ap.add_mutually_exclusive_group()
    component.add_argument('--touch-firmware', type=Path, metavar='DIRECTORY',
                           help='Package the two local stock touch firmware files')
    component.add_argument('--display-firmware', type=Path, metavar='DIRECTORY',
                           help='Package the two local display calibration files')
    component.add_argument('--keyboard-firmware', type=Path, metavar='DIRECTORY',
                           help='Package the Nanosic keyboard cover firmware')
    component.add_argument('--fingerprint-firmware', type=Path, metavar='DIRECTORY',
                           help='Package the local Goodix fingerprint application and models')
    component.add_argument('--fingerprint-tools', type=Path, metavar='UAPI',
                           help='Build the optional Goodix CLI from exported release kernel headers')
    component.add_argument('--audio-topology', action='store_true',
                           help='Build the AudioReach topology package from the fixed sources')
    component.add_argument('--power-profiles', action='store_true',
                           help='Package the board-specific TuneD profiles and desktop mapping')
    ap.add_argument('--output', type=Path)
    ap.add_argument('--version', help='Override the package version')
    return ap


if __name__ == '__main__':
    try:
        print(json.dumps(build(parser().parse_args()), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
