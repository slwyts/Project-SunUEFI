#!/usr/bin/env python3
"""Export pinned stable/next boot files and provenance; never writes a device.

The raw RAM smoke record is preserved with its exact scope. Exporting does not
establish standard EFI handoff, full DDR readiness, drivers or a distribution.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct

from build_kernel import image_info

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def export(profile, dtb, output):
    if profile not in ('stable', 'next'):
        raise ValueError('Expected stable or next')
    dtb, output = Path(dtb), Path(output)
    if output.exists() or output.is_symlink():
        raise ValueError('Output exists; preserve previous bundle')
    source = ROOT/'artifacts/kernels'/profile/'ram'
    pin_path = ROOT/'kernel-profiles.json'
    kernel_path, initrd_path = source/'manifest.json', source/'initramfs-manifest.json'
    metadata = {path: path.read_bytes() for path in (pin_path, kernel_path, initrd_path)}
    pin = json.loads(metadata[pin_path])['profiles'][profile]
    kernel = json.loads(metadata[kernel_path])
    initrd = json.loads(metadata[initrd_path])
    test = 71 if profile == 'stable' else 73
    evidence_path = source/f'ram-validation-test-{test}.json'
    metadata[evidence_path] = evidence_path.read_bytes()
    evidence = json.loads(metadata[evidence_path])
    if kernel['source_commit'] != pin['commit'] or kernel['source_dirty'] is not False:
        raise ValueError('Kernel source is not the pinned clean build')
    if kernel['status'] != 'HOST_BUILT_NOT_HARDWARE_VERIFIED' or not image_info(source/'Image')['efi_stub']:
        raise ValueError('Missing actual built EFI stub Image')
    if evidence.get('status') != 'VERIFIED_RAW_ARM64_RAM_SMOKE_ONLY' or evidence['source_commit'] != pin['commit']:
        raise ValueError('Matching raw smoke record missing')
    expected = {'Image': kernel['image']['sha256'],
                'initramfs.cpio.gz': initrd['initramfs']['sha256'],
                'config': kernel['config_sha256']}
    if initrd['source_commit'] != pin['commit'] or initrd['kernel_image']['sha256'] != expected['Image']:
        raise ValueError('Kernel/initramfs provenance differs')
    if (expected['Image'] != evidence['kernel_sha256'] or expected['initramfs.cpio.gz'] != evidence['initrd_sha256']
            or expected['config'] != evidence['config_sha256']):
        raise ValueError('Built files differ from the raw smoke evidence')
    for name, checksum in expected.items():
        if sha(source/name) != checksum:
            raise ValueError('Source bytes changed: '+name)
    data = dtb.read_bytes()
    if not 40 <= len(data) <= 2*1024*1024 or data[:4] != b'\xd0\x0d\xfe\xed' or struct.unpack_from('>I', data, 4)[0] != len(data):
        raise ValueError('Expected complete explicitly selected DTB')
    if hashlib.sha256(data).hexdigest() != evidence['dtb_sha256']:
        raise ValueError('DTB differs from the validated raw bundle')
    total = (source/'Image').stat().st_size+(source/'initramfs.cpio.gz').stat().st_size+len(data)
    if total > 64*1024*1024:
        raise ValueError('Current CPU boot file budget is 64MiB')
    output.mkdir(parents=True, exist_ok=False)
    records = {}
    try:
        for name in expected:
            shutil.copyfile(source/name, output/name)
            if sha(output/name) != expected[name] or sha(source/name) != expected[name]:
                raise ValueError('Copy/source changed: '+name)
            records[name] = {'bytes': (output/name).stat().st_size, 'sha256': expected[name]}
        (output/'piano.dtb').write_bytes(data)
        if (output/'piano.dtb').read_bytes() != data or dtb.read_bytes() != data:
            raise ValueError('DTB copy/source changed')
        records['piano.dtb'] = {'bytes': len(data), 'sha256': evidence['dtb_sha256']}
        (output/'raw-smoke-evidence.json').write_bytes(metadata[evidence_path])
        if any(path.read_bytes() != original for path, original in metadata.items()):
            raise ValueError('Source provenance changed during export')
        entry = {'name': 'Piano Stable' if profile == 'stable' else 'Piano Next',
                 'kind': 'linux-efi-stub', 'kernel': '/EFI/Piano/'+profile+'/Image',
                 'dtb': '/EFI/Piano/'+profile+'/piano.dtb',
                 'initrd': '/EFI/Piano/'+profile+'/initramfs.cpio.gz',
                 'command_line': 'rdinit=/init ro nokaslr efi=novamap',
                 'files': records, 'source_commit': pin['commit'],
                 'default_enabled': False, 'requires_complete_ddr_and_os_exit': True}
        manifest = {'schema': 1, 'status': 'FILES_EXPORTED_NOT_EFI_BOOT_VERIFIED',
                    'profile': profile, 'entry': entry, 'payload_bytes': total,
                    'raw_evidence_sha256': sha(output/'raw-smoke-evidence.json'),
                    'kernel_manifest_sha256': hashlib.sha256(metadata[kernel_path]).hexdigest(),
                    'initramfs_manifest_sha256': hashlib.sha256(metadata[initrd_path]).hexdigest(),
                    'device_writes': False, 'efi_handoff_verified': False,
                    'distribution_verified': False, 'hardware_drivers_verified': False,
                    'storage_requirement': 'external or approved dedicated OS volume; 14MiB state volume is too small'}
        (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    except Exception:
        (output/'FAILED.txt').write_text('Incomplete export; do not load.\n')
        raise
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=('stable', 'next'), required=True)
    parser.add_argument('--dtb', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = export(args.profile, args.dtb, args.output)
    except (ValueError, OSError, KeyError) as error:
        parser.exit(1, str(error)+'\n')
    print(json.dumps({key: result[key] for key in ('status', 'profile', 'payload_bytes', 'efi_handoff_verified')}, indent=2))


if __name__ == '__main__':
    main()
