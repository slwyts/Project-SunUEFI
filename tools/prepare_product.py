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

CORE_GUID='35E0D1B5-93CE-4D6A-9A93-6ADAA3F26C40'
SOURCE_NAMES=(
    'PianoProductCore.c','PianoBootPolicy.c','PianoFvApplication.c','PianoProductPayload.c','PianoProductOwners.c',
    'NativeProbe.c','PianoKeys.c','PianoFaultRecovery.c',
    'PianoSmmu.c','PianoDma.c','PianoOwnedSmmu.c','PianoIoPageTable.c',
    'PianoUfsProbe.c','PianoUfsReadOnlyDma.c','PianoUfsDmaLayout.c','PianoGpt.c','PianoReadOnlyBlock.c',
    'PianoFastboot.c','PianoFastbootBlockRead.c','PianoFastbootBoot.c','PianoFastbootLaunch.c','PianoFastbootDownloadBlob.c',
    'PianoFastbootScreen.c','PianoDwc3Device.c','PianoUsbControl.c','PianoUsbController.c',
    'PianoPogoReport.c','PianoPogoInput.c','PianoPogoI2c.c','PianoPogoTransport.c','PianoGeniI2cPio.c','PianoUsbHostPci.c',
)
PRODUCT_FLAGS=('PIANO_USB_SERVICE=1','PIANO_USB_EP0=1','PIANO_USB_FASTBOOT=1','PIANO_USB_SCREENSHOT=1',
    'PIANO_USB_UFS_FETCH=1','PIANO_USB_RAM_BOOT=1','PIANO_USB_POWER_PROBE=1','PIANO_UFS_BLOCKIO=1')
NATIVE_NAMES=('SmemDxe','DALSys','ChipInfo','PlatformInfoDxeDriver','HWIODxeDriver','ULogDxe',
    'CmdDbDxe','PwrUtilsDxe','RpmhDxe','NpaDxe','VcsDxe','ClockDxe','HALIOMMU')
DISK_MODULES=('MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf',
    'MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf',
    'MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf','FatPkg/EnhancedFatDxe/Fat.inf')
HOST_MODULES=('MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf','MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf',
    'MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf','MdeModulePkg/Bus/Usb/UsbMouseDxe/UsbMouseDxe.inf',
    'MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf')


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def backend_status():
    # Integration/validation state is separate from the required enabled set.
    return {
      'gop':{'status':'IMPLEMENTED_INHERITED_FRAMEBUFFER','physical_evidence':'test84 real GOP screenshot; product untested'},
      'physical_keys':{'status':'IMPLEMENTED_PMIC_READONLY','physical_evidence':'tests23/24; product input retirement untested'},
      'pogo_keyboard_touchpad':{'status':'NOT_READY','missing':'verified SE6 firmware/clock ownership and live report transport'},
      'touchscreen':{'status':'NOT_READY','missing':'verified GPI/PAS/DMA physical touch reports'},
      'dma_smmu':{'status':'IMPLEMENTED_STRICT_OWNERS','physical_evidence':'test91 readonly fetch and exact combined USB/UFS retirement passed; resident product retirement still untested'},
      'ufs_blockio_read_write':{'status':'READ_ONLY_BACKEND','missing':'normal writable provider and permanent explicit test/storage reservation; bounded RW test86 is not product RW'},
      'gpt':{'status':'IMPLEMENTED_READ','physical_evidence':'real UFS GPT reads; product untested'},
      'fat_simplefilesystem':{'status':'IMPLEMENTED_READ_ONLY_VOLUMES','physical_evidence':'7 read-only SFS, bounded FAT RW test86; product untested'},
      'persistent_variables':{'status':'RAM_ONLY','missing':'durable NV variable backend; PcdEmuVariableNvModeEnable remains TRUE'},
      'uefi_shell':{'status':'LINKED_STANDARD_SHELL','missing':'product cooperative exit and UFS file operations acceptance'},
      'setup_hii':{'status':'LINKED_STANDARD_UIAPP','missing':'product navigation/F12/cooperative exit acceptance'},
      'simpleinit':{'status':'LINKED_PRODUCT_GUI','missing':'product APPv1 load, visible Setup/Shell navigation and background service acceptance'},
      'usb_device_fastboot':{'status':'IMPLEMENTED_RESIDENT_SERVICE_UNTESTED','physical_evidence':'isolated standard bulk82/reboot83/screen84/fetch91; resident product untested','navigation_commands':['oem setup','oem shell','oem simpleinit'],'navigation_physical_validation':False,'current_download_limit_bytes':67108864,'target_download_limit_bytes':1073741824},
      'usb_host':{'status':'NOT_READY','missing':'actual Host PCI_IO/NC common DMA and Type-C/VBUS ownership backend; standard consumers linked'},
      'debug_logs_screenshot':{'status':'IMPLEMENTED_FASTBOOT','physical_evidence':'test82 ramlog and84 screenshot; product UI snapshots untested'},
      'efi_android_linux_boot':{'status':'PARTIAL','missing':'generic product EFI/img boot, autonomous UFS OS load and Android/Recovery/Windows handoff'},
      'os_exit':{'status':'STRICT_RETIREMENT_INTEGRATED_UNTESTED','missing':'joint controller retirement and full EFI DRAM/OS handoff contract'},
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
'''+''.join('  '+name+'\n' for name in SOURCE_NAMES)+'''[Packages]
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
[Guids]
  gEfiEventExitBootServicesGuid
  gEfiFileInfoGuid
  gEfiFileSystemInfoGuid
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
    (app/'PianoProductSimpleInitDigest.h').write_text(header)
    (app/'ProductCore.inf').write_text(core_inf())
    native_fdf,native_id=native_modules(root,app)
    memory=target/'Library/MemoryMapLib/MemoryMapLib.c';text=memory.read_text()
    anchor='  {"CRYPTO0_CRYPTO",'
    if text.count(anchor)!=1:raise ValueError('Unexpected product MMIO map anchor')
    text=text.replace(anchor,'  {"UFS_HCI", 0x1D84000, 0x3000, AddDev, 1, 0x400, 11, NS_DEVICE},\n'
        '  {"Piano_USB2_PHY", 0x88E3000, 0x1000, AddDev, 1, 0x400, 11, NS_DEVICE},\n'
        '  {"Piano_USB3_PHY", 0x88E8000, 0x3000, AddDev, 1, 0x400, 11, NS_DEVICE},\n'+anchor)
    old='  {"Display_Demura", 0xA3500000, 0x2C80000, AddMem, 5, 0x703C07, 0, WRITE_THROUGH_XN},'
    if text.count(old)!=1:raise ValueError('Unexpected product ramoops map')
    text=text.replace(old,'  {"Piano_Ramoops", 0xA3500000, 0x400000, AddMem, 5, 0x703C07, 0, UNCACHED_UNBUFFERED_XN},\n'
        '  {"Display_Demura_Tail", 0xA3900000, 0x2880000, AddMem, 5, 0x703C07, 0, WRITE_THROUGH_XN},')
    memory.write_text(text)
    dsc=target/'pianoProduct.dsc';text=dsc.read_text().replace('183A8587-C1F1-5FDD-8E2A-127FB6BD81A4','5E182CB1-63D4-44B5-AF6E-C23959625BA1')
    text=text.replace('pianoProductPkg/Library/Stage0BootManagerLib/Stage0BootManagerLib.inf','pianoProductPkg/Library/ProductBootManagerLib/ProductBootManagerLib.inf')
    text=text.replace('pianoProductPkg/Library/RamLogSerialPortLib/FrameBufferSerialPortLib.inf','pianoProductPkg/Library/RamOnlySerialPortLib/RamOnlySerialPortLib.inf')
    text=text.replace('# Stage 0: no persistent variables, storage, capsules or OS boot.','# Single product integration candidate; unfinished backends reported explicitly.')
    text+='''
[LibraryClasses.common.DXE_CORE, LibraryClasses.common.DXE_DRIVER, LibraryClasses.common.UEFI_DRIVER, LibraryClasses.common.UEFI_APPLICATION]
  PianoProductPumpLib|MdePkg/Library/PianoProductPumpLib/PianoProductPumpLib.inf
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
    staged=root/'upstream/Mu-Silicium/Platforms/Xiaomi/pianoProductPkg'
    if staged.exists():shutil.rmtree(staged)
    shutil.copytree(target,staged)
    manifest={'target':'product','artifact':contract['artifact'],'status':'INCOMPLETE_NOT_RELEASE',
      'features':contract['features'],'shared_core':True,'entry_points':contract['entry_points'],'entry_policy_only':True,
      'fastboot_mode':'resident_background','fastboot_surfaces':contract['fastboot']['available_in'],
      'default_application':'SimpleInit','setup_key':'F12','diagnostic_reboot_timer':False,
      'backend_initialization_required':True,'backends':backend_status(),'runtime_readiness':'NOT_PRODUCT_DEVICE_VALIDATED',
      'service_compile_flags':list(PRODUCT_FLAGS),'sources':list(SOURCE_NAMES),'native_foundation':native_id,
      'simpleinit':simpleinit,'simpleinit_payload':app_identity,'ui_hooks':ui,'pump_hooks':prepare_pump(root,apply=False),
      'platform_files':{str(path.relative_to(target)):sha(path)for path in sorted(target.rglob('*'))if path.is_file()},
      'device_boot_performed':False,'permanent_storage_writes':False}
    out=root/'build/product';out.mkdir(parents=True,exist_ok=True)
    (out/'prepared-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.parse_args()
    result=prepare();print(json.dumps({'status':result['status'],'artifact':result['artifact'],'sources':len(result['sources']),'diagnostic_reboot_timer':False},indent=2))


if __name__=='__main__':main()
