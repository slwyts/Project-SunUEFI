#!/usr/bin/env python3
"""Assemble the release bundle from the recorded kernel source and staged roots."""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main(argv):
    record = json.loads((ROOT / 'config/release.json').read_text())
    kernel = record['kernel']
    source = json.loads((ROOT / 'build/release/source-manifest.json').read_text())
    args = ['python3', 'tools/package_release.py',
            '--uefi', 'artifacts/product/PianoUEFI-product.img',
            '--kernel', 'artifacts/kernels/release',
            '--kernel-source', source['worktree'],
            '--dtb', 'vendor/piano-linux/board.dtb',
            '--cpu-model-overlay', kernel['cpu_model_overlay'],
            '--panel-vendor', kernel['preferred_display_mode']['panel_vendor']]
    for overlay in kernel['additional_dtb_overlays']:
        args += ['--dtb-overlay', overlay]
    args += ['--initramfs', 'build/distros/release/initramfs/initramfs.cpio.gz',
             '--rootfs', 'build/distros/release/rootfs',
             '--rootfs-manifest', 'build/distros/release/manifest.json',
             '--output', 'artifacts/release', '--root-selector', kernel['root_policy']]
    return subprocess.run(args + argv, check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
