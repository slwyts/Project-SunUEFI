#!/usr/bin/env python3
"""Package the actual shared product FD and validated APPv1 into one image."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

from analyze_capture import parse_fdt
from build_integrity import validate as validate_build
from product_contract import ROOT,validate,validate_build_manifest
from simpleinit_build_identity import inspect as inspect_simpleinit


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--header-version',type=int,choices=(3,4),default=3)
    args=parser.parse_args();root=ROOT;ws=root/'upstream/Mu-Silicium';out=root/'artifacts/product'
    build=validate_build(root,'product')
    contract=validate(json.loads((root/'config/piano-product.json').read_text()))
    prepared=json.loads((root/'build/product/prepared-manifest.json').read_text())
    validate_build_manifest(contract,prepared)
    if set(prepared['backends'])!=set(contract['features']):raise ValueError('Product backend disclosure incomplete')
    app=root/'artifacts/simpleinit/product'
    identity=inspect_simpleinit(root/'build/simpleinit-product-edk2',app,True)
    if identity!=prepared['simpleinit'] or identity!=json.loads((app/'build-ok.json').read_text()):raise ValueError('Product application identity changed')
    core=ws/'Build/pianoProductPkg/DEBUG_CLANGPDB/AARCH64/pianoProductPkg/Applications/ProductCore/ProductCore'
    core_make=(core/'GNUmakefile').read_text()
    if 'PianoProductPumpLib/PianoProductPumpLib/OUTPUT/PianoProductPumpLib.lib' not in core_make:raise ValueError('Product core linked the wrong Pump library')
    dxe_make=ws/'Build/pianoProductPkg/DEBUG_CLANGPDB/AARCH64/MdeModulePkg/Core/Dxe/DxeMain/GNUmakefile'
    if 'PianoProductPumpLib/PianoProductPumpLib/OUTPUT/PianoProductPumpLib.lib' not in dxe_make.read_text():raise ValueError('Product DXE Core linked Null service pump')
    fd=out/'PianoUEFI-product.fd';shim=out/'BootShim.bin';dtb=ws/'Resources/DTBs/piano.dtb'
    fd_bytes=fd.read_bytes();shim_bytes=shim.read_bytes()
    if len(fd_bytes)!=0x300000 or shim_bytes[56:60]!=b'ARM\x64':raise ValueError('Unexpected product FD/BootShim')
    symbols=subprocess.check_output([str(Path(os.environ.get('SUNUEFI_TOOLCHAIN_ROOT',str(root/'build/host-tools/usr')))/'bin/aarch64-linux-gnu-nm'),'-n',str(root/'uefi/handoff/bootshim/BootShim.elf')],text=True)
    offsets=[int(row.split()[0],16)for row in symbols.splitlines()if len(row.split())==3 and row.split()[2]=='_Payload']
    if offsets!=[len(shim_bytes)]:raise ValueError('Product BootShim FD payload offset mismatch')
    for path in (fd,ws/'Build/pianoProductPkg/DEBUG_CLANGPDB/FV/FVMAIN.Fv'):
        if not struct.unpack_from('<I',path.read_bytes(),44)[0]&4:raise ValueError('Product FV READ_STATUS is clear')
    nodes=parse_fdt(dtb.read_bytes())
    if any(key in nodes.get('/chosen',{})for key in ('linux,initrd-start','linux,initrd-end','kaslr-seed','rng-seed')):raise ValueError('Stale packaged DT handoff')
    payload=(app/'app-payload.bin').read_bytes();raw=(app/'SimpleInit.efi').read_bytes()
    magic,version,header,app_bytes,digest=struct.unpack_from('<16sIIQ32s',payload)
    if magic!=b'SUNUEFI-APPv1\0'.ljust(16,b'\0')or version!=1 or header!=64 or app_bytes!=len(raw)or payload[64:]!=raw or digest!=hashlib.sha256(raw).digest():raise ValueError('Product APPv1 payload mismatch')
    kernel=out/'product-kernel.bin';kernel.write_bytes(gzip.compress(shim_bytes+fd_bytes,mtime=0)+dtb.read_bytes())
    image=out/'PianoUEFI-product.img'
    subprocess.run([sys.executable,str(ws/'Resources/Scripts/mkbootimg.py'),'--header_version',str(args.header_version),'--kernel',str(kernel),'--ramdisk',str(app/'app-payload.bin'),'-o',str(image)],check=True)
    blob=image.read_bytes();ks,rs=struct.unpack_from('<2I',blob,8)
    roff=4096+((ks+4095)//4096)*4096
    if blob[:8]!=b'ANDROID!'or struct.unpack_from('<I',blob,40)[0]!=args.header_version or blob[4096:4096+ks]!=kernel.read_bytes()or rs!=len(payload)or blob[roff:roff+rs]!=payload:raise ValueError('Product boot image contents mismatch')
    result={**prepared,'build_id':build['build_id'],'build_inputs':build['inputs'],'android_header_version':args.header_version,
        'status':'INCOMPLETE_NOT_RELEASE','not_release_reasons':[name+': '+entry['status']for name,entry in prepared['backends'].items()],
        'single_image_entry_compatibility':'same bytes; all entry-specific physical boot/recovery/fastboot_boot validation pending',
        'files':{path.name:{'bytes':path.stat().st_size,'sha256':sha(path)}for path in (image,fd,shim,kernel)},
        'core_application_sha256':sha(core/'OUTPUT/PianoProductCore.efi'),'device_boot_performed':False,'os_boot_performed':False}
    (out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'artifact':str(image),'sha256':sha(image),'bytes':image.stat().st_size,'status':result['status'],'device_boot_performed':False},indent=2))


if __name__=='__main__':main()
