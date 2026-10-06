// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoDisplayRailObserve.h"
#include <Guid/EventGroup.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#define RAIL_SIGNATURE SIGNATURE_32('P','R','O','B')
STATIC EFI_GUID mNpa={0x79d6c870,0x725e,0x489e,{0xa0,0xa1,0x27,0xe7,0xa5,0xd0,0xcb,0x35}};
STATIC CONST struct {EFI_GUID Guid;UINT32 Bytes,CodeEnd,Reloc;UINT8 Hash[32];} mPin[2]={
  {{0xcb29f4d1,0x7f37,0x4692,{0xa4,0x16,0x93,0xe8,0x2e,0x21,0x97,0x11}},0x13000,0xddac,0x12000,
   {0x19,0xa7,0xdf,0x5c,0xdf,0x25,0xae,0x24,0x7c,0xa5,0x62,0x8b,0xd6,0x74,0x96,0x72,0x9f,0x17,0xf3,0x0d,0x74,0xe5,0x64,0xa7,0xd7,0xf9,0x99,0xac,0x2d,0x59,0x34,0xb9}},
  {{0x8bd3b475,0x401a,0x4b0b,{0x93,0x15,0xed,0xee,0x61,0xa1,0xea,0xe5}},0x11000,0x7f4c,0x10000,
   {0xd3,0x35,0xdb,0xcf,0xd4,0x17,0x5e,0x6d,0x9f,0x18,0x90,0x62,0xbc,0x77,0x79,0x61,0xf4,0x97,0xe9,0x68,0xd6,0x6a,0xcc,0x7d,0xac,0xb4,0xaa,0xe8,0xb3,0x05,0x5e,0x4f}}};
STATIC EFI_STATUS Exact(EFI_STATUS E){return E==EFI_SUCCESS?E:EFI_ERROR(E)?E:EFI_DEVICE_ERROR;}
STATIC BOOLEAN Alias(CONST VOID *A,UINTN N,CONST VOID *B,UINTN Z){UINTN X=(UINTN)A,Y=(UINTN)B;return N>MAX_UINTN-X||Z>MAX_UINTN-Y||(X<Y+Z&&Y<X+N);}
STATIC BOOLEAN Span(UINT64 B,UINT64 N,UINT64 A,UINTN Z){return B&&N&&B<=MAX_UINT64-N&&Z&&A>=B&&A-B<=N&&Z<=N-(A-B);}
STATIC BOOLEAN Heap(UINT64 A,UINTN N){return Span(0xbd980000,0x174a3000,A,N)||Span(0xd5100000,0x2f00000,A,N);}
STATIC BOOLEAN Live(PIANO_DISPLAY_RAIL_OBSERVER *S){if(S->Report.ServicesLost||S->Env.Alive(S->Env.Context)!=TRUE){S->Report.ServicesLost=S->Report.Retained=TRUE;return FALSE;}return TRUE;}
STATIC BOOLEAN GuardAlive(VOID *Context){return Live(Context);}
STATIC EFI_STATUS Fail(PIANO_DISPLAY_RAIL_OBSERVER *S,EFI_STATUS E){if(!Live(S))E=EFI_ABORTED;S->Report.Busy=FALSE;return S->Report.Status=Exact(E);}
STATIC VOID EFIAPI Exit(EFI_EVENT Event,VOID *Context){(VOID)Event;PIANO_DISPLAY_RAIL_OBSERVER *S=Context;S->Report.ServicesLost=S->Report.Retained=TRUE;}
STATIC EFI_STATUS App(PIANO_DISPLAY_RAIL_OBSERVER *S){if(!Live(S))return EFI_ABORTED;EFI_TPL T=S->Env.Services->RaiseTPL(TPL_HIGH_LEVEL);if(!Live(S))return EFI_ABORTED;S->Env.Services->RestoreTPL(T);return !Live(S)?EFI_ABORTED:T==TPL_APPLICATION?EFI_SUCCESS:EFI_UNSUPPORTED;}
STATIC EFI_STATUS Free(PIANO_DISPLAY_RAIL_OBSERVER *S,VOID **P){if(!*P)return EFI_SUCCESS;if(!Live(S))return EFI_ABORTED;EFI_STATUS E=S->Env.Services->FreePool(*P);if(!Live(S))return EFI_ABORTED;if(E==EFI_SUCCESS)*P=NULL;else S->Report.Retained=TRUE;return Exact(E);}
STATIC EFI_STATUS Map(PIANO_DISPLAY_RAIL_OBSERVER *S,UINT64 A,UINTN N,BOOLEAN Image){
  if(!Heap(A,N))return EFI_ACCESS_DENIED;if(!Live(S))return EFI_ABORTED;UINTN Bytes=sizeof(S->Map),Key=0,Stride=0;UINT32 Version=0;
  EFI_STATUS E=S->Env.Services->GetMemoryMap(&Bytes,(VOID *)S->Map,&Key,&Stride,&Version);if(!Live(S))return EFI_ABORTED;if(E!=EFI_SUCCESS)return Exact(E);
  if(!Bytes||Bytes>sizeof(S->Map)||Stride<sizeof(EFI_MEMORY_DESCRIPTOR)||Stride>256||(Stride&7)||Bytes%Stride||Version!=EFI_MEMORY_DESCRIPTOR_VERSION)return EFI_COMPROMISED_DATA;
  for(UINTN I=0;I<Bytes;I+=Stride){EFI_MEMORY_DESCRIPTOR D;CopyMem(&D,(UINT8 *)S->Map+I,sizeof(D));
    if(!D.NumberOfPages||D.NumberOfPages>MAX_UINT64/4096||(D.PhysicalStart&4095)||D.PhysicalStart>MAX_UINT64-D.NumberOfPages*4096)return EFI_COMPROMISED_DATA;
    for(UINTN J=0;J<I;J+=Stride){EFI_MEMORY_DESCRIPTOR P;CopyMem(&P,(UINT8 *)S->Map+J,sizeof(P));if(D.PhysicalStart<P.PhysicalStart+P.NumberOfPages*4096&&P.PhysicalStart<D.PhysicalStart+D.NumberOfPages*4096)return EFI_COMPROMISED_DATA;}
  }
  UINT64 Cursor=A,End=A+N;while(Cursor<End){BOOLEAN Found=FALSE;for(UINTN I=0;I<Bytes;I+=Stride){EFI_MEMORY_DESCRIPTOR D;CopyMem(&D,(UINT8 *)S->Map+I,sizeof(D));UINT64 Last=D.PhysicalStart+D.NumberOfPages*4096;
    if(Cursor<D.PhysicalStart||Cursor>=Last)continue;if((Image?D.Type!=EfiBootServicesCode&&D.Type!=EfiBootServicesData:D.Type!=EfiBootServicesData)||!(D.Attribute&EFI_MEMORY_WB)||(D.Attribute&(EFI_MEMORY_RP|EFI_MEMORY_RUNTIME))||(D.VirtualStart&&D.VirtualStart!=D.PhysicalStart))return EFI_NOT_READY;
    Cursor=MIN(Last,End);Found=TRUE;break;}if(!Found)return EFI_NOT_FOUND;
  }return EFI_SUCCESS;
}
STATIC EFI_STATUS Read(PIANO_DISPLAY_RAIL_OBSERVER *S,UINT64 A,UINTN N,VOID *Out,BOOLEAN Image){
  if(!A||!N||N>256||A>MAX_UINT64-N-3||!Out)return EFI_INVALID_PARAMETER;UINT64 First=A&~3ULL,Last=(A+N+3)&~3ULL;UINTN Bytes=(UINTN)(Last-First);if(Bytes>256)return EFI_INVALID_PARAMETER;
  EFI_STATUS E=Map(S,First,Bytes,Image);if(E!=EFI_SUCCESS)return E;
  PIANO_GUARDED_CONFIG C={.Context=S,.Services=S->Env.Services,.DxeServices=S->Env.DxeServices,.BootServicesAlive=GuardAlive,
    .Ranges={{First,Bytes,EfiGcdMemoryTypeSystemMemory,EFI_MEMORY_WB,0xff}},.RangeCount=1,.MaxReads=(UINT32)(Bytes/4),.MaxUsecs=100000};
  VOID *Token=NULL;UINT32 Scratch[64];++S->Report.Sessions;E=PianoGuardedReadBegin(&C,&Token);EFI_STATUS End=EFI_NOT_STARTED;
  if(E==EFI_SUCCESS&&Token){E=PianoGuardedTryRead(Token,First,Bytes,Scratch);End=PianoGuardedReadEnd(Token);}
  else if(E==EFI_SUCCESS){S->Report.Retained=TRUE;E=EFI_COMPROMISED_DATA;}
  S->Report.Guard=*PianoGuardedReadReport();S->Report.Reads+=S->Report.Guard.Reads;
  CONST PIANO_GUARDED_REPORT *G=&S->Report.Guard;
  if(G->Retained||G->ServicesLost||G->Fatal||G->Active||G->SyncOwned||G->SErrorOwned||(Token&&End!=EFI_SUCCESS))S->Report.Retained=TRUE;
  if(!Live(S))return EFI_ABORTED;if(S->Report.Retained)return End==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(End==EFI_NOT_STARTED?E:End);
  if(E!=EFI_SUCCESS)return Exact(E);CopyMem(Out,(UINT8 *)Scratch+(UINTN)(A-First),N);ZeroMem(Scratch,sizeof(Scratch));return EFI_SUCCESS;
}
STATIC EFI_STATUS Loaded(PIANO_DISPLAY_RAIL_OBSERVER *S,UINT32 I){
  PIANO_DISPLAY_RAIL_IMAGE *P=&S->Image[I];if(!P->Handle||!P->Loaded||!P->Base)return EFI_NOT_READY;
  if(!Live(S))return EFI_ABORTED;EFI_LOADED_IMAGE_PROTOCOL *L=NULL;EFI_STATUS E=S->Env.Services->HandleProtocol(P->Handle,&gEfiLoadedImageProtocolGuid,(VOID **)&L);
  if(!Live(S))return EFI_ABORTED;if(E!=EFI_SUCCESS)return Exact(E);
  return L==P->Loaded&&L->Revision>=EFI_LOADED_IMAGE_PROTOCOL_REVISION&&(UINT64)(UINTN)L->ImageBase==P->Base&&L->ImageSize==mPin[I].Bytes&&L->ImageCodeType==EfiBootServicesCode&&L->ImageDataType==EfiBootServicesData?EFI_SUCCESS:EFI_MEDIA_CHANGED;
}
STATIC EFI_STATUS Normalize(VOID *Copy,UINT32 I,UINT64 Base){
  UINT8 *B=Copy;for(UINTN At=mPin[I].Reloc;At<mPin[I].Bytes;){if(mPin[I].Bytes-At<8)return EFI_COMPROMISED_DATA;UINT32 Page,N;CopyMem(&Page,B+At,4);CopyMem(&N,B+At+4,4);if(!N)break;if(N<8||(N&3)||N>mPin[I].Bytes-At)return EFI_COMPROMISED_DATA;
    for(UINTN X=At+8;X<At+N;X+=2){UINT16 V;CopyMem(&V,B+X,2);if(!(V>>12))continue;if((V>>12)!=10)return EFI_UNSUPPORTED;UINT64 R=(UINT64)Page+(V&4095);if(R<0x1000||R>=mPin[I].CodeEnd)continue;if(R>mPin[I].CodeEnd-8)return EFI_COMPROMISED_DATA;
      UINT64 Value;CopyMem(&Value,B+R,8);if(Value>MAX_UINT64-Base)return EFI_COMPROMISED_DATA;Value+=Base;CopyMem(B+R,&Value,8);}At+=N;
  }return EFI_SUCCESS;
}
STATIC EFI_STATUS VerifyCode(PIANO_DISPLAY_RAIL_OBSERVER *S,UINT32 I){
  PIANO_DISPLAY_RAIL_IMAGE *P=&S->Image[I];EFI_STATUS E=Normalize(P->Copy,I,P->Base);if(E!=EFI_SUCCESS)return E;
  UINT8 Scratch[256];for(UINTN At=0x1000;At<mPin[I].CodeEnd;At+=sizeof(Scratch)){UINTN N=MIN(sizeof(Scratch),(UINTN)mPin[I].CodeEnd-At);E=Read(S,P->Base+At,N,Scratch,TRUE);if(E!=EFI_SUCCESS)return E;if(CompareMem(Scratch,(UINT8 *)P->Copy+At,N))return EFI_SECURITY_VIOLATION;}
  ZeroMem(Scratch,sizeof(Scratch));P->CodeVerified=TRUE;return Loaded(S,I);
}
STATIC EFI_STATUS Identity(PIANO_DISPLAY_RAIL_OBSERVER *S){
  for(UINT32 I=0;I<2;++I){S->Report.Identity[I]=Loaded(S,I);if(S->Report.Identity[I]!=EFI_SUCCESS)return S->Report.Identity[I];if(!S->Image[I].CodeVerified)return EFI_NOT_READY;}
  if(!Live(S))return EFI_ABORTED;VOID *P=NULL;EFI_STATUS E=S->Env.Services->LocateProtocol(&mNpa,NULL,&P);if(!Live(S))return EFI_ABORTED;
  if(E!=EFI_SUCCESS)return Exact(E);if(P!=S->NpaProtocol||(UINT64)(UINTN)P!=S->Image[0].Base+0x110b8)return EFI_MEDIA_CHANGED;
  UINT64 V[2];E=Read(S,S->Image[0].Base+0x110b8,8,V,TRUE);if(E!=EFI_SUCCESS)return E;if(V[0]!=0x10003)return EFI_UNSUPPORTED;
  E=Read(S,S->Image[0].Base+0x11268,16,V,TRUE);if(E!=EFI_SUCCESS)return E;
  return V[0]==S->Image[0].Base+0x1ae0&&V[1]==S->Image[0].Base+0x1af8?EFI_SUCCESS:EFI_MEDIA_CHANGED;
}
EFI_STATUS PianoDisplayRailInit(PIANO_DISPLAY_RAIL_OBSERVER *S,CONST PIANO_DISPLAY_RAIL_ENV *Env){
  if(S&&S->Signature==RAIL_SIGNATURE)return EFI_ALREADY_STARTED;
  if(!S||!Env||S->Signature||Alias(S,sizeof(*S),Env,sizeof(*Env))||!Env->Services||!Env->DxeServices||!Env->Alive||!Env->ClockReader||!Env->NpaHandle||!Env->VcsHandle||Env->NpaHandle==Env->VcsHandle)return EFI_INVALID_PARAMETER;
  if(Alias(S,sizeof(*S),Env->Services,sizeof(*Env->Services))||Alias(S,sizeof(*S),Env->DxeServices,sizeof(*Env->DxeServices))||Alias(S,sizeof(*S),Env->ClockReader,sizeof(*Env->ClockReader)))return EFI_INVALID_PARAMETER;
  if(Env->Services!=gBS||Env->DxeServices!=gDS)return EFI_UNSUPPORTED;
  S->Signature=RAIL_SIGNATURE;S->Env=*Env;S->Report.Busy=TRUE;S->Report.Revision=1;S->Report.Close=EFI_NOT_STARTED;for(UINT32 I=0;I<2;++I)S->Report.Pin[I]=S->Report.Identity[I]=EFI_NOT_STARTED;
  if(!Live(S))return Fail(S,EFI_ABORTED);
  if(!Env->Services->RaiseTPL||!Env->Services->RestoreTPL||!Env->Services->CreateEventEx||!Env->Services->CloseEvent||!Env->Services->LocateProtocol||!Env->Services->HandleProtocol||!Env->Services->GetMemoryMap||!Env->Services->FreePool){S->Report.Retained=TRUE;return Fail(S,EFI_UNSUPPORTED);}
  EFI_STATUS E=App(S);if(E!=EFI_SUCCESS)return Fail(S,E);E=S->Env.Services->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,S,&gEfiEventExitBootServicesGuid,&S->Exit);
  if(!Live(S))return Fail(S,EFI_ABORTED);if(E!=EFI_SUCCESS||!S->Exit){if(S->Exit||!EFI_ERROR(E))S->Report.Retained=TRUE;return Fail(S,E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);}
  for(UINT32 I=0;I<2;++I){PIANO_DISPLAY_RAIL_IMAGE *P=&S->Image[I];E=GetSectionFromAnyFv(&mPin[I].Guid,EFI_SECTION_PE32,0,&P->Copy,&P->Bytes);if(!Live(S))return Fail(S,EFI_ABORTED);
    if(E!=EFI_SUCCESS){if(P->Copy||!EFI_ERROR(E))S->Report.Retained=TRUE;return Fail(S,E);}UINT8 H[32];
    S->Report.Pin[I]=P->Copy&&P->Bytes==mPin[I].Bytes&&Sha256HashAll(P->Copy,P->Bytes,H)&&!CompareMem(H,mPin[I].Hash,32)?EFI_SUCCESS:EFI_SECURITY_VIOLATION;if(S->Report.Pin[I]!=EFI_SUCCESS)return Fail(S,S->Report.Pin[I]);
  }
  E=S->Env.Services->LocateProtocol(&mNpa,NULL,&S->NpaProtocol);if(!Live(S))return Fail(S,EFI_ABORTED);if(E!=EFI_SUCCESS)return Fail(S,E);
  for(UINT32 I=0;I<2;++I){PIANO_DISPLAY_RAIL_IMAGE *P=&S->Image[I];P->Handle=I?Env->VcsHandle:Env->NpaHandle;
    EFI_LOADED_IMAGE_PROTOCOL *L=NULL;E=S->Env.Services->HandleProtocol(P->Handle,&gEfiLoadedImageProtocolGuid,(VOID **)&L);if(!Live(S))return Fail(S,EFI_ABORTED);if(E!=EFI_SUCCESS)return Fail(S,E);
    if(!L||L->Revision<EFI_LOADED_IMAGE_PROTOCOL_REVISION||L->ImageCodeType!=EfiBootServicesCode||L->ImageDataType!=EfiBootServicesData||L->ImageSize!=mPin[I].Bytes||!Heap((UINT64)(UINTN)L->ImageBase,mPin[I].Bytes))return Fail(S,EFI_COMPROMISED_DATA);
    P->Loaded=L;P->Base=(UINT64)(UINTN)L->ImageBase;if(!I&&(UINT64)(UINTN)S->NpaProtocol!=P->Base+0x110b8)return Fail(S,EFI_MEDIA_CHANGED);
  }
  for(UINT32 I=0;I<2;++I){if(!S->Image[I].Handle)return Fail(S,EFI_NOT_FOUND);S->Report.Identity[I]=VerifyCode(S,I);if(S->Report.Identity[I]!=EFI_SUCCESS)return Fail(S,S->Report.Identity[I]);}
  E=Identity(S);if(E!=EFI_SUCCESS)return Fail(S,E);S->Report.Initialized=TRUE;S->Report.Busy=FALSE;return S->Report.Status=EFI_SUCCESS;
}
STATIC EFI_STATUS Field(PIANO_DISPLAY_RAIL_OBSERVER *S,UINT64 B,UINTN Offset,UINTN N,VOID *Out,BOOLEAN Image){if(!B||B>MAX_UINT64-Offset)return EFI_COMPROMISED_DATA;return Read(S,B+Offset,N,Out,Image);}
STATIC EFI_STATUS Name(PIANO_DISPLAY_RAIL_OBSERVER *S,UINT64 A,CONST CHAR8 *Expected,CHAR8 Out[16]){
  BOOLEAN Image=Span(S->Image[0].Base,mPin[0].Bytes,A,16)||Span(S->Image[1].Base,mPin[1].Bytes,A,16)||Span(S->Env.ClockReader->ImageBase,0x44000,A,16);
  EFI_STATUS E=Read(S,A,16,Out,Image);if(E!=EFI_SUCCESS)return E;UINTN N=0;while(Expected[N])++N;
  if(!CompareMem(Out,Expected,N+1))return EFI_SUCCESS;
  // The pinned UEFI DT declares primary /vcs/vdd_mxa plus /vcs/vdd_mx
  // alias; a lookup through the latter can retain the primary resource name.
  return !CompareMem(Expected,"/vcs/vdd_mx",12)&&!CompareMem(Out,"/vcs/vdd_mxa",13)?EFI_SUCCESS:EFI_COMPROMISED_DATA;
}
#define RF(Base,Offset,FieldName) do{E=Field(S,Base,Offset,sizeof(G->FieldName),&G->FieldName,FALSE);if(E!=EFI_SUCCESS)goto Done;}while(0)
STATIC EFI_STATUS Graph(PIANO_DISPLAY_RAIL_OBSERVER *S,UINT64 Client,CONST CHAR8 *Expected,PIANO_DISPLAY_RAIL_GRAPH *G){
  ZeroMem(G,sizeof(*G));G->Client=Client;EFI_STATUS E=Client?EFI_SUCCESS:EFI_NOT_READY;if(!Client)goto Done;
  RF(Client,0x20,Resource);RF(Client,0x30,ClientType);RF(Client,0x68,ActiveIndex);RF(Client,0x80,RequestAttributes);RF(Client,0x88,RequestCallback);
  if(G->ClientType!=0x40||G->ActiveIndex>1||!Span(S->Image[0].Base+0x1000,mPin[0].CodeEnd-0x1000,G->RequestCallback,4)){E=EFI_COMPROMISED_DATA;goto Done;}
  RF(Client,0x38+24*G->ActiveIndex,ActiveRequest);RF(Client,0x38+24*(G->ActiveIndex^1),PendingRequest);
  RF(G->Resource,0,Definition);RF(G->Resource,0x10,Node);RF(G->Resource,0x30,NpaApplied);RF(G->Resource,0x40,NpaRequired);RF(G->Resource,0x44,NpaSuppressible);
  RF(G->Resource,0x28,Plugin);if(G->Plugin!=S->Image[1].Base+0x98d8){E=EFI_COMPROMISED_DATA;goto Done;}
  UINT64 Plugin[2];E=Read(S,G->Plugin,16,Plugin,TRUE);if(E!=EFI_SUCCESS)goto Done;
  if(Plugin[0]!=S->Image[1].Base+0x6d68||Plugin[1]!=0x844){E=EFI_MEDIA_CHANGED;goto Done;}
  E=Field(S,G->Plugin,0x30,8,&G->RequestMapping,TRUE);if(E!=EFI_SUCCESS)goto Done;if(G->RequestMapping){E=EFI_UNSUPPORTED;goto Done;}
  UINT64 P=0,Q=0;E=Field(S,G->Definition,0,8,&P,FALSE);if(E!=EFI_SUCCESS)goto Done;E=Name(S,P,Expected,G->ResourceName);if(E!=EFI_SUCCESS)goto Done;
  RF(G->Node,8,Driver);RF(G->Node,0x18,Rail);if(G->Driver!=S->Image[1].Base+0x6a8c){E=EFI_COMPROMISED_DATA;goto Done;}
  E=Field(S,G->Definition,0x28,8,&Q,FALSE);if(E!=EFI_SUCCESS)goto Done;if(Q!=G->Rail){E=EFI_COMPROMISED_DATA;goto Done;}
  E=Field(S,G->Rail,0xb8,8,&Q,FALSE);if(E!=EFI_SUCCESS)goto Done;if(Q!=G->Resource){E=EFI_COMPROMISED_DATA;goto Done;}
  E=Field(S,G->Rail,0x78,8,&P,FALSE);if(E!=EFI_SUCCESS)goto Done;E=Name(S,P,Expected,G->RailName);if(E!=EFI_SUCCESS)goto Done;
  RF(G->Rail,0x20,Backend);RF(G->Rail,0x30,VcsApplied);if(G->Backend!=S->Image[1].Base+0xa2c0){E=EFI_COMPROMISED_DATA;goto Done;}
  UINT64 Backend[2];E=Read(S,G->Backend,16,Backend,TRUE);if(E!=EFI_SUCCESS)goto Done;if(Backend[0]!=S->Image[1].Base+0x711c||Backend[1]!=S->Image[1].Base+0x7218){E=EFI_MEDIA_CHANGED;goto Done;}
  RF(G->Rail,0x28,RpmhContext);if(G->RpmhContext){RF(G->RpmhContext,0x10,RpmhConfig);if(G->RpmhConfig){RF(G->RpmhConfig,0,RpmhDrvId);RF(G->RpmhConfig,8,RpmhHandle);}}
Done:return G->Status=Exact(E);
}
#undef RF
STATIC VOID Emit(CONST PIANO_DISPLAY_RAIL_SNAPSHOT *R){
  DEBUG((DEBUG_WARN,"PIANO_RAIL_OBSERVE phase=%a status=%r mm_pair=%u mx_pair=%u retained=%u lost=%u power_ready=0 completion_observed=0\n",R->Phase,R->Status,R->MmCoherent,R->MxCoherent,R->Retained,R->ServicesLost));
  DEBUG((DEBUG_WARN,"PIANO_RAIL_CLOCK status=%r/%r cached=%u config_corner=%u parent_refs=%u/%u railmask=%x\n",R->SelectorBefore,R->SelectorAfter,R->ClockBefore.ParentCachedCorner,R->ClockBefore.CurrentCorner,R->ClockBefore.ParentRefs[0],R->ClockBefore.ParentRefs[1],R->ClockBefore.ParentRailMask));
  DEBUG((DEBUG_WARN,"PIANO_RAIL_ANCHORS mm_client=%lx mx_client=%lx expected_clock_id=%x\n",R->ClockBefore.MmClient,R->ClockBefore.MxClient,R->ClockBefore.ExpectedClockId));
  for(UINT32 I=0;I<2;++I)for(UINT32 J=0;J<2;++J){CONST PIANO_DISPLAY_RAIL_GRAPH *G=I?&R->Mx[J]:&R->Mm[J];
    DEBUG((DEBUG_WARN,"PIANO_RAIL_GRAPH rail=%u round=%u status=%r client=%lx resource=%lx vcs=%lx backend=%lx\n",I,J,G->Status,G->Client,G->Resource,G->Rail,G->Backend));
    DEBUG((DEBUG_WARN,"PIANO_RAIL_STATE rail=%u round=%u active_i=%u type=%x request=%u pending=%u npa=%u vcs=%u\n",I,J,G->ActiveIndex,G->ClientType,G->ActiveRequest,G->PendingRequest,G->NpaApplied,G->VcsApplied));
  }
}
EFI_STATUS PianoDisplayRailObserve(PIANO_DISPLAY_RAIL_OBSERVER *S,CONST CHAR8 *Phase){
  if(!S||S->Signature!=RAIL_SIGNATURE||!Phase)return EFI_INVALID_PARAMETER;UINTN N=0;while(N<32&&Phase[N])++N;if(!N||N==32)return EFI_INVALID_PARAMETER;
  if(S->Report.Busy)return EFI_ALREADY_STARTED;if(S->Report.Retained||S->Report.ServicesLost)return EFI_NOT_READY;if(!S->Report.Initialized)return EFI_NOT_READY;if(S->Report.Count==PIANO_DISPLAY_RAIL_OBSERVE_PHASES)return EFI_OUT_OF_RESOURCES;
  S->Report.Busy=TRUE;EFI_STATUS E=App(S);if(E!=EFI_SUCCESS)return Fail(S,E);PIANO_DISPLAY_RAIL_SNAPSHOT *R=&S->Report.Snapshot[S->Report.Count++];ZeroMem(R,sizeof(*R));CopyMem(R->Phase,Phase,N);R->SelectorBefore=R->SelectorAfter=EFI_NOT_STARTED;
  for(UINT32 J=0;J<2;++J)R->Mm[J].Status=R->Mx[J].Status=EFI_NOT_STARTED;
  E=Identity(S);if(E!=EFI_SUCCESS)goto Done;
  R->SelectorBefore=E=PianoDisplayClockReadSnapshotClock(S->Env.ClockReader,PianoClockSelectNonGdscAhb,&R->ClockBefore);
  if(S->Env.ClockReader->Report.Retained||S->Env.ClockReader->Report.ServicesLost)S->Report.Retained=TRUE;
  if(!Live(S)){E=EFI_ABORTED;goto Done;}if(E!=EFI_SUCCESS||S->Report.Retained){if(E==EFI_SUCCESS)E=EFI_COMPROMISED_DATA;goto Done;}
  for(UINT32 J=0;J<2;++J){E=Graph(S,R->ClockBefore.MmClient,"/vcs/vdd_mm",&R->Mm[J]);if(E!=EFI_SUCCESS)goto Done;
    EFI_STATUS Mx=Graph(S,R->ClockBefore.MxClient,"/vcs/vdd_mx",&R->Mx[J]);if(S->Report.Retained||!Live(S)){E=EFI_ABORTED;goto Done;}(VOID)Mx;}
  R->MmCoherent=!CompareMem(&R->Mm[0],&R->Mm[1],sizeof(R->Mm[0]));R->MxCoherent=R->Mx[0].Status==EFI_SUCCESS&&R->Mx[1].Status==EFI_SUCCESS&&!CompareMem(&R->Mx[0],&R->Mx[1],sizeof(R->Mx[0]));
  if(!R->MmCoherent){E=EFI_MEDIA_CHANGED;goto Done;}
  E=Identity(S);if(E!=EFI_SUCCESS)goto Done;R->SelectorAfter=E=PianoDisplayClockReadSnapshotClock(S->Env.ClockReader,PianoClockSelectNonGdscAhb,&R->ClockAfter);
  if(S->Env.ClockReader->Report.Retained||S->Env.ClockReader->Report.ServicesLost)S->Report.Retained=TRUE;
  if(!Live(S)){E=EFI_ABORTED;goto Done;}if(S->Report.Retained){if(E==EFI_SUCCESS)E=EFI_COMPROMISED_DATA;goto Done;}
  if(E==EFI_SUCCESS&&CompareMem(&R->ClockBefore,&R->ClockAfter,sizeof(R->ClockBefore)))E=EFI_MEDIA_CHANGED;
Done:R->Retained=S->Report.Retained;R->ServicesLost=S->Report.ServicesLost;if(!Live(S)){E=EFI_ABORTED;R->Retained=R->ServicesLost=TRUE;}R->Status=Fail(S,E);S->Report.Busy=FALSE;if(!R->ServicesLost)Emit(R);return R->Status;
}
EFI_STATUS PianoDisplayRailReemit(PIANO_DISPLAY_RAIL_OBSERVER *S){if(!S||S->Signature!=RAIL_SIGNATURE)return EFI_INVALID_PARAMETER;if(S->Report.Busy)return EFI_ALREADY_STARTED;S->Report.Busy=TRUE;if(!Live(S)){S->Report.Busy=FALSE;return EFI_ABORTED;}for(UINT32 I=0;I<S->Report.Count;++I){if(!Live(S)){S->Report.Busy=FALSE;return EFI_ABORTED;}Emit(&S->Report.Snapshot[I]);}S->Report.Busy=FALSE;return EFI_SUCCESS;}
EFI_STATUS PianoDisplayRailClose(PIANO_DISPLAY_RAIL_OBSERVER *S){if(!S||S->Signature!=RAIL_SIGNATURE)return EFI_INVALID_PARAMETER;if(S->Report.Busy)return EFI_ALREADY_STARTED;if(S->Report.Retained||S->Report.ServicesLost)return EFI_ACCESS_DENIED;
  S->Report.Busy=TRUE;EFI_STATUS E=App(S);if(E!=EFI_SUCCESS)return S->Report.Close=Fail(S,E);for(UINT32 I=0;I<2;++I){E=Free(S,&S->Image[I].Copy);if(E!=EFI_SUCCESS)return S->Report.Close=Fail(S,E);}
  if(S->Exit){E=S->Env.Services->CloseEvent(S->Exit);if(!Live(S))return S->Report.Close=Fail(S,EFI_ABORTED);if(E!=EFI_SUCCESS){S->Report.Retained=TRUE;return S->Report.Close=Fail(S,E);}S->Exit=NULL;}S->Report.Initialized=FALSE;S->Report.Busy=FALSE;return S->Report.Close=EFI_SUCCESS;}
BOOLEAN PianoDisplayRailRetained(CONST PIANO_DISPLAY_RAIL_OBSERVER *S){return S&&S->Signature==RAIL_SIGNATURE&&(S->Report.Retained||S->Report.ServicesLost);}
CONST PIANO_DISPLAY_RAIL_REPORT *PianoDisplayRailReport(CONST PIANO_DISPLAY_RAIL_OBSERVER *S){return S&&S->Signature==RAIL_SIGNATURE?&S->Report:NULL;}
