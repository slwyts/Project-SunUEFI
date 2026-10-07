// SPDX-License-Identifier: BSD-2-Clause-Patent
// Firmware allocation/cache diagnostics only, no controller submission.
#include "PianoDma.h"
#include <Protocol/Cpu.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
VOID PianoDmaMemoryTest(VOID) {
#ifdef __aarch64__
  // Translation instructions do not read/write the target memory. This is an
  // opt-in diagnostic through existing DMA-probe profiles, not a RAM allocator.
  STATIC CONST UINT64 Candidates[]={0xA00000000ULL,0xA00001000ULL,0xA3FFFF000ULL,0xA3FFFFFFFULL};
  for(UINTN I=0;I<ARRAY_SIZE(Candidates);++I) {
    UINT64 Par;
    __asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1":"=r"(Par):"r"(Candidates[I]):"memory");
    DEBUG((DEBUG_WARN,"SUNUEFI_DRAM_AT_READ_ONLY va=%lx par=%lx translated=%u target_dereferenced=0 hardware_ram_owned_verified=0\n",
      Candidates[I],Par,(UINT32)((Par&1)==0)));
  }
#endif
  EFI_CPU_ARCH_PROTOCOL *Cpu=NULL;
  EFI_STATUS Status=gBS->LocateProtocol(&gEfiCpuArchProtocolGuid,NULL,(VOID **)&Cpu);
  if(EFI_ERROR(Status))return;
  DEBUG((DEBUG_WARN,"SUNUEFI_DMA_CPU_ALIGNMENT %u\n",Cpu->DmaBufferAlignment));
  UINTN Cache=MAX((UINTN)64,(UINTN)Cpu->DmaBufferAlignment);
  PIANO_DMA_DEVICE Device={.Name="ufs-memory-test",.StreamId=0x60,.AddressBits=32,.CacheLine=Cache};
  STATIC CONST UINTN Bytes[]={1024,128,4096};
  STATIC CONST UINTN Align[]={1024,128,4096};
  for(UINTN I=0;I<ARRAY_SIZE(Bytes);++I) {
    PIANO_DMA_BUFFER B;
    Status=PianoDmaAllocate(&Device,Bytes[I],Align[I],32,PianoDmaBidirectional,&B);
    DEBUG((DEBUG_WARN,"SUNUEFI_DMA_MEMORY_ALLOC case=%u %r\n",(UINT32)I,Status));
    if(EFI_ERROR(Status))return;
    Status=PianoDmaMap(&B);
    DEBUG((DEBUG_WARN,"SUNUEFI_DMA_NO_BACKEND_EXPECTED case=%u status=%r mapped=%u\n",(UINT32)I,Status,B.Mapped));
    if(Status!=EFI_NOT_READY || B.Mapped)return;
    Status=PianoDmaFree(&B);DEBUG((DEBUG_WARN,"SUNUEFI_DMA_MEMORY_FREE %r\n",Status));
    if(EFI_ERROR(Status))return;
  }
  DEBUG((DEBUG_WARN,"SUNUEFI_DMA_MEMORY_TEST_END physical_dma_submitted=0\n"));
}
