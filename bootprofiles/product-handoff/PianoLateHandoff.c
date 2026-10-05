// SPDX-License-Identifier: BSD-2-Clause-Patent
// Trusted product authority backed by the actual manager, not a ready report.
#include "PianoLateHandoff.h"
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#define LATE_SIG SIGNATURE_32('P','L','H','F')
STATIC EFI_GUID mLateGuid=PIANO_PRODUCT_EXIT_PROTOCOL_GUID;
STATIC BOOLEAN LateOverlap(CONST VOID*A,UINTN An,CONST VOID*B,UINTN Bn){UINTN X=(UINTN)A,Y=(UINTN)B;return X>MAX_UINTN-An||Y>MAX_UINTN-Bn||(X<Y+Bn&&Y<X+An);}
STATIC BOOLEAN LateLive(PIANO_LATE_HANDOFF *S){return S->Env.BootServicesAlive&&S->Env.BootServicesAlive(S->Env.Context)==TRUE;}
STATIC VOID LateStop(PIANO_LATE_HANDOFF *S,EFI_STATUS E){S->Retained=TRUE;S->Phase=PianoExitRetained;S->Status=E==EFI_SUCCESS?EFI_COMPROMISED_DATA:EFI_ERROR(E)?E:EFI_DEVICE_ERROR;
 S->Env.FailStop(S->Env.Context,S->Status);CpuDeadLoop();while(TRUE){}
}
STATIC EFI_STATUS LateApp(PIANO_LATE_HANDOFF *S){
 if(!LateLive(S))return EFI_ABORTED;
 EFI_TPL T=S->Env.Services->RaiseTPL(TPL_HIGH_LEVEL);if(!LateLive(S))return EFI_ABORTED;
 S->Env.Services->RestoreTPL(T);if(!LateLive(S))return EFI_ABORTED;
 return T==TPL_APPLICATION?EFI_SUCCESS:EFI_UNSUPPORTED;
}
STATIC BOOLEAN LateMemoryReady(CONST PIANO_LINUX_MEMORY_PROOF *P){return P->Revision==1&&P->Status==EFI_SUCCESS&&P->BootEpoch&&P->DramBytes&&P->NormalBytes&&
 P->NormalBytes<=P->DramBytes&&!P->UnresolvedReservations&&P->FullDdr==TRUE&&P->FixedReservations==TRUE&&P->DynamicReservations==TRUE&&
 P->RuntimeRegions==TRUE&&P->CacheVerified==TRUE&&P->OwnershipVerified==TRUE;}
STATIC EFI_STATUS LateMemory(PIANO_LATE_HANDOFF *S,PIANO_LINUX_MEMORY_PROOF *P){
 ZeroMem(P,sizeof(*P));EFI_STATUS E=S->Env.CheckMemory(S->Env.Context,P);if(!LateLive(S))return EFI_ABORTED;
 if(E!=EFI_SUCCESS)return EFI_ERROR(E)?E:EFI_DEVICE_ERROR;if(!LateMemoryReady(P))return EFI_NOT_READY;
 PIANO_LINUX_MEMORY_PROOF Saved=*P;E=S->Env.ValidateMemory(S->Env.Context,P);if(!LateLive(S))return EFI_ABORTED;
 if(CompareMem(P,&Saved,sizeof(*P)))return EFI_COMPROMISED_DATA;return E==EFI_SUCCESS?E:EFI_ERROR(E)?E:EFI_DEVICE_ERROR;
}
STATIC BOOLEAN LateIdentity(PIANO_LATE_HANDOFF *S,EFI_HANDLE Image,CONST EFI_LOADED_IMAGE_PROTOCOL *L){return Image==S->Image&&L==S->Identity&&L&&
 L->Revision==S->Loaded.Revision&&L->ImageBase==S->Loaded.ImageBase&&L->ImageSize==S->Loaded.ImageSize&&
 L->ParentHandle==S->Loaded.ParentHandle&&L->SystemTable==S->Loaded.SystemTable&&
 L->ImageCodeType==S->Loaded.ImageCodeType&&L->ImageDataType==S->Loaded.ImageDataType;}
STATIC BOOLEAN LateClean(PIANO_LATE_HANDOFF *S){
 CONST PIANO_PRODUCT_OWNERS_REPORT *R=&S->Env.Owners->Report;
 return !CompareMem(&S->Env.Owners->Config,&S->OwnerConfig,sizeof(S->OwnerConfig))&&R->Initialized&&R->Revision==1&&R->Phase==PianoProductOwnersClean&&
 R->Status==EFI_SUCCESS&&R->Clean&&!R->Retained&&!R->ServicesLost&&!R->Busy&&!R->OuterTplHeld&&R->ManagerEventClosed&&
 R->PolicyStopped&&R->UsbStopped&&R->ProofAccepted&&R->BridgeStopped&&R->UfsStopped&&R->InputStopped&&
 R->RegisteredStartedMask==S->OwnerConfig.StartedOwnerMask&&R->RegisteredAbsentMask==S->OwnerConfig.AbsentOwnerMask&&
 R->RetiredMask==R->RegisteredStartedMask&&!(R->RegisteredStartedMask&R->RegisteredAbsentMask)&&
 (R->RegisteredStartedMask|R->RegisteredAbsentMask)==PIANO_OWNER_ALL_MASK&&
 !CompareMem(R,&S->CleanOwners,sizeof(*R));
}
STATIC EFI_STATUS EFIAPI LateObserve(PIANO_PRODUCT_EXIT_PROTOCOL *P,EFI_HANDLE Image,CONST EFI_LOADED_IMAGE_PROTOCOL *L,UINT64 *Epoch){
 PIANO_LATE_HANDOFF *S=BASE_CR(P,PIANO_LATE_HANDOFF,Protocol);
 if(S->Signature!=LATE_SIG||!Epoch||LateOverlap(Epoch,sizeof(*Epoch),S,sizeof(*S))||
    LateOverlap(Epoch,sizeof(*Epoch),S->Env.Owners,sizeof(*S->Env.Owners))||
    S->Retained||S->Phase!=PianoExitClean||S->Busy||!LateIdentity(S,Image,L)||!LateClean(S))return EFI_COMPROMISED_DATA;
 *Epoch=S->Memory.BootEpoch;return EFI_SUCCESS; // no BS, HAL or provider lookup
}
STATIC VOID EFIAPI LateFail(PIANO_PRODUCT_EXIT_PROTOCOL *P,EFI_STATUS E){LateStop(BASE_CR(P,PIANO_LATE_HANDOFF,Protocol),E);}
STATIC EFI_STATUS EFIAPI LateEnter(PIANO_PRODUCT_EXIT_PROTOCOL *P,EFI_HANDLE Image,CONST EFI_LOADED_IMAGE_PROTOCOL *L,UINTN MapKey){
 PIANO_LATE_HANDOFF *S=BASE_CR(P,PIANO_LATE_HANDOFF,Protocol);
 if(S->Signature!=LATE_SIG||S->Retained||!S->Installed)return EFI_NOT_READY;
 if(S->Busy)return EFI_ALREADY_STARTED;
 if(S->Phase!=PianoExitArmed||!LateIdentity(S,Image,L))return EFI_ACCESS_DENIED;
 EFI_STATUS E=LateApp(S);if(E!=EFI_SUCCESS)return E;
 if(CompareMem(&S->Env.Owners->Config,&S->OwnerConfig,sizeof(S->OwnerConfig)))return EFI_COMPROMISED_DATA;
 PIANO_LINUX_MEMORY_PROOF Fresh;E=LateMemory(S,&Fresh);if(E!=EFI_SUCCESS)return E;
 if(Fresh.BootEpoch!=S->Memory.BootEpoch||Fresh.DramBytes!=S->Memory.DramBytes)return EFI_NOT_READY;
 S->Busy=TRUE;S->Phase=PianoExitRetiring;S->CallerMapKey=MapKey;++S->ExitCalls;++S->RetireCalls;
 E=PianoProductOwnersRetire(S->Env.Owners);
 if(E!=EFI_SUCCESS||!LateLive(S))LateStop(S,E==EFI_SUCCESS?EFI_ABORTED:E);
 S->CleanOwners=S->Env.Owners->Report;
 if(!LateClean(S))LateStop(S,EFI_COMPROMISED_DATA);
 E=LateMemory(S,&Fresh);
 if(E!=EFI_SUCCESS||Fresh.BootEpoch!=S->Memory.BootEpoch||Fresh.DramBytes!=S->Memory.DramBytes)
   LateStop(S,E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);
 S->Memory=Fresh;S->Busy=FALSE;S->Phase=PianoExitClean;S->Status=EFI_SUCCESS;return EFI_SUCCESS;
}
EFI_STATUS PianoLateHandoffInitialize(PIANO_LATE_HANDOFF *S,CONST PIANO_LATE_HANDOFF_ENV *E){
 if(!S||!E||!E->Services||!E->SystemTable||!E->ParentImage||!E->Owners||!E->BootServicesAlive||!E->CheckMemory||!E->ValidateMemory||!E->FailStop)return EFI_INVALID_PARAMETER;
 if(LateOverlap(S,sizeof(*S),E,sizeof(*E))||LateOverlap(S,sizeof(*S),E->Owners,sizeof(*E->Owners)))return EFI_INVALID_PARAMETER;
 if(S->Signature)return EFI_ALREADY_STARTED;
 if(!E->Services->RaiseTPL||!E->Services->RestoreTPL||!E->Services->HandleProtocol||!E->Services->InstallProtocolInterface||
    !E->Services->UninstallProtocolInterface||!E->Services->LocateProtocol)return EFI_UNSUPPORTED;
 S->Env=*E;if(LateApp(S)!=EFI_SUCCESS)return EFI_NOT_READY;
 VOID *Existing=NULL;EFI_STATUS Found=E->Services->LocateProtocol(&mLateGuid,NULL,&Existing);
 if(!LateLive(S))return EFI_ABORTED;
 if(Found!=EFI_NOT_FOUND)return Found==EFI_SUCCESS?EFI_ALREADY_STARTED:EFI_ERROR(Found)?Found:EFI_DEVICE_ERROR;
 S->Signature=LATE_SIG;S->Protocol=(PIANO_PRODUCT_EXIT_PROTOCOL){PIANO_PRODUCT_EXIT_REVISION,LateEnter,LateObserve,LateFail};
 EFI_STATUS Status=E->Services->InstallProtocolInterface(&S->ProtocolHandle,&mLateGuid,EFI_NATIVE_INTERFACE,&S->Protocol);
 if(Status!=EFI_SUCCESS||!S->ProtocolHandle||!LateLive(S))LateStop(S,Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Status);
 S->Installed=TRUE;S->Phase=PianoExitUnarmed;return S->Status=EFI_SUCCESS;
}
EFI_STATUS PianoLateHandoffArm(PIANO_LATE_HANDOFF *S,EFI_HANDLE Image){
 if(!S||S->Signature!=LATE_SIG||!S->Installed||!Image||S->Retained)return EFI_INVALID_PARAMETER;
 if(S->Phase!=PianoExitUnarmed||S->Busy)return EFI_ALREADY_STARTED;
 EFI_STATUS E=LateApp(S);if(E!=EFI_SUCCESS)return E;
 PIANO_PRODUCT_OWNERS *O=S->Env.Owners;
 if(!O->Report.Initialized||O->Report.Phase!=PianoProductOwnersReturnRequested||O->Report.Retained||O->Report.Clean||O->Report.ServicesLost||
    O->Config.ExpectedOwnerMask!=PIANO_OWNER_ALL_MASK||
    (O->Report.RequestedAction!=PianoUsbServiceActionContinue&&O->Report.RequestedAction!=PianoUsbServiceActionBoot))return EFI_NOT_READY;
 EFI_LOADED_IMAGE_PROTOCOL *L=NULL;E=S->Env.Services->HandleProtocol(Image,&gEfiLoadedImageProtocolGuid,(VOID **)&L);
 if(!LateLive(S))return EFI_ABORTED;if(E!=EFI_SUCCESS)return EFI_ERROR(E)?E:EFI_DEVICE_ERROR;
 if(!L||L->Revision<EFI_LOADED_IMAGE_PROTOCOL_REVISION||L->ParentHandle!=S->Env.ParentImage||L->SystemTable!=S->Env.SystemTable||
    !L->ImageBase||!L->ImageSize||L->ImageCodeType!=EfiLoaderCode||L->ImageDataType!=EfiLoaderData)return EFI_COMPROMISED_DATA;
 E=LateMemory(S,&S->Memory);if(E!=EFI_SUCCESS)return E;
 S->Image=Image;S->Identity=L;S->Loaded=*L;S->OwnerConfig=O->Config;S->Phase=PianoExitArmed;return S->Status=EFI_SUCCESS;
}
EFI_STATUS PianoLateHandoffDisarm(PIANO_LATE_HANDOFF *S){
 if(!S||S->Signature!=LATE_SIG||!S->Installed)return EFI_INVALID_PARAMETER;
 if(S->Phase!=PianoExitArmed||S->Busy||S->Retained||S->RetireCalls)return EFI_ACCESS_DENIED;
 EFI_STATUS E=LateApp(S);if(E!=EFI_SUCCESS)return E;
 S->Image=NULL;S->Identity=NULL;ZeroMem(&S->Loaded,sizeof(S->Loaded));ZeroMem(&S->Memory,sizeof(S->Memory));
 S->Phase=PianoExitUnarmed;return S->Status=EFI_SUCCESS;
}
EFI_STATUS PianoLateHandoffShutdown(PIANO_LATE_HANDOFF *S){
 if(!S||S->Signature!=LATE_SIG||!S->Installed)return EFI_INVALID_PARAMETER;
 if(S->Phase!=PianoExitUnarmed||S->Busy||S->Retained||S->RetireCalls)return EFI_ACCESS_DENIED;
 EFI_STATUS E=LateApp(S);if(E!=EFI_SUCCESS)return E;
 S->Busy=TRUE;E=S->Env.Services->UninstallProtocolInterface(S->ProtocolHandle,&mLateGuid,&S->Protocol);
 if(E!=EFI_SUCCESS||!LateLive(S))LateStop(S,E==EFI_SUCCESS?EFI_ABORTED:E);
 S->ProtocolHandle=NULL;S->Installed=FALSE;S->Busy=FALSE;return S->Status=EFI_SUCCESS;
}
