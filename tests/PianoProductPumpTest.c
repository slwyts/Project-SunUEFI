// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual protocol client and exact extracted CoreWaitForEvent/gui_main bodies.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#undef NULL
#include <Library/PianoProductPumpLib.h>
#include <Library/TimerLib.h>
#include "piano_product_runtime.h"
#include <Guid/EventGroup.h>

EFI_BOOT_SERVICES *gBS;EFI_SYSTEM_TABLE *gST;
EFI_GUID gEfiEventExitBootServicesGuid=EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
EFI_STATUS EFIAPI PianoProductPumpLibDestructor(EFI_HANDLE,EFI_SYSTEM_TABLE *);
STATIC EFI_BOOT_SERVICES Bs;STATIC EFI_SYSTEM_TABLE St;
STATIC PIANO_PRODUCT_RUNTIME_PROTOCOL Protocol;
STATIC PIANO_PRODUCT_IDLE_PROTOCOL Idle;
STATIC UINT64 IdleSequence;
STATIC EFI_EVENT_NOTIFY ExitCallback;STATIC VOID *ExitContext;
STATIC UINT32 Scenario,Pumps,AliveCalls,PendingCalls,Acks,Requests,Locates,Creates,Closes,Raises,Restores,Checks,Signals,Tasks,QuitCalls;
STATIC BOOLEAN FenceLive,Ready,Exited;STATIC jmp_buf DeadJump;
STATIC UINT64 Counter=100000,CounterStart=0,CounterEnd=MAX_UINT64;
STATIC UINT32 Stalls,TimerReads;STATIC UINT64 StallUs;
STATIC UINT32 FrameLogs;
EFI_TPL gEfiCurrentTpl;EFI_EVENT gIdleLoopEvent=(VOID *)0x9;
bool gui_run;STATIC int (*run_exit)(void *);STATIC UINT32 gui_lock;STATIC UINT64 tick_ms;
typedef int runnable_t(void *);
typedef VOID lv_timer_t;
#define MUTEX_INIT(L) ((L)=0)
#define MUTEX_LOCK(L) do{assert(!(L));(L)=1;}while(0)
#define MUTEX_UNLOCK(L) do{assert((L));(L)=0;}while(0)
#define REPORT_STATUS_CODE(A,B) do{(void)(A);(void)(B);}while(0)
#define EFI_PROGRESS_CODE 0
#define EFI_SOFTWARE_DXE_BS_DRIVER 0
#define EFI_SW_PC_INPUT_WAIT 0
/* The real EFI timer macro is 100ns units, not Stall microseconds. */
#define EFI_TIMER_PERIOD_MILLISECONDS(M) ((M)*10000)
STATIC VOID Advance(UINT64 Us){
  if(Scenario==51)return;
  if(CounterStart<CounterEnd){
    Counter=CounterEnd==MAX_UINT64?Counter+Us:(Counter+Us)%(CounterEnd+1);
  }else Counter=Us<=Counter?Counter-Us:CounterStart-((Us-Counter-1)%(CounterStart+1));
}
UINT64 EFIAPI GetPerformanceCounter(VOID){assert(!Exited);TimerReads++;return Counter;}
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *Start,UINT64 *End){assert(!Exited);*Start=CounterStart;*End=CounterEnd;return 1000000;}
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 Ticks){assert(!Exited);return Ticks*1000;}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL New){assert(!Exited);Raises++;EFI_TPL Old=gEfiCurrentTpl;assert(New==TPL_HIGH_LEVEL);gEfiCurrentTpl=New;return Old;}
STATIC VOID EFIAPI Restore(EFI_TPL Old){assert(!Exited);Restores++;gEfiCurrentTpl=Old;}
STATIC EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,CONST VOID *Context,CONST EFI_GUID *Group,EFI_EVENT *Event){
  assert(!Exited&&Type==EVT_NOTIFY_SIGNAL&&Tpl==TPL_NOTIFY&&Group==&gEfiEventExitBootServicesGuid);Creates++;
  ExitCallback=Notify;ExitContext=(VOID *)Context;FenceLive=TRUE;*Event=(VOID *)0x77;
  if(Scenario==22){*Event=NULL;return EFI_WARN_STALE_DATA;}
  return Scenario==8?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Close(EFI_EVENT Event){assert(!Exited&&Event==(VOID *)0x77&&FenceLive);Closes++;if(Scenario==15)return EFI_WARN_STALE_DATA;FenceLive=FALSE;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI IdleRead(PIANO_PRODUCT_IDLE_PROTOCOL *This,CONST PIANO_PRODUCT_RUNTIME_PROTOCOL *Runtime,UINT64 *Sequence,BOOLEAN *Active){
  assert(This==&Idle&&Runtime==&Protocol&&!Exited);*Sequence=Scenario==59?1:IdleSequence;
  *Active=(Scenario==56||Scenario==57)?Pumps<3:Scenario==61?2:FALSE;
  if(Scenario==60){ExitCallback((VOID*)0x77,ExitContext);Exited=TRUE;return EFI_SUCCESS;}
  return Scenario==63?EFI_NOT_READY:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Value){
  (VOID)Registration;assert(!Exited);EFI_GUID G=PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID,H=PIANO_PRODUCT_IDLE_PROTOCOL_GUID;
  if(!memcmp(Guid,&H,sizeof(H))){if(Scenario==58)return EFI_NOT_FOUND;Idle=(PIANO_PRODUCT_IDLE_PROTOCOL){PIANO_PRODUCT_IDLE_REVISION,&Protocol,IdleRead};*Value=&Idle;if(Scenario==62)Idle.Runtime=(VOID*)123;return EFI_SUCCESS;}
  assert(!memcmp(Guid,&G,sizeof(G)));if(!Pumps || Scenario==7)Locates++;*Value=&Protocol;
  return Scenario==7?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC VOID SignalExit(VOID){assert(FenceLive&&ExitCallback);ExitCallback((VOID *)0x77,ExitContext);Exited=TRUE;}
STATIC BOOLEAN EFIAPI ProviderAlive(PIANO_PRODUCT_RUNTIME_PROTOCOL *P){assert(!Exited&&P==&Protocol);AliveCalls++;if(Scenario==4){SignalExit();return TRUE;}return TRUE;}
STATIC EFI_STATUS EFIAPI ProviderPump(PIANO_PRODUCT_RUNTIME_PROTOCOL *P,UINT32 Reason,UINTN Budget){
  assert(!Exited&&P==&Protocol&&gEfiCurrentTpl==TPL_APPLICATION&&!gui_lock&&Budget==1000);assert(Reason==PIANO_PRODUCT_PUMP_WAIT_EVENT||Reason==PIANO_PRODUCT_PUMP_GUI||Reason==PIANO_PRODUCT_PUMP_APP);Pumps++;++IdleSequence;
  if(Scenario==3)assert(PianoProductPumpApplication(PIANO_PRODUCT_PUMP_APP,1000)==EFI_NOT_READY);
  if(Scenario==5){SignalExit();return EFI_SUCCESS;}
  if(Scenario==52)Advance(50);
  Ready=(Scenario==56||Scenario==58)?Pumps>=3:Scenario==57?Pumps>=4:TRUE;return Scenario==9?EFI_WARN_STALE_DATA:Scenario==35?EFI_ABORTED:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Request(PIANO_PRODUCT_RUNTIME_PROTOCOL *P,UINT32 Action){assert(!Exited&&P==&Protocol&&(Action==PIANO_PRODUCT_ACTION_REQUEST_REBOOT||Action==PIANO_PRODUCT_ACTION_REQUEST_CONTINUE||(Action>=1&&Action<=3)));Requests++;if(Scenario==25||Scenario==32||Scenario==40)return EFI_WARN_STALE_DATA;if(Scenario==26||Scenario==37||Scenario==42)SignalExit();if(Scenario==29||Scenario==33||Scenario==41)return EFI_UNSUPPORTED;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Pending(PIANO_PRODUCT_RUNTIME_PROTOCOL *P,UINT32 *Action,UINT64 *Sequence){
  assert(!Exited&&P==&Protocol&&gEfiCurrentTpl==TPL_APPLICATION&&!gui_lock);PendingCalls++;
  *Action=(Scenario==10||(Scenario==46&&Pumps==3))?PIANO_PRODUCT_ACTION_SETUP:(Scenario==18||Scenario==20)?PIANO_PRODUCT_ACTION_RETURN_CORE:(Scenario==35||Scenario==36)?PIANO_PRODUCT_ACTION_SHELL:Scenario==13?99:PIANO_PRODUCT_ACTION_NONE;*Sequence=19;
  if(Scenario==19||Scenario==21||Scenario==38)SignalExit();
  return Scenario==12?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Ack(PIANO_PRODUCT_RUNTIME_PROTOCOL *P,UINT64 Sequence){(VOID)P;(VOID)Sequence;Acks++;return EFI_SUCCESS;}
VOID EFIAPI CpuDeadLoop(VOID){longjmp(DeadJump,1);}
STATIC EFI_STATUS EFIAPI Watchdog(UINTN Timeout,UINT64 Code,UINTN Bytes,CHAR16 *Data){(VOID)Timeout;(VOID)Code;(VOID)Bytes;(VOID)Data;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Stall(UINTN Us){assert(!Exited&&!gui_lock&&Us==1000);Stalls++;StallUs+=Us;Advance(Us);if(Scenario==47){SignalExit();gBS=(VOID *)1;}return Scenario==53?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
EFI_STATUS CoreCheckEvent(EFI_EVENT Event){assert(Event==(VOID *)0x1);Checks++;return Ready?EFI_SUCCESS:EFI_NOT_READY;}
EFI_STATUS CoreSignalEvent(EFI_EVENT Event){assert(Event==gIdleLoopEvent);Signals++;Ready=TRUE;return EFI_SUCCESS;}
STATIC int64_t confd_get_integer(const char *Name,int64_t Default){(VOID)Name;return Default;}
STATIC bool confd_get_boolean(const char *Name,bool Default){(VOID)Name;return Default;}
STATIC bool guidrv_can_sleep(void){return false;}
STATIC UINT32 lv_disp_get_inactive_time(VOID *P){(VOID)P;return 0;}
STATIC UINT32 lv_task_handler(void){assert(gui_lock);if(Scenario==54){Advance(5000);return 1;}if(Scenario==55)return 30;return 0;}
STATIC VOID guidrv_taskhandler(void){assert(gui_lock);Tasks++;if(Scenario==55){if(Tasks==100)gui_run=false;}else if(Scenario!=54||Tasks==2)gui_run=false;}
STATIC VOID gui_enter_sleep(void){assert(!"no sleep in fixture");}
STATIC VOID gui_do_quit(void){assert(!gui_lock);QuitCalls++;}
STATIC VOID conf_save_cb(lv_timer_t *T){(VOID)T;}
STATIC VOID image_cache_cb(lv_timer_t *T){(VOID)T;}
STATIC VOID *lv_timer_create(VOID (*Callback)(lv_timer_t *),UINT32 Ms,VOID *Data){(VOID)Callback;(VOID)Ms;(VOID)Data;return NULL;}
typedef struct { UINT32 Id; } lv_obj_t;
struct gui_activity { char name[256]; };
STATIC lv_obj_t Screen={1},Child={2};
STATIC lv_obj_t *lv_scr_act(VOID){return &Screen;}
STATIC UINT32 lv_obj_get_child_cnt(lv_obj_t *P){assert(P==&Screen);return 1;}
STATIC lv_obj_t *lv_obj_get_child(lv_obj_t *P,int Index){assert(P==&Screen&&Index==-1);return &Child;}
STATIC UINT32 lv_obj_get_style_opa(lv_obj_t *P,int Selector){assert((P==&Screen||P==&Child)&&!Selector);return 255;}
STATIC struct gui_activity *guiact_get_last(VOID){return NULL;}
STATIC VOID tlog_notice(const char *Text,...){assert(!gui_lock);if(!strncmp(Text,"PIANO_GUI_FRAME ",16))FrameLogs++;}
extern void piano_product_gui_pump(void);
#include "PianoActualWaitAndGui.h"
STATIC VOID Setup(UINT32 Case){
  Scenario=Case;memset(&Bs,0,sizeof(Bs));memset(&St,0,sizeof(St));gBS=&Bs;gST=&St;
  Bs.Hdr.Signature=EFI_BOOT_SERVICES_SIGNATURE;Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Bs.CreateEventEx=Create;Bs.CloseEvent=Close;Bs.LocateProtocol=Locate;Bs.SetWatchdogTimer=Watchdog;Bs.Stall=Stall;St.BootServices=&Bs;
  Protocol=(PIANO_PRODUCT_RUNTIME_PROTOCOL){PIANO_PRODUCT_RUNTIME_REVISION,ProviderPump,ProviderAlive,Request,Pending,Ack};gEfiCurrentTpl=TPL_APPLICATION;gui_run=true;
}
STATIC VOID Run(UINT32 Case){
  Setup(Case);EFI_STATUS Status;EFI_EVENT Event=(VOID *)1;UINTN Index=99;
  if(Case==0){assert(CoreWaitForEvent(1,&Event,&Index)==EFI_SUCCESS&&Index==0&&Pumps==1&&Checks==1&&Signals==0&&Locates==1);}
  if(Case==1){gEfiCurrentTpl=TPL_CALLBACK;Ready=TRUE;assert(CoreWaitForEvent(1,&Event,&Index)==EFI_SUCCESS&&!Pumps&&!Raises&&!Creates);}
  if(Case==2){gEfiCurrentTpl=TPL_CALLBACK;assert(PianoProductPumpApplication(4,1000)==EFI_UNSUPPORTED&&!Pumps&&!Creates&&!Locates&&Raises==1&&Restores==1);}
  if(Case==3){assert(PianoProductPumpApplication(4,1000)==EFI_SUCCESS&&Pumps==1&&Raises==1);}
  if(Case==4||Case==5){assert(PianoProductPumpApplication(4,1000)==EFI_ABORTED);UINT32 Calls=Raises+Locates+Creates+Pumps+AliveCalls;assert(PianoProductPumpApplication(4,1000)==EFI_NOT_READY&&Raises+Locates+Creates+Pumps+AliveCalls==Calls);assert(!PianoProductPumpBootServicesAlive());if(Case==4)assert(!Pumps);}
  if(Case==6){assert(PianoProductPumpApplication(4,1000)==EFI_SUCCESS);St.BootServices=NULL;UINT32 Calls=Raises+AliveCalls+Pumps;assert(PianoProductPumpApplication(4,1000)==EFI_NOT_READY&&Raises+AliveCalls+Pumps==Calls);}
  if(Case==7){assert(PianoProductPumpApplication(4,1000)==EFI_DEVICE_ERROR&&!AliveCalls&&!Pumps);}
  if(Case==8){assert(PianoProductPumpApplication(4,1000)==EFI_DEVICE_ERROR&&!Locates&&!Pumps&&!PianoProductPumpBootServicesAlive());}
  if(Case==9){assert(PianoProductPumpApplication(4,1000)==EFI_DEVICE_ERROR&&Pumps==1);}
  if(Case>=10&&Case<=13){assert(gui_main()==0&&QuitCalls==1&&!Acks&&!Requests);if(Case==10&&PIANO_PRODUCT_GUI_PUMP){assert(Pumps==1&&PendingCalls==1&&!Tasks);}else assert(Tasks==1);}
  if(Case==14){assert(PianoProductPumpApplication(4,1000)==EFI_SUCCESS);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS&&!FenceLive&&Closes==1);}
  if(Case==15){assert(PianoProductPumpApplication(4,1000)==EFI_SUCCESS);if(!setjmp(DeadJump)){PianoProductPumpLibDestructor(NULL,NULL);assert(!"warning close must fail stop");}assert(Closes==1&&FenceLive);}
  if(Case==16){assert(PianoProductPumpApplication(4,1000)==EFI_SUCCESS);St.BootServices=NULL;if(!setjmp(DeadJump)){PianoProductPumpLibDestructor(NULL,NULL);assert(!"live callback and missing BS must fail stop");}assert(!Closes&&FenceLive);}
  if(Case==17){assert(PianoProductPumpApplication(4,1000)==EFI_SUCCESS);SignalExit();UINT32 Calls=Raises+Locates+Creates+Pumps+AliveCalls+Closes;UINT32 Action=99;UINT64 Seq=99;assert(PianoProductGetPendingAction(&Action,&Seq)==EFI_NOT_READY&&Action==0&&Seq==0);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_ABORTED);assert(Raises+Locates+Creates+Pumps+AliveCalls+Closes==Calls);}
  if(Case==18){assert(gui_main()==0&&QuitCalls==1&&!Acks&&!Requests);if(PIANO_PRODUCT_GUI_PUMP)assert(PendingCalls==1&&!Tasks);else assert(Tasks==1);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==19){if(PIANO_PRODUCT_GUI_PUMP){if(!setjmp(DeadJump)){gui_main();assert(!"GUI pending EBS must fail stop before further BS/GUI cleanup");}assert(Exited&&!Tasks&&!QuitCalls&&!Acks&&!Requests);}else assert(gui_main()==0&&Tasks==1&&QuitCalls==1);}
  if(Case==20){assert(CoreWaitForEvent(1,&Event,&Index)==EFI_ABORTED&&Pumps==1&&PendingCalls==1&&!Acks&&!Requests&&!Checks&&!Signals);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==21){if(!setjmp(DeadJump)){CoreWaitForEvent(1,&Event,&Index);assert(!"helper EBS must stop before any UI/Core cleanup");}assert(Pumps==1&&PendingCalls==1&&Exited&&!Checks&&!Signals&&!Acks&&!Requests);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_ABORTED);}
  if(Case==22){assert(PianoProductPumpApplication(4,1000)==EFI_DEVICE_ERROR&&!Locates&&!Pumps);if(!setjmp(DeadJump)){PianoProductPumpLibDestructor(NULL,NULL);assert(!"unknown callback and absent handle must retain app");}assert(!Closes);}
  if(Case==23){gBS=NULL;if(!setjmp(DeadJump)){PianoProductReturnCoreRequested();assert(!"missing BS must not permit UI cleanup");}assert(!Raises&&!Creates&&!Locates&&!Pumps&&!PendingCalls);}
  if(Case==24){assert(PianoProductRequestReboot()==EFI_SUCCESS&&Requests==1&&!Acks&&!Pumps&&!PendingCalls);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==25){assert(PianoProductRequestReboot()==EFI_DEVICE_ERROR&&Requests==1&&!Acks&&!Pumps&&!PendingCalls);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==26){if(!setjmp(DeadJump)){PianoProductRequestReboot();assert(!"UI reboot EBS must stop before cleanup");}assert(Exited&&Requests==1&&!Acks&&!Pumps&&!PendingCalls&&!Closes);}
  if(Case==27){assert(PianoProductRebootManaged());gEfiCurrentTpl=TPL_CALLBACK;assert(PianoProductRequestReboot()==EFI_ACCESS_DENIED&&!Requests&&!Creates&&!Locates&&!Pumps);}
  if(Case==28){gBS=NULL;if(!setjmp(DeadJump)){PianoProductRequestReboot();assert(!"missing BS must not return into UI cleanup");}assert(!Raises&&!Creates&&!Locates&&!Requests);}
  if(Case==29){assert(PianoProductRequestReboot()==EFI_ACCESS_DENIED&&Requests==1);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==30||Case==31){for(UINT32 A=1;A<=3;++A)assert(PianoProductRequestNavigation(A)==EFI_SUCCESS);assert(Requests==3&&!Acks&&!Pumps);assert(PianoProductRequestNavigation(0)==EFI_INVALID_PARAMETER&&PianoProductRequestNavigation(4)==EFI_INVALID_PARAMETER&&PianoProductRequestNavigation(5)==EFI_INVALID_PARAMETER&&Requests==3);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==32||Case==33){assert(PianoProductRequestNavigation(2)==(Case==32?EFI_DEVICE_ERROR:EFI_ACCESS_DENIED)&&Requests==1&&!Acks);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==34){gEfiCurrentTpl=TPL_CALLBACK;assert(PianoProductRequestNavigation(2)==EFI_ACCESS_DENIED&&!Creates&&!Requests&&!Pumps);}
  if(Case==35){assert(CoreWaitForEvent(1,&Event,&Index)==EFI_ABORTED&&Pumps==1&&!Checks&&!Signals&&!Acks&&!Requests&&!Exited);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==36){assert(PianoProductUiReturnRequested()&&!PianoProductReturnCoreRequested()&&!Pumps&&!Acks&&!Requests);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==37){if(!setjmp(DeadJump)){PianoProductRequestNavigation(2);assert(!"navigation EBS must halt before UI cleanup");}assert(Exited&&Requests==1&&!Closes&&!Acks);}
  if(Case==38){if(!setjmp(DeadJump)){PianoProductUiReturnRequested();assert(!"navigation query EBS must halt before UI cleanup");}assert(Exited&&!Closes&&!Acks&&!Requests);}
  if(Case==39){assert(PianoProductRequestContinue()==EFI_SUCCESS&&Requests==1&&!Pumps&&!Acks);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==40||Case==41){assert(PianoProductRequestContinue()==(Case==40?EFI_DEVICE_ERROR:EFI_ACCESS_DENIED)&&Requests==1);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==42){if(!setjmp(DeadJump)){PianoProductRequestContinue();assert(!"continue EBS must halt");}assert(Exited&&Requests==1&&!Closes&&!Acks);}
  if(Case==43){gEfiCurrentTpl=TPL_CALLBACK;assert(PianoProductRequestContinue()==EFI_ACCESS_DENIED&&!Requests&&!Creates&&!Pumps);}
  if(Case==44||Case==45){assert(piano_product_gui_tick()==0);assert(piano_product_gui_wait(Case==44?30:MAX_UINT32)==30);assert(Stalls==30&&StallUs==30000&&piano_product_gui_tick()==30);assert(Pumps==(PIANO_PRODUCT_GUI_PUMP?30:0)&&!Tasks&&!Acks&&!Requests);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==46){UINT32 Elapsed=piano_product_gui_wait(30);if(PIANO_PRODUCT_GUI_PUMP)assert(Elapsed==2&&Pumps==3&&Stalls==2&&!gui_run&&!Acks&&!Requests);else assert(Elapsed==30&&!Pumps&&Stalls==30);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==47){if(PIANO_PRODUCT_GUI_PUMP){if(!setjmp(DeadJump)){piano_product_gui_wait(30);assert(!"EBS during Stall must halt before subsequent timer/provider/BS calls");}assert(Exited&&Stalls==1&&Pumps==1&&!QuitCalls);}else assert(piano_product_gui_wait(0)==0&&!Stalls&&!TimerReads);}
  if(Case==48){assert(piano_product_gui_tick()==0);Advance(600000);assert(piano_product_gui_tick()==600);assert(!Pumps&&!Stalls);assert(piano_product_gui_wait(0)==0&&!Pumps&&!Stalls);
#if PIANO_PRODUCT_GUI_PUMP
    assert(custom_tick_get()==600);Advance(1000);assert(custom_tick_get()==601);
#endif
  }
  if(Case==49||Case==50){CounterStart=Case==49?0:3999;CounterEnd=Case==49?3999:0;Counter=Case==49?3900:100;assert(piano_product_gui_tick()==0);Advance(2000);assert(piano_product_gui_tick()==2);Advance(2000);assert(piano_product_gui_tick()==4);}
  if(Case==51){assert(piano_product_gui_wait(MAX_UINT32)==0&&Stalls==30&&StallUs==30000&&piano_product_gui_tick()==0);assert(Pumps==(PIANO_PRODUCT_GUI_PUMP?30:0));assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==52){assert(piano_product_gui_wait(30)==(PIANO_PRODUCT_GUI_PUMP?31:30));assert(Stalls==30&&StallUs==30000);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==53){if(!setjmp(DeadJump)){piano_product_gui_wait(30);assert(!"warning Stall cannot be success");}assert(Stalls==1&&!QuitCalls&&!Acks);assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==54){assert(piano_product_gui_tick()==0);assert(gui_main()==0&&Tasks==2&&QuitCalls==1);assert(piano_product_gui_tick()==(PIANO_PRODUCT_GUI_PUMP?11:12));assert(Stalls==(PIANO_PRODUCT_GUI_PUMP?1:2));assert(Pumps==(PIANO_PRODUCT_GUI_PUMP?3:0));assert(tick_ms==(PIANO_PRODUCT_GUI_PUMP?1:2));assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==55){assert(gui_main()==0&&Tasks==100&&QuitCalls==1);assert(FrameLogs==(PIANO_PRODUCT_GUI_PUMP?8:0));assert(Stalls==(PIANO_PRODUCT_GUI_PUMP?2970:3000)&&StallUs==(PIANO_PRODUCT_GUI_PUMP?2970000:3000000));assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==56||Case==57||Case==58){assert(CoreWaitForEvent(1,&Event,&Index)==EFI_SUCCESS&&Pumps==(Case==57?4:3)&&Signals==(Case==57?1:0));assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==59){assert(PianoProductPumpApplication(1,1000)==EFI_SUCCESS&&PianoProductPumpShouldIdle());assert(!PianoProductPumpShouldIdle());assert(PianoProductPumpApplication(1,1000)==EFI_SUCCESS&&!PianoProductPumpShouldIdle());assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case==60){assert(PianoProductPumpApplication(1,1000)==EFI_SUCCESS&&!PianoProductPumpShouldIdle()&&!PianoProductPumpBootServicesAlive());}
  if(Case==61||Case==62||Case==63){assert(PianoProductPumpApplication(1,1000)==EFI_SUCCESS&&!PianoProductPumpShouldIdle());assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);}
  if(Case<=3||Case==7||Case==9||(Case>=10&&Case<=13))assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);
  (VOID)Status;
}
int main(void){
  for(UINT32 I=0;I<64;I++){pid_t P=fork();assert(P>=0);if(!P){Run(I);_exit(0);}int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"pump case%u failed\n",I);return 1;}}
  puts("Actual product client + CoreWaitForEvent/gui_main/custom_tick_get: 64 fork cases, measured GUI time/bounded 1ms service/navigation/EBS/8 logs; no fake keys/device");return 0;
}
