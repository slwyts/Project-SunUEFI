#!/usr/bin/env python3
"""Assemble the release bundle from the recorded kernel source and staged roots."""
import fcntl
import hashlib
import io
import json
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from prepare_steamos_rootfs import download


def shared_stub(record):
    folder = ROOT / 'build/source-cache/efi-stub'
    folder.mkdir(parents=True, exist_ok=True)
    with (folder / 'download.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return prepare_stub(record, folder)


def prepare_stub(record, folder):
    archive = folder / (record['sha256'] + '.deb')
    download(record['url'], archive, record['sha256'])
    contents = subprocess.check_output(['dpkg-deb', '--fsys-tarfile', archive])
    with tarfile.open(fileobj=io.BytesIO(contents)) as stream:
        member = stream.getmember(record['member'])
        if not member.isfile() or not 0 < member.size <= 1024 * 1024:
            raise ValueError('The shared EFI stub must be a regular file below 1 MiB')
        data = stream.extractfile(member).read()
    if hashlib.sha256(data).hexdigest() != record['stub_sha256']:
        raise ValueError('The shared EFI stub differs from the release configuration')
    stub = folder / (record['stub_sha256'] + '.efi.stub')
    if not stub.is_file() or stub.read_bytes() != data:
        partial = stub.with_suffix('.partial')
        partial.write_bytes(data)
        partial.replace(stub)
    return stub


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
    if not any(value == '--stub' or value.startswith('--stub=') for value in argv):
        args += ['--stub', str(shared_stub(record['efi_stub']))]
    args += ['--initramfs', 'build/distros/release/initramfs/initramfs.cpio.gz',
             '--rootfs', 'build/distros/release/rootfs',
             '--rootfs-manifest', 'build/distros/release/manifest.json',
             '--output', 'artifacts/release', '--root-selector', kernel['root_policy']]
    return subprocess.run(args + argv, check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
