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
FULL_PREFIXES = ('.github/workflows/_', '.github/actions/', '.github/scripts/',
                 'tools/', 'config/', 'containers/', 'linux/', 'vendor/piano-linux/',
                 'vendor/firmware/', 'patches/', 'upstream/')
UEFI_FILES = {'requirements-build.txt', 'tools/build_android_product_module.py',
              'tools/build_android_module.py', 'tools/build_boot_repack.py',
              'tools/build_boot_request.py', 'tools/build_storage.py',
              'tools/build_resize_f2fs.py', 'tools/build_e2fs_tools.py',
              'tools/android/piano_resize_f2fs.c', 'upstream/Mu-Silicium', 'upstream/simple-init',
              'tools/build_product.sh', 'tools/build_simpleinit.sh', 'tools/build_stage0.sh',
              'tools/firmware_workspace.py', 'tools/apply_firmware_patches.py',
              'tools/prepare_simpleinit.py', 'tools/simpleinit_build_identity.py',
              'tools/prepare_product.py', 'tools/prepare_nv_runtime_guard.py', 'tools/firmware/prepare_ext4.py',
              'tools/package_product.py', 'tools/build_integrity.py',
              'tools/product_payload_digest.py', 'tools/piano_vendor_inputs.py'}
UEFI_PREFIXES = ('android/', 'uefi/', 'vendor/piano/', 'patches/firmware/',
                 'tools/prepare_product_')
DISTROS = ['debian-gnome', 'debian-kde', 'debian-denial', 'nyarch-gnome',
           'arch-denial', 'steamos']
DENIAL_TARGETS = ('debian-denial', 'arch-denial')
DENIAL_FILES = {'config/denial.json', 'config/keys/denial-repo-key.asc',
                'containers/denial.Dockerfile', 'tools/build/denial.py',
                'tools/build/denial_manifest.py', 'tools/build/denial_overlay.py',
                'tools/desktop/build_denial.py', 'tools/desktop/denial_packages.py'}
TARGETS = ('uefi', 'linux', *DISTROS)
NON_PRODUCT_FILES = {
    '.github/workflows/_checks.yml', '.github/workflows/_downloads.yml',
    '.github/scripts/publish_build_downloads.py', '.github/scripts/publish_release_images.py',
    'tools/check_host.py', 'tools/build/check_patch_series.py', 'tools/ci_build_targets.py',
    'patches/linux/AGENTS.md', 'patches/linux/LICENSE', 'patches/linux/README.md',
    'patches/linux/7.3/README.md',
}


def shared_kinds(targets):
    result = []
    if any(target != 'linux' for target in targets):
        result.append('firmware')
    if any(target != 'uefi' for target in targets):
        result.append('kernel')
    return result


def overlay_targets(targets):
    """Build one set of Debian overlays and the matched pacman package sets."""
    selected = set(targets)
    result = ['debian-gnome'] if any(name.startswith('debian-') for name in selected) else []
    # The shared Arch sensor package must have an ABI-matched SSC provider in both repositories.
    if selected & {'nyarch-gnome', 'arch-denial', 'steamos'}:
        result.extend(('nyarch-gnome', 'steamos'))
    result.extend(target for target in DENIAL_TARGETS if target in selected)
    return result


def select(files, manual=None):
    if manual:
        if manual == 'all':
            return DISTROS.copy()
        if manual not in TARGETS:
            raise ValueError('Unknown manual product target')
        return [manual]
    files = list(files)
    full = DISTROS.copy()
    uefi = False
    denial = False
    for name in files:
        if name in NON_PRODUCT_FILES:
            continue
        if name in DENIAL_FILES or name.startswith('patches/denial/'):
            denial = True
            continue
        if name in FULL_FILES:
            return full
        if name in UEFI_FILES or name.startswith(UEFI_PREFIXES):
            uefi = True
        elif name.startswith(FULL_PREFIXES):
            # Full already builds UEFI, kernel, Mesa, sensors, root and installer.
            return full
    # A normal product push always delivers the matching module, ESP and root.
    # Firmware/kernel-only builds remain explicit maintainer dispatch options.
    return full if uefi else list(DENIAL_TARGETS) if denial else []


def changed_files(root, before):
    if before and re.fullmatch(r'[0-9a-f]{40}', before) and before != '0' * 40:
        exists = subprocess.run(['git', '-C', str(root), 'cat-file', '-e', before + '^{commit}'],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if exists.returncode == 0:
            # A rename into an unfiltered directory still removes a build input.
            return subprocess.check_output(
                ['git', '-C', str(root), 'diff', '--no-renames', '--name-only', '-z',
                 before, 'HEAD'], text=True).split('\0')
    # An unavailable push base selects full from the complete tracked input set.
    return subprocess.check_output(['git', '-C', str(root), 'ls-files', '-z'], text=True).split('\0')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', default=os.environ.get('PUSH_BEFORE', ''))
    parser.add_argument('--target', default=os.environ.get('MANUAL_TARGET', ''))
    args = parser.parse_args()
    targets = select(changed_files(ROOT, args.before), args.target)
    distributions = [target for target in targets if target in DISTROS]
    overlays = overlay_targets(distributions)
    def matrix(axis, values):
        return json.dumps({axis: values}, separators=(',', ':'))

    if output := os.environ.get('GITHUB_OUTPUT'):
        with open(output, 'a') as stream:
            values = {
                'has_build': 'true' if targets else 'false',
                'has_rootfs': 'true' if distributions else 'false',
                'rootfs_matrix': matrix('target', distributions),
                'overlay_matrix': matrix('target', overlays),
                'shared_matrix': matrix('kind', shared_kinds(targets)),
                'shared_target': distributions[0] if distributions else '',
            }
            stream.write(''.join(f'{name}={value}\n' for name, value in values.items()))
    print(matrix('target', targets))


if __name__ == '__main__':
    main()
