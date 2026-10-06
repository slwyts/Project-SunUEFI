// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoDisplayClockRead.h"
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/DxeServicesLib.h>
#define DCR_SIGNATURE SIGNATURE_32('P','D','C','R')
#define DCR_IMAGE_BYTES 0x44000U
#define DCR_TEXT_FIRST 0x1000U
#define DCR_TEXT_END 0x28000U
#define DCR_DATA_END 0x41000U
#define DCR_GCC_NODE 0x33a68U
#define DCR_CACHE_TYPES (EFI_MEMORY_UC|EFI_MEMORY_WC|EFI_MEMORY_WT|EFI_MEMORY_WB|EFI_MEMORY_UCE)
STATIC EFI_GUID mDcrImageGuid={0x4db5dea6,0x5302,0x4d1a,{0x8a,0x82,0x67,0x7a,0x68,0x3b,0x0d,0x29}};
STATIC CONST UINT8 mDcrPin[32]={0xf9,0xe8,0x5a,0xa7,0x58,0x93,0x2b,0x43,0x66,0xc5,0x5e,0xc8,0x3b,0x58,0xe0,0x17,0x6f,0x58,0xfb,0xa2,0x94,0x4c,0xc2,0xfd,0xd3,0x48,0x75,0xa4,0x6f,0xdb,0x76,0x9e};
typedef struct {PIANO_DISPLAY_CLOCK_READ_ROLE Role;UINT64 Base,Bytes,Anchor;} DCR_OBJECT;
typedef struct {
 UINT64 Global,Client,RootClient,Registry,Entry,Modules,Module,Array,Name,Parent,Head,Node;
 UINT32 ModuleCount,ClockCount,Provider,Index,Used,Entries,Clients;
 UINT64 Seen[64],Next[64],ClientAt[64];
 UINT64 RegistrySeen[64],RegistryNext[64],RegistryName[64],ClientSeen[64],ClientNext[64],ClientBack[64];
 UINT32 RegistryId[64];
} DCR_GRAPH;
STATIC EFI_STATUS DcrExact(EFI_STATUS E){return E==EFI_SUCCESS?E:EFI_ERROR(E)?E:EFI_DEVICE_ERROR;}
STATIC BOOLEAN DcrSpan(UINT64 B,UINT64 N,UINT64 A,UINT64 Z){return B&&N&&B<=MAX_UINT64-N&&Z&&A>=B&&A-B<=N&&Z<=N-(A-B);}
STATIC BOOLEAN DcrAlias(CONST VOID *A,UINTN N,CONST VOID *B,UINTN Z){UINTN X=(UINTN)A,Y=(UINTN)B;return !N||!Z||N>MAX_UINTN-X||Z>MAX_UINTN-Y||(X<Y+Z&&Y<X+N);}
STATIC BOOLEAN DcrLive(PIANO_DISPLAY_CLOCK_READ *S){if(S->Report.ServicesLost||S->Env.BootServicesAlive(S->Env.Context)!=TRUE){S->Report.ServicesLost=S->Report.Retained=TRUE;return FALSE;}return TRUE;}
STATIC BOOLEAN DcrGuardAlive(VOID *Context){return DcrLive(Context);}
STATIC EFI_STATUS DcrFail(PIANO_DISPLAY_CLOCK_READ *S,EFI_STATUS E){if(!DcrLive(S))E=EFI_ABORTED;return S->Report.Status=DcrExact(E);}
VOID PianoDisplayClockReadFenceExit(PIANO_DISPLAY_CLOCK_READ *S){if(S)S->Report.ServicesLost=S->Report.Retained=TRUE;}
STATIC EFI_STATUS DcrFree(PIANO_DISPLAY_CLOCK_READ *S){
 if(!S->PinnedCopy)return EFI_SUCCESS;if(!DcrLive(S))return EFI_ABORTED;
 EFI_STATUS E=S->Env.Services->FreePool(S->PinnedCopy);if(!DcrLive(S))return EFI_ABORTED;
 if(E==EFI_SUCCESS){S->PinnedCopy=NULL;S->PinnedBytes=0;}else S->Report.Retained=TRUE;return DcrExact(E);
}
EFI_STATUS PianoDisplayClockReadInitialize(PIANO_DISPLAY_CLOCK_READ *S,CONST PIANO_DISPLAY_CLOCK_READ_ENV *E){
 if(!S||!E||S->Signature||DcrAlias(S,sizeof(*S),E,sizeof(*E))||!E->Lease||DcrAlias(S,sizeof(*S),E->Lease,sizeof(*E->Lease)))return EFI_INVALID_PARAMETER;
 if(!E->Services||!E->DxeServices||!E->BootServicesAlive)return EFI_INVALID_PARAMETER;
 if(S->Report.ServicesLost||S->Report.Retained||E->BootServicesAlive(E->Context)!=TRUE)return EFI_NOT_READY;
 if(!E->Services->HandleProtocol||!E->Services->GetMemoryMap||!E->Services->FreePool||!E->DxeServices->GetMemorySpaceDescriptor)return EFI_UNSUPPORTED;
 S->Signature=DCR_SIGNATURE;S->Env=*E;S->Report.Revision=PIANO_DISPLAY_CLOCK_READ_REVISION;S->NextText=DCR_TEXT_FIRST;
 EFI_STATUS Q=GetSectionFromAnyFv(&mDcrImageGuid,EFI_SECTION_PE32,0,&S->PinnedCopy,&S->PinnedBytes);
 if(!DcrLive(S))return DcrFail(S,EFI_ABORTED);
 if(Q!=EFI_SUCCESS||!S->PinnedCopy||S->PinnedBytes!=DCR_IMAGE_BYTES){S->Report.PinStatus=DcrExact(Q==EFI_SUCCESS?EFI_COMPROMISED_DATA:Q);goto Done;}
 UINT8 H[32];S->Report.PinStatus=Sha256HashAll(S->PinnedCopy,S->PinnedBytes,H)&&!CompareMem(H,mDcrPin,sizeof(H))?EFI_SUCCESS:EFI_SECURITY_VIOLATION;
Done:
 if(S->Report.PinStatus!=EFI_SUCCESS){EFI_STATUS F=DcrFree(S);return DcrFail(S,F==EFI_SUCCESS?S->Report.PinStatus:F);}return S->Report.Status=EFI_SUCCESS;
}
STATIC EFI_STATUS DcrRelocate(PIANO_DISPLAY_CLOCK_READ *S){
 UINT8 *B=S->PinnedCopy;if(!B||S->PinnedBytes!=DCR_IMAGE_BYTES)return EFI_NOT_READY;
 for(UINTN At=0x41000;At<DCR_IMAGE_BYTES;){UINT32 Page,N;CopyMem(&Page,B+At,4);CopyMem(&N,B+At+4,4);if(!N)break;
  if(N<8||(N&3)||N>DCR_IMAGE_BYTES-At)return EFI_COMPROMISED_DATA;
  for(UINTN X=At+8;X<At+N;X+=2){UINT16 V;CopyMem(&V,B+X,2);if(!(V>>12))continue;if((V>>12)!=10)return EFI_UNSUPPORTED;
   UINT64 R=(UINT64)Page+(V&4095);if(R<DCR_TEXT_FIRST||R>=DCR_TEXT_END)continue;if(R>DCR_TEXT_END-8)return EFI_COMPROMISED_DATA;
   UINT64 P;CopyMem(&P,B+(UINTN)R,8);if(P>MAX_UINT64-S->ImageBase)return EFI_COMPROMISED_DATA;P+=S->ImageBase;CopyMem(B+(UINTN)R,&P,8);
  }At+=N;
 }return EFI_SUCCESS;
}
STATIC EFI_STATUS DcrIdentity(PIANO_DISPLAY_CLOCK_READ *S){
 if(!DcrLive(S))return EFI_ABORTED;PIANO_DISPLAY_CLOCK_LEASE *P=S->Env.Lease;
 if(!P->NativeImage||!P->ImageIdentity||!P->ImageBase||P->ImageSize!=DCR_IMAGE_BYTES||P->Report.NativeBase!=(UINT64)(UINTN)P->ImageBase||
    (!DcrSpan(0xbd980000,0x174a3000,(UINT64)(UINTN)P->ImageBase,DCR_IMAGE_BYTES)&&!DcrSpan(0xd5100000,0x2f00000,(UINT64)(UINTN)P->ImageBase,DCR_IMAGE_BYTES)))return EFI_NOT_READY;
 EFI_LOADED_IMAGE_PROTOCOL *L=NULL;EFI_STATUS E=S->Env.Services->HandleProtocol(P->NativeImage,&gEfiLoadedImageProtocolGuid,(VOID**)&L);
 if(!DcrLive(S))return EFI_ABORTED;if(E!=EFI_SUCCESS)return DcrExact(E);
 if(!L||L!=P->ImageIdentity||L->Revision<EFI_LOADED_IMAGE_PROTOCOL_REVISION||L->ImageBase!=P->ImageBase||L->ImageSize!=DCR_IMAGE_BYTES||L->ImageCodeType!=EfiBootServicesCode||L->ImageDataType!=EfiBootServicesData)return EFI_COMPROMISED_DATA;
 if(!S->ImageHandle){S->ImageHandle=P->NativeImage;S->ImageIdentity=L;S->ImageBase=(UINT64)(UINTN)L->ImageBase;return DcrRelocate(S);}
 return S->ImageHandle==P->NativeImage&&S->ImageIdentity==L&&S->ImageBase==(UINT64)(UINTN)L->ImageBase?EFI_SUCCESS:EFI_MEDIA_CHANGED;
}
STATIC EFI_STATUS DcrMap(PIANO_DISPLAY_CLOCK_READ *S,CONST DCR_OBJECT *O){
 if(!DcrLive(S))return EFI_ABORTED;UINTN N=sizeof(S->Map),Key=0,Stride=0;UINT32 Version=0;
 EFI_STATUS E=S->Env.Services->GetMemoryMap(&N,(VOID*)S->Map,&Key,&Stride,&Version);if(!DcrLive(S))return EFI_ABORTED;if(E!=EFI_SUCCESS)return DcrExact(E);
 if(!N||N>sizeof(S->Map)||Stride<sizeof(EFI_MEMORY_DESCRIPTOR)||Stride>256||(Stride&7)||N%Stride||Version!=EFI_MEMORY_DESCRIPTOR_VERSION||N/Stride>1024)return EFI_COMPROMISED_DATA;
 UINT64 Cursor=O->Base,End=O->Base+O->Bytes;
 for(UINTN I=0;I<N;I+=Stride){EFI_MEMORY_DESCRIPTOR A;CopyMem(&A,(UINT8*)S->Map+I,sizeof(A));
  if(!A.NumberOfPages||A.NumberOfPages>MAX_UINT64/4096||(A.PhysicalStart&4095)||A.PhysicalStart>MAX_UINT64-A.NumberOfPages*4096)return EFI_COMPROMISED_DATA;
  for(UINTN J=0;J<I;J+=Stride){EFI_MEMORY_DESCRIPTOR B;CopyMem(&B,(UINT8*)S->Map+J,sizeof(B));if(A.PhysicalStart<B.PhysicalStart+B.NumberOfPages*4096&&B.PhysicalStart<A.PhysicalStart+A.NumberOfPages*4096)return EFI_COMPROMISED_DATA;}
 }
 while(Cursor<End){BOOLEAN Found=FALSE;for(UINTN I=0;I<N;I+=Stride){EFI_MEMORY_DESCRIPTOR A;CopyMem(&A,(UINT8*)S->Map+I,sizeof(A));UINT64 AE=A.PhysicalStart+A.NumberOfPages*4096;
   if(Cursor<A.PhysicalStart||Cursor>=AE)continue;
   BOOLEAN Image=O->Role==PianoClockReadText||O->Role==PianoClockReadImageData||DcrSpan(S->ImageBase+DCR_TEXT_END,DCR_DATA_END-DCR_TEXT_END,O->Base,O->Bytes);
   if((Image?A.Type!=EfiBootServicesData&&A.Type!=EfiBootServicesCode:A.Type!=EfiBootServicesData)||
      (A.Attribute&DCR_CACHE_TYPES)!=EFI_MEMORY_WB||(A.Attribute&(EFI_MEMORY_RP|EFI_MEMORY_RUNTIME))||
      (A.VirtualStart&&A.VirtualStart!=A.PhysicalStart))return EFI_NOT_READY;
   Cursor=MIN(AE,End);Found=TRUE;break;
  }if(!Found)return EFI_NOT_FOUND;
 }return EFI_SUCCESS;
}
STATIC BOOLEAN DcrObject(PIANO_DISPLAY_CLOCK_READ *S,DCR_OBJECT O){
 if(!O.Base||!O.Bytes||O.Base>MAX_UINT64-O.Bytes||(O.Base&3)||(O.Bytes&3))return FALSE;
 if(O.Role==PianoClockReadClient||O.Role==PianoClockReadRegistryEntry||O.Role==PianoClockReadClientRef){
  if(O.Base<S->ImageBase+DCR_IMAGE_BYTES&&S->ImageBase<O.Base+O.Bytes)return FALSE;
  return DcrSpan(0xbd980000,0x174a3000,O.Base,O.Bytes)||DcrSpan(0xd5100000,0x2f00000,O.Base,O.Bytes);
 }
 if(DcrSpan(S->ImageBase+DCR_TEXT_END,DCR_DATA_END-DCR_TEXT_END,O.Base,O.Bytes))return TRUE;
 return O.Role!=PianoClockReadText&&O.Role!=PianoClockReadImageData&&
  (DcrSpan(0xbd980000,0x174a3000,O.Base,O.Bytes)||DcrSpan(0xd5100000,0x2f00000,O.Base,O.Bytes));
}
STATIC EFI_STATUS DcrReadObject(PIANO_DISPLAY_CLOCK_READ *S,DCR_OBJECT O,UINT64 A,UINTN N,VOID *Out){
 if(!DcrSpan(O.Base,O.Bytes,A,N)||!Out||!N||N>256)return EFI_INVALID_PARAMETER;
 UINT64 Start=A&~3ULL,End=(A+N+3)&~3ULL;UINTN Size=(UINTN)(End-Start);if(Size>256||!DcrSpan(O.Base,O.Bytes,Start,Size))return EFI_INVALID_PARAMETER;
 EFI_STATUS E=S->Report.IdentityStatus=DcrIdentity(S);if(E!=EFI_SUCCESS)return E;
 // Text object is exact known PE section; data/dynamic objects are bounded.
 if(O.Role!=PianoClockReadText&&!DcrObject(S,O))return EFI_NOT_READY;
 S->Report.MapStatus=E=DcrMap(S,&O);if(E!=EFI_SUCCESS)return E;
 PIANO_GUARDED_CONFIG C={.Context=S,.Services=S->Env.Services,.DxeServices=S->Env.DxeServices,.BootServicesAlive=DcrGuardAlive,
  .Ranges={{O.Base,O.Bytes,EfiGcdMemoryTypeSystemMemory,EFI_MEMORY_WB,0xff}},.RangeCount=1,.MaxReads=(UINT32)(Size/4),.MaxUsecs=100000};
 VOID *Token=NULL;UINT32 Scratch[64];++S->Report.Sessions;E=PianoGuardedReadBegin(&C,&Token);BOOLEAN Began=E==EFI_SUCCESS;
 if(Began&&!Token){S->Report.Retained=TRUE;E=EFI_COMPROMISED_DATA;S->Report.EndStatus=EFI_NOT_STARTED;}
 else if(Began){E=PianoGuardedTryRead(Token,Start,Size,Scratch);S->Report.EndStatus=PianoGuardedReadEnd(Token);}else S->Report.EndStatus=EFI_NOT_STARTED;
 S->Report.Guard=*PianoGuardedReadReport();S->Report.Words+=S->Report.Guard.Reads;
 if(S->Report.Guard.Active||S->Report.Guard.SyncOwned||S->Report.Guard.SErrorOwned||S->Report.Guard.Fatal||S->Report.Guard.Retained||S->Report.Guard.ServicesLost||(Began&&S->Report.EndStatus!=EFI_SUCCESS))S->Report.Retained=TRUE;
 S->Report.ServicesLost|=S->Report.Guard.ServicesLost;
 if(!DcrLive(S))return EFI_ABORTED;if(E!=EFI_SUCCESS)return DcrExact(E);
 if(S->Report.Retained||S->Report.EndStatus!=EFI_SUCCESS)return DcrExact(S->Report.EndStatus==EFI_SUCCESS?EFI_COMPROMISED_DATA:S->Report.EndStatus);
 S->Report.IdentityStatus=E=DcrIdentity(S);if(E!=EFI_SUCCESS)return E;S->Report.MapStatus=E=DcrMap(S,&O);if(E!=EFI_SUCCESS)return E;
 CopyMem(Out,(UINT8*)Scratch+(UINTN)(A-Start),N);ZeroMem(Scratch,sizeof(Scratch));return EFI_SUCCESS;
}
STATIC EFI_STATUS DcrStable(PIANO_DISPLAY_CLOCK_READ *S,DCR_OBJECT O,UINT64 A,UINTN N,VOID *Out){
 UINT8 One[256],Two[256];EFI_STATUS E=DcrReadObject(S,O,A,N,One);if(E!=EFI_SUCCESS)return E;E=DcrReadObject(S,O,A,N,Two);if(E!=EFI_SUCCESS)return E;
 if(CompareMem(One,Two,N))return EFI_MEDIA_CHANGED;CopyMem(Out,One,N);++S->Report.AnchorSnapshots;return EFI_SUCCESS;
}
STATIC BOOLEAN DcrField(UINT64 B,UINT64 A,UINTN N,UINTN Offset,UINTN Size){return B<=MAX_UINT64-Offset&&A==B+Offset&&N==Size;}
STATIC BOOLEAN DcrSameObject(CONST DCR_OBJECT *A,CONST DCR_OBJECT *B){return A->Role==B->Role&&A->Base==B->Base&&A->Bytes==B->Bytes&&A->Anchor==B->Anchor;}
STATIC DCR_OBJECT DcrData(PIANO_DISPLAY_CLOCK_READ *S,UINT64 A,UINTN N){return (DCR_OBJECT){PianoClockReadImageData,A&~3ULL,((A+N+3)&~3ULL)-(A&~3ULL),S->ImageBase};}
STATIC EFI_STATUS DcrResolve(PIANO_DISPLAY_CLOCK_READ *S,UINT64 A,UINTN N,DCR_OBJECT *O,DCR_GRAPH *G){
 ZeroMem(G,sizeof(*G));UINT64 B=S->ImageBase;
 if(DcrSpan(B+DCR_TEXT_FIRST,DCR_TEXT_END-DCR_TEXT_FIRST,A,N)){*O=(DCR_OBJECT){PianoClockReadText,A,N,B};return EFI_SUCCESS;}
 if(DcrSpan(B+DCR_TEXT_END,DCR_DATA_END-DCR_TEXT_END,A,N)){if(S->NextText!=DCR_TEXT_END)return EFI_NOT_READY;*O=DcrData(S,A,N);return EFI_SUCCESS;}
 if(S->NextText!=DCR_TEXT_END)return EFI_NOT_READY;
 EFI_STATUS E=DcrStable(S,DcrData(S,B+0x3fe48,8),B+0x3fe48,8,&G->Global);if(E!=EFI_SUCCESS)return E;
 E=DcrStable(S,DcrData(S,B+0x3f5f0,8),B+0x3f5f0,8,&G->Client);if(E!=EFI_SUCCESS)return E;
 // Pinned BSP Global/Modules/Module/Parent are image-owned static data. An
 // alternate BSP pointer is unknown, never a general low-heap read grant.
 if(G->Global!=B+0x283a0)return EFI_NOT_READY;
 DCR_OBJECT Global={PianoClockReadGlobal,G->Global,0x30,B+0x3fe48},Client={PianoClockReadClient,G->Client,0x20,B+0x3f5f0};
 if(!DcrObject(S,Global)||!DcrObject(S,Client))return EFI_NOT_READY;
 if(DcrField(G->Global,A,N,0,8)||DcrField(G->Global,A,N,8,4)||DcrField(G->Global,A,N,0x2c,4)){*O=Global;return EFI_SUCCESS;}
 // The pinned DAL attach creates a 32B INTERNAL id0 registry entry and 32B
 // client, writes RootDrvCtxt+98, then protocol entry copies that pointer to
 // 3f5f0. Prove both roots and both list memberships; a cookie is insufficient.
 E=DcrStable(S,DcrData(S,B+0x3fed8,8),B+0x3fed8,8,&G->RootClient);if(E!=EFI_SUCCESS)return E;if(G->Client!=G->RootClient)return EFI_NOT_READY;
 E=DcrStable(S,DcrData(S,B+0x3fed0,8),B+0x3fed0,8,&G->Registry);if(E!=EFI_SUCCESS)return E;
 UINT64 Entry=G->Registry;while(Entry){if(G->Entries>=64)return EFI_COMPROMISED_DATA;for(UINT32 I=0;I<G->Entries;++I)if(G->RegistrySeen[I]==Entry)return EFI_COMPROMISED_DATA;
  UINT32 I=G->Entries++;G->RegistrySeen[I]=Entry;DCR_OBJECT R={PianoClockReadRegistryEntry,Entry,0x20,I?G->RegistrySeen[I-1]:B+0x3fed0};if(!DcrObject(S,R))return EFI_NOT_READY;
  E=DcrStable(S,R,Entry,8,&G->RegistryNext[I]);if(E!=EFI_SUCCESS)return E;E=DcrStable(S,R,Entry+8,4,&G->RegistryId[I]);if(E!=EFI_SUCCESS)return E;
  E=DcrStable(S,R,Entry+0x10,8,&G->RegistryName[I]);if(E!=EFI_SUCCESS)return E;
  if(!G->RegistryId[I]){if(G->RegistryName[I]!=B+0x25533)return EFI_COMPROMISED_DATA;G->Entry=Entry;break;}Entry=G->RegistryNext[I];
 }if(!G->Entry)return EFI_NOT_READY;
 DCR_OBJECT Registration={PianoClockReadRegistryEntry,G->Entry,0x20,B+0x3fed0};UINT64 ClientNode=0;
 E=DcrStable(S,Registration,G->Entry+0x18,8,&ClientNode);if(E!=EFI_SUCCESS)return E;
 BOOLEAN Found=FALSE;while(ClientNode){if(G->Clients>=64)return EFI_COMPROMISED_DATA;for(UINT32 I=0;I<G->Clients;++I)if(G->ClientSeen[I]==ClientNode)return EFI_COMPROMISED_DATA;
  UINT32 I=G->Clients++;G->ClientSeen[I]=ClientNode;DCR_OBJECT C={PianoClockReadClient,ClientNode,0x20,I?G->ClientSeen[I-1]:G->Entry+0x18};if(!DcrObject(S,C))return EFI_NOT_READY;
  E=DcrStable(S,C,ClientNode,8,&G->ClientNext[I]);if(E!=EFI_SUCCESS)return E;E=DcrStable(S,C,ClientNode+0x10,8,&G->ClientBack[I]);if(E!=EFI_SUCCESS)return E;
  if(G->ClientBack[I]!=G->Entry)return EFI_COMPROMISED_DATA;if(ClientNode==G->Client){Found=TRUE;break;}ClientNode=G->ClientNext[I];
 }if(!Found)return EFI_NOT_READY;
 UINTN Id=S->Env.Lease->Report.ClockId;if(Id>MAX_UINT32||((Id>>16)&255)!=1)return EFI_NOT_READY;G->Provider=(UINT32)(Id>>24)&255;G->Index=(UINT32)Id&65535;
 E=DcrStable(S,Global,G->Global,8,&G->Modules);if(E!=EFI_SUCCESS)return E;E=DcrStable(S,Global,G->Global+8,4,&G->ModuleCount);if(E!=EFI_SUCCESS)return E;
 if(G->Modules!=B+0x28308||G->ModuleCount!=9||G->Provider>=G->ModuleCount)return EFI_COMPROMISED_DATA;
 DCR_OBJECT Modules={PianoClockReadModules,G->Modules,8*(UINT64)G->ModuleCount,G->Global};if(!DcrObject(S,Modules))return EFI_NOT_READY;
 UINT64 ModuleEntry=G->Modules+8*(UINT64)G->Provider;if(A==ModuleEntry&&N==8){*O=Modules;return EFI_SUCCESS;}
 E=DcrStable(S,Modules,ModuleEntry,8,&G->Module);if(E!=EFI_SUCCESS)return E;if(G->Module!=B+0x28678)return EFI_NOT_READY;DCR_OBJECT Module={PianoClockReadModule,G->Module,0x48,ModuleEntry};if(!DcrObject(S,Module))return EFI_NOT_READY;
 if(DcrField(G->Module,A,N,0x38,8)||DcrField(G->Module,A,N,0x40,4)){*O=Module;return EFI_SUCCESS;}
 E=DcrStable(S,Module,G->Module+0x38,8,&G->Array);if(E!=EFI_SUCCESS)return E;E=DcrStable(S,Module,G->Module+0x40,4,&G->ClockCount);if(E!=EFI_SUCCESS)return E;
 if(G->ClockCount!=149||G->Array!=B+0x32418||G->Index>=G->ClockCount||G->Array>MAX_UINT64-112*(UINT64)G->Index||G->Array+112*(UINT64)G->Index!=B+DCR_GCC_NODE)return EFI_COMPROMISED_DATA;
 E=DcrStable(S,DcrData(S,B+DCR_GCC_NODE,8),B+DCR_GCC_NODE,8,&G->Name);if(E!=EFI_SUCCESS)return E;if(G->Name!=B+0x14e02)return EFI_COMPROMISED_DATA;
 E=DcrStable(S,DcrData(S,B+DCR_GCC_NODE+8,8),B+DCR_GCC_NODE+8,8,&G->Parent);if(E!=EFI_SUCCESS)return E;
 if(G->Parent!=B+0x37528)return EFI_NOT_READY;
 if(DcrField(G->Client,A,N,0x19,1)){*O=Client;return EFI_SUCCESS;}
 if(DcrField(G->Parent,A,N,0x18,4)){*O=(DCR_OBJECT){PianoClockReadParent,G->Parent,0x1c,B+DCR_GCC_NODE+8};return DcrObject(S,*O)?EFI_SUCCESS:EFI_NOT_READY;}
 E=DcrStable(S,DcrData(S,B+DCR_GCC_NODE+0x58,8),B+DCR_GCC_NODE+0x58,8,&G->Head);if(E!=EFI_SUCCESS)return E;
 UINT64 Node=G->Head;while(Node){if(G->Used>=64)return EFI_COMPROMISED_DATA;for(UINT32 I=0;I<G->Used;++I)if(G->Seen[I]==Node)return EFI_COMPROMISED_DATA;
  UINT32 I=G->Used++;G->Seen[I]=Node;DCR_OBJECT Ref={PianoClockReadClientRef,Node,0x18,I?G->Seen[I-1]:B+DCR_GCC_NODE+0x58};if(!DcrObject(S,Ref))return EFI_NOT_READY;
  UINT64 Pair[2];E=DcrStable(S,Ref,Node,16,Pair);if(E!=EFI_SUCCESS)return E;G->Next[I]=Pair[0];G->ClientAt[I]=Pair[1];
  if(DcrField(Node,A,N,0,8)||DcrField(Node,A,N,8,8)||(Pair[1]==G->Client&&DcrField(Node,A,N,0x10,4))){*O=Ref;G->Node=Node;return EFI_SUCCESS;}Node=Pair[0];
 }return EFI_ACCESS_DENIED;
}
EFI_STATUS PianoDisplayClockReadCpu(VOID *Context,UINT64 A,UINTN N,VOID *Out){
 PIANO_DISPLAY_CLOCK_READ *S=Context;if(!S||S->Signature!=DCR_SIGNATURE||!Out||!A||!N||N>256||A>MAX_UINT64-N-3||DcrAlias(Out,N,S,sizeof(*S))||DcrAlias(Out,N,&S->Env.Lease->Env,sizeof(S->Env.Lease->Env))||DcrAlias(Out,N,&S->Env.Lease->NativeImage,sizeof(S->Env.Lease->NativeImage))||DcrAlias(Out,N,&S->Env.Lease->ImageIdentity,sizeof(S->Env.Lease->ImageIdentity))||DcrAlias(Out,N,&S->Env.Lease->ImageBase,sizeof(S->Env.Lease->ImageBase))||DcrAlias(Out,N,&S->Env.Lease->ImageSize,sizeof(S->Env.Lease->ImageSize))||DcrAlias(Out,N,&S->Env.Lease->Report.NativeBase,sizeof(S->Env.Lease->Report.NativeBase))||DcrAlias(Out,N,&S->Env.Lease->Report.ClockId,sizeof(S->Env.Lease->Report.ClockId))||DcrAlias(Out,N,S->Env.Services,sizeof(*S->Env.Services))||DcrAlias(Out,N,S->Env.DxeServices,sizeof(*S->Env.DxeServices))||DcrAlias(Out,N,(VOID*)(UINTN)A,N))return EFI_INVALID_PARAMETER;
 if((S->ImageBase&&DcrAlias(Out,N,(VOID*)(UINTN)S->ImageBase,DCR_IMAGE_BYTES))||(S->PinnedCopy&&DcrAlias(Out,N,S->PinnedCopy,S->PinnedBytes))||DcrAlias(Out,N,S->Env.Lease->ImageIdentity,sizeof(*S->Env.Lease->ImageIdentity)))return EFI_INVALID_PARAMETER;
 if(S->Report.Retained||S->Report.ServicesLost||S->Report.Busy||S->Report.PinStatus!=EFI_SUCCESS)return EFI_ACCESS_DENIED;S->Report.Busy=TRUE;
 DCR_OBJECT O={0},After={0};DCR_GRAPH G={0},H={0};UINT8 Scratch[256];EFI_STATUS E=S->Report.IdentityStatus=DcrIdentity(S);if(E!=EFI_SUCCESS)goto Done;
 E=DcrResolve(S,A,N,&O,&G);if(E!=EFI_SUCCESS)goto Done;
 if(O.Role==PianoClockReadText){if(A!=S->ImageBase+S->NextText||(A&3)||(N&3)||!S->PinnedCopy){E=EFI_ACCESS_DENIED;goto Done;}
  E=DcrReadObject(S,O,A,N,Scratch);if(E!=EFI_SUCCESS)goto Done;if(CompareMem(Scratch,(UINT8*)S->PinnedCopy+S->NextText,N)){E=EFI_COMPROMISED_DATA;goto Done;}
  if(S->NextText+N==DCR_TEXT_END){E=DcrFree(S);if(E!=EFI_SUCCESS)goto Done;S->Report.TextVerified=TRUE;}S->NextText+=N;
 }else{
  E=DcrStable(S,O,A,N,Scratch);if(E!=EFI_SUCCESS)goto Done;E=DcrResolve(S,A,N,&After,&H);if(E!=EFI_SUCCESS)goto Done;
  if(!DcrSameObject(&O,&After)||CompareMem(&G,&H,sizeof(G))){E=EFI_MEDIA_CHANGED;goto Done;}
 }
 if(!DcrLive(S)){E=EFI_ABORTED;goto Done;}CopyMem(Out,Scratch,N);
Done:
 ZeroMem(Scratch,sizeof(Scratch));S->Report.Address=A;S->Report.Role=O.Role;S->Report.ObjectBase=O.Base;S->Report.ObjectBytes=O.Bytes;S->Report.ProducerAnchor=O.Anchor;S->Report.ClientNodes=G.Used;S->Report.Busy=FALSE;return DcrFail(S,E);
}
EFI_STATUS PianoDisplayClockReadClose(PIANO_DISPLAY_CLOCK_READ *S){if(!S||S->Signature!=DCR_SIGNATURE)return EFI_INVALID_PARAMETER;if(S->Report.Busy||S->Report.Retained||S->Report.ServicesLost)return EFI_ACCESS_DENIED;return DcrFail(S,DcrFree(S));}
