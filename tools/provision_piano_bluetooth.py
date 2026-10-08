#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Provision a per-device Linux BOOTv2 from the real QTI persist raw6 file.

Host files only: no ADB, mount, firmware rebuild, external mkbootimg or daemon.
QTI OS3.0.309 GetLocalAddress reads six human/MSB bytes from
/mnt/vendor/persist/bluetooth/.bt_nv.bin and reverses them (0x985b8..0x98604).
BOOTv2 offsets and SHA1 ID follow AOSP mkbootimg.write_header/update_sha.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import struct

from compose_piano_dtb import read_fdt, write_fdt

PAGE = 4096
BOOT_LIMIT = 1024 * 1024 * 1024
FACTORY_PATH = '/mnt/vendor/persist/bluetooth/.bt_nv.bin'


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def factory_address(data):
    require(len(data) == 6 and any(data) and data != bytes([255]) * 6,
            'Factory persist must contain exactly six nonzero/non-FF raw bytes')
    require(not data[0] & 3, 'Factory address must be public/unicast, without a random fallback')
    little = data[::-1]
    require(little not in (bytes.fromhex('00bd0000bd00'), bytes.fromhex('112233445566')),
            'Factory file contains a QTI default address')
    return little


def boot_id(kernel, ramdisk, dtb):
    # AOSP BOOTv2: kernel, ramdisk, empty second, empty recovery_dtbo, DTB.
    digest = hashlib.sha1()
    for part in (kernel, ramdisk, b'', b'', dtb):
        digest.update(part)
        digest.update(struct.pack('<I', len(part)))
    return digest.digest() + bytes(12)


def split_boot(data):
    require(1660 <= len(data) <= BOOT_LIMIT and data[:8] == b'ANDROID!', 'Expected bounded Linux BOOTv2')
    kernel_size, _, ramdisk_size, _, second, _, _, page, version = struct.unpack_from('<9I', data, 8)
    recovery, recovery_offset, header, dtb_size = struct.unpack_from('<IQII', data, 1632)
    require((page, version, header, second, recovery, recovery_offset) == (PAGE, 2, 1660, 0, 0, 0),
            'Expected BOOTv2/4096 without second stage, recovery DTBO or AVB footer')
    require(kernel_size >= 64 and ramdisk_size > 0 and 40 <= dtb_size < 2 * 1024 * 1024 - 65536,
            'Linux BOOT component dimensions differ')
    offset, parts = PAGE, []
    for size in (kernel_size, ramdisk_size, dtb_size):
        end = offset + size
        padded = (end + PAGE - 1) // PAGE * PAGE
        require(padded <= len(data) and not any(data[end:padded]), 'BOOT padding is nonzero or outside the file')
        parts.append(data[offset:end])
        offset = padded
    require(offset == len(data), 'Unexpected BOOT trailer; no bytes will be discarded')
    kernel, ramdisk, dtb = parts
    require(kernel[56:60] == b'ARMd' and not struct.unpack_from('<Q', kernel, 24)[0] & 1,
            'Expected little-endian raw AArch64 Linux Image')
    require(data[576:608] == boot_id(kernel, ramdisk, dtb), 'BOOTv2 AOSP SHA1 ID differs')
    return kernel, ramdisk, dtb


def provision_boot(data, raw6):
    address = factory_address(raw6)
    kernel, ramdisk, dtb = split_boot(data)
    before = read_fdt(dtb)
    root = before['tree']['/']
    require(b'piano' in root.get('model', b'').lower() or b'xiaomi,piano' in root.get('compatible', b'').split(b'\0'),
            'Expected a Piano Linux DTB')
    nodes = [name for name, props in before['tree'].items()
             if any(value in props.get('compatible', b'').split(b'\0')
                    for value in (b'qcom,wcn7850-bt', b'qcom,wcn7861-bt'))]
    require(len(nodes) == 1, 'Expected exactly one Piano QCA Bluetooth controller')
    node = nodes[0]
    require('qcom,local-bd-address-broken' not in before['tree'][node],
            'Cannot provision standard bytes over a broken-byte-order firmware quirk')
    after = copy.deepcopy(before)
    after['tree'][node]['local-bd-address'] = address
    derived_dtb = write_fdt(after)
    require(read_fdt(derived_dtb) == after, 'DTB roundtrip changed properties/reservations')
    header = bytearray(data[:PAGE])
    header[576:608] = boot_id(kernel, ramdisk, derived_dtb)
    struct.pack_into('<I', header, 1648, len(derived_dtb))
    result = bytearray(header)
    for part in (kernel, ramdisk, derived_dtb):
        result.extend(part)
        result.extend(bytes(-len(result) % PAGE))
    result = bytes(result)
    check_kernel, check_ramdisk, check_dtb = split_boot(result)
    require(check_kernel == kernel and check_ramdisk == ramdisk and check_dtb == derived_dtb,
            'Provisioning changed kernel or initramfs')
    record = {'schema_version': 1, 'status': 'HOST_DEVICE_DTB_DERIVED_NOT_DEVICE_TESTED',
              'factory_source': FACTORY_PATH, 'factory_format': 'QTI persist raw6, human/MSB order',
              'factory_source_sha256': sha(raw6), 'factory_bytes': 6,
              'property': 'local-bd-address', 'property_byte_order': 'LSB first', 'node': node,
              'input_boot_sha256': sha(data), 'output_boot_sha256': sha(result),
              'input_dtb_sha256': sha(dtb), 'output_dtb_sha256': sha(derived_dtb),
              'kernel_sha256': sha(kernel), 'initramfs_sha256': sha(ramdisk),
              'kernel_initramfs_unchanged': True, 'header_and_command_line_preserved_except_dtb_size_and_id': True,
              'only_declared_dt_property_changed': True, 'device_operation_performed': False,
              'address_values_in_report': False, 'daemon_added': False,
              'provider_sources': {name: sha(Path(__file__).with_name(name).read_bytes()) for name in
                                   ('provision_piano_bluetooth.py', 'compose_piano_dtb.py')}}
    return result, derived_dtb, record


def save(path, data):
    with path.open('xb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    path.chmod(0o600)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--boot-image', type=Path, required=True)
    parser.add_argument('--factory-nv', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New private device output directory')
    args = parser.parse_args()
    try:
        require(not args.output.exists() and not args.output.is_symlink(), 'Use a new device output directory')
        for path, limit in ((args.boot_image, BOOT_LIMIT), (args.factory_nv, 6)):
            require(path.is_file() and not path.is_symlink() and 0 < path.stat().st_size <= limit,
                    'Expected bounded regular source file')
        image, dtb, record = provision_boot(args.boot_image.read_bytes(), args.factory_nv.read_bytes())
        args.output.mkdir(parents=True, mode=0o700)
        save(args.output / 'boot.img', image)
        save(args.output / 'board.dtb', dtb)
        save(args.output / 'device-provision.json', (json.dumps(record, indent=2) + '\n').encode())
        require(sha((args.output / 'boot.img').read_bytes()) == record['output_boot_sha256'], 'Output BOOT readback differs')
        print(json.dumps(record, indent=2))
    except (ValueError, OSError, KeyError, struct.error) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
