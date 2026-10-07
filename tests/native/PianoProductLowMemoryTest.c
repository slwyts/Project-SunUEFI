// Actual Mu HOB/MMU helper + actual SEC arena and DMA named-row selection.
#define main PianoFrozenCmaMain
#include "PianoCmaHobTest.c"
#undef main
#include <Guid/MemoryAllocationHob.h>
EFI_GUID gEfiHobMemoryAllocStackGuid=EFI_HOB_MEMORY_ALLOC_STACK_GUID;
EFI_GUID *EFIAPI CopyGuid(EFI_GUID *D,CONST EFI_GUID *S){memcpy(D,S,sizeof(*D));return D;}
#define GetMemoryMap PianoProductLowGetMemoryMap
#define gMemoryDescriptor PianoProductLowDescriptors
#include "../../build/product-low-memory-test.c"
#undef GetMemoryMap
#undef gMemoryDescriptor
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
INTN EFIAPI AsciiStriCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcasecmp(A,B);}
#include "../../upstream/Mu-Silicium/Silicon/Silicium/SiliciumPkg/Library/MemoryMapHelperLib/MemoryMapHelper.c"
static UINT64 arena_base,arena_bytes,arena_end;
static EFI_HOB_HANDOFF_INFO_TABLE *HostArena(VOID *Base,UINTN Bytes,VOID *Bottom,VOID *Top){
  assert(Base==Bottom);arena_base=(UINTN)Base;arena_bytes=Bytes;arena_end=(UINTN)Top;
  return HobConstructor(arena.Bytes,sizeof(arena.Bytes),arena.Bytes,arena.Bytes+sizeof(arena.Bytes));
}
#define HobConstructor HostArena
#include "PianoActualSecMemory.h"
#undef HobConstructor
#include "PianoActualDmaHeap.h"
int main(void){
  setup(FALSE);EFI_MEMORY_REGION_DESCRIPTOR *Product;UINT8 N;PianoProductLowGetMemoryMap(&Product,&N);
  assert(N==45);memcpy(rows,Product,N*sizeof(*rows));count=N;
  assert(PianoActualInitializeMemory()==EFI_SUCCESS);
  assert(arena_base==0xBD980000&&arena_end==0xD4E23000&&arena_bytes==0x174A3000);
  UINT64 Base,Length;assert(Heap(&Base,&Length)==EFI_SUCCESS&&Base==arena_base&&Length==arena_bytes);
  assert(mapped_at(0xBD980000)&&mapped_at(0xD5100000));
  assert(!mapped_at(0xBD930000)&&!mapped_at(0xD4E23000));
  EFI_PEI_HOB_POINTERS P;P.Raw=hob_list;unsigned Low=0,Upper=0,Protected=0;
  while(!END_OF_HOB_LIST(P)){
    if(GET_HOB_TYPE(P)==EFI_HOB_TYPE_MEMORY_ALLOCATION){
      UINT64 B=P.MemoryAllocation->AllocDescriptor.MemoryBaseAddress,E=B+P.MemoryAllocation->AllocDescriptor.MemoryLength;
      if(P.MemoryAllocation->AllocDescriptor.MemoryType==EfiConventionalMemory){
        assert(!(B<0xBD980000&&E>0xBD930000)&&!(B<0xD5100000&&E>0xD4E23000));
        if(B==0xBD980000&&E==0xD4E23000)Low++;
        if(B==0xD5100000&&E==0xD8000000)Upper++;
      }
    }
    if(GET_HOB_TYPE(P)==EFI_HOB_TYPE_RESOURCE_DESCRIPTOR){
      UINT64 B=P.ResourceDescriptor->PhysicalStart;
      if(B==0xBD930000||B==0xD4E23000){assert(P.ResourceDescriptor->ResourceType==EFI_RESOURCE_MEMORY_RESERVED);Protected++;}
    }
    P.Raw=GET_NEXT_HOB(P);
  }
  assert(Low==1&&Upper==1&&Protected==2);
  for(unsigned I=0;I<N;++I){
    if(!strcmp(Product[I].Name,"DXE_Heap_Upper"))assert(Product[I].ArmAttributes==WRITE_BACK_XN);
    if(!strcmp(Product[I].Name,"DBI_Dump"))assert(Product[I].MemoryType==EfiReservedMemoryType&&Product[I].HobOption==NoHob&&Product[I].ArmAttributes==UNCACHED_UNBUFFERED_XN);
  }
  puts("Actual product cold low-memory fix: SEC/HobConstructor named safe372MiB arena; actual MemoryPeim resource+allocation HOB/MMU input; upper47MiB separate; no protected mapping/Conventional; actual DMA lower-only heap passed; no hardware");return 0;
}
