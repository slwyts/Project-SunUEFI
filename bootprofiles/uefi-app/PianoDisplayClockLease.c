// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoDisplayClockLease.h"
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/DxeServicesLib.h>
#define SIGNATURE SIGNATURE_32('P','D','C','L')
#define IMAGE_BYTES 0x44000U
#define TEXT_FIRST 0x1000U
#define TEXT_BYTES 0x27000U
#define GCC_NODE 0x33A68U
STATIC EFI_GUID mClockGuid=EFI_CLOCK_PROTOCOL_GUID;
STATIC EFI_GUID mImageGuid={0x4db5dea6,0x5302,0x4d1a,{0x8a,0x82,0x67,0x7a,0x68,0x3b,0x0d,0x29}};
STATIC CONST UINT8 mPin[32]={0xf9,0xe8,0x5a,0xa7,0x58,0x93,0x2b,0x43,0x66,0xc5,0x5e,0xc8,0x3b,0x58,0xe0,0x17,0x6f,0x58,0xfb,0xa2,0x94,0x4c,0xc2,0xfd,0xd3,0x48,0x75,0xa4,0x6f,0xdb,0x76,0x9e};
STATIC EFI_STATUS Exact(EFI_STATUS E){return E==EFI_SUCCESS?E:EFI_ERROR(E)?E:EFI_DEVICE_ERROR;}
STATIC BOOLEAN Alias(CONST VOID *A,UINTN An,CONST VOID *B,UINTN Bn){UINTN X=(UINTN)A,Y=(UINTN)B;return An>MAX_UINTN-X||Bn>MAX_UINTN-Y||(X<Y+Bn&&Y<X+An);}
STATIC BOOLEAN Live(PIANO_DISPLAY_CLOCK_LEASE *S){if(S->Report.ServicesLost||S->Env.BootServicesAlive(S->Env.Context)!=TRUE){S->Report.ServicesLost=TRUE;return FALSE;}return TRUE;}
STATIC EFI_STATUS Retain(PIANO_DISPLAY_CLOCK_LEASE *S,EFI_STATUS E){S->Report.Retained=TRUE;S->Report.Busy=FALSE;return S->Report.Status=Exact(E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);}
STATIC VOID EFIAPI Exit(EFI_EVENT Event,VOID *Context){(VOID)Event;PIANO_DISPLAY_CLOCK_LEASE *S=Context;S->Report.ServicesLost=S->Report.Retained=TRUE;}
STATIC BOOLEAN CleanFirstTextRefusal(PIANO_DISPLAY_CLOCK_LEASE *S,UINT64 Address,UINTN Bytes,EFI_STATUS Status){
 if(Status!=EFI_NOT_READY||S->ReadCpuCalls!=1||Address!=S->Report.NativeBase+TEXT_FIRST||Bytes!=sizeof(S->LiveText)||
   !S->PinnedCopy||S->PinnedBytes!=IMAGE_BYTES||S->Exit||S->Report.GetId!=EFI_NOT_STARTED||S->Report.Enable!=EFI_NOT_STARTED||
   S->Report.AcquireAttempted||S->Report.Held||S->Report.OwnedReferences||S->Report.Retained||!S->Env.GetReadFailureEvidence)return FALSE;
 PIANO_DISPLAY_CLOCK_LEASE_READ_FAILURE_EVIDENCE *R=&S->Report.ReadFailureEvidence;ZeroMem(R,sizeof(*R));
 S->Report.ReadFailureEvidenceStatus=S->Env.GetReadFailureEvidence(S->Env.Context,Address,Bytes,Status,R);
 if(!Live(S)||S->Report.ReadFailureEvidenceStatus!=EFI_SUCCESS)return FALSE;
 if(R->Revision!=PIANO_DISPLAY_CLOCK_LEASE_READ_FAILURE_REVISION||R->Sequence!=1||R->Address!=Address||R->Bytes!=Bytes||R->Status!=Status||
   R->MapStatus!=EFI_NOT_READY||(R->EndStatus!=EFI_SUCCESS&&R->EndStatus!=EFI_NOT_STARTED)||R->Busy||R->Retained||R->ServicesLost||
   R->Sessions||R->GuardReads||R->GuardActive||R->GuardSyncOwned||R->GuardSErrorOwned||R->GuardFatal||R->GuardRetained||R->GuardServicesLost)return FALSE;
 S->Report.CleanSourceRefusal=TRUE;return TRUE;
}
STATIC EFI_STATUS Read(PIANO_DISPLAY_CLOCK_LEASE *S,UINT64 Address,UINTN Bytes,VOID *Out){
 if(!Address||!Bytes||Bytes>256||Address>MAX_UINT64-Bytes||!Out)return EFI_INVALID_PARAMETER;
 if(!Live(S))return EFI_ABORTED;if(S->ReadCpuCalls==MAX_UINT64)return Retain(S,EFI_OUT_OF_RESOURCES);++S->ReadCpuCalls;
 EFI_STATUS E=S->Env.ReadCpu(S->Env.Context,Address,Bytes,Out);if(!Live(S))return Retain(S,EFI_ABORTED);if(E==EFI_SUCCESS)return E;
 if(CleanFirstTextRefusal(S,Address,Bytes,E))return E;return Retain(S,!Live(S)?EFI_ABORTED:E);
}
STATIC EFI_STATUS ReadOffset(PIANO_DISPLAY_CLOCK_LEASE *S,UINT64 Base,UINTN Offset,UINTN Bytes,VOID *Out){
 if(!Base||Base>MAX_UINT64-Offset)return EFI_COMPROMISED_DATA;return Read(S,Base+Offset,Bytes,Out);
}
STATIC EFI_STATUS App(PIANO_DISPLAY_CLOCK_LEASE *S){if(!Live(S))return EFI_ABORTED;EFI_TPL T=S->Env.Services->RaiseTPL(TPL_HIGH_LEVEL);if(!Live(S))return EFI_ABORTED;S->Env.Services->RestoreTPL(T);return !Live(S)?EFI_ABORTED:T==TPL_APPLICATION?EFI_SUCCESS:EFI_UNSUPPORTED;}
STATIC EFI_STATUS FreeCopy(PIANO_DISPLAY_CLOCK_LEASE *S){
 if(!S->PinnedCopy)return EFI_SUCCESS;if(!Live(S))return EFI_ABORTED;
 EFI_STATUS E=S->Env.Services->FreePool(S->PinnedCopy);if(!Live(S))return EFI_ABORTED;if(E==EFI_SUCCESS){S->PinnedCopy=NULL;S->PinnedBytes=0;}return Exact(E);
}
STATIC EFI_STATUS Source(PIANO_DISPLAY_CLOCK_LEASE *S){
 EFI_STATUS E=GetSectionFromAnyFv(&mImageGuid,EFI_SECTION_PE32,0,&S->PinnedCopy,&S->PinnedBytes);
 if(!Live(S))return Retain(S,EFI_ABORTED);if(E!=EFI_SUCCESS){if(S->PinnedCopy||!EFI_ERROR(E))return Retain(S,E);return Exact(E);}
 if(!S->PinnedCopy||S->PinnedBytes!=IMAGE_BYTES)return EFI_COMPROMISED_DATA;
 UINT8 Hash[32];if(!Sha256HashAll(S->PinnedCopy,S->PinnedBytes,Hash)||CompareMem(Hash,mPin,32))return EFI_SECURITY_VIOLATION;
 UINT8 *B=S->PinnedCopy;
 // Exact captured PE has raw offsets equal to RVAs. Normalize only DIR64
 // literals in owned .text; live driver memory is never written.
 for(UINTN At=0x41000;At<IMAGE_BYTES;){UINT32 Page,Bytes;CopyMem(&Page,B+At,4);CopyMem(&Bytes,B+At+4,4);if(!Bytes)break;
  if(Bytes<8||(Bytes&3)||Bytes>IMAGE_BYTES-At)return EFI_COMPROMISED_DATA;
  for(UINTN X=At+8;X<At+Bytes;X+=2){UINT16 Entry;CopyMem(&Entry,B+X,2);if(!(Entry>>12))continue;if((Entry>>12)!=10)return EFI_UNSUPPORTED;
   UINT64 Target=(UINT64)Page+(Entry&4095);if(Target<TEXT_FIRST||Target>=TEXT_FIRST+TEXT_BYTES)continue;if(Target>TEXT_FIRST+TEXT_BYTES-8)return EFI_COMPROMISED_DATA;
   UINT64 Value;CopyMem(&Value,B+(UINTN)Target,8);if(Value>MAX_UINT64-S->Report.NativeBase)return EFI_COMPROMISED_DATA;Value+=S->Report.NativeBase;CopyMem(B+(UINTN)Target,&Value,8);
  }At+=Bytes;
 }
 for(UINTN At=0;At<TEXT_BYTES;At+=sizeof(S->LiveText)){UINTN N=MIN((UINTN)sizeof(S->LiveText),TEXT_BYTES-At);E=Read(S,S->Report.NativeBase+TEXT_FIRST+At,N,S->LiveText);if(E!=EFI_SUCCESS)return E;
  if(CompareMem(S->LiveText,B+TEXT_FIRST+At,N))return EFI_COMPROMISED_DATA;
 }return EFI_SUCCESS;
}
STATIC BOOLEAN Methods(PIANO_DISPLAY_CLOCK_LEASE *S,EFI_CLOCK_PROTOCOL *P){UINT64 B=S->Report.NativeBase;return P&&(UINT64)(UINTN)P==B+0x28148&&P->Version==0x1000b&&
 (UINT64)(UINTN)P->GetClockID==B+0x156c&&(UINT64)(UINTN)P->EnableClock==B+0x15ac&&(UINT64)(UINTN)P->DisableClock==B+0x15f0&&
 (UINT64)(UINTN)P->IsClockEnabled==B+0x1634&&(UINT64)(UINTN)P->IsClockOn==B+0x1674&&
 (VOID*)P->GetClockPowerDomainID==(VOID*)P->GetClockID&&(VOID*)P->EnableClockPowerDomain==(VOID*)P->EnableClock&&(VOID*)P->DisableClockPowerDomain==(VOID*)P->DisableClock;}
STATIC EFI_STATUS Identity(PIANO_DISPLAY_CLOCK_LEASE *S,BOOLEAN First){
 EFI_CLOCK_PROTOCOL *P=NULL;EFI_STATUS E=S->Env.Services->LocateProtocol(&mClockGuid,NULL,(VOID**)&P);if(!Live(S))return EFI_ABORTED;if(E!=EFI_SUCCESS||!P)return E==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(E);
 if(First){UINT64 Fn=(UINT64)(UINTN)P->GetClockID;if(Fn<0x156c)return EFI_COMPROMISED_DATA;S->Report.NativeBase=Fn-0x156c;if(S->Report.NativeBase>MAX_UINT64-IMAGE_BYTES)return EFI_COMPROMISED_DATA;}else if(P!=S->Clock)return EFI_MEDIA_CHANGED;
 if(!Methods(S,P))return EFI_UNSUPPORTED;
 if(First){EFI_HANDLE *H=NULL;UINTN Count=0;E=S->Env.Services->LocateHandleBuffer(ByProtocol,&gEfiLoadedImageProtocolGuid,NULL,&Count,&H);if(!Live(S))return EFI_ABORTED;
  if(E!=EFI_SUCCESS||!H||!Count||Count>256){if(H){S->PinnedCopy=H;S->PinnedBytes=Count*sizeof(*H);return Retain(S,E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);}return E==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(E);}
  for(UINTN I=0;I<Count;++I){EFI_LOADED_IMAGE_PROTOCOL *L=NULL;EFI_STATUS Q=S->Env.Services->HandleProtocol(H[I],&gEfiLoadedImageProtocolGuid,(VOID**)&L);if(!Live(S))return EFI_ABORTED;
   if(Q!=EFI_SUCCESS||!L||(UINT64)(UINTN)L->ImageBase!=S->Report.NativeBase)continue;
   if(S->NativeImage||L->Revision<EFI_LOADED_IMAGE_PROTOCOL_REVISION||L->ImageSize!=IMAGE_BYTES||L->ImageCodeType!=EfiBootServicesCode||L->ImageDataType!=EfiBootServicesData){E=EFI_COMPROMISED_DATA;goto HandlesDone;}
   S->NativeImage=H[I];S->ImageIdentity=L;S->ImageBase=L->ImageBase;S->ImageSize=L->ImageSize;
  }E=S->NativeImage?EFI_SUCCESS:EFI_NOT_FOUND;
HandlesDone:
  {EFI_STATUS F=S->Env.Services->FreePool(H);if(!Live(S))return EFI_ABORTED;if(F!=EFI_SUCCESS){S->PinnedCopy=H;return Exact(F);}}
  if(E!=EFI_SUCCESS)return E;S->Clock=P;return Source(S);
 }
 EFI_LOADED_IMAGE_PROTOCOL *L=NULL;E=S->Env.Services->HandleProtocol(S->NativeImage,&gEfiLoadedImageProtocolGuid,(VOID**)&L);if(!Live(S))return EFI_ABORTED;
 if(E!=EFI_SUCCESS||L!=S->ImageIdentity||!L||L->ImageBase!=S->ImageBase||L->ImageSize!=S->ImageSize||L->ImageCodeType!=EfiBootServicesCode||L->ImageDataType!=EfiBootServicesData)return E==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(E);
 return EFI_SUCCESS;
}
STATIC EFI_STATUS RefsOnce(PIANO_DISPLAY_CLOCK_LEASE *S,PIANO_DISPLAY_CLOCK_LEASE_REFS *R,BOOLEAN WithId){
 ZeroMem(R,sizeof(*R));UINT64 Base=S->Report.NativeBase;EFI_STATUS E=Read(S,Base+0x3fe48,8,&R->Global);if(E!=EFI_SUCCESS)return E;
 E=Read(S,Base+0x3f5f0,8,&R->Client);if(E!=EFI_SUCCESS)return E;if(!R->Global||!R->Client)return EFI_NOT_READY;
 E=ReadOffset(S,R->Global,0x2c,4,&R->GlobalFlags);if(E!=EFI_SUCCESS)return E;
 // Native Get/Enable skip bit11; Disable has additional no-release policy.
 if(R->GlobalFlags&(BIT11|BIT8))return EFI_UNSUPPORTED;
 if(!WithId)return EFI_SUCCESS;
 R->Provider=(UINT32)(S->Report.ClockId>>24)&255;R->Index=(UINT32)S->Report.ClockId&65535;
 E=ReadOffset(S,R->Global,8,4,&R->ModuleCount);if(E!=EFI_SUCCESS)return E;if(!R->ModuleCount||R->ModuleCount>256||R->Provider>=R->ModuleCount)return EFI_COMPROMISED_DATA;
 UINT64 Modules=0,Clocks=0;E=Read(S,R->Global,8,&Modules);if(E!=EFI_SUCCESS)return E;if(!Modules||Modules>MAX_UINT64-8*(UINT64)R->Provider)return EFI_COMPROMISED_DATA;
 E=Read(S,Modules+8*(UINT64)R->Provider,8,&R->Module);if(E!=EFI_SUCCESS)return E;if(!R->Module)return EFI_COMPROMISED_DATA;
 E=ReadOffset(S,R->Module,0x40,4,&R->ClockCount);if(E!=EFI_SUCCESS)return E;if(!R->ClockCount||R->ClockCount>65536||R->Index>=R->ClockCount)return EFI_COMPROMISED_DATA;
 E=ReadOffset(S,R->Module,0x38,8,&Clocks);if(E!=EFI_SUCCESS)return E;
 if(!Clocks||Clocks>MAX_UINT64-112*(UINT64)R->Index)return EFI_COMPROMISED_DATA;R->Node=Clocks+112*(UINT64)R->Index;
 if(R->Node!=Base+GCC_NODE)return EFI_COMPROMISED_DATA;
 UINT64 Name=0;E=Read(S,R->Node,8,&Name);if(E!=EFI_SUCCESS)return E;if(Name!=Base+0x14e02)return EFI_COMPROMISED_DATA;
 E=Read(S,R->Node+0x10,4,&R->NodeFlags);if(E!=EFI_SUCCESS)return E;
 // Restrict the observed ordinary vote class and release path; private
 // do-not-disable/protected/suppress flags cannot count as an owned release.
 if(R->NodeFlags&(BIT8|BIT9|BIT14))return EFI_UNSUPPORTED;
 E=ReadOffset(S,R->Client,0x19,1,&R->ClientFlags);if(E!=EFI_SUCCESS)return E;
 UINT64 Parent=0;UINT32 ParentFlags=0;E=Read(S,R->Node+8,8,&Parent);if(E!=EFI_SUCCESS)return E;if(!Parent)return EFI_NOT_READY;
 E=ReadOffset(S,Parent,0x18,4,&ParentFlags);if(E!=EFI_SUCCESS)return E;if(ParentFlags&BIT9)return EFI_UNSUPPORTED;
 E=Read(S,R->Node+0x50,4,R->Total);if(E!=EFI_SUCCESS)return E;
 E=Read(S,R->Node+0x58,8,&R->ClientRef);if(E!=EFI_SUCCESS)return E;
 UINT64 Seen[64];UINTN Used=0;
 while(R->ClientRef){if(Used>=ARRAY_SIZE(Seen))return EFI_COMPROMISED_DATA;for(UINTN I=0;I<Used;++I)if(Seen[I]==R->ClientRef)return EFI_COMPROMISED_DATA;Seen[Used++]=R->ClientRef;
  UINT64 Client=0,Next=0;E=ReadOffset(S,R->ClientRef,8,8,&Client);if(E!=EFI_SUCCESS)return E;
  if(Client==R->Client){E=ReadOffset(S,R->ClientRef,0x10,4,R->PerClient);if(E!=EFI_SUCCESS)return E;
   if(R->PerClient[0]>R->Total[0]||R->PerClient[1]>R->Total[1])return EFI_COMPROMISED_DATA;return EFI_SUCCESS;}
  E=Read(S,R->ClientRef,8,&Next);if(E!=EFI_SUCCESS)return E;R->ClientRef=Next;
 }return EFI_NOT_FOUND;
}
STATIC EFI_STATUS Refs(PIANO_DISPLAY_CLOCK_LEASE *S,PIANO_DISPLAY_CLOCK_LEASE_REFS *R,BOOLEAN WithId){
 EFI_STATUS E=RefsOnce(S,R,WithId);if(E!=EFI_SUCCESS)return E;
 PIANO_DISPLAY_CLOCK_LEASE_REFS Second;E=RefsOnce(S,&Second,WithId);if(E!=EFI_SUCCESS)return E;
 if(CompareMem(R,&Second,sizeof(*R)))return EFI_MEDIA_CHANGED;R->MatchingSnapshots=2;return EFI_SUCCESS;
}
STATIC BOOLEAN SameRef(CONST PIANO_DISPLAY_CLOCK_LEASE_REFS *A,CONST PIANO_DISPLAY_CLOCK_LEASE_REFS *B){
 return A->Global==B->Global&&A->Client==B->Client&&A->Module==B->Module&&A->Node==B->Node&&A->ClientRef==B->ClientRef&&
 A->Provider==B->Provider&&A->Index==B->Index&&A->ModuleCount==B->ModuleCount&&A->ClockCount==B->ClockCount&&
 A->GlobalFlags==B->GlobalFlags&&A->NodeFlags==B->NodeFlags&&A->ClientFlags==B->ClientFlags;
}
STATIC EFI_STATUS Gcc(PIANO_DISPLAY_CLOCK_LEASE *S,PIANO_DISPLAY_CLOCK_LEASE_GCC *R){
 ZeroMem(R,sizeof(*R));EFI_STATUS E=S->Env.ReadGcc(S->Env.Context,R);if(!Live(S))return EFI_ABORTED;
 if(R->Retained||R->ServicesLost||R->EndStatus!=EFI_SUCCESS||(E!=EFI_SUCCESS&&!EFI_ERROR(E)))return Retain(S,!Live(S)?EFI_ABORTED:E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);
 if(E!=EFI_SUCCESS||R->Status!=EFI_SUCCESS||R->EndStatus!=EFI_SUCCESS||R->Retained||R->ServicesLost||R->Reads!=4||R->Pages!=1||R->Ahb[0]!=R->Ahb[1]||R->HfAxi[0]!=R->HfAxi[1])return E==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(E);return EFI_SUCCESS;
}
STATIC EFI_STATUS Get(PIANO_DISPLAY_CLOCK_LEASE *S,UINTN *Id){
#ifdef PIANO_DISPLAY_CLOCK_HOST_TEST
 extern EFI_STATUS PianoDisplayClockHostGet(EFI_CLOCK_PROTOCOL *,CONST CHAR8 *,UINTN *);return PianoDisplayClockHostGet(S->Clock,"gcc_disp_ahb_clk",Id);
#else
 return S->Clock->GetClockID(S->Clock,"gcc_disp_ahb_clk",Id);
#endif
}
STATIC EFI_STATUS Enable(PIANO_DISPLAY_CLOCK_LEASE *S){
#ifdef PIANO_DISPLAY_CLOCK_HOST_TEST
 extern EFI_STATUS PianoDisplayClockHostEnable(EFI_CLOCK_PROTOCOL *,UINTN);return PianoDisplayClockHostEnable(S->Clock,S->Report.ClockId);
#else
 return S->Clock->EnableClock(S->Clock,S->Report.ClockId);
#endif
}
STATIC EFI_STATUS Disable(PIANO_DISPLAY_CLOCK_LEASE *S){
#ifdef PIANO_DISPLAY_CLOCK_HOST_TEST
 extern EFI_STATUS PianoDisplayClockHostDisable(EFI_CLOCK_PROTOCOL *,UINTN);return PianoDisplayClockHostDisable(S->Clock,S->Report.ClockId);
#else
 return S->Clock->DisableClock(S->Clock,S->Report.ClockId);
#endif
}
STATIC EFI_STATUS IsOn(PIANO_DISPLAY_CLOCK_LEASE *S,BOOLEAN *On){
#ifdef PIANO_DISPLAY_CLOCK_HOST_TEST
 extern EFI_STATUS PianoDisplayClockHostIsOn(EFI_CLOCK_PROTOCOL *,UINTN,BOOLEAN *);return PianoDisplayClockHostIsOn(S->Clock,S->Report.ClockId,On);
#else
 return S->Clock->IsClockOn(S->Clock,S->Report.ClockId,On);
#endif
}
STATIC EFI_STATUS IsEnabled(PIANO_DISPLAY_CLOCK_LEASE *S,BOOLEAN *Enabled){
#ifdef PIANO_DISPLAY_CLOCK_HOST_TEST
 extern EFI_STATUS PianoDisplayClockHostIsEnabled(EFI_CLOCK_PROTOCOL *,UINTN,BOOLEAN *);return PianoDisplayClockHostIsEnabled(S->Clock,S->Report.ClockId,Enabled);
#else
 return S->Clock->IsClockEnabled(S->Clock,S->Report.ClockId,Enabled);
#endif
}
EFI_STATUS PianoDisplayClockLeaseAcquire(PIANO_DISPLAY_CLOCK_LEASE *S,CONST PIANO_DISPLAY_CLOCK_LEASE_ENV *Env){
 if(!S||!Env||S->Signature||Alias(S,sizeof(*S),Env,sizeof(*Env))||(Env->Context&&Alias(S,sizeof(*S),Env->Context,1)))return EFI_INVALID_PARAMETER;
 if(!Env->Services||!Env->BootServicesAlive||!Env->ReadCpu||!Env->ReadGcc||!Env->Services->LocateProtocol||!Env->Services->LocateHandleBuffer||!Env->Services->HandleProtocol||!Env->Services->FreePool||!Env->Services->RaiseTPL||!Env->Services->RestoreTPL||!Env->Services->CreateEventEx||!Env->Services->CloseEvent)return EFI_UNSUPPORTED;
 S->Signature=SIGNATURE;S->Env=*Env;S->Report.Revision=1;
 S->Report.Status=S->Report.Identity=S->Report.Before=S->Report.GetId=S->Report.Enable=S->Report.IsOn=S->Report.IsEnabled=S->Report.After=S->Report.Disable=S->Report.Cleanup=S->Report.CounterStatus=S->Report.ReleaseReadbackStatus=S->Report.ReadFailureEvidenceStatus=EFI_NOT_STARTED;
 S->Report.Busy=TRUE;EFI_STATUS E=App(S);if(E!=EFI_SUCCESS)return Retain(S,E);
 S->Report.Identity=E=Identity(S,TRUE);if(E!=EFI_SUCCESS)goto Unheld;
 E=Refs(S,&S->Report.Baseline,FALSE);if(E!=EFI_SUCCESS)goto Unheld;
 S->Report.Before=E=Gcc(S,&S->Report.BeforeGcc);if(E!=EFI_SUCCESS)goto Unheld;
 E=S->Env.Services->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,S,&gEfiEventExitBootServicesGuid,&S->Exit);
 if(E!=EFI_SUCCESS||!S->Exit||!Live(S))return Retain(S,E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);
 UINTN Id=MAX_UINTN;S->Report.GetId=E=Get(S,&Id);if(!Live(S)||E!=EFI_SUCCESS)return Retain(S,!Live(S)?EFI_ABORTED:E);
 if(Id>MAX_UINT32||((Id>>16)&255)!=1)return Retain(S,EFI_COMPROMISED_DATA);S->Report.ClockId=Id;
 S->Report.Before=E=Refs(S,&S->Report.Baseline,TRUE);if(E!=EFI_SUCCESS)return Retain(S,E);
 if(S->Report.Baseline.Total[0]==MAX_UINT16||S->Report.Baseline.PerClient[0]==MAX_UINT16)return Retain(S,EFI_OUT_OF_RESOURCES);
 E=Identity(S,FALSE);if(E!=EFI_SUCCESS)return Retain(S,E);
 S->Report.AcquireAttempted=TRUE;S->Report.Enable=E=Enable(S);if(!Live(S)||E!=EFI_SUCCESS)return Retain(S,!Live(S)?EFI_ABORTED:E);
 S->Report.Held=TRUE;S->Report.CounterStatus=E=Refs(S,&S->Report.Acquired,TRUE);if(E!=EFI_SUCCESS)return Retain(S,E);
 if(!SameRef(&S->Report.Baseline,&S->Report.Acquired)||S->Report.Acquired.Total[0]!=S->Report.Baseline.Total[0]+1||S->Report.Acquired.PerClient[0]!=S->Report.Baseline.PerClient[0]+1||S->Report.Acquired.Total[1]!=S->Report.Baseline.Total[1]||S->Report.Acquired.PerClient[1]!=S->Report.Baseline.PerClient[1])return Retain(S,EFI_COMPROMISED_DATA);
 S->Report.OwnedReferences=1;
 BOOLEAN Enabled=0xA5;S->Report.IsEnabled=E=IsEnabled(S,&Enabled);S->Report.EnabledObserved=Enabled;
 if(!Live(S)||E!=EFI_SUCCESS||Enabled!=TRUE)return Retain(S,!Live(S)?EFI_ABORTED:E==EFI_SUCCESS?EFI_NOT_READY:E);
 // Native FF0C returns TRUE only for CBCR top nibble0 or2. HWCG idle nibble8
 // is legitimate with bit0 enabled; exact ref and GCC proof remain mandatory.
 BOOLEAN On=0xA5;S->Report.IsOn=E=IsOn(S,&On);S->Report.OnObserved=On;
 if(!Live(S)||E!=EFI_SUCCESS||(On!=TRUE&&On!=FALSE))return Retain(S,!Live(S)?EFI_ABORTED:E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);
 S->Report.After=E=Gcc(S,&S->Report.AfterGcc);if(E!=EFI_SUCCESS)return Retain(S,E);
 if(!(S->Report.AfterGcc.Ahb[0]&BIT0)||S->Report.AfterGcc.HfAxi[0]!=S->Report.BeforeGcc.HfAxi[0]||
   ((S->Report.AfterGcc.Ahb[0]^S->Report.BeforeGcc.Ahb[0])&~(UINT32)(BIT0|BIT31)))return Retain(S,EFI_COMPROMISED_DATA);
 E=Identity(S,FALSE);if(E!=EFI_SUCCESS)return Retain(S,E);
 S->Report.Cleanup=E=FreeCopy(S);if(E!=EFI_SUCCESS)return Retain(S,E);
 S->Report.Busy=FALSE;return S->Report.Status=EFI_SUCCESS;
Unheld:
 if(S->Report.Retained)return S->Report.Status;
 S->Report.Cleanup=FreeCopy(S);if(S->Report.Cleanup!=EFI_SUCCESS)return Retain(S,S->Report.Cleanup);
 S->Report.Busy=FALSE;return S->Report.Status=Exact(E);
}
EFI_STATUS PianoDisplayClockLeaseRelease(PIANO_DISPLAY_CLOCK_LEASE *S){
 if(!S||S->Signature!=SIGNATURE)return EFI_INVALID_PARAMETER;
 if(S->Report.Retained||S->Report.ServicesLost||S->Report.Busy||!S->Report.Held||S->Report.ReleaseAttempted)return EFI_ACCESS_DENIED;
 S->Report.Busy=TRUE;EFI_STATUS E=App(S);if(E!=EFI_SUCCESS)return Retain(S,E);
 E=Identity(S,FALSE);if(E!=EFI_SUCCESS)return Retain(S,E);
 PIANO_DISPLAY_CLOCK_LEASE_REFS Before;E=Refs(S,&Before,TRUE);if(E!=EFI_SUCCESS)return Retain(S,E);
 S->Report.ReleaseBefore=Before;
 if(S->Report.OwnedReferences!=1||!SameRef(&Before,&S->Report.Acquired)||!Before.Total[0]||!Before.PerClient[0])return Retain(S,EFI_COMPROMISED_DATA);
 S->Report.ReleaseAttempted=TRUE;S->Report.Disable=E=Disable(S);if(!Live(S)||E!=EFI_SUCCESS)return Retain(S,!Live(S)?EFI_ABORTED:E);
 S->Report.CounterStatus=E=Refs(S,&S->Report.Retired,TRUE);if(E!=EFI_SUCCESS)return Retain(S,E);
 if(!SameRef(&S->Report.Retired,&S->Report.ReleaseBefore)||S->Report.Retired.Total[0]!=S->Report.ReleaseBefore.Total[0]-1||S->Report.Retired.PerClient[0]!=S->Report.ReleaseBefore.PerClient[0]-1||S->Report.Retired.Total[1]!=S->Report.ReleaseBefore.Total[1]||S->Report.Retired.PerClient[1]!=S->Report.ReleaseBefore.PerClient[1])return Retain(S,EFI_COMPROMISED_DATA);
 S->Report.OwnedReferences=0;
 S->Report.ReleaseReadbackStatus=E=Gcc(S,&S->Report.ReleaseGcc);if(E!=EFI_SUCCESS)return Retain(S,E);
 if(S->Report.ReleaseGcc.HfAxi[0]!=S->Report.AfterGcc.HfAxi[0]||((S->Report.ReleaseGcc.Ahb[0]^S->Report.AfterGcc.Ahb[0])&~(UINT32)(BIT0|BIT31)))return Retain(S,EFI_COMPROMISED_DATA);
 E=Identity(S,FALSE);if(E!=EFI_SUCCESS)return Retain(S,E);
 E=S->Env.Services->CloseEvent(S->Exit);if(!Live(S)||E!=EFI_SUCCESS)return Retain(S,!Live(S)?EFI_ABORTED:E);
 S->Exit=NULL;S->Report.Held=FALSE;S->Report.Released=TRUE;S->Report.Busy=FALSE;return S->Report.Status=EFI_SUCCESS;
}
