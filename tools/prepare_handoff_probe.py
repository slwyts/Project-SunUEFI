#!/usr/bin/env python3
"""Build a separate diagnostic target; preserve the previous piano source."""
from pathlib import Path
import shutil

def main():
    root = Path(__file__).resolve().parent.parent
    src = root / 'platforms/pianoPkg'
    dst = root / 'platforms/pianoProbePkg'
    shutil.copytree(src, dst, dirs_exist_ok=True)
    for path in list(dst.rglob('*')):
        if path.is_file() and path.suffix in ('.py', '.dsc', '.fdf', '.inf', '.dec', '.c'):
            path.write_text(path.read_text().replace('pianoPkg', 'pianoProbePkg').replace('piano.dsc', 'pianoProbe.dsc').replace('piano.fdf', 'pianoProbe.fdf'))
    for suffix in ('dsc', 'fdf', 'dec'):
        (dst / f'piano.{suffix}').rename(dst / f'pianoProbe.{suffix}')
    dsc = dst / 'pianoProbe.dsc'
    dsc.write_text(dsc.read_text().replace('PLATFORM_NAME                  = piano', 'PLATFORM_NAME                  = pianoProbe'))
    # Duplicate the existing framebuffer serial backend and append its output
    # to the Linux-compatible reserved-RAM console; no storage backend.
    serial = dst/'Library/RamLogSerialPortLib'
    reference = root/'upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib'
    shutil.copytree(reference,serial,dirs_exist_ok=True)
    shutil.copyfile(root/'bootprofiles/handoff/RamLog.c',serial/'RamLog.c')
    inf = serial/'FrameBufferSerialPortLib.inf'
    inf.write_text(inf.read_text().replace('  FrameBufferSerialPortLib.c','  FrameBufferSerialPortLib.c\n  RamLog.c'))
    code = serial/'FrameBufferSerialPortLib.c'
    value = code.read_text()
    # Keep declarations after UEFI headers, before the first function.
    value = value.replace('#include "FrameBuffer.h"','#include "FrameBuffer.h"\nVOID RamLogWrite (IN CONST UINT8 *Buffer, IN UINTN Length);')
    needle = '  // Get Frame Buffer Memory\n  Status = GetFrameBufferMemory (&FbBase, &FbLength);'
    if needle not in value: raise SystemExit('Serial backend source changed')
    value = value.replace(needle,'  RamLogWrite (Buffer, NumberOfBytes);\n'+needle,1)
    code.write_text(value)
    dsc.write_text(dsc.read_text()+'\n[LibraryClasses]\n  SerialPortLib|pianoProbePkg/Library/RamLogSerialPortLib/FrameBufferSerialPortLib.inf\n')
    # The raw video shows ScmDxeCompat fails and TzDxeLA asserts before BDS.
    # This diagnostic needs only EnvDxeEnhanced among native binaries. Do not
    # bring up preboot secure applications or their unrelated dependencies.
    fdf = dst / 'pianoProbe.fdf'
    lines = fdf.read_text().splitlines()
    lines = [line for line in lines if 'Binaries/piano/Stage0/' not in line
             or '/EnvDxeEnhanced/' in line]
    fdf.write_text('\n'.join(lines) + '\n')
    mem = dst / 'Library/MemoryMapLib/MemoryMapLib.c'
    data = mem.read_text()
    old = '{"UEFI_RESV", 0xA7ED9000, 0x127000, AddMem, 0, 0x703C07, 4, WRITE_BACK_XN}'
    new = '{"UEFI_RESV", 0xA7ED9000, 0x126000, AddMem, 0, 0x703C07, 4, WRITE_BACK_XN},\n  {"BootHandoff", 0xA7FFF000, 0x1000, AddMem, 0, 0x703C07, 4, WRITE_BACK_XN}'
    if old not in data:
        raise SystemExit('Native UEFI reserved memory layout changed')
    mem.write_text(data.replace(old, new))
    lib = dst / 'Library/Stage0BootManagerLib'
    shutil.copyfile(root / 'bootprofiles/handoff/Probe.c', lib / 'Probe.c')
    inf = lib / 'Stage0BootManagerLib.inf'
    data = inf.read_text().replace('  Stage0BootManagerLib.c', '  Stage0BootManagerLib.c\n  Probe.c')
    data = data.replace('  MsCorePkg/MsCorePkg.dec', '  MsCorePkg/MsCorePkg.dec\n  SiliciumPkg/SiliciumPkg.dec')
    data = data.replace('  DebugLib', '  DebugLib\n  FdtLib\n  MemoryMapLib\n  BaseLib')
    inf.write_text(data)
    c = lib / 'Stage0BootManagerLib.c'
    data = c.read_text().replace('STATIC EFI_EVENT mReturnToAndroidEvent;', 'VOID PrintBootHandoff (VOID);\nSTATIC EFI_EVENT mReturnToAndroidEvent;')
    data = data.replace('  DEBUG ((DEBUG_WARN, "PIANO_STAGE0_CONSOLE_READY', '  PrintBootHandoff ();\n  DEBUG ((DEBUG_WARN, "PIANO_STAGE0_CONSOLE_READY')
    data = data.replace('  __asm__ volatile ("mrs %0, CurrentEL"', '  DEBUG ((DEBUG_WARN, "SUNUEFI_RAMLOG_BEGIN\\n"));\n  __asm__ volatile ("mrs %0, CurrentEL"')
    c.write_text(data)
    shutil.copytree(dst, root / 'upstream/Mu-Silicium/Platforms/Xiaomi/pianoProbePkg', dirs_exist_ok=True)
    print('Separate minimal pianoProbePkg prepared; previous pianoPkg remains unchanged')

if __name__ == '__main__':
    main()
