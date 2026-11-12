#!/usr/bin/env python3
"""Install the tracked Piano firmware components into a rootfs.

Every file comes from vendor/firmware: each component directory carries its
files, a manifest of their sizes and SHA256, and a note on where they came
from. Nothing is unpacked from an OTA package here.
"""
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil

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


def stage(source, rootfs):
    source, rootfs = Path(source).resolve(), Path(rootfs).resolve()
    if not rootfs.is_relative_to(ROOT / 'build/distros'):
        raise ValueError('Use a project rootfs')
    firmware = rootfs / 'usr/lib/firmware'
    notices = rootfs / 'usr/share/licenses/piano-firmware'
    record = {'components': []}
    hashes = {}
    for directory, manifest, data in components(source):
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
