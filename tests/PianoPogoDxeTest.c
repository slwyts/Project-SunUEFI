// Real producer/adapter/parser. Backend is a host fixture, not wire evidence.
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#undef NULL
#include "../bootprofiles/pogo-product/PianoPogoDxe.h"
#include <Guid/EventGroup.h>
EFI_BOOT_SERVICES *gBS;EFI_SYSTEM_TABLE *gST;
EFI_GUID gEfiEventExitBootServicesGuid=EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
EFI_GUID gEfiSimpleTextInProtocolGuid={.Data1=1},gEfiSimpleTextInputExProtocolGuid={.Data1=2},gEfiSimplePointerProtocolGuid={.Data1=3},gEfiAbsolutePointerProtocolGuid={.Data1=4},gEfiDevicePathProtocolGuid={.Data1=5};
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
typedef struct {UINT32 Type;EFI_EVENT_NOTIFY Fn;VOID *Context;} EVENT;
static EFI_BOOT_SERVICES Bs;static EFI_SYSTEM_TABLE St;static PIANO_POGO_DXE Driver;static PIANO_POGO_DXE *Current=&Driver;
static UINTN Case,Creates,Closes,Installs,Uninstalls,Signals,Reads,Stops,Notices,Calls;static EFI_TPL Tpl;static UINT64 Clock;
static VOID Alive(void){assert(St.BootServices==&Bs);Calls++;}
static EFI_TPL EFIAPI Raise(EFI_TPL T){Alive();EFI_TPL O=Tpl;Tpl=T;return O;}static VOID EFIAPI Restore(EFI_TPL T){Alive();Tpl=T;}
static EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL Priority,EFI_EVENT_NOTIFY Fn,VOID *Ctx,EFI_EVENT *Out){
  Alive();assert(Priority==TPL_CALLBACK);Creates++;EVENT *E=malloc(sizeof(*E));*E=(EVENT){Type,Fn,Ctx};*Out=E;return Case==9&&Creates==2?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static EFI_STATUS EFIAPI CreateEx(UINT32 Type,EFI_TPL Priority,EFI_EVENT_NOTIFY Fn,CONST VOID *Ctx,CONST EFI_GUID *G,EFI_EVENT *Out){
  Alive();assert(Priority==TPL_NOTIFY&&G==&gEfiEventExitBootServicesGuid);Creates++;EVENT *E=malloc(sizeof(*E));*E=(EVENT){Type,Fn,(VOID *)Ctx};*Out=E;return Case==10?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static VOID Exit(void){EVENT *E=Current->Exit;assert(E&&E->Fn);E->Fn(E,E->Context);St.BootServices=NULL;}
static EFI_STATUS EFIAPI Close(EFI_EVENT Value){Alive();Closes++;if(Case==12&&Closes==1)return EFI_WARN_STALE_DATA;if(Case==15&&Closes==1){Exit();return EFI_SUCCESS;}free(Value);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Timer(EFI_EVENT E,EFI_TIMER_DELAY Mode,UINT64 Value){Alive();assert(E==Current->Timer);assert((Mode==TimerPeriodic&&Value==10000)||(Mode==TimerCancel&&Value==0));return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Signal(EFI_EVENT E){Alive();assert(E!=NULL);Signals++;return Case==16?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static VOID VerifyTuple(va_list V){
  EFI_GUID *G;unsigned N=0;VOID *Expected[]={&Current->Path,&Current->Adapter.Text,&Current->Adapter.TextEx,&Current->Adapter.Pointer,&Current->Adapter.Absolute};
  while((G=va_arg(V,EFI_GUID *))){(void)G;assert(N<5&&va_arg(V,VOID *)==Expected[N]);N++;}assert(N==5);
}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...){Alive();Installs++;va_list V;va_start(V,H);VerifyTuple(V);va_end(V);*H=(VOID *)77;return Case==11?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Uninstall(EFI_HANDLE H,...){Alive();Uninstalls++;assert(H==(VOID *)77);va_list V;va_start(V,H);VerifyTuple(V);va_end(V);return Case==14?EFI_ACCESS_DENIED:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Connect(EFI_HANDLE H,EFI_HANDLE *B,EFI_DEVICE_PATH_PROTOCOL *P,BOOLEAN R){Alive();assert(H==(VOID *)77&&!B&&!P&&R);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Disconnect(EFI_HANDLE H,EFI_HANDLE B,EFI_HANDLE Child){Alive();assert(H==(VOID *)77&&!B&&!Child);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Ready(VOID *C){(void)C;assert(Tpl==TPL_APPLICATION);return Case==0?EFI_NOT_READY:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Data(VOID *C,BOOLEAN *R){(void)C;assert(Tpl==TPL_APPLICATION);*R=Case!=5;return EFI_SUCCESS;}
static UINT64 EFIAPI Now(VOID *C){(void)C;assert(Tpl==TPL_APPLICATION);return Clock;}
static EFI_STATUS EFIAPI Read(VOID *C,UINT64 Deadline,UINT8 *Frame,UINTN *N){
  (void)C;assert(Tpl==TPL_APPLICATION&&Deadline==Clock+10000);Reads++;memset(Frame,0,68);Frame[0]=0x57;Frame[2]=1;Frame[3]=5;Frame[6]=0x45;*N=Case==6?67:68;Clock+=Case==21?11000:100;
  if(Case==8)Exit();if(Case==18){Frame[12]=2;Frame[13]=3;Frame[14]=1;Frame[16]=2;Frame[18]=3;}
  return Case==7?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Stop(VOID *C,PIANO_POGO_BACKEND_STOP *R){(void)C;assert(Tpl==TPL_APPLICATION);Stops++;*R=(PIANO_POGO_BACKEND_STOP){EFI_SUCCESS,TRUE,TRUE,FALSE};if(Case==13)R->Quiet=FALSE;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Notice(EFI_KEY_DATA *K){assert(Tpl==TPL_CALLBACK&&K->Key.ScanCode==SCAN_F12);Notices++;assert(PianoPogoDxeStop(&Driver)==EFI_UNSUPPORTED);return EFI_SUCCESS;}
static VOID Notify(EVENT *E){EFI_TPL O=Tpl;Tpl=TPL_CALLBACK;E->Fn(E,E->Context);Tpl=O;}
static VOID Run(UINTN C){
  Case=C;Tpl=TPL_APPLICATION;Clock=1000;gBS=&Bs;gST=&St;St.BootServices=&Bs;Bs.Hdr.Signature=EFI_BOOT_SERVICES_SIGNATURE;
  Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Bs.CreateEvent=Create;Bs.CreateEventEx=CreateEx;Bs.CloseEvent=Close;Bs.SetTimer=Timer;Bs.SignalEvent=Signal;Bs.InstallMultipleProtocolInterfaces=Install;Bs.UninstallMultipleProtocolInterfaces=Uninstall;Bs.ConnectController=Connect;Bs.DisconnectController=Disconnect;
  PIANO_POGO_DXE_BACKEND B={1,NULL,Ready,Data,Now,Read,Stop};EFI_SIMPLE_POINTER_MODE Mode={1,1,1,TRUE,TRUE};
  EFI_STATUS S=PianoPogoDxeStart(&Driver,&B,&Mode);
  if(C==0){assert(S==EFI_NOT_READY&&!Creates&&!Installs&&!Reads);return;}
  if(C>=9&&C<=11){assert(S!=EFI_SUCCESS&&Driver.Report.Retained);UINTN Before=Calls;assert(PianoPogoDxeStop(&Driver)!=EFI_SUCCESS&&!Closes&&!Uninstalls);assert(Calls>=Before);return;}
  assert(S==EFI_SUCCESS&&Installs==1&&Creates==6&&Driver.Report.Published);
  assert(PianoPogoDxeStart(&Driver,&B,&Mode)==EFI_ALREADY_STARTED);
  EFI_KEY_DATA K={0};K.Key.ScanCode=SCAN_F12;VOID *Handle=NULL,*Again=NULL;
  assert(Driver.Adapter.TextEx.RegisterKeyNotify(&Driver.Adapter.TextEx,&K,Notice,&Handle)==EFI_SUCCESS&&Handle);
  assert(Driver.Adapter.TextEx.RegisterKeyNotify(&Driver.Adapter.TextEx,&K,Notice,&Again)==EFI_SUCCESS&&Again==Handle);
  if(C==22){
    assert(PianoPogoDxeStop(&Driver)==EFI_SUCCESS);PIANO_POGO_DXE NewDriver={0};Current=&NewDriver;
    assert(PianoPogoDxeStart(&NewDriver,&B,&Mode)==EFI_SUCCESS);
    assert(NewDriver.Adapter.TextEx.RegisterKeyNotify(&NewDriver.Adapter.TextEx,&K,Notice,&Again)==EFI_SUCCESS&&Again!=Handle);
    assert(NewDriver.Adapter.TextEx.UnregisterKeyNotify(&NewDriver.Adapter.TextEx,Handle)==EFI_INVALID_PARAMETER);
    assert(NewDriver.Adapter.TextEx.UnregisterKeyNotify(&NewDriver.Adapter.TextEx,Again)==EFI_SUCCESS);
    assert(PianoPogoDxeStop(&NewDriver)==EFI_SUCCESS);return;
  }
  if(C==2){assert(Driver.Adapter.TextEx.UnregisterKeyNotify(&Driver.Adapter.TextEx,Handle)==EFI_SUCCESS);assert(Driver.Adapter.TextEx.RegisterKeyNotify(&Driver.Adapter.TextEx,&K,Notice,&Again)==EFI_SUCCESS&&Again!=Handle);assert(Driver.Adapter.TextEx.UnregisterKeyNotify(&Driver.Adapter.TextEx,Handle)==EFI_INVALID_PARAMETER);}
  if(C==3){Driver.Report.WorkPending=FALSE;UINTN Before=Reads;Notify(Driver.Timer);assert(Driver.Report.WorkPending&&Reads==Before);}
  S=PianoPogoDxePump(&Driver);
  if(C==5){assert(S==EFI_NOT_READY&&!Reads&&!Driver.Report.Frames);}
  else if(C==6||C==7||C==8||C==16||C==21){assert(S!=EFI_SUCCESS&&Driver.Report.Retained);if(C==8){UINTN Before=Calls;assert(PianoPogoDxePump(&Driver)==EFI_ABORTED&&PianoPogoDxeStop(&Driver)==EFI_ABORTED&&Calls==Before);return;}}
  else {assert(S==EFI_SUCCESS&&Reads==1&&Driver.Report.Frames==1&&Notices==1&&Signals>=2);assert(Driver.Adapter.TextEx.ReadKeyStrokeEx(&Driver.Adapter.TextEx,&K)==EFI_SUCCESS&&K.Key.ScanCode==SCAN_F12);}
  if(C==4){UINTN Before=Reads;Notify(Driver.Wait[0]);assert(Reads==Before);}
  if(C==18){EFI_SIMPLE_POINTER_STATE P;assert(Driver.Adapter.Pointer.GetState(&Driver.Adapter.Pointer,&P)==EFI_SUCCESS&&P.RelativeMovementX==1&&P.RelativeMovementY==2&&P.RelativeMovementZ==3);}
  if(C==19){Exit();UINTN Before=Calls;Notify(Driver.Timer);assert(PianoPogoDxePump(&Driver)==EFI_ABORTED&&PianoPogoDxeStop(&Driver)==EFI_ABORTED&&Calls==Before);return;}
  S=PianoPogoDxeStop(&Driver);
  if(C==12||C==13||C==14||C==15){assert(S!=EFI_SUCCESS&&Driver.Report.Retained);UINTN Before=Calls+Stops;assert(PianoPogoDxeStop(&Driver)!=EFI_SUCCESS);if(C==15)assert(Calls+Stops==Before);return;}
  assert(S==EFI_SUCCESS&&Stops==1&&Uninstalls==1&&Closes==6&&Driver.Report.Stopped&&!Driver.Report.Published&&!Driver.Report.Retained);
  UINTN Before=Calls;assert(PianoPogoDxeStop(&Driver)==EFI_SUCCESS&&Calls==Before);assert(Driver.Adapter.TextEx.ReadKeyStrokeEx(&Driver.Adapter.TextEx,&K)==EFI_NOT_READY);
}
int main(void){for(UINTN C=0;C<23;C++){pid_t P=fork();assert(P>=0);if(!P){Run(C);_exit(0);}int S;waitpid(P,&S,0);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"pogo producer case %llu failed\n",(unsigned long long)C);return 1;}}puts("Actual pogo EFI producer: 23 fork readiness/publish/APP report/timer-only/wait events/F12 callback/same+cross-instance ABA/checked-stop/warnings/EBS cases passed; synthetic host backend only, no real wire input");return 0;}
