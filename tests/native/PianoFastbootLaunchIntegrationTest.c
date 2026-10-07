// SPDX-License-Identifier: BSD-2-Clause-Patent
// Linked against four actual production translation units. CPU-only synthetic
// AA64 PE and real EFI API signatures; no firmware/device or instruction exec.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#undef NULL
#include "../../uefi/core/PianoFastbootDownloadBlob.h"
#include <Guid/EventGroup.h>
EFI_GUID gEfiEventBeforeExitBootServicesGuid=EFI_EVENT_GROUP_BEFORE_EXIT_BOOT_SERVICES;
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

EFI_GUID gEfiLoadedImageProtocolGuid={.Data1=1};
EFI_GUID gEfiEventExitBootServicesGuid={.Data1=2};
typedef enum {ReturnedApp,LoadFailure,ReleaseError,ReleaseWarning} SCENARIO;
typedef struct {
  SCENARIO Scenario;
  PIANO_FASTBOOT Fastboot;
  PIANO_FASTBOOT_DOWNLOAD_BLOB Adapter;
  PIANO_FASTBOOT_LAUNCH Launch;
  PIANO_LAUNCH_BLOB Blob;
  PIANO_LAUNCH_ENV Env;
  EFI_BOOT_SERVICES Services;
  EFI_LOADED_IMAGE_PROTOCOL Loaded;
  UINT8 File[1024];
  UINT8 *Source;
  VOID *ImageAllocation;
  EFI_EVENT_NOTIFY Notify;
  VOID *NotifyContext;
  UINT32 EventToken;
  UINTN Allocations,Frees,Zeroes,QuietCalls,PendingFrames,ReplyCount;
  UINTN ShutdownCalls,LoadCalls,StartCalls,CloseCalls,HandleCalls,ReleaseCalls;
  BOOLEAN SourceLive,ImageLive,EventLive,Frozen,Alive;
  CHAR8 Replies[2][32];
} FIXTURE;
// Production state/ledger have driver lifetime. Retained cases are distinct
// objects and are never reinitialized/overwritten to proceed to the next case.
static FIXTURE fixtures[4];
static FIXTURE *current;

UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S){return strlen(S);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
INTN EFIAPI AsciiStrnCmp(CONST CHAR8 *A,CONST CHAR8 *B,UINTN N){return strncmp(A,B,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID *EFIAPI CopyMem(VOID *A,CONST VOID *B,UINTN N){return memcpy(A,B,N);}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){
  if(current && P==current->Source){assert(current->SourceLive && N==sizeof(current->File));++current->Zeroes;}
  return memset(P,0,N);
}
VOID *EFIAPI AllocateZeroPool(UINTN Bytes){
  assert(current && !current->Source && Bytes==sizeof(current->File));
  current->Source=calloc(1,Bytes);assert(current->Source);
  current->SourceLive=TRUE;++current->Allocations;return current->Source;
}
VOID EFIAPI FreePool(VOID *P){
  assert(current && P==current->Source && current->SourceLive);
  for(UINTN I=0;I<sizeof(current->File);++I)assert(((UINT8 *)P)[I]==0);
  free(P);current->SourceLive=FALSE;++current->Frees;
}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *Data,UINTN Bytes,UINT8 *Digest){return SHA256(Data,Bytes,Digest)!=NULL;}
UINTN EFIAPI Sha256GetContextSize(VOID){return sizeof(SHA256_CTX);}
BOOLEAN EFIAPI Sha256Init(VOID *C){return SHA256_Init(C)==1;}
BOOLEAN EFIAPI Sha256Update(VOID *C,CONST VOID *P,UINTN N){return SHA256_Update(C,P,N)==1;}
BOOLEAN EFIAPI Sha256Final(VOID *C,UINT8 *D){return SHA256_Final(D,C)==1;}
VOID EFIAPI CpuDeadLoop(VOID){assert(!"unexpected runtime failstop");abort();}

static EFI_STATUS reply(VOID *Context,CONST VOID *Data,UINTN Bytes){
  FIXTURE *F=Context;assert(F==current && !F->Frozen && Bytes<sizeof(F->Replies[0]) && F->ReplyCount<2);
  // Implements the actual command layer's copy-before-return contract. This
  // models completion independently; enqueue success is not an IN ACK.
  memcpy(F->Replies[F->ReplyCount++],Data,Bytes);++F->PendingFrames;return EFI_SUCCESS;
}
static EFI_STATUS quiet(VOID *Context){
  FIXTURE *F=Context;assert(F==current);++F->QuietCalls;
  if(F->PendingFrames)return EFI_NOT_READY;
  F->Frozen=TRUE;return EFI_SUCCESS;
}
static BOOLEAN services_alive(VOID *Context){FIXTURE *F=Context;assert(F==current);return F->Alive;}
static VOID failstop(VOID *Context,EFI_STATUS Status){(void)Context;(void)Status;assert(!"unexpected coordinator failstop");abort();}
static VOID source_unchanged(FIXTURE *F){
  assert(F->SourceLive && !F->Frees && !F->Zeroes && !memcmp(F->Source,F->File,sizeof(F->File)));
  assert(F->Adapter.Taken && F->Adapter.Owned==F->Source && !F->Adapter.Consumed && !F->Adapter.ReleaseAttempted);
  assert(F->Launch.Owner==&F->Adapter && F->Launch.View==F->Source && F->Launch.Loan==F->Adapter.ActiveLoan && F->Launch.Loan);
}
static EFI_STATUS shutdown_all(VOID *Context){
  FIXTURE *F=Context;assert(F==current && F->Frozen && !F->PendingFrames && !F->LoadCalls);
  ++F->ShutdownCalls;source_unchanged(F);
  assert(!F->Fastboot.Download && !F->Fastboot.Upload && !F->Fastboot.Expected && !F->Fastboot.Received);
  // Actual reset, not a mock no-op: this would zero/free the original pool if
  // Take left either ownership/borrow pointer in the command state.
  PianoFastbootReset(&F->Fastboot);source_unchanged(F);
  assert(F->Allocations==1);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI create_event(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,
  CONST VOID *Context,CONST EFI_GUID *Group,EFI_EVENT *Event){
  FIXTURE *F=current;
  assert(F->Alive && F->ShutdownCalls==1 && !F->EventLive && Type==EVT_NOTIFY_SIGNAL && Tpl==TPL_NOTIFY);
  assert(Group==&gEfiEventExitBootServicesGuid && Context==&F->Launch);
  F->Notify=Notify;F->NotifyContext=(VOID *)Context;F->EventLive=TRUE;*Event=&F->EventToken;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI close_event(EFI_EVENT Event){
  FIXTURE *F=current;assert(F->Alive && F->EventLive && Event==&F->EventToken && !F->ImageLive);
  F->EventLive=FALSE;F->Notify=NULL;F->NotifyContext=NULL;++F->CloseCalls;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI load_image(BOOLEAN Policy,EFI_HANDLE Parent,EFI_DEVICE_PATH_PROTOCOL *Path,
  VOID *Source,UINTN Bytes,EFI_HANDLE *Image){
  FIXTURE *F=current;source_unchanged(F);
  assert(F->Alive && !Policy && Parent==(VOID *)555 && !Path && F->EventLive && F->ShutdownCalls==1);
  assert(Source==F->Source && Bytes==sizeof(F->File) && F->Launch.Parsed.Kind==PianoBootArm64Pe);
  assert(F->Launch.Parsed.Pe.Machine==0xaa64 && F->Launch.Parsed.Pe.Subsystem==10 && F->Launch.Parsed.Pe.ImageBytes==8192);
  ++F->LoadCalls;if(F->Scenario==LoadFailure)return EFI_SECURITY_VIOLATION;
  F->ImageAllocation=malloc(8192);assert(F->ImageAllocation);memset(F->ImageAllocation,0,8192);
  memcpy(F->ImageAllocation,Source,Bytes); // Loader-owned destination, not a source ownership move.
  F->Loaded=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,
    .ImageBase=F->ImageAllocation,.ImageSize=8192};F->ImageLive=TRUE;*Image=&F->Loaded;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI handle_protocol(EFI_HANDLE Image,EFI_GUID *Guid,VOID **Interface){
  FIXTURE *F=current;assert(F->Alive && Image==&F->Loaded && Guid==&gEfiLoadedImageProtocolGuid);
  ++F->HandleCalls;*Interface=NULL;
  if(!F->ImageLive)return EFI_INVALID_PARAMETER;
  *Interface=&F->Loaded;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI start_image(EFI_HANDLE Image,UINTN *ExitBytes,CHAR16 **ExitData){
  FIXTURE *F=current;source_unchanged(F);
  assert(F->Alive && Image==&F->Loaded && F->ImageLive && F->ShutdownCalls==1 && F->LoadCalls==1);
  ++F->StartCalls;*ExitBytes=0;*ExitData=NULL;
  // Model Mu's automatically unloaded returning EFI application. The source
  // pool remains independent and loaned until coordinator cleanup.
  free(F->ImageAllocation);F->ImageAllocation=NULL;F->ImageLive=FALSE;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI unload_image(EFI_HANDLE Image){(void)Image;assert(!"returned app should be detected as already unloaded");return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI allocate_pool(EFI_MEMORY_TYPE Type,UINTN Bytes,VOID **Buffer){
  (void)Type;(void)Bytes;(void)Buffer;assert(!"matrix uses no LoadOptions allocation");return EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS EFIAPI release_pool(VOID *Pointer){
  FIXTURE *F=current;
  assert(F->Alive && Pointer==F->Source && F->SourceLive && !F->ImageLive && !F->EventLive);
  assert(F->Adapter.ReleaseAttempted && !F->Adapter.ActiveLoan && !F->Launch.Loan);
  for(UINTN I=0;I<sizeof(F->File);++I)assert(((UINT8 *)Pointer)[I]==0);
  // Count the verified zeroed source, independent of whether the shared
  // helper uses volatile chunk stores or the transport's old ZeroMem call.
  if(!F->Zeroes)++F->Zeroes;
  ++F->ReleaseCalls;
  if(F->Scenario==ReleaseError)return EFI_DEVICE_ERROR;
  if(F->Scenario==ReleaseWarning)return EFI_WARN_UNKNOWN_GLYPH;
  FreePool(Pointer);return EFI_SUCCESS;
}
static VOID put16(UINT8 *P,UINT16 V){P[0]=(UINT8)V;P[1]=(UINT8)(V>>8);}
static VOID put32(UINT8 *P,UINT32 V){for(UINTN I=0;I<4;++I)P[I]=(UINT8)(V>>(8*I));}
static VOID put64(UINT8 *P,UINT64 V){for(UINTN I=0;I<8;++I)P[I]=(UINT8)(V>>(8*I));}
static VOID valid_pe(UINT8 *P){
  // Minimal synthetic AA64 PE32+ EFI application. Instructions are fixture
  // data only; LoadImage/StartImage are CPU mocks and never execute them.
  put16(P,0x5a4d);put32(P+60,128);put32(P+128,0x4550);put16(P+132,0xaa64);
  put16(P+134,1);put16(P+148,240);put16(P+150,2);
  UINT8 *Opt=P+152;put16(Opt,0x20b);put32(Opt+16,4096);put64(Opt+24,0x10000000);
  put32(Opt+32,4096);put32(Opt+36,512);put32(Opt+56,8192);put32(Opt+60,512);put16(Opt+68,10);put32(Opt+108,16);
  UINT8 *Sec=P+392;memcpy(Sec,".text",5);put32(Sec+8,512);put32(Sec+12,4096);
  put32(Sec+16,512);put32(Sec+20,512);put32(Sec+36,0x60000020);
  put32(P+512,0xd2800000);put32(P+516,0xd65f03c0);
  for(UINTN I=520;I<1024;++I)P[I]=(UINT8)(I*13+9);
}
static VOID setup(FIXTURE *F,SCENARIO Scenario){
  assert(!F->Source && !F->Launch.Signature);current=F;F->Scenario=Scenario;F->Alive=TRUE;
  valid_pe(F->File);
  assert(PianoFastbootInit(&F->Fastboot,F,reply,NULL)==EFI_SUCCESS);
  assert(PianoFastbootPacket(&F->Fastboot,"download:00000400",17)==EFI_SUCCESS);
  assert(F->Fastboot.Receiving && F->Fastboot.Expected==1024);
  assert(PianoFastbootPacket(&F->Fastboot,F->File,257)==EFI_SUCCESS);
  assert(F->Fastboot.Receiving && F->Fastboot.Received==257);
  assert(PianoFastbootPacket(&F->Fastboot,F->File+257,767)==EFI_SUCCESS);
  assert(F->Fastboot.Complete && !F->Fastboot.Receiving && F->Fastboot.Download==F->Source && F->Fastboot.Upload==F->Source);
  assert(F->Fastboot.UploadBorrowed && F->ReplyCount==2 && !strcmp(F->Replies[0],"DATA00000400") && !strcmp(F->Replies[1],"OKAY"));
  assert(PianoFastbootDownloadBlobBind(&F->Adapter,&F->Fastboot,F,quiet,release_pool,&F->Blob)==EFI_NOT_READY);
  assert(!F->Adapter.Signature && F->SourceLive && !F->Frozen); // Enqueue is not ACK.
  F->PendingFrames=0; // Mock host completed both copied IN frames.
  assert(PianoFastbootDownloadBlobBind(&F->Adapter,&F->Fastboot,F,quiet,release_pool,&F->Blob)==EFI_SUCCESS);
  assert(F->Frozen && F->Adapter.BoundDownload==F->Source && F->Blob.Bytes==1024);
  F->Services=(EFI_BOOT_SERVICES){.CreateEventEx=create_event,.CloseEvent=close_event,.LoadImage=load_image,
    .HandleProtocol=handle_protocol,.StartImage=start_image,.UnloadImage=unload_image,.AllocatePool=allocate_pool,.FreePool=release_pool};
  F->Env=(PIANO_LAUNCH_ENV){.Context=F,.Services=&F->Services,.ParentImage=(VOID *)555,
    .MaxImageBytes=PIANO_FASTBOOT_MAX_DOWNLOAD,.MaxSourceBytes=PIANO_FASTBOOT_MAX_DOWNLOAD,
    .ShutdownAll=shutdown_all,.BootServicesAlive=services_alive,.FailStop=failstop,.RestoreOnFailure=TRUE};
  assert(PianoFastbootLaunchInit(&F->Launch)==EFI_SUCCESS);
}
int main(void){
  for(UINTN N=0;N<ARRAY_SIZE(fixtures);++N){
    FIXTURE *F=&fixtures[N];setup(F,(SCENARIO)N);
    EFI_STATUS Status=PianoFastbootLaunchRun(&F->Launch,&F->Env,&F->Blob,NULL,0);
    assert(F->QuietCalls==3 && F->ShutdownCalls==1 && F->LoadCalls==1 && F->CloseCalls==1);
    assert(!F->EventLive && !F->ImageLive && !F->Adapter.ActiveLoan && !F->Launch.Loan);
    assert(F->Launch.Result.ShutdownSucceeded && F->Allocations==1 && F->Frozen);
    if(N==ReturnedApp){
      assert(Status==EFI_SUCCESS && F->StartCalls==1 && F->HandleCalls==2 && F->ReleaseCalls==1);
      assert(F->Launch.Result.AppReturned && F->Launch.Result.ImageUnloaded && F->Launch.Result.BlobZeroReleased);
      assert(!F->Launch.Owner && !F->Launch.Busy && !F->Launch.Result.ResourcesRetained && !F->SourceLive && F->Frees==1 && F->Zeroes==1);
    } else if(N==LoadFailure){
      assert(Status==EFI_SECURITY_VIOLATION && !F->StartCalls && !F->ReleaseCalls && !F->Frees && !F->Zeroes);
      assert(F->Launch.Result.BlobRestored && !F->Launch.Result.BlobZeroReleased && !F->Launch.Owner && !F->Launch.Busy);
      assert(F->SourceLive && F->Fastboot.Complete && F->Fastboot.Download==F->Source && F->Fastboot.UploadBorrowed);
      assert(F->Fastboot.Expected==1024 && F->Fastboot.Received==1024 && !memcmp(F->Source,F->File,1024));
      PianoFastbootReset(&F->Fastboot);assert(F->Frees==1 && F->Zeroes==1 && !F->SourceLive);
    } else {
      assert(Status==EFI_DEVICE_ERROR && F->StartCalls==1 && F->ReleaseCalls==1 && !F->Frees && F->Zeroes==1);
      assert(F->Launch.Result.CleanupStatus==EFI_DEVICE_ERROR && F->Launch.Result.ResourcesRetained && F->Launch.Busy);
      assert(F->Launch.Owner==&F->Adapter && F->Adapter.Owned==F->Source && F->Adapter.Taken && !F->Adapter.Consumed);
      assert(F->Adapter.ReleaseAttempted && F->Adapter.ReleaseStatus==(N==ReleaseError?EFI_DEVICE_ERROR:EFI_WARN_UNKNOWN_GLYPH));
      UINT8 Byte=0xaa;VOID *Loan=NULL;CONST VOID *View=NULL;
      assert(F->Blob.Read(F->Blob.Context,F->Launch.Owner,0,1,&Byte)==EFI_INVALID_PARAMETER && Byte==0xaa);
      assert(F->Blob.BorrowView(F->Blob.Context,F->Launch.Owner,(PIANO_BOOT_RANGE){0,1024},&View,&Loan)==EFI_INVALID_PARAMETER && !View && !Loan);
      assert(F->Blob.Restore(F->Blob.Context,F->Launch.Owner)==EFI_INVALID_PARAMETER);
      assert(F->Blob.ZeroRelease(F->Blob.Context,F->Launch.Owner)==EFI_INVALID_PARAMETER && F->ReleaseCalls==1);
      assert(PianoFastbootLaunchInit(&F->Launch)==EFI_ACCESS_DENIED);
      assert(PianoFastbootLaunchRun(&F->Launch,&F->Env,&F->Blob,NULL,0)==EFI_ALREADY_STARTED && F->LoadCalls==1);
      PianoFastbootReset(&F->Fastboot);assert(F->SourceLive && !F->Frees); // Cannot free retained owner.
      // Test-only disposal: this mock explicitly returned without freeing.
      // Do not touch/reinitialize the retained production ledger afterward.
      FreePool(F->Source);
    }
  }
  puts("Actual-source launch integration: 4 cases passed (returned app, Load failure Restore, typed release error/warning retained); real FastbootReset during ShutdownAll preserved the borrowed PE source.");
  return 0;
}
