// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoBootPolicy.h"
#include <Protocol/PianoProductIdle.h>
#include "PianoProductPayload.h"
#include "PianoUsbStorageExperiment.h"
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/HiiDatabase.h>
#include <Protocol/HiiString.h>
#include <Protocol/HiiFont.h>
#include <Protocol/HiiConfigRouting.h>
#include <Protocol/FormBrowser2.h>
#include <Protocol/DisplayProtocol.h>
#include <Protocol/Variable.h>
#include <Protocol/VariableWrite.h>
#include <Guid/EventGroup.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/TimerLib.h>
#include <Library/DebugLib.h>

STATIC EFI_GUID mIdleGuid=PIANO_PRODUCT_IDLE_PROTOCOL_GUID;
STATIC PIANO_PRODUCT_IDLE_PROTOCOL mIdle;
STATIC EFI_GUID mRuntimeGuid=PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID;
STATIC EFI_GUID mSetupGuid={0x462caa21,0x7614,0x4503,{0x83,0x6e,0x8a,0xb6,0xf4,0x66,0x23,0x31}};
STATIC EFI_GUID mShellGuid={0x7c04a583,0x9e3e,0x4f1c,{0xad,0x65,0xe0,0x52,0x68,0xd0,0xb4,0xd1}};
STATIC PIANO_BOOT_POLICY_REPORT mReport;
STATIC EFI_HANDLE mParent,mProtocolHandle;
STATIC EFI_EVENT mExitEvent,mKeyEvent;
STATIC VOID *mKeyRegistration;
STATIC volatile BOOLEAN mAlive,mKeysDirty;
STATIC PIANO_PRODUCT_PAYLOAD_VIEW mPayload;
STATIC BOOLEAN mPayloadLoan;
STATIC struct {EFI_HANDLE Handle;EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *Input;VOID *Notify[4];EFI_UNREGISTER_KEYSTROKE_NOTIFY Unregister;} mKeys[32];
STATIC UINTN mKeysCount;
STATIC volatile BOOLEAN mStartupWindow;
STATIC BOOLEAN mStartupWindowUsed;
STATIC PIANO_PRODUCT_RUNTIME_PROTOCOL mRuntime;
STATIC CONST PIANO_SMMU_RETIRED_USB_PROOF *mStartupUsbProof;
STATIC CONST VOID *mStartupUsbFdt;
BOOLEAN PianoSetStandardKeyNavigation(BOOLEAN Enable);
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC VOID RetainAndHalt(VOID){mReport.Retained=TRUE;
#ifdef __aarch64__
  __asm__ volatile("msr daifset, #15":::"memory");
#endif
  CpuDeadLoop();}
STATIC VOID RequireAlive(VOID){if(!mAlive){mReport.ServicesLost=mReport.Retained=TRUE;
#ifdef __aarch64__
  __asm__ volatile("msr daifset, #15":::"memory");
#endif
  CpuDeadLoop();}}
STATIC EFI_STATUS AtApp(VOID) {
  RequireAlive();EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Old);
  return Old==TPL_APPLICATION?EFI_SUCCESS:EFI_UNSUPPORTED;
}
STATIC UINT64 Critical(VOID) {
#ifdef __aarch64__
  UINT64 Mask;__asm__ volatile("mrs %0, daif\n msr daifset, #3":"=r"(Mask)::"memory");return Mask;
#else
  return 0;
#endif
}
STATIC VOID EndCritical(UINT64 Mask) {
#ifdef __aarch64__
  __asm__ volatile("msr daif, %0"::"r"(Mask):"memory");
#else
  (VOID)Mask;
#endif
}
STATIC BOOLEAN EFIAPI Alive(PIANO_PRODUCT_RUNTIME_PROTOCOL *This){return This==&mRuntime && mAlive;}
STATIC EFI_STATUS EFIAPI Request(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 Action) {
  if(This!=&mRuntime || Action<PIANO_PRODUCT_ACTION_SIMPLEINIT || Action>PIANO_PRODUCT_ACTION_REQUEST_BOOT_STABLE)return EFI_INVALID_PARAMETER;
  PIANO_USB_SERVICE_ACTION Reason=Action==PIANO_PRODUCT_ACTION_REQUEST_REBOOT?PianoUsbServiceActionReboot:
    Action==PIANO_PRODUCT_ACTION_REQUEST_CONTINUE?PianoUsbServiceActionContinue:
    Action==PIANO_PRODUCT_ACTION_REQUEST_BOOT_STABLE?PianoUsbServiceActionBoot:PianoUsbServiceActionNone;
  BOOLEAN Typed=Reason!=PianoUsbServiceActionNone;
  if(Typed)Action=PIANO_PRODUCT_ACTION_RETURN_CORE;
  UINT64 Mask=Critical();EFI_STATUS S=EFI_SUCCESS;
  if(!mAlive)S=EFI_ABORTED;
  else if(Typed && Reason!=PianoUsbServiceActionBoot && (mReport.ActiveAction<PIANO_PRODUCT_ACTION_SIMPLEINIT || mReport.ActiveAction>PIANO_PRODUCT_ACTION_SHELL))S=EFI_ACCESS_DENIED;
  else if(Typed && mReport.PendingAction==PIANO_PRODUCT_ACTION_RETURN_CORE && mReport.RequestedCoreAction!=Reason)S=EFI_ACCESS_DENIED;
  else if(mReport.PendingAction==PIANO_PRODUCT_ACTION_RETURN_CORE && Action!=PIANO_PRODUCT_ACTION_RETURN_CORE)S=EFI_ACCESS_DENIED;
  else if(!Typed && Action<=PIANO_PRODUCT_ACTION_SHELL && mReport.ActiveAction==Action){ /* same UI: successful no-op, preserve any newer pending action */ }
  else if(mReport.PendingAction!=Action) {
    if(mReport.Sequence==MAX_UINT64)S=EFI_OUT_OF_RESOURCES;
    else {if(Typed)mReport.RequestedCoreAction=Reason;mReport.PendingAction=Action;++mReport.Sequence;}
  }
  EndCritical(Mask);return S;
}
STATIC EFI_STATUS EFIAPI Pending(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 *Action,UINT64 *Sequence) {
  if(This!=&mRuntime || !Action || !Sequence)return EFI_INVALID_PARAMETER;
  UINT64 Mask=Critical();*Action=mReport.PendingAction;*Sequence=mReport.Sequence;BOOLEAN Live=mAlive;EndCritical(Mask);
  return Live?EFI_SUCCESS:EFI_ABORTED;
}
STATIC EFI_STATUS EFIAPI Ack(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT64 Sequence) {
  if(This!=&mRuntime || !Sequence)return EFI_INVALID_PARAMETER;
  UINT64 Mask=Critical();EFI_STATUS S=EFI_SUCCESS;
  if(!mAlive)S=EFI_ABORTED;
  else if(Sequence!=mReport.Sequence || mReport.PendingAction==PIANO_PRODUCT_ACTION_NONE)S=EFI_NOT_READY;
  else mReport.PendingAction=PIANO_PRODUCT_ACTION_NONE;
  EndCritical(Mask);return S;
}
STATIC EFI_STATUS EFIAPI F12(EFI_KEY_DATA *Key) {
  if(!Key || Key->Key.ScanCode!=SCAN_F12 || Key->Key.UnicodeChar)return EFI_SUCCESS;
  if(mReport.ActiveAction==PIANO_PRODUCT_ACTION_SETUP)return EFI_SUCCESS;
  return Request(&mRuntime,PIANO_PRODUCT_ACTION_SETUP); // CPU latch only; no BS/StartImage.
}
STATIC EFI_STATUS EFIAPI StartupEsc(EFI_KEY_DATA *Key) {
  if(!mStartupWindow || !Key || Key->Key.UnicodeChar)return EFI_SUCCESS;
  if(Key->Key.ScanCode!=SCAN_ESC && !(mReport.Entry==PianoBootEntryRecovery &&
     (Key->Key.ScanCode==SCAN_UP || Key->Key.ScanCode==SCAN_DOWN)))return EFI_SUCCESS;
  return Request(&mRuntime,PIANO_PRODUCT_ACTION_SIMPLEINIT);
}
STATIC EFI_STATUS RegisterEsc(UINTN I) {
  CONST UINT16 Scans[]={SCAN_ESC,SCAN_UP,SCAN_DOWN};
  UINTN Count=mReport.Entry==PianoBootEntryRecovery?3:1;
  for(UINTN J=0;J<Count;++J) {
    if(mKeys[I].Notify[J+1])continue;
    EFI_KEY_DATA Key;ZeroMem(&Key,sizeof(Key));Key.Key.ScanCode=Scans[J];
    VOID *Token=NULL;EFI_STATUS S=mKeys[I].Input->RegisterKeyNotify(mKeys[I].Input,&Key,StartupEsc,&Token);RequireAlive();
    if(S==EFI_SUCCESS && Token){mKeys[I].Notify[J+1]=Token;continue;}
    if(Token || S==EFI_SUCCESS || !EFI_ERROR(S))mReport.Retained=TRUE;
    return S==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(S);
  }
  return EFI_SUCCESS;
}
STATIC VOID EFIAPI KeysChanged(EFI_EVENT Event,VOID *Context){(VOID)Event;(VOID)Context;mKeysDirty=TRUE;}
STATIC VOID EFIAPI Ebs(EFI_EVENT Event,VOID *Context){(VOID)Event;(VOID)Context;mStartupWindow=FALSE;mAlive=FALSE;mReport.ServicesLost=TRUE;mReport.IdleKnown=FALSE;}
STATIC EFI_STATUS RefreshKeys(VOID) {
  // Protocol removal does not signal RegisterProtocolNotify. Fresh lookup on
  // every APP slice prevents using a retired or replaced keyboard instance.
  for(UINTN I=0;I<mKeysCount;) {
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *Fresh=NULL;
    EFI_STATUS S=gBS->HandleProtocol(mKeys[I].Handle,&gEfiSimpleTextInputExProtocolGuid,(VOID **)&Fresh);RequireAlive();
    if(S==EFI_SUCCESS && Fresh==mKeys[I].Input && Fresh && Fresh->UnregisterKeyNotify==mKeys[I].Unregister){
      if(mStartupWindow && !mKeys[I].Notify[1]){S=RegisterEsc(I);if(S!=EFI_SUCCESS)return mReport.KeyStatus=S;}
      ++I;continue;}
    if((S==EFI_SUCCESS && (!Fresh || Fresh==mKeys[I].Input)) ||
       (S!=EFI_SUCCESS && S!=EFI_NOT_FOUND && S!=EFI_UNSUPPORTED && S!=EFI_INVALID_PARAMETER)){
      mReport.Retained=TRUE;return mReport.KeyStatus=S==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(S);
    }
    // The provider owns its notification list. Do not dereference the former
    // interface/token after uninstall/replacement; enroll the new one afresh.
    if(I+1<mKeysCount)CopyMem(&mKeys[I],&mKeys[I+1],(mKeysCount-I-1)*sizeof(mKeys[0]));
    --mKeysCount;mKeysDirty=TRUE;
  }
  mReport.KeyboardProviders=(UINT32)mKeysCount;
  if(!mKeysDirty)return EFI_SUCCESS;
  mKeysDirty=FALSE;
  EFI_HANDLE *Handles=NULL;UINTN Count=0;EFI_STATUS S=gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleTextInputExProtocolGuid,NULL,&Count,&Handles);RequireAlive();
  if(S==EFI_NOT_FOUND && !Count && !Handles)return mReport.KeyStatus=EFI_NOT_READY;
  if((Count && !Handles) || (!Count && Handles) || Count>256 || (S!=EFI_SUCCESS && (Count || Handles))){mReport.Retained=TRUE;return mReport.KeyStatus=EFI_COMPROMISED_DATA;}
  if(S!=EFI_SUCCESS)return mReport.KeyStatus=Exact(S);
  EFI_STATUS WindowFailure=EFI_SUCCESS;
  for(UINTN I=0;I<Count;++I) {
    BOOLEAN Seen=FALSE;for(UINTN J=0;J<mKeysCount;++J)if(mKeys[J].Handle==Handles[I])Seen=TRUE;
    if(Seen)continue;
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *Input=NULL;
    S=gBS->HandleProtocol(Handles[I],&gEfiSimpleTextInputExProtocolGuid,(VOID **)&Input);RequireAlive();
    if(S!=EFI_SUCCESS || !Input || !Input->RegisterKeyNotify || !Input->UnregisterKeyNotify)continue;
    if(mKeysCount==ARRAY_SIZE(mKeys)){S=EFI_OUT_OF_RESOURCES;break;}
    EFI_KEY_DATA Key;ZeroMem(&Key,sizeof(Key));Key.Key.ScanCode=SCAN_F12;
    VOID *Token=NULL;S=Input->RegisterKeyNotify(Input,&Key,F12,&Token);RequireAlive();
    if(S!=EFI_SUCCESS || !Token){
      // Error with a token, warning, or success without a token leaves callback
      // ownership uncertain. Keep this policy resident and refuse dispatch.
      if(Token || S==EFI_SUCCESS || !EFI_ERROR(S))mReport.Retained=TRUE;
      mReport.KeyStatus=Exact(S);continue;
    }
    mKeys[mKeysCount].Handle=Handles[I];mKeys[mKeysCount].Input=Input;ZeroMem(mKeys[mKeysCount].Notify,sizeof(mKeys[mKeysCount].Notify));mKeys[mKeysCount].Notify[0]=Token;mKeys[mKeysCount].Unregister=Input->UnregisterKeyNotify;++mKeysCount;
    if(mStartupWindow){WindowFailure=RegisterEsc(mKeysCount-1);if(WindowFailure!=EFI_SUCCESS)break;}
  }
  EFI_STATUS Free=Handles?gBS->FreePool(Handles):EFI_SUCCESS;RequireAlive();if(Free!=EFI_SUCCESS)mReport.Retained=TRUE;
  mReport.KeyboardProviders=(UINT32)mKeysCount;
  return mReport.KeyStatus=mReport.Retained?EFI_COMPROMISED_DATA:WindowFailure!=EFI_SUCCESS?WindowFailure:mKeysCount?EFI_SUCCESS:EFI_NOT_READY;
}
STATIC EFI_STATUS EFIAPI ReadIdle(PIANO_PRODUCT_IDLE_PROTOCOL *This,CONST PIANO_PRODUCT_RUNTIME_PROTOCOL *Runtime,UINT64 *Sample,BOOLEAN *BulkActive){
  if(This!=&mIdle || Runtime!=&mRuntime || !Sample || !BulkActive)return EFI_INVALID_PARAMETER;
  UINT64 Mask=Critical();EFI_STATUS S=EFI_NOT_READY;
  if(mAlive && mReport.ProtocolInstalled && mReport.IdleInstalled && !mReport.Pumping && !mReport.Retained && mReport.IdleKnown && mReport.IdleSample){
    *Sample=mReport.IdleSample;*BulkActive=mReport.IdleBulkActive;S=EFI_SUCCESS;
  }
  EndCritical(Mask);return S;
}
STATIC EFI_STATUS StartupUsbAbsent(VOID) {
  if(!mStartupUsbProof || !mStartupUsbFdt)return EFI_NOT_READY;
  EFI_STATUS S=PianoUsbControllerValidateStartupFailureProof(mStartupUsbFdt,mStartupUsbProof);RequireAlive();
  if(S!=EFI_SUCCESS)return Exact(S);
  PIANO_DWC3_SERVICE_STATUS Actual;ZeroMem(&Actual,sizeof(Actual));S=PianoUsbControllerServiceGetStatus(&Actual);RequireAlive();
  if(S!=EFI_SUCCESS)return Exact(S);
  // A verified pre-DMA rollback preserves its original startup error. Fault
  // here is diagnostic state, never a live USB request or permission to exit.
  BOOLEAN StartupFault=Actual.Action==PianoUsbServiceActionFault &&
    mStartupUsbProof->Execution.Kind==PIANO_USB_RETIRE_STARTUP_FAILED_CLEAN &&
    mStartupUsbProof->Execution.StartupStatus==EFI_TIMEOUT && Actual.LastStatus==EFI_TIMEOUT;
  if(Actual.Revision!=1 || Actual.Started || Actual.Retained || Actual.ServicesLost || Actual.Busy ||
     Actual.WorkPending || Actual.QueuedEvents || Actual.Phase!=PianoUsbServiceOff ||
     (Actual.Action!=PianoUsbServiceActionNone && !StartupFault))
    return EFI_COMPROMISED_DATA;
  mReport.Usb=Actual;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Pump(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 Reason,UINTN BudgetUs) {
  if(This!=&mRuntime || !Reason || (Reason&~(PIANO_PRODUCT_PUMP_WAIT_EVENT|PIANO_PRODUCT_PUMP_GUI|PIANO_PRODUCT_PUMP_APP)) || !BudgetUs || BudgetUs>10000)return EFI_INVALID_PARAMETER;
  if(!mAlive)return EFI_ABORTED;
  if(mReport.Pumping)return EFI_NOT_READY;
  EFI_STATUS S=AtApp();if(S!=EFI_SUCCESS)return S;
  mReport.Pumping=TRUE;mReport.IdleKnown=FALSE;++mReport.PumpCalls;
  mReport.KeyStatus=RefreshKeys();RequireAlive();
  if(mReport.Retained){mReport.Pumping=FALSE;return EFI_COMPROMISED_DATA;}
  if(mStartupUsbProof){
    S=StartupUsbAbsent();
    if(S==EFI_SUCCESS){
      if(mReport.IdleSample==MAX_UINT64)S=EFI_OUT_OF_RESOURCES;
      else {++mReport.IdleSample;mReport.IdleBulkActive=FALSE;mReport.IdleKnown=TRUE;}
      if(S==EFI_SUCCESS && mReport.ActiveAction!=PIANO_PRODUCT_ACTION_NONE &&
         mReport.PendingAction>=PIANO_PRODUCT_ACTION_SIMPLEINIT && mReport.PendingAction<=PIANO_PRODUCT_ACTION_SHELL &&
         mReport.PendingAction!=mReport.ActiveAction)S=EFI_ABORTED;
    }
    mReport.Pumping=FALSE;return mReport.LastPump=Exact(S);
  }
  S=PianoUsbControllerServicePumpApp(Reason,BudgetUs);RequireAlive();
  PIANO_DWC3_SERVICE_STATUS Actual;ZeroMem(&Actual,sizeof(Actual));
  EFI_STATUS Status=PianoUsbControllerServiceGetStatus(&Actual);RequireAlive();
  if(Status==EFI_SUCCESS && Actual.Revision==1)mReport.Usb=Actual;else {ZeroMem(&mReport.Usb,sizeof(mReport.Usb));if(S==EFI_SUCCESS)S=Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Status;}
  if(Status==EFI_SUCCESS && Actual.Revision==1 && Actual.Phase==PianoUsbServiceStopRequested &&
     Actual.Action>=PianoUsbServiceActionContinue && Actual.Action<=PianoUsbServiceActionFault){
    // Only latch a parent return. The GUI yields cooperatively; this APP pump
    // never performs nested StartImage or a partial device shutdown.
    EFI_STATUS Latch=Request(&mRuntime,PIANO_PRODUCT_ACTION_RETURN_CORE);if(Latch!=EFI_SUCCESS)S=Latch;
  }
  if(S==EFI_SUCCESS && (!mReport.Usb.Started || mReport.Usb.Retained || mReport.Usb.ServicesLost))S=EFI_NOT_READY;
  if((S==EFI_SUCCESS || S==EFI_NOT_READY) && mReport.ActiveAction!=PIANO_PRODUCT_ACTION_NONE &&
     mReport.PendingAction>=PIANO_PRODUCT_ACTION_SIMPLEINIT && mReport.PendingAction<=PIANO_PRODUCT_ACTION_SHELL &&
     mReport.PendingAction!=mReport.ActiveAction)S=EFI_ABORTED; // cooperative UI yield, not EBS/USB shutdown
  if((S==EFI_SUCCESS || S==EFI_NOT_READY) && Status==EFI_SUCCESS && Actual.Revision==1 && Actual.Started &&
     Actual.Phase==PianoUsbServiceListening && !Actual.Retained && !Actual.ServicesLost && !Actual.Busy){
    if(mReport.IdleSample==MAX_UINT64)S=EFI_OUT_OF_RESOURCES;
    else {++mReport.IdleSample;mReport.IdleBulkActive=Actual.BulkActive;mReport.IdleKnown=TRUE;}
  }
  mReport.Pumping=FALSE;return mReport.LastPump=Exact(S);
}
STATIC PIANO_PRODUCT_RUNTIME_PROTOCOL mRuntime={PIANO_PRODUCT_RUNTIME_REVISION,Pump,Alive,Request,Pending,Ack};
STATIC EFI_STATUS RetireEsc(VOID) {
  if(mReport.Retained)return EFI_COMPROMISED_DATA;
  for(UINTN I=0;I<mKeysCount;++I){
    if(!mKeys[I].Notify[1] && !mKeys[I].Notify[2] && !mKeys[I].Notify[3])continue;
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *Fresh=NULL;
    EFI_STATUS S=gBS->HandleProtocol(mKeys[I].Handle,&gEfiSimpleTextInputExProtocolGuid,(VOID **)&Fresh);RequireAlive();
    if(S==EFI_SUCCESS && Fresh==mKeys[I].Input && Fresh && Fresh->UnregisterKeyNotify==mKeys[I].Unregister){
      for(UINTN J=1;J<4;++J)if(mKeys[I].Notify[J]) {
        S=Fresh->UnregisterKeyNotify(Fresh,mKeys[I].Notify[J]);RequireAlive();
        if(S!=EFI_SUCCESS){mReport.Retained=TRUE;return Exact(S);}mKeys[I].Notify[J]=NULL;
      }
    }else if((S==EFI_SUCCESS && (!Fresh || Fresh==mKeys[I].Input)) ||
      (S!=EFI_SUCCESS && S!=EFI_NOT_FOUND && S!=EFI_UNSUPPORTED && S!=EFI_INVALID_PARAMETER)){
      mReport.Retained=TRUE;return S==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(S);
    }else for(UINTN J=1;J<4;++J)mKeys[I].Notify[J]=NULL; // Removed/replaced provider owns its old callback list.
  }return EFI_SUCCESS;
}
STATIC BOOLEAN Space(CHAR8 C){return C==' ' || C=='\t' || C=='\r' || C=='\n';}
EFI_STATUS PianoBootPolicySetEntryBootArgs(CONST CHAR8 *BootArgs,UINTN Bytes) {
  if(!mReport.Initialized || !mReport.ProtocolInstalled)return EFI_NOT_READY;
  if(mStartupWindowUsed || mReport.Dispatching || mReport.Pumping || mReport.Retained)return EFI_ACCESS_DENIED;
  EFI_STATUS S=AtApp();if(S!=EFI_SUCCESS)return S;
  mReport.Entry=PianoBootEntryUnknown;mReport.StartupDefaultAction=PIANO_PRODUCT_ACTION_SIMPLEINIT;
  // ABL derives this exact non-Android token from its recovery flag. It is an
  // entry-policy hint only, never authority for new memory or storage access.
  STATIC CONST CHAR8 Key[]="bootmonitor.bootmode=";
  if(!BootArgs || !Bytes || Bytes>8192 || BootArgs[Bytes-1]!=0)return EFI_SUCCESS;
  for(UINTN I=0;I<Bytes-1;++I)if(!BootArgs[I] ||
     ((!Space(BootArgs[I])) && ((UINT8)BootArgs[I]<0x20 || (UINT8)BootArgs[I]>0x7e)))return EFI_SUCCESS;
  PIANO_BOOT_ENTRY Entry=PianoBootEntryUnknown;UINTN Matches=0;
  for(UINTN I=0;I<Bytes-1;) {
    while(I<Bytes-1 && Space(BootArgs[I]))++I;
    UINTN Start=I;while(I<Bytes-1 && !Space(BootArgs[I]))++I;
    UINTN Length=I-Start;
    if(Length<sizeof(Key)-1 || CompareMem(BootArgs+Start,Key,sizeof(Key)-1))continue;
    if(++Matches!=1)return EFI_SUCCESS;
    CONST CHAR8 *Value=BootArgs+Start+sizeof(Key)-1;UINTN ValueBytes=Length-(sizeof(Key)-1);
    if(ValueBytes==8 && !CompareMem(Value,"recovery",8))Entry=PianoBootEntryRecovery;
    else if(ValueBytes==6 && !CompareMem(Value,"normal",6))Entry=PianoBootEntryNormal;
  }
  if(Matches==1)mReport.Entry=Entry;
  if(mReport.Entry==PianoBootEntryRecovery)mReport.StartupDefaultAction=PIANO_PRODUCT_ACTION_REQUEST_BOOT_STABLE;
  return EFI_SUCCESS;
}
EFI_STATUS PianoBootPolicyStartupWindow(UINTN Milliseconds) {
  if(!Milliseconds || Milliseconds>3000)return EFI_INVALID_PARAMETER;
  if(!mReport.Initialized || !mReport.ProtocolInstalled)return EFI_NOT_READY;
  if(mStartupWindowUsed || mStartupWindow || mReport.Pumping || mReport.Dispatching || mReport.Retained)return EFI_ACCESS_DENIED;
  EFI_STATUS S=AtApp();if(S!=EFI_SUCCESS)return S;
  if(!gBS->Stall)return EFI_UNSUPPORTED;
  UINT64 First,Last;UINT64 Frequency=GetPerformanceCounterProperties(&First,&Last);
  if(!Frequency || First==Last)return EFI_UNSUPPORTED;
  BOOLEAN Down=First>Last;UINT64 Start=GetPerformanceCounter(),Previous=Start;
  BOOLEAN Navigation=FALSE,PreviousNavigation=FALSE;
  if(mReport.Entry==PianoBootEntryRecovery){PreviousNavigation=PianoSetStandardKeyNavigation(TRUE);Navigation=TRUE;}
  mStartupWindowUsed=TRUE;mStartupWindow=TRUE;mKeysDirty=TRUE;
  for(UINTN Slice=0;Slice<3100;++Slice){
    UINT32 Action;UINT64 Sequence;S=Pending(&mRuntime,&Action,&Sequence);RequireAlive();
    if(S!=EFI_SUCCESS || Action!=PIANO_PRODUCT_ACTION_NONE){S=S==EFI_SUCCESS?EFI_SUCCESS:S;goto Done;}
    S=Pump(&mRuntime,PIANO_PRODUCT_PUMP_APP,1000);RequireAlive();
    if(S!=EFI_SUCCESS && S!=EFI_NOT_READY)goto Done;
    if(mReport.KeyStatus!=EFI_SUCCESS && mReport.KeyStatus!=EFI_NOT_READY){S=mReport.KeyStatus;goto Done;}
    S=Pending(&mRuntime,&Action,&Sequence);RequireAlive();if(S!=EFI_SUCCESS || Action!=PIANO_PRODUCT_ACTION_NONE)goto Done;
    UINT64 Now=GetPerformanceCounter();
    if((Down&&Now>Previous)||(!Down&&Now<Previous)){S=EFI_COMPROMISED_DATA;goto Done;}
    Previous=Now;UINT64 Delta=Down?Start-Now:Now-Start;
    if(GetTimeInNanoSecond(Delta)/1000000>=Milliseconds){S=EFI_SUCCESS;goto Done;}
    S=gBS->Stall(1000);RequireAlive();if(S!=EFI_SUCCESS){S=Exact(S);goto Done;}
  }
  S=EFI_TIMEOUT;
Done:
  mStartupWindow=FALSE;
  EFI_STATUS Cleanup=RetireEsc();
  if(Navigation)PianoSetStandardKeyNavigation(PreviousNavigation);
  if(Cleanup!=EFI_SUCCESS)return Cleanup;
  if(S==EFI_SUCCESS && (mReport.StartupDefaultAction==PIANO_PRODUCT_ACTION_REQUEST_BOOT_STABLE ||
                       mReport.StartupDefaultAction==PIANO_PRODUCT_ACTION_SETUP)) {
    UINT64 Mask=Critical();
    if(mReport.PendingAction==PIANO_PRODUCT_ACTION_NONE)S=Request(&mRuntime,mReport.StartupDefaultAction);
    EndCritical(Mask);
  }
  return S;
}
EFI_STATUS PianoBootPolicySetStartupTarget(UINT32 Target) {
  if(Target>3)return EFI_INVALID_PARAMETER;
  if(!mReport.Initialized || !mReport.ProtocolInstalled)return EFI_NOT_READY;
  if(mStartupWindowUsed || mStartupWindow || mReport.Pumping || mReport.Dispatching || mReport.Retained)return EFI_ACCESS_DENIED;
  EFI_STATUS S=AtApp();if(S!=EFI_SUCCESS)return S;
  if(Target)mReport.StartupDefaultAction=Target==2?PIANO_PRODUCT_ACTION_REQUEST_BOOT_STABLE:
    Target==3?PIANO_PRODUCT_ACTION_SETUP:PIANO_PRODUCT_ACTION_SIMPLEINIT;
  return EFI_SUCCESS;
}
EFI_STATUS PianoBootPolicySetUsbStartupFailure(CONST VOID *Fdt,CONST PIANO_SMMU_RETIRED_USB_PROOF *Proof) {
  if(!Fdt || !Proof)return EFI_INVALID_PARAMETER;
  if(!mReport.Initialized || !mReport.ProtocolInstalled)return EFI_NOT_READY;
  if(mStartupUsbProof || mStartupWindowUsed || mStartupWindow || mReport.Dispatching || mReport.Pumping || mReport.Retained)return EFI_ACCESS_DENIED;
  EFI_STATUS S=AtApp();if(S!=EFI_SUCCESS)return S;
  S=PianoUsbControllerValidateStartupFailureProof(Fdt,Proof);RequireAlive();if(S!=EFI_SUCCESS)return Exact(S);
  mStartupUsbFdt=Fdt;mStartupUsbProof=Proof;
  S=StartupUsbAbsent();if(S!=EFI_SUCCESS){mStartupUsbFdt=NULL;mStartupUsbProof=NULL;}return S;
}
STATIC EFI_STATUS SetupReady(BOOLEAN *Unavailable) {
  *Unavailable=FALSE;
  EFI_GUID *Guids[]={&gEfiHiiDatabaseProtocolGuid,&gEfiHiiStringProtocolGuid,&gEfiHiiFontProtocolGuid,
    &gEfiHiiConfigRoutingProtocolGuid,&gEfiFormBrowser2ProtocolGuid,&gEdkiiFormDisplayEngineProtocolGuid,
    &gEfiVariableArchProtocolGuid,&gEfiVariableWriteArchProtocolGuid};
  for(UINTN I=0;I<ARRAY_SIZE(Guids);++I){VOID *P=NULL;EFI_STATUS S=gBS->LocateProtocol(Guids[I],NULL,&P);RequireAlive();if(S!=EFI_SUCCESS || (I<6 && !P)){
    *Unavailable=S==EFI_NOT_FOUND || (S==EFI_SUCCESS && I<6 && !P);
    EFI_STATUS Result=S==EFI_SUCCESS?EFI_NOT_READY:Exact(S);
    DEBUG((DEBUG_WARN,"SUNUEFI_SETUP_READY protocol=%g index=%u status=%r unavailable=%u application_loaded=0\n",Guids[I],(UINT32)I,Result,*Unavailable));return Result;
  }}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS SetupFallbackReady(VOID) {
  RequireAlive();CONST PIANO_FV_APPLICATION *C=&mReport.Application;
  if(mReport.Retained || mReport.ServicesLost || mPayloadLoan || C->Retained || C->Busy || C->EbsObserved ||
     C->Image || C->ExitEvent || C->SourceOwned || C->PathOwned || C->HandlesOwned || C->OptionsCopy || C->ExitData || C->OptionsInstalled || C->Cleanup!=EFI_SUCCESS)
    return EFI_COMPROMISED_DATA;
  if(mStartupUsbProof)return StartupUsbAbsent();
  PIANO_DWC3_SERVICE_STATUS Actual;ZeroMem(&Actual,sizeof(Actual));
  EFI_STATUS S=PianoUsbControllerServiceGetStatus(&Actual);RequireAlive();
  if(S!=EFI_SUCCESS)return Exact(S);
  if(Actual.Revision!=1)return EFI_COMPROMISED_DATA;
  mReport.Usb=Actual;
  if(Actual.ServicesLost)return EFI_ABORTED;
  if(Actual.Retained)return EFI_COMPROMISED_DATA;
  return Actual.Started && Actual.Phase==PianoUsbServiceListening && Actual.Action==PianoUsbServiceActionNone?EFI_SUCCESS:EFI_NOT_READY;
}
EFI_STATUS PianoBootPolicyInitialize(EFI_HANDLE Parent) {
  if(!Parent || !gBS || !gBS->LocateProtocol || !gBS->InstallProtocolInterface || !gBS->UninstallProtocolInterface ||
     !gBS->CreateEventEx || !gBS->CreateEvent || !gBS->CloseEvent || !gBS->RegisterProtocolNotify ||
     !gBS->LocateHandleBuffer || !gBS->HandleProtocol || !gBS->FreePool || !gBS->RaiseTPL || !gBS->RestoreTPL)return EFI_INVALID_PARAMETER;
  if(mReport.Initialized)return EFI_ALREADY_STARTED;
  VOID *Existing=NULL;EFI_STATUS S=gBS->LocateProtocol(&mRuntimeGuid,NULL,&Existing);
  if(S!=EFI_NOT_FOUND)return S==EFI_SUCCESS?EFI_ALREADY_STARTED:Exact(S);
  Existing=NULL;S=gBS->LocateProtocol(&mIdleGuid,NULL,&Existing);
  if(S!=EFI_NOT_FOUND)return S==EFI_SUCCESS?EFI_ALREADY_STARTED:Exact(S);
  ZeroMem(&mReport,sizeof(mReport));mReport.Initialized=TRUE;mParent=Parent;mAlive=TRUE;
  mReport.StartupDefaultAction=PIANO_PRODUCT_ACTION_SIMPLEINIT;
  S=AtApp();if(S!=EFI_SUCCESS)return S;
  S=Exact(gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Ebs,NULL,&gEfiEventExitBootServicesGuid,&mExitEvent));RequireAlive();if(S!=EFI_SUCCESS || !mExitEvent){mReport.Retained=TRUE;return S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S;}
  S=Exact(gBS->InstallProtocolInterface(&mProtocolHandle,&mRuntimeGuid,EFI_NATIVE_INTERFACE,&mRuntime));RequireAlive();if(S!=EFI_SUCCESS || !mProtocolHandle){mReport.Retained=TRUE;return S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S;}
  mReport.ProtocolInstalled=TRUE;
  mIdle=(PIANO_PRODUCT_IDLE_PROTOCOL){PIANO_PRODUCT_IDLE_REVISION,&mRuntime,ReadIdle};
  EFI_HANDLE RuntimeHandle=mProtocolHandle;
  S=gBS->InstallProtocolInterface(&mProtocolHandle,&mIdleGuid,EFI_NATIVE_INTERFACE,&mIdle);RequireAlive();
  if(S!=EFI_SUCCESS || mProtocolHandle!=RuntimeHandle){mReport.Retained=TRUE;return S==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(S);}mReport.IdleInstalled=TRUE;
  S=Exact(gBS->CreateEvent(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,KeysChanged,NULL,&mKeyEvent));RequireAlive();if(S!=EFI_SUCCESS || !mKeyEvent){mReport.Retained=TRUE;return S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S;}
  S=Exact(gBS->RegisterProtocolNotify(&gEfiSimpleTextInputExProtocolGuid,mKeyEvent,&mKeyRegistration));RequireAlive();if(S!=EFI_SUCCESS || !mKeyRegistration){mReport.Retained=TRUE;return S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S;}
  mKeysDirty=TRUE;RefreshKeys();return mReport.Retained?EFI_COMPROMISED_DATA:EFI_SUCCESS;
}
EFI_STATUS PianoBootPolicyDispatchPending(VOID) {
  if(!mReport.Initialized || !mReport.ProtocolInstalled)return EFI_NOT_READY;
  if(mReport.Dispatching || mReport.Retained)return EFI_ALREADY_STARTED;
  EFI_STATUS S=AtApp();if(S!=EFI_SUCCESS)return S;
  UINT32 Action;UINT64 Sequence;S=Pending(&mRuntime,&Action,&Sequence);if(S!=EFI_SUCCESS)return S;
  if(Action==PIANO_PRODUCT_ACTION_NONE)return EFI_NOT_FOUND;
  S=Ack(&mRuntime,Sequence);if(S!=EFI_SUCCESS)return S;
  mReport.Dispatching=TRUE;mReport.ActiveAction=Action;
  if(Action==PIANO_PRODUCT_ACTION_RETURN_CORE){
    mReport.LastAction=EFI_END_OF_FILE;mReport.ActiveAction=PIANO_PRODUCT_ACTION_NONE;mReport.Dispatching=FALSE;return EFI_END_OF_FILE;
  }
  BOOLEAN SetupFallback=FALSE;++mReport.AppRuns;
  if(Action==PIANO_PRODUCT_ACTION_SIMPLEINIT) {
    S=Exact(PianoProductAcquireSimpleInit(&mPayload));RequireAlive();mReport.PayloadStatus=S;
    if(S==EFI_SUCCESS){
      mPayloadLoan=TRUE;
      if(!mPayload.Image || !mPayload.Lease || mPayload.Bytes<4096 || mPayload.Bytes>0x4000000)RetainAndHalt();
      S=Exact(PianoProductValidateSimpleInit(&mPayload));RequireAlive();
    }
    if(S==EFI_SUCCESS)S=PianoApplicationRunBuffer(mParent,mPayload.Image,mPayload.Bytes,NULL,0x8000000,&mReport.Application);
    RequireAlive();
    if(mReport.Application.Retained)RetainAndHalt();
    if(mPayloadLoan){EFI_STATUS Release=Exact(PianoProductReleaseSimpleInit(&mPayload));RequireAlive();if(Release!=EFI_SUCCESS){mReport.PayloadStatus=Release;RetainAndHalt();}mPayloadLoan=FALSE;ZeroMem(&mPayload,sizeof(mPayload));}
  } else {
    BOOLEAN Unavailable=FALSE;S=Action==PIANO_PRODUCT_ACTION_SETUP?SetupReady(&Unavailable):EFI_SUCCESS;
    if(Unavailable){
      EFI_STATUS Ready=SetupFallbackReady();
      if(Ready==EFI_SUCCESS)SetupFallback=TRUE;
      else {DEBUG((DEBUG_WARN,"SUNUEFI_SETUP_FALLBACK_DENIED setup_status=%r status=%r\n",S,Ready));S=Ready;}
    }
    if(S==EFI_SUCCESS){BOOLEAN Previous=PianoSetStandardKeyNavigation(TRUE);
      S=PianoFvApplicationRun(mParent,Action==PIANO_PRODUCT_ACTION_SETUP?&mSetupGuid:&mShellGuid,
        Action==PIANO_PRODUCT_ACTION_SHELL?L"Shell.efi -nostartup -nointerrupt":NULL,0x2000000,&mReport.Application);
      RequireAlive();if(!mReport.Application.Retained)PianoSetStandardKeyNavigation(Previous);
    }
    RequireAlive();if(mReport.Application.Retained)RetainAndHalt();
  }
  mReport.LastAction=S;mReport.ActiveAction=PIANO_PRODUCT_ACTION_NONE;mReport.Dispatching=FALSE;
  if(SetupFallback){
    // Setup was ACKed before readiness, and no loader ran. Queue the menu once
    // while preserving any newer UI/core request; never retire shared USB here.
    UINT64 Mask=Critical();EFI_STATUS Queued=mReport.PendingAction==PIANO_PRODUCT_ACTION_NONE?Request(&mRuntime,PIANO_PRODUCT_ACTION_SIMPLEINIT):EFI_SUCCESS;EndCritical(Mask);
    DEBUG((DEBUG_WARN,"SUNUEFI_SETUP_UNAVAILABLE status=%r menu_queued=%u queue_status=%r resident_usb=%u\n",S,mReport.PendingAction==PIANO_PRODUCT_ACTION_SIMPLEINIT,Queued,mReport.Usb.Started));return Queued;
  }
  return S;
}
EFI_STATUS PianoBootPolicyRun(VOID) {
  if(!mReport.Initialized)return EFI_NOT_READY;
  RequireAlive();
  UINT32 Initial;UINT64 Sequence;
  EFI_STATUS S=Pending(&mRuntime,&Initial,&Sequence);if(S!=EFI_SUCCESS)return S;
  if(Initial==PIANO_PRODUCT_ACTION_NONE)S=Request(&mRuntime,PIANO_PRODUCT_ACTION_SIMPLEINIT);
  if(S!=EFI_SUCCESS)return S;
  for(;;) {
    UINT32 Action;UINT64 Sequence;EFI_STATUS Get=Pending(&mRuntime,&Action,&Sequence);RequireAlive();if(Get!=EFI_SUCCESS)return Get;
    if(Action==PIANO_PRODUCT_ACTION_NONE)return S;
    S=PianoBootPolicyDispatchPending();
    if(S!=EFI_SUCCESS){
      // A console/form wait may return EFI_ABORTED to unwind a child through
      // its normal cleanup. Preserve the real app result, but still deliver
      // the unacknowledged core-return after the loader retired the image.
      UINT32 Next;UINT64 NextSequence;EFI_STATUS Peek=Pending(&mRuntime,&Next,&NextSequence);RequireAlive();
      if(S!=EFI_END_OF_FILE && Peek==EFI_SUCCESS && Next==PIANO_PRODUCT_ACTION_RETURN_CORE)
        return PianoBootPolicyDispatchPending();
      return S;
    }
    // UI navigation does not stop USB. Give its copied response queue an APP
    // slice between children, preserving the same DMA/service owner instance.
    EFI_STATUS Service=Pump(&mRuntime,PIANO_PRODUCT_PUMP_APP,1000);
    if(Service!=EFI_SUCCESS && Service!=EFI_NOT_READY && Service!=EFI_ABORTED)return Service;
    if(Action==PIANO_PRODUCT_ACTION_SETUP || Action==PIANO_PRODUCT_ACTION_SHELL) {
      Get=Pending(&mRuntime,&Action,&Sequence);RequireAlive();if(Get!=EFI_SUCCESS)return Get;
      if(Action==PIANO_PRODUCT_ACTION_NONE){Get=Request(&mRuntime,PIANO_PRODUCT_ACTION_SIMPLEINIT);if(Get!=EFI_SUCCESS)return Get;}
    }
  }
}
EFI_STATUS PianoBootPolicyCancelStable(VOID) {
  EFI_STATUS S=AtApp();if(S!=EFI_SUCCESS)return S;
  if(mReport.Retained||mReport.Pumping||mReport.Dispatching||mReport.ActiveAction!=PIANO_PRODUCT_ACTION_NONE||
     mReport.RequestedCoreAction!=PianoUsbServiceActionBoot||mReport.PendingAction!=PIANO_PRODUCT_ACTION_NONE)return EFI_ACCESS_DENIED;
  mReport.RequestedCoreAction=PianoUsbServiceActionNone;
  return Request(&mRuntime,PIANO_PRODUCT_ACTION_SIMPLEINIT);
}
EFI_STATUS PianoBootPolicyStop(VOID) {
  if(!mReport.Initialized)return EFI_NOT_STARTED;
  if(mStartupWindow || mReport.Dispatching || mReport.Pumping || mReport.Retained)return EFI_ACCESS_DENIED;
  EFI_STATUS S=AtApp();if(S!=EFI_SUCCESS)return S;
  while(mKeysCount){UINTN I=mKeysCount-1;EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *Fresh=NULL;
    S=gBS->HandleProtocol(mKeys[I].Handle,&gEfiSimpleTextInputExProtocolGuid,(VOID **)&Fresh);RequireAlive();
    if(S==EFI_NOT_FOUND || S==EFI_UNSUPPORTED || S==EFI_INVALID_PARAMETER || (S==EFI_SUCCESS && (Fresh!=mKeys[I].Input || !Fresh || Fresh->UnregisterKeyNotify!=mKeys[I].Unregister))){--mKeysCount;continue;}
    if(S!=EFI_SUCCESS){mReport.Retained=TRUE;return Exact(S);}
    for(UINTN K=4;K>0;--K){if(mKeys[I].Notify[K-1]){S=Fresh->UnregisterKeyNotify(Fresh,mKeys[I].Notify[K-1]);RequireAlive();if(S!=EFI_SUCCESS){mReport.Retained=TRUE;return Exact(S);}mKeys[I].Notify[K-1]=NULL;}}
    --mKeysCount;}
  if(mKeyEvent){S=gBS->CloseEvent(mKeyEvent);RequireAlive();if(S!=EFI_SUCCESS){mReport.Retained=TRUE;return Exact(S);}mKeyEvent=NULL;}
  mReport.IdleKnown=FALSE;
  if(mReport.IdleInstalled){S=gBS->UninstallProtocolInterface(mProtocolHandle,&mIdleGuid,&mIdle);RequireAlive();if(S!=EFI_SUCCESS){mReport.Retained=TRUE;return Exact(S);}mReport.IdleInstalled=FALSE;}
  if(mReport.ProtocolInstalled){S=gBS->UninstallProtocolInterface(mProtocolHandle,&mRuntimeGuid,&mRuntime);RequireAlive();if(S!=EFI_SUCCESS){mReport.Retained=TRUE;return Exact(S);}mReport.ProtocolInstalled=FALSE;}
  if(mExitEvent){S=gBS->CloseEvent(mExitEvent);RequireAlive();if(S!=EFI_SUCCESS){mReport.Retained=TRUE;return Exact(S);}mExitEvent=NULL;}
  mAlive=FALSE;return EFI_SUCCESS;
}
CONST PIANO_BOOT_POLICY_REPORT *PianoBootPolicyReport(VOID){return &mReport;}
