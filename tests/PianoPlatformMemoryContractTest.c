#define main PianoFrozenCmaMain
#include "PianoCmaHobTest.c"
#undef main
#include "../bootprofiles/uefi-app/PianoPlatformMemoryContract.h"
#include <Library/FdtLib.h>
#define GetMemoryMap PianoProductLowGetMemoryMap
#define gMemoryDescriptor PianoProductLowDescriptors
#include "../build/product-low-memory-test.c"
#undef GetMemoryMap
#undef gMemoryDescriptor
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S){return strlen(S);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
static PIANO_RAM_PARTITION_REPORT inventory;
static PIANO_PLATFORM_MEMORY_INPUT input;
static PIANO_PLATFORM_MEMORY_CONTRACT contract;
static EFI_STATUS ApproveHostOnly(VOID *Context,CONST PIANO_PLATFORM_MEMORY_INPUT *I,CONST PIANO_PLATFORM_MEMORY_CONTRACT *C){(void)Context;assert(I==&input&&C==&contract);return EFI_SUCCESS;}
static EFI_STATUS Warn(VOID *Context,CONST PIANO_PLATFORM_MEMORY_INPUT *I,CONST PIANO_PLATFORM_MEMORY_CONTRACT *C){(void)Context;(void)I;(void)C;return EFI_WARN_STALE_DATA;}
static unsigned Named(CONST CHAR8 *Name){unsigned N=0;for(unsigned X=0;X<contract.Count;X++)if(!strcmp(contract.Rows[X].Name,Name))N++;return N;}
int main(int argc,char **argv){
  assert(argc==2);FILE *File=fopen(argv[1],"rb");assert(File);fseek(File,0,SEEK_END);long Bytes=ftell(File);rewind(File);VOID *Dtb=malloc(Bytes);assert(fread(Dtb,1,Bytes,File)==(unsigned long)Bytes);fclose(File);
  EFI_MEMORY_REGION_DESCRIPTOR *Native;UINT8 N;PianoProductLowGetMemoryMap(&Native,&N);
  inventory.Status=EFI_SUCCESS;inventory.IdentityVerified=inventory.AbiVerified=inventory.DataValid=TRUE;
  // fixture bank list from real captured DT, passed through real parser.
  INT32 Node=FdtPathOffset(Dtb,"/memory"),Length;CONST UINT8 *P=FdtGetProp(Dtb,Node,"reg",&Length);assert(P&&Length%16==0);
  for(INT32 X=0;X<Length;X+=16){UINT64 B=0,Z=0;for(unsigned Q=0;Q<8;Q++){B=(B<<8)|P[X+Q];Z=(Z<<8)|P[X+8+Q];}if(Z)inventory.Banks[inventory.BankCount++]=(PIANO_RAM_BANK){B,Z};}
  input=(PIANO_PLATFORM_MEMORY_INPUT){.Native=Native,.NativeCount=N,.Inventory=&inventory,.Fdt=Dtb,.FdtBytes=Bytes,.CpuArenaBase=0xA00000000ULL,.CpuArenaBytes=0x40000000ULL};
  assert(PianoPlatformMemoryCompose(&input,&contract)==EFI_SUCCESS&&contract.Composed&&!contract.ReadyForMemoryPeim&&
    !contract.UnplacedDynamicConstraints&&contract.FutureLinuxDynamicConstraints==14&&Named("Piano_CPU_Arena")==1);
  assert(contract.CpuArenaBytes==0x40000000ULL&&contract.AddedOccupiedBytes>13ULL*1024*1024*1024);
  printf("Host fixture typed contract: rows=%u added_occupied_bytes=%llu future_linux_dynamic=%u ready=%u\n",contract.Count,(unsigned long long)contract.AddedOccupiedBytes,contract.FutureLinuxDynamicConstraints,contract.ReadyForMemoryPeim);
  for(unsigned X=0;X<N;X++)assert(!memcmp(&Native[X],&contract.Rows[X],sizeof(*Native)));
  for(unsigned X=N;X<contract.Count;X++)assert(contract.Rows[X].MemoryType==EfiLoaderData&&contract.Rows[X].ArmAttributes==ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP);
  EFI_MEMORY_REGION_DESCRIPTOR *Exposed=(VOID *)1;UINT8 Count=99;
  assert(PianoPlatformMemoryAcquireForMemoryPeim(&contract,&Exposed,&Count)==EFI_NOT_READY&&!Exposed&&!Count);
  assert(PianoPlatformMemoryAuthorizeCold(&input,&contract,NULL,NULL)==EFI_NOT_READY&&!contract.ReadyForMemoryPeim);
  assert(PianoPlatformMemoryAuthorizeCold(&input,&contract,Warn,NULL)==EFI_DEVICE_ERROR&&!contract.ReadyForMemoryPeim);
  contract.FutureLinuxDynamicConstraints=0;
  assert(PianoPlatformMemoryAuthorizeCold(&input,&contract,ApproveHostOnly,NULL)==EFI_COMPROMISED_DATA&&!contract.ReadyForMemoryPeim);
  assert(PianoPlatformMemoryCompose(&input,&contract)==EFI_SUCCESS);
  assert(PianoPlatformMemoryAuthorizeCold(&input,&contract,ApproveHostOnly,NULL)==EFI_SUCCESS);
  assert(PianoPlatformMemoryAcquireForMemoryPeim(&contract,&Exposed,&Count)==EFI_SUCCESS&&Count==contract.Count);
  assert(PianoPlatformMemoryAcquireForMemoryPeim(&contract,&Exposed,&Count)==EFI_NOT_READY);
  // Actual MemoryPeim/AddHob: authorized host fixture only, hardware MMU stub.
  setup(FALSE);memcpy(rows,contract.Rows,contract.Count*sizeof(*rows));count=contract.Count;
  assert(MemoryPeim(0,0)==EFI_SUCCESS);
  check_hob(0xA00000000ULL,0x40000000ULL,EfiLoaderData,TRUE);
  check_hob(0x82800000,0x2000000,EfiLoaderData,TRUE);check_hob(0xF3800000,0x5000000,EfiLoaderData,TRUE);
  assert(mapped_at(0xA00000000ULL)->Attributes==ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP);
  assert(!mapped_at(0xBD930000)&&!mapped_at(0xD4E23000));
  PIANO_PLATFORM_MEMORY_OWNER Owner={0xA00000000ULL,4096,-1};input.KnownOwners=&Owner;input.OwnerCount=1;
  assert(PianoPlatformMemoryCompose(&input,&contract)==EFI_ACCESS_DENIED&&!contract.ReadyForMemoryPeim);
  input.KnownOwners=NULL;input.OwnerCount=0;inventory.PotentialFallback=TRUE;
  assert(PianoPlatformMemoryCompose(&input,&contract)==EFI_NOT_READY&&!contract.ReadyForMemoryPeim);
  inventory.PotentialFallback=FALSE;inventory.DataValid=FALSE;
  assert(PianoPlatformMemoryCompose(&input,&contract)==EFI_NOT_READY);
  inventory.DataValid=TRUE;assert(PianoPlatformMemoryCompose(&input,&contract)==EFI_SUCCESS);
  inventory.Banks[0].AvailableLength-=4096;
  assert(PianoPlatformMemoryAuthorizeCold(&input,&contract,ApproveHostOnly,NULL)==EFI_COMPROMISED_DATA&&!contract.ReadyForMemoryPeim);
  inventory.Banks[0].AvailableLength+=4096;
  inventory.PreloadedCount=1;inventory.Preloaded[0]=(PIANO_RAM_PRELOADED){0xA20000000ULL,4096,7,0};
  assert(PianoPlatformMemoryCompose(&input,&contract)==EFI_ACCESS_DENIED); // real preloaded overlap excludes arena
  inventory.PreloadedCount=0;input.CpuArenaBase=0xF3800000;input.CpuArenaBytes=0x5000000;
  assert(PianoPlatformMemoryCompose(&input,&contract)==EFI_ACCESS_DENIED); // CMA never CPU download arena
  input.CpuArenaBase=0xA00000000ULL;input.CpuArenaBytes=0x40000000;
  inventory.Banks[1]=inventory.Banks[0];
  assert(PianoPlatformMemoryCompose(&input,&contract)==EFI_COMPROMISED_DATA);
  free(Dtb);puts("Actual full-DDR C contract + pinned libfdt + real Mu HOB/MMU inputs: occupied high DDR/CMA, safe lowheap, arena ownership/fallback/mutation/warning/default-unready gates passed; no actual MMU/device/high-DDR touch");return 0;
}
