#!/usr/bin/env python3
"""Stage the sealed Stable disk bundle and disabled Next files, host only.

Reuses the existing EFI file validators/materializer. No mount, device access,
rootfs edit, kernel build or firmware setting is performed by this tool.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct

from assemble_piano_linux import materialize
from make_kernel_initramfs import inspect_newc
from prepare_boot_files import digest, fdt_info, image_info, initramfs_info, require

ROOT = Path(__file__).resolve().parents[1]
ESP_UUID = '5fd95c71-fea3-47af-b0df-29ca9994a23a'
ROOT_UUID = 'ffc480ed-c219-400b-a8f9-5f6805aa1f34'
ESP_BYTES = 512 * 1024 * 1024
ROOT_BYTES = 68182605824
STABLE = ROOT / 'artifacts/kernels/stable-sunuefi-disk-r4'
NEXT = ROOT / 'artifacts/kernels/next-full/498569101e34'
BOOTSTRAP = ROOT / 'artifacts/linux-disk/bootstrap-disk-r5'
DTB = ROOT / 'private/analysis/piano-linux-managed-dsp-pcie-v4'
BOOT = ROOT / 'artifacts/linux-disk/sunuefi64-r5/sunuefi-linux-stable-boot.img'
PINS = {
    STABLE / 'manifest.json': '465bca3ebd50a70030366c013b420a942c71f23926b272664e45e4335897ad7c',
    NEXT / 'manifest.json': 'ed65d7a7bc5e49b14b0938a305f65f0a4ef94642208d6845cc2ca52c961f70ff',
    BOOTSTRAP / 'manifest.json': '493159c56afa35f3b56a00f1a6ecd17fc02ba26194788aac51ad9cd53fc0cb08',
    DTB / 'manifest.json': '9bf5f7bbd5513c9162bf0e03787891a7acea18b1fea1a0e2f5114c512176fbbf',
}
BOOT_SHA = 'bb0b05532fe55279e0e40a19d45f76c407513a7ee6cde00e39f23715903cb938'


def command_line(folder):
    lines = (folder / 'config').read_text().splitlines()
    require('CONFIG_CMDLINE_FORCE=y' in lines, 'Expected sealed forced command line')
    values = [line[len('CONFIG_CMDLINE='):] for line in lines if line.startswith('CONFIG_CMDLINE=')]
    require(len(values) == 1, 'Expected one CONFIG_CMDLINE')
    return json.loads(values[0])


def check_boot_image(kernel, initrd, dtb):
    """Verify the v2 boot container contains precisely the sealed components."""
    data = BOOT.read_bytes()
    require(hashlib.sha256(data).hexdigest() == BOOT_SHA, 'Stable boot container changed')
    require(data[:8] == b'ANDROID!' and len(data) > 1660, 'Expected Android boot image')
    ksize, _, isize, _, second, _, _, page, version = struct.unpack_from('<9I', data, 8)
    recovery_size = struct.unpack_from('<I', data, 1632)[0]
    header_size, dsize = struct.unpack_from('<II', data, 1644)
    require(version == 2 and page == 4096 and header_size == 1660, 'Expected Android header v2/4KiB')
    require(second == recovery_size == 0, 'Unexpected second stage/recovery DTBO')
    require(not data[64:576].split(b'\0', 1)[0] and not data[608:1632].split(b'\0', 1)[0],
            'Container adds an unexpected external command line')
    align = lambda size: (size + page - 1) // page * page
    offsets = (page, page + align(ksize), page + align(ksize) + align(isize))
    require(len(data) == offsets[2] + align(dsize), 'Boot container bounds differ')
    for offset, size, source in zip(offsets, (ksize, isize, dsize), (kernel, initrd, dtb)):
        require(size == source.stat().st_size and data[offset:offset + size] == source.read_bytes(),
                'Boot component differs: ' + str(source))
    return {'bytes': len(data), 'sha256': BOOT_SHA, 'format': 'android-boot-v2',
            'page_bytes': page, 'all_component_bytes_match': True}


def stage(output):
    for path, expected in PINS.items():
        require(digest(path) == expected, 'Sealed manifest changed: ' + str(path))
    stable, next_kernel, bootstrap, dt = [json.loads((path / 'manifest.json').read_text())
                                         for path in (STABLE, NEXT, BOOTSTRAP, DTB)]
    stable_cmd, next_cmd = command_line(STABLE), command_line(NEXT)
    for folder, record, config_sha in ((STABLE, stable, stable['config_sha256']),
                                       (NEXT, next_kernel, next_kernel['inputs']['build/kernel-topics/piano-next-full-audit/.config'])):
        require(record['source_clean'] is True, 'Kernel manifest does not record a clean source')
        require(image_info(folder / 'Image') == record['image'], 'Kernel image differs from manifest')
        require(digest(folder / 'config') == config_sha, 'Kernel config differs from manifest')
    root_arg = 'piano.root=PARTUUID=' + ROOT_UUID
    require(root_arg in stable_cmd.split() and 'piano.root=ram' not in stable_cmd.split(), 'Stable root policy differs')
    require(stable_cmd == stable['command_line'] and stable['root_policy'] == 'PARTUUID=' + ROOT_UUID,
            'Stable manifest command line differs')
    require('piano.root=ram' in next_cmd.split() and root_arg not in next_cmd.split(),
            'Next candidate policy changed; review disk readiness before staging')
    require(bootstrap['kernel_manifest_sha256'] == PINS[STABLE / 'manifest.json']
            and bootstrap['kernel_cmdline_matches_root'] is True
            and bootstrap['root_partuuid'] == ROOT_UUID
            and bootstrap['kernel_release'] == stable['kernel_release'], 'Disk bootstrap provenance differs')
    initrd = BOOTSTRAP / 'initramfs.cpio.gz'
    info = initramfs_info(initrd)
    require(info['sha256'] == bootstrap['initramfs_sha256']
            and info['bytes'] == bootstrap['initramfs_bytes'], 'Disk bootstrap bytes differ')
    members = {row['name']: row['data'] for row in inspect_newc(gzip.decompress(initrd.read_bytes()))}
    require(members['etc/piano/root-partuuid'].decode().strip() == ROOT_UUID
            and members['etc/piano/kernel-release'].decode().strip() == stable['kernel_release'],
            'Embedded root/kernel selection differs')
    for name, row in bootstrap['files'].items():
        require(len(members[name]) == row['bytes'] and hashlib.sha256(members[name]).hexdigest() == row['sha256'],
                'Initramfs member differs: ' + name)
    dtb = DTB / 'Piano-full-linux-managed-dsp-pcie.dtb'
    dt_info = fdt_info(dtb)
    require(dt_info['sha256'] == dt['output_sha256'] and dt_info['bytes'] == dt['output_bytes'], 'DTB bytes differ')
    boot = check_boot_image(STABLE / 'Image', initrd, dtb)

    files = {}
    def add(source, name):
        files['/EFI/Piano/' + name] = {'source': str(source), 'bytes': source.stat().st_size, 'sha256': digest(source)}
    for family, folder in (('stable', STABLE), ('next', NEXT)):
        for source, name in ((folder / 'Image', 'Image.efi'), (folder / 'config', 'config'),
                             (folder / 'manifest.json', 'kernel-manifest.json')):
            add(source, family + '/' + name)
        add(dtb, family + '/piano.dtb')
    add(initrd, 'stable/initramfs.cpio.gz')
    add(BOOT, 'stable/boot.img')
    add(BOOTSTRAP / 'manifest.json', 'stable/bootstrap-manifest.json')
    add(DTB / 'manifest.json', 'dtb-manifest.json')
    total = sum(row['bytes'] for row in files.values())
    require(total + 1024 * 1024 < ESP_BYTES, 'Staged payload exceeds ESP capacity including metadata reserve')
    report = {
        'schema': 1, 'status': 'HOST_STAGED_STABLE_DISK_INPUTS_NEXT_DISABLED',
        'partitions': {
            'esp': {'partlabel': 'sunuefi_esp', 'partuuid': ESP_UUID, 'filesystem': 'FAT32', 'capacity_bytes': ESP_BYTES},
            'root': {'partlabel': 'sunuefi_linux', 'partuuid': ROOT_UUID, 'filesystem': 'ext4', 'capacity_bytes': ROOT_BYTES}},
        'entries': {
            'stable': {'enabled': False, 'disk_inputs_ready': True, 'boot_image': '/EFI/Piano/stable/boot.img',
                       'efi_image': '/EFI/Piano/stable/Image.efi', 'device_tree': '/EFI/Piano/stable/piano.dtb',
                       'initramfs': '/EFI/Piano/stable/initramfs.cpio.gz', 'load_options': stable_cmd,
                       'kernel_release': stable['kernel_release'], 'kernel_source_commit': stable['source_commit'],
                       'source_bytes': boot['bytes'], 'forced_command_line': True,
                       'boot_image_format': boot['format'], 'efi_handoff_verified': False,
                       'runtime_scope': 'test110 DISK_ROOT_READY and ext4 mount; switch_root fault follows; systemd/GNOME unverified'},
            'next': {'enabled': False, 'disk_inputs_ready': False, 'efi_image': '/EFI/Piano/next/Image.efi',
                     'device_tree': '/EFI/Piano/next/piano.dtb', 'initramfs': None, 'boot_image': None,
                     'load_options': next_cmd, 'kernel_release': next_kernel['kernel_release'],
                     'kernel_source_commit': next_kernel['source_commit'], 'forced_command_line': True,
                     'blockers': ['FORCED_PIANO_ROOT_RAM', 'NO_MATCHING_NEXT_DISK_BOOTSTRAP',
                                  'STABLE_R5_BOOTSTRAP_HAS_STABLE_ONLY_UFS_MODULES', 'NEXT_DISK_RUNTIME_UNVERIFIED'],
                     'existing_ram_bootstrap_bytes': 904729600,
                     'existing_ram_bootstrap_exceeds_esp': True}},
        'files': files, 'combined_boot_image': boot,
        'payload_bytes': total, 'esp_metadata_reserve_bytes': 1024 * 1024,
        'capacity_remaining_before_fat_overhead_bytes': ESP_BYTES - total,
        'device_operation_performed': False, 'rootfs_changed': False,
        'runtime_entries_enabled': False, 'tool_sha256': digest(__file__),
    }
    materialize(files, report, output)
    report['status'] = 'HOST_STAGED_STABLE_DISK_INPUTS_NEXT_DISABLED'
    # Keep the bounded loader/install manifest inside the ESP tree too.
    text = json.dumps(report, indent=2) + '\n'
    (Path(output) / 'manifest.json').write_text(text)
    (Path(output) / 'EFI/Piano/DiskBootManifest.json').write_text(text)
    require(total + 2 * len(text.encode()) < ESP_BYTES, 'Metadata exceeds ESP capacity')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True,
                        help='Fresh directory under artifacts/linux-assembled')
    args = parser.parse_args()
    result = stage(args.output)
    print(json.dumps({key: result[key] for key in ('status', 'payload_bytes', 'partitions', 'entries')}, indent=2))


if __name__ == '__main__':
    main()
