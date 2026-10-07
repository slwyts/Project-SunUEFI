// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse the frozen real Mu HOB harness; add the separate high prototype only.
#define main PianoFrozenCmaTests
#include "PianoCmaHobTest.c"
#undef main
#define GetMemoryMap PianoHighGetMemoryMap
#define gMemoryDescriptor PianoHighDescriptors
#include "../../artifacts/dram/high-occupied-prototype/MemoryMapLib.c"
#undef gMemoryDescriptor
#undef GetMemoryMap

int main(void) {
  const UINT64 Begin=0xA00000000ULL,Length=0x40000000ULL,End=Begin+Length;
  EFI_MEMORY_REGION_DESCRIPTOR *High,*Cma;
  UINT8 HighCount,CmaCount;
  PianoFrozenCmaTests(); // Frozen main has C's implicit main return; do not use renamed return value.
  PianoHighGetMemoryMap(&High,&HighCount);PianoCandidateGetMemoryMap(&Cma,&CmaCount);
  assert(HighCount==45 && CmaCount==44);
  for(unsigned I=0;I<CmaCount;I++) {
    assert(!strcmp(High[I].Name,Cma[I].Name));
    assert(High[I].Address==Cma[I].Address && High[I].Length==Cma[I].Length);
    assert(High[I].HobOption==Cma[I].HobOption && High[I].ResourceType==Cma[I].ResourceType);
    assert(High[I].ResourceAttribute==Cma[I].ResourceAttribute && High[I].MemoryType==Cma[I].MemoryType);
    assert(High[I].ArmAttributes==Cma[I].ArmAttributes);
  }
  assert(High[44].Address==Begin && High[44].Length==Length && High[44].MemoryType==EfiLoaderData);
  setup(TRUE);memcpy(rows,High,HighCount*sizeof(*rows));count=HighCount;
  assert(MemoryPeim(0,0)==EFI_SUCCESS && mmu_calls==1 && !asserts && !loops);
  check_hob(Begin,Length,EfiLoaderData,TRUE);
  check_hob(0x82800000,0x02000000,EfiLoaderData,TRUE);check_hob(0xF3800000,0x05000000,EfiLoaderData,TRUE);
  const ARM_MEMORY_REGION_DESCRIPTOR *M=mapped_at(Begin);
  assert(M && M->PhysicalBase==Begin && M->VirtualBase==Begin && M->Length==Length);
  assert(M->Attributes==ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP);verify_original();
  EFI_PEI_HOB_POINTERS P;P.Raw=hob_list;unsigned overlaps=0;
  while(!END_OF_HOB_LIST(P)) {
    if(GET_HOB_TYPE(P)==EFI_HOB_TYPE_MEMORY_ALLOCATION) {
      UINT64 B=P.MemoryAllocation->AllocDescriptor.MemoryBaseAddress;
      UINT64 E=B+P.MemoryAllocation->AllocDescriptor.MemoryLength;
      if(B<End && E>Begin) {assert(B==Begin && E==End && P.MemoryAllocation->AllocDescriptor.MemoryType==EfiLoaderData);overlaps++;}
    }
    P.Raw=GET_NEXT_HOB(P);
  }
  assert(overlaps==1);
  setup(TRUE);assert(MemoryPeim(0,0)==EFI_SUCCESS && !mapped_at(Begin)); // default 88 remains unchanged
  puts("High offline prototype actual Mu HOB: 64-bit 1GiB Type2 occupied resource+allocation, identity WB-XP input, all 44 prior rows unchanged, no Conventional overlap PASS; real ownership/MMU/DXE/AT unverified.");
  return 0;
}
