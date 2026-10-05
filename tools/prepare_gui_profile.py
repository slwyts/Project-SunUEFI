#!/usr/bin/env python3
"""Create a separate GOP/simple-init RAM target, without contacting a device."""
from pathlib import Path
import shutil
import argparse

# Fixed Mu_Basecore standard browser/display engine; no OEM/MsDisplayEngine UI.
# SetupBrowserDxe already has a Component entry in SiliciumPkg.dsc.inc.
SETUP_FV_MODULES = (
    'MdeModulePkg/Universal/SetupBrowserDxe/SetupBrowserDxe.inf',
    'MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf',
    'MdeModulePkg/Application/UiApp/UiApp.inf',
)
SETUP_DSC_ADDITIONS = '''
[PcdsFixedAtBuild]
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|TRUE
[PcdsDynamicDefault]
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoHorizontalResolution|3200
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoVerticalResolution|2136
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutColumn|80
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutRow|25
[LibraryClasses]
  CustomizedDisplayLib|MdeModulePkg/Library/CustomizedDisplayLib/CustomizedDisplayLib.inf
  FileExplorerLib|MdeModulePkg/Library/FileExplorerLib/FileExplorerLib.inf
[Components]
  MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf
  MdeModulePkg/Application/UiApp/UiApp.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/DeviceManagerUiLib/DeviceManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootManagerUiLib/BootManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootMaintenanceManagerUiLib/BootMaintenanceManagerUiLib.inf
  }
'''


def argument_parser():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--foundation',action='store_true',help='Gate the prepared native support modules by original DEPEX after console timer starts')
    parser.add_argument('--keys',action='store_true',help='Use the standalone read-only piano key transport; excludes native PMIC bring-up')
    parser.add_argument('--touch-probe',action='store_true',help='SPI identity probe with the explicitly staged spi support group')
    parser.add_argument('--gpi-probe',action='store_true',help='Initialize and inspect GPI with clocks held; never register channels or issue DMA')
    parser.add_argument('--ram-qupfw',action='store_true',help='Expose only the hash-verified active-slot QUP firmware through an in-memory read interface')
    parser.add_argument('--qupfw-disk',action='store_true',help='Expose immutable QUP firmware RAM partition via standard BlockIO')
    parser.add_argument('--usb-debug',action='store_true',help='Native USB protocol inventory plus RAM-only fastboot transport; requires usb group')
    parser.add_argument('--fault-recovery',action='store_true',help='Log synchronous/SError exceptions to reserved RAM then cold reboot')
    parser.add_argument('--fault-recovery-test',action='store_true',help='Trigger one undefined instruction after installing recovery; no hardware MMIO probe')
    parser.add_argument('--pmic-metadata',action='store_true',help='Independent read-only version provider before USB config; requires usb-debug and fault-recovery')
    parser.add_argument('--ufs-probe',action='store_true',help='Clock/GDSC and HCI register probe only; no DMA/LUN/disk access')
    parser.add_argument('--dma-probe',action='store_true',help='Read-only SMMU snapshot before/after foundation plus DMA memory diagnostics')
    parser.add_argument('--dma-owned',action='store_true',help='UFS-only owned SMMU tables and map/unmap experiment, no controller DMA submission')
    parser.add_argument('--ufs-dma-nop',action='store_true',help='Read-only NOP then QUERY descriptor using unified DMA; requires verified owned SMMU')
    parser.add_argument('--ufs-blockio',action='store_true',help='Publish persistent read-only UFS BlockIO using the verified DMA transport')
    parser.add_argument('--ufs-filesystems',action='store_true',help='Connect standard FAT/EnglishDxe and inspect real UFS SimpleFileSystem volumes without writing')
    parser.add_argument('--ufs-shell',action='store_true',help='Run firmware-volume UEFI Shell read-only enumeration before RAM simple-init; implies ufs-filesystems')
    parser.add_argument('--ufs-shell-interactive',action='store_true',help='Remain in UEFI Shell after automatic enumeration until exit or the recovery timer; implies ufs-shell')
    parser.add_argument('--ufs-setup',action='store_true',help='Launch the pinned standard TianoCore UiApp/HII Setup with temporary RAM settings; implies ufs-filesystems')
    writes=parser.add_mutually_exclusive_group()
    writes.add_argument('--ufs-write-preflight',action='store_true',help='Isolated fixed LUN4/LBA375040 baseline/live gate and full gap reads only; never WRITE/SYNC or boot SimpleInit')
    writes.add_argument('--ufs-write-restore-test',action='store_true',help='Explicit isolated fixed one-block FUA write/sync/read/restore/verify transaction; all registered BlockIO remains readonly and no SimpleInit boot')
    writes.add_argument('--ufs-bounded-filesystem-test',action='store_true',help='Isolated gap-only FAT12/SFS format/file test then mandatory whole-gap restore; no original volumes or OS boot')
    parser.add_argument('--usb-controller',action='store_true',help='Isolated DWC3 clocks/registers and owned USB0 SMMU context')
    parser.add_argument('--usb-ep0',action='store_true',help='USB2 device EP0 enumeration using shared DMA and USB0 owned context')
    parser.add_argument('--usb-fastboot',action='store_true',help='Isolated standard USB fastboot bulk with RAM-only stage/upload and diagnostics; implies usb-ep0')
    parser.add_argument('--usb-screenshot',action='store_true',help='Enable actual GOP BMP capture over the isolated USB fastboot profile')
    parser.add_argument('--usb-ufs-fetch',action='store_true',help='Explicit read-only UFS/USB coexistence profile with standard partition fetch')
    parser.add_argument('--usb-ram-boot',action='store_true',help='Isolated 64MiB fastboot boot diagnostic; accepts only the pinned returning AA64 probe')
    parser.add_argument('--pogo-register-probe',action='store_true',help='Isolated protected readonly SE6/wrapper snapshot; no I2C transaction or firmware load')
    parser.add_argument('--high-ram-readonly',action='store_true',help='Isolated high address AT/EFI/GCD evidence; no table walk, mapping or data access')
    parser.add_argument('--return-seconds',type=int,default=75,help='Diagnostic cold-reboot timer, 30 to 120 seconds (default 75)')
    return parser


def validate_write_options(parser,args):
    if args.ufs_write_preflight or args.ufs_write_restore_test or args.ufs_bounded_filesystem_test:
        if any((args.ufs_filesystems,args.ufs_shell,args.ufs_shell_interactive,args.ufs_setup,
                args.usb_controller,args.usb_ep0,args.usb_fastboot,args.usb_screenshot,args.usb_ufs_fetch,args.usb_ram_boot,args.pogo_register_probe,args.high_ram_readonly,args.usb_debug,args.touch_probe,args.gpi_probe,
                args.fault_recovery_test,args.ram_qupfw,args.qupfw_disk,args.pmic_metadata)):
            parser.error('UFS write/preflight requires an isolated profile without filesystem/Shell/Setup/USB/touch consumers')
        args.ufs_blockio=True


def write_test_ram_app(text):
    text=text.replace('UINT8 Hash[32]; EFI_HANDLE App; EFI_STATUS Status;',
                      'UINT8 Hash[32]; EFI_STATUS Status;')
    """Keep the app/ledger resident; the explicit diagnostic cannot boot an OS."""
    anchor='  Status=gBS->LoadImage (FALSE,ImageHandle'
    if text.count(anchor)!=1:
        raise ValueError('Unexpected RamApp SimpleInit launch anchor')
    return text[:text.index(anchor)]+'''  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WRITE_PROFILE_WAIT boot_blocked=1 readonly_blockio=1\\n"));
  // RunWriteTransaction holds the recovery timer until restore is verified;
  // an unsafe result never returns here. A safe result waits for final Halt.
  while(TRUE){gBS->Stall(100000);}
}
'''


def load_write_attestation(root):
    import importlib.util
    spec=importlib.util.spec_from_file_location('piano_write_attestation',root/'tools/prepare_ufs_write_test.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module


def load_bounded_format(root):
    import importlib.util
    spec=importlib.util.spec_from_file_location('piano_bounded_format',root/'tools/prepare_ufs_bounded_fs_test.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module


def bounded_fs_ram_app(text):
    text=write_test_ram_app(text)
    text=text.replace('#pragma pack(1)','EFI_STATUS PianoUfsRunBoundedFileSystemTest(VOID);\n#pragma pack(1)',1)
    anchor='  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WRITE_PROFILE_WAIT'
    if text.count(anchor)!=1:raise ValueError('Unexpected bounded wait anchor')
    return text.replace(anchor,'  Status=PianoUfsRunBoundedFileSystemTest();\n  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_FS_SESSION_RETURN %r\\n",Status));\n'+anchor)


def validate_usb_fastboot_options(parser,args):
    if args.usb_ram_boot:
        if args.usb_ufs_fetch or args.usb_screenshot or args.return_seconds!=120:
            parser.error('--usb-ram-boot is isolated and requires --return-seconds 120')
        args.usb_fastboot=True
    if args.usb_ufs_fetch:
        if any((args.ufs_filesystems,args.ufs_shell,args.ufs_shell_interactive,args.ufs_setup,
                args.ufs_write_preflight,args.ufs_write_restore_test,args.ufs_bounded_filesystem_test,
                args.usb_screenshot,args.usb_debug,args.touch_probe,args.gpi_probe,args.ram_qupfw,
                args.qupfw_disk,args.pmic_metadata,args.fault_recovery_test)):
            parser.error('--usb-ufs-fetch requires readonly UFS and USB only')
        args.usb_fastboot=True;args.usb_ep0=True;args.ufs_blockio=True
        return
    if not args.usb_fastboot:
        return
    if any((args.ufs_probe,args.dma_probe,args.dma_owned,args.ufs_dma_nop,args.ufs_blockio,
            args.ufs_filesystems,args.ufs_shell,args.ufs_shell_interactive,args.ufs_setup,
            args.ufs_write_preflight,args.ufs_write_restore_test,args.ufs_bounded_filesystem_test,args.usb_debug,
            args.touch_probe,args.gpi_probe,args.ram_qupfw,args.qupfw_disk,
            args.pmic_metadata,args.fault_recovery_test)):
        parser.error('--usb-fastboot requires an isolated profile without UFS/native USB/touch consumers')
    args.usb_ep0=True


def validate_readonly_diagnostic_options(parser,args):
    if not (args.pogo_register_probe or args.high_ram_readonly):return
    if args.pogo_register_probe and args.high_ram_readonly:
        parser.error('Pogo and high RAM diagnostics must be separate')
    forbidden=('keys','touch_probe','gpi_probe','ram_qupfw','qupfw_disk','usb_debug',
        'fault_recovery','fault_recovery_test','pmic_metadata','ufs_probe','dma_probe','dma_owned',
        'ufs_dma_nop','ufs_blockio','ufs_filesystems','ufs_shell','ufs_shell_interactive','ufs_setup',
        'ufs_write_preflight','ufs_write_restore_test','ufs_bounded_filesystem_test',
        'usb_controller','usb_ep0','usb_fastboot','usb_screenshot','usb_ufs_fetch','usb_ram_boot')
    if any(getattr(args,name) for name in forbidden):parser.error('Readonly register/memory diagnostic requires an isolated profile')
    if args.high_ram_readonly and args.foundation:parser.error('High RAM evidence excludes native foundation')


def main():
    parser=argument_parser()
    args=parser.parse_args()
    validate_readonly_diagnostic_options(parser,args)
    if args.usb_screenshot:args.usb_fastboot=True
    validate_write_options(parser,args)
    validate_usb_fastboot_options(parser,args)
    if args.pogo_register_probe:args.foundation=True
    if args.high_ram_readonly:args.fault_recovery=True
    if args.ufs_shell_interactive:args.ufs_shell=True
    if args.ufs_shell:args.ufs_filesystems=True
    if args.ufs_setup:args.ufs_filesystems=True
    if args.ufs_filesystems:args.ufs_blockio=True
    if args.usb_ep0:args.usb_controller=True
    if args.usb_controller:
        if not args.usb_ufs_fetch and (args.ufs_blockio or args.ufs_probe or args.dma_probe or args.usb_debug or args.touch_probe):parser.error('USB controller experiment must be isolated')
        args.foundation=True;args.keys=True;args.fault_recovery=True
    if args.ufs_blockio:args.ufs_dma_nop=True
    if args.ufs_dma_nop:args.dma_owned=True
    if args.dma_owned:args.dma_probe=True
    if args.dma_probe:args.ufs_probe=True
    if args.fault_recovery_test:
        args.fault_recovery=True
    if args.pmic_metadata and (not args.usb_debug or not args.fault_recovery or args.fault_recovery_test):
        parser.error('--pmic-metadata requires --usb-debug --fault-recovery without the synthetic fault test')
    if args.ufs_probe:
        if args.usb_debug or args.touch_probe or args.gpi_probe or args.fault_recovery_test:
            parser.error('UFS controller probe requires an isolated hardware profile')
        args.foundation=True;args.keys=True;args.fault_recovery=True
        import json
        selection=Path(__file__).resolve().parent.parent/'build/native-probe-selection.json'
        if json.loads(selection.read_text())['group']!='ufs':
            parser.error('--ufs-probe requires prepare_native_probe.py --group ufs')
    if not 30<=args.return_seconds<=120:
        parser.error('--return-seconds must be between 30 and 120')
    if args.gpi_probe:
        args.touch_probe=True
    if args.qupfw_disk:
        args.ram_qupfw=True
    if args.ram_qupfw and not args.gpi_probe:
        parser.error('--ram-qupfw requires --gpi-probe')
    if args.usb_debug:
        if args.touch_probe or args.gpi_probe:
            parser.error('USB and touch hardware isolation profiles must be tested separately')
        args.foundation=True;args.keys=True
        import json
        selection=Path(__file__).resolve().parent.parent/'build/native-probe-selection.json'
        if json.loads(selection.read_text())['group']!='usb':
            parser.error('--usb-debug requires prepare_native_probe.py --group usb')
    if args.touch_probe:
        args.keys=True;args.foundation=True
        selection=Path(__file__).resolve().parent.parent/'build/native-probe-selection.json'
        import json
        expected='gpi' if args.gpi_probe else 'spi'
        if json.loads(selection.read_text())['group']!=expected:
            parser.error('Probe requires prepare_native_probe.py --group '+expected)
    if args.keys and args.foundation and not args.touch_probe and not args.usb_debug and not args.ufs_probe and not args.usb_controller:
        parser.error('--keys and --foundation must be tested separately')
    root = Path(__file__).resolve().parent.parent
    write_attestation=None
    bounded_format=None
    if args.ufs_write_preflight or args.ufs_write_restore_test or args.ufs_bounded_filesystem_test:
        write_attestation=load_write_attestation(root)
        # Refuse an absent/drifted archive before mutating profile staging.
        write_attestation.verify_capture()
    if args.ufs_bounded_filesystem_test:
        bounded_format=load_bounded_format(root)
        bounded_format.verify()
    source = root / 'platforms/pianoProbePkg'
    target = root / 'platforms/pianoGuiPkg'
    shutil.copytree(source, target, dirs_exist_ok=True)
    for path in target.rglob('*'):
        if path.is_file() and path.suffix in ('.c','.h','.inf','.dsc','.dec','.fdf','.py'):
            path.write_text(path.read_text().replace('pianoProbe','pianoGui'))
    for suffix in ('dsc','dec','fdf'):
        (target / f'pianoProbe.{suffix}').rename(target / f'pianoGui.{suffix}')
    app = target / 'Applications/RamApp'
    app.mkdir(parents=True, exist_ok=True)
    for name in ('RamApp.c','RamApp.inf'):
        shutil.copyfile(root / 'bootprofiles/uefi-app' / name, app / name)
    if args.foundation:
        for name in ('NativeProbe.c','NativeProbeTable.h'):
            shutil.copyfile(root / 'bootprofiles/uefi-app' / name,app / name)
        path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c','  RamApp.c\n  NativeProbe.c').replace('  DebugLib','  DebugLib\n  DxeServicesLib');path.write_text(text)
        path=app/'RamApp.c';text=path.read_text().replace('#pragma pack(1)','VOID PianoProbeFoundation (VOID);\n#pragma pack(1)',1).replace('  ProbeGop ();','  PianoProbeFoundation ();\n  ProbeGop ();');path.write_text(text)
    if args.keys:
        shutil.copyfile(root/'bootprofiles/uefi-app/PianoKeys.c',app/'PianoKeys.c')
        shutil.copyfile(root/'bootprofiles/uefi-app/PianoKeysLifecycle.h',app/'PianoKeysLifecycle.h')
        path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoKeys.c').replace('  DebugLib','  DebugLib\n  IoLib')
        text+='  gEfiSimpleTextInProtocolGuid\n  gEfiDevicePathProtocolGuid\n';path.write_text(text)
        path=app/'RamApp.c';text=path.read_text().replace('#pragma pack(1)',
            'EFI_STATUS PianoStartKeys (CONST VOID *Fdt);\nVOID PianoStopKeys (VOID);\n#pragma pack(1)',1)
        text=text.replace('  Status=gBS->LoadImage (FALSE,ImageHandle',
            '  Status=PianoStartKeys(Fdt);\n  DEBUG((DEBUG_WARN,"SUNUEFI_KEYS_START %r\\n",Status));\n  Status=gBS->LoadImage (FALSE,ImageHandle')
        text=text.replace('  if (EFI_ERROR (Status)) { return Status; }\n  Status=gBS->StartImage (App',
            '  if (EFI_ERROR (Status)) { PianoStopKeys(); return Status; }\n  Status=gBS->StartImage (App')
        text=text.replace('  gBS->UnloadImage (App); return Status;',
            '  gBS->UnloadImage (App); PianoStopKeys(); return Status;')
        path.write_text(text)
    if args.touch_probe:
        shutil.copyfile(root/'bootprofiles/uefi-app/PianoTouchProbe.c',app/'PianoTouchProbe.c')
        if args.gpi_probe:
            path=app/'PianoTouchProbe.c';path.write_text('#define PIANO_GPI_PROBE 1\n'+path.read_text())
        if args.ram_qupfw:
            shutil.copyfile(root/'bootprofiles/uefi-app/PianoQupFwRam.c',app/'PianoQupFwRam.c')
            path=app/'PianoTouchProbe.c';text=path.read_text().replace('STATIC VOID ProbeGpiLibrary(VOID) {',
                'EFI_STATUS PianoInstallQupFwRam(VOID);\nSTATIC VOID ProbeGpiLibrary(VOID) {\n  EFI_STATUS Firmware=PianoInstallQupFwRam();\n  if(EFI_ERROR(Firmware)) {DEBUG((DEBUG_WARN,"SUNUEFI_QUPFW_PROVIDER_ERROR %r\\n",Firmware));return;}')
            path.write_text(text)
            if args.qupfw_disk:
                shutil.copyfile(root/'bootprofiles/uefi-app/PianoQupFwDisk.c',app/'PianoQupFwDisk.c')
                path.write_text(path.read_text().replace('PianoInstallQupFwRam','PianoInstallQupFwDisk'))
        path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoTouchProbe.c').replace('  DebugLib','  DebugLib\n  CacheMaintenanceLib\n  ArmSmcLib')
        text=text.replace('  MdePkg/MdePkg.dec','  MdePkg/MdePkg.dec\n  QcomPkg/QcomPkg.dec')
        text+='  gEfiLoadedImageProtocolGuid\n';path.write_text(text)
        if args.ram_qupfw:
            text=path.read_text().replace('  PianoTouchProbe.c','  PianoTouchProbe.c\n  PianoQupFwRam.c');path.write_text(text)
            if args.qupfw_disk:
                path.write_text(path.read_text().replace('  PianoQupFwRam.c','  PianoQupFwDisk.c'))
        path=app/'RamApp.c';text=path.read_text().replace('#pragma pack(1)',
            'EFI_STATUS PianoPrepareTouch (VOID);\nVOID PianoProbeTouch (VOID);\n#pragma pack(1)',1)
        text=text.replace('  PianoProbeFoundation ();',
            '  EFI_STATUS TouchConfig=PianoPrepareTouch();\n  DEBUG((DEBUG_WARN,"SUNUEFI_TOUCH_CONFIG %r\\n",TouchConfig));\n  if(!EFI_ERROR(TouchConfig))PianoProbeFoundation();')
        text=text.replace('  Status=PianoStartKeys(Fdt);',
            '  PianoProbeTouch();\n  Status=PianoStartKeys(Fdt);')
        path.write_text(text)
    if args.usb_debug:
        path=app/'PianoKeys.c';path.write_text('#define PIANO_USB_POWER_PROBE 1\n'+path.read_text())
        for name in ('PianoFastboot.h','PianoFastboot.c','PianoUsbDebug.c'):
            shutil.copyfile(root/'bootprofiles/uefi-app'/name,app/name)
        path=app/'RamApp.inf'
        text=path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoFastboot.c\n  PianoUsbDebug.c')
        text=text.replace('  MdePkg/MdePkg.dec','  MdePkg/MdePkg.dec\n  QcomPkg/QcomPkg.dec')
        text=text.replace('  UefiLib','  UefiLib\n  UefiRuntimeServicesTableLib')
        text+='  gEfiLoadedImageProtocolGuid\n'
        path.write_text(text)
        path=app/'RamApp.c';text=path.read_text().replace('#pragma pack(1)',
            '#include "PianoFastboot.h"\nVOID PianoProbeUsbPower(CONST VOID *Fdt);\n#pragma pack(1)',1)
        text=text.replace('  Status=PianoStartKeys(Fdt);',
            '  PianoProbeUsbPower(Fdt);\n  Status=PianoStartUsbDebug();\n  DEBUG((DEBUG_WARN,"SUNUEFI_USB_DEBUG_START %r\\n",Status));\n  Status=PianoStartKeys(Fdt);')
        text=text.replace('PianoStopKeys();','PianoStopKeys(); PianoStopUsbDebug();')
        path.write_text(text)
    if args.ufs_probe:
        mapping=target/'Library/MemoryMapLib/MemoryMapLib.c'
        text=mapping.read_text();anchor='  {"CRYPTO0_CRYPTO",'
        if text.count(anchor)!=1:raise SystemExit('Unexpected piano MMIO map')
        text=text.replace(anchor,'  {"UFS_HCI", 0x1D84000, 0x3000, AddDev, 1, 0x400, 11, NS_DEVICE},\n'+anchor)
        # Linux owns the first 4 MiB of this inherited XBL reservation as
        # ramoops. Isolate its CPU cache attribute without changing the total
        # reserved interval or any DMA heap / framebuffer / SMMU mapping.
        old='  {"Display_Demura", 0xA3500000, 0x2C80000, AddMem, 5, 0x703C07, 0, WRITE_THROUGH_XN},'
        if text.count(old)!=1:raise SystemExit('Unexpected ramoops parent reservation')
        text=text.replace(old,'  {"Piano_Ramoops", 0xA3500000, 0x400000, AddMem, 5, 0x703C07, 0, UNCACHED_UNBUFFERED_XN},\n'
                             '  {"Display_Demura_Tail", 0xA3900000, 0x2880000, AddMem, 5, 0x703C07, 0, WRITE_THROUGH_XN},')
        mapping.write_text(text)
        shutil.copyfile(root/'bootprofiles/uefi-app/PianoUfsProbe.c',app/'PianoUfsProbe.c')
        path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoUfsProbe.c')
        text=text.replace('  MdePkg/MdePkg.dec','  MdePkg/MdePkg.dec\n  QcomPkg/QcomPkg.dec');path.write_text(text)
        path=app/'RamApp.c';text=path.read_text().replace('#pragma pack(1)',
            'VOID PianoProbeUfs(CONST VOID *Fdt);\n#pragma pack(1)',1)
        text=text.replace('  Status=PianoStartKeys(Fdt);','  PianoProbeUfs(Fdt);\n  Status=PianoStartKeys(Fdt);');path.write_text(text)
    if args.fault_recovery:
        shutil.copyfile(root/'bootprofiles/uefi-app/PianoFaultRecovery.c',app/'PianoFaultRecovery.c')
        path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoFaultRecovery.c')
        if '  ArmSmcLib\n' not in text:text=text.replace('  DebugLib','  DebugLib\n  ArmSmcLib')
        if '  UefiRuntimeServicesTableLib\n' not in text:text=text.replace('  UefiLib','  UefiLib\n  UefiRuntimeServicesTableLib')
        text=text.replace('  DebugLib','  DebugLib\n  SerialPortLib\n  PrintLib')
        text+='  gEfiCpuArchProtocolGuid\n';path.write_text(text)
        path=app/'RamApp.c';text=path.read_text().replace('#pragma pack(1)',
            'EFI_STATUS PianoStartFaultRecovery(VOID);\nVOID PianoStopFaultRecovery(VOID);\nEFI_STATUS PianoTestFaultRecovery(VOID);\n#pragma pack(1)',1)
        anchor='  DEBUG ((DEBUG_WARN, "SUNUEFI_RAM_APP_LOADER\\n"));'
        setup='\n  Status=PianoStartFaultRecovery();\n  DEBUG((DEBUG_WARN,"SUNUEFI_FAULT_RECOVERY_START %r\\n",Status));'
        if args.pmic_metadata or args.ufs_probe:setup+='\n  if(EFI_ERROR(Status)){return Status;}'
        if args.fault_recovery_test:setup+='\n  if(!EFI_ERROR(Status))return PianoTestFaultRecovery();'
        text=text.replace(anchor,anchor+setup)
        # App code is unloaded after return; remove our exception callbacks.
        text=text.replace('return Status;','PianoStopFaultRecovery(); return Status;')
        text=text.replace('return EFI_NOT_FOUND;','PianoStopFaultRecovery(); return EFI_NOT_FOUND;')
        text=text.replace('return EFI_COMPROMISED_DATA;','PianoStopFaultRecovery(); return EFI_COMPROMISED_DATA;')
        text=text.replace('return EFI_BAD_BUFFER_SIZE;','PianoStopFaultRecovery(); return EFI_BAD_BUFFER_SIZE;')
        text=text.replace('return EFI_SECURITY_VIOLATION;','PianoStopFaultRecovery(); return EFI_SECURITY_VIOLATION;')
        if args.fault_recovery_test:
            text=text.replace('if(!EFI_ERROR(Status))return PianoTestFaultRecovery();',
                'if(!EFI_ERROR(Status)){Status=PianoTestFaultRecovery();PianoStopFaultRecovery();return Status;}')
        path.write_text(text)
    if args.pmic_metadata:
        driver=target/'Drivers/PianoPmicMetadata';driver.mkdir(parents=True,exist_ok=True)
        for name in ('PianoPmicMetadata.c','PianoPmicMetadata.inf'):
            shutil.copyfile(root/'bootprofiles/uefi-app'/name,driver/name)
        path=app/'RamApp.c';text=path.read_text()
        helper='''STATIC EFI_STATUS LoadPmicMetadata(VOID) {
  EFI_GUID Guid={0x7BA3F20C,0x2A18,0x4F68,{0x85,0x40,0x0E,0x12,0x4B,0x6A,0x51,0xBD}};
  VOID *Source=NULL;UINTN Bytes=0;EFI_HANDLE Driver=NULL;
  EFI_STATUS Status=GetSectionFromAnyFv(&Guid,EFI_SECTION_PE32,0,&Source,&Bytes);
  if(!EFI_ERROR(Status)) {
    Status=gBS->LoadImage(FALSE,gImageHandle,NULL,Source,Bytes,&Driver);FreePool(Source);
    if(!EFI_ERROR(Status)) {
      Status=gBS->StartImage(Driver,NULL,NULL);
      if(EFI_ERROR(Status))gBS->UnloadImage(Driver);
    }
  }
  DEBUG((DEBUG_WARN,"SUNUEFI_PMIC_METADATA_LOAD %r\\n",Status));return Status;
}
'''
        text=text.replace('#include <Uefi.h>','#include <Uefi.h>\n#include <PiDxe.h>\n#include <Library/DxeServicesLib.h>')
        text=text.replace('#pragma pack(1)',helper+'\n#pragma pack(1)',1)
        # Hardware metadata is validated using the real handoff DT after its
        # bounds and app payload are verified; then the dependency graph starts.
        text=text.replace('  PianoProbeFoundation ();','')
        text=text.replace('  PianoProbeUsbPower(Fdt);',
            '  Status=LoadPmicMetadata();\n  if(EFI_ERROR(Status)){PianoStopFaultRecovery();return Status;}\n  PianoProbeFoundation();\n  PianoProbeUsbPower(Fdt);')
        path.write_text(text)
    if args.dma_probe:
        for name in ('PianoSmmu.c','PianoSmmu.h','PianoDma.c','PianoDma.h','PianoDmaSelfTest.c'):
            shutil.copyfile(root/'bootprofiles/uefi-app'/name,app/name)
        path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoSmmu.c\n  PianoDma.c\n  PianoDmaSelfTest.c')
        text=text.replace('  DebugLib','  DebugLib\n  DxeServicesTableLib\n  CacheMaintenanceLib');path.write_text(text)
        path=app/'RamApp.c';text=path.read_text().replace('#pragma pack(1)',
            '#include "PianoSmmu.h"\nSTATIC PIANO_SMMU_SNAPSHOT mBefore,mAfter;\nVOID PianoDmaMemoryTest(VOID);\nVOID PianoFaultSetDiagnostic(VOID (*Diagnostic)(VOID));\nSTATIC VOID DmaFaultDiagnostic(VOID){PianoSmmuLogFaults(mAfter.Valid?&mAfter:&mBefore);}\n#pragma pack(1)',1)
        text=text.replace('  PianoProbeFoundation ();','')
        text=text.replace('  PianoProbeUfs(Fdt);',
            '  Status=PianoSmmuCapture(Fdt,"before-foundation",&mBefore);\n  DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_BEFORE %r\\n",Status));\n  PianoProbeFoundation();\n  Status=PianoSmmuCapture(Fdt,"after-foundation",&mAfter);\n  DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_AFTER %r\\n",Status));\n  PianoFaultSetDiagnostic(DmaFaultDiagnostic);\n  PianoDmaMemoryTest();\n  PianoProbeUfs(Fdt);')
        path.write_text(text)
    if args.dma_owned:
        for name in ('PianoOwnedSmmu.c','PianoOwnedSmmu.h','PianoIoPageTable.c','PianoIoPageTable.h'):
            shutil.copyfile(root/'bootprofiles/uefi-app'/name,app/name)
        path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoOwnedSmmu.c\n  PianoIoPageTable.c')
        text+='  gEfiLoadedImageProtocolGuid\n';path.write_text(text)
        path=app/'RamApp.c';text=path.read_text().replace('#pragma pack(1)',
            '#include "PianoOwnedSmmu.h"\nVOID PianoUfsSetProbeAction(EFI_STATUS (*Action)(CONST VOID *Fdt));\n#pragma pack(1)',1)
        text=text.replace('  PianoProbeUfs(Fdt);','  PianoUfsSetProbeAction(PianoOwnedSmmuMemoryExperiment);\n  PianoProbeUfs(Fdt);');path.write_text(text)
    if args.ufs_dma_nop:
        for name in ('PianoUfsReadOnlyDma.c','PianoUfsDmaLayout.c','PianoUfsDmaLayout.h','PianoGpt.c','PianoGpt.h'):
            shutil.copyfile(root/'bootprofiles/uefi-app'/name,app/name)
        path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoUfsReadOnlyDma.c\n  PianoUfsDmaLayout.c\n  PianoGpt.c')
        if '  PrintLib\n' not in text:text=text.replace('  DebugLib','  DebugLib\n  PrintLib')
        path.write_text(text)
        path=app/'RamApp.c';text=path.read_text().replace('#pragma pack(1)',
            'EFI_STATUS PianoUfsReadOnlyDmaExperiment(CONST VOID *Fdt);\n#pragma pack(1)',1)
        text=text.replace('PianoUfsSetProbeAction(PianoOwnedSmmuMemoryExperiment)',
            'PianoUfsSetProbeAction(PianoUfsReadOnlyDmaExperiment)');path.write_text(text)
    if args.ufs_blockio:
        for name in ('PianoReadOnlyBlock.c','PianoReadOnlyBlock.h','PianoUfsShutdown.h'):
            shutil.copyfile(root/'bootprofiles/uefi-app'/name,app/name)
        path=app/'PianoUfsReadOnlyDma.c';path.write_text('#define PIANO_UFS_BLOCKIO 1\n'+path.read_text())
        path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoReadOnlyBlock.c')
        text+='  gEfiDevicePathProtocolGuid\n';text=text.replace('[Protocols]','[Guids]\n  gEfiEventExitBootServicesGuid\n\n[Protocols]');path.write_text(text)
        path=app/'RamApp.c';text=path.read_text().replace('#pragma pack(1)','VOID PianoUfsBlockIoStop(VOID);\n#pragma pack(1)',1)
        text=text.replace('PianoStopFaultRecovery();','PianoUfsBlockIoStop(); PianoStopFaultRecovery();');path.write_text(text)
    if args.ufs_write_preflight or args.ufs_write_restore_test:
        for name in ('PianoUfsWriteTest.c','PianoUfsWriteTest.h'):
            shutil.copyfile(root/'bootprofiles/uefi-app'/name,app/name)
        write_attestation.prepare_ufs_write_test(output=app/'PianoUfsWriteTestBaseline.h')
        mode='PIANO_UFS_WRITE_PREFLIGHT' if args.ufs_write_preflight else 'PIANO_UFS_WRITE_RESTORE_TEST'
        path=app/'PianoUfsReadOnlyDma.c'
        path.write_text('#define PIANO_UFS_WRITE_TEST 1\n#define '+mode+' 1\n'+path.read_text())
        path=app/'RamApp.inf';path.write_text(path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoUfsWriteTest.c'))
        path=app/'RamApp.c';path.write_text(write_test_ram_app(path.read_text()))
    if args.ufs_bounded_filesystem_test:
        for name in ('PianoUfsWriteTest.c','PianoUfsWriteTest.h','PianoUfsBoundedBlock.c','PianoUfsBoundedBlock.h',
                     'PianoUfsBoundedLayout.c','PianoUfsBoundedLayout.h','PianoUfsBoundedTransport.h',
                     'PianoUfsBoundedBindings.inc','PianoUfsBoundedFileSystemTest.c','PianoUfsBoundedFileSystemTest.h'):
            shutil.copyfile(root/'bootprofiles/uefi-app'/name,app/name)
        write_attestation.prepare_ufs_write_test(output=app/'PianoUfsWriteTestBaseline.h')
        bounded_format.prepare(app/'PianoUfsBoundedFsFormat.h')
        path=app/'PianoUfsReadOnlyDma.c'
        path.write_text('#define PIANO_UFS_BOUNDED_VOLUME 1\n#define PIANO_UFS_BOUNDED_FS_TEST 1\n'+path.read_text())
        path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c',
            '  RamApp.c\n  PianoUfsWriteTest.c\n  PianoUfsBoundedBlock.c\n  PianoUfsBoundedLayout.c\n  PianoUfsBoundedFileSystemTest.c')
        text=text.replace('  gEfiEventExitBootServicesGuid','  gEfiEventExitBootServicesGuid\n  gEfiFileInfoGuid')
        text+='  gEfiSimpleFileSystemProtocolGuid\n';path.write_text(text)
        path=app/'RamApp.c';path.write_text(bounded_fs_ram_app(path.read_text()))
    if args.ufs_filesystems:
        shutil.copyfile(root/'bootprofiles/uefi-app/PianoUfsFileSystemProbe.c',app/'PianoUfsFileSystemProbe.c')
        path=app/'PianoUfsReadOnlyDma.c';path.write_text('#define PIANO_UFS_FILESYSTEMS 1\n'+path.read_text())
        path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoUfsFileSystemProbe.c')
        text=text.replace('  gEfiEventExitBootServicesGuid','  gEfiEventExitBootServicesGuid\n  gEfiFileInfoGuid\n  gEfiFileSystemInfoGuid')
        text+='  gEfiSimpleFileSystemProtocolGuid\n';path.write_text(text)
    if args.ufs_shell:
        shutil.copyfile(root/'bootprofiles/uefi-app/PianoLaunchShell.c',app/'PianoLaunchShell.c')
        path=app/'PianoUfsReadOnlyDma.c';path.write_text('#define PIANO_UFS_SHELL 1\n'+path.read_text())
        path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoLaunchShell.c')
        text=text.replace('  DebugLib','  DebugLib\n  DevicePathLib')
        text+='  gEfiFirmwareVolume2ProtocolGuid\n';path.write_text(text)
        path=app/'RamApp.c';text=path.read_text().replace('#pragma pack(1)',
            'EFI_STATUS PianoRunShellDiagnostics(EFI_HANDLE Parent,BOOLEAN Interactive);\n#pragma pack(1)',1)
        text=text.replace('  Status=gBS->LoadImage (FALSE,ImageHandle',
            '  Status=PianoRunShellDiagnostics(ImageHandle,'+('TRUE' if args.ufs_shell_interactive else 'FALSE')+');\n'
            '  DEBUG((DEBUG_WARN,"SUNUEFI_SHELL_DIAGNOSTICS_RETURN %r\\n",Status));\n'
            '  Status=gBS->LoadImage (FALSE,ImageHandle')
        path.write_text(text)
    if args.ufs_setup:
        shutil.copyfile(root/'bootprofiles/uefi-app/PianoLaunchSetup.c',app/'PianoLaunchSetup.c')
        path=app/'PianoUfsReadOnlyDma.c';path.write_text('#define PIANO_UFS_SETUP 1\n'+path.read_text())
        path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoLaunchSetup.c')
        text=text.replace('  MdePkg/MdePkg.dec','  MdePkg/MdePkg.dec\n  MdeModulePkg/MdeModulePkg.dec')
        for library in ('DevicePathLib','PcdLib'):
            if '  '+library+'\n' not in text:text=text.replace('  DebugLib','  DebugLib\n  '+library)
        for protocol in ('gEfiFirmwareVolume2ProtocolGuid','gEfiHiiDatabaseProtocolGuid','gEfiHiiStringProtocolGuid',
                         'gEfiHiiFontProtocolGuid','gEfiHiiConfigRoutingProtocolGuid','gEfiFormBrowser2ProtocolGuid',
                         'gEdkiiFormDisplayEngineProtocolGuid','gEfiVariableArchProtocolGuid','gEfiVariableWriteArchProtocolGuid'):
            if '  '+protocol+'\n' not in text:text+='  '+protocol+'\n'
        text+='\n[Pcd]\n  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable\n'
        path.write_text(text)
        path=app/'RamApp.c';text=path.read_text().replace('#pragma pack(1)',
            'EFI_STATUS PianoLaunchSetup(EFI_HANDLE Parent);\n#pragma pack(1)',1)
        text=text.replace('  Status=gBS->LoadImage (FALSE,ImageHandle',
            '  Status=PianoLaunchSetup(ImageHandle);\n'
            '  DEBUG((DEBUG_WARN,"SUNUEFI_SETUP_DIAGNOSTICS_RETURN %r\\n",Status));\n'
            '  Status=gBS->LoadImage (FALSE,ImageHandle')
        path.write_text(text)
    if args.usb_controller:
        for name in ('PianoUsbController.c','PianoUsbRamBootExperiment.h','PianoUsbService.h','PianoDwc3Service.h','PianoDma.c','PianoDma.h','PianoSmmu.c','PianoSmmu.h',
                     'PianoOwnedSmmu.c','PianoOwnedSmmu.h','PianoIoPageTable.c','PianoIoPageTable.h'):
            shutil.copyfile(root/'bootprofiles/uefi-app'/name,app/name)
        path=app/'PianoKeys.c';path.write_text('#define PIANO_USB_POWER_PROBE 1\n'+path.read_text())
        mapping=target/'Library/MemoryMapLib/MemoryMapLib.c';text=mapping.read_text()
        anchor='  {"CRYPTO0_CRYPTO",'
        if text.count(anchor)!=1:raise SystemExit('Unexpected USB PHY mapping anchor')
        text=text.replace(anchor,'  {"Piano_USB2_PHY", 0x88E3000, 0x1000, AddDev, 1, 0x400, 11, NS_DEVICE},\n'
                                 '  {"Piano_USB3_PHY", 0x88E8000, 0x3000, AddDev, 1, 0x400, 11, NS_DEVICE},\n'+anchor)
        mapping.write_text(text)
        path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoUsbController.c\n  PianoDma.c\n  PianoSmmu.c\n  PianoOwnedSmmu.c\n  PianoIoPageTable.c')
        text=text.replace('  DebugLib','  DebugLib\n  PrintLib\n  IoLib\n  CacheMaintenanceLib\n  DxeServicesTableLib')
        text=text.replace('  MdePkg/MdePkg.dec','  MdePkg/MdePkg.dec\n  QcomPkg/QcomPkg.dec')
        text+='  gEfiLoadedImageProtocolGuid\n';path.write_text(text)
        path=app/'RamApp.c';text=path.read_text().replace('#pragma pack(1)','EFI_STATUS PianoUsbControllerExperiment(CONST VOID *Fdt);\n#pragma pack(1)',1)
        text=text.replace('  PianoProbeFoundation ();','')
        foundation='' if args.usb_ufs_fetch else '  PianoProbeFoundation();\n'
        text=text.replace('  Status=PianoStartKeys(Fdt);',foundation+'  Status=PianoUsbControllerExperiment(Fdt);\n  DEBUG((DEBUG_WARN,"SUNUEFI_USB_CONTROLLER_ACTION %r\\n",Status));\n  Status=PianoStartKeys(Fdt);');path.write_text(text)
        if args.usb_ep0:
            for name in ('PianoDwc3Device.c','PianoUsbControl.c','PianoUsbControl.h'):
                shutil.copyfile(root/'bootprofiles/uefi-app'/name,app/name)
            path=app/'PianoUsbController.c';path.write_text('#define PIANO_USB_EP0 1\n'+path.read_text())
            path=app/'RamApp.inf';path.write_text(path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoDwc3Device.c\n  PianoUsbControl.c'))
            if args.usb_fastboot:
                for name in ('PianoFastboot.c','PianoFastboot.h'):
                    shutil.copyfile(root/'bootprofiles/uefi-app'/name,app/name)
                for name in ('PianoDwc3Device.c','PianoUsbControl.c'):
                    path=app/name;path.write_text('#define PIANO_USB_FASTBOOT 1\n'+path.read_text())
                path=app/'RamApp.inf';path.write_text(path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoFastboot.c'))
                if args.usb_screenshot:
                    for name in ('PianoFastbootScreen.c','PianoFastbootScreen.h'):
                        shutil.copyfile(root/'bootprofiles/uefi-app'/name,app/name)
                    path=app/'PianoDwc3Device.c';path.write_text('#define PIANO_USB_SCREENSHOT 1\n'+path.read_text())
                    path=app/'RamApp.inf';path.write_text(path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoFastbootScreen.c'))
                if args.usb_ram_boot:
                    sources=('PianoFastbootBoot.c','PianoFastbootLaunch.c','PianoFastbootDownloadBlob.c','PianoUsbRamBoot.c')
                    headers=('PianoFastbootBoot.h','PianoFastbootLaunch.h','PianoFastbootDownloadBlob.h','PianoUsbRamBoot.h','PianoCpuInput.h',
                             'PianoRamBootProbe.h','PianoUsbStorageExperiment.h')
                    for name in sources+headers:shutil.copyfile(root/'bootprofiles/uefi-app'/name,app/name)
                    # Existing ram-boot source now shares the product CPU
                    # chunks/ownership helper; default transport limit stays64MiB.
                    shutil.copyfile(root/'bootprofiles/os-boot/PianoCpuInput.c',app/'PianoCpuInput.c')
                    for name in ('PianoFastboot.c','PianoDwc3Device.c','PianoUsbController.c'):
                        path=app/name;path.write_text('#define PIANO_USB_RAM_BOOT 1\n'+path.read_text())
                    path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c','  RamApp.c\n'+''.join('  '+name+'\n' for name in (*sources,'PianoCpuInput.c')).rstrip())
                    text=text.replace('  UefiLib','  UefiLib\n  UefiRuntimeServicesTableLib')
                    text+='\n[Guids]\n  gEfiEventExitBootServicesGuid\n  gEfiEventBeforeExitBootServicesGuid\n';path.write_text(text)
                    path=app/'RamApp.c';text=path.read_text().replace('#pragma pack(1)',
                        '#include "PianoUsbRamBoot.h"\n#pragma pack(1)',1)
                    text=text.replace('Status=PianoUsbControllerExperiment(Fdt)','Status=PianoRunUsbRamBoot(Fdt,ImageHandle)')
                    text=text.replace('  Status=PianoStartKeys(Fdt);\n','').replace('  DEBUG((DEBUG_WARN,"SUNUEFI_KEYS_START %r\\n",Status));\n','')
                    text=write_test_ram_app(text).replace('SUNUEFI_UFS_WRITE_PROFILE_WAIT boot_blocked=1 readonly_blockio=1',
                        'SUNUEFI_RAM_BOOT_PROFILE_WAIT probe_only=1 max_download_64MiB=1')
                    path.write_text(text)
                    path=app/'PianoFaultRecovery.c';path.write_text('#define PIANO_USB_RAM_BOOT 1\n'+path.read_text())
                if args.usb_ufs_fetch:
                    for name in ('PianoUsbStorageExperiment.h','PianoFastbootBlockRead.c','PianoFastbootBlockRead.h',
                                 'PianoUsbUfsFetch.c','PianoUsbUfsFetch.h'):
                        shutil.copyfile(root/'bootprofiles/uefi-app'/name,app/name)
                    for name in ('PianoUsbController.c','PianoDwc3Device.c'):
                        path=app/name;path.write_text('#define PIANO_USB_UFS_FETCH 1\n'+path.read_text())
                    path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c','  RamApp.c\n  PianoFastbootBlockRead.c\n  PianoUsbUfsFetch.c')
                    text+='  gEfiPartitionInfoProtocolGuid\n';path.write_text(text)
                    path=app/'RamApp.c';text=path.read_text().replace('#pragma pack(1)',
                        '#include "PianoUsbUfsFetch.h"\n#pragma pack(1)',1)
                    text=text.replace('Status=PianoUsbControllerExperiment(Fdt)','Status=PianoRunUsbUfsFetch(Fdt)')
                    text=write_test_ram_app(text).replace('SUNUEFI_UFS_WRITE_PROFILE_WAIT boot_blocked=1 readonly_blockio=1',
                        'SUNUEFI_FETCH_PROFILE_WAIT boot_blocked=1 readonly=1')
                    path.write_text(text)
                    path=app/'PianoFaultRecovery.c';path.write_text('#define PIANO_USB_UFS_FETCH 1\n'+path.read_text())
    if args.pogo_register_probe or args.high_ram_readonly:
        if args.pogo_register_probe:
            sources=('PianoPogoProbe.c','PianoGeniI2cPio.c');headers=('PianoPogoProbe.h','PianoGeniI2cPio.h')
            call='Status=PianoProbePogo(Fdt);';declaration='#include "PianoPogoProbe.h"'
        else:
            sources=('PianoHighRamReadonly.c','PianoHighRamProbe.c');headers=('PianoHighRamProbe.h',)
            call='Status=PianoProbeHighRamReadonly();';declaration='EFI_STATUS PianoProbeHighRamReadonly(VOID);'
        for name in sources+headers:shutil.copyfile(root/'bootprofiles/uefi-app'/name,app/name)
        for name in sources:
            path=app/name;define='#define PIANO_POGO_PROBE_EXPERIMENT 1\n' if args.pogo_register_probe else '#define PIANO_HIGH_RAM_PROBE_EXPERIMENT 1\n'
            path.write_text(define+path.read_text())
        path=app/'RamApp.inf';text=path.read_text().replace('  RamApp.c','  RamApp.c\n'+''.join('  '+name+'\n' for name in sources).rstrip())
        text=text.replace('  DebugLib','  DebugLib\n  DxeServicesTableLib\n  PrintLib\n  TimerLib')
        if args.pogo_register_probe:
            text=text.replace('  MdePkg/MdePkg.dec','  MdePkg/MdePkg.dec\n  QcomPkg/QcomPkg.dec')
            text+='  gEfiCpuArchProtocolGuid\n  gEfiMemoryAttributeProtocolGuid\n'
        path.write_text(text)
        path=app/'RamApp.c';text=path.read_text().replace('#pragma pack(1)',declaration+'\n#pragma pack(1)',1)
        anchor='  Status=gBS->LoadImage (FALSE,ImageHandle'
        if text.count(anchor)!=1:raise ValueError('Unexpected readonly diagnostic app anchor')
        text=text.replace(anchor,'  '+call+'\n  DEBUG((DEBUG_WARN,"SUNUEFI_READONLY_DIAGNOSTIC_RETURN %r\\n",Status));\n'+anchor)
        path.write_text(write_test_ram_app(text).replace('SUNUEFI_UFS_WRITE_PROFILE_WAIT boot_blocked=1 readonly_blockio=1',
            'SUNUEFI_READONLY_PROFILE_WAIT no_storage=1 no_os=1'))
    dsc = target / 'pianoGui.dsc'
    text = dsc.read_text().replace('pianoGuiPkg/Library/RamLogSerialPortLib/FrameBufferSerialPortLib.inf',
                                  'pianoGuiPkg/Library/RamOnlySerialPortLib/RamOnlySerialPortLib.inf')
    text += '''
[Components]
  pianoGuiPkg/Drivers/PianoGopDxe/PianoGopDxe.inf
  pianoGuiPkg/Applications/RamApp/RamApp.inf {
    <LibraryClasses>
      BaseCryptLib|OpensslPkg/Library/BaseCryptLib/BaseCryptLib.inf
      OpensslLib|OpensslPkg/Library/OpensslLib/OpensslLib.inf
      IntrinsicLib|CryptoPkg/Library/IntrinsicLib/IntrinsicLib.inf
      RngLib|MdePkg/Library/BaseRngLibNull/BaseRngLibNull.inf
  }
'''
    dsc.write_text(text)
    if args.ufs_shell:
        # Match the pinned ShellPkg, rather than importing an unrelated binary.
        # No debug1 memory-write, install1/bcfg or network command libraries.
        dsc.write_text(dsc.read_text()+'''
[LibraryClasses]
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
  HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
  OrderedCollectionLib|MdePkg/Library/BaseOrderedCollectionRedBlackTreeLib/BaseOrderedCollectionRedBlackTreeLib.inf
[Components]
  ShellPkg/Application/Shell/Shell.inf {
    <PcdsFixedAtBuild>
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
      gEfiShellPkgTokenSpaceGuid.PcdShellSupportLevel|3
      gEfiShellPkgTokenSpaceGuid.PcdShellProfileMask|0x01
      gEfiShellPkgTokenSpaceGuid.PcdShellPageBreakDefault|FALSE
    <LibraryClasses>
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDriver1CommandsLib/UefiShellDriver1CommandsLib.inf
  }
''')
    if args.ufs_setup:
        dsc.write_text(dsc.read_text()+SETUP_DSC_ADDITIONS)
    if args.pmic_metadata:
        dsc.write_text(dsc.read_text()+'\n[Components]\n  pianoGuiPkg/Drivers/PianoPmicMetadata/PianoPmicMetadata.inf\n')
    fdf = target / 'pianoGui.fdf'
    text = fdf.read_text().replace('SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf',
                                  'pianoGuiPkg/Drivers/PianoGopDxe/PianoGopDxe.inf')
    text = text.replace('!include SiliciumPkg/Common.fdf.inc',
                        '  INF pianoGuiPkg/Applications/RamApp/RamApp.inf\n!include SiliciumPkg/Common.fdf.inc')
    if args.foundation:
        text=text.replace('!include SiliciumPkg/Common.fdf.inc',
                          (root/'build/native-foundation.fdf.inc').read_text()+'\n!include SiliciumPkg/Common.fdf.inc')
    if args.ufs_blockio:
        text=text.replace('!include SiliciumPkg/Common.fdf.inc',
            '  INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf\n'
            '  INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf\n!include SiliciumPkg/Common.fdf.inc')
    if args.ufs_filesystems or args.ufs_bounded_filesystem_test:
        text=text.replace('!include SiliciumPkg/Common.fdf.inc',
            '  INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf\n'
            '  INF FatPkg/EnhancedFatDxe/Fat.inf\n!include SiliciumPkg/Common.fdf.inc')
    if args.ufs_shell:
        text=text.replace('!include SiliciumPkg/Common.fdf.inc',
            '  INF ShellPkg/Application/Shell/Shell.inf\n!include SiliciumPkg/Common.fdf.inc')
    if args.ufs_setup:
        text=text.replace('!include SiliciumPkg/Common.fdf.inc',
            ''.join('  INF '+module+'\n' for module in SETUP_FV_MODULES)+'!include SiliciumPkg/Common.fdf.inc')
    if args.usb_debug:
        # Real EDK2 SDT service; no invented USB AML or fake success protocol.
        text=text.replace('!include SiliciumPkg/Common.fdf.inc',
            '  INF MdeModulePkg/Universal/Acpi/AcpiTableDxe/AcpiTableDxe.inf\n!include SiliciumPkg/Common.fdf.inc')
    if args.pmic_metadata:
        text=text.replace('!include SiliciumPkg/Common.fdf.inc',
            '  INF pianoGuiPkg/Drivers/PianoPmicMetadata/PianoPmicMetadata.inf\n!include SiliciumPkg/Common.fdf.inc')
    if args.ram_qupfw:
        import hashlib
        firmware=(root/'private/captures/qupfw-test28/qupfw_a.img').read_bytes()
        if hashlib.sha256(firmware).hexdigest()!='b648516e1fde83a1b6a0d6a3c1bc3084adc63aecc34f7756c31850ab4ad4b4e9':
            raise SystemExit('QUP active-slot firmware hash mismatch')
        folder=root/'upstream/Mu-Silicium/Binaries/piano/QupFw';folder.mkdir(parents=True,exist_ok=True)
        (folder/'qupfw_a.bin').write_bytes(firmware)
        text=text.replace('!include SiliciumPkg/Common.fdf.inc',
            '  FILE FREEFORM = 77997A49-2795-457B-95B3-409BAC124CA3 {\n    SECTION RAW = Binaries/piano/QupFw/qupfw_a.bin\n  }\n!include SiliciumPkg/Common.fdf.inc')
    fdf.write_text(text)
    lib = target / 'Library/Stage0BootManagerLib'
    inf = lib / 'Stage0BootManagerLib.inf'
    inf.write_text(inf.read_text().replace('  FdtLib','  FdtLib\n  DxeServicesLib\n  MemoryAllocationLib'))
    c = lib / 'Stage0BootManagerLib.c'
    text = c.read_text().replace('#include <Library/DebugLib.h>',
        '#include <Library/DebugLib.h>\n#include <Library/DxeServicesLib.h>\n#include <Library/MemoryAllocationLib.h>')
    if args.ufs_blockio:
        shutil.copyfile(root/'bootprofiles/uefi-app/PianoUfsShutdown.h',lib/'PianoUfsShutdown.h')
        text=text.replace('#include <Protocol/GraphicsOutput.h>','#include <Protocol/GraphicsOutput.h>\n#include "PianoUfsShutdown.h"')
        text=text.replace('  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);',
            '  EFI_GUID ShutdownGuid=PIANO_UFS_SHUTDOWN_GUID; PIANO_UFS_SHUTDOWN *Shutdown=NULL;\n'
            '  if(!EFI_ERROR(gBS->LocateProtocol(&ShutdownGuid,NULL,(VOID **)&Shutdown)) && Shutdown->Revision==1)\n'
            '    Shutdown->Halt();\n  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);')
    if args.usb_ufs_fetch or args.usb_ram_boot:
        for name in ('PianoUsbStorageExperiment.h','PianoFastboot.h'):
            shutil.copyfile(root/'bootprofiles/uefi-app'/name,lib/name)
        if args.usb_ram_boot:
            text=text.replace('#include <Protocol/GraphicsOutput.h>',
                '#include <Protocol/GraphicsOutput.h>\n#include "PianoUsbStorageExperiment.h"\n#include <Library/BaseLib.h>')
            anchor='  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);'
            if text.count(anchor)!=1:raise ValueError('Unexpected RAM boot timer anchor')
            text=text.replace(anchor,'''  EFI_GUID UsbGuid=PIANO_USB_SHUTDOWN_GUID; PIANO_USB_SHUTDOWN *Usb=NULL;
  EFI_STATUS UsbLocate=gBS->LocateProtocol(&UsbGuid,NULL,(VOID **)&Usb);
  if(UsbLocate==EFI_SUCCESS) {
    if(Usb==NULL || Usb->Revision!=1 || Usb->Halt==NULL || Usb->Halt()!=EFI_SUCCESS)CpuDeadLoop();
  } else if(UsbLocate!=EFI_NOT_FOUND)CpuDeadLoop();
'''+anchor)
            text=text.replace(anchor,anchor+'\n  CpuDeadLoop(); // Reset returning must never resume the halted USB poll loop.')
        else:
            text=text.replace('#include "PianoUfsShutdown.h"',
                '#include "PianoUfsShutdown.h"\n#include "PianoUsbStorageExperiment.h"\n#include <Library/BaseLib.h>')
    if args.usb_ufs_fetch:
        before='  EFI_GUID ShutdownGuid=PIANO_UFS_SHUTDOWN_GUID; PIANO_UFS_SHUTDOWN *Shutdown=NULL;'
        replacement='''  EFI_GUID UsbGuid=PIANO_USB_SHUTDOWN_GUID; PIANO_USB_SHUTDOWN *Usb=NULL;
  EFI_STATUS UsbLocate=gBS->LocateProtocol(&UsbGuid,NULL,(VOID **)&Usb);
  if(UsbLocate==EFI_SUCCESS) {
    if(Usb==NULL || Usb->Revision!=1 || Usb->Halt==NULL || Usb->Halt()!=EFI_SUCCESS)CpuDeadLoop();
  } else if(UsbLocate!=EFI_NOT_FOUND)CpuDeadLoop();
  EFI_GUID ShutdownGuid=PIANO_UFS_SHUTDOWN_GUID; PIANO_UFS_SHUTDOWN *Shutdown=NULL;'''
        if text.count(before)!=1:raise ValueError('Unexpected combined timer UFS shutdown anchor')
        text=text.replace(before,replacement)
        old='''  if(!EFI_ERROR(gBS->LocateProtocol(&ShutdownGuid,NULL,(VOID **)&Shutdown)) && Shutdown->Revision==1)
    Shutdown->Halt();'''
        new='''  EFI_STATUS UfsLocate=gBS->LocateProtocol(&ShutdownGuid,NULL,(VOID **)&Shutdown);
  if(UfsLocate==EFI_SUCCESS) {
    if(Shutdown==NULL || Shutdown->Revision!=1 || Shutdown->Halt==NULL || Shutdown->Halt()!=EFI_SUCCESS)CpuDeadLoop();
  } else if(UfsLocate!=EFI_NOT_FOUND)CpuDeadLoop();'''
        if text.count(old)!=1:raise ValueError('Unexpected combined timer halt anchor')
        text=text.replace(old,new)
    text = text.replace('Internal storage and USB mass-storage drivers are excluded.',
                        'Piano UEFI GOP/simple-init diagnostic.')
    text = text.replace('Linux and Windows PE boot are not implemented in this image.',
                        'Simple-init is loaded from temporary boot RAM.')
    text = text.replace('45ULL * 10000000ULL',f'{args.return_seconds}ULL * 10000000ULL').replace('in 45 seconds',f'in {args.return_seconds} seconds')
    text = text.replace('VOID EFIAPI DeviceBootManagerUnableToBoot (VOID) { }','''VOID EFIAPI DeviceBootManagerUnableToBoot (VOID) {
  EFI_GUID Guid = {0xA2610F94,0x834D,0x4E8D,{0xB5,0x12,0x22,0x50,0x38,0x21,0xFD,0xB9}};
  VOID *Source = NULL; UINTN Size = 0; EFI_HANDLE App; EFI_STATUS Status;
  Status = GetSectionFromAnyFv (&Guid,EFI_SECTION_PE32,0,&Source,&Size);
  if (!EFI_ERROR (Status)) {
    Status = gBS->LoadImage (FALSE,gImageHandle,NULL,Source,Size,&App);
    FreePool (Source);
    if (!EFI_ERROR (Status)) {
      Status = gBS->StartImage (App,NULL,NULL);
      gBS->UnloadImage (App);
    }
  }
  DEBUG ((DEBUG_WARN,"SUNUEFI_GUI_LOADER_RETURN %r\\n",Status));
  // Retain logs until the existing 75-second recovery timer fires.
  while (TRUE) { gBS->Stall (100000); }
}''')
    c.write_text(text.replace('existing 75-second recovery timer',f'existing {args.return_seconds}-second recovery timer'))
    generated=(app/'RamApp.c').read_text()
    for enabled,symbol in ((args.usb_debug,'PianoStartUsbDebug'),
                           (args.ufs_probe,'PianoProbeUfs'),
                           (args.touch_probe,'PianoProbeTouch')):
        if not enabled and symbol in generated:
            raise SystemExit('Generated profile contains an unexpected feature: '+symbol)
    import json
    (root/'build/gui-profile.json').write_text(json.dumps(vars(args),indent=2)+'\n')
    shutil.copytree(target,root / 'upstream/Mu-Silicium/Platforms/Xiaomi/pianoGuiPkg',dirs_exist_ok=True)
    print('Separate pianoGuiPkg prepared; tested Linux/probe targets retained')


if __name__ == '__main__':
    main()
