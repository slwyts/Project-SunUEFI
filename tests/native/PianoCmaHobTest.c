// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual Mu MemoryInitPei + PrePiHobLib builders; no actual MMU/hardware/allocator.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <PiPei.h>
#include <Library/BaseMemoryLib.h>
#include <Library/HobLib.h>
#include <Library/PeCoffLib.h>
#include <Library/PrePiHobListPointerLib.h>
#include <Library/MemoryMapLib.h>
#define GetMemoryMap PianoNativeGetMemoryMap
#include "../../uefi/platforms/pianoProbePkg/Library/MemoryMapLib/MemoryMapLib.c"
#undef GetMemoryMap
#include "../../upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Library/MemoryInitPeiLib/MemoryInitPei.c"
EFI_HOB_HANDOFF_INFO_TABLE *HobConstructor(VOID *,UINTN,VOID *,VOID *);
static union {UINT64 Align;UINT8 Bytes[65536];} arena;
static VOID *hob_list;
static EFI_MEMORY_REGION_DESCRIPTOR rows[128],original[128];static UINT8 count;
static ARM_MEMORY_REGION_DESCRIPTOR mapped[128];static UINTN mmu_calls,sort_calls,asserts,loops;
static BOOLEAN mmu_error;static jmp_buf jump;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN N){return FALSE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
VOID EFIAPI DebugPrint(UINTN N,CONST CHAR8 *F,...){ }
VOID EFIAPI DebugAssert(CONST CHAR8 *File,UINTN Line,CONST CHAR8 *Text){++asserts;longjmp(jump,1);}
VOID EFIAPI CpuDeadLoop(VOID){++loops;longjmp(jump,2);}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI PrePeiGetHobList(VOID){return hob_list;}EFI_STATUS EFIAPI PrePeiSetHobList(VOID *H){hob_list=H;return EFI_SUCCESS;}
VOID GetMemoryMap(EFI_MEMORY_REGION_DESCRIPTOR **Map,UINT8 *Count){*Map=rows;*Count=count;}
static SORT_COMPARE current_compare;
static int host_compare(const void *A,const void *B){INTN V=current_compare(A,B);return V<0?-1:V>0?1:0;}
VOID EFIAPI PerformQuickSort(VOID *Buffer,UINTN Count,UINTN Size,SORT_COMPARE Compare){++sort_calls;current_compare=Compare;qsort(Buffer,Count,Size,host_compare);}
EFI_STATUS EFIAPI ArmConfigureMmu(ARM_MEMORY_REGION_DESCRIPTOR *Table,VOID **Base,UINTN *Size){
 ++mmu_calls;memcpy(mapped,Table,sizeof(mapped));*Base=(VOID *)0x12345000;*Size=4096;return mmu_error?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
// ASan retains the real Hob.c PE/COFF protocol global; these unrelated entries
// are linked only and must never run in a memory/HOB test.
RETURN_STATUS EFIAPI PeCoffLoaderGetImageInfo(PE_COFF_LOADER_IMAGE_CONTEXT *C){assert(!"PE loader not part of CMA test");return RETURN_UNSUPPORTED;}
RETURN_STATUS EFIAPI PeCoffLoaderLoadImage(PE_COFF_LOADER_IMAGE_CONTEXT *C){assert(!"PE loader not part of CMA test");return RETURN_UNSUPPORTED;}
RETURN_STATUS EFIAPI PeCoffLoaderRelocateImage(PE_COFF_LOADER_IMAGE_CONTEXT *C){assert(!"PE loader not part of CMA test");return RETURN_UNSUPPORTED;}
RETURN_STATUS EFIAPI PeCoffLoaderUnloadImage(PE_COFF_LOADER_IMAGE_CONTEXT *C){assert(!"PE loader not part of CMA test");return RETURN_UNSUPPORTED;}
RETURN_STATUS EFIAPI PeCoffLoaderImageReadFromMemory(VOID *H,UINTN O,UINTN *N,VOID *B){assert(!"PE loader not part of CMA test");return RETURN_UNSUPPORTED;}
VOID EFIAPI PeCoffLoaderRelocateImageForRuntime(PHYSICAL_ADDRESS P,PHYSICAL_ADDRESS V,UINTN S,VOID *D){assert(!"PE loader not part of CMA test");}
static void setup(void){
 memset(&arena,0,sizeof(arena));hob_list=HobConstructor(arena.Bytes,sizeof(arena.Bytes),arena.Bytes,arena.Bytes+sizeof(arena.Bytes));
 EFI_MEMORY_REGION_DESCRIPTOR *Native;UINT8 N;PianoNativeGetMemoryMap(&Native,&N);assert(N+2<128);memcpy(rows,Native,N*sizeof(*rows));memcpy(original,Native,N*sizeof(*rows));count=N;
 memset(mapped,0,sizeof(mapped));mmu_calls=sort_calls=asserts=loops=0;mmu_error=FALSE;
}
static void check_hob(EFI_PHYSICAL_ADDRESS Base,UINT64 Length,EFI_MEMORY_TYPE Type,BOOLEAN Resource){
 unsigned resources=0,allocations=0;EFI_PEI_HOB_POINTERS P;P.Raw=hob_list;
 while(!END_OF_HOB_LIST(P)){
  if(GET_HOB_TYPE(P)==EFI_HOB_TYPE_RESOURCE_DESCRIPTOR && P.ResourceDescriptor->PhysicalStart==Base){assert(P.ResourceDescriptor->ResourceLength==Length && P.ResourceDescriptor->ResourceType==EFI_RESOURCE_SYSTEM_MEMORY && P.ResourceDescriptor->ResourceAttribute==SYS_MEM_CAP);++resources;}
  if(GET_HOB_TYPE(P)==EFI_HOB_TYPE_MEMORY_ALLOCATION && P.MemoryAllocation->AllocDescriptor.MemoryBaseAddress==Base){assert(P.MemoryAllocation->AllocDescriptor.MemoryLength==Length && P.MemoryAllocation->AllocDescriptor.MemoryType==Type);EFI_GUID Empty={0};assert(!memcmp(&P.MemoryAllocation->AllocDescriptor.Name,&Empty,sizeof(Empty)));for(unsigned I=0;I<sizeof(P.MemoryAllocation->AllocDescriptor.Reserved);++I)assert(P.MemoryAllocation->AllocDescriptor.Reserved[I]==0);++allocations;}
  P.Raw=GET_NEXT_HOB(P);
 }
 assert(resources==(unsigned)Resource && allocations==1);
}
static const ARM_MEMORY_REGION_DESCRIPTOR *mapped_at(UINT64 Base){for(unsigned I=0;I<128 && mapped[I].Length;++I)if(mapped[I].PhysicalBase==Base)return &mapped[I];return NULL;}
static void verify_original(void){
 EFI_MEMORY_REGION_DESCRIPTOR *Native;UINT8 N;PianoNativeGetMemoryMap(&Native,&N);
 assert(!memcmp(Native,original,N*sizeof(*Native)));
 for(unsigned I=0;I<N;++I){if(Native[I].HobOption==HobOnlyNoCacheSetting)continue;const ARM_MEMORY_REGION_DESCRIPTOR *M=mapped_at(Native[I].Address);assert(M && M->VirtualBase==Native[I].Address && M->Length==Native[I].Length && M->Attributes==Native[I].ArmAttributes);}
}
