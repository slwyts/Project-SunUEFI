// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoProductOwners.h"
#include "PianoFastbootBlockRead.h"
#include <Guid/EventGroup.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
STATIC EFI_STATUS Exact(EFI_STATUS Status){return Status==EFI_SUCCESS?Status:EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;}
STATIC EFI_STATUS Retain(PIANO_PRODUCT_OWNERS *Owners,EFI_STATUS Status) {
  Owners->Report.Retained=TRUE;Owners->Report.Clean=FALSE;Owners->Report.Busy=FALSE;
  Owners->Report.AllowedAction=PianoUsbServiceActionNone;
  Owners->Report.Phase=PianoProductOwnersRetained;
  return Owners->Report.Status=Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(Status);
}
VOID PianoProductOwnersFenceExit(PIANO_PRODUCT_OWNERS *Owners) {
  if(!Owners)return;
  Owners->Report.ServicesLost=TRUE;Owners->Report.Retained=TRUE;Owners->Report.Clean=FALSE;
  Owners->Report.AllowedAction=PianoUsbServiceActionNone;Owners->Report.Phase=PianoProductOwnersRetained;
}
STATIC VOID EFIAPI ExitFence(EFI_EVENT Event,VOID *Context){(VOID)Event;PianoProductOwnersFenceExit(Context);}
STATIC EFI_STATUS AtApp(PIANO_PRODUCT_OWNERS *Owners) {
  if(Owners->Report.ServicesLost)return EFI_ACCESS_DENIED;
  if(!gBS || !gBS->RaiseTPL || !gBS->RestoreTPL)return EFI_UNSUPPORTED;
  EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Old);
  return Old==TPL_APPLICATION?EFI_SUCCESS:EFI_UNSUPPORTED;
}
STATIC BOOLEAN PolicyLive(CONST PIANO_BOOT_POLICY_REPORT *Policy) {
  return Policy && Policy->Initialized && Policy->ProtocolInstalled && !Policy->ServicesLost && !Policy->Retained;
}
STATIC EFI_STATUS FreshRuntime(PIANO_PRODUCT_OWNERS *Owners) {
  EFI_GUID Guid=PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID;PIANO_PRODUCT_RUNTIME_PROTOCOL *Fresh=NULL;
  EFI_STATUS S=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Fresh);
  if(Owners->Report.ServicesLost || !PolicyLive(PianoBootPolicyReport()))return EFI_ACCESS_DENIED;
  if(S!=EFI_SUCCESS)return Exact(S);
  if(Fresh!=Owners->Config.Runtime || !Fresh || Fresh->Revision!=PIANO_PRODUCT_RUNTIME_REVISION ||
     Fresh->RequestAction!=Owners->RuntimeRequest)return EFI_COMPROMISED_DATA;
  return EFI_SUCCESS;
}
EFI_STATUS PianoProductOwnersInitialize(PIANO_PRODUCT_OWNERS *Owners,CONST PIANO_PRODUCT_OWNERS_CONFIG *Config) {
  if(!Owners || !Config || Config->Revision!=PIANO_PRODUCT_OWNERS_REVISION || !Config->Fdt ||
     !Config->Runtime || !Config->StopInput)return EFI_INVALID_PARAMETER;
  if(Owners->Report.Initialized || Owners->Report.Retained || Owners->ExitEvent)return EFI_ALREADY_STARTED;
  // Require the full platform inventory. Extra DMA owners cannot participate in
  // the current UFS/USB peer contract, and are not silently omitted.
  if(Config->ExpectedOwnerMask!=PIANO_OWNER_ALL_MASK ||
     (Config->StartedOwnerMask|Config->AbsentOwnerMask)!=PIANO_OWNER_ALL_MASK ||
     (Config->StartedOwnerMask&Config->AbsentOwnerMask))return EFI_NOT_READY;
  if((Config->StartedOwnerMask&PIANO_OWNER_CORE_MASK)!=PIANO_OWNER_CORE_MASK)return EFI_NOT_READY;
  if(Config->StartedOwnerMask&~PIANO_OWNER_CORE_MASK)return EFI_UNSUPPORTED;
  if(!gBS || !gBS->CreateEventEx || !gBS->CloseEvent || !gBS->LocateProtocol)return EFI_UNSUPPORTED;
  EFI_STATUS S=AtApp(Owners);if(S!=EFI_SUCCESS)return S;
  CONST PIANO_BOOT_POLICY_REPORT *Policy=PianoBootPolicyReport();if(!PolicyLive(Policy))return EFI_NOT_READY;
  PIANO_DWC3_SERVICE_STATUS Usb;ZeroMem(&Usb,sizeof(Usb));S=PianoUsbControllerServiceGetStatus(&Usb);
  if(S!=EFI_SUCCESS)return Exact(S);
  if(Usb.Revision!=1 || !Usb.Started || Usb.Retained || Usb.ServicesLost || Usb.Phase!=PianoUsbServiceListening)return EFI_NOT_READY;
  CONST PIANO_FB_STORAGE *Storage=PianoFastbootBlockReadStorage();
  if(!Storage || !Storage->Ready || !Storage->Info || !Storage->ReadBlocks)return EFI_NOT_READY;
  S=Storage->Ready(Storage->Context);if(S!=EFI_SUCCESS)return Exact(S);
  if(!PolicyLive(PianoBootPolicyReport()))return EFI_ACCESS_DENIED;
  ZeroMem(Owners,sizeof(*Owners));Owners->Config=*Config;Owners->Report.Revision=1;
  Owners->Report.RegisteredStartedMask=Config->StartedOwnerMask;Owners->Report.RegisteredAbsentMask=Config->AbsentOwnerMask;
  // Runtime is not dereferenced until a fresh protocol lookup matches it.
  PIANO_PRODUCT_RUNTIME_PROTOCOL *Fresh=NULL;EFI_GUID Guid=PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID;
  S=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Fresh);
  if(!PolicyLive(PianoBootPolicyReport()))return EFI_ACCESS_DENIED;
  if(S!=EFI_SUCCESS || Fresh!=Config->Runtime || !Fresh || Fresh->Revision!=PIANO_PRODUCT_RUNTIME_REVISION || !Fresh->RequestAction)
    return S==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(S);
  Owners->RuntimeRequest=Fresh->RequestAction;
  Owners->Report.Initialized=TRUE;Owners->Report.Phase=PianoProductOwnersRunning;
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ExitFence,Owners,&gEfiEventExitBootServicesGuid,&Owners->ExitEvent);
  if(S!=EFI_SUCCESS || !Owners->ExitEvent || Owners->Report.ServicesLost)return Retain(Owners,S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);
  return Owners->Report.Status=EFI_SUCCESS;
}
EFI_STATUS PianoProductOwnersObserveUsbAction(PIANO_PRODUCT_OWNERS *Owners) {
  if(!Owners || !Owners->Report.Initialized)return EFI_INVALID_PARAMETER;
  if(Owners->Report.Retained || Owners->Report.Clean)return EFI_ACCESS_DENIED;
  if(Owners->Report.Busy)return EFI_ALREADY_STARTED;
  EFI_STATUS S=AtApp(Owners);if(S!=EFI_SUCCESS)return S;
  if(!PolicyLive(PianoBootPolicyReport()))return Retain(Owners,EFI_ACCESS_DENIED);
  PIANO_DWC3_SERVICE_STATUS Usb;ZeroMem(&Usb,sizeof(Usb));S=PianoUsbControllerServiceGetStatus(&Usb);
  if(S!=EFI_SUCCESS || Usb.Revision!=1 || Usb.ServicesLost || Usb.Retained)return Retain(Owners,S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);
  if(Usb.Phase!=PianoUsbServiceStopRequested)return EFI_NOT_READY;
  if(Usb.Action<PianoUsbServiceActionContinue || Usb.Action>PianoUsbServiceActionFault)return Retain(Owners,EFI_COMPROMISED_DATA);
  if(Owners->Report.Origin==PianoProductRequestUi)return Retain(Owners,EFI_COMPROMISED_DATA);
  if(Owners->Report.RequestedAction!=PianoUsbServiceActionNone && Owners->Report.RequestedAction!=Usb.Action)return Retain(Owners,EFI_COMPROMISED_DATA);
  S=FreshRuntime(Owners);if(S!=EFI_SUCCESS)return Retain(Owners,S);
  S=Owners->RuntimeRequest(Owners->Config.Runtime,PIANO_PRODUCT_ACTION_RETURN_CORE);
  if(S!=EFI_SUCCESS || Owners->Report.ServicesLost)return Retain(Owners,S==EFI_SUCCESS?EFI_ACCESS_DENIED:S);
  Owners->Report.RequestedAction=Usb.Action;Owners->Report.Origin=PianoProductRequestUsb;Owners->Report.Phase=PianoProductOwnersReturnRequested;
  return Owners->Report.Status=EFI_SUCCESS;
}
EFI_STATUS PianoProductOwnersRequestUiReboot(PIANO_PRODUCT_OWNERS *Owners) {
  if(!Owners || !Owners->Report.Initialized)return EFI_INVALID_PARAMETER;
  if(Owners->Report.Retained || Owners->Report.Clean)return EFI_ACCESS_DENIED;
  if(Owners->Report.Busy)return EFI_ALREADY_STARTED;
  EFI_STATUS S=AtApp(Owners);if(S!=EFI_SUCCESS)return S;
  CONST PIANO_BOOT_POLICY_REPORT *Policy=PianoBootPolicyReport();
  if(!PolicyLive(Policy))return Retain(Owners,EFI_ACCESS_DENIED);
  if(Policy->Dispatching || Policy->Pumping || Policy->ActiveAction!=PIANO_PRODUCT_ACTION_NONE)return EFI_NOT_READY;
  if(Policy->RequestedCoreAction!=PianoUsbServiceActionReboot)return EFI_NOT_READY;
  if(Owners->Report.Origin==PianoProductRequestUsb)return Retain(Owners,EFI_COMPROMISED_DATA);
  S=FreshRuntime(Owners);if(S!=EFI_SUCCESS)return Retain(Owners,S);
  PIANO_DWC3_SERVICE_STATUS Usb;ZeroMem(&Usb,sizeof(Usb));S=PianoUsbControllerServiceGetStatus(&Usb);
  if(S!=EFI_SUCCESS || Usb.Revision!=1 || !Usb.Started || Usb.Retained || Usb.ServicesLost ||
     Usb.Phase!=PianoUsbServiceListening || Usb.Action!=PianoUsbServiceActionNone || Owners->Report.ServicesLost)
    return Retain(Owners,S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);
  // The UI request was already latched by actual Runtime RequestAction. Only
  // this trusted report grants UI origin; no host status or caller bool can.
  Owners->Report.Origin=PianoProductRequestUi;Owners->Report.RequestedAction=PianoUsbServiceActionReboot;
  Owners->Report.Phase=PianoProductOwnersReturnRequested;
  return Owners->Report.Status=EFI_SUCCESS;
}
STATIC BOOLEAN UsbClean(CONST PIANO_USB_SERVICE_RETIRE_REPORT *R) {
  return R->Revision==1 && R->Attempted && R->Clean && !R->Retained && R->Status==EFI_SUCCESS &&
    R->DeviceStatus==EFI_SUCCESS && R->OwnedCloseStatus==EFI_SUCCESS && R->TimerCloseStatus==EFI_SUCCESS &&
    R->DeviceHalted && R->DmaFreed && R->DomainFreed && R->ClocksReleased && R->DmaBuffersFreed==9 && R->ClockReleaseMask==0xff;
}
STATIC BOOLEAN UfsClean(CONST PIANO_UFS_RESET_REPORT *R) {
  return R && R->Started && R->Prepared && R->Returned && R->Clean && !R->Failed && R->TplHeld &&
    R->Result==EFI_SUCCESS && R->Disconnect==EFI_SUCCESS && R->Halt==EFI_SUCCESS && R->Bases==EFI_SUCCESS &&
    R->Dma==EFI_SUCCESS && R->Domain==EFI_SUCCESS && R->Protocols==EFI_SUCCESS && R->Clocks==EFI_SUCCESS &&
    R->DmaFreed==3 && R->Disconnected>0 && R->ProtocolsRemoved==R->Disconnected+1 &&
    !R->TransferDoorbell && !R->TaskDoorbell && !R->TransferRun && !R->TaskRun && !R->Interrupt;
}
EFI_STATUS PianoProductOwnersRetire(PIANO_PRODUCT_OWNERS *Owners) {
  if(!Owners || !Owners->Report.Initialized)return EFI_INVALID_PARAMETER;
  if(Owners->Report.Retained || Owners->Report.Clean)return EFI_ACCESS_DENIED;
  if(Owners->Report.Busy)return EFI_ALREADY_STARTED;
  if(Owners->Report.Phase!=PianoProductOwnersReturnRequested || Owners->Report.RequestedAction==PianoUsbServiceActionNone)return EFI_NOT_READY;
  EFI_STATUS S=AtApp(Owners);if(S!=EFI_SUCCESS)return S;
  CONST PIANO_BOOT_POLICY_REPORT *Policy=PianoBootPolicyReport();
  if(!PolicyLive(Policy))return Retain(Owners,EFI_ACCESS_DENIED);
  if(Policy->Dispatching || Policy->Pumping || Policy->ActiveAction!=PIANO_PRODUCT_ACTION_NONE)return EFI_NOT_READY;
  PIANO_DWC3_SERVICE_STATUS Usb;ZeroMem(&Usb,sizeof(Usb));S=PianoUsbControllerServiceGetStatus(&Usb);
  BOOLEAN OriginReady=Owners->Report.Origin==PianoProductRequestUsb?
    (Usb.Phase==PianoUsbServiceStopRequested && Usb.Action==Owners->Report.RequestedAction):
    Owners->Report.Origin==PianoProductRequestUi?
    (Owners->Report.RequestedAction==PianoUsbServiceActionReboot && Policy->RequestedCoreAction==PianoUsbServiceActionReboot &&
     Usb.Started && Usb.Phase==PianoUsbServiceListening && Usb.Action==PianoUsbServiceActionNone):FALSE;
  if(S!=EFI_SUCCESS || Usb.Revision!=1 || !OriginReady || Usb.ServicesLost || Usb.Retained)
    return Retain(Owners,S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);
  Owners->Report.Busy=TRUE;Owners->Report.Phase=PianoProductOwnersRetiring;
  Owners->Report.PolicyStatus=S=PianoBootPolicyStop();
  Policy=PianoBootPolicyReport();
  if(S!=EFI_SUCCESS || Owners->Report.ServicesLost || !Policy || Policy->ProtocolInstalled || Policy->Dispatching || Policy->Pumping || Policy->Retained || Policy->ServicesLost)
    return Retain(Owners,S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);
  Owners->Report.PolicyStopped=TRUE;Owners->Report.RetiredMask|=PIANO_OWNER_POLICY;
  Owners->Report.UsbStatus=S=PianoUsbControllerServiceStop(EFI_SUCCESS,&Owners->Report.Usb);
  if(Owners->Report.RequestedAction==PianoUsbServiceActionBoot) {
    // Consume even an error/partial token before rejecting owner cleanup.
    EFI_STATUS Take=PianoDwc3ConsumeBootAction(&Owners->Report.Boot);Owners->Report.BootActionConsumed=TRUE;
    if(Take!=EFI_SUCCESS && S==EFI_SUCCESS)S=Take;
  }
  if(S!=EFI_SUCCESS || Owners->Report.ServicesLost || !UsbClean(&Owners->Report.Usb))return Retain(Owners,S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);
  if(Owners->Report.RequestedAction==PianoUsbServiceActionBoot &&
     (Owners->Report.Boot.Status!=EFI_SUCCESS || !Owners->Report.Boot.Taken || Owners->Report.Boot.Retained || !Owners->Report.Boot.Token ||
      !Owners->Report.Boot.Proof.AckCompleted || !Owners->Report.Boot.Proof.QueueEmpty || !Owners->Report.Boot.Proof.DeviceHalted ||
      !Owners->Report.Boot.Proof.DmaFreed || !Owners->Report.Boot.Proof.DispatchFrozen || Owners->Report.Boot.Proof.AckBytes!=4 ||
      Owners->Report.Boot.Proof.DmaBuffersFreed!=9))return Retain(Owners,EFI_COMPROMISED_DATA);
  Owners->Report.UsbStopped=TRUE;Owners->Report.RetiredMask|=PIANO_OWNER_USB;
  Owners->Report.ProofStatus=S=PianoUsbControllerMakeRetiredUsbProof(Owners->Config.Fdt,&Owners->UsbProof);
  if(S!=EFI_SUCCESS || !Owners->UsbProof.Valid || Owners->Report.ServicesLost)return Retain(Owners,S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);
  // USB proxy has been revoked by its exact Stop; invalidate partition tokens.
  PianoFastbootBlockReadStop();Owners->Report.BridgeStopped=TRUE;Owners->Report.RetiredMask|=PIANO_OWNER_BRIDGE;
  if(PianoFastbootBlockReadStorage()!=NULL || Owners->Report.ServicesLost)return Retain(Owners,EFI_COMPROMISED_DATA);
  Owners->Report.AcceptStatus=S=PianoUfsAcceptRetiredUsb(&Owners->UsbProof);
  if(S!=EFI_SUCCESS || Owners->Report.ServicesLost)return Retain(Owners,S==EFI_SUCCESS?EFI_ACCESS_DENIED:S);
  Owners->Report.ProofAccepted=TRUE;
  Owners->OuterTpl=gBS->RaiseTPL(TPL_CALLBACK);Owners->Report.OuterTplHeld=TRUE;
  if(Owners->OuterTpl!=TPL_APPLICATION)return Retain(Owners,EFI_COMPROMISED_DATA);
  Owners->Report.PrepareStatus=S=PianoUfsBlockIoPrepareForReset();
  if(S==EFI_SUCCESS)Owners->Report.ShutdownStatus=S=PianoUfsBlockIoShutdownForReset();
  else Owners->Report.ShutdownStatus=EFI_NOT_STARTED;
  CONST PIANO_UFS_RESET_REPORT *Ufs=PianoUfsResetShutdownReport();if(Ufs)Owners->Report.Ufs=*Ufs;
  if(S!=EFI_SUCCESS || Owners->Report.ServicesLost || !UfsClean(Ufs))return Retain(Owners,S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);
  Owners->Report.UfsStopped=TRUE;Owners->Report.RetiredMask|=PIANO_OWNER_UFS;
  // Typed UFS cleanup holds CALLBACK. Our outer lease restores the original
  // APP only after full DMA/domain/clock proof, before software input teardown.
  gBS->RestoreTPL(Owners->OuterTpl);Owners->Report.OuterTplHeld=FALSE;
  if(Owners->Report.ServicesLost)return Retain(Owners,EFI_ACCESS_DENIED);
  Owners->Report.InputStatus=S=Owners->Config.StopInput(Owners->Config.InputContext,&Owners->Report.Input);
  PIANO_PRODUCT_INPUT_RETIRE_REPORT *Input=&Owners->Report.Input;
  if(S!=EFI_SUCCESS || Input->Revision!=1 || !Input->Started || !Input->Returned || !Input->Clean || Input->Retained || Input->ServicesLost ||
     Input->Status!=EFI_SUCCESS || Input->Timer!=EFI_SUCCESS || Input->Protocols!=EFI_SUCCESS || Owners->Report.ServicesLost)
    return Retain(Owners,S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);
  Owners->Report.InputStopped=TRUE;Owners->Report.RetiredMask|=PIANO_OWNER_INPUT;
  Owners->Report.EventStatus=S=gBS->CloseEvent(Owners->ExitEvent);
  if(S!=EFI_SUCCESS || Owners->Report.ServicesLost)return Retain(Owners,S==EFI_SUCCESS?EFI_ACCESS_DENIED:S);
  Owners->ExitEvent=NULL;Owners->Report.ManagerEventClosed=TRUE;
  if(Owners->Report.RetiredMask!=Owners->Report.RegisteredStartedMask)return Retain(Owners,EFI_COMPROMISED_DATA);
  Owners->Report.Busy=FALSE;Owners->Report.Clean=TRUE;Owners->Report.Phase=PianoProductOwnersClean;
  // Fault is a clean stop result for diagnostics, never an OS/reset permit.
  Owners->Report.AllowedAction=Owners->Report.RequestedAction==PianoUsbServiceActionFault?PianoUsbServiceActionNone:Owners->Report.RequestedAction;
  return Owners->Report.Status=EFI_SUCCESS;
}
