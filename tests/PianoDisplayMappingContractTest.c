// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real Mu MemoryPeim/AddHob and actual ArmMmu attribute conversion; no target.
#define main PianoUnusedCmaMain
#include "PianoCmaHobTest.c"
#undef main
#include <AArch64/AArch64.h>
#include <AArch64/AArch64Mmu.h>
#include <PiDxe.h>
#define GetMemoryMap PianoDisplayOriginalGetMemoryMap
#define gMemoryDescriptor PianoDisplayOriginalDescriptors
#include "PianoDisplayOriginal.c"
#undef GetMemoryMap
#undef gMemoryDescriptor
#define GetMemoryMap PianoDisplayCandidateGetMemoryMap
#define gMemoryDescriptor PianoDisplayCandidateDescriptors
#include "PianoDisplayCandidate.c"
#undef GetMemoryMap
#undef gMemoryDescriptor
UINTN ArmReadCurrentEL(VOID){return AARCH64_EL1;}
UINTN ArmReadHcr(VOID){return 0;}
#include "PianoDisplayActualArmMmu.h"
#include "PianoDisplayActualGcd.h"
static CONST UINT64 bases[]={0x100000,0xae00000,0xaf00000};
static CONST UINT64 lengths[]={0x1f5000,0x94000,0x20000};
static VOID CheckHobs(VOID){
  UINTN resources[3]={0},allocations[3]={0};EFI_PEI_HOB_POINTERS H;H.Raw=hob_list;
  while(!END_OF_HOB_LIST(H)){
    for(UINTN I=0;I<3;++I){
      if(GET_HOB_TYPE(H)==EFI_HOB_TYPE_RESOURCE_DESCRIPTOR&&H.ResourceDescriptor->PhysicalStart==bases[I]){
        assert(H.ResourceDescriptor->ResourceLength==lengths[I]&&H.ResourceDescriptor->ResourceType==EFI_RESOURCE_MEMORY_MAPPED_IO&&
          H.ResourceDescriptor->ResourceAttribute==EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE);
        assert(ActualHobMmioType(H.ResourceDescriptor->ResourceType)==EfiGcdMemoryTypeMemoryMappedIo);
        assert(CoreConvertResourceDescriptorHobAttributesToCapabilities(EfiGcdMemoryTypeMemoryMappedIo,H.ResourceDescriptor->ResourceAttribute)==EFI_MEMORY_UC);++resources[I];
      }
      if(GET_HOB_TYPE(H)==EFI_HOB_TYPE_MEMORY_ALLOCATION&&H.MemoryAllocation->AllocDescriptor.MemoryBaseAddress==bases[I])++allocations[I];
    }
    H.Raw=GET_NEXT_HOB(H);
  }
  for(UINTN I=0;I<3;++I)assert(resources[I]==1&&!allocations[I]);
}
int main(VOID){
  assert(AddDev==2&&MMAP_IO==1&&EfiMemoryMappedIO==11&&EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE==0x400&&ARM_MEMORY_REGION_ATTRIBUTE_DEVICE==6);
  EFI_MEMORY_REGION_DESCRIPTOR *Native,*Generated;UINT8 N,C;
  PianoDisplayOriginalGetMemoryMap(&Native,&N);PianoDisplayCandidateGetMemoryMap(&Generated,&C);
  assert(N==49&&C==52&&!memcmp(Native,Generated,N*sizeof(*Native)));
  for(UINTN I=0;I<3;++I){EFI_MEMORY_REGION_DESCRIPTOR *R=&Generated[N+I];
    assert(R->Address==bases[I]&&R->Length==lengths[I]&&R->HobOption==AddDev&&R->ResourceType==MMAP_IO&&
      R->ResourceAttribute==EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE&&R->MemoryType==EfiMemoryMappedIO&&R->ArmAttributes==ARM_MEMORY_REGION_ATTRIBUTE_DEVICE);
  }
  setup(FALSE);memcpy(rows,Generated,C*sizeof(*rows));count=C;
  assert(MemoryPeim(0,0)==EFI_SUCCESS&&mmu_calls==1);CheckHobs();
  for(UINTN I=0;I<3;++I){CONST ARM_MEMORY_REGION_DESCRIPTOR *M=mapped_at(bases[I]);
    assert(M&&M->PhysicalBase==M->VirtualBase&&M->Length==lengths[I]&&M->Attributes==ARM_MEMORY_REGION_ATTRIBUTE_DEVICE);
    UINT64 Attr=ArmMemoryAttributeToPageAttribute(M->Attributes);
    assert(Attr==(TT_ATTR_INDX_DEVICE_MEMORY|TT_UXN_MASK|TT_PXN_MASK));
    assert(PageAttributeToGcdAttribute(Attr|TT_AF)==(EFI_MEMORY_UC|EFI_MEMORY_XP));
    assert((Attr&TT_ATTR_INDX_MASK)==TT_ATTR_INDX_DEVICE_MEMORY&&!(Attr&TT_AP_NO_RO));
  }
  for(UINTN I=0;I<N;++I){if(Native[I].HobOption==HobOnlyNoCacheSetting)continue;
    CONST ARM_MEMORY_REGION_DESCRIPTOR *M=mapped_at(Native[I].Address);
    assert(M&&M->Length==Native[I].Length&&M->Attributes==Native[I].ArmAttributes);
  }
  assert(mapped_at(0xfc800000)->Attributes==ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH);
  // No new System RAM, Conventional allocation or high-DRAM descriptor exists.
  for(UINTN I=N;I<C;++I)assert(Generated[I].ResourceType!=EFI_RESOURCE_SYSTEM_MEMORY&&Generated[I].MemoryType!=EfiConventionalMemory&&Generated[I].Address<0x80000000);
  puts("Actual Mu MemoryPeim+HobLib and actual ArmMmu DEVICE helper: product49 unchanged; three MMIO UC resource HOBs, no allocation HOB, identity DEVICE/XN inputs, EL1 PXN/UXN passed; live GCD/AT/registers unverified");return 0;
}
