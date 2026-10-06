#!/usr/bin/env python3
"""Prepare one PianoUEFI product integration candidate; host-only, no device."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import uuid

from product_contract import ROOT, validate
from prepare_product_pump import prepare as prepare_pump
from product_payload_digest import digest_header
from simpleinit_build_identity import inspect as inspect_simpleinit
from prepare_gui_profile import SETUP_DSC_ADDITIONS, SETUP_FV_MODULES
from prepare_product_early_memory import prepare as prepare_early_memory, verify as verify_early_memory
from prepare_product_handoff import prepare as prepare_handoff, stage_provider, verify_provider
import piano_display_mapping as display_mapping

CORE_GUID='35E0D1B5-93CE-4D6A-9A93-6ADAA3F26C40'
SOURCE_NAMES=(
    'PianoProductCore.c','PianoProductBootLog.c','PianoProductDisplayObserve.c','PianoDisplaySmmuObserve.c','PianoFrameBufferMappingObserve.c','PianoDisplayClockObserve.c','PianoProductDisplayOwner.c','PianoDisplayClockLease.c','PianoDisplayClockRead.c','PianoBootPolicy.c','PianoFvApplication.c','PianoProductPayload.c','PianoProductOwners.c','PianoRamPartition.c','PianoProductSmem.c',
    'NativeProbe.c','PianoKeys.c','PianoFaultRecovery.c',
    'PianoSmmu.c','PianoDma.c','PianoOwnedSmmu.c','PianoIoPageTable.c',
    'PianoUfsProbe.c','PianoUfsReadOnlyDma.c','PianoUfsDmaLayout.c','PianoGpt.c','PianoReadOnlyBlock.c',
    'PianoUfsProductVolume.c','PianoUfsBoundedLayout.c',
    'PianoFastboot.c','PianoFastbootBlockRead.c','PianoFastbootBoot.c','PianoFastbootLaunch.c','PianoFastbootDownloadBlob.c','PianoRawLinuxBoot.c',
    'PianoFastbootScreen.c','PianoDwc3Device.c','PianoUsbControl.c','PianoUsbController.c',
    'PianoPogoReport.c','PianoPogoInput.c','PianoPogoI2c.c','PianoPogoTransport.c','PianoGeniI2cPio.c','PianoUsbHostPci.c',
)
OS_BOOT_SOURCES=('PianoBootFileSource.c','PianoCpuImageLoan.c','PianoLinuxEfiSession.c','PianoCpuInput.c')
OS_BOOT_HEADERS=tuple(name[:-2]+'.h' for name in OS_BOOT_SOURCES)
LATE_HANDOFF_INF_SOURCES=('LateHandoff/PianoLateHandoff.c','LateHandoff/PianoLateHandoff.h')
OS_BOOT_INF_SOURCES=tuple('OsBoot/'+name for name in (*OS_BOOT_SOURCES,*OS_BOOT_HEADERS))
OBSERVATION_FAMILIES={'early-memory':('PianoSmemRam.c','PianoSmemRam.h'),
                      'guarded-read':('PianoGuardedRead.c','PianoGuardedRead.h'),
                      'display-rail':('PianoDisplayRailObserve.c','PianoDisplayRailObserve.h','PianoDisplayNonGdscClock.c','PianoDisplayNonGdscClock.h')}
OBSERVATION_INF_SOURCES=tuple(name for names in OBSERVATION_FAMILIES.values() for name in names)
PRODUCT_FLAGS=('PIANO_USB_SERVICE=1','PIANO_USB_EP0=1','PIANO_USB_FASTBOOT=1','PIANO_USB_SCREENSHOT=1',
    'PIANO_USB_UFS_FETCH=1','PIANO_USB_RAM_BOOT=1','PIANO_USB_POWER_PROBE=1','PIANO_UFS_BLOCKIO=1',
    'PIANO_UFS_PRODUCT_STORAGE=1','PIANO_NV_BOOT_ONLY=1','PIANO_PRODUCT_NATIVE_LATE=1')
NATIVE_NAMES=('SmemDxe','DALSys','ChipInfo','PlatformInfoDxeDriver','HWIODxeDriver','ULogDxe',
    'CmdDbDxe','PwrUtilsDxe','RpmhDxe','NpaDxe','VcsDxe','ClockDxe','HALIOMMU')
DISK_MODULES=('MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf',
    'MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf',
    'MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf','FatPkg/EnhancedFatDxe/Fat.inf')
HOST_MODULES=('MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf','MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf',
    'MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf','MdeModulePkg/Bus/Usb/UsbMouseDxe/UsbMouseDxe.inf',
    'MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf')


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_display_mapping(root,target,record):
    if set(record)!={'candidate','compiled_into_product','hardware_verified','register_access_authorized'} or \
       record['compiled_into_product'] is not True or record['hardware_verified'] is not False or \
       record['register_access_authorized'] is not False:
        raise ValueError('Product display mapping binding or readiness metadata changed')
    memory=Path(target)/'Library/MemoryMapLib/MemoryMapLib.c'
    if memory.is_symlink()or not memory.is_file():raise ValueError('Product display mapping source missing or unowned')
    return display_mapping.verify(root,memory.read_text(),record['candidate'])


def observation_files(root):
    files={family:{name:root/'bootprofiles'/family/name for name in names}
           for family,names in OBSERVATION_FAMILIES.items()}
    for family,rows in files.items():
        for name,path in rows.items():
            if not path.is_file() or path.is_symlink():raise ValueError('Canonical DXE observation input missing or unowned: '+family+'/'+name)
    return files


def prepare_observation_families(root,app):
    families=observation_files(root)
    for rows in families.values():
        for name,path in rows.items():
            if (root/'bootprofiles/uefi-app'/name).exists():raise ValueError('Flat DXE observation name conflicts with core source: '+name)
            shutil.copyfile(path,app/name)
    return {'status':'READ_ONLY_DXE_OBSERVATION_BOUND_UNTESTED','phase':'DXE_AFTER_FOUNDATION_BEFORE_RAM_INVENTORY_UFS_USB',
            'platform_bound':True,'device_validated':False,'sec_early_ready':False,
            'high_ddr_mapped':False,'memory_ownership_granted':False,
            'sources':list(OBSERVATION_INF_SOURCES),
            'families':{family:{name:sha(path)for name,path in sorted(rows.items())}for family,rows in families.items()}}


def verify_observation_families(root,app,record):
    expected={family:{name:sha(path)for name,path in sorted(rows.items())}for family,rows in observation_files(root).items()}
    if record.get('families')!=expected or record.get('sources')!=list(OBSERVATION_INF_SOURCES):
        raise ValueError('Prepared DXE observation families differ from canonical sources')
    for rows in expected.values():
        for name,digest in rows.items():
            path=app/name
            if not path.is_file() or sha(path)!=digest:raise ValueError('ProductCore DXE observation compiled copy stale or missing: '+name)
    inf=(app/'ProductCore.inf').read_text();sources=inf.split('[Sources]\n',1)[1].split('[Packages]',1)[0].splitlines()
    if not set((*OBSERVATION_INF_SOURCES,'PianoProductSmem.c'))<={line.strip()for line in sources}:
        raise ValueError('ProductCore INF omits actual DXE observation source binding')
    guids=inf.split('[Guids]\n',1)[1].split('[Protocols]',1)[0].splitlines()
    protocols=inf.split('[Protocols]\n',1)[1].splitlines()
    if 'gEfiEventExitBootServicesGuid' not in {line.strip()for line in guids} or 'gEfiCpuArchProtocolGuid' not in {line.strip()for line in protocols}:
        raise ValueError('ProductCore INF omits actual DXE observation event or CPU protocol declaration')
    return True


def os_boot_files(root):
    folder=root/'bootprofiles/os-boot'
    files={path.relative_to(folder).as_posix():path for path in folder.rglob('*')
           if path.is_file() and '__pycache__' not in path.parts}
    if any(name not in files for name in (*OS_BOOT_SOURCES,*OS_BOOT_HEADERS)):
        raise ValueError('Canonical OS boot sources or headers missing')
    if {name for name in files if name.endswith('.c')}!=set(OS_BOOT_SOURCES):
        raise ValueError('OS boot C sources must all have explicit ProductCore INF bindings')
    if any(path.is_symlink() for path in files.values()):
        raise ValueError('Canonical OS boot inputs must be ordinary owned files')
    return files


def shared_boot_headers(root):
    folder=root/'bootprofiles/uefi-app'
    return {path.relative_to(folder).as_posix():path for path in folder.rglob('*')
            if path.is_file() and path.suffix in ('.h','.inc')}


def prepare_os_boot(root,app):
    """Byte-identical shared code; preserve its canonical relative includes."""
    files=os_boot_files(root)
    shutil.copytree(root/'bootprofiles/os-boot',app/'OsBoot',
                    ignore=shutil.ignore_patterns('__pycache__'))
    # ../uefi-app paths remain unchanged. The module root already supplies the
    # ordinary quoted PianoFastbootLaunch.h include used by CpuImageLoan.
    headers=shared_boot_headers(root)
    for name,path in headers.items():
        destination=app/'uefi-app'/name;destination.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(path,destination)
    return {'status':'SHARED_IMPLEMENTATION_COMPILED_PLATFORM_NOT_READY',
            'platform_bound':False,'full_ddr_verified':False,'start_enabled':False,
            'configured_source_budget_bytes':64*1024*1024,
            'sources':list(OS_BOOT_INF_SOURCES),
            'canonical_files':{name:sha(path)for name,path in sorted(files.items())},
            'shared_headers':{name:sha(path)for name,path in sorted(headers.items())}}


def verify_os_boot(root,app,record):
    """Freshness includes actual compiled copies, not just a canonical hash."""
    expected={name:sha(path)for name,path in sorted(os_boot_files(root).items())}
    headers={name:sha(path)for name,path in sorted(shared_boot_headers(root).items())}
    if record.get('canonical_files')!=expected or record.get('shared_headers')!=headers or record.get('sources')!=list(OS_BOOT_INF_SOURCES):
        raise ValueError('Prepared shared OS boot identity differs from canonical sources')
    actual={path.relative_to(app/'OsBoot').as_posix():sha(path)
            for path in (app/'OsBoot').rglob('*')if path.is_file() and '__pycache__' not in path.parts}
    if actual!=expected:raise ValueError('ProductCore OS boot compiled copies are stale or missing')
    for name,digest in headers.items():
        for path in (app/'uefi-app'/name,app/name):
            if not path.is_file() or sha(path)!=digest:
                raise ValueError('ProductCore shared OS boot include identity mismatch: '+name)
    sections={};section=None
    for line in (app/'ProductCore.inf').read_text().splitlines():
        line=line.strip()
        if line.startswith('[')and line.endswith(']'):section=line;sections[section]=set()
        elif line and section:sections[section].add(line)
    if not set(OS_BOOT_INF_SOURCES)<=sections.get('[Sources]',set()) or 'SynchronizationLib' not in sections.get('[LibraryClasses]',set()):
        raise ValueError('ProductCore INF omits actual shared OS boot sources or synchronization')
    return True


def fix_product_low_heap(text,dtb):
    """Cold product-only carveout correction; never retag live DXE allocations."""
    from plan_piano_dram import extract_dt
    dt=extract_dt(dtb)
    expected={'/reserved-memory/adspslpi_region@9ee00000':(0xB8200000,0xBD980000),
              '/reserved-memory/hwfence-shmem':(0xD4E23000,0xD5100000)}
    for path,bounds in expected.items():
        rows=[row for row in dt['fixed']if row['path']==path]
        if len(rows)!=1 or (rows[0]['base'],rows[0]['end'])!=bounds or not rows[0]['no_map']:
            raise ValueError('Product low heap fixed owner changed: '+path)
    old='  {"DXE_Heap", 0xBD930000, 0x1A6D0000, AddMem, 0, 0x703C07, 7, WRITE_BACK_XN},'
    new='''  {"Piano_ADSP_Heap_Conflict", 0xBD930000, 0x50000, HobOnlyNoCacheSetting, 5, 0x703C07, 0, WRITE_BACK_XN},
  {"DXE_Heap", 0xBD980000, 0x174A3000, AddMem, 0, 0x703C07, 7, WRITE_BACK_XN},
  {"Piano_HWFence_Reserved", 0xD4E23000, 0x2DD000, HobOnlyNoCacheSetting, 5, 0x703C07, 0, WRITE_BACK_XN},
  {"DXE_Heap_Upper", 0xD5100000, 0x2F00000, AddMem, 0, 0x703C07, 7, WRITE_BACK_XN},'''
    if text.count(old)!=1:raise ValueError('Unexpected original product DXE heap row')
    text=text.replace(old,new)
    # NoHob already excludes DBI from the allocator; remove its misleading
    # Conventional token without altering the inherited UC CPU attribute.
    old='  {"DBI_Dump", 0xB81C0000, 0x5770000, NoHob, 1, 0x2, 7, UNCACHED_UNBUFFERED_XN},'
    if text.count(old)!=1:raise ValueError('Unexpected original DBI owner row')
    text=text.replace(old,old.replace('0x2, 7,','0x2, 0,'))
    return text,{'phase':'COLD_BOOT_ONLY','named_hob_arena':{'base':0xBD980000,'end':0xD4E23000},
                 'upper_resource':{'base':0xD5100000,'end':0xD8000000},
                 'excluded_from_phit_allocator_and_cpu_map':[[0xBD930000,0xBD980000],[0xD4E23000,0xD5100000]],
                 'other_native_cache_attributes_changed':False,'high_ddr_added':False}


def backend_status():
    # Integration/validation state is separate from the required enabled set.
    return {
      'gop':{'status':'IMPLEMENTED_INHERITED_FRAMEBUFFER','physical_evidence':'test84 real GOP screenshot; product untested'},
      'physical_keys':{'status':'IMPLEMENTED_PMIC_READONLY','physical_evidence':'tests23/24; product input retirement untested'},
      'pogo_keyboard_touchpad':{'status':'NOT_READY','missing':'verified SE6 firmware/clock ownership and live report transport'},
      'touchscreen':{'status':'NOT_READY','missing':'verified GPI/PAS/DMA physical touch reports'},
      'dma_smmu':{'status':'IMPLEMENTED_STRICT_OWNERS','physical_evidence':'test91 readonly fetch and exact combined USB/UFS retirement passed; resident product retirement still untested','ram_partition_inventory':'AUDITED_NATIVE_ABI_LINKED_UNTESTED','smem_observation':'READ_ONLY_COLD_SEC_AND_DXE_BOUND_UNTESTED','sec_early_ready':False,'high_ddr_mapped':False,'high_ram_ownership_verified':False},
      'ufs_blockio_read_write':{'status':'RESERVED_VOLUME_BACKEND_UNPROVISIONED','original_media':'READ_ONLY','missing':'explicit permanent reservation, provisioning approval and product physical RW acceptance'},
      'gpt':{'status':'IMPLEMENTED_READ','physical_evidence':'real UFS GPT reads; product untested'},
      'fat_simplefilesystem':{'status':'IMPLEMENTED_READ_ONLY_VOLUMES','physical_evidence':'7 read-only SFS, bounded FAT RW test86; product untested'},
      'persistent_variables':{'status':'RAM_ONLY','prepared_backend':'STANDARD_FVB_DUAL_JOURNAL_NOT_ACTIVATED','runtime_nv_set_supported':False,'missing':'early recovered NV before standard variable initialization; HALIOMMU architectural dependency cycle; PcdEmuVariableNvModeEnable remains TRUE'},
      'uefi_shell':{'status':'LINKED_STANDARD_SHELL','missing':'product cooperative exit and UFS file operations acceptance'},
      'setup_hii':{'status':'LINKED_STANDARD_UIAPP','missing':'product navigation/F12/cooperative exit acceptance'},
      'simpleinit':{'status':'LINKED_PRODUCT_GUI','missing':'product APPv1 load, visible Setup/Shell navigation and background service acceptance'},
      'usb_device_fastboot':{'status':'IMPLEMENTED_RESIDENT_SERVICE_UNTESTED','physical_evidence':'isolated standard bulk82/reboot83/screen84/fetch91; resident product untested','navigation_commands':['oem setup','oem shell','oem simpleinit'],'navigation_physical_validation':False,'current_download_limit_bytes':67108864,'target_download_limit_bytes':1073741824},
      'usb_host':{'status':'NOT_READY','missing':'actual Host PCI_IO/NC common DMA and Type-C/VBUS ownership backend; standard consumers linked'},
      'debug_logs_screenshot':{'status':'IMPLEMENTED_FASTBOOT','physical_evidence':'test82 ramlog and84 screenshot; product UI snapshots untested'},
      'efi_android_linux_boot':{'status':'PARTIAL','shared_os_loader':'COMPILED_PLATFORM_NOT_READY','source_budget_bytes':67108864,'platform_bound':False,'full_ddr_verified':False,'missing':'approved SFS/path source, independent BS fence, actual full-DDR and all-owner EFI handoff binding; generic Android/Recovery/Windows handoff'},
      'os_exit':{'status':'NATIVE_LATE_APP_HOOK_AND_ROOT_PROVIDER_BOUND_UNTESTED','os_image_armed':False,'full_ddr_verified':False,'missing':'live full-DDR authority, actual selected OS Arm and device acceptance'},
    }


def native_modules(root,app):
    catalog=json.loads((root/'private/analysis/native-driver-inventory.json').read_text())['drivers']
    table=['typedef struct { CONST CHAR8 *Name; EFI_GUID Guid; CONST UINT8 *Depex; UINTN DepexBytes; } NATIVE_IMAGE;']
    ffs=[];identities={}
    for index,name in enumerate(NATIVE_NAMES):
        row=catalog[name];source=Path(row['pe_path'])
        if sha(source)!=row['pe_sha256']:raise ValueError('Native PE hash mismatch: '+name)
        destination=root/'upstream/Mu-Silicium/Binaries/piano/ProductFoundation'/name
        destination.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,destination/(name+'.efi'))
        dep=Path(row['depex_path']).read_bytes() if row['depex_path'] else b'\x06\x08'
        table.append(f'STATIC CONST UINT8 mDepex{index}[]={{'+','.join(hex(value) for value in dep)+'};')
        ffs.append(f'  FILE FREEFORM = {row["file_guid"]} {{\n    SECTION PE32 = Binaries/piano/ProductFoundation/{name}/{name}.efi\n  }}')
        identities[name]={'pe_sha256':row['pe_sha256'],'depex_sha256':hashlib.sha256(dep).hexdigest(),'file_guid':row['file_guid']}
    table.append('STATIC CONST NATIVE_IMAGE mNativeImages[]={')
    for index,name in enumerate(NATIVE_NAMES):
        guid=uuid.UUID(catalog[name]['file_guid']);literal='{0x%08X,0x%04X,0x%04X,{'%guid.fields[:3]+','.join(f'0x{value:02X}' for value in guid.bytes[8:])+'}}'
        table.append('{"'+name+'",'+literal+f',mDepex{index},sizeof(mDepex{index})'+'},')
    table.append('};');(app/'NativeProbeTable.h').write_text('\n'.join(table)+'\n')
    return '\n'.join(ffs)+'\n',identities


def core_inf():
    return '''[Defines]
  INF_VERSION = 0x00010005
  BASE_NAME = PianoProductCore
  FILE_GUID = '''+CORE_GUID+'''
  MODULE_TYPE = UEFI_APPLICATION
  VERSION_STRING = 0.1
  ENTRY_POINT = PianoProductCoreEntry
[Sources]
'''+''.join('  '+name+'\n' for name in (*SOURCE_NAMES,*OS_BOOT_INF_SOURCES,*OBSERVATION_INF_SOURCES,*LATE_HANDOFF_INF_SOURCES))+'''[Packages]
  MdePkg/MdePkg.dec
  MdeModulePkg/MdeModulePkg.dec
  QcomPkg/QcomPkg.dec
  CryptoPkg/CryptoPkg.dec
  SiliciumPkg/SiliciumPkg.dec
[LibraryClasses]
  UefiApplicationEntryPoint
  UefiLib
  UefiBootServicesTableLib
  UefiRuntimeServicesTableLib
  MemoryMapLib
  BaseMemoryLib
  BaseLib
  BaseCryptLib
  FdtLib
  DebugLib
  PrintLib
  IoLib
  CacheMaintenanceLib
  DxeServicesTableLib
  DxeServicesLib
  SerialPortLib
  ArmSmcLib
  TimerLib
  MemoryAllocationLib
  DevicePathLib
  PcdLib
  PianoProductPumpLib
  SynchronizationLib
  HobLib
[Guids]
  gEfiEventExitBootServicesGuid
  gEfiEventBeforeExitBootServicesGuid
  gEfiFileInfoGuid
  gEfiFileSystemInfoGuid
  gFdtTableGuid
  gLinuxEfiInitrdMediaGuid
[Protocols]
  gEfiGraphicsOutputProtocolGuid
  gEfiAbsolutePointerProtocolGuid
  gEfiSimplePointerProtocolGuid
  gEfiSerialIoProtocolGuid
  gEfiUsb2HcProtocolGuid
  gEfiUsbIoProtocolGuid
  gEfiBlockIoProtocolGuid
  gEfiSimpleTextInProtocolGuid
  gEfiSimpleTextInputExProtocolGuid
  gEfiDevicePathProtocolGuid
  gEfiCpuArchProtocolGuid
  gEfiLoadedImageProtocolGuid
  gEfiLoadFile2ProtocolGuid
  gEfiPartitionInfoProtocolGuid
  gEfiSimpleFileSystemProtocolGuid
  gEfiFirmwareVolume2ProtocolGuid
  gEfiHiiDatabaseProtocolGuid
  gEfiHiiStringProtocolGuid
  gEfiHiiFontProtocolGuid
  gEfiHiiConfigRoutingProtocolGuid
  gEfiFormBrowser2ProtocolGuid
  gEdkiiFormDisplayEngineProtocolGuid
  gEfiVariableArchProtocolGuid
  gEfiVariableWriteArchProtocolGuid
'''


def prepare(root=ROOT):
    contract=validate(json.loads((root/'config/piano-product.json').read_text()))
    source=root/'bootprofiles/uefi-app';missing=[name for name in SOURCE_NAMES if not(source/name).is_file()]
    if missing:raise ValueError('Actual product core sources missing: '+', '.join(missing))
    os_boot_files(root)
    observation_files(root)
    handoff_hooks=prepare_handoff(root,apply=True)
    prepare_pump(root,apply=True)
    from prepare_product_ui import prepare as prepare_ui
    ui=prepare_ui(root,apply=True)
    product_app=root/'artifacts/simpleinit/product'
    simpleinit=inspect_simpleinit(root/'build/simpleinit-product-edk2',product_app,True)
    if json.loads((product_app/'build-ok.json').read_text())!=simpleinit:raise ValueError('Product SimpleInit build identity stale')
    header,app_identity=digest_header(product_app/'SimpleInit.efi')
    raw=(product_app/'SimpleInit.efi').read_bytes()
    (product_app/'app-payload.bin').write_bytes(struct.pack('<16sIIQ32s',b'SUNUEFI-APPv1\0',1,64,len(raw),hashlib.sha256(raw).digest())+raw)
    target=root/'platforms/pianoProductPkg'
    if target.exists():shutil.rmtree(target)
    shutil.copytree(root/'platforms/pianoProbePkg',target)
    for path in target.rglob('*'):
        if path.is_file() and path.suffix in ('.c','.h','.inf','.dsc','.dec','.fdf','.py'):
            path.write_text(path.read_text().replace('pianoProbe','pianoProduct'))
    for suffix in ('dsc','dec','fdf'):(target/f'pianoProbe.{suffix}').rename(target/f'pianoProduct.{suffix}')
    shutil.rmtree(target/'Library/Stage0BootManagerLib')
    shutil.rmtree(target/'Library/RamLogSerialPortLib')
    shutil.copytree(root/'bootprofiles/product-support',target,dirs_exist_ok=True)
    app=target/'Applications/ProductCore';app.mkdir(parents=True)
    for name in SOURCE_NAMES:shutil.copyfile(source/name,app/name)
    for path in source.iterdir():
        if path.is_file() and path.suffix in ('.h','.inc'):shutil.copyfile(path,app/path.name)
    shutil.copytree(source/'Protocol',app/'Protocol')
    os_boot=prepare_os_boot(root,app)
    late_provider=stage_provider(root,app)
    late_provider['root_initialization_bound']=True
    late_provider['os_image_armed']=False
    observation=prepare_observation_families(root,app)
    (app/'PianoProductSimpleInitDigest.h').write_text(header)
    from prepare_ufs_write_test import verify_capture, _c_array
    storage_blobs=verify_capture()
    storage_baseline='// Pinned original GPT bytes; no provisioning or write authorization.\n#include <Uefi.h>\n'
    for symbol,name in (('mProductStorageOriginalPrimary','primary-header.bin'),
                        ('mProductStorageOriginalEntries','primary-entries.bin'),
                        ('mProductStorageOriginalBackup','backup-header.bin')):
        storage_baseline+=_c_array(symbol,storage_blobs[name])+'\n'
    (app/'PianoProductStorageBaseline.h').write_text(storage_baseline)
    (app/'ProductCore.inf').write_text(core_inf())
    verify_os_boot(root,app,os_boot)
    verify_observation_families(root,app,observation)
    verify_provider(root,app,late_provider)
    native_fdf,native_id=native_modules(root,app)
    memory=target/'Library/MemoryMapLib/MemoryMapLib.c';text=memory.read_text()
    text,low_memory_contract=fix_product_low_heap(text,(root/'private/captures/2026-10-03-piano/live.dtb').read_bytes())
    anchor='  {"CRYPTO0_CRYPTO",'
    if text.count(anchor)!=1:raise ValueError('Unexpected product MMIO map anchor')
    text=text.replace(anchor,'  {"UFS_HCI", 0x1D84000, 0x3000, AddDev, 1, 0x400, 11, NS_DEVICE},\n'
        '  {"Piano_USB2_PHY", 0x88E3000, 0x1000, AddDev, 1, 0x400, 11, NS_DEVICE},\n'
        '  {"Piano_USB3_PHY", 0x88E8000, 0x3000, AddDev, 1, 0x400, 11, NS_DEVICE},\n'+anchor)
    old='  {"Display_Demura", 0xA3500000, 0x2C80000, AddMem, 5, 0x703C07, 0, WRITE_THROUGH_XN},'
    if text.count(old)!=1:raise ValueError('Unexpected product ramoops map')
    text=text.replace(old,'  {"Piano_Ramoops", 0xA3500000, 0x400000, AddMem, 5, 0x703C07, 0, UNCACHED_UNBUFFERED_XN},\n'
        '  {"Display_Demura_Tail", 0xA3900000, 0x2880000, AddMem, 5, 0x703C07, 0, WRITE_THROUGH_XN},')
    text,display_candidate=display_mapping.prepare(root,text)
    display_contract={'candidate':display_candidate,'compiled_into_product':True,
                      'hardware_verified':False,'register_access_authorized':False}
    memory.write_text(text)
    verify_display_mapping(root,target,display_contract)
    dsc=target/'pianoProduct.dsc';text=dsc.read_text().replace('183A8587-C1F1-5FDD-8E2A-127FB6BD81A4','5E182CB1-63D4-44B5-AF6E-C23959625BA1')
    text=text.replace('pianoProductPkg/Library/Stage0BootManagerLib/Stage0BootManagerLib.inf','pianoProductPkg/Library/ProductBootManagerLib/ProductBootManagerLib.inf')
    text=text.replace('pianoProductPkg/Library/RamLogSerialPortLib/FrameBufferSerialPortLib.inf','pianoProductPkg/Library/RamOnlySerialPortLib/RamOnlySerialPortLib.inf')
    text=text.replace('# Stage 0: no persistent variables, storage, capsules or OS boot.','# Single product integration candidate; unfinished backends reported explicitly.')
    text+='''
[LibraryClasses.common.DXE_CORE, LibraryClasses.common.DXE_DRIVER, LibraryClasses.common.UEFI_DRIVER, LibraryClasses.common.UEFI_APPLICATION]
  PianoProductPumpLib|MdePkg/Library/PianoProductPumpLib/PianoProductPumpLib.inf
[LibraryClasses.common.DXE_CORE]
  PianoProductExitLib|MdePkg/Library/PianoProductExitLib/PianoProductExitLib.inf
[LibraryClasses]
  BootLogoLib|MdeModulePkg/Library/BootLogoLib/BootLogoLib.inf
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
  HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
  BcfgCommandLib|ShellPkg/Library/UefiShellBcfgCommandLib/UefiShellBcfgCommandLib.inf
[Components]
  MdeModulePkg/Logo/LogoDxe.inf
  MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf
  MdeModulePkg/Bus/Usb/UsbMouseDxe/UsbMouseDxe.inf
  pianoProductPkg/Drivers/PianoGopDxe/PianoGopDxe.inf
  pianoProductPkg/Applications/ProductCore/ProductCore.inf {
    <LibraryClasses>
      BaseCryptLib|OpensslPkg/Library/BaseCryptLib/BaseCryptLib.inf
      OpensslLib|OpensslPkg/Library/OpensslLib/OpensslLib.inf
      IntrinsicLib|CryptoPkg/Library/IntrinsicLib/IntrinsicLib.inf
      RngLib|MdePkg/Library/BaseRngLibNull/BaseRngLibNull.inf
  }
  ShellPkg/Application/Shell/Shell.inf {
    <PcdsFixedAtBuild>
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
      gEfiShellPkgTokenSpaceGuid.PcdShellSupportLevel|3
      gEfiShellPkgTokenSpaceGuid.PcdShellProfileMask|0x01
    <LibraryClasses>
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDriver1CommandsLib/UefiShellDriver1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellInstall1CommandsLib/UefiShellInstall1CommandsLib.inf
  }
'''
    text+=SETUP_DSC_ADDITIONS
    # All service flags bind actual product implementations; diagnostics are
    # not dispatched. FETCH enables the shared live SMMU storage guard.
    text+='\n[BuildOptions]\n  GCC:*_CLANGPDB_AARCH64_CC_FLAGS = '+ ' '.join('-D'+flag for flag in PRODUCT_FLAGS)+'\n'
    dsc.write_text(text)
    fdf=target/'pianoProduct.fdf';text=fdf.read_text().replace('# Host-only stage-0 diagnostic; NOT hardware validated.','# Single PianoUEFI product integration candidate; INCOMPLETE_NOT_RELEASE.')
    text=text.replace('SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf','pianoProductPkg/Drivers/PianoGopDxe/PianoGopDxe.inf')
    modules=DISK_MODULES+HOST_MODULES+SETUP_FV_MODULES+('MdeModulePkg/Logo/LogoDxe.inf','ShellPkg/Application/Shell/Shell.inf','pianoProductPkg/Applications/ProductCore/ProductCore.inf')
    text=text.replace('!include SiliciumPkg/Common.fdf.inc',''.join('  INF '+module+'\n' for module in modules)+native_fdf+'\n!include SiliciumPkg/Common.fdf.inc')
    fdf.write_text(text)
    early_memory=prepare_early_memory(root,target)
    verify_early_memory(root,target,early_memory)
    staged=root/'upstream/Mu-Silicium/Platforms/Xiaomi/pianoProductPkg'
    if staged.exists():shutil.rmtree(staged)
    shutil.copytree(target,staged)
    verify_early_memory(root,staged,early_memory)
    verify_display_mapping(root,staged,display_contract)
    verify_os_boot(root,staged/'Applications/ProductCore',os_boot)
    verify_observation_families(root,staged/'Applications/ProductCore',observation)
    verify_provider(root,staged/'Applications/ProductCore',late_provider)
    manifest={'target':'product','artifact':contract['artifact'],'status':'INCOMPLETE_NOT_RELEASE',
      'features':contract['features'],'shared_core':True,'entry_points':contract['entry_points'],'entry_policy_only':True,
      'fastboot_mode':'resident_background','fastboot_surfaces':contract['fastboot']['available_in'],
      'default_application':'SimpleInit','setup_key':'F12','diagnostic_reboot_timer':False,
      'backend_initialization_required':True,'backends':backend_status(),'runtime_readiness':'NOT_PRODUCT_DEVICE_VALIDATED',
      'service_compile_flags':list(PRODUCT_FLAGS),'sources':list((*SOURCE_NAMES,*OS_BOOT_INF_SOURCES,*OBSERVATION_INF_SOURCES)),'native_foundation':native_id,'os_boot':os_boot,'dxe_observation':observation,'early_memory':early_memory,
      'simpleinit':simpleinit,'simpleinit_payload':app_identity,'ui_hooks':ui,'pump_hooks':prepare_pump(root,apply=False),
      'low_memory_contract':low_memory_contract,
      'display_mapping':display_contract,
      'native_late_handoff':handoff_hooks,'late_provider':late_provider,
      'platform_files':{str(path.relative_to(target)):sha(path)for path in sorted(target.rglob('*'))if path.is_file()},
      'device_boot_performed':False,'permanent_storage_writes':False}
    out=root/'build/product';out.mkdir(parents=True,exist_ok=True)
    (out/'prepared-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.parse_args()
    result=prepare();print(json.dumps({'status':result['status'],'artifact':result['artifact'],'sources':len(result['sources']),'diagnostic_reboot_timer':False},indent=2))


if __name__=='__main__':main()
