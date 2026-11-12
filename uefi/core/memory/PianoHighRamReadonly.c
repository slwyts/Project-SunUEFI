// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual first-device binding: AT/EFI/GCD only. No table or target read.
#include "PianoHighRamProbe.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#if PIANO_HIGH_RAM_PROBE_EXPERIMENT
STATIC UINT64 mMap[4096];
STATIC VOID EFIAPI Log(VOID *Context,CONST CHAR8 *Line) {(VOID)Context;DEBUG((DEBUG_WARN,"%a",Line));}
#endif
EFI_STATUS PianoProbeHighRamReadonly(VOID) {
#if !PIANO_HIGH_RAM_PROBE_EXPERIMENT
  return EFI_UNSUPPORTED;
#else
  PIANO_HIGH_RAM_ENV Env={.Boot=gBS,.Dxe=gDS,.MapBuffer=mMap,.MapCapacity=sizeof(mMap),
    .State=PianoHighRamArchitectureState,.AtRead=PianoHighRamArchitectureAtRead,.Log=Log,
    .RowBudget=1,.ExplicitEnable=TRUE};
  // Both ownership/regime attestations remain false, ReadTableWord/AtWrite and
  // MemoryAttribute provider remain NULL: no direct or indirect PTE walk.
  PIANO_HIGH_RAM_RESULT Result;
  EFI_STATUS Status=PianoHighRamProbe(&Env,&Result);
  STATIC CONST UINT64 Points[]={0xA20000000ULL,0xA3FFFF000ULL,0xA3FFFFFFFULL};
  for(UINTN I=0;I<ARRAY_SIZE(Points);++I) {
    UINT64 Par=1;EFI_MEMORY_DESCRIPTOR Efi={0};EFI_GCD_MEMORY_SPACE_DESCRIPTOR Gcd={0};
    EFI_STATUS At=PianoHighRamArchitectureAtRead(NULL,Points[I],&Par),Memory=EFI_NOT_FOUND;
    if(Result.MapStatus==EFI_SUCCESS && Result.DescriptorBytes>=sizeof(Efi) &&
       Result.MapBytes<=sizeof(mMap) && Result.MapBytes%Result.DescriptorBytes==0) {
      for(UINTN N=0;N<Result.MapBytes;N+=Result.DescriptorBytes) {
        EFI_MEMORY_DESCRIPTOR Row;CopyMem(&Row,(UINT8 *)mMap+N,sizeof(Row));
        if(Row.NumberOfPages<=MAX_UINT64/4096 && Row.PhysicalStart<=MAX_UINT64-Row.NumberOfPages*4096 &&
           Points[I]>=Row.PhysicalStart && Points[I]<Row.PhysicalStart+Row.NumberOfPages*4096) {
          Efi=Row;Memory=EFI_SUCCESS;break;
        }
      }
    }
    EFI_STATUS Space=gDS!=NULL && gDS->GetMemorySpaceDescriptor!=NULL?
      gDS->GetMemorySpaceDescriptor(Points[I],&Gcd):EFI_NOT_READY;
    DEBUG((DEBUG_WARN,"SUNUEFI_HIGH_RAM_POINT va=%lx at=%r par=%lx efi=%r type=%u base=%lx pages=%lx attrs=%lx gcd=%r gcd_type=%u gcd_base=%lx gcd_bytes=%lx gcd_attrs=%lx\n",
      Points[I],At,Par,Memory,Efi.Type,Efi.PhysicalStart,Efi.NumberOfPages,Efi.Attribute,
      Space,Gcd.GcdMemoryType,Gcd.BaseAddress,Gcd.Length,Gcd.Attributes));
  }
  DEBUG((DEBUG_WARN,"SUNUEFI_HIGH_RAM_READONLY_RESULT status=%r metadata_rows=%u covered_end=%lx reasons=%lx table_walk=0 target_dereferenced=0 ownership_verified=0 full_1GiB_verified=0 pattern=0\n",
    Status,Result.Rows,Result.CoveredEnd,Result.Reasons));return Status;
#endif
}
