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
#include <Guid/EventGroup.h>

EFI_BOOT_SERVICES *gBS;EFI_SYSTEM_TABLE *gST;
EFI_GUID gEfiEventExitBootServicesGuid=EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
EFI_STATUS EFIAPI PianoProductPumpLibDestructor(EFI_HANDLE,EFI_SYSTEM_TABLE *);
STATIC EFI_BOOT_SERVICES Bs;STATIC EFI_SYSTEM_TABLE St;
STATIC PIANO_PRODUCT_RUNTIME_PROTOCOL Protocol;
STATIC EFI_EVENT_NOTIFY ExitCallback;STATIC VOID *ExitContext;
STATIC UINT32 Scenario,Pumps,AliveCalls,PendingCalls,Acks,Requests,Locates,Creates,Closes,Raises,Restores,Checks,Signals,Tasks,QuitCalls;
STATIC BOOLEAN FenceLive,Ready,Exited;STATIC jmp_buf DeadJump;
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
#define EFI_TIMER_PERIOD_MILLISECONDS(M) ((M)*1000)
STATIC EFI_TPL EFIAPI Raise(EFI_TPL New){assert(!Exited);Raises++;EFI_TPL Old=gEfiCurrentTpl;assert(New==TPL_HIGH_LEVEL);gEfiCurrentTpl=New;return Old;}
STATIC VOID EFIAPI Restore(EFI_TPL Old){assert(!Exited);Restores++;gEfiCurrentTpl=Old;}
STATIC EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,CONST VOID *Context,CONST EFI_GUID *Group,EFI_EVENT *Event){
  assert(!Exited&&Type==EVT_NOTIFY_SIGNAL&&Tpl==TPL_NOTIFY&&Group==&gEfiEventExitBootServicesGuid);Creates++;
  ExitCallback=Notify;ExitContext=(VOID *)Context;FenceLive=TRUE;*Event=(VOID *)0x77;
  if(Scenario==22){*Event=NULL;return EFI_WARN_STALE_DATA;}
  return Scenario==8?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Close(EFI_EVENT Event){assert(!Exited&&Event==(VOID *)0x77&&FenceLive);Closes++;if(Scenario==15)return EFI_WARN_STALE_DATA;FenceLive=FALSE;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Value){
  (VOID)Registration;assert(!Exited);EFI_GUID G=PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID;assert(!memcmp(Guid,&G,sizeof(G)));Locates++;*Value=&Protocol;
  return Scenario==7?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC VOID SignalExit(VOID){assert(FenceLive&&ExitCallback);ExitCallback((VOID *)0x77,ExitContext);Exited=TRUE;}
STATIC BOOLEAN EFIAPI ProviderAlive(PIANO_PRODUCT_RUNTIME_PROTOCOL *P){assert(!Exited&&P==&Protocol);AliveCalls++;if(Scenario==4){SignalExit();return TRUE;}return TRUE;}
STATIC EFI_STATUS EFIAPI ProviderPump(PIANO_PRODUCT_RUNTIME_PROTOCOL *P,UINT32 Reason,UINTN Budget){
  assert(!Exited&&P==&Protocol&&gEfiCurrentTpl==TPL_APPLICATION&&!gui_lock&&Budget==1000);assert(Reason==PIANO_PRODUCT_PUMP_WAIT_EVENT||Reason==PIANO_PRODUCT_PUMP_GUI||Reason==PIANO_PRODUCT_PUMP_APP);Pumps++;
  if(Scenario==3)assert(PianoProductPumpApplication(PIANO_PRODUCT_PUMP_APP,1000)==EFI_NOT_READY);
  if(Scenario==5){SignalExit();return EFI_SUCCESS;}
  Ready=TRUE;return Scenario==9?EFI_WARN_STALE_DATA:Scenario==35?EFI_ABORTED:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Request(PIANO_PRODUCT_RUNTIME_PROTOCOL *P,UINT32 Action){assert(!Exited&&P==&Protocol&&(Action==PIANO_PRODUCT_ACTION_REQUEST_REBOOT||(Action>=1&&Action<=3)));Requests++;if(Scenario==25||Scenario==32)return EFI_WARN_STALE_DATA;if(Scenario==26||Scenario==37)SignalExit();if(Scenario==29||Scenario==33)return EFI_UNSUPPORTED;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Pending(PIANO_PRODUCT_RUNTIME_PROTOCOL *P,UINT32 *Action,UINT64 *Sequence){
  assert(!Exited&&P==&Protocol&&gEfiCurrentTpl==TPL_APPLICATION&&!gui_lock);PendingCalls++;
  *Action=Scenario==10?PIANO_PRODUCT_ACTION_SETUP:(Scenario==18||Scenario==20)?PIANO_PRODUCT_ACTION_RETURN_CORE:(Scenario==35||Scenario==36)?PIANO_PRODUCT_ACTION_SHELL:Scenario==13?99:PIANO_PRODUCT_ACTION_NONE;*Sequence=19;
  if(Scenario==19||Scenario==21||Scenario==38)SignalExit();
  return Scenario==12?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Ack(PIANO_PRODUCT_RUNTIME_PROTOCOL *P,UINT64 Sequence){(VOID)P;(VOID)Sequence;Acks++;return EFI_SUCCESS;}
VOID EFIAPI CpuDeadLoop(VOID){longjmp(DeadJump,1);}
STATIC EFI_STATUS EFIAPI Watchdog(UINTN Timeout,UINT64 Code,UINTN Bytes,CHAR16 *Data){(VOID)Timeout;(VOID)Code;(VOID)Bytes;(VOID)Data;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Stall(UINTN Us){(VOID)Us;assert(!Exited);return EFI_SUCCESS;}
EFI_STATUS CoreCheckEvent(EFI_EVENT Event){assert(Event==(VOID *)0x1);Checks++;return Ready?EFI_SUCCESS:EFI_NOT_READY;}
EFI_STATUS CoreSignalEvent(EFI_EVENT Event){assert(Event==gIdleLoopEvent);Signals++;Ready=TRUE;return EFI_SUCCESS;}
STATIC int64_t confd_get_integer(const char *Name,int64_t Default){(VOID)Name;return Default;}
STATIC bool confd_get_boolean(const char *Name,bool Default){(VOID)Name;return Default;}
STATIC bool guidrv_can_sleep(void){return false;}
STATIC UINT32 lv_disp_get_inactive_time(VOID *P){(VOID)P;return 0;}
STATIC UINT32 lv_task_handler(void){assert(gui_lock);return 0;}
STATIC VOID guidrv_taskhandler(void){assert(gui_lock);Tasks++;gui_run=false;}
STATIC VOID gui_enter_sleep(void){assert(!"no sleep in fixture");}
STATIC VOID gui_do_quit(void){assert(!gui_lock);QuitCalls++;}
STATIC VOID conf_save_cb(lv_timer_t *T){(VOID)T;}
STATIC VOID image_cache_cb(lv_timer_t *T){(VOID)T;}
STATIC VOID *lv_timer_create(VOID (*Callback)(lv_timer_t *),UINT32 Ms,VOID *Data){(VOID)Callback;(VOID)Ms;(VOID)Data;return NULL;}
STATIC VOID tlog_notice(const char *Text,...){(VOID)Text;}
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
  if(Case<=3||Case==7||Case==9||(Case>=10&&Case<=13))assert(PianoProductPumpLibDestructor(NULL,NULL)==EFI_SUCCESS);
  (VOID)Status;
}
int main(void){
  for(UINT32 I=0;I<39;I++){pid_t P=fork();assert(P>=0);if(!P){Run(I);_exit(0);}int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"pump case%u failed\n",I);return 1;}}
  puts("Actual product client + CoreWaitForEvent/gui_main: 39 fork cases including navigation request/yield/EBS; no fake keys/device");return 0;
}
