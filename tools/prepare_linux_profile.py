#!/usr/bin/env python3
"""Create a separate RAM Linux loader target from the minimal piano diagnostic."""
from pathlib import Path
import shutil
import argparse

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--raw', action='store_true')
    args = ap.parse_args()
    root = Path(__file__).resolve().parent.parent
    src = root / 'platforms/pianoProbePkg'
    dst = root / 'platforms/pianoLinuxPkg'
    shutil.copytree(src, dst, dirs_exist_ok=True)
    for f in list(dst.rglob('*')):
        if f.is_file() and f.suffix in ('.c','.h','.inf','.dsc','.dec','.fdf','.py'):
            f.write_text(f.read_text().replace('pianoProbe', 'pianoLinux'))
    for suffix in ('dsc','dec','fdf'):
        (dst/f'pianoProbe.{suffix}').rename(dst/f'pianoLinux.{suffix}')
    app = dst/'Applications/LinuxRamBoot'
    app.mkdir(parents=True, exist_ok=True)
    for name in ('LinuxRamBoot.c','LinuxRamBoot.inf'):
        shutil.copyfile(root/'bootprofiles/linux-ram'/name, app/name)
    if args.raw:
        f=app/'LinuxRamBoot.c';f.write_text('#define SUNUEFI_RAW_HANDOFF 1\n'+f.read_text())
    dsc = dst/'pianoLinux.dsc'
    data = dsc.read_text() + '''
[Components]
  pianoLinuxPkg/Applications/LinuxRamBoot/LinuxRamBoot.inf {
    <LibraryClasses>
      BaseCryptLib|OpensslPkg/Library/BaseCryptLib/BaseCryptLib.inf
      OpensslLib|OpensslPkg/Library/OpensslLib/OpensslLib.inf
      IntrinsicLib|CryptoPkg/Library/IntrinsicLib/IntrinsicLib.inf
      RngLib|MdePkg/Library/BaseRngLibNull/BaseRngLibNull.inf
  }
'''
    dsc.write_text(data)
    fdf = dst/'pianoLinux.fdf'
    fdf.write_text(fdf.read_text().replace('!include SiliciumPkg/Common.fdf.inc',
        '  INF pianoLinuxPkg/Applications/LinuxRamBoot/LinuxRamBoot.inf\n!include SiliciumPkg/Common.fdf.inc'))
    lib = dst/'Library/Stage0BootManagerLib'
    inf = lib/'Stage0BootManagerLib.inf'
    inf.write_text(inf.read_text().replace('  FdtLib', '  FdtLib\n  DxeServicesLib\n  MemoryAllocationLib'))
    c = lib/'Stage0BootManagerLib.c'
    data = c.read_text().replace('#include <Library/DebugLib.h>',
        '#include <Library/DebugLib.h>\n#include <Library/DxeServicesLib.h>\n#include <Library/MemoryAllocationLib.h>')
    old = 'VOID EFIAPI DeviceBootManagerUnableToBoot (VOID) { }'
    new = '''VOID EFIAPI DeviceBootManagerUnableToBoot (VOID)
{
  EFI_GUID AppGuid = {0x9EAD13AC,0xA4EC,0x47D4,{0xB9,0x9E,0x2D,0x17,0x97,0x9D,0xC8,0xEF}};
  VOID *Source = NULL;
  UINTN SourceSize = 0;
  EFI_HANDLE Handle = NULL;
  EFI_STATUS Status;
  EFI_EVENT Hold;
  UINTN Index;
  if (gST->ConOut != NULL) { gST->ConOut->ClearScreen (gST->ConOut); }
  Status = GetSectionFromAnyFv (&AppGuid, EFI_SECTION_PE32, 0, &Source, &SourceSize);
  if (!EFI_ERROR (Status)) {
    Status = gBS->LoadImage (FALSE, gImageHandle, NULL, Source, SourceSize, &Handle);
    FreePool (Source);
    if (!EFI_ERROR (Status)) {
      Status = gBS->StartImage (Handle, NULL, NULL);
      gBS->UnloadImage (Handle);
    }
  }
  Print (L"Linux RAM loader returned: %r\\r\\n", Status);
  DEBUG ((DEBUG_WARN, "PIANO_LINUX_LOADER_RETURN %r\\n", Status));
  // Preserve diagnostic output instead of the parent's 10-second shutdown page.
  if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER, TPL_APPLICATION, NULL, NULL, &Hold))) {
    if (!EFI_ERROR (gBS->SetTimer (Hold, TimerRelative, 55ULL * 10000000ULL))) {
      gBS->WaitForEvent (1, &Hold, &Index);
    }
    gBS->CloseEvent (Hold);
  }
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
}'''
    if old not in data: raise SystemExit('Unexpected diagnostic hook')
    c.write_text(data.replace(old,new))
    shutil.copytree(dst, root/'upstream/Mu-Silicium/Platforms/Xiaomi/pianoLinuxPkg', dirs_exist_ok=True)
    print('Separate pianoLinuxPkg prepared; minimal diagnostic source retained')

if __name__ == '__main__': main()
