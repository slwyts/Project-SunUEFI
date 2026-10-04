#!/usr/bin/env python3
"""Package the byte-for-byte captured kernel and RAM initramfs on the host."""
import hashlib
import json
from pathlib import Path
import struct


def main():
    root = Path(__file__).resolve().parent.parent
    capture = root / 'private/captures/2026-10-03-piano'
    manifest = json.loads((capture / 'manifest.json').read_text())
    boot = (capture / 'boot_a.img').read_bytes()
    if hashlib.sha256(boot).hexdigest() != manifest['files']['boot_a.img']['sha256']:
        raise SystemExit('Captured boot_a hash mismatch')
    if boot[:8] != b'ANDROID!' or struct.unpack_from('<I', boot, 40)[0] not in (3, 4):
        raise SystemExit('Expected Android boot header 3 or 4')
    size = struct.unpack_from('<I', boot, 8)[0]
    kernel = boot[4096:4096 + size]
    if len(kernel) != size or kernel[56:60] != b'ARM\x64' or kernel[:2] != b'MZ':
        raise SystemExit('Captured kernel must be a raw ARM64 EFI Image')
    pe = struct.unpack_from('<I', kernel, 60)[0]
    if (pe + 6 > len(kernel) or kernel[pe:pe + 4] != b'PE\0\0' or
            struct.unpack_from('<H', kernel, pe + 4)[0] != 0xaa64):
        raise SystemExit('Expected ARM64 PE/COFF kernel')
    out = root / 'artifacts/linux-ram'
    initrd = (out / 'initramfs.cpio.gz').read_bytes()
    khash = hashlib.sha256(kernel).digest()
    ihash = hashlib.sha256(initrd).digest()
    header = struct.pack('<16sIIQQ32s32s', b'SUNUEFI-LINUXv1\0', 1, 104,
                         len(kernel), len(initrd), khash, ihash)
    payload = header + kernel + initrd
    (out / 'Image.efi').write_bytes(kernel)
    (out / 'linux-payload.bin').write_bytes(payload)
    result = {'status': 'HOST_ARTIFACT_ONLY_NOT_BOOTED', 'header_bytes': 104,
              'kernel_bytes': len(kernel), 'initrd_bytes': len(initrd),
              'kernel_sha256': khash.hex(), 'initrd_sha256': ihash.hex(),
              'payload_bytes': len(payload), 'payload_sha256': hashlib.sha256(payload).hexdigest(),
              'kernel_matches_captured_boot_a': True}
    (out / 'payload-manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
