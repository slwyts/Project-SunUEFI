// SPDX-License-Identifier: BSD-2-Clause-Patent
// App is actual source here; Core/BootParser/Adapter/Launch/Probe link as actual
// production units. Controller/EFI/hardware ACK are host fixtures, not devices.
// Each case forks: a retained driver-lifetime session is never reset for reuse.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <openssl/sha.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoUsbRamBoot.c"
#include <Protocol/LoadedImage.h>

EFI_BOOT_SERVICES *gBS;EFI_RUNTIME_SERVICES *gRT;
EFI_GUID gEfiLoadedImageProtocolGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
EFI_GUID gEfiEventExitBootServicesGuid=EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
typedef struct {EFI_EVENT_NOTIFY Notify;VOID *Context;BOOLEAN Live;} EVENT;
typedef struct {
  UINT32 Case,ControllerCalls,Replies,CoreBoots,TakeCalls,SourceAllocations,SourceFrees,SourceReleaseCalls;
  UINT32 Loads,Starts,Unloads,Events,EventCloses,Handles,Gets,Sets,Prints,DeadLoops,Resets,BsAfterEbs;
  UINT8 Fixture[3584];UINT8 *Source;VOID *ImageBuffer;UINTN SourceBytes;
  BOOLEAN SourceLive,ImageLive,VariableLive,Alive;
  CHAR8 Reply[64];PIANO_FASTBOOT Wire;EFI_BOOT_SERVICES Bs;EFI_RUNTIME_SERVICES Rt;
  EFI_SYSTEM_TABLE St;EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Console;EFI_LOADED_IMAGE_PROTOCOL Loaded;
  EVENT Event[2];PIANO_RAM_BOOT_PROBE_RECORD Record;jmp_buf Terminal;
} FIXTURE;
STATIC FIXTURE F;
STATIC VOID CheckBs(VOID){if(!F.Alive)F.BsAfterEbs++;assert(F.Alive);}
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S){return strlen(S);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
INTN EFIAPI AsciiStrnCmp(CONST CHAR8 *A,CONST CHAR8 *B,UINTN N){return strncmp(A,B,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID *EFIAPI CopyMem(VOID *A,CONST VOID *B,UINTN N){return memcpy(A,B,N);}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID *EFIAPI AllocateZeroPool(UINTN N){CheckBs();assert(!F.Source&&N==3584);F.Source=calloc(1,N);assert(F.Source);F.SourceBytes=N;F.SourceLive=TRUE;F.SourceAllocations++;return F.Source;}
VOID EFIAPI FreePool(VOID *P){CheckBs();assert(P==F.Source&&F.SourceLive);for(UINTN I=0;I<F.SourceBytes;I++)assert(((UINT8 *)P)[I]==0);free(P);F.SourceLive=FALSE;F.SourceFrees++;}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *P,UINTN N,UINT8 *Digest){return SHA256(P,N,Digest)!=NULL;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){(void)Level;return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){(void)Level;(void)Format;}
VOID EFIAPI CpuDeadLoop(VOID){F.DeadLoops++;longjmp(F.Terminal,1);}
STATIC UINT32 Crc(CONST VOID *P,UINTN N){UINT32 V=0xffffffff;CONST UINT8 *B=P;for(UINTN I=0;I<N;I++){V^=B[I];for(UINTN J=0;J<8;J++)V=(V>>1)^((V&1)?0xedb88320U:0);}return ~V;}
STATIC VOID FixCrc(PIANO_RAM_BOOT_PROBE_RECORD *R){R->Crc32=0;R->Crc32=Crc(R,sizeof(*R));}
#if PIANO_USB_RAM_BOOT
STATIC EFI_STATUS Send(VOID *Context,CONST VOID *Data,UINTN Bytes){assert(Context==&F&&Bytes<sizeof(F.Reply));memcpy(F.Reply,Data,Bytes);F.Reply[Bytes]=0;F.Replies++;return EFI_SUCCESS;}
#endif
STATIC EFI_STATUS EFIAPI CreateEvent(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,CONST VOID *Context,CONST EFI_GUID *Group,EFI_EVENT *Event){
  CheckBs();assert(Type==EVT_NOTIFY_SIGNAL&&Tpl==TPL_NOTIFY&&Group==&gEfiEventExitBootServicesGuid&&F.Events<2);
  EVENT *E=&F.Event[F.Events++];E->Notify=Notify;E->Context=(VOID *)Context;E->Live=TRUE;*Event=E;
  return F.Case==22&&E==&F.Event[0]?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI CloseEvent(EFI_EVENT Event){
  CheckBs();EVENT *E=Event;assert((E==&F.Event[0]||E==&F.Event[1])&&E->Live);
  if(F.Case==23&&E==&F.Event[0])return EFI_WARN_STALE_DATA;
  E->Live=FALSE;F.EventCloses++;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Allocate(EFI_MEMORY_TYPE Type,UINTN Bytes,VOID **Buffer){(void)Type;(void)Bytes;(void)Buffer;assert(!"No LoadOptions expected");return EFI_OUT_OF_RESOURCES;}
STATIC EFI_STATUS EFIAPI Release(VOID *Pointer){
  CheckBs();assert(Pointer==F.Source&&F.SourceLive&&!F.ImageLive&&!F.Event[1].Live&&mSession.Adapter.ReleaseAttempted);
  for(UINTN I=0;I<F.SourceBytes;I++)assert(((UINT8 *)Pointer)[I]==0);F.SourceReleaseCalls++;
  if(F.Case==11)return EFI_WARN_STALE_DATA;FreePool(Pointer);return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Load(BOOLEAN Policy,EFI_HANDLE Parent,EFI_DEVICE_PATH_PROTOCOL *Path,VOID *Source,UINTN Bytes,EFI_HANDLE *Image){
  CheckBs();F.Loads++;assert(!Policy&&Parent==(VOID *)0xabc&&!Path&&Bytes==3584&&Source==F.Source);
  assert(mSession.ControllerClean&&mSession.Retire.Clean&&mSession.Captured&&mSession.Delivered&&mSession.Adapter.Taken);
  assert(mSession.Adapter.Owned==F.Source&&mSession.Adapter.ActiveLoan&&F.SourceLive&&!F.SourceReleaseCalls);
  if(F.Case==9){*Image=NULL;return EFI_WARN_STALE_DATA;}
  F.ImageBuffer=calloc(1,20480);assert(F.ImageBuffer);memcpy(F.ImageBuffer,Source,Bytes);F.ImageLive=TRUE;
  F.Loaded=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,.ParentHandle=Parent,.ImageBase=F.ImageBuffer,.ImageSize=20480};
  *Image=&F.Loaded;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Handle(EFI_HANDLE Image,EFI_GUID *Guid,VOID **Protocol){
  CheckBs();F.Handles++;assert(Image==&F.Loaded&&!memcmp(Guid,&gEfiLoadedImageProtocolGuid,sizeof(*Guid)));*Protocol=NULL;
  if(!F.ImageLive)return EFI_INVALID_PARAMETER;*Protocol=&F.Loaded;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,CHAR16 *Text){assert(This==&F.Console&&Text[0]=='S');F.Prints++;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI GetVariable(CHAR16 *Name,EFI_GUID *Guid,UINT32 *Attrs,UINTN *Bytes,VOID *Data){
  assert(F.Alive);assert(!memcmp(Name,PIANO_RAM_BOOT_PROBE_NAME,sizeof(PIANO_RAM_BOOT_PROBE_NAME))&&Guid->Data1==0x97ed2b41);F.Gets++;
  if(!F.VariableLive)return EFI_NOT_FOUND;
  if(Data==NULL||*Bytes<sizeof(F.Record)){*Bytes=sizeof(F.Record);return EFI_BUFFER_TOO_SMALL;}
  *Bytes=sizeof(F.Record);*Attrs=F.Case==16?EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_NON_VOLATILE:EFI_VARIABLE_BOOTSERVICE_ACCESS;
  PIANO_RAM_BOOT_PROBE_RECORD R=F.Record;
  if(F.Case==13)R.Crc32^=1;
  if(F.Case==14){R.ImageBase=0;FixCrc(&R);}
  if(F.Case==15){R.ImageBytes=20481;FixCrc(&R);}
  if(F.Case==17){R.ParentHandle^=1;FixCrc(&R);}
  if(F.Case==20){R.CurrentEl=8;FixCrc(&R);}
  memcpy(Data,&R,sizeof(R));return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SetVariable(CHAR16 *Name,EFI_GUID *Guid,UINT32 Attrs,UINTN Bytes,VOID *Data){
  assert(F.Alive);assert(!memcmp(Name,PIANO_RAM_BOOT_PROBE_NAME,sizeof(PIANO_RAM_BOOT_PROBE_NAME))&&Guid->Data1==0x97ed2b41);
  assert(Attrs==EFI_VARIABLE_BOOTSERVICE_ACCESS&&Bytes==sizeof(F.Record)&&!F.VariableLive);F.Sets++;
  F.Record=*(PIANO_RAM_BOOT_PROBE_RECORD *)Data;PIANO_RAM_BOOT_PROBE_RECORD Check=F.Record;UINT32 Expected=Check.Crc32;Check.Crc32=0;
  assert(Crc(&Check,sizeof(Check))==Expected); // actual producer's CRC is checked before architecture bridge
  // x64 host cannot execute CurrentEL. Bridge only the AA64 register evidence;
  // all entry/Get/Set/console/CRC source logic and artifact allowlist are real.
  F.Record.Flags|=BIT1;F.Record.CurrentEl=4;FixCrc(&F.Record);F.VariableLive=TRUE;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Start(EFI_HANDLE Image,UINTN *ExitBytes,CHAR16 **ExitData){
  CheckBs();F.Starts++;assert(Image==&F.Loaded&&F.ImageLive&&F.SourceLive&&mSession.Adapter.ActiveLoan);
  *ExitBytes=0;*ExitData=NULL;
  if(F.Case==12){for(UINTN I=0;I<F.Events;I++)F.Event[I].Notify(&F.Event[I],F.Event[I].Context);F.Alive=FALSE;return EFI_SUCCESS;}
  if(F.Case==10)return EFI_DEVICE_ERROR;
  EFI_STATUS Status=PianoRamBootProbeEntry(Image,&F.St);assert(Status==EFI_SUCCESS);
  free(F.ImageBuffer);F.ImageBuffer=NULL;F.ImageLive=FALSE;return EFI_SUCCESS; // Mu returned-app auto-unload model
}
STATIC EFI_STATUS EFIAPI Unload(EFI_HANDLE Image){CheckBs();assert(Image==&F.Loaded&&F.ImageLive);F.Unloads++;free(F.ImageBuffer);F.ImageBuffer=NULL;F.ImageLive=FALSE;return EFI_SUCCESS;}
STATIC VOID EFIAPI Reset(EFI_RESET_TYPE Type,EFI_STATUS Status,UINTN Bytes,VOID *Data){
  CheckBs();assert(Type==EfiResetCold&&Status==EFI_SUCCESS&&!Bytes&&!Data&&!F.Event[0].Live);F.Resets++;longjmp(F.Terminal,2);
}
EFI_STATUS PianoUsbControllerRunForRamBoot(CONST VOID *Fdt,CONST PIANO_FB_BOOT *Boot,PIANO_FB_BOOT_ACTION *Action,PIANO_USB_BOOT_RETIRE_REPORT *Report,BOOLEAN *Reboot){
  CheckBs();F.ControllerCalls++;assert(Fdt==(VOID *)123&&F.Event[0].Live);memset(Action,0,sizeof(*Action));memset(Report,0,sizeof(*Report));*Reboot=FALSE;
#if !PIANO_USB_RAM_BOOT
  (void)Boot;return Report->Result=EFI_UNSUPPORTED;
#else
  *Report=(PIANO_USB_BOOT_RETIRE_REPORT){.Result=EFI_SUCCESS,.Attempted=TRUE,.Returned=TRUE,.Clean=TRUE,.UfsAbsent=TRUE,.UsbAbsent=TRUE,
    .DeviceHalted=TRUE,.DmaFreed=TRUE,.DomainFreed=TRUE,.ClocksReleased=TRUE,.OtherStreamsStable=TRUE,.OwnedStreamIndex=117,.ClockReleaseMask=0xff};
  if(F.Case==19){Report->Retained=TRUE;*Reboot=TRUE;return EFI_SUCCESS;}
  if(F.Case==21){F.Event[0].Notify(&F.Event[0],F.Event[0].Context);F.Alive=FALSE;return EFI_SUCCESS;}
  assert(PianoFastbootInit(&F.Wire,&F,Send,NULL)==EFI_SUCCESS&&PianoFastbootSetBoot(&F.Wire,Boot)==EFI_SUCCESS);
  assert(PianoFastbootPacket(&F.Wire,"download:00000e00",17)==EFI_SUCCESS&&!strcmp(F.Reply,"DATA00000e00"));
  UINT8 Bytes[3584];memcpy(Bytes,F.Fixture,sizeof(Bytes));if(F.Case==1)Bytes[3583]^=1;
  assert(PianoFastbootPacket(&F.Wire,Bytes,777)==EFI_SUCCESS&&F.Wire.Receiving);
  assert(PianoFastbootPacket(&F.Wire,Bytes+777,sizeof(Bytes)-777)==EFI_SUCCESS&&!strcmp(F.Reply,"OKAY"));
  F.CoreBoots++;assert(PianoFastbootPacket(&F.Wire,"boot",4)==EFI_SUCCESS);
  if(F.Case==1){assert(!strncmp(F.Reply,"FAIL",4)&&!F.Wire.BootPending);PianoFastbootReset(&F.Wire);return EFI_SUCCESS;}
  assert(!strcmp(F.Reply,"OKAY")&&F.Wire.BootPending&&F.Replies==3);
  // Actual Core acknowledgement enqueue is not hardware ACK. Try before ACK:
  VOID *Premature=(VOID *)1;assert(Boot->TakeAfterAck(Boot->Context,&F.Wire,&F.Wire.BootView,&Premature)==EFI_NOT_READY&&Premature==NULL);
  assert(F.Wire.Download==F.Source&&!mSession.Captured&&!mSession.Adapter.Signature&&!F.Loads);
  F.Wire.BootTransferFrozen=TRUE;
  F.Wire.BootProof=(PIANO_FB_BOOT_PROOF){TRUE,TRUE,TRUE,TRUE,TRUE,4,9};
  if(F.Case==2)F.Wire.BootProof.AckBytes=3;
  if(F.Case==3)F.Wire.BootProof.DmaBuffersFreed=8;
  if(F.Case==4)F.Wire.BootProof.QueueEmpty=FALSE;
  if(F.Case==5)F.Wire.BootProof.AckCompleted=FALSE;
  if(F.Case==6)F.Source[3583]^=1;
  F.TakeCalls++;EFI_STATUS Status=Boot->TakeAfterAck(Boot->Context,&F.Wire,&F.Wire.BootView,&Action->Token);
  Action->Context=Boot->Context;Action->View=F.Wire.BootView;Action->Proof=F.Wire.BootProof;Action->Status=Status;
  Action->Taken=Status==EFI_SUCCESS&&Action->Token!=NULL;Action->Retained=!Action->Taken;
  if(!Action->Taken){assert(F.SourceLive&&!mSession.Captured&&!mSession.Adapter.Signature&&!F.Loads);Report->Clean=FALSE;Report->Retained=TRUE;return Report->Result=EFI_DEVICE_ERROR;}
  assert(mSession.Captured&&Action->Token==&mSession.Adapter&&mSession.Adapter.Taken&&!F.Wire.Download&&!F.Wire.Upload);
  assert(mSession.Adapter.Owned==F.Source&&!mSession.Delivered);
  VOID *Again=(VOID *)1;assert(mSession.Underlying.Take(mSession.Underlying.Context,&Again)==EFI_ALREADY_STARTED&&Again==NULL);
  assert(CarrierTake(&mSession,&Again)==EFI_NOT_READY&&!Again); // ControllerClean has not been handed back yet
  if(F.Case==7){Report->Clean=FALSE;Report->Retained=TRUE;return Report->Result=EFI_DEVICE_ERROR;}
  if(F.Case==8)Report->DomainFreed=FALSE;
  if(F.Case==18)Report->Retained=TRUE;
  return EFI_SUCCESS;
#endif
}
STATIC VOID Setup(UINT32 Case,CONST UINT8 *Bytes){
  memset(&F,0,sizeof(F));F.Case=Case;F.Alive=TRUE;memcpy(F.Fixture,Bytes,3584);gBS=&F.Bs;gRT=&F.Rt;
  F.Bs=(EFI_BOOT_SERVICES){.Hdr={.Signature=EFI_BOOT_SERVICES_SIGNATURE,.Revision=0x20000,.HeaderSize=sizeof(F.Bs)},
    .CreateEventEx=CreateEvent,.CloseEvent=CloseEvent,.LoadImage=Load,.StartImage=Start,.HandleProtocol=Handle,.UnloadImage=Unload,.AllocatePool=Allocate,.FreePool=Release};
  F.Rt=(EFI_RUNTIME_SERVICES){.Hdr={.Signature=EFI_RUNTIME_SERVICES_SIGNATURE,.HeaderSize=sizeof(F.Rt)},.GetVariable=GetVariable,.SetVariable=SetVariable,.ResetSystem=Reset};
  F.Console.OutputString=Output;F.St=(EFI_SYSTEM_TABLE){.Hdr={.Signature=EFI_SYSTEM_TABLE_SIGNATURE,.Revision=0x20000,.HeaderSize=sizeof(F.St)},.BootServices=&F.Bs,.RuntimeServices=&F.Rt,.ConOut=&F.Console};
}
STATIC VOID Run(UINT32 Case,CONST UINT8 *Bytes){
  Setup(Case,Bytes);int Terminal=setjmp(F.Terminal);EFI_STATUS Status=EFI_ABORTED;
  if(!Terminal)Status=PianoRunUsbRamBoot((VOID *)123,(VOID *)0xabc);
  if(!PIANO_USB_RAM_BOOT){assert(!Terminal&&Status==EFI_UNSUPPORTED&&F.Events==1&&F.EventCloses==1&&!F.SourceAllocations&&!F.Loads);return;}
  if(Case>=2&&Case<=7) {assert(Terminal==1&&F.DeadLoops==1&&!F.Loads&&!F.SourceFrees&&F.SourceLive);}
  else if(Case==11) {assert(Terminal==1&&F.SourceReleaseCalls==1&&!F.SourceFrees&&F.SourceLive&&mSession.Launch.Result.ResourcesRetained);}
  else if(Case==12) {assert(Terminal==1&&mSession.EbsObserved&&mSession.Launch.LostServices&&mSession.Launch.Result.ResourcesRetained&&!F.SourceReleaseCalls&&!F.EventCloses&&!F.BsAfterEbs);}
  else if(Case==19) {assert(Terminal==1&&!F.Resets&&!F.Loads&&!F.SourceAllocations);}
  else if(Case==21) {assert(Terminal==1&&mSession.EbsObserved&&!F.Loads&&!F.SourceAllocations&&!F.EventCloses&&!F.BsAfterEbs);}
  else if(Case==22) {assert(!Terminal&&Status==EFI_DEVICE_ERROR&&!F.ControllerCalls&&F.Events==1&&F.EventCloses==1&&!F.SourceAllocations);}
  else if(Case==23) {assert(Terminal==1&&F.SourceFrees==1&&F.EventCloses==1&&F.Event[0].Live&&mLastFailure==EFI_DEVICE_ERROR);}
  else if(Case==1) {assert(!Terminal&&Status==EFI_NOT_FOUND&&!F.Loads&&!F.TakeCalls&&F.SourceFrees==1&&F.EventCloses==1);}
  else if(Case==8||Case==18) {assert(!Terminal&&Status==EFI_NOT_READY&&!F.Loads&&F.SourceFrees==1&&F.EventCloses==1);}
  else if(Case==9||Case==10) {assert(!Terminal&&Status==EFI_DEVICE_ERROR&&F.Loads==1&&F.SourceFrees==1&&F.EventCloses==2);}
  else {
    assert(!Terminal&&F.Loads==1&&F.Starts==1&&F.SourceReleaseCalls==1&&F.SourceFrees==1&&F.EventCloses==2&&F.VariableLive&&F.Sets==1);
    if(Case==0)assert(Status==EFI_SUCCESS&&F.Gets==2&&F.Prints==1);
    else if(Case==13)assert(Status==EFI_CRC_ERROR);
    else assert(Status==EFI_COMPROMISED_DATA);
  }
  if(!Terminal&&Case!=1&&Case!=22){assert(mSession.Delivered&&mSession.Adapter.Consumed&&!mSession.Adapter.Taken&&!mSession.Adapter.Owned&&!F.SourceLive);VOID *Owner=(VOID *)1;assert(CarrierTake(&mSession,&Owner)==EFI_NOT_READY&&!Owner);}
  if(!Terminal){UINT32 Calls=F.ControllerCalls;assert(PianoRunUsbRamBoot((VOID *)123,(VOID *)0xabc)==EFI_ALREADY_STARTED&&F.ControllerCalls==Calls);}
}
int main(int Argc,char **Argv){
  assert(Argc==2);FILE *File=fopen(Argv[1],"rb");assert(File);UINT8 Bytes[3584];assert(fread(Bytes,1,sizeof(Bytes),File)==sizeof(Bytes)&&fgetc(File)==EOF);fclose(File);
  UINT32 Count=PIANO_USB_RAM_BOOT?24:1;
  for(UINT32 Case=0;Case<Count;Case++){pid_t P=fork();assert(P>=0);if(P==0){Run(Case,Bytes);_exit(0);}int Status;assert(waitpid(P,&Status,0)==P);if(!WIFEXITED(Status)||WEXITSTATUS(Status)){fprintf(stderr,"App RAM boot case %u failed\n",Case);return 1;}}
  printf("Actual App/Core/Parser/Adapter/Launch/Probe %u fork cases PASS: fixed real probe SHA policy, ACK4+DMA9, underlying/carrier single take, Root/Launch EBS fences, returned probe record identity+CRC, cleanup/quarantine; AA64 CurrentEL field is a host-only bridge; no device\n",Count);
  return 0;
}
