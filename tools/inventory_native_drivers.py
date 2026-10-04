#!/usr/bin/env python3
"""Inventory captured piano DXE drivers and decode original DEPEX, host only."""
import hashlib
import json
from pathlib import Path
import uuid

ROOTS = ('ChipInfo','PlatformInfoDxeDriver','DALSys','HWIODxeDriver','HALIOMMU',
         'SmemDxe','ULogDxe','CmdDbDxe','PwrUtilsDxe','RpmhDxe','NpaDxe','VcsDxe',
         'MailboxDxe','QcomScmiDxe','ClockDxe','ICBDxe','DALTLMM','Qup','I2C','SPI',
         'GpiDxe','SPMI','PmicDxe','IPCCDxe','SerialPortDxe','ButtonsDxe','PmicGlinkDxe','UsbPwrCtrlDxe',
         'UsbfnDwc3Dxe','UsbConfigDxe','UsbDeviceDxe','UsbInitDxe','XhciPciEmulation',
         'XhciDxe','UFSDxe')


def decode(data):
    names = {0:'AFTER',1:'BEFORE',2:'PUSH',3:'AND',4:'OR',5:'NOT',6:'TRUE',7:'FALSE',8:'END',9:'SOR'}
    tokens = [];offset=0
    while offset<len(data):
        code=data[offset];offset+=1
        token={'op':names.get(code,f'UNKNOWN_{code}')}
        if code in (0,1,2):
            if offset+16>len(data):raise ValueError('Truncated DEPEX GUID')
            token['guid']=str(uuid.UUID(bytes_le=data[offset:offset+16]));offset+=16
        tokens.append(token)
        if code==8:break
    return tokens


root=Path(__file__).resolve().parent.parent
capture=root/'private/captures/2026-10-03-piano'
manifest=json.loads((capture/'manifest.json').read_text())
if hashlib.sha256((capture/'uefi_a.img').read_bytes()).hexdigest()!=manifest['files']['uefi_a.img']['sha256']:
    raise SystemExit('Native UEFI capture hash mismatch')
drivers={}
for ui in (root/'private/uefi-extracted').rglob('*.ui'):
    name=ui.read_bytes().decode('utf-16-le',errors='replace').strip('\0')
    if name not in ROOTS:continue
    if name in drivers:raise ValueError('Ambiguous native module '+name)
    files=list(ui.parent.glob('*.pe'));depex=list(ui.parent.glob('*.dxe.depex'))
    if len(files)!=1 or len(depex)>1:raise ValueError('Ambiguous PE/DEPEX')
    pe=files[0];data=depex[0].read_bytes() if depex else b''
    drivers[name]={'file_guid':ui.parent.name.removeprefix('file-'),'pe_path':str(pe),
                   'bytes':pe.stat().st_size,'pe_sha256':hashlib.sha256(pe.read_bytes()).hexdigest(),
                   'depex':decode(data),'depex_path':str(depex[0]) if depex else None,
                   'status':'INVENTORIED_NOT_ACTIVATED'}
output=root/'private/analysis/native-driver-inventory.json'
output.write_text(json.dumps({'drivers':drivers,'missing':[n for n in ROOTS if n not in drivers],
    'note':'Original device modules; DEPEX identifies required protocols, not producers. No device code executed.'},indent=2)+'\n')
print(json.dumps({'drivers':len(drivers),'missing':[n for n in ROOTS if n not in drivers],'output':str(output)},indent=2))
