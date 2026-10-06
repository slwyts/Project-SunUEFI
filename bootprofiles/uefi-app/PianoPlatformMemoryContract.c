// SPDX-License-Identifier: BSD-2-Clause-Patent
// Typed resource/allocation/MMU table, not a parallel JSON planning model.
#include "PianoPlatformMemoryContract.h"
#include <Library/FdtLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
typedef struct {UINT64 Begin,End;BOOLEAN Cma;} SPAN;
#define MAX_SPANS 128U
#define MAX_POINTS 512U
STATIC UINT64 Be(CONST UINT8 *P,UINTN N){UINT64 V=0;for(UINTN I=0;I<N;++I)V=(V<<8)|P[I];return V;}
STATIC BOOLEAN Valid(UINT64 B,UINT64 N){return N&&B<=MAX_UINT64-N;}
STATIC BOOLEAN Overlap(UINT64 A,UINT64 B,UINT64 C,UINT64 D){return A<D&&C<B;}
STATIC BOOLEAN MmioAlias(CONST EFI_MEMORY_REGION_DESCRIPTOR *A,CONST EFI_MEMORY_REGION_DESCRIPTOR *B){
  if(A->HobOption!=AddDev||B->HobOption!=AddDev||A->ResourceType!=MMAP_IO||B->ResourceType!=MMAP_IO||
     A->MemoryType!=EfiMemoryMappedIO||B->MemoryType!=EfiMemoryMappedIO||
     A->ArmAttributes!=ARM_MEMORY_REGION_ATTRIBUTE_DEVICE||B->ArmAttributes!=A->ArmAttributes||
     A->ResourceAttribute!=B->ResourceAttribute)return FALSE;
  return (A->Address<=B->Address&&B->Address+B->Length<=A->Address+A->Length)||
    (B->Address<=A->Address&&A->Address+A->Length<=B->Address+B->Length);
}
STATIC BOOLEAN Covered(SPAN *S,UINTN N,UINT64 B,UINT64 E){for(UINTN I=0;I<N;++I)if(B>=S[I].Begin&&E<=S[I].End)return TRUE;return FALSE;}
STATIC EFI_STATUS Add(SPAN *S,UINTN *N,UINT64 B,UINT64 Z,BOOLEAN Cma,BOOLEAN Outward){
  if(!Z)return EFI_SUCCESS;if(!Valid(B,Z)||*N>=MAX_SPANS)return EFI_COMPROMISED_DATA;
  UINT64 E=B+Z;if(Outward){if(E>MAX_UINT64-4095)return EFI_COMPROMISED_DATA;B&=~4095ULL;E=(E+4095)&~4095ULL;}
  else{if(B>MAX_UINT64-4095)return EFI_COMPROMISED_DATA;B=(B+4095)&~4095ULL;E&=~4095ULL;}
  if(E>B)S[(*N)++]=(SPAN){B,E,Cma};return EFI_SUCCESS;
}
STATIC BOOLEAN Prop(CONST VOID *F,INT32 N,CONST CHAR8 *Name){INT32 Len;return FdtGetProp(F,N,Name,&Len)!=NULL;}
STATIC BOOLEAN Cells(CONST VOID *F,INT32 N){INT32 L;CONST UINT8 *P=FdtGetProp(F,N,"#address-cells",&L);if(!P||L!=4||Be(P,4)!=2)return FALSE;P=FdtGetProp(F,N,"#size-cells",&L);return P&&L==4&&Be(P,4)==2;}
STATIC EFI_STATUS Pairs(CONST VOID *F,INT32 Node,SPAN *S,UINTN *N,BOOLEAN Cma,BOOLEAN Outward){
  INT32 Len;CONST UINT8 *P=FdtGetProp(F,Node,"reg",&Len);
  if(!P||Len<=0||Len%16)return EFI_COMPROMISED_DATA;
  for(INT32 I=0;I<Len;I+=16){EFI_STATUS E=Add(S,N,Be(P+I,8),Be(P+I+8,8),Cma,Outward);if(E!=EFI_SUCCESS)return E;}return EFI_SUCCESS;
}
STATIC BOOLEAN Compat(CONST VOID *F,INT32 N){INT32 L;CONST CHAR8 *P=FdtGetProp(F,N,"compatible",&L);CONST CHAR8 Wanted[]="shared-dma-pool";
  if(!P||L<=0)return FALSE;for(INT32 I=0;I<L;){INT32 Z=I;while(Z<L&&P[Z])++Z;if(Z==L)return FALSE;if(Z-I==sizeof(Wanted)-1&&!CompareMem(P+I,Wanted,sizeof(Wanted)-1))return TRUE;I=Z+1;}return FALSE;}
STATIC EFI_STATUS Point(UINT64 *P,UINTN *N,UINT64 V){for(UINTN I=0;I<*N;++I)if(P[I]==V)return EFI_SUCCESS;if(*N>=MAX_POINTS)return EFI_BUFFER_TOO_SMALL;UINTN I=(*N)++;while(I&&P[I-1]>V){P[I]=P[I-1];--I;}P[I]=V;return EFI_SUCCESS;}
STATIC UINT64 Hash(UINT64 H,CONST VOID *P,UINTN N){CONST UINT8 *B=P;for(UINTN X=0;X<N;++X){H^=B[X];H*=1099511628211ULL;}return H;}
STATIC UINT64 Fingerprint(CONST PIANO_PLATFORM_MEMORY_INPUT *I){
  UINT64 H=14695981039346656037ULL;H=Hash(H,I->Fdt,I->FdtBytes);H=Hash(H,I->Native,I->NativeCount*sizeof(I->Native[0]));
  H=Hash(H,I->Inventory,sizeof(*I->Inventory));H=Hash(H,I->KnownOwners,I->OwnerCount*sizeof(I->KnownOwners[0]));
  H=Hash(H,&I->CpuArenaBase,sizeof(I->CpuArenaBase));return Hash(H,&I->CpuArenaBytes,sizeof(I->CpuArenaBytes));
}
STATIC EFI_STATUS Row(PIANO_PLATFORM_MEMORY_CONTRACT *C,CONST CHAR8 *Name,UINT64 B,UINT64 E){
  if(C->Count>=MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT-1)return EFI_BUFFER_TOO_SMALL;
  EFI_MEMORY_REGION_DESCRIPTOR R={0};UINTN Z=AsciiStrLen(Name);if(Z>=sizeof(R.Name))return EFI_COMPROMISED_DATA;
  CopyMem(R.Name,Name,Z+1);R.Address=B;R.Length=E-B;R.HobOption=AddMem;R.ResourceType=SYS_MEM;R.ResourceAttribute=SYS_MEM_CAP;
  R.MemoryType=EfiLoaderData;R.ArmAttributes=ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_XP;
  // Merge only newly constructed identical adjacent occupied rows; native
  // descriptors are copied byte-for-byte and never silently rewritten.
  if(C->Count){EFI_MEMORY_REGION_DESCRIPTOR *Last=&C->Rows[C->Count-1];if(!AsciiStrCmp(Last->Name,Name)&&Last->Address+Last->Length==B&&Last->MemoryType==EfiLoaderData&&Last->ArmAttributes==R.ArmAttributes){Last->Length+=R.Length;return EFI_SUCCESS;}}
  C->Rows[C->Count++]=R;return EFI_SUCCESS;
}
EFI_STATUS PianoPlatformMemoryCompose(CONST PIANO_PLATFORM_MEMORY_INPUT *I,PIANO_PLATFORM_MEMORY_CONTRACT *C){
  if(!I||!C)return EFI_INVALID_PARAMETER;ZeroMem(C,sizeof(*C));C->Authorization=EFI_NOT_READY;
  if(!I->Native||!I->NativeCount||I->NativeCount>=MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT||!I->Inventory||!I->Fdt||I->FdtBytes<40||I->FdtBytes>0x200000||
     I->OwnerCount>PIANO_PLATFORM_MEMORY_MAX_OWNERS||(I->OwnerCount&&!I->KnownOwners))return C->Status=EFI_INVALID_PARAMETER;
  CONST PIANO_RAM_PARTITION_REPORT *R=I->Inventory;
  if(R->Status!=EFI_SUCCESS||!R->IdentityVerified||!R->AbiVerified||!R->DataValid||R->PotentialFallback||R->Retained||!R->BankCount||R->BankCount>PIANO_RAM_PARTITION_MAX||R->PreloadedCount>PIANO_RAM_PARTITION_MAX)return C->Status=EFI_NOT_READY;
  if(FdtCheckHeader(I->Fdt)||FdtTotalSize(I->Fdt)!=I->FdtBytes||!Cells(I->Fdt,0))return C->Status=EFI_COMPROMISED_DATA;
  SPAN Dram[MAX_SPANS],Fixed[MAX_SPANS],Banks[MAX_SPANS],Owners[MAX_SPANS];UINTN Nd=0,Nf=0,Nb=0,No=0;EFI_STATUS S;
  INT32 Node;FdtForEachSubnode(Node,I->Fdt,0){INT32 L;CONST CHAR8 *P=FdtGetProp(I->Fdt,Node,"device_type",&L);if(P&&L==7&&!CompareMem(P,"memory",7)){S=Pairs(I->Fdt,Node,Dram,&Nd,FALSE,FALSE);if(S!=EFI_SUCCESS)return C->Status=S;}}
  if(Node!=-FDT_ERR_NOTFOUND)return C->Status=EFI_COMPROMISED_DATA;
  if(!Nd)return C->Status=EFI_NOT_FOUND;
  INT32 Reserved=FdtPathOffset(I->Fdt,"/reserved-memory");if(Reserved<0||!Cells(I->Fdt,Reserved))return C->Status=EFI_COMPROMISED_DATA;
  FdtForEachSubnode(Node,I->Fdt,Reserved){
    if(Prop(I->Fdt,Node,"reg")){
      INT32 L;CONST CHAR8 *State=FdtGetProp(I->Fdt,Node,"status",&L);BOOLEAN Enabled=!State||(L==5&&!CompareMem(State,"okay",5))||(L==3&&!CompareMem(State,"ok",3));
      BOOLEAN Reuse=Prop(I->Fdt,Node,"reusable"),NoMap=Prop(I->Fdt,Node,"no-map");if(Reuse&&NoMap)return C->Status=EFI_COMPROMISED_DATA;
      S=Pairs(I->Fdt,Node,Fixed,&Nf,Enabled&&Reuse&&!NoMap&&Compat(I->Fdt,Node),TRUE);if(S!=EFI_SUCCESS)return C->Status=S;
    }else if(Prop(I->Fdt,Node,"size")){
      // Generic Linux reserved-memory allocates these requests in memblock
      // after EBS. Their envelopes are not present UEFI owner intervals.
      // Any real current owner supplied by Root is independently excluded.
      ++C->FutureLinuxDynamicConstraints;
    }
  }
  if(Node!=-FDT_ERR_NOTFOUND)return C->Status=EFI_COMPROMISED_DATA;
  INTN Reserves=FdtGetNumberOfReserveMapEntries(I->Fdt);if(Reserves<0||(UINTN)Reserves>MAX_SPANS)return C->Status=EFI_COMPROMISED_DATA;
  for(INTN X=0;X<Reserves;++X){UINT64 B,Z;if(FdtGetReserveMapEntry(I->Fdt,X,&B,&Z))return C->Status=EFI_COMPROMISED_DATA;S=Add(Fixed,&Nf,B,Z,FALSE,TRUE);if(S!=EFI_SUCCESS)return C->Status=S;}
  C->FixedReservations=(UINT32)Nf;
  for(UINTN X=0;X<R->BankCount;++X){if(!R->Banks[X].AvailableLength)continue;if(!Valid(R->Banks[X].Base,R->Banks[X].AvailableLength))return C->Status=EFI_COMPROMISED_DATA;S=Add(Banks,&Nb,R->Banks[X].Base,R->Banks[X].AvailableLength,FALSE,FALSE);if(S!=EFI_SUCCESS)return C->Status=S;}
  for(UINTN X=0;X<Nb;++X)for(UINTN Y=0;Y<X;++Y)if(Overlap(Banks[X].Begin,Banks[X].End,Banks[Y].Begin,Banks[Y].End))return C->Status=EFI_COMPROMISED_DATA;
  for(UINTN X=0;X<Nd;++X)for(UINTN Y=0;Y<X;++Y)if(Overlap(Dram[X].Begin,Dram[X].End,Dram[Y].Begin,Dram[Y].End))return C->Status=EFI_COMPROMISED_DATA;
  for(UINTN X=0;X<R->PreloadedCount;++X){if(!Valid(R->Preloaded[X].Base,R->Preloaded[X].Size))return C->Status=EFI_COMPROMISED_DATA;S=Add(Owners,&No,R->Preloaded[X].Base,R->Preloaded[X].Size,FALSE,TRUE);if(S!=EFI_SUCCESS)return C->Status=S;}
  for(UINTN X=0;X<I->OwnerCount;++X){S=Add(Owners,&No,I->KnownOwners[X].Base,I->KnownOwners[X].Bytes,FALSE,TRUE);if(S!=EFI_SUCCESS)return C->Status=S;}
  // Require the real cold-boot lowheap fix before proposing any new DDR.
  BOOLEAN Heap=FALSE;for(UINTN X=0;X<I->NativeCount;++X){CONST EFI_MEMORY_REGION_DESCRIPTOR *N=&I->Native[X];
    BOOLEAN Terminated=FALSE;for(UINTN Y=0;Y<sizeof(N->Name);++Y)if(!N->Name[Y])Terminated=TRUE;if(!Terminated)return C->Status=EFI_COMPROMISED_DATA;
    if(!Valid(N->Address,N->Length)||(N->Address|N->Length)&4095)return C->Status=EFI_COMPROMISED_DATA;
    for(UINTN Y=0;Y<X;++Y)if(Overlap(N->Address,N->Address+N->Length,I->Native[Y].Address,I->Native[Y].Address+I->Native[Y].Length)&&!MmioAlias(N,&I->Native[Y]))return C->Status=EFI_COMPROMISED_DATA;
    if(!AsciiStrCmp(N->Name,"DXE_Heap")){if(Heap||N->Address!=0xBD980000||N->Length!=0x174A3000||N->MemoryType!=EfiConventionalMemory)return C->Status=EFI_ACCESS_DENIED;Heap=TRUE;}
    C->Rows[C->Count++]=*N;
  }if(!Heap)return C->Status=EFI_NOT_FOUND;
  UINT64 Points[MAX_POINTS];UINTN Np=0;
#define POINT(V) do{S=Point(Points,&Np,(V));if(S!=EFI_SUCCESS)return C->Status=S;}while(0)
  for(UINTN X=0;X<Nd;++X){POINT(Dram[X].Begin);POINT(Dram[X].End);}
  for(UINTN X=0;X<Nb;++X){POINT(Banks[X].Begin);POINT(Banks[X].End);}
  for(UINTN X=0;X<Nf;++X){POINT(Fixed[X].Begin);POINT(Fixed[X].End);}
  for(UINTN X=0;X<No;++X){POINT(Owners[X].Begin);POINT(Owners[X].End);}
  for(UINTN X=0;X<I->NativeCount;++X){POINT(I->Native[X].Address);POINT(I->Native[X].Address+I->Native[X].Length);}
  UINT64 Ae=0;if(I->CpuArenaBytes){if(!Valid(I->CpuArenaBase,I->CpuArenaBytes)||(I->CpuArenaBase|I->CpuArenaBytes)&4095)return C->Status=EFI_INVALID_PARAMETER;Ae=I->CpuArenaBase+I->CpuArenaBytes;POINT(I->CpuArenaBase);POINT(Ae);}
  for(UINTN X=0;X+1<Np;++X){UINT64 B=Points[X],E=Points[X+1];if(!Covered(Dram,Nd,B,E)||!Covered(Banks,Nb,B,E))continue;
    BOOLEAN Existing=FALSE;for(UINTN Y=0;Y<I->NativeCount;++Y)if(Overlap(B,E,I->Native[Y].Address,I->Native[Y].Address+I->Native[Y].Length))Existing=TRUE;
    if(Existing||Covered(Owners,No,B,E))continue;
    BOOLEAN ReservedHere=FALSE,Cma=FALSE;for(UINTN Y=0;Y<Nf;++Y)if(Overlap(B,E,Fixed[Y].Begin,Fixed[Y].End)){if(Fixed[Y].Cma)Cma=TRUE;else ReservedHere=TRUE;}
    if(ReservedHere)continue;
    BOOLEAN Arena=I->CpuArenaBytes&&B>=I->CpuArenaBase&&E<=Ae;
    if(Arena&&Cma)return C->Status=EFI_ACCESS_DENIED;
    S=Row(C,Arena?"Piano_CPU_Arena":Cma?"Piano_Fixed_CMA":"Piano_DDR_Occupied",B,E);if(S!=EFI_SUCCESS)return C->Status=S;
    C->AddedOccupiedBytes+=E-B;if(Arena)C->CpuArenaBytes+=E-B;
  }
#undef POINT
  if(I->CpuArenaBytes&&C->CpuArenaBytes!=I->CpuArenaBytes)return C->Status=EFI_ACCESS_DENIED;
  C->CpuArenaBase=I->CpuArenaBase;C->InputFingerprint=Fingerprint(I);C->Composed=TRUE;C->ReadyForMemoryPeim=FALSE;return C->Status=EFI_SUCCESS;
}
EFI_STATUS PianoPlatformMemoryAuthorizeCold(CONST PIANO_PLATFORM_MEMORY_INPUT *I,PIANO_PLATFORM_MEMORY_CONTRACT *C,PIANO_PLATFORM_MEMORY_AUTHORIZE A,VOID *Context){
  if(!I||!C||!C->Composed||C->Exposed||C->Status!=EFI_SUCCESS)return EFI_NOT_READY;
  C->ReadyForMemoryPeim=FALSE;C->AuthorizationAttempted=TRUE;if(!A)return C->Authorization=EFI_NOT_READY;
  // Same input set must still compose identically; no mutable owner/DT snapshot.
  PIANO_PLATFORM_MEMORY_CONTRACT Fresh;EFI_STATUS S=PianoPlatformMemoryCompose(I,&Fresh);
  if(S!=EFI_SUCCESS||Fresh.InputFingerprint!=C->InputFingerprint||Fresh.Count!=C->Count||
     Fresh.FixedReservations!=C->FixedReservations||Fresh.UnplacedDynamicConstraints!=C->UnplacedDynamicConstraints||
     Fresh.FutureLinuxDynamicConstraints!=C->FutureLinuxDynamicConstraints||
     CompareMem(Fresh.Rows,C->Rows,C->Count*sizeof(C->Rows[0])))return C->Authorization=EFI_COMPROMISED_DATA;
  S=A(Context,I,C);C->Authorization=S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;
  if(Fingerprint(I)!=C->InputFingerprint||Fresh.Count!=C->Count||
     Fresh.FixedReservations!=C->FixedReservations||Fresh.UnplacedDynamicConstraints!=C->UnplacedDynamicConstraints||
     Fresh.FutureLinuxDynamicConstraints!=C->FutureLinuxDynamicConstraints||
     CompareMem(Fresh.Rows,C->Rows,C->Count*sizeof(C->Rows[0])))return C->Authorization=EFI_COMPROMISED_DATA;
  C->ReadyForMemoryPeim=S==EFI_SUCCESS;return C->Authorization;
}
EFI_STATUS PianoPlatformMemoryAcquireForMemoryPeim(PIANO_PLATFORM_MEMORY_CONTRACT *C,EFI_MEMORY_REGION_DESCRIPTOR **Rows,UINT8 *Count){
  if(!C||!Rows||!Count)return EFI_INVALID_PARAMETER;*Rows=NULL;*Count=0;
  if(!C->Composed||C->Exposed||!C->ReadyForMemoryPeim||C->Authorization!=EFI_SUCCESS)return EFI_NOT_READY;
  C->Exposed=TRUE;*Rows=C->Rows;*Count=C->Count;return EFI_SUCCESS;
}
