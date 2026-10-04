#!/usr/bin/env python3
"""Package the experimental FD on the host. Contains no flashing/boot command."""
import gzip
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
from analyze_capture import parse_fdt
from build_integrity import validate

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--profile', choices=('stage0', 'probe', 'linux', 'gui'), default='stage0')
    ap.add_argument('--header-version', type=int, choices=(3,4), default=4)
    args = ap.parse_args()
    root = Path(__file__).resolve().parent.parent
    ws = root / 'upstream/Mu-Silicium'
    out = root / 'artifacts' / args.profile
    try:
        build_record=validate(root,args.profile)
    except (ValueError,OSError,KeyError) as error:
        raise SystemExit(str(error))
    name = {'stage0':'piano','probe':'pianoProbe','linux':'pianoLinux','gui':'pianoGui'}[args.profile]
    fd = (out / 'piano-stage0.fd').read_bytes()
    shim = (out / 'BootShim.bin').read_bytes()
    dtb = (ws / 'Resources/DTBs/piano.dtb').read_bytes()
    if len(fd) != 0x300000 or shim[56:60] != b'ARM\x64':
        raise SystemExit('Unexpected FD/BootShim header')
    elf = ws / 'BootShim/BootShim.elf' if args.profile == 'stage0' else root / 'bootprofiles/handoff/BootShim.elf'
    symbols = subprocess.check_output([str(root / 'build/host-tools/usr/bin/aarch64-linux-gnu-nm'),
                                       '-n', str(elf)], text=True)
    payload_offsets = [int(line.split()[0], 16) for line in symbols.splitlines()
                       if len(line.split()) == 3 and line.split()[2] == '_Payload']
    if payload_offsets != [len(shim)]:
        raise SystemExit('BootShim _Payload does not point to the appended FD; constant pool/padding mismatch')
    # PI FV reads return EFI_ACCESS_DENIED when READ_STATUS is clear.
    fv_files = (out / 'piano-stage0.fd', ws / f'Build/{name}Pkg/DEBUG_CLANGPDB/FV/FVMAIN.Fv')
    for fv_file in fv_files:
        attributes = struct.unpack_from('<I', fv_file.read_bytes(), 44)[0]
        if not attributes & 0x4:
            raise SystemExit(f'FV READ_STATUS is clear: {fv_file}')
    nodes = parse_fdt(dtb)
    if any(k in nodes.get('/chosen', {}) for k in ('linux,initrd-start','linux,initrd-end','kaslr-seed','rng-seed')):
        raise SystemExit('Stale boot handoff pointers/seeds in DTB')
    kernel = gzip.compress(shim + fd, mtime=0) + dtb
    (out / 'stage0-kernel.bin').write_bytes(kernel)
    image = out / 'piano-stage0-UNTESTED.img'
    ramdisk = (root / 'artifacts/linux-ram/linux-payload.bin' if args.profile == 'linux' else
               root / 'artifacts/simpleinit/app-payload.bin' if args.profile == 'gui' else ws / 'Resources/ramdisk')
    subprocess.run([sys.executable, str(ws / 'Resources/Scripts/mkbootimg.py'),
                    '--header_version', str(args.header_version), '--kernel', str(out / 'stage0-kernel.bin'),
                    '--ramdisk', str(ramdisk), '-o', str(image)], check=True)
    boot = image.read_bytes()
    if boot[:8] != b'ANDROID!' or struct.unpack_from('<I', boot, 40)[0] != args.header_version:
        raise SystemExit('Invalid Android boot image header')
    ks, rs = struct.unpack_from('<2I', boot, 8)
    if ks != len(kernel) or boot[4096:4096+ks] != kernel:
        raise SystemExit('Kernel packaging mismatch')
    ramdisk_offset = 4096 + ((ks + 4095) // 4096) * 4096
    if rs != ramdisk.stat().st_size or boot[ramdisk_offset:ramdisk_offset+rs] != ramdisk.read_bytes():
        raise SystemExit('Ramdisk packaging mismatch')
    linux_mode = None
    linux_payload = None
    if args.profile == 'linux':
        app = ws / 'Platforms/Xiaomi/pianoLinuxPkg/Applications/LinuxRamBoot/LinuxRamBoot.c'
        linux_mode = 'raw-arm64' if app.read_text().startswith('#define SUNUEFI_RAW_HANDOFF 1') else 'efi-stub'
        if ramdisk.read_bytes()[:16] == b'SUNUEFI-LINUXv2\0':
            linux_payload = json.loads((ramdisk.parent/'payload-manifest.json').read_text())
            if linux_payload['payload_sha256'] != hashlib.sha256(ramdisk.read_bytes()).hexdigest() or linux_payload['payload_bytes'] != rs:
                raise SystemExit('Pinned Linux payload provenance does not match packaged bytes')
    checks = {
        'build_id':build_record['build_id'],
        'build_inputs':build_record['inputs'],
        'status': 'HOST_BUILD_ONLY_NOT_HARDWARE_VALIDATED',
        'purpose': (f'RAM Linux {linux_mode} experiment; not hardware validated' if args.profile == 'linux'
                    else 'UEFI GOP/simple-init RAM experiment; not hardware validated' if args.profile == 'gui'
                    else 'UEFI initialization/display diagnostic; no OS loader'),
        'safety_limit': 'Native blob side effects and bootloader handoff are not hardware verified',
        'android_header_version': args.header_version, 'kernel_size': ks, 'ramdisk_size': rs,
        'linux_handoff_mode': linux_mode,
        'linux_payload': linux_payload,
        'ramdisk_sha256': hashlib.sha256(ramdisk.read_bytes()).hexdigest(),
        'fd_base': '0xA7100000', 'fd_size': len(fd),
        'files': {p.name: {'bytes': p.stat().st_size,
                           'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                  for p in (image, out / 'piano-stage0.fd', out / 'BootShim.bin')},
        'capsule_policy': 'PianoCapsuleArchNullDxe provides the required protocol; UpdateCapsule and QueryCapsuleCapabilities always return EFI_UNSUPPORTED',
        'device_boot_performed': False, 'os_boot_performed': False,
    }
    (out / 'manifest.json').write_text(json.dumps(checks, indent=2) + '\n')
    print(json.dumps(checks, indent=2))

if __name__ == '__main__':
    main()
