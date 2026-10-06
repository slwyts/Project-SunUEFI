// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the actual Core display callbacks; unrelated owner code is GC'd.
#define MDEPKG_NDEBUG
#include "../bootprofiles/uefi-app/PianoProductCore.c"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

EFI_BOOT_SERVICES *gBS;
EFI_SYSTEM_TABLE *gST;
EFI_RUNTIME_SERVICES *gRT;
EFI_GUID gEfiEventExitBootServicesGuid={.Data1=1};
EFI_GUID gEfiGraphicsOutputProtocolGuid={.Data1=2};
STATIC EFI_BOOT_SERVICES Services;
STATIC EFI_SYSTEM_TABLE System;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;
STATIC jmp_buf Halt;
STATIC UINT32 Creates,Closes,Locates,Paints,Inject;
STATIC EFI_STATUS CreateStatus,PaintStatus;
STATIC BOOLEAN Down;
STATIC UINT64 Counter,LastElapsed;
STATIC EFI_EVENT_NOTIFY Notify;
STATIC UINT32 ObserveOrder,SmmuCalls,MappingCalls;
STATIC BOOLEAN RetainedSmmu,RetainedMapping;
STATIC UINT32 ObserveInject;

BOOLEAN PianoProductDisplayRetained(VOID){return FALSE;}
BOOLEAN PianoDisplaySmmuRetained(VOID){return RetainedSmmu;}
BOOLEAN PianoFrameBufferMappingRetained(VOID){return RetainedMapping;}
UINTN EFIAPI AsciiSPrint(CHAR8 *Buffer,UINTN Size,CONST CHAR8 *Format,...){
  (VOID)Format;assert(Size==32);strcpy(Buffer,"pre:ClockDxe");return strlen(Buffer);
}
EFI_STATUS PianoDisplaySmmuObserve(CONST CHAR8 *Phase,PIANO_DISPLAY_SMMU_ALIVE Alive){
  assert(Phase&&Alive()&&ObserveOrder==0);ObserveOrder=1;++SmmuCalls;
  if(ObserveInject==1)Notify((EFI_EVENT)(UINTN)1,NULL);
  if(ObserveInject==2)RetainedSmmu=TRUE;
  return RetainedSmmu?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
EFI_STATUS PianoFrameBufferMappingObserve(CONST CHAR8 *Phase,PIANO_FB_MAPPING_ALIVE Alive){
  assert(Phase&&Alive()&&ObserveOrder==1);ObserveOrder=2;++MappingCalls;
  if(ObserveInject==3)Notify((EFI_EVENT)(UINTN)1,NULL);
  if(ObserveInject==4)RetainedMapping=TRUE;
  return RetainedMapping?EFI_DEVICE_ERROR:EFI_SUCCESS;
}

VOID EFIAPI CpuDeadLoop(VOID){longjmp(Halt,1);}
UINT64 EFIAPI GetPerformanceCounter(VOID){UINT64 Value=Counter;Counter=Down?Counter-100:Counter+100;return Value;}
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *First,UINT64 *Last){*First=Down?MAX_UINT64:0;*Last=Down?0:MAX_UINT64;return 1000000;}
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 Ticks){return Ticks*1000;}
STATIC EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Callback,CONST VOID *Context,CONST EFI_GUID *Guid,EFI_EVENT *Event){
  assert(Type==EVT_NOTIFY_SIGNAL&&Tpl==TPL_NOTIFY&&Callback&&Guid&&!Context);
  ++Creates;Notify=Callback;*Event=(EFI_EVENT)(UINTN)1;return CreateStatus;
}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Interface){
  assert(Guid&&!Registration);++Locates;*Interface=&Gop;if(Inject==1)Notify((EFI_EVENT)(UINTN)1,NULL);return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Close(EFI_EVENT Event){assert(Event==(EFI_EVENT)(UINTN)1);++Closes;if(Inject==3)Notify(Event,NULL);return EFI_SUCCESS;}
EFI_STATUS PianoProductBootLogInitialize(EFI_GRAPHICS_OUTPUT_PROTOCOL *Display,PIANO_PRODUCT_BOOT_LOG_ALIVE Alive){
  assert(Display==&Gop&&Alive());if(Inject==2)Notify((EFI_EVENT)(UINTN)1,NULL);return PaintStatus;
}
EFI_STATUS PianoProductBootLogStage(CONST CHAR8 *Name,EFI_STATUS Status,UINT64 ElapsedMs){
  assert(Name);(VOID)Status;++Paints;LastElapsed=ElapsedMs;if(Inject==4)Notify((EFI_EVENT)(UINTN)1,NULL);return PaintStatus;
}
STATIC VOID Reset(VOID){
  memset(&Services,0,sizeof(Services));memset(&System,0,sizeof(System));
  Services.CreateEventEx=Create;Services.LocateProtocol=Locate;Services.CloseEvent=Close;
  gBS=&Services;gST=&System;System.BootServices=gBS;
  memset(&mOwners,0,sizeof(mOwners));mBootLogExited=FALSE;mBootLogExitEvent=NULL;mBootLogEnabled=FALSE;
  Creates=Closes=Locates=Paints=Inject=0;CreateStatus=PaintStatus=EFI_SUCCESS;Down=FALSE;Counter=1000000;Notify=NULL;
  ObserveOrder=SmmuCalls=MappingCalls=ObserveInject=0;RetainedSmmu=RetainedMapping=FALSE;
}
int main(VOID){
  Reset();assert(!setjmp(Halt));BootLogStart();assert(Creates==1&&Locates==1&&Paints==2&&mBootLogEnabled);
  BootLogReturned();assert(Closes==1&&!mBootLogExitEvent&&!mBootLogEnabled);BootLogReturned();assert(Closes==1);
  Reset();Down=TRUE;assert(!setjmp(Halt));BootLogStart();BootLogStage("INPUT",EFI_SUCCESS);assert(LastElapsed==0);BootLogReturned();
  CONST EFI_STATUS Failures[]={EFI_DEVICE_ERROR,EFI_WARN_UNKNOWN_GLYPH};
  for(UINTN I=0;I<ARRAY_SIZE(Failures);++I){
    Reset();CreateStatus=Failures[I];assert(!setjmp(Halt));BootLogStart();assert(Creates==1&&Closes==1&&!Locates&&!mBootLogEnabled);
    Reset();PaintStatus=Failures[I];assert(!setjmp(Halt));BootLogStart();assert(!mBootLogEnabled&&!Paints);BootLogReturned();assert(Closes==1);
    Reset();assert(!setjmp(Halt));BootLogStart();PaintStatus=Failures[I];BootLogStage("USB",EFI_SUCCESS);assert(!mBootLogEnabled);BootLogReturned();assert(Closes==1);
  }
  for(UINT32 Case=1;Case<=4;++Case){
    Reset();if(Case<=2)Inject=Case;
    if(!setjmp(Halt)){
      BootLogStart();Inject=Case;
      if(Case==3)BootLogReturned();else if(Case==4)BootLogStage("USB",EFI_SUCCESS);
      assert(!"lost BootServices must halt before the caller resumes");
    }
    assert(mBootLogExited);assert(Closes==(Case==3?1:0));
  }
  Reset();assert(!setjmp(Halt));BootLogStart();ObserveNative("ClockDxe",TRUE);
  assert(SmmuCalls==1&&MappingCalls==1&&ObserveOrder==2);BootLogReturned();
  for(UINT32 Case=1;Case<=4;++Case){
    Reset();assert(!setjmp(Halt));BootLogStart();ObserveInject=Case;
    if(!setjmp(Halt)){ObserveNative("ClockDxe",TRUE);assert(!"observer lifetime/retention must halt");}
    assert(SmmuCalls==1&&MappingCalls==(Case>2));
    // Retention and EBS must not paint or close/call protocols before halt.
    assert(Paints==2&&Closes==0);
  }
  puts("Actual Core boot-log event lifetime, warning cleanup, counter direction and EBS post-call CPU fences passed");return 0;
}
