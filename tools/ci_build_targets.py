#!/usr/bin/env python3
"""Select the real product builds affected by a push; manual targets stay explicit."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FULL_FILES = {'.github/workflows/build-products.yml', '.gitmodules',
              'build.sh', 'Makefile', 'sources.lock.json',
              'android/native/piano-boot-request.c',
              'uefi/handoff/bootselect/BootRequest.c',
              'uefi/handoff/bootselect/BootRequest.h',
              'docs/user/install-from-artifact.md'}
FULL_PREFIXES = ('tools/', 'config/', 'containers/', 'linux/',
                 'vendor/piano-linux/', 'patches/', 'upstream/')
UEFI_FILES = {'requirements-build.txt', 'upstream/Mu-Silicium', 'upstream/simple-init',
              'tools/build_product.sh', 'tools/build_simpleinit.sh', 'tools/build_stage0.sh',
              'tools/firmware_workspace.py', 'tools/apply_firmware_patches.py',
              'tools/prepare_simpleinit.py', 'tools/simpleinit_build_identity.py',
              'tools/prepare_product.py', 'tools/prepare_nv_runtime_guard.py',
              'tools/package_product.py', 'tools/build_integrity.py',
              'tools/product_payload_digest.py', 'tools/piano_vendor_inputs.py'}
UEFI_PREFIXES = ('uefi/', 'vendor/piano/', 'patches/firmware/',
                 'tools/prepare_product_')


def select(files, manual=None):
    if manual:
        if manual not in ('uefi', 'linux', 'debian-gnome'):
            raise ValueError('Unknown manual product target')
        return [manual]
    uefi = False
    for name in files:
        if name in FULL_FILES:
            return ['debian-gnome']
        if name in UEFI_FILES or name.startswith(UEFI_PREFIXES):
            uefi = True
        elif name.startswith(FULL_PREFIXES):
            # Full already builds UEFI, kernel, Mesa, sensors, root and installer.
            return ['debian-gnome']
    return ['uefi'] if uefi else []


def changed_files(root, before):
    if before and re.fullmatch(r'[0-9a-f]{40}', before) and before != '0' * 40:
        exists = subprocess.run(['git', '-C', str(root), 'cat-file', '-e', before + '^{commit}'],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if exists.returncode == 0:
            return subprocess.check_output(['git', '-C', str(root), 'diff', '--name-only', '-z', before, 'HEAD'], text=True).split('\0')
    # An unavailable push base selects full from the complete tracked input set.
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
