// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoFastbootLaunch.h"
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#define LAUNCH_SIGNATURE SIGNATURE_32('P','F','L','1')
STATIC BOOLEAN Alive(PIANO_FASTBOOT_LAUNCH *S) {
  return !S->LostServices && !S->BeforeEbs && S->Env.BootServicesAlive(S->Env.Context)==TRUE;
}
STATIC VOID LoseServices(PIANO_FASTBOOT_LAUNCH *S) {
  S->LostServices=TRUE;S->Phase=PianoLaunchServicesLost;S->Result.ResourcesRetained=TRUE;S->Result.Status=EFI_ABORTED;
  S->Env.FailStop(S->Env.Context,EFI_ABORTED);CpuDeadLoop();
}
STATIC VOID RequireAlive(PIANO_FASTBOOT_LAUNCH *S){if(!Alive(S))LoseServices(S);}
STATIC VOID EFIAPI ExitFence(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;PIANO_FASTBOOT_LAUNCH *S=Context;S->LostServices=TRUE;S->Result.ExitSignalSeen=TRUE;
}
STATIC VOID EFIAPI BeforeFence(EFI_EVENT Event,VOID *Context){(VOID)Event;((PIANO_FASTBOOT_LAUNCH *)Context)->BeforeEbs=TRUE;}
STATIC EFI_STATUS Exact(EFI_STATUS Status){return Status==EFI_SUCCESS?Status:EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;}
STATIC EFI_STATUS SourceRead(VOID *Context,UINT64 Offset,UINTN Bytes,VOID *Buffer) {
  PIANO_FASTBOOT_LAUNCH *S=Context;
  RequireAlive(S);EFI_STATUS Status=S->Blob.Read(S->Blob.Context,S->Owner,Offset,Bytes,Buffer);RequireAlive(S);return Status;
}
STATIC BOOLEAN MissingHandle(EFI_STATUS Status) {
  return Status==EFI_NOT_FOUND || Status==EFI_INVALID_PARAMETER || Status==EFI_UNSUPPORTED;
}
STATIC EFI_STATUS RetireImage(PIANO_FASTBOOT_LAUNCH *S) {
  if(S->Image==NULL)return EFI_SUCCESS;
  RequireAlive(S);EFI_LOADED_IMAGE_PROTOCOL *Loaded=NULL;
  EFI_STATUS Status=S->Env.Services->HandleProtocol(S->Image,&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded);RequireAlive(S);
  // Mu CoreStartImage automatically closes returned EFI applications. Never
  // dereference the saved pre-Start protocol or blindly unload its old handle.
  if(MissingHandle(Status) && S->Result.StartInvoked){S->Image=NULL;S->OptionsInstalled=FALSE;S->Result.ImageUnloaded=TRUE;return EFI_SUCCESS;}
  if(Status!=EFI_SUCCESS || Loaded==NULL)return Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(Status);
  if(S->ImageIdentityKnown && (Loaded!=S->LoadedProtocolIdentity || Loaded->ImageBase!=S->ImageBaseIdentity || Loaded->ImageSize!=S->ImageSizeIdentity))return EFI_COMPROMISED_DATA;
  if(S->OptionsInstalled) {
    if(Loaded->LoadOptions!=S->OptionsCopy || Loaded->LoadOptionsSize!=S->OptionsBytes)return EFI_COMPROMISED_DATA;
    Loaded->LoadOptions=S->OriginalOptions;Loaded->LoadOptionsSize=S->OriginalOptionsBytes;
    S->Result.OptionsRestored=TRUE;S->OptionsInstalled=FALSE;
  }
  RequireAlive(S);Status=S->Env.Services->UnloadImage(S->Image);RequireAlive(S);
  if(Status!=EFI_SUCCESS)return Exact(Status);
  S->Image=NULL;S->Result.ImageUnloaded=TRUE;return EFI_SUCCESS;
}
STATIC EFI_STATUS Cleanup(PIANO_FASTBOOT_LAUNCH *S,BOOLEAN Failure) {
  if(S->LateArmed){
    RequireAlive(S);EFI_STATUS Disarm=Exact(S->Env.NativeLateDisarm(S->Env.Context,S->Image));RequireAlive(S);
    if(Disarm!=EFI_SUCCESS){S->Result.ResourcesRetained=TRUE;S->Result.Status=Disarm;S->Env.FailStop(S->Env.Context,Disarm);CpuDeadLoop();return Disarm;}
    S->LateArmed=FALSE;
  }
  EFI_STATUS Status=RetireImage(S);if(Status!=EFI_SUCCESS)return Status;
  if(S->ExitData!=NULL) {
    RequireAlive(S);Status=S->Env.Services->FreePool(S->ExitData);RequireAlive(S);
    if(Status!=EFI_SUCCESS)return Exact(Status);
    S->ExitData=NULL;S->ExitDataBytes=0;
  }
  if(S->OptionsCopy!=NULL) {
    RequireAlive(S);ZeroMem(S->OptionsCopy,S->OptionsBytes);Status=S->Env.Services->FreePool(S->OptionsCopy);RequireAlive(S);
    if(Status!=EFI_SUCCESS)return Exact(Status);
    S->OptionsCopy=NULL;S->OptionsBytes=0;
  }
  if(S->ExitEvent!=NULL) {
    RequireAlive(S);Status=S->Env.Services->CloseEvent(S->ExitEvent);RequireAlive(S);
    if(Status!=EFI_SUCCESS)return Exact(Status);
    S->ExitEvent=NULL;
  }
  if(S->BeforeEvent!=NULL){RequireAlive(S);Status=S->Env.Services->CloseEvent(S->BeforeEvent);RequireAlive(S);if(Status!=EFI_SUCCESS)return Exact(Status);S->BeforeEvent=NULL;}
  if(S->Loan!=NULL) {
    RequireAlive(S);Status=S->Blob.Unborrow(S->Blob.Context,S->Owner,S->Loan);RequireAlive(S);
    if(Status!=EFI_SUCCESS)return Exact(Status);
    S->Loan=NULL;S->View=NULL;
  }
  if(S->UnknownOwnership)return EFI_COMPROMISED_DATA;
  if(S->Owner!=NULL) {
    RequireAlive(S);
    if(Failure && S->Env.RestoreOnFailure) {
      Status=S->Blob.Restore(S->Blob.Context,S->Owner);RequireAlive(S);
      if(Status!=EFI_SUCCESS)return Exact(Status);
      S->Result.BlobRestored=TRUE;
    } else {
      Status=S->Blob.ZeroRelease(S->Blob.Context,S->Owner);RequireAlive(S);
      if(Status!=EFI_SUCCESS)return Exact(Status);
      S->Result.BlobZeroReleased=TRUE;
    }
    S->Owner=NULL;
  }
  return EFI_SUCCESS;
}
EFI_STATUS PianoFastbootLaunchInit(PIANO_FASTBOOT_LAUNCH *S) {
  if(S==NULL)return EFI_INVALID_PARAMETER;
  if(S->Signature==LAUNCH_SIGNATURE && (S->Busy || S->Result.ResourcesRetained))return EFI_ACCESS_DENIED;
  ZeroMem(S,sizeof(*S));S->Signature=LAUNCH_SIGNATURE;return EFI_SUCCESS;
}
EFI_STATUS PianoFastbootLaunchRun(PIANO_FASTBOOT_LAUNCH *S,CONST PIANO_LAUNCH_ENV *E,CONST PIANO_LAUNCH_BLOB *B,CONST VOID *Options,UINT32 OptionsBytes) {
  if(S==NULL || S->Signature!=LAUNCH_SIGNATURE || E==NULL || B==NULL || E->Services==NULL || E->ParentImage==NULL ||
     !E->MaxImageBytes || !E->MaxSourceBytes || E->BootServicesAlive==NULL || E->FailStop==NULL || B->Take==NULL ||
     B->Read==NULL || B->BorrowView==NULL || B->Unborrow==NULL || B->Restore==NULL || B->ZeroRelease==NULL || !B->Bytes ||
     OptionsBytes>4096 || (OptionsBytes && Options==NULL))return EFI_INVALID_PARAMETER;
  if(E->HandoffMode!=PianoHandoffLegacyPreStart&&E->HandoffMode!=PianoHandoffNativeLate)return EFI_INVALID_PARAMETER;
  if(PIANO_PRODUCT_NATIVE_LATE && E->HandoffMode!=PianoHandoffNativeLate)return EFI_UNSUPPORTED;
  if(E->HandoffMode==PianoHandoffNativeLate){if(!E->NativeLateArm||!E->NativeLateDisarm||!E->ServiceSlice)return EFI_NOT_READY;}
  else if(!E->ShutdownAll)return EFI_INVALID_PARAMETER;
  if(S->Busy || S->Result.ResourcesRetained)return EFI_ALREADY_STARTED;
  ZeroMem(&S->Result,sizeof(S->Result));ZeroMem(&S->Parsed,sizeof(S->Parsed));
  S->Env=*E;S->Blob=*B;if(!Alive(S)){S->Result.Status=EFI_NOT_READY;return EFI_NOT_READY;}
  if(E->Services->CreateEventEx==NULL || E->Services->CloseEvent==NULL || E->Services->LoadImage==NULL || E->Services->StartImage==NULL ||
     E->Services->HandleProtocol==NULL || E->Services->UnloadImage==NULL || E->Services->AllocatePool==NULL || E->Services->FreePool==NULL){S->Result.Status=EFI_UNSUPPORTED;return EFI_UNSUPPORTED;}
  if(B->Bytes>E->MaxSourceBytes){S->Result.Status=EFI_OUT_OF_RESOURCES;return EFI_OUT_OF_RESOURCES;}
  S->Busy=TRUE;S->UnknownOwnership=FALSE;S->ImageIdentityKnown=FALSE;S->BeforeEbs=FALSE;S->LateArmed=FALSE;
  S->OptionsInstalled=FALSE;S->OriginalOptions=NULL;S->OriginalOptionsBytes=0;
  E=&S->Env;B=&S->Blob;
  EFI_STATUS Status=Exact(B->Take(B->Context,&S->Owner));RequireAlive(S);S->Phase=PianoLaunchOwned;
  if(Status!=EFI_SUCCESS){if(S->Owner)S->UnknownOwnership=TRUE;goto Done;}
  if(S->Owner==NULL){S->UnknownOwnership=TRUE;Status=EFI_COMPROMISED_DATA;goto Done;}
  PIANO_BOOT_SOURCE Source={S,SourceRead,B->Bytes};Status=PianoFastbootBootParse(&Source,&S->Parsed);RequireAlive(S);
  if(Status!=EFI_SUCCESS)goto Done;
  S->Phase=PianoLaunchParsed;PIANO_BOOT_RANGE View=S->Parsed.Kernel;
  if(S->Parsed.Kind==PianoBootAndroid) {
    if(!S->Parsed.KernelIsArm64Pe || S->Parsed.Ramdisk.Bytes || S->Parsed.Second.Bytes || S->Parsed.RecoveryDtbo.Bytes || S->Parsed.Dtb.Bytes ||
       (S->Parsed.KnownV4CliHeaderQuirk && !E->AllowKnownV4CliHeaderQuirk)){Status=EFI_UNSUPPORTED;goto Done;}
  } else if(S->Parsed.Kind!=PianoBootArm64Pe){Status=EFI_UNSUPPORTED;goto Done;}
  if(!View.Bytes || View.Bytes>MAX_UINTN || S->Parsed.Pe.ImageBytes>E->MaxImageBytes){Status=EFI_OUT_OF_RESOURCES;goto Done;}
  Status=Exact(B->BorrowView(B->Context,S->Owner,View,&S->View,&S->Loan));RequireAlive(S);
  if(Status!=EFI_SUCCESS){if(S->View||S->Loan)S->UnknownOwnership=TRUE;goto Done;}
  if(S->View==NULL || S->Loan==NULL){S->UnknownOwnership=TRUE;Status=EFI_COMPROMISED_DATA;goto Done;}
  S->Phase=PianoLaunchBorrowed;
  if(E->HandoffMode==PianoHandoffLegacyPreStart){
    Status=Exact(E->ShutdownAll(E->Context));RequireAlive(S);if(Status!=EFI_SUCCESS)goto Done;
    S->Result.ShutdownSucceeded=TRUE;S->Phase=PianoLaunchShutdown;
  }
  Status=Exact(E->Services->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ExitFence,S,&gEfiEventExitBootServicesGuid,&S->ExitEvent));RequireAlive(S);
  if(Status!=EFI_SUCCESS)goto Done;
  if(S->ExitEvent==NULL){S->UnknownOwnership=TRUE;Status=EFI_COMPROMISED_DATA;goto Done;}
  if(E->HandoffMode==PianoHandoffNativeLate){
    Status=Exact(E->Services->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,BeforeFence,S,&gEfiEventBeforeExitBootServicesGuid,&S->BeforeEvent));RequireAlive(S);
    if(Status!=EFI_SUCCESS||!S->BeforeEvent){S->UnknownOwnership=TRUE;if(Status==EFI_SUCCESS)Status=EFI_COMPROMISED_DATA;goto Done;}
  }
  Status=Exact(E->Services->LoadImage(FALSE,E->ParentImage,NULL,(VOID *)S->View,(UINTN)View.Bytes,&S->Image));RequireAlive(S);
  if(Status!=EFI_SUCCESS)goto Done;
  if(S->Image==NULL){S->UnknownOwnership=TRUE;Status=EFI_COMPROMISED_DATA;goto Done;}
  S->Phase=PianoLaunchLoaded;EFI_LOADED_IMAGE_PROTOCOL *Loaded=NULL;
  Status=Exact(E->Services->HandleProtocol(S->Image,&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded));RequireAlive(S);
  if(Status!=EFI_SUCCESS)goto Done;
  if(Loaded==NULL){Status=EFI_COMPROMISED_DATA;goto Done;}
  if(Loaded->Revision<EFI_LOADED_IMAGE_PROTOCOL_REVISION || Loaded->ImageBase==NULL || !Loaded->ImageSize){Status=EFI_COMPROMISED_DATA;goto Done;}
  S->LoadedProtocolIdentity=Loaded;S->ImageBaseIdentity=Loaded->ImageBase;S->ImageSizeIdentity=Loaded->ImageSize;S->ImageIdentityKnown=TRUE;
  if(Loaded->ImageSize<S->Parsed.Pe.ImageBytes){Status=EFI_COMPROMISED_DATA;goto Done;}
  if(Loaded->ImageSize>E->MaxImageBytes){Status=EFI_OUT_OF_RESOURCES;goto Done;}
  if(OptionsBytes) {
    S->OriginalOptions=Loaded->LoadOptions;S->OriginalOptionsBytes=Loaded->LoadOptionsSize;
    S->OptionsBytes=OptionsBytes;Status=Exact(E->Services->AllocatePool(EfiLoaderData,OptionsBytes,&S->OptionsCopy));RequireAlive(S);
    if(Status!=EFI_SUCCESS)goto Done;
    if(S->OptionsCopy==NULL){S->UnknownOwnership=TRUE;Status=EFI_COMPROMISED_DATA;goto Done;}
    CopyMem(S->OptionsCopy,Options,OptionsBytes);S->OptionsBytes=OptionsBytes;
    Loaded->LoadOptions=S->OptionsCopy;Loaded->LoadOptionsSize=OptionsBytes;S->OptionsInstalled=TRUE;
  }
  if(E->HandoffMode==PianoHandoffNativeLate){
    Status=E->NativeLateArm(E->Context,S->Image,Loaded);RequireAlive(S);
    if(Status!=EFI_SUCCESS){if(!EFI_ERROR(Status)){S->UnknownOwnership=TRUE;S->Result.ResourcesRetained=TRUE;S->Result.Status=EFI_DEVICE_ERROR;S->Env.FailStop(S->Env.Context,EFI_DEVICE_ERROR);CpuDeadLoop();return EFI_DEVICE_ERROR;}goto Done;}
    S->LateArmed=TRUE;
    Status=Exact(E->ServiceSlice(E->Context,1000));RequireAlive(S);if(Status!=EFI_SUCCESS)goto Done;
  }
  S->Result.StartInvoked=TRUE;S->Phase=PianoLaunchStarted;
  Status=Exact(E->Services->StartImage(S->Image,&S->ExitDataBytes,&S->ExitData));RequireAlive(S);
  S->Result.ImageExitStatus=Status;S->Result.AppReturned=TRUE;S->Phase=PianoLaunchReturned;
Done:
  if(S->Env.HandoffMode==PianoHandoffNativeLate&&S->UnknownOwnership){S->Result.ResourcesRetained=TRUE;S->Result.Status=Status;S->Env.FailStop(S->Env.Context,Status);CpuDeadLoop();return Status;}
  S->Result.Status=Status;EFI_STATUS Clean=Cleanup(S,Status!=EFI_SUCCESS);S->Result.CleanupStatus=Clean;
  if(Clean!=EFI_SUCCESS){S->Result.ResourcesRetained=TRUE;S->Phase=PianoLaunchRetained;S->Result.Status=Status!=EFI_SUCCESS?Status:Clean;return S->Result.Status;}
  S->Busy=FALSE;S->Phase=PianoLaunchReleased;return Status;
}
