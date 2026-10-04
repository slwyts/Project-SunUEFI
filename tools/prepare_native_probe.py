#!/usr/bin/env python3
"""Stage dependency-gated native support modules as passive FFS, host only."""
import argparse
import hashlib
import json
from pathlib import Path
import uuid

BASE = ('SmemDxe','DALSys','ChipInfo','PlatformInfoDxeDriver','HWIODxeDriver','ULogDxe')
IO = ('CmdDbDxe','PwrUtilsDxe','RpmhDxe','NpaDxe','MailboxDxe','QcomScmiDxe','VcsDxe',
      'ClockDxe','HALIOMMU','ICBDxe','DALTLMM','Qup','I2C','SPI','GpiDxe','SPMI','PmicDxe',
      'IPCCDxe','ButtonsDxe')
SPI = ('CmdDbDxe','PwrUtilsDxe','RpmhDxe','NpaDxe','VcsDxe','ClockDxe',
       'DALTLMM','Qup','SPI')
USB = ('CmdDbDxe','PwrUtilsDxe','RpmhDxe','NpaDxe','VcsDxe','ClockDxe',
       'HALIOMMU','UsbfnDwc3Dxe','UsbDeviceDxe','UsbConfigDxe')

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--group',choices=('foundation','io','spi','gpi','usb','ufs'),default='foundation')
parser.add_argument('--exclude',action='append',default=[],metavar='MODULE',
                    help='Omit a selected module for fault isolation; repeat as needed')
args=parser.parse_args()
root=Path(__file__).resolve().parent.parent
catalog=json.loads((root/'private/analysis/native-driver-inventory.json').read_text())['drivers']
selected=BASE+({'io':IO,'spi':SPI,'gpi':SPI+('GpiDxe',),'usb':USB,
               'ufs':USB[:6]+('HALIOMMU',)}.get(args.group,()))
unknown=set(args.exclude)-set(selected)
if unknown:
    parser.error('Module is not in selected group: '+', '.join(sorted(unknown)))
names=tuple(name for name in selected if name not in args.exclude)
if not names:
    parser.error('At least one module must remain selected')
header=['typedef struct { CONST CHAR8 *Name; EFI_GUID Guid; CONST UINT8 *Depex; UINTN DepexBytes; } NATIVE_IMAGE;']
ffs=[]
for i,name in enumerate(names):
    row=catalog[name];pe=Path(row['pe_path']).read_bytes()
    if hashlib.sha256(pe).hexdigest()!=row['pe_sha256']:raise ValueError('Native PE hash mismatch')
    folder=root/'upstream/Mu-Silicium/Binaries/piano/Bringup'/name;folder.mkdir(parents=True,exist_ok=True)
    (folder/(name+'.efi')).write_bytes(pe)
    dep=Path(row['depex_path']).read_bytes() if row['depex_path'] else b'\x06\x08'
    header.append(f'STATIC CONST UINT8 mDepex{i}[]={{'+','.join(hex(b) for b in dep)+'};')
    ffs.append('  FILE FREEFORM = '+row['file_guid']+' {\n    SECTION PE32 = Binaries/piano/Bringup/'+name+'/'+name+'.efi\n  }')
header.append('STATIC CONST NATIVE_IMAGE mNativeImages[]={')
for i,name in enumerate(names):
    guid=uuid.UUID(catalog[name]['file_guid']);fields=guid.fields
    literal='{0x%08X,0x%04X,0x%04X,{'%fields[:3]+','.join(f'0x{b:02X}' for b in guid.bytes[8:])+'}}'
    header.append('{"'+name+'",'+literal+f',mDepex{i},sizeof(mDepex{i})'+'},')
header.append('};')
(root/'bootprofiles/uefi-app/NativeProbeTable.h').write_text('\n'.join(header)+'\n')
(root/'build/native-foundation.fdf.inc').write_text('\n'.join(ffs)+'\n')
(root/'build/native-probe-selection.json').write_text(json.dumps({
    'group':args.group,'modules':names,'excluded':sorted(set(args.exclude)),
    'storage_drivers':False,
    'pe_sha256':{name:catalog[name]['pe_sha256'] for name in names},
},indent=2)+'\n')
print(f'Staged {len(names)} passive support modules with original dependency expressions')
