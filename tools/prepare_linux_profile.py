#!/usr/bin/env python3
"""Create a separate RAM Linux loader target from the minimal piano diagnostic."""
from pathlib import Path
import shutil
import argparse
import hashlib
import json
from piano_cma_contract import create as create_cma_candidate

def clear_generated_cma_manifest(path):
    if not path.exists(): return
    try: metadata=json.loads(path.read_text())
    except (ValueError,OSError): raise SystemExit('Unknown derived CMA snapshot; refusing to remove it')
    if metadata.get('status')!='HOST_CMA_CANDIDATE_NOT_HARDWARE_VERIFIED' or metadata.get('scope')!='exactly_two_fixed_reusable_CMA_pools':
        raise SystemExit('Unknown derived CMA snapshot; refusing to remove it')
    path.unlink()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--raw', action='store_true')
    ap.add_argument('--cma-contract-candidate', action='store_true',
        help='Opt-in pinned two-pool CMA occupied LoaderData/WB-XP candidate; hardware ownership unverified')
    args = ap.parse_args()
    root = Path(__file__).resolve().parent.parent
    candidate = None
    if args.cma_contract_candidate:
        if args.raw: ap.error('--cma-contract-candidate is an EFI contract diagnostic and cannot be combined with --raw')
        # Complete pin/DT/native/Mu/HOB preflight before creating a derived target.
        candidate = create_cma_candidate(root)
    src = root / 'uefi/platforms/pianoProbePkg'
    dst = root / 'uefi/platforms/pianoLinuxPkg'
    shutil.copytree(src, dst, dirs_exist_ok=True)
    for f in list(dst.rglob('*')):
        if f.is_file() and f.suffix in ('.c','.h','.inf','.dsc','.dec','.fdf','.py'):
            f.write_text(f.read_text().replace('pianoProbe', 'pianoLinux'))
    for suffix in ('dsc','dec','fdf'):
        (dst/f'pianoProbe.{suffix}').rename(dst/f'pianoLinux.{suffix}')
    app = dst/'Applications/LinuxRamBoot'
    app.mkdir(parents=True, exist_ok=True)
    for name in ('LinuxRamBoot.c','LinuxRamBoot.inf'):
        shutil.copyfile(root/'uefi/components/linux-loader'/name, app/name)
    shutil.copyfile(root/'uefi/core/PianoFaultRecovery.c',app/'PianoFaultRecovery.c')
    for name in ('PianoEfiHandoffTrace.c','PianoEfiHandoffTrace.h'):
        shutil.copyfile(root/'uefi/components/linux-loader'/name,app/name)
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
    if candidate is not None:
        table=dst/'Library/MemoryMapLib/MemoryMapLib.c'
        source,metadata=candidate
        if hashlib.sha256(table.read_bytes()).hexdigest()!=metadata['original_native_sha256']:
            raise SystemExit('Derived native table differs from pinned source before CMA insertion')
        table.write_bytes(source)
        if hashlib.sha256(table.read_bytes()).hexdigest()!=metadata['candidate_table_sha256']:
            raise SystemExit('Derived CMA descriptor snapshot mismatch')
        (dst/'cma-contract-candidate.json').write_text(json.dumps(metadata,indent=2)+'\n')
    else:
        clear_generated_cma_manifest(dst/'cma-contract-candidate.json')
    shutil.copytree(dst, root/'upstream/Mu-Silicium/Platforms/Xiaomi/pianoLinuxPkg', dirs_exist_ok=True)
    if candidate is None:
        clear_generated_cma_manifest(root/'upstream/Mu-Silicium/Platforms/Xiaomi/pianoLinuxPkg/cma-contract-candidate.json')
    print('Separate pianoLinuxPkg prepared; minimal diagnostic source retained'+
          ('; pinned CMA candidate opt-in, not hardware verified' if candidate is not None else ''))

if __name__ == '__main__': main()
