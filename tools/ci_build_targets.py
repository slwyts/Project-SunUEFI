#!/usr/bin/env python3
"""Select the real product builds affected by a push; manual targets stay explicit."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
COMMON_FILES = {'.github/workflows/build-products.yml', '.gitmodules',
                'build.sh', 'Makefile', 'sources.lock.json'}
COMMON_PREFIXES = ('tools/', 'config/', 'containers/')
UEFI_PREFIXES = ('uefi/', 'vendor/piano/', 'patches/firmware/')
LINUX_PREFIXES = ('linux/', 'vendor/piano-linux/', 'patches/linux/')


def select(files, manual=None):
    if manual:
        if manual not in ('uefi', 'linux', 'debian-gnome'):
            raise ValueError('Unknown manual product target')
        return [manual]
    targets = set()
    for name in files:
        if name in COMMON_FILES or name.startswith(COMMON_PREFIXES):
            targets.update(('uefi', 'linux'))
        elif (name.startswith(UEFI_PREFIXES) or name == 'requirements-build.txt' or
              name in ('upstream/Mu-Silicium', 'upstream/simple-init')):
            targets.add('uefi')
        elif name.startswith(LINUX_PREFIXES) or name.startswith('upstream/'):
            targets.add('linux')
    return [name for name in ('uefi', 'linux') if name in targets]


def changed_files(root, before):
    if before and re.fullmatch(r'[0-9a-f]{40}', before) and before != '0' * 40:
        exists = subprocess.run(['git', '-C', str(root), 'cat-file', '-e', before + '^{commit}'],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if exists.returncode == 0:
            return subprocess.check_output(['git', '-C', str(root), 'diff', '--name-only', '-z', before, 'HEAD'], text=True).split('\0')
    # A new branch or an unavailable force-push base requires both builds.
    return subprocess.check_output(['git', '-C', str(root), 'ls-files', '-z'], text=True).split('\0')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', default=os.environ.get('PUSH_BEFORE', ''))
    parser.add_argument('--target', default=os.environ.get('MANUAL_TARGET', ''))
    args = parser.parse_args()
    targets = select(changed_files(ROOT, args.before), args.target)
    matrix = json.dumps({'target': targets}, separators=(',', ':'))
    if output := os.environ.get('GITHUB_OUTPUT'):
        with open(output, 'a') as stream:
            stream.write('has_build=' + ('true' if targets else 'false') + '\nmatrix=' + matrix + '\n')
    print(matrix)


if __name__ == '__main__':
    main()
