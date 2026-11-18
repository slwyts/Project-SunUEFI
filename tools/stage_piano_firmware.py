#!/usr/bin/env python3
"""Install and verify the tracked Piano firmware components in a rootfs.

Every file comes from vendor/firmware: each component directory carries its
files, a manifest of their sizes and SHA256, and a source note. The native
display calibration package owns its files; the other components are staged.
"""
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import tempfile

from build import overlay_packages
from build_piano_ram_bootstrap import guest_resolve
import package_bsp

ROOT = Path(__file__).resolve().parents[1]
COMPONENTS = ROOT / 'vendor/firmware'
DISPLAY = 'piano-display-calibration'


def components(source=None):
    """Return (directory, manifest path, manifest) for each component."""
    base = Path(source or COMPONENTS).resolve()
    found = []
    for manifest in sorted(base.glob('*/manifest.json')):
        data = json.loads(manifest.read_text())
        directory = PurePosixPath(data.get('directory', '.'))
        if directory.is_absolute() or '..' in directory.parts:
            raise ValueError('Invalid firmware directory: ' + str(manifest))
        found.append((directory, manifest, data))
    if not found:
        raise ValueError('No firmware component manifests under ' + str(base))
    return found


def verify_display(rootfs, package_format, metadata):
    if metadata is None:
        return
    name = package_bsp.DISPLAY_PACKAGE
    if package_format == 'debian':
        query = ['dpkg-query', '-W', '-f=${db:Status-Status} ${Version}', name]
        expected = 'installed ' + metadata['version']
    else:
        query = ['pacman', '-Q', name]
        expected = name + ' ' + metadata['version']
    if subprocess.check_output(['chroot', rootfs, *query], text=True).strip() != expected:
        raise ValueError('Installed display calibration package version differs')
    rows = metadata['firmware']
    package_bsp.conflict_check(rootfs, [(row, None) for row in rows.values()], name)
    # Reject missing files as well as conflicts with another package.
    for relative, row in rows.items():
        relative = PurePosixPath(relative)
        path = guest_resolve(rootfs, relative.parent.as_posix()) / relative.name
        if (path.is_symlink() or not path.is_file() or
                stat.S_IMODE(path.stat().st_mode) != 0o644 or path.stat().st_size != row['bytes'] or
                overlay_packages.sha(path) != row['sha256']):
            raise ValueError('Installed display calibration differs: ' + relative.as_posix())


def install_display(source, rootfs, package_format, *, shared, run):
    name = package_bsp.DISPLAY_PACKAGE
    _, rows = package_bsp.local_firmware_payload(
        source, package_bsp.DISPLAY_MANIFEST, 'display firmware')
    tmp = guest_resolve(rootfs, '/var/tmp')
    tmp.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sunuefi-display-', dir=tmp) as temporary:
        work = Path(temporary)
        work.chmod(0o755)
        if shared:
            archive, metadata = overlay_packages.bsp_package(
                ROOT / 'artifacts/packages/bsp/display-firmware' / package_format,
                package_format, component='display-firmware')
        else:
            archive, metadata = overlay_packages.build_bsp(
                work / 'package', package_format, run, component='display-firmware',
                firmware_source=source)
        staged = work / archive.name
        shutil.copyfile(archive, staged)
        if overlay_packages.sha(staged) != metadata['sha256']:
            raise ValueError('Display calibration package changed during installation')
        guest_archive = '/' + staged.relative_to(rootfs).as_posix()
        migration = package_bsp.conflict_check(rootfs, rows, name, adopt_display=True)
        moved = []
        try:
            for index, relative in enumerate(migration['adopted']):
                path, saved = rootfs / relative, work / f'previous-{index}'
                path.rename(saved)
                moved.append((path, saved))
            if package_format == 'debian':
                from build_release_rootfs import build_devices
                with build_devices(rootfs):
                    run(['chroot', rootfs, 'env', 'DEBIAN_FRONTEND=noninteractive',
                         'apt-get', 'install', '-y', '--no-install-recommends',
                         '--reinstall', guest_archive])
            else:
                from build_nyarch_apps import guest_environment
                with guest_environment(rootfs) as guest:
                    guest(['pacman', '-U', '--noconfirm', guest_archive])
            verify_display(rootfs, package_format, metadata)
        except Exception:
            for path, saved in moved:
                if not path.exists() and not path.is_symlink():
                    saved.rename(path)
            raise
    return metadata


def stage(source, rootfs, package_format, *, shared=False, run=None):
    source, rootfs = Path(source).resolve(), Path(rootfs).resolve()
    builds = ROOT / 'build/distros'
    if rootfs == builds or not rootfs.is_relative_to(builds) or not rootfs.is_dir():
        raise ValueError('Use a project rootfs')
    if package_format not in overlay_packages.BSP_TARGETS:
        raise ValueError('Choose the native package format for the product rootfs')
    if run is None:
        def run(args):
            return subprocess.run(list(map(str, args)), check=True)
    firmware = rootfs / 'usr/lib/firmware'
    notices = rootfs / 'usr/share/licenses/piano-firmware'
    selected = components(source)
    display = [manifest for _, manifest, data in selected if data['component'] == DISPLAY]
    pinned_display = package_bsp.read_json(package_bsp.DISPLAY_MANIFEST)
    if len(display) > 1 or (display and package_bsp.read_json(display[0]) != pinned_display):
        raise ValueError('Display calibration manifest differs from the fixed source')
    display_paths = {PurePosixPath(pinned_display['directory']) / filename
                     for filename in pinned_display['files']}
    if any(directory / name in display_paths for directory, _, data in selected
           if data['component'] != DISPLAY for name in data['files']):
        raise ValueError('Only the display calibration package may install its firmware files')
    display_package = (install_display(display[0].parent, rootfs, package_format, shared=shared, run=run)
                       if display else None)
    record = {'components': [], 'display_package': display_package}
    hashes = {}
    for directory, manifest, data in selected:
        if data['component'] == DISPLAY:
            hashes.update({(directory / filename).as_posix(): row['sha256']
                           for filename, row in data['files'].items()})
            record['components'].append({
                'component': DISPLAY, 'rom': data['rom'], 'directory': directory.as_posix(),
                'sha256': hashlib.sha256(manifest.read_bytes()).hexdigest(),
                'package': display_package['name']})
            continue
        destination = firmware
        for part in directory.parts:
            destination /= part
            if destination.is_symlink():
                raise ValueError('Firmware destination traverses a symlink')
        # Check the whole set before replacing any file.
        contents = {}
        for filename, item in data['files'].items():
            path = manifest.parent / filename
            if Path(filename).name != filename or path.is_symlink():
                raise ValueError('Invalid firmware filename: ' + filename)
            content = path.read_bytes()
            if len(content) != item['bytes'] or hashlib.sha256(content).hexdigest() != item['sha256']:
                raise ValueError('Firmware checksum mismatch: ' + filename)
            contents[filename] = content
        destination.mkdir(parents=True, exist_ok=True)
        for filename, content in contents.items():
            (destination / filename).write_bytes(content)
            hashes[(directory / filename).as_posix()] = data['files'][filename]['sha256']
        record['components'].append({
            'component': data['component'], 'rom': data['rom'],
            'source': data.get('source', ''),
            'directory': directory.as_posix(),
            'sha256': hashlib.sha256(manifest.read_bytes()).hexdigest()})
        readme = manifest.parent / 'README.md'
        if readme.is_file():
            notices.mkdir(parents=True, exist_ok=True)
            shutil.copy2(readme, notices / (manifest.parent.name + '.md'))
    (firmware / 'SHA256SUMS').write_text(
        ''.join(f'{checksum}  ./{name}\n' for name, checksum in sorted(hashes.items())))
    provenance = rootfs / 'usr/share/piano-provenance/firmware.json'
    provenance.parent.mkdir(parents=True, exist_ok=True)
    provenance.write_text(json.dumps(record, indent=2) + '\n')
    return record
