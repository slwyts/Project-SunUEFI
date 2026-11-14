#!/usr/bin/env python3
"""Adapt the current Nyarch Material UwU source for system-installed GNOME 51."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
UUID = 'material-you-colors@francescocaracciolo.github.io'
PATCH = ROOT / 'patches/material-you-colors/30/0001-system-install-gnome51.patch'


def prepare(source, sysroot, output):
    upstream = source / 'Gnome/etc/skel/.local/share/gnome-shell/extensions' / UUID
    resource = sysroot / 'usr/share/gnome-shell/gnome-shell-theme.gresource'
    if not resource.is_file():
        raise ValueError('The target root must contain the installed GNOME Shell theme')
    version = subprocess.check_output(['chroot', sysroot, 'gnome-shell', '--version'], text=True)
    if not version.strip().startswith('GNOME Shell 51.'):
        raise ValueError('This adaptation targets the installed GNOME 51 theme')
    if output.exists():
        raise ValueError('Use a fresh output directory')
    shutil.copytree(upstream, output, symlinks=True,
                    ignore=shutil.ignore_patterns('node_modules', '__pycache__'))
    subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', PATCH],
                   cwd=output, check=True)
    metadata = output / 'metadata.json'
    data = json.loads(metadata.read_text())
    if '51' not in data['shell-version']:
        data['shell-version'].append('51')
    metadata.write_text(json.dumps(data, indent=2) + '\n')
    folder = output / 'shell/51'
    folder.mkdir(parents=True)
    for mode in ('dark', 'light'):
        content = subprocess.check_output([
            'gresource', 'extract', resource,
            '/org/gnome/shell/theme/gnome-shell-' + mode + '.css'])
        (folder / ('compiled_shell_' + mode + '.scss')).write_bytes(content)
        (folder / ('compiled_shell_' + mode + '.scss.map')).write_text(json.dumps(
            {'version': 3, 'sources': [], 'names': [], 'mappings': ''}) + '\n')
    record = {
        'source_commit': subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'],
                                                text=True).strip(),
        'patch': str(PATCH.relative_to(ROOT)),
        'patch_sha256': hashlib.sha256(PATCH.read_bytes()).hexdigest(),
        'shell_version': version.strip(),
        'shell_resource_sha256': hashlib.sha256(resource.read_bytes()).hexdigest(),
        'backend': 'upstream JavaScript Material color engine',
        'native_node_addons': False,
    }
    (output / 'sunuefi-source.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'sysroot', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.source.resolve(), args.sysroot.resolve(), args.output.resolve()), indent=2))
