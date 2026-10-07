// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/PianoIoPageTable.c"
#include "../../uefi/core/PianoOwnedSmmu.c"
static unsigned fsr_writes;static UINT32 fsr_clear_value;
static unsigned sync_calls;static int sync_fail;
static unsigned detaches,destroys,releases,exit_retains;static int retained_stream,changed_other;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){ }
UINTN EFIAPI AsciiSPrint(CHAR8 *Buffer,UINTN Size,CONST CHAR8 *Format,...){abort();return 0;}
UINT32 EFIAPI MmioRead32(UINTN Address){abort();return 0;}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value){assert(Address==0x15080058);++fsr_writes;fsr_clear_value=Value;return Value;}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID *EFIAPI WriteBackDataCacheRange(VOID *P,UINTN N){return P;}
VOID EFIAPI MemoryFence(VOID){ }
static UINT32 sync(VOID *Domain){assert(Domain==(void *)123);++sync_calls;return sync_fail?1:0;}
static UINT32 detach(VOID *Domain,CONST CHAR8 *Name,UINT32 Arid,UINT32 Flags){assert(Domain==(void *)123 && !strcmp(Name,"UFS_MEM"));++detaches;return 0;}
static UINT32 destroy(VOID *Domain){assert(Domain==(void *)123);++destroys;return 0;}
EFI_STATUS PianoDmaFree(PIANO_DMA_BUFFER *B){assert(!B->Quarantined);++releases;B->Signature=0;return EFI_SUCCESS;}
EFI_STATUS PianoDmaRetainForExit(PIANO_DMA_BUFFER *B){
  if(B->MemoryType!=EfiReservedMemoryType || B->Quarantined)return EFI_ACCESS_DENIED;
  ++exit_retains;B->ExitRetained=TRUE;return EFI_SUCCESS;
}
EFI_STATUS PianoSmmuCapture(CONST VOID *Fdt,CONST CHAR8 *Phase,PIANO_SMMU_SNAPSHOT *S){
  memset(S,0,sizeof(*S));S->Valid=TRUE;S->Groups=2;S->Device[0].Present=retained_stream;
  S->RawSmr[1]=changed_other?124:123;return EFI_SUCCESS;
}
int main(void){
  assert(OFFSET_OF(DOMAIN_CONFIG,Mair0)==0x18 && OFFSET_OF(DOMAIN_CONFIG,Sctlr)==0x20 &&
         OFFSET_OF(DOMAIN_CONFIG,Tcr)==0x24 && OFFSET_OF(DOMAIN_CONFIG,Tcr2)==0x28);
  PIANO_OWNED_SMMU *c=calloc(1,sizeof(*c));assert(c);void *memory=NULL;
  assert(posix_memalign(&memory,4096,PIANO_IO_PT_BYTES)==0);
  assert(PianoIoPageTableInit(&c->PageTable,memory,0xd7000000,PIANO_IO_PT_BYTES)==EFI_SUCCESS);
  c->TableMemory.Cpu=memory;c->TableMemory.ReservedBytes=PIANO_IO_PT_BYTES;
  UINTN functions[15]={0};functions[5]=(UINTN)sync;c->Api=functions;c->Domain=(void *)123;c->Attached=c->Verified=TRUE;
  PIANO_DMA_DEVICE d={.Name="ufs",.StreamId=0x60,.Context=c};EFI_PHYSICAL_ADDRESS a,b,pa;VOID *x=NULL,*y=NULL;
  assert(Map(&d,0x1000000000ULL,4096,PianoDmaToDevice,4096,&a,&x)==EFI_BAD_BUFFER_SIZE);
  assert(Map(&d,0xc0000000,4096,PianoDmaToDevice,0,&a,&x)==EFI_INVALID_PARAMETER);
  assert(Map(&d,0xc0000000,PIANO_IOVA_BYTES+4096,PianoDmaToDevice,4096,&a,&x)==EFI_INVALID_PARAMETER);
  assert(Map(&d,0xc0000000,4096,PianoDmaBidirectional,4096,&a,&x)==EFI_SUCCESS && a==PIANO_IOVA_BASE);
  assert(Map(&d,0xc1000000,8192,PianoDmaToDevice,65536,&b,&y)==EFI_SUCCESS && !(b&65535));
  assert(PianoIoPageTableTranslate(&c->PageTable,b,FALSE,&pa)==EFI_SUCCESS && pa==0xc1000000);
  assert(PianoIoPageTableTranslate(&c->PageTable,b,TRUE,&pa)==EFI_ACCESS_DENIED);
  assert(Unmap(&d,x)==EFI_SUCCESS && Unmap(&d,x)==EFI_INVALID_PARAMETER);
  assert(Unmap(&d,y)==EFI_SUCCESS);
  sync_fail=1;x=NULL;
  assert(Map(&d,0xc0000000,4096,PianoDmaBidirectional,4096,&a,&x)==EFI_DEVICE_ERROR && x!=NULL);
  assert(c->Mapping[0].Used); // A failed rollback must not silently recycle IOVA or RAM.
  assert(PianoOwnedSmmuClose(c)==EFI_ACCESS_DENIED);
  memset(c->Mapping,0,sizeof(c->Mapping));functions[3]=(UINTN)detach;functions[1]=(UINTN)destroy;
  c->Before.Groups=2;c->Before.RawSmr[1]=123;c->After.Device[0].StreamIndex=0;c->TableMemory.Signature=123;
  retained_stream=1;
  assert(PianoOwnedSmmuClose(c)==EFI_COMPROMISED_DATA && c->TableMemory.Quarantined && !destroys && !releases);
  assert(PianoOwnedSmmuClose(c)==EFI_ACCESS_DENIED && detaches==1);
  retained_stream=0;c->TableMemory.Quarantined=FALSE;c->Attached=TRUE;
  assert(PianoOwnedSmmuClose(c)==EFI_SUCCESS && destroys==1 && releases==1);
  c->Domain=(void *)123;c->Attached=TRUE;c->TableMemory.Signature=123;changed_other=1;
  assert(PianoOwnedSmmuClose(c)==EFI_COMPROMISED_DATA && c->TableMemory.Quarantined && releases==1);
  c->TableMemory.Quarantined=FALSE;c->Attached=c->Verified=TRUE;c->TableMemory.Device=&d;
  c->TableMemory.MemoryType=EfiBootServicesData;
  assert(PianoOwnedSmmuRetainForExit(c)==EFI_ACCESS_DENIED && !c->ExitRetained);
  c->TableMemory.MemoryType=EfiReservedMemoryType;
  unsigned old_detaches=detaches,old_destroys=destroys,old_releases=releases;
  assert(PianoOwnedSmmuRetainForExit(c)==EFI_SUCCESS && c->ExitRetained && exit_retains==1);
  assert(PianoOwnedSmmuClose(c)==EFI_ACCESS_DENIED && detaches==old_detaches && destroys==old_destroys && releases==old_releases);
  assert(Map(&d,0xc0000000,4096,PianoDmaFromDevice,4096,&a,&x)==EFI_NOT_READY);
  assert(Unmap(&d,x)==EFI_ACCESS_DENIED);
  c->DeviceIndex=0;c->After.Base=0x15000000;c->After.ContextBase=0x80000;c->After.PageShift=12;c->After.Device[0].ContextBank=0;
  c->After.Device[0].Fsr=0x400;ClearOwnedStickyFault(c);assert(!fsr_writes && c->After.Device[0].Fsr==0x400);
  c->After.Device[0].Fsr=0x402;ClearOwnedStickyFault(c);assert(fsr_writes==1 && fsr_clear_value==2 && c->After.Device[0].Fsr==0x402);
  free(memory);free(c);puts("Owned SMMU backend: nonidentity arena, alignment, per-direction PTEs, unmap and retained rollback failure passed.");return 0;
}
