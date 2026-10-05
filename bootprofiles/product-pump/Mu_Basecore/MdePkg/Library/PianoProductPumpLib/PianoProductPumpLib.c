// SPDX-License-Identifier: BSD-2-Clause-Patent
// Product-only protocol client. Constructors cannot create events before DXE
// Event Services initialize: create the local EBS fence lazily at APP TPL.
#include <Library/PianoProductPumpLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Guid/EventGroup.h>
STATIC EFI_GUID mGuid=PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID;
STATIC PIANO_PRODUCT_RUNTIME_PROTOCOL *mRuntime;
STATIC EFI_GUID mIdleGuid=PIANO_PRODUCT_IDLE_PROTOCOL_GUID;
STATIC PIANO_PRODUCT_IDLE_PROTOCOL *mIdle;
STATIC EFI_STATUS (EFIAPI *mIdleRead)(PIANO_PRODUCT_IDLE_PROTOCOL *,CONST PIANO_PRODUCT_RUNTIME_PROTOCOL *,UINT64 *,BOOLEAN *);
STATIC UINT64 mIdleSample;
STATIC BOOLEAN mCanIdle;
STATIC EFI_EVENT mFence;
STATIC volatile BOOLEAN mExited;
STATIC BOOLEAN mBusy,mFenceUnknown;
STATIC EFI_STATUS Exact(EFI_STATUS Status) { return Status==EFI_SUCCESS?Status:EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR; }
STATIC VOID EFIAPI ExitNotify(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;(VOID)Context;mExited=TRUE;mRuntime=NULL;mIdle=NULL;mCanIdle=FALSE; // CPU flags only
}
BOOLEAN EFIAPI PianoProductPumpBootServicesAlive(VOID) {
  return !mExited && !mFenceUnknown && gST!=NULL && gBS!=NULL &&
    gST->BootServices==gBS && gBS->Hdr.Signature==EFI_BOOT_SERVICES_SIGNATURE;
}
STATIC EFI_STATUS Enter(VOID) {
  if(mBusy || !PianoProductPumpBootServicesAlive())return EFI_NOT_READY;
  if(gBS->RaiseTPL==NULL || gBS->RestoreTPL==NULL || gBS->CreateEventEx==NULL ||
     gBS->LocateProtocol==NULL)return EFI_UNSUPPORTED;
  mBusy=TRUE;
  EFI_TPL Current=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Current);
  if(!PianoProductPumpBootServicesAlive()){mBusy=FALSE;return EFI_ABORTED;}
  if(Current!=TPL_APPLICATION){mBusy=FALSE;return EFI_UNSUPPORTED;}
  if(mFence==NULL) {
    EFI_STATUS Status=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ExitNotify,NULL,&gEfiEventExitBootServicesGuid,&mFence);
    if(!PianoProductPumpBootServicesAlive()){mBusy=FALSE;return EFI_ABORTED;}
    if(Status!=EFI_SUCCESS || mFence==NULL) {mFenceUnknown=TRUE;mRuntime=NULL;mBusy=FALSE;return Status==EFI_SUCCESS?EFI_COMPROMISED_DATA:Exact(Status);}
  }
  if(mRuntime==NULL) {
    PIANO_PRODUCT_RUNTIME_PROTOCOL *Found=NULL;
    EFI_STATUS Status=gBS->LocateProtocol(&mGuid,NULL,(VOID **)&Found);
    if(!PianoProductPumpBootServicesAlive()){mBusy=FALSE;return EFI_ABORTED;}
    if(Status!=EFI_SUCCESS){mBusy=FALSE;return Exact(Status);}
    if(Found==NULL || Found->Revision!=PIANO_PRODUCT_RUNTIME_REVISION || Found->Pump==NULL || Found->BootServicesAlive==NULL ||
       Found->RequestAction==NULL || Found->GetPendingAction==NULL || Found->AckAction==NULL){mBusy=FALSE;return EFI_COMPROMISED_DATA;}
    mRuntime=Found;
  }
  BOOLEAN Alive=mRuntime->BootServicesAlive(mRuntime);
  if(!PianoProductPumpBootServicesAlive()){mBusy=FALSE;return EFI_ABORTED;}
  if(Alive!=TRUE){mBusy=FALSE;return EFI_NOT_READY;}
  return EFI_SUCCESS;
}
BOOLEAN EFIAPI PianoProductPumpShouldIdle(VOID) {
  BOOLEAN Result=mCanIdle && !mBusy && PianoProductPumpBootServicesAlive();
  mCanIdle=FALSE; // one use, only immediately after a fresh completed APP pump
  return Result;
}
STATIC VOID ReadIdleSnapshot(VOID){
  PIANO_PRODUCT_RUNTIME_PROTOCOL *Fresh=NULL;
  EFI_STATUS S=gBS->LocateProtocol(&mGuid,NULL,(VOID **)&Fresh);
  if(!PianoProductPumpBootServicesAlive() || S!=EFI_SUCCESS || Fresh!=mRuntime)return;
  PIANO_PRODUCT_IDLE_PROTOCOL *Hint=NULL;
  S=gBS->LocateProtocol(&mIdleGuid,NULL,(VOID **)&Hint);
  if(!PianoProductPumpBootServicesAlive() || S!=EFI_SUCCESS || !Hint || Hint->Revision!=PIANO_PRODUCT_IDLE_REVISION ||
     Hint->Runtime!=mRuntime || !Hint->Read || (mIdle && (Hint!=mIdle || Hint->Read!=mIdleRead)))return;
  UINT64 Sample=0;BOOLEAN Active=TRUE;
  EFI_STATUS (EFIAPI *Read)(PIANO_PRODUCT_IDLE_PROTOCOL *,CONST PIANO_PRODUCT_RUNTIME_PROTOCOL *,UINT64 *,BOOLEAN *)=Hint->Read;
  S=Read(Hint,mRuntime,&Sample,&Active);
  if(!PianoProductPumpBootServicesAlive() || S!=EFI_SUCCESS || Hint->Revision!=PIANO_PRODUCT_IDLE_REVISION || Hint->Runtime!=mRuntime || Hint->Read!=Read ||
     !Sample || Sample<=mIdleSample || (Active!=TRUE && Active!=FALSE))return;
  mIdle=Hint;mIdleRead=Hint->Read;mIdleSample=Sample;mCanIdle=!Active;
}
EFI_STATUS EFIAPI PianoProductPumpApplication(UINT32 Reason,UINTN BudgetUs) {
  if(!(Reason&(PIANO_PRODUCT_PUMP_WAIT_EVENT|PIANO_PRODUCT_PUMP_GUI|PIANO_PRODUCT_PUMP_APP)) ||
     (Reason&~(PIANO_PRODUCT_PUMP_WAIT_EVENT|PIANO_PRODUCT_PUMP_GUI|PIANO_PRODUCT_PUMP_APP)) || !BudgetUs)return EFI_INVALID_PARAMETER;
  mCanIdle=FALSE;EFI_STATUS Status=Enter();if(Status!=EFI_SUCCESS)return Status;
  Status=mRuntime->Pump(mRuntime,Reason,BudgetUs);
  if(!PianoProductPumpBootServicesAlive())Status=EFI_ABORTED;
  else if(Status==EFI_SUCCESS || Status==EFI_NOT_READY)ReadIdleSnapshot();
  mBusy=FALSE;return Exact(Status);
}
EFI_STATUS EFIAPI PianoProductGetPendingAction(UINT32 *Action,UINT64 *Sequence) {
  if(Action==NULL || Sequence==NULL)return EFI_INVALID_PARAMETER;
  *Action=PIANO_PRODUCT_ACTION_NONE;*Sequence=0;
  EFI_STATUS Status=Enter();if(Status!=EFI_SUCCESS)return Status;
  UINT32 Pending=PIANO_PRODUCT_ACTION_NONE;UINT64 Seq=0;
  Status=mRuntime->GetPendingAction(mRuntime,&Pending,&Seq);
  if(!PianoProductPumpBootServicesAlive())Status=EFI_ABORTED;
  if(Status==EFI_SUCCESS && (Pending>PIANO_PRODUCT_ACTION_RETURN_CORE || (Pending!=PIANO_PRODUCT_ACTION_NONE && Seq==0)))Status=EFI_COMPROMISED_DATA;
  if(Status==EFI_SUCCESS){*Action=Pending;*Sequence=Seq;}
  mBusy=FALSE;return Exact(Status);
}
BOOLEAN EFIAPI PianoProductReturnCoreRequested(VOID) {
  UINT32 Action=PIANO_PRODUCT_ACTION_NONE;UINT64 Sequence=0;
  EFI_STATUS Status=PianoProductGetPendingAction(&Action,&Sequence);
  // UI callers may use this as their lifetime checkpoint before ordinary
  // cleanup. Lost services must never look like a harmless "no request".
  if(!PianoProductPumpBootServicesAlive()) {
#ifdef __aarch64__
    __asm__ volatile("msr daifset, #15":::"memory");
#endif
    CpuDeadLoop();return FALSE;
  }
  return Status==EFI_SUCCESS && Action==PIANO_PRODUCT_ACTION_RETURN_CORE;
}
BOOLEAN EFIAPI PianoProductUiReturnRequested(VOID) {
  UINT32 Action=PIANO_PRODUCT_ACTION_NONE;UINT64 Sequence=0;
  EFI_STATUS Status=PianoProductGetPendingAction(&Action,&Sequence);
  if(!PianoProductPumpBootServicesAlive()){
#ifdef __aarch64__
    __asm__ volatile("msr daifset, #15" ::: "memory");
#endif
    CpuDeadLoop();return FALSE;
  }
  return Status==EFI_SUCCESS && Action!=PIANO_PRODUCT_ACTION_NONE;
}
EFI_STATUS EFIAPI PianoProductRequestNavigation(UINT32 Action) {
  if(Action<PIANO_PRODUCT_ACTION_SIMPLEINIT || Action>PIANO_PRODUCT_ACTION_SHELL)return EFI_INVALID_PARAMETER;
  EFI_STATUS Status=Enter();
  if(!PianoProductPumpBootServicesAlive()){
#ifdef __aarch64__
    __asm__ volatile("msr daifset, #15" ::: "memory");
#endif
    CpuDeadLoop();return EFI_ABORTED;
  }
  if(Status!=EFI_SUCCESS)return Status==EFI_UNSUPPORTED?EFI_ACCESS_DENIED:Status;
  Status=mRuntime->RequestAction(mRuntime,Action);mBusy=FALSE;
  if(!PianoProductPumpBootServicesAlive()){
#ifdef __aarch64__
    __asm__ volatile("msr daifset, #15" ::: "memory");
#endif
    CpuDeadLoop();return EFI_ABORTED;
  }
  return Status==EFI_UNSUPPORTED?EFI_ACCESS_DENIED:Exact(Status);
}
EFI_STATUS EFIAPI PianoProductRequestReboot(VOID) {
  EFI_STATUS Status=Enter();
  if(!PianoProductPumpBootServicesAlive()){
#ifdef __aarch64__
    __asm__ volatile("msr daifset, #15":::"memory");
#endif
    CpuDeadLoop();return EFI_ABORTED;
  }
  if(Status!=EFI_SUCCESS)return Status==EFI_UNSUPPORTED?EFI_ACCESS_DENIED:Status;
  Status=mRuntime->RequestAction(mRuntime,PIANO_PRODUCT_ACTION_REQUEST_REBOOT);
  mBusy=FALSE;
  if(!PianoProductPumpBootServicesAlive()) {
#ifdef __aarch64__
    __asm__ volatile("msr daifset, #15":::"memory");
#endif
    CpuDeadLoop();return EFI_ABORTED;
  }
  // EFI_UNSUPPORTED is reserved for the Null implementation's legacy fallback.
  return Status==EFI_UNSUPPORTED?EFI_ACCESS_DENIED:Exact(Status);
}
EFI_STATUS EFIAPI PianoProductRequestContinue(VOID) {
  EFI_STATUS Status=Enter();
  if(!PianoProductPumpBootServicesAlive()){
#ifdef __aarch64__
    __asm__ volatile("msr daifset, #15":::"memory");
#endif
    CpuDeadLoop();return EFI_ABORTED;
  }
  if(Status!=EFI_SUCCESS)return Status==EFI_UNSUPPORTED?EFI_ACCESS_DENIED:Status;
  Status=mRuntime->RequestAction(mRuntime,PIANO_PRODUCT_ACTION_REQUEST_CONTINUE);
  mBusy=FALSE;
  if(!PianoProductPumpBootServicesAlive()) {
#ifdef __aarch64__
    __asm__ volatile("msr daifset, #15":::"memory");
#endif
    CpuDeadLoop();return EFI_ABORTED;
  }
  // EFI_UNSUPPORTED is reserved for the Null implementation's legacy fallback.
  return Status==EFI_UNSUPPORTED?EFI_ACCESS_DENIED:Exact(Status);
}
BOOLEAN EFIAPI PianoProductRebootManaged(VOID){return TRUE;}
EFI_STATUS EFIAPI PianoProductPumpLibDestructor(EFI_HANDLE Image,EFI_SYSTEM_TABLE *SystemTable) {
  (VOID)Image;(VOID)SystemTable;
  // Never leave a callback pointing into an unloaded GUI/app module. If the
  // local callback fired, no BS is called and the parent must not unload.
  if(mExited)return EFI_ABORTED;
  if(mFenceUnknown && mFence==NULL){CpuDeadLoop();return EFI_ABORTED;}
  if(mFence!=NULL) {
    if(gST==NULL || gBS==NULL || gST->BootServices!=gBS || gBS->CloseEvent==NULL){CpuDeadLoop();return EFI_ABORTED;}
    EFI_STATUS Status=gBS->CloseEvent(mFence);
    if(mExited)return EFI_ABORTED;
    if(Status!=EFI_SUCCESS){CpuDeadLoop();return Exact(Status);}
    mFence=NULL;
  }
  mRuntime=NULL;mIdle=NULL;mIdleSample=0;mCanIdle=FALSE;return EFI_SUCCESS;
}
