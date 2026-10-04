#!/usr/bin/env python3
"""Prepare a host-only SM8750 piano stage-0 target from captured native data.

No device commands. Firmware blobs stay in the ignored upstream workspace.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid
from analyze_capture import parse_fdt, reg_ranges, scan_fdts, text

REFERENCE_COMMIT = '66e7bd1e7bcb757d4b28629bd6409d7209d3b242'
NATIVE_MODULES = (
    'EnvDxeEnhanced', 'ShmBridgeDxeLA', 'ScmDxeCompat', 'TzDxeLA',
    'ChipInfo', 'PlatformInfoDxeDriver', 'DALSys', 'HWIODxeDriver',
    'HALIOMMU', 'SmemDxe',
)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--capture', type=Path, required=True)
    ap.add_argument('--extracted', type=Path, required=True)
    ap.add_argument('--workspace', type=Path, default=Path('upstream/Mu-Silicium'))
    ap.add_argument('--source', type=Path, default=Path('platforms/pianoPkg'))
    ap.add_argument('--fdtput', type=Path, default=Path('upstream/dtc/fdtput'))
    args = ap.parse_args()
    manifest = json.loads((args.capture / 'manifest.json').read_text())
    if manifest['properties']['ro.product.device'] != 'piano':
        raise SystemExit('Capture is not piano')
    # Refuse corrupt or incomplete native input before constructing platform files.
    for name in ('xbl_config_a.img', 'uefi_a.img', 'live.dtb', 'dtbo_a.img'):
        actual = hashlib.sha256((args.capture / name).read_bytes()).hexdigest()
        if actual != manifest['files'][name]['sha256']:
            raise SystemExit(f'Capture hash mismatch: {name}')
    fdts = list(scan_fdts((args.capture / 'xbl_config_a.img').read_bytes()))
    config = [nodes for _, _, nodes in fdts if '/soc/memorymap' in nodes and '/sw/uefi/int_param' in nodes]
    if len(config) != 1:
        raise SystemExit('Expected one native UEFI configuration DTB')
    nodes = config[0]
    reference = args.workspace / 'Platforms/OnePlus/dodgePkg'
    args.source.mkdir(parents=True, exist_ok=True)
    # Source template contains no device binaries or device trees.
    for rel in ('DeviceBuild.py', 'dodge.dsc', 'dodge.dec',
                'Library/MemoryMapLib/MemoryMapLib.inf',
                'Library/ConfigurationMapLib/ConfigurationMapLib.inf'):
        target = args.source / rel.replace('dodge', 'piano')
        target.parent.mkdir(parents=True, exist_ok=True)
        data = (reference / rel).read_text().replace('dodge', 'piano').replace('Platforms/OnePlus', 'Platforms/Xiaomi')
        data = data.replace('Silicium-ACPI/Platforms/OnePlus', 'Silicium-ACPI/Platforms/Xiaomi')
        if rel.endswith('.dec'):
            data = data.replace('7BD81CCC-CCD3-41CE-878A-72321436C338', str(uuid.uuid5(uuid.NAMESPACE_DNS, 'SunUEFI.piano.package')).upper())
        if rel.endswith('.dsc'):
            data = data.replace('FA3B7E2A-4F73-4C78-BBF2-1D8E292FFBD7', str(uuid.uuid5(uuid.NAMESPACE_DNS, 'SunUEFI.piano.platform')).upper())
            data = data.replace('|"OnePlus"', '|"Xiaomi"').replace('|"13"', '|"Pad 8 Pro"')
            data = data.replace('|"CPH2653"', '|"25091RP04C"').replace('|"23893"', '|"piano"')
            data = data.replace('PcdFrameBufferWidth|1440', 'PcdFrameBufferWidth|3200')
            data = data.replace('PcdFrameBufferHeight|3168', 'PcdFrameBufferHeight|2136')
            data = data.replace('  #dodge/AcpiTables.inf', '')
            data += '\n# Stage 0: no persistent variables, storage, capsules or OS boot.\n'
            data += '[PcdsFixedAtBuild]\n  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|TRUE\n'
            data += '  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorPartNumber|"SM8750P"\n'
            data += '  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorModel|"Snapdragon 8 Elite"\n'
            data += '[LibraryClasses]\n  DeviceBootManagerLib|pianoPkg/Library/Stage0BootManagerLib/Stage0BootManagerLib.inf\n'
            data += '[Components]\n  pianoPkg/Drivers/CapsuleArchNullDxe/CapsuleArchNullDxe.inf\n'
        target.write_text(data)
    # Convert stock Qualcomm DT attributes explicitly; unknown values are errors.
    hobs = {0: 'AddMem', 4: 'NoHob', 5: 'AddDev'}
    cache = {2: 'WRITE_BACK', 0x22: 'WRITE_BACK_XN', 0x20: 'WRITE_THROUGH_XN',
             0x25: 'UNCACHED_UNBUFFERED_XN', 9: 'NS_DEVICE'}
    entries = []
    observed = []
    for path, props in nodes.items():
        if not path.startswith(('/soc/memorymap/memory@', '/soc/registermap/memory@')):
            continue
        label = text(props['mem-label'])[0]
        if label == 'NOMAP':
            continue
        r, = reg_ranges(nodes, path)
        hob = hobs[int.from_bytes(props['build-hob'], 'big')]
        attr = cache[int.from_bytes(props['cache-attributes'], 'big')]
        resource = int.from_bytes(props['resource-type'], 'big')
        capability = int.from_bytes(props['resource-attribute'], 'big')
        memory = int.from_bytes(props['memory-type'], 'big')
        observed.append({'name': label, **r, 'hob': hob, 'resource_type': resource,
                         'resource_attribute': capability, 'memory_type': memory, 'arm_attribute': attr})
        # Same-chip upstream BootShim uses 3 MiB FD inside the original 4 MiB FD range.
        if label == 'UEFI_FD':
            entries.append('{"FD_Reserved", 0xA7000000, 0x100000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK}')
            r = {'base': 0xA7100000, 'size': 0x300000}
        if len(label.encode()) >= 32 or r['base'] % 4096 or r['size'] % 4096:
            raise SystemExit(f'Invalid memory descriptor {label}')
        entries.append(f'{{"{label}", 0x{r["base"]:X}, 0x{r["size"]:X}, {hob}, '
                       f'{resource}, 0x{capability:X}, {memory}, {attr}}}')
    memory_source = '// Generated from piano native xbl_config_a, not a phone memory map.\n'
    memory_source += '// SPDX-License-Identifier: BSD-2-Clause-Patent\n#include <Library/MemoryMapLib.h>\n'
    memory_source += 'STATIC EFI_MEMORY_REGION_DESCRIPTOR gMemoryDescriptor[] = {\n  ' + ',\n  '.join(entries) + '\n};\n'
    memory_source += 'VOID GetMemoryMap (OUT EFI_MEMORY_REGION_DESCRIPTOR **Map, OUT UINT8 *Count) {\n'
    memory_source += '  *Map = gMemoryDescriptor; *Count = ARRAY_SIZE (gMemoryDescriptor);\n}\n'
    (args.source / 'Library/MemoryMapLib/MemoryMapLib.c').write_text(memory_source)
    (args.source / 'native-memory-map.json').write_text(json.dumps(observed, indent=2) + '\n')
    params = nodes['/sw/uefi/int_param']
    # Logging writes are disabled; native configuration otherwise remains evidence-based.
    overrides = {'EnableLogFsSyncInRetail': 0, 'EnableDisplayImageFv': 0}
    values = [(k, overrides.get(k, int.from_bytes(v, 'big'))) for k,v in params.items()
              if k not in ('MaxCount', 'StrMaxCount')]
    config_source = '// Generated from piano native UEFI DT configuration.\n'
    config_source += '// SPDX-License-Identifier: BSD-2-Clause-Patent\n#include <Library/ConfigurationMapLib.h>\n'
    config_source += 'STATIC EFI_CONFIGURATION_ENTRY_DESCRIPTOR gConfigurationDescriptor[] = {\n'
    config_source += ',\n'.join(f'  {{"{k}", 0x{v:X}}}' for k,v in values) + '\n};\n'
    config_source += 'VOID GetConfigurationMap (OUT EFI_CONFIGURATION_ENTRY_DESCRIPTOR **Map, OUT UINT8 *Count) {\n'
    config_source += '  *Map = gConfigurationDescriptor; *Count = ARRAY_SIZE (gConfigurationDescriptor);\n}\n'
    (args.source / 'Library/ConfigurationMapLib/ConfigurationMapLib.c').write_text(config_source)

    # Retain original native binary DEPEX instead of borrowing patched phone drivers.
    binary_root = args.workspace / 'Binaries/piano/Stage0'
    binary_root.mkdir(parents=True, exist_ok=True)
    index = {}
    for ui in args.extracted.rglob('*.ui'):
        name = ui.read_bytes().decode('utf-16-le', errors='replace').strip('\0')
        if name in NATIVE_MODULES:
            index[name] = ui.parent
    binary_manifest = {}
    for name in NATIVE_MODULES:
        directory = index[name]
        guid = directory.name.removeprefix('file-')
        pe, = directory.glob('*.pe')
        depex_files = list(directory.glob('*.dxe.depex'))
        if len(depex_files) > 1:
            raise SystemExit(f'Ambiguous dependency section: {name}')
        depex = depex_files[0] if depex_files else None
        target = binary_root / name
        target.mkdir(exist_ok=True)
        shutil.copyfile(pe, target / (name + '.efi'))
        if depex is not None:
            shutil.copyfile(depex, target / (name + '.depex'))
        inf = f'''[Defines]
  INF_VERSION = 0x0001001B
  BASE_NAME = {name}
  FILE_GUID = {guid}
  MODULE_TYPE = DXE_DRIVER
  VERSION_STRING = 1.0
  ENTRY_POINT = EfiEntry
[Binaries.AARCH64]
  PE32|{name}.efi|*
'''
        if depex is not None:
            inf += f'  DXE_DEPEX|{name}.depex|*\n'
        (target / (name + '.inf')).write_text(inf)
        binary_manifest[name] = {'guid': guid, 'pe_sha256': hashlib.sha256(pe.read_bytes()).hexdigest(),
                                'depex_sha256': hashlib.sha256(depex.read_bytes()).hexdigest() if depex else None}
    (args.source / 'native-modules.json').write_text(json.dumps(binary_manifest, indent=2) + '\n')
    # Only selected components go into this FV. No disk/partition/UFS/USB/capsule/update apps.
    core = [
        'MdeModulePkg/Core/Dxe/DxeMain.inf', 'MdeModulePkg/Universal/PCD/Dxe/Pcd.inf',
        'MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf',
        'MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf',
        'MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf',
        'ArmPkg/Drivers/CpuDxe/CpuDxe.inf', 'ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf',
        'ArmPkg/Drivers/TimerDxe/TimerDxe.inf', 'EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf',
        'MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf',
        'MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf',
        'MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf',
        'MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf',
        'pianoPkg/Drivers/CapsuleArchNullDxe/CapsuleArchNullDxe.inf',
        'EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf',
        'EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf',
        'MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf',
        'MdeModulePkg/Universal/PrintDxe/PrintDxe.inf',
        'MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf',
        'MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf',
        'MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf',
        'MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf',
        'SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf',
        'MdeModulePkg/Universal/BdsDxe/BdsDxe.inf',
    ]
    binaries = [f'Binaries/piano/Stage0/{n}/{n}.inf' for n in NATIVE_MODULES]
    fdf = '''# Host-only stage-0 diagnostic; NOT hardware validated.
[FD.SILICIUM_UEFI]
BaseAddress = $(FD_BASE)|gArmTokenSpaceGuid.PcdFdBaseAddress
Size = $(FD_SIZE)|gArmTokenSpaceGuid.PcdFdSize
ErasePolarity = 1
BlockSize = 0x1000
NumBlocks = $(FD_BLOCKS)
0x00000000|$(FD_SIZE)
gArmTokenSpaceGuid.PcdFvBaseAddress|gArmTokenSpaceGuid.PcdFvSize
FV = FVMAIN_COMPACT
[FV.FVMAIN_COMPACT]
FvAlignment = 8
ERASE_POLARITY = 1
MEMORY_MAPPED = TRUE
READ_ENABLED_CAP = TRUE
READ_STATUS = TRUE
  INF SiliciumPkg/Sec/Sec.inf
  FILE FV_IMAGE = 9E21FD93-9C72-4C15-8C4B-E77F1DB2D792 {
    SECTION GUIDED EE4E5898-3914-4259-9D6E-DC7BD79403CF PROCESSING_REQUIRED = TRUE {
      SECTION FV_IMAGE = FVMAIN
    }
  }
[FV.FVMAIN]
FvNameGuid = CD8D4C84-20AD-4073-8A28-3400FAE05941
BlockSize = 0x1000
NumBlocks = 0
FvAlignment = 8
ERASE_POLARITY = 1
MEMORY_MAPPED = TRUE
READ_ENABLED_CAP = TRUE
READ_STATUS = TRUE
  APRIORI DXE {
    INF MdeModulePkg/Core/Dxe/DxeMain.inf
    INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf
    INF Binaries/piano/Stage0/EnvDxeEnhanced/EnvDxeEnhanced.inf
  }
'''
    # Explicit architecture-first order. Keep PE images in the decompressed FV;
    # current Mu's runtime PE alignment differs from file alignment and cannot
    # be rebased for XIP by GenFv without changing the reference toolchain.
    early = [m for m in core if not any(x in m for x in ('/Console/', '/HiiDatabaseDxe/', '/PrintDxe/', '/SimpleFbDxe/', '/BdsDxe/'))]
    late = [m for m in core if m not in early]
    early.insert(2, binaries[0])
    ordered = early + binaries[1:] + late
    # Replace the minimal APRIORI list with the complete early driver order.
    start = fdf.index('  APRIORI DXE {')
    fdf = fdf[:start] + '  APRIORI DXE {\n' + ''.join('    INF ' + m + '\n' for m in ordered) + '  }\n'
    fdf += ''.join('  INF ' + module + '\n' for module in ordered)
    fdf += '\n!include SiliciumPkg/Common.fdf.inc\n'
    (args.source / 'piano.fdf').write_text(fdf)
    destination = args.workspace / 'Platforms/Xiaomi/pianoPkg'
    shutil.copytree(args.source, destination, dirs_exist_ok=True)
    config = '''# NOT hardware validated. Stage-0 only.
[boot_shim]
requires_kernel_header = true
[uefi_fd]
base = 0xA7100000
size = 0x300000
[boot_image_kernel]
kernel_compression = "gzip"
append_dtb = true
[boot_image]
header_version = 4
'''
    (args.source / 'piano.toml').write_text(config)
    (args.workspace / 'Resources/Configs/piano.toml').write_text(config)
    # Verify board identity. An overlay cannot serve as the appended base DTB.
    _, board_dtb, board_nodes = next(scan_fdts((args.capture / 'dtbo_a.img').read_bytes()))
    if 'Piano' not in ' '.join(text(board_nodes['/'].get('model', b''))):
        raise SystemExit('Unexpected board DTBO')
    (args.workspace / 'Resources/DTBs').mkdir(exist_ok=True)
    boot_dtb = args.workspace / 'Resources/DTBs/piano.dtb'
    boot_dtb.write_bytes((args.capture / 'live.dtb').read_bytes())
    live_nodes = parse_fdt(boot_dtb.read_bytes())
    # Remove snapshot pointers/seeds. ABL must set fresh handoff data on each boot.
    for path in ('/', '/chosen'):
        for prop in ('linux,initrd-start', 'linux,initrd-end', 'kaslr-seed', 'rng-seed',
                     'serial-number', 'bootargs', 'linux,elfcorehdr', 'linux,usable-memory-range'):
            if prop in live_nodes.get(path, {}):
                env = os.environ.copy()
                env['LD_LIBRARY_PATH'] = str(args.fdtput.resolve().parent / 'libfdt') + ':' + env.get('LD_LIBRARY_PATH', '')
                subprocess.run([str(args.fdtput), '-d', str(boot_dtb), path, prop], check=True, env=env)
    sanitized = parse_fdt(boot_dtb.read_bytes())
    if any(k in sanitized.get('/chosen', {}) for k in ('linux,initrd-start','linux,initrd-end','kaslr-seed','rng-seed')):
        raise SystemExit('Stale handoff properties remain in boot DTB')
    print(f'Prepared {destination}, {len(entries)} native memory regions, {len(binaries)} native modules')

if __name__ == '__main__':
    main()
