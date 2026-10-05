// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual Device/Controller + extracted exact policy Pump/ReadIdle/CoreWait +
// actual client. Hardware/keys/event idle only are fixtures, no USB execution.
#define USB_SERVICE_TEST_MAIN persistent_service_suite
#include "PianoUsbServiceTest.c"
#undef USB_SERVICE_TEST_MAIN
#include "../bootprofiles/uefi-app/PianoBootPolicy.h"
#include <Library/PianoProductPumpLib.h>
EFI_SYSTEM_TABLE *gST;static EFI_SYSTEM_TABLE system_table;
static PIANO_BOOT_POLICY_REPORT mReport;
static PIANO_PRODUCT_RUNTIME_PROTOCOL mRuntime;
static PIANO_PRODUCT_IDLE_PROTOCOL mIdle;
static BOOLEAN mAlive=TRUE,wait_ready;
static UINTN wait_checks,idles;
#define gEfiCurrentTpl current_tpl
EFI_EVENT gIdleLoopEvent=(VOID*)9;
static UINT64 Critical(VOID){return 0;}static VOID EndCritical(UINT64 Mask){(VOID)Mask;}
static EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
static VOID RequireAlive(VOID){assert(mAlive);}
static EFI_STATUS AtApp(VOID){return current_tpl==TPL_APPLICATION?EFI_SUCCESS:EFI_UNSUPPORTED;}
static EFI_STATUS RefreshKeys(VOID){return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Request(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 Action){(VOID)This;(VOID)Action;assert(!"no navigation expected in bulk fixture");return EFI_ACCESS_DENIED;}
static EFI_STATUS EFIAPI PendingIdle(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 *Action,UINT64 *Sequence){assert(This==&mRuntime);*Action=PIANO_PRODUCT_ACTION_NONE;*Sequence=1;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI AckIdle(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT64 Sequence){(VOID)This;(VOID)Sequence;assert(!"wait never acknowledges");return EFI_ACCESS_DENIED;}
static BOOLEAN EFIAPI AliveIdle(PIANO_PRODUCT_RUNTIME_PROTOCOL *This){return This==&mRuntime&&mAlive;}
static EFI_STATUS EFIAPI IdleLocate(EFI_GUID *Guid,VOID *Registration,VOID **Out){(VOID)Registration;EFI_GUID R=PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID,H=PIANO_PRODUCT_IDLE_PROTOCOL_GUID;if(!memcmp(Guid,&R,sizeof(R))){*Out=&mRuntime;return EFI_SUCCESS;}if(!memcmp(Guid,&H,sizeof(H))){*Out=&mIdle;return EFI_SUCCESS;}return EFI_NOT_FOUND;}
EFI_STATUS CoreCheckEvent(EFI_EVENT Event){assert(Event==(VOID*)1&&current_tpl==TPL_APPLICATION);++wait_checks;
 if(wait_checks==1){assert(mReport.IdleKnown&&mReport.IdleBulkActive&&mService.State.BulkActive&&!mService.Count&&!mService.State.WorkPending&&mPending[3]);DWC_TRB *T=mTrbs[3].Cpu;T->Size=0;T->Control&=~BIT0;publish(0xC046);timer_tick();}
 if(wait_checks==2)assert(!mReport.IdleBulkActive&&!mService.State.BulkActive&&!mService.Count&&mPending[2]&&!mPending[3]);
 return wait_ready?EFI_SUCCESS:EFI_NOT_READY;
}
EFI_STATUS CoreSignalEvent(EFI_EVENT Event){assert(Event==gIdleLoopEvent&&!mReport.IdleBulkActive&&!mPending[3]);++idles;wait_ready=TRUE;return EFI_SUCCESS;}
#include "PianoActualBulkIdlePipeline.h"
EFI_STATUS EFIAPI PianoProductPumpLibDestructor(EFI_HANDLE,EFI_SYSTEM_TABLE *);
int main(VOID){
 setup_model();stall_hook=NULL;current_tpl=TPL_APPLICATION;scenario=0;fake_now=0;
 timer_notify=exit_notify=NULL;bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore_tpl;bs.CreateEvent=create_event;bs.CreateEventEx=create_event_ex;bs.SetTimer=set_timer;bs.CloseEvent=close_event;clock.DisableClock=clock.DisableClockPowerDomain=persistent_disable;
 PIANO_DWC3_SERVICE_CONFIG C={.Context=(VOID*)0x55,.NowUs=now_us};assert(PianoUsbControllerServiceStart((VOID*)123,&C)==EFI_SUCCESS);service_enumerate();service_out("getvar:version");
 assert(mPending[3]&&mService.State.BulkActive&&!mService.Count);bs.LocateProtocol=IdleLocate;bs.Hdr.Signature=EFI_BOOT_SERVICES_SIGNATURE;system_table.BootServices=&bs;gST=&system_table;
 mReport.Initialized=mReport.ProtocolInstalled=mReport.IdleInstalled=TRUE;mRuntime=(PIANO_PRODUCT_RUNTIME_PROTOCOL){1,Pump,AliveIdle,Request,PendingIdle,AckIdle};mIdle=(PIANO_PRODUCT_IDLE_PROTOCOL){1,&mRuntime,ReadIdle};
 EFI_EVENT Event=(VOID*)1;UINTN Index=99;assert(CoreWaitForEvent(1,&Event,&Index)==EFI_SUCCESS&&Index==0&&wait_checks==3&&idles==1&&mReport.PumpCalls==3);
 assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);PIANO_USB_SERVICE_RETIRE_REPORT R;assert(PianoUsbControllerServiceStop(EFI_SUCCESS,&R)==EFI_SUCCESS&&R.Clean);free_cold_model();
 puts("Actual wait/policy/idle-client/device pipeline: inflight empty queue stays awake, completion restores idle, timer submits no commands, full stop clean");return 0;
}
