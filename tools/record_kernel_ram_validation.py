#!/usr/bin/env python3
"""Record an existing raw ARM64 RAM smoke test with exact payload evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

def sha(data): return hashlib.sha256(data).hexdigest()

def payload_from_boot(image):
    if image[:8] != b'ANDROID!' or len(image) < 4096:
        raise ValueError('Invalid Android test image')
    kernel_size, size = struct.unpack_from('<II', image, 8)
    start = 4096 + ((kernel_size + 4095) // 4096) * 4096
    payload = image[start:start + size]
    if len(payload) != size or len(payload) < 144:
        raise ValueError('Truncated test payload')
    magic, version, header_bytes, kernel_bytes, initrd_bytes, kernel_hash, initrd_hash, dtb_bytes, dtb_hash = struct.unpack_from('<16sIIQQ32s32sQ32s', payload)
    if (magic, version, header_bytes) != (b'SUNUEFI-LINUXv2\0', 2, 144) or 144 + kernel_bytes + initrd_bytes + dtb_bytes != size:
        raise ValueError('Invalid v2 payload bounds')
    parts = [payload[144:144 + kernel_bytes], payload[144 + kernel_bytes:144 + kernel_bytes + initrd_bytes], payload[144 + kernel_bytes + initrd_bytes:]]
    if any(hashlib.sha256(data).digest() != expected for data, expected in zip(parts, (kernel_hash, initrd_hash, dtb_hash))):
        raise ValueError('Payload component SHA mismatch')
    return dict(payload_sha256=sha(payload), kernel_sha256=sha(parts[0]), initrd_sha256=sha(parts[1]), dtb_sha256=sha(parts[2]))

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--profile', choices=('stable', 'next'), required=True)
    ap.add_argument('--test-id', type=int, required=True)
    args = ap.parse_args(); root = Path(__file__).resolve().parent.parent
    source = root / 'artifacts/kernels' / args.profile / 'ram'
    metadata = json.loads((source / 'manifest.json').read_text())
    init = json.loads((source / 'initramfs-manifest.json').read_text())
    private = root / 'private/analysis'
    record = json.loads((private / f'stage0-test-{args.test_id}.json').read_text())
    partition = json.loads((private / f'partition-verification-test-{args.test_id}.json').read_text())
    log = (private / f'ramlog-test-{args.test_id}/console.txt').read_text(errors='replace')
    archive = root / f'artifacts/tests/stage0-test-{args.test_id}'
    image = (archive / 'piano-stage0-UNTESTED.img').read_bytes()
    test_meta = json.loads((archive / 'manifest.json').read_text())
    pin = json.loads((root / 'linux/kernel-profiles.json').read_text())['profiles'][args.profile]['commit']
    if record.get('profile') != 'linux' or record.get('fastboot_boot', {}).get('exit_code') != 0 or record.get('flash_commands_performed') or not partition['all_26_match']:
        raise ValueError('No successful RAM-only test with restored Android partition verification')
    if sha(image) != record['image_sha256'] or test_meta['linux_handoff_mode'] != 'raw-arm64':
        raise ValueError('Wrong test image or handoff mode')
    payload = payload_from_boot(image)
    if metadata['source_commit'] != pin or metadata['profile'] != args.profile or metadata['mode'] != 'ram':
        raise ValueError('Build/profile/pin mismatch')
    if payload['kernel_sha256'] != metadata['image']['sha256'] or payload['initrd_sha256'] != init['initramfs']['sha256'] or sha((source / 'config').read_bytes()) != metadata['config_sha256']:
        raise ValueError('Test payload and current build provenance differ')
    markers = [f'PIANO_KERNEL_RAM BEGIN pid=1 kernel={metadata["kernel_release"]}',
               f'PIANO_KERNEL_RAM BUILD source_commit={pin}',
               f'PIANO_KERNEL_RAM CONFIG_RUNTIME_SHA256 {metadata["config_sha256"]}',
               'PIANO_KERNEL_RAM CPU_ONLINE 0-7',
               'PIANO_KERNEL_RAM DIAGNOSTICS_READY no_modules_loaded_by_init',
               'PIANO_KERNEL_RAM TIMEOUT seconds=180 rebooting', 'reboot: Restarting system']
    if any(marker not in log for marker in markers) or 'Kernel panic -' in log:
        raise ValueError('RAM smoke incomplete or panicked')
    memory = re.search(r'PIANO_KERNEL_RAM MEMINFO MemTotal:\s+(\d+) kB', log)
    if not memory or not 14 * 1024 * 1024 <= int(memory[1]) <= 16 * 1024 * 1024:
        raise ValueError('Unexpected RAM capacity')
    mounts = re.findall(r'PIANO_KERNEL_RAM MOUNT \S+ \S+ (\S+) ', log)
    if not mounts or set(mounts) - {'rootfs', 'devtmpfs', 'proc', 'sysfs', 'tmpfs'}:
        raise ValueError('Unexpected persistent mount in RAM smoke')
    result = dict(status='VERIFIED_RAW_ARM64_RAM_SMOKE_ONLY', test_id=args.test_id,
                  profile=args.profile, source_commit=pin, kernel_release=metadata['kernel_release'],
                  config_sha256=metadata['config_sha256'], boot_image_sha256=sha(image), **payload,
                  cpu_online='0-7', memory_kb=int(memory[1]), pid1=True,
                  virtual_mount_types=sorted(set(mounts)), loaded_modules_by_init=0,
                  android_auto_recovery_seconds=180, all_26_boot_partitions_match=True,
                  limitations=['Raw ARM64 handoff; EFI stub/runtime services not verified.',
                               'No native display, GPU, networking, touch, audio or distribution validation.',
                               'Captured stock DTB topology still needs mainline board integration.'])
    target = source / f'ram-validation-test-{args.test_id}.json'
    with target.open('x') as stream: stream.write(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))

if __name__ == '__main__': main()
