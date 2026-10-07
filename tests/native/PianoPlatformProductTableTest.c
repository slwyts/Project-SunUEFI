// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual prepared product native table + final DTB + captured current records.
// Composition only; this test never authorizes or invokes MemoryPeim/MMU.
#define main PianoFrozenCmaMain
#include "PianoCmaHobTest.c"
#undef main
#include "PianoPlatformMemoryContract.h"
#include "../../uefi/handoff/early-memory/PianoSmemRam.h"
#define GetMemoryMap PianoActualProductGetMemoryMap
#define gMemoryDescriptor PianoActualProductDescriptors
#include "PianoActualProductTable.c"
#undef GetMemoryMap
#undef gMemoryDescriptor
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S){return strlen(S);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
static EFI_MEMORY_REGION_DESCRIPTOR native_copy[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];
static PIANO_PLATFORM_MEMORY_INPUT input;
static PIANO_PLATFORM_MEMORY_CONTRACT contract,again;
static PIANO_RAM_PARTITION_REPORT inventory;
static PIANO_SMEM_RAM_REPORT observed;
static UINT8 raw[PIANO_SMEM_PAYLOAD_MAX];
static UINT32 cases;
static VOID *ReadFile(CONST CHAR8 *Path,UINTN *Bytes){
  FILE *F=fopen(Path,"rb");assert(F);assert(!fseek(F,0,SEEK_END));long N=ftell(F);assert(N>0);rewind(F);
  VOID *P=malloc((UINTN)N);assert(P&&fread(P,1,(UINTN)N,F)==(UINTN)N&&!fclose(F));*Bytes=(UINTN)N;return P;
}
static UINTN Named(CONST CHAR8 *Name){for(UINTN I=0;I<input.NativeCount;++I)if(!strcmp(native_copy[I].Name,Name))return I;assert(!"missing actual row");return 0;}
static VOID Compose(EFI_STATUS Expected){assert(PianoPlatformMemoryCompose(&input,&contract)==Expected&&!contract.ReadyForMemoryPeim);++cases;}
int main(int Argc,char **Argv){
  assert(Argc==3);UINTN Bytes,RawBytes;VOID *Dtb=ReadFile(Argv[1],&Bytes),*Payload=ReadFile(Argv[2],&RawBytes);
  assert(RawBytes==2328&&RawBytes<sizeof(raw));memcpy(raw,Payload,RawBytes);free(Payload);
  assert(PianoSmemRamParse(raw,RawBytes,&observed)==EFI_SUCCESS&&observed.BankCount==12&&observed.OtherCategoryCount==3);
  UINT8 NativeCount;EFI_MEMORY_REGION_DESCRIPTOR *Native;PianoActualProductGetMemoryMap(&Native,&NativeCount);assert(NativeCount==49||NativeCount==52||NativeCount==53);
  if(NativeCount>49){
    CONST CHAR8 *Names[]={"Piano_Display_GCC","Piano_Display_DPU","Piano_Display_DISPCC","Piano_Display_CESTA"};
    CONST UINT64 Base[]={0x100000,0xae00000,0xaf00000,0xaf27000},Length[]={0x1f5000,0x94000,0x20000,0x3000};
    for(UINTN I=0;I<NativeCount-49U;++I){CONST EFI_MEMORY_REGION_DESCRIPTOR *R=&Native[49+I];
      assert(!strcmp(R->Name,Names[I])&&R->Address==Base[I]&&R->Length==Length[I]&&R->HobOption==AddDev&&
        R->ResourceType==MMAP_IO&&R->ResourceAttribute==EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE&&
        R->MemoryType==EfiMemoryMappedIO&&R->ArmAttributes==ARM_MEMORY_REGION_ATTRIBUTE_DEVICE);}
  }
  memcpy(native_copy,Native,NativeCount*sizeof(*Native));
  // The actual native-call identity boundary is separately executed in
  // PianoRamPartitionTest. These flags are explicit host fixture inputs only.
  inventory.Status=EFI_SUCCESS;inventory.IdentityVerified=inventory.AbiVerified=inventory.DataValid=TRUE;
  inventory.BankCount=observed.BankCount;for(UINTN I=0;I<observed.BankCount;++I)
    inventory.Banks[I]=(PIANO_RAM_BANK){observed.Banks[I].Base,observed.Banks[I].AvailableLength};
  assert(!inventory.Banks[3].AvailableLength&&!inventory.OwnershipVerified);
  input=(PIANO_PLATFORM_MEMORY_INPUT){.Native=native_copy,.NativeCount=NativeCount,.Inventory=&inventory,
    .Fdt=Dtb,.FdtBytes=Bytes,.CpuArenaBase=0xA00000000ULL,.CpuArenaBytes=0x80000000ULL};
  Compose(EFI_SUCCESS);assert(contract.Composed&&contract.CpuArenaBytes==0x80000000ULL&&
    !contract.UnplacedDynamicConstraints&&contract.FutureLinuxDynamicConstraints==14);
  for(UINTN I=0;I<NativeCount;++I)assert(!memcmp(&Native[I],&contract.Rows[I],sizeof(*Native)));
  for(UINTN I=NativeCount;I<contract.Count;++I)assert(contract.Rows[I].MemoryType==EfiLoaderData);
  assert(PianoPlatformMemoryCompose(&input,&again)==EFI_SUCCESS&&again.Count==contract.Count&&
    again.InputFingerprint==contract.InputFingerprint&&!memcmp(again.Rows,contract.Rows,contract.Count*sizeof(contract.Rows[0])));++cases;
  EFI_MEMORY_REGION_DESCRIPTOR *Exposed=(VOID *)1;UINT8 Count=99;
  assert(PianoPlatformMemoryAcquireForMemoryPeim(&contract,&Exposed,&Count)==EFI_NOT_READY&&!Exposed&&!Count);
  assert(PianoPlatformMemoryAuthorizeCold(&input,&contract,NULL,NULL)==EFI_NOT_READY&&!contract.ReadyForMemoryPeim);++cases;
  UINTN Usb2=Named("Piano_USB2_PHY"),Parent=Named("PERIPH_SS");EFI_MEMORY_REGION_DESCRIPTOR Saved=native_copy[Usb2];
  native_copy[Usb2].ArmAttributes=ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP;Compose(EFI_COMPROMISED_DATA);native_copy[Usb2]=Saved;
  native_copy[Usb2].MemoryType=EfiConventionalMemory;Compose(EFI_COMPROMISED_DATA);native_copy[Usb2]=Saved;
  native_copy[Usb2].ResourceAttribute^=1;Compose(EFI_COMPROMISED_DATA);native_copy[Usb2]=Saved;
  native_copy[Usb2].HobOption=AddMem;Compose(EFI_COMPROMISED_DATA);native_copy[Usb2]=Saved;
  native_copy[Usb2].Address=native_copy[Parent].Address+native_copy[Parent].Length-4096;
  native_copy[Usb2].Length=8192;Compose(EFI_COMPROMISED_DATA);native_copy[Usb2]=Saved;
  native_copy[Usb2].Address=MAX_UINT64-4095;Compose(EFI_COMPROMISED_DATA);native_copy[Usb2]=Saved;
  // Even equal-attribute aliases in DRAM/allocator space must remain rejected.
  UINTN Lower=Named("DXE_Heap"),Upper=Named("DXE_Heap_Upper");Saved=native_copy[Upper];
  native_copy[Upper]=native_copy[Lower];memcpy(native_copy[Upper].Name,"Forbidden_DRAM_Alias",21);
  Compose(EFI_COMPROMISED_DATA);native_copy[Upper]=Saved;
  inventory.Banks[3]=(PIANO_RAM_BANK){inventory.Banks[0].Base,0};Compose(EFI_SUCCESS);
  inventory.Banks[3]=(PIANO_RAM_BANK){MAX_UINT64,0};Compose(EFI_SUCCESS);
  inventory.Banks[3]=(PIANO_RAM_BANK){MAX_UINT64-4095,8192};Compose(EFI_COMPROMISED_DATA);
  inventory.Banks[3]=(PIANO_RAM_BANK){inventory.Banks[0].Base,4096};Compose(EFI_COMPROMISED_DATA);
  inventory.Banks[3]=(PIANO_RAM_BANK){observed.Banks[3].Base,0};
  PIANO_PLATFORM_MEMORY_OWNER CurrentOwner={0xA10000000ULL,4096,-1};input.KnownOwners=&CurrentOwner;input.OwnerCount=1;
  Compose(EFI_ACCESS_DENIED);input.KnownOwners=NULL;input.OwnerCount=0;Compose(EFI_SUCCESS);
  assert(!inventory.OwnershipVerified&&!contract.ReadyForMemoryPeim);
  free(Dtb);printf("Actual product%u rows + final DTB + physical current12: %u canonical/zero/MMIO-alias/default-unready cases passed; no HOB/MMU/high allocation\n",NativeCount,cases);return 0;
}
