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
#include "../platforms/pianoProbePkg/Library/MemoryMapLib/MemoryMapLib.c"
#undef GetMemoryMap
#define GetMemoryMap PianoCandidateGetMemoryMap
#define gMemoryDescriptor PianoCandidateDescriptors
#include "../artifacts/dram/cma-contract-candidate/MemoryMapLib.c"
#undef gMemoryDescriptor
#undef GetMemoryMap
#include "../upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Library/MemoryInitPeiLib/MemoryInitPei.c"
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
static void setup(BOOLEAN Candidate){
 memset(&arena,0,sizeof(arena));hob_list=HobConstructor(arena.Bytes,sizeof(arena.Bytes),arena.Bytes,arena.Bytes+sizeof(arena.Bytes));
 EFI_MEMORY_REGION_DESCRIPTOR *Native;UINT8 N;PianoNativeGetMemoryMap(&Native,&N);assert(N+2<128);memcpy(rows,Native,N*sizeof(*rows));memcpy(original,Native,N*sizeof(*rows));count=N;
 if(Candidate){EFI_MEMORY_REGION_DESCRIPTOR *Generated;UINT8 C;PianoCandidateGetMemoryMap(&Generated,&C);assert(C==N+2);memcpy(rows,Generated,C*sizeof(*rows));count=C;}
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
int main(void){
 assert(sizeof(EFI_PHYSICAL_ADDRESS)==8 && EfiLoaderData==2 && WRITE_BACK_XN==ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK && WRITE_BACK_XN!=ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP);
 setup(TRUE);assert(MemoryPeim(0,0)==EFI_SUCCESS && mmu_calls==1 && sort_calls==1 && !asserts && !loops);
 check_hob(0x82800000,0x02000000,EfiLoaderData,TRUE);check_hob(0xF3800000,0x05000000,EfiLoaderData,TRUE);
 assert(mapped_at(0x82800000)->Attributes==ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP && mapped_at(0xF3800000)->Attributes==ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP);
 verify_original();
 // All candidate coverage has an actual Type2 allocation HOB; no Conventional
 // allocation overlaps it. This is HOB reservation, not a simulated allocator.
 EFI_PEI_HOB_POINTERS P;P.Raw=hob_list;while(!END_OF_HOB_LIST(P)){if(GET_HOB_TYPE(P)==EFI_HOB_TYPE_MEMORY_ALLOCATION && P.MemoryAllocation->AllocDescriptor.MemoryType==EfiConventionalMemory){UINT64 B=P.MemoryAllocation->AllocDescriptor.MemoryBaseAddress,E=B+P.MemoryAllocation->AllocDescriptor.MemoryLength;assert(!(B<0x84800000 && E>0x82800000) && !(B<0xF8800000 && E>0xF3800000));}P.Raw=GET_NEXT_HOB(P);}
 setup(FALSE);assert(MemoryPeim(0,0)==EFI_SUCCESS && !mapped_at(0x82800000) && !mapped_at(0xF3800000));verify_original();
 setup(FALSE);EFI_MEMORY_REGION_DESCRIPTOR R={"HobOnly",0x82800000,0x2000000,HobOnlyNoCacheSetting,SYS_MEM,SYS_MEM_CAP,EfiLoaderData,ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP};rows[count++]=R;assert(MemoryPeim(0,0)==EFI_SUCCESS && !mapped_at(R.Address));check_hob(R.Address,R.Length,EfiLoaderData,TRUE);
 setup(FALSE);R.HobOption=AllocOnly;AddHob(R);check_hob(R.Address,R.Length,EfiLoaderData,FALSE);
 setup(FALSE);R.HobOption=AddDev;R.ResourceType=EFI_RESOURCE_MEMORY_RESERVED;R.MemoryType=EfiReservedMemoryType;AddHob(R);P.Raw=hob_list;unsigned allocs=0;while(!END_OF_HOB_LIST(P)){if(GET_HOB_TYPE(P)==EFI_HOB_TYPE_MEMORY_ALLOCATION)++allocs;P.Raw=GET_NEXT_HOB(P);}assert(allocs==0);
 setup(TRUE);mmu_error=TRUE;assert(MemoryPeim(0,0)==EFI_DEVICE_ERROR && mmu_calls==1);check_hob(0x82800000,0x02000000,EfiLoaderData,TRUE);
 setup(TRUE);rows[count]=rows[count-1];++count;int result=setjmp(jump);if(result==0){MemoryPeim(0,0);assert(!"Overlap must fail before MMU");}assert(result==2 && loops==1 && !mmu_calls);
 setup(FALSE);result=setjmp(jump);if(result==0){BuildMemoryAllocationHob(0x82800001,4096,EfiLoaderData);assert(!"Misaligned allocation must assert");}assert(result==1 && asserts==1);
 puts("Real Mu MemoryPeim/AddHob + real PrePiHobLib: two Type2 system-memory allocation HOBs, XP MMU descriptors, unchanged native cache, no Conventional candidate overlap, HOB-only/alloc-only, overlap/alignment/MMU-error paths passed; no real allocator/MMU/device validation.");
}
