#!/usr/bin/env python3
"""Package pinned built Image/initramfs/final DTB for the UEFI RAM loader."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from build_kernel import image_info

def digest(data):return hashlib.sha256(data).digest()
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--profile',choices=('stable','next'),required=True)
    ap.add_argument('--mode',choices=('ram','userspace-debug'),default='ram')
    ap.add_argument('--dtb',type=Path,required=True)
    args=ap.parse_args();root=Path(__file__).resolve().parent.parent
    source=root/'artifacts/kernels'/args.profile/args.mode
    metadata=json.loads((source/'manifest.json').read_text())
    init_meta=json.loads((source/'initramfs-manifest.json').read_text())
    kernel=(source/'Image').read_bytes();initrd=(source/'initramfs.cpio.gz').read_bytes();dtb=args.dtb.read_bytes()
    pin=json.loads((root/'linux/kernel-profiles.json').read_text())['profiles'][args.profile]
    if metadata.get('profile')!=args.profile or metadata.get('mode')!=args.mode or metadata.get('source_commit')!=pin['commit']:
        raise SystemExit('Kernel profile/mode/pinned commit mismatch')
    if metadata['status']!='HOST_BUILT_NOT_HARDWARE_VERIFIED' or metadata['source_dirty']:
        raise SystemExit('Kernel is not a clean completed build')
    if digest(kernel).hex()!=metadata['image']['sha256'] or not image_info(source/'Image')['efi_stub']:
        raise SystemExit('Kernel manifest/image mismatch')
    if digest((source/'config').read_bytes()).hex()!=metadata.get('config_sha256'):
        raise SystemExit('Kernel config/manifest mismatch')
    if init_meta.get('source_commit')!=metadata['source_commit']:raise SystemExit('Initramfs/kernel commit mismatch')
    if init_meta.get('profile')!=args.profile or init_meta.get('mode')!=args.mode:
        raise SystemExit('Initramfs profile/mode mismatch')
    if init_meta.get('kernel_image',{}).get('sha256')!=digest(kernel).hex():
        raise SystemExit('Initramfs provenance/image mismatch')
    expected=init_meta.get('initramfs',{}).get('sha256')
    if digest(initrd).hex()!=expected:raise SystemExit('Initramfs manifest mismatch')
    if len(dtb)<40 or dtb[:4]!=b'\xd0\x0d\xfe\xed' or struct.unpack_from('>I',dtb,4)[0]!=len(dtb):
        raise SystemExit('Expected a complete explicitly selected board FDT')
    header=struct.pack('<16sIIQQ32s32sQ32s',b'SUNUEFI-LINUXv2\0',2,144,len(kernel),len(initrd),
        digest(kernel),digest(initrd),len(dtb),digest(dtb))
    payload=header+kernel+initrd+dtb
    out=root/'artifacts/linux-ram';out.mkdir(exist_ok=True)
    (out/'linux-payload.bin').write_bytes(payload)
    result={'status':'HOST_PACKAGED_NOT_BOOTED','payload_version':2,'profile':args.profile,'mode':args.mode,
        'source_commit':metadata['source_commit'],'config_sha256':metadata['config_sha256'],
        'kernel_sha256':digest(kernel).hex(),'initrd_sha256':digest(initrd).hex(),
        'dtb_source':str(args.dtb.resolve()),'dtb_sha256':digest(dtb).hex(),
        'dtb_board_topology_verified':False,'payload_sha256':digest(payload).hex(),'payload_bytes':len(payload),
        'runtime_dtb_from_abl':False,'storage_drivers_allowed':args.mode!='ram','hardware_verified':False}
    (out/'payload-manifest.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))

if __name__=='__main__':main()
