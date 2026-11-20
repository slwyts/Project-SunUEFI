#!/usr/bin/env python3
"""Install and verify the tracked Piano firmware components in a rootfs.

Every file comes from vendor/firmware: each component directory carries its
files, a manifest of their sizes and SHA256, and a source note. Native
packages own the display calibration, keyboard cover and fingerprint firmware.
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


def verify_firmware_package(rootfs, package_format, metadata):
    name = metadata['name']
    if package_format == 'debian':
        query = ['dpkg-query', '-W', '-f=${db:Status-Status} ${Version}', name]
        expected = 'installed ' + metadata['version']
    else:
        query = ['pacman', '-Q', name]
        expected = name + ' ' + metadata['version']
    if subprocess.check_output(['chroot', rootfs, *query], text=True).strip() != expected:
        raise ValueError(f'Installed {name} package version differs')
    rows = metadata['firmware']
    package_bsp.conflict_check(
        rootfs, [(row, None) for row in rows.values()], name,
        packaged_compressed_aliases=package_format == 'pacman' and name == package_bsp.KEYBOARD_PACKAGE)
    # Reject missing files as well as conflicts with another package.
    for relative, row in {**rows, **metadata.get('compressed_firmware', {})}.items():
        relative = PurePosixPath(relative)
        path = guest_resolve(rootfs, relative.parent.as_posix()) / relative.name
        if (path.is_symlink() or not path.is_file() or
                stat.S_IMODE(path.stat().st_mode) != 0o644 or path.stat().st_size != row['bytes'] or
                overlay_packages.sha(path) != row['sha256']):
            raise ValueError(f'Installed {name} file differs: {relative}')


def verify_firmware_packages(rootfs, package_format, packages):
    for metadata in packages.values():
        verify_firmware_package(rootfs, package_format, metadata)


def install_firmware_package(source, rootfs, package_format, component, *, shared, run):
    name, manifest = package_bsp.IMAGE_FIRMWARE[component]
    _, rows = package_bsp.local_firmware_payload(
        source, manifest, component.replace('-', ' '))
    if shared and name in package_bsp.PUBLISHED_STOCK_FIRMWARE:
        _, metadata = overlay_packages.shared_bsp(package_format, component=component)
        verify_firmware_package(rootfs, package_format, metadata)
        return metadata
    migration = package_bsp.conflict_check(
        rootfs, rows, name, adopt_identical=True,
        packaged_compressed_aliases=package_format == 'pacman' and name == package_bsp.KEYBOARD_PACKAGE)
    tmp = guest_resolve(rootfs, '/var/tmp')
    tmp.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f'sunuefi-{component}-', dir=tmp) as temporary:
        work = Path(temporary)
        work.chmod(0o755)
        if shared and component == 'display-firmware':
            archive, metadata = overlay_packages.shared_bsp(package_format, component=component)
        else:
            archive, metadata = overlay_packages.build_bsp(
                work / 'package', package_format, run, component=component,
                firmware_source=source)
        staged = work / archive.name
        shutil.copyfile(archive, staged)
        if overlay_packages.sha(staged) != metadata['sha256']:
            raise ValueError(f'{name} archive changed during installation')
        guest_archive = '/' + staged.relative_to(rootfs).as_posix()
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
                         '--reinstall', '--allow-downgrades', guest_archive])
            else:
                from build_nyarch_apps import guest_environment
                with guest_environment(rootfs) as guest:
                    guest(['pacman', '-U', '--noconfirm', guest_archive])
            verify_firmware_package(rootfs, package_format, metadata)
        except Exception:
            for path, saved in moved:
                if not path.exists() and not path.is_symlink():
                    saved.rename(path)
            raise
    return metadata


def packaged_components(selected):
    pinned = {}
    paths = set()
    for component, (_, manifest) in package_bsp.IMAGE_FIRMWARE.items():
        data = package_bsp.read_json(manifest)
        pinned[data['component']] = (component, data)
        directory = PurePosixPath(data.get('directory', '.'))
        paths.update(directory / filename for filename in data['files'])
    found = {}
    for directory, _, data in selected:
        name = data['component']
        if name in pinned:
            component, expected = pinned[name]
            if name in found or data != expected:
                raise ValueError(f'{component.capitalize()} manifest differs from the fixed source')
            found[name] = component
        elif any(directory / filename in paths for filename in data['files']):
            raise ValueError(f'{name} overlaps firmware owned by a native package')
    return found


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
    packaged = packaged_components(selected)
    record = {'components': [], 'packages': {}}
    hashes = {}
    for directory, manifest, data in selected:
        component = packaged.get(data['component'])
        if component:
            metadata = install_firmware_package(
                manifest.parent, rootfs, package_format, component,
                shared=shared, run=run)
            record['packages'][metadata['name']] = metadata
            hashes.update({(directory / filename).as_posix(): row['sha256']
                           for filename, row in data['files'].items()})
            record['components'].append({
                'component': data['component'], 'rom': data['rom'], 'directory': directory.as_posix(),
                'sha256': hashlib.sha256(manifest.read_bytes()).hexdigest(),
                'package': metadata['name']})
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
