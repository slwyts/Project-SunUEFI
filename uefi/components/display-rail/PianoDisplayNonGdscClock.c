// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoDisplayNonGdscClock.h"
#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
#define NGC_SIGNATURE SIGNATURE_32('P','N','G','C')
#define NGC_ID 0x02010006U
STATIC EFI_STATUS Exact(EFI_STATUS E){return E==EFI_SUCCESS?E:EFI_ERROR(E)?E:EFI_DEVICE_ERROR;}
STATIC BOOLEAN Alias(CONST VOID *A,UINTN N,CONST VOID *B,UINTN Z){UINTN X=(UINTN)A,Y=(UINTN)B;return N>MAX_UINTN-X||Z>MAX_UINTN-Y||(X<Y+Z&&Y<X+N);}
STATIC BOOLEAN Live(PIANO_NON_GDSC_CLOCK *S){
 if(S->Report.ServicesLost||S->Env.Gcc->Env.BootServicesAlive(S->Env.Gcc->Env.Context)!=TRUE||S->Env.Reader->Env.BootServicesAlive(S->Env.Reader->Env.Context)!=TRUE||S->Env.Rail->Env.Alive(S->Env.Rail->Env.Context)!=TRUE){S->Report.ServicesLost=S->Report.Retained=TRUE;return FALSE;}return TRUE;
}
STATIC EFI_STATUS Retain(PIANO_NON_GDSC_CLOCK *S,EFI_STATUS E){S->Report.Retained=TRUE;S->Report.Busy=FALSE;return S->Report.Status=Exact(E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);}
STATIC VOID EFIAPI Exit(EFI_EVENT Event,VOID *Context){(VOID)Event;PIANO_NON_GDSC_CLOCK *S=Context;S->Report.ServicesLost=S->Report.Retained=TRUE;}
STATIC EFI_STATUS App(PIANO_NON_GDSC_CLOCK *S){
 if(!Live(S))return EFI_ABORTED;EFI_TPL T=S->Env.Gcc->Env.Services->RaiseTPL(TPL_HIGH_LEVEL);if(!Live(S))return EFI_ABORTED;S->Env.Gcc->Env.Services->RestoreTPL(T);return !Live(S)?EFI_ABORTED:T==TPL_APPLICATION?EFI_SUCCESS:EFI_UNSUPPORTED;
}
STATIC EFI_STATUS Idle(PIANO_NON_GDSC_CLOCK *S){
 CONST PIANO_GUARDED_REPORT *G=PianoGuardedReadReport();if(!Live(S))return EFI_ABORTED;
 return !G||G->Active||G->SyncOwned||G->SErrorOwned||G->Fatal||G->Retained||G->ServicesLost||S->Env.Reader->Report.Busy||S->Env.Reader->Report.Retained||S->Env.Rail->Report.Busy||PianoDisplayRailRetained(S->Env.Rail)?EFI_NOT_READY:EFI_SUCCESS;
}
STATIC EFI_STATUS FreshParent(PIANO_NON_GDSC_CLOCK *S){
 EFI_STATUS E=Idle(S);if(E!=EFI_SUCCESS)return E;E=PianoDisplayClockLeaseValidateHeld(S->Env.Gcc,S->Report.TransactionToken,&S->Report.ParentAfter);if(!Live(S))return EFI_ABORTED;if(E!=EFI_SUCCESS)return E;
 PIANO_DISPLAY_CLOCK_LEASE_HELD_PROOF *R=&S->Report.ParentAfter;
 if(R->LeaseContext!=S->Env.Gcc||R->Token!=S->Report.TransactionToken||R->NativeBase!=S->Report.NativeBase||R->OwnedReferences!=1||R->Status!=EFI_SUCCESS||S->Env.Gcc->Clock!=S->Clock)return EFI_COMPROMISED_DATA;return Idle(S);
}
STATIC EFI_STATUS Borrow(PIANO_NON_GDSC_CLOCK *S){
 EFI_STATUS E=Idle(S);if(E!=EFI_SUCCESS)return E;S->Report.Borrow=E=PianoDisplayClockLeaseBorrowHeld(S->Env.Gcc,&S->Report.ParentBefore);if(!Live(S))return EFI_ABORTED;if(E!=EFI_SUCCESS)return E;
 S->Report.TransactionToken=S->Report.ParentBefore.Token;if(!S->Report.TransactionToken||S->Report.ParentBefore.LeaseContext!=S->Env.Gcc||S->Report.ParentBefore.Status!=EFI_SUCCESS||S->Report.ParentBefore.OwnedReferences!=1)return EFI_COMPROMISED_DATA;
 if(S->Report.NativeBase&&S->Report.NativeBase!=S->Report.ParentBefore.NativeBase)return EFI_MEDIA_CHANGED;
 S->Report.NativeBase=S->Report.ParentBefore.NativeBase;S->Clock=S->Env.Gcc->Clock;return FreshParent(S);
}
STATIC EFI_STATUS Return(PIANO_NON_GDSC_CLOCK *S){
 EFI_STATUS E=Idle(S);if(E!=EFI_SUCCESS)return E;S->Report.Return=E=PianoDisplayClockLeaseReturnHeld(S->Env.Gcc,S->Report.TransactionToken,&S->Report.ParentAfter);if(!Live(S))return EFI_ABORTED;if(E==EFI_SUCCESS)S->Report.TransactionToken=0;return E;
}
STATIC EFI_STATUS Snapshot(PIANO_NON_GDSC_CLOCK *S,PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT *R){
 EFI_STATUS E=Idle(S);if(E!=EFI_SUCCESS)return E;E=PianoDisplayClockReadSnapshotClock(S->Env.Reader,PianoClockSelectNonGdscAhb,R);if(!Live(S))return EFI_ABORTED;if(E!=EFI_SUCCESS)return E;
 if(R->Status!=EFI_SUCCESS||R->Identity!=EFI_SUCCESS||R->MatchingSnapshots!=2||R->ReaderContext!=S->Env.Reader||R->LeaseContext!=S->Env.Gcc||R->NativeBase!=S->Report.NativeBase||R->NativeImage!=S->Env.Gcc->NativeImage||R->ExpectedClockId!=NGC_ID||
   R->ParentRailMask!=8||(R->GlobalFlags&(BIT8|BIT11))||(R->NodeFlags&(BIT8|BIT9|BIT14))||(R->ParentFlags&(BIT9|BIT10))){return EFI_UNSUPPORTED;}
 return Idle(S);
}
STATIC BOOLEAN SameSource(CONST PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT *A,CONST PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT *B){
 return A->NativeBase==B->NativeBase&&A->NativeImage==B->NativeImage&&A->Global==B->Global&&A->Client==B->Client&&A->Node==B->Node&&A->Parent==B->Parent&&A->Name==B->Name&&A->Module==B->Module&&A->Array==B->Array&&A->ClientRef==B->ClientRef&&A->ClientRefPresent==B->ClientRefPresent&&A->GlobalFlags==B->GlobalFlags&&A->NodeFlags==B->NodeFlags&&A->ParentFlags==B->ParentFlags&&A->ParentRailMask==B->ParentRailMask&&A->ClientFlags==B->ClientFlags&&A->MmClient==B->MmClient;
}
STATIC EFI_STATUS Rail(PIANO_NON_GDSC_CLOCK *S,CONST CHAR8 *Phase,PIANO_DISPLAY_RAIL_SNAPSHOT *R){
 EFI_STATUS E=Idle(S);if(E!=EFI_SUCCESS)return E;CONST PIANO_DISPLAY_RAIL_REPORT *Before=PianoDisplayRailReport(S->Env.Rail);if(!Before||!Before->Initialized||Before->Count>=PIANO_DISPLAY_RAIL_OBSERVE_PHASES)return EFI_NOT_READY;UINT32 Count=Before->Count;
 E=PianoDisplayRailObserve(S->Env.Rail,Phase);if(!Live(S))return EFI_ABORTED;CONST PIANO_DISPLAY_RAIL_REPORT *After=PianoDisplayRailReport(S->Env.Rail);if(!After||After->Count!=Count+1)return EFI_COMPROMISED_DATA;CopyMem(R,&After->Snapshot[Count],sizeof(*R));
 if(E!=EFI_SUCCESS)return E;if(R->Status!=EFI_SUCCESS||R->Retained||R->ServicesLost||!R->MmCoherent||R->Mm[0].Status!=EFI_SUCCESS||R->Mm[1].Status!=EFI_SUCCESS||R->ClockBefore.NativeBase!=S->Report.NativeBase||R->ClockBefore.ReaderContext!=S->Env.Reader||R->ClockBefore.LeaseContext!=S->Env.Gcc||R->ClockBefore.ExpectedClockId!=NGC_ID||R->ClockAfter.MmClient!=R->ClockBefore.MmClient||R->Mm[0].Client!=R->ClockBefore.MmClient)return EFI_COMPROMISED_DATA;
 // MX and completion are recorded by the real observer, never independent
 // permission gates or a numeric cached-corner->physical-power assertion.
 return Idle(S);
}
STATIC EFI_STATUS Get(PIANO_NON_GDSC_CLOCK *S,UINTN *Id){
#ifdef PIANO_NON_GDSC_CLOCK_HOST_TEST
 extern EFI_STATUS PianoNonGdscHostGet(EFI_CLOCK_PROTOCOL *,CONST CHAR8 *,UINTN *);return PianoNonGdscHostGet(S->Clock,"disp_cc_mdss_non_gdsc_ahb_clk",Id);
#else
 return S->Clock->GetClockID(S->Clock,"disp_cc_mdss_non_gdsc_ahb_clk",Id);
#endif
}
STATIC EFI_STATUS Change(PIANO_NON_GDSC_CLOCK *S,BOOLEAN Enable){
#ifdef PIANO_NON_GDSC_CLOCK_HOST_TEST
 extern EFI_STATUS PianoNonGdscHostChange(EFI_CLOCK_PROTOCOL *,UINTN,BOOLEAN);return PianoNonGdscHostChange(S->Clock,S->Report.ClockId,Enable);
#else
 return Enable?S->Clock->EnableClock(S->Clock,S->Report.ClockId):S->Clock->DisableClock(S->Clock,S->Report.ClockId);
#endif
}
STATIC EFI_STATUS Enabled(PIANO_NON_GDSC_CLOCK *S,BOOLEAN *Value,BOOLEAN On){
#ifdef PIANO_NON_GDSC_CLOCK_HOST_TEST
 extern EFI_STATUS PianoNonGdscHostQuery(EFI_CLOCK_PROTOCOL *,UINTN,BOOLEAN *,BOOLEAN);return PianoNonGdscHostQuery(S->Clock,S->Report.ClockId,Value,On);
#else
 return On?S->Clock->IsClockOn(S->Clock,S->Report.ClockId,Value):S->Clock->IsClockEnabled(S->Clock,S->Report.ClockId,Value);
#endif
}
EFI_STATUS PianoDisplayNonGdscClockAcquire(PIANO_NON_GDSC_CLOCK *S,CONST PIANO_NON_GDSC_CLOCK_ENV *Env){
 if(!S||!Env||S->Signature||Alias(S,sizeof(*S),Env,sizeof(*Env))||!Env->Gcc||!Env->Reader||!Env->Rail||Alias(S,sizeof(*S),Env->Gcc,sizeof(*Env->Gcc))||Alias(S,sizeof(*S),Env->Reader,sizeof(*Env->Reader))||Alias(S,sizeof(*S),Env->Rail,sizeof(*Env->Rail)))return EFI_INVALID_PARAMETER;
 if(Env->Reader->Env.Lease!=Env->Gcc||Env->Rail->Env.ClockReader!=Env->Reader||Env->Reader->Env.Services!=Env->Gcc->Env.Services||Env->Rail->Env.Services!=Env->Gcc->Env.Services||!Env->Gcc->Env.Services||!Env->Gcc->Env.BootServicesAlive||!Env->Reader->Env.BootServicesAlive||!Env->Rail->Env.Alive||!Env->Gcc->Env.Services->RaiseTPL||!Env->Gcc->Env.Services->RestoreTPL||!Env->Gcc->Env.Services->CreateEventEx||!Env->Gcc->Env.Services->CloseEvent)return EFI_UNSUPPORTED;
 S->Signature=NGC_SIGNATURE;S->Env=*Env;S->Report.Revision=PIANO_NON_GDSC_CLOCK_REVISION;S->Report.ClockId=MAX_UINTN;
 S->Report.Status=S->Report.Borrow=S->Report.Create=S->Report.GetId=S->Report.Before=S->Report.Enable=S->Report.Counter=S->Report.IsEnabled=S->Report.IsOn=S->Report.After=S->Report.Return=S->Report.Disable=S->Report.Close=EFI_NOT_STARTED;
 S->Report.Busy=TRUE;EFI_STATUS E=App(S);if(E!=EFI_SUCCESS)return Retain(S,E);E=Borrow(S);if(E!=EFI_SUCCESS)return Retain(S,E);
 S->Report.Create=E=S->Env.Gcc->Env.Services->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,S,&gEfiEventExitBootServicesGuid,&S->Exit);if(!Live(S)||E!=EFI_SUCCESS||!S->Exit)return Retain(S,!Live(S)?EFI_ABORTED:E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);
 S->Report.Before=E=Rail(S,"non-gdsc-before",&S->Report.RailBefore);if(E!=EFI_SUCCESS)return Retain(S,E);E=FreshParent(S);if(E!=EFI_SUCCESS)return Retain(S,E);
 UINTN Id=MAX_UINTN;S->Report.GetAttempted=TRUE;S->Report.GetId=E=Get(S,&Id);if(!Live(S)||E!=EFI_SUCCESS||Id!=NGC_ID)return Retain(S,!Live(S)?EFI_ABORTED:E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);S->Report.ClockId=Id;
 S->Report.Before=E=Snapshot(S,&S->Report.Baseline);if(E!=EFI_SUCCESS)return Retain(S,E);PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT *B=&S->Report.Baseline;
 if(!B->ClientRefPresent||B->Total[0]==MAX_UINT16||B->PerClient[0]==MAX_UINT16||(!B->Total[0]&&B->ParentRefs[0]==MAX_UINT16))return Retain(S,EFI_OUT_OF_RESOURCES);
 E=FreshParent(S);if(E!=EFI_SUCCESS)return Retain(S,E);S->Report.EnableAttempted=TRUE;S->Report.Enable=E=Change(S,TRUE);if(!Live(S)||E!=EFI_SUCCESS)return Retain(S,!Live(S)?EFI_ABORTED:E);S->Report.Held=TRUE;
 S->Report.Counter=E=Snapshot(S,&S->Report.Acquired);if(E!=EFI_SUCCESS)return Retain(S,E);PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT *A=&S->Report.Acquired;
 if(!SameSource(B,A)||A->Total[0]!=B->Total[0]+1||A->PerClient[0]!=B->PerClient[0]+1||A->Total[1]!=B->Total[1]||A->PerClient[1]!=B->PerClient[1]||A->ParentRefs[0]!=B->ParentRefs[0]+(!B->Total[0])||A->ParentRefs[1]!=B->ParentRefs[1])return Retain(S,EFI_COMPROMISED_DATA);S->Report.OwnedReferences=1;
 E=FreshParent(S);if(E!=EFI_SUCCESS)return Retain(S,E);BOOLEAN V=0xA5;S->Report.IsEnabled=E=Enabled(S,&V,FALSE);S->Report.EnabledObserved=V;if(!Live(S)||E!=EFI_SUCCESS||(V!=TRUE&&V!=FALSE))return Retain(S,!Live(S)?EFI_ABORTED:E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);
 if(V==TRUE){E=FreshParent(S);if(E!=EFI_SUCCESS)return Retain(S,E);V=0xA5;S->Report.IsOn=E=Enabled(S,&V,TRUE);S->Report.OnObserved=V;if(!Live(S)||E!=EFI_SUCCESS||(V!=TRUE&&V!=FALSE))return Retain(S,!Live(S)?EFI_ABORTED:E==EFI_SUCCESS?EFI_COMPROMISED_DATA:E);}
 S->Report.After=E=Rail(S,"non-gdsc-acquired",&S->Report.RailAfter);if(E!=EFI_SUCCESS)return Retain(S,E);E=Return(S);if(E!=EFI_SUCCESS)return Retain(S,E);
 S->Report.Busy=FALSE;
 if(S->Report.EnabledObserved==FALSE){
  // A known FALSE readback is not hardware success. Preserve the post-enable
  // rail state, then undo only our verified reference through normal release.
  E=PianoDisplayNonGdscClockRelease(S);if(E!=EFI_SUCCESS)return E;
  return S->Report.Status=EFI_NOT_READY;
 }
 return S->Report.Status=EFI_SUCCESS;
}
EFI_STATUS PianoDisplayNonGdscClockRelease(PIANO_NON_GDSC_CLOCK *S){
 if(!S||S->Signature!=NGC_SIGNATURE)return EFI_INVALID_PARAMETER;if(S->Report.Busy||S->Report.Retained||S->Report.ServicesLost||!S->Report.Held||S->Report.Released||S->Report.DisableAttempted||S->Report.OwnedReferences!=1||S->Report.TransactionToken||!S->Exit)return EFI_ACCESS_DENIED;
 S->Report.Busy=TRUE;EFI_STATUS E=App(S);if(E!=EFI_SUCCESS)return Retain(S,E);E=Borrow(S);if(E!=EFI_SUCCESS)return Retain(S,E);S->Report.Before=E=Rail(S,"non-gdsc-release",&S->Report.RailReleaseBefore);if(E!=EFI_SUCCESS)return Retain(S,E);
 E=Snapshot(S,&S->Report.ReleaseBefore);if(E!=EFI_SUCCESS)return Retain(S,E);PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT *B=&S->Report.ReleaseBefore;
 if(!SameSource(B,&S->Report.Acquired)||!B->Total[0]||!B->PerClient[0]||!B->ParentRefs[0])return Retain(S,EFI_COMPROMISED_DATA);
 E=FreshParent(S);if(E!=EFI_SUCCESS)return Retain(S,E);S->Report.DisableAttempted=TRUE;S->Report.Disable=E=Change(S,FALSE);if(!Live(S)||E!=EFI_SUCCESS)return Retain(S,!Live(S)?EFI_ABORTED:E);
 S->Report.Counter=E=Snapshot(S,&S->Report.Retired);if(E!=EFI_SUCCESS)return Retain(S,E);PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT *A=&S->Report.Retired;
 if(!SameSource(B,A)||A->Total[0]!=B->Total[0]-1||A->PerClient[0]!=B->PerClient[0]-1||A->Total[1]!=B->Total[1]||A->PerClient[1]!=B->PerClient[1]||A->ParentRefs[0]!=B->ParentRefs[0]-(!A->Total[0])||A->ParentRefs[1]!=B->ParentRefs[1])return Retain(S,EFI_COMPROMISED_DATA);S->Report.OwnedReferences=0;
 S->Report.After=E=Rail(S,"non-gdsc-retired",&S->Report.RailRetired);if(E!=EFI_SUCCESS)return Retain(S,E);
 S->Report.Close=E=S->Env.Gcc->Env.Services->CloseEvent(S->Exit);if(!Live(S)||E!=EFI_SUCCESS)return Retain(S,!Live(S)?EFI_ABORTED:E);S->Exit=NULL;
 E=Return(S);if(E!=EFI_SUCCESS)return Retain(S,E);S->Report.Held=FALSE;S->Report.Released=TRUE;S->Report.Busy=FALSE;return S->Report.Status=EFI_SUCCESS;
}
