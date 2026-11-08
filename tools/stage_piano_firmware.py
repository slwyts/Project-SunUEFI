#!/usr/bin/env python3
"""Stage the fixed upstream firmware and the selected complete Piano update."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def stage(source, rootfs):
    source, rootfs = Path(source).resolve(), Path(rootfs).resolve()
    if not rootfs.is_relative_to(ROOT / 'build/distros'):
        raise ValueError('Use a project rootfs')
    cfg = json.loads((ROOT / 'config/release.json').read_text())['firmware']
    subprocess.run(['sha256sum', '--check', '--quiet', 'SHA256SUMS'],
                   cwd=source / 'firmware', check=True)
    firmware = rootfs / 'usr/lib/firmware'
    shutil.copytree(source / 'firmware', firmware, dirs_exist_ok=True)
    shutil.copytree(source / 'LICENSES', rootfs / 'usr/share/licenses/piano-firmware',
                    dirs_exist_ok=True)
    record = {'upstream_commit': cfg['commit'], 'overlays': []}
    hashes = {}
    for line in (source / 'firmware/SHA256SUMS').read_text().splitlines():
        checksum, name = line.split(maxsplit=1)
        hashes[name.removeprefix('./')] = checksum
    for name in cfg.get('overlays', []):
        manifest = ROOT / name
        data = json.loads(manifest.read_text())
        directory = PurePosixPath(data.get('directory', '.'))
        if directory.is_absolute() or '..' in directory.parts:
            raise ValueError('Invalid firmware directory')
        destination = firmware
        for part in directory.parts:
            destination /= part
            if destination.is_symlink():
                raise ValueError('Firmware destination traverses a symlink')
        for filename, item in data['files'].items():
            path = manifest.parent / filename
            if Path(filename).name != filename or path.is_symlink():
                raise ValueError('Invalid firmware filename')
            content = path.read_bytes()
            if len(content) != item['bytes'] or hashlib.sha256(content).hexdigest() != item['sha256']:
                raise ValueError('Firmware checksum mismatch: ' + filename)
        # Validate the whole set before replacing any file.
        destination.mkdir(parents=True, exist_ok=True)
        for filename, item in data['files'].items():
            shutil.copy2(manifest.parent / filename, destination / filename)
            hashes[(directory / filename).as_posix()] = item['sha256']
        record['overlays'].append({'manifest': name, 'rom': data['rom'],
                                  'sha256': hashlib.sha256(manifest.read_bytes()).hexdigest()})
        notice = rootfs / 'usr/share/licenses/piano-firmware' / (manifest.parent.name + '.md')
        shutil.copy2(manifest.parent / 'README.md', notice)
    (firmware / 'SHA256SUMS').write_text(
        ''.join(f'{checksum}  ./{name}\n' for name, checksum in sorted(hashes.items())))
    provenance = rootfs / 'usr/share/piano-provenance/firmware.json'
    provenance.parent.mkdir(parents=True, exist_ok=True)
    provenance.write_text(json.dumps(record, indent=2) + '\n')
    return record
