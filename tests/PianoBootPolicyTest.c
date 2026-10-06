// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual supervisor+FV loader, real EFI signatures; no devices or PE execution.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/mman.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoBootPolicy.h"
#include <Protocol/PianoProductIdle.h>
#include "../bootprofiles/uefi-app/PianoProductPayload.h"
#include <PiDxe.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/FirmwareVolume2.h>
#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiLoadedImageProtocolGuid={.Data1=1},gEfiFirmwareVolume2ProtocolGuid={.Data1=2},gEfiEventExitBootServicesGuid={.Data1=3},gEfiSimpleTextInputExProtocolGuid={.Data1=4};
EFI_GUID gEfiHiiDatabaseProtocolGuid={.Data1=5},gEfiHiiStringProtocolGuid={.Data1=6},gEfiHiiFontProtocolGuid={.Data1=7},gEfiHiiConfigRoutingProtocolGuid={.Data1=8},gEfiFormBrowser2ProtocolGuid={.Data1=9},gEdkiiFormDisplayEngineProtocolGuid={.Data1=10},gEfiVariableArchProtocolGuid={.Data1=11},gEfiVariableWriteArchProtocolGuid={.Data1=12};
static UINT32 scenario;static EFI_TPL tpl=TPL_APPLICATION;static PIANO_PRODUCT_RUNTIME_PROTOCOL *runtime;
static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *keyboard,*old_keyboard;static EFI_KEY_NOTIFY_FUNCTION key_callback,escape_callback,volume_callback;
static EFI_FIRMWARE_VOLUME2_PROTOCOL fv;static EFI_LOADED_IMAGE_PROTOCOL loaded;
static BOOLEAN image_live,keyboard_live=TRUE,loan,alive=TRUE;static UINT8 payload[4096];
static EFI_LOADED_IMAGE_PROTOCOL replacement_loaded;
static UINTN pumps,stops,starts,loads,unloads,acquires,validates,releases,key_reg,key_unreg,frees;
static UINT32 current_action,simple_runs;static VOID *image_base;static jmp_buf stop;
static BOOLEAN navigation;
static UINT64 window_clock;static UINTN window_stalls;
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *First,UINT64 *Last){*First=scenario==71?MAX_UINT64:0;*Last=scenario==71?0:MAX_UINT64;return scenario==74?0:1000000;}
UINT64 EFIAPI GetPerformanceCounter(VOID){return scenario==71?MAX_UINT64-window_clock:window_clock;}
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 Ticks){return Ticks*1000;}
BOOLEAN PianoSetStandardKeyNavigation(BOOLEAN Enable){BOOLEAN Previous=navigation;navigation=Enable;return Previous;}
static struct {EFI_EVENT_NOTIFY Notify;VOID *Context;BOOLEAN Live;EFI_GUID *Group;} events[16];
static struct {VOID *Pointer;UINTN Bytes;} pools[64];
static EFI_GUID idle_guid=PIANO_PRODUCT_IDLE_PROTOCOL_GUID;static PIANO_PRODUCT_IDLE_PROTOCOL *idle;
static EFI_GUID runtime_guid=PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID;
static VOID *pool(UINTN Bytes){for(UINTN I=0;I<64;++I)if(!pools[I].Pointer){VOID *P=calloc(1,Bytes);assert(P);pools[I].Pointer=P;pools[I].Bytes=Bytes;return P;}abort();}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID *EFIAPI CopyMem(VOID *A,CONST VOID *B,UINTN N){return memmove(A,B,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID EFIAPI CpuDeadLoop(VOID){longjmp(stop,1);}
static EFI_STATUS EFIAPI free_pool(VOID *P){
  ++frees;if(scenario==6 && current_action==2)return EFI_WARN_UNKNOWN_GLYPH;
  for(UINTN I=0;I<64;++I)if(pools[I].Pointer==P){free(P);pools[I].Pointer=NULL;return EFI_SUCCESS;}
  assert(!"unknown pool");return EFI_INVALID_PARAMETER;
}
static EFI_STATUS EFIAPI alloc(EFI_MEMORY_TYPE Type,UINTN Bytes,VOID **P){assert(Type==EfiLoaderData);*P=pool(Bytes);return EFI_SUCCESS;}
static EFI_TPL EFIAPI raise(EFI_TPL New){EFI_TPL Old=tpl;assert(New>=Old);tpl=New;return Old;}
static VOID EFIAPI restore(EFI_TPL Old){tpl=Old;}
static EFI_STATUS EFIAPI create_ex(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,CONST VOID *Context,CONST EFI_GUID *Group,EFI_EVENT *Out){
  assert(Type==EVT_NOTIFY_SIGNAL && Tpl==TPL_NOTIFY && Group==&gEfiEventExitBootServicesGuid);
  for(UINTN I=0;I<16;++I)if(!events[I].Live){events[I].Notify=Notify;events[I].Context=(VOID *)Context;events[I].Group=(EFI_GUID *)Group;events[I].Live=TRUE;*Out=&events[I];return EFI_SUCCESS;}
  abort();
}
static EFI_STATUS EFIAPI create(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,VOID *Context,EFI_EVENT *Out){
  assert(Type==EVT_NOTIFY_SIGNAL && Tpl==TPL_CALLBACK);
  for(UINTN I=0;I<16;++I)if(!events[I].Live){events[I].Notify=Notify;events[I].Context=Context;events[I].Live=TRUE;*Out=&events[I];return EFI_SUCCESS;}
  abort();
}
static EFI_STATUS EFIAPI close_event(EFI_EVENT Event){for(UINTN I=0;I<16;++I)if(Event==&events[I]){assert(events[I].Live);if(scenario==31 && starts)return EFI_WARN_UNKNOWN_GLYPH;events[I].Live=FALSE;events[I].Group=NULL;return EFI_SUCCESS;}abort();}
static EFI_STATUS EFIAPI notify(EFI_GUID *Guid,EFI_EVENT Event,VOID **Registration){assert(Guid==&gEfiSimpleTextInputExProtocolGuid && Event);*Registration=(VOID *)321;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI install(EFI_HANDLE *Handle,EFI_GUID *Guid,EFI_INTERFACE_TYPE Type,VOID *Interface){
 assert(Type==EFI_NATIVE_INTERFACE);
 if(!memcmp(Guid,&idle_guid,sizeof(*Guid))){assert(runtime&&!idle&&*Handle==(VOID*)100);idle=Interface;return scenario==84?EFI_WARN_STALE_DATA:scenario==85?EFI_DEVICE_ERROR:EFI_SUCCESS;}
 assert(!memcmp(Guid,&runtime_guid,sizeof(*Guid))&&!runtime);runtime=Interface;*Handle=(VOID*)100;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI uninstall(EFI_HANDLE Handle,EFI_GUID *Guid,VOID *Interface){
 assert(Handle==(VOID*)100);
 if(!memcmp(Guid,&idle_guid,sizeof(*Guid))){assert(Interface==idle&&runtime);if(scenario==86)return EFI_WARN_STALE_DATA;idle=NULL;return EFI_SUCCESS;}
 assert(Interface==runtime&&!idle&&!memcmp(Guid,&runtime_guid,sizeof(*Guid)));runtime=NULL;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI locate(EFI_GUID *Guid,VOID *Registration,VOID **Out){
  if(!memcmp(Guid,&idle_guid,sizeof(*Guid))){if(scenario==87){*Out=(VOID*)123;return EFI_SUCCESS;}if(idle){*Out=idle;return EFI_SUCCESS;}return EFI_NOT_FOUND;}
  if(!memcmp(Guid,&runtime_guid,sizeof(*Guid))){if(scenario==8){*Out=(VOID *)111;return EFI_SUCCESS;}if(runtime){*Out=runtime;return EFI_SUCCESS;}return EFI_NOT_FOUND;}
  if(scenario==12 && Guid==&gEfiHiiFontProtocolGuid)return EFI_NOT_FOUND;
  if(scenario==22 && (Guid==&gEfiVariableArchProtocolGuid || Guid==&gEfiVariableWriteArchProtocolGuid)){*Out=NULL;return EFI_SUCCESS;}
  *Out=(VOID *)123;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI locate_handles(EFI_LOCATE_SEARCH_TYPE Type,EFI_GUID *Guid,VOID *Key,UINTN *Count,EFI_HANDLE **Out){
  assert(Type==ByProtocol);*Count=0;*Out=NULL;
  if(Guid==&gEfiSimpleTextInputExProtocolGuid){if(!keyboard_live)return EFI_NOT_FOUND;if(scenario==13){*Count=1;return EFI_SUCCESS;}*Count=1;*Out=pool(sizeof(EFI_HANDLE));(*Out)[0]=(VOID *)200;return EFI_SUCCESS;}
  assert(Guid==&gEfiFirmwareVolume2ProtocolGuid);
  if(scenario==19){*Count=1;return EFI_SUCCESS;}
  if(scenario==20){*Out=pool(sizeof(EFI_HANDLE));return EFI_SUCCESS;}
  if(scenario==21){*Count=257;*Out=pool(sizeof(EFI_HANDLE));return EFI_SUCCESS;}
  *Count=1;*Out=pool(sizeof(EFI_HANDLE));(*Out)[0]=(VOID *)300;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI handle(EFI_HANDLE Handle,EFI_GUID *Guid,VOID **Out){
  *Out=NULL;
  if(Guid==&gEfiSimpleTextInputExProtocolGuid){assert(Handle==(VOID *)200);if(!keyboard_live)return EFI_UNSUPPORTED;if(scenario==26 && key_reg)return EFI_SUCCESS;*Out=keyboard;return EFI_SUCCESS;}
  if(Guid==&gEfiFirmwareVolume2ProtocolGuid){assert(Handle==(VOID *)300);*Out=&fv;return EFI_SUCCESS;}
  assert(Guid==&gEfiLoadedImageProtocolGuid && Handle==(VOID *)400);
  if(!image_live)return EFI_INVALID_PARAMETER;
  *Out=scenario==32 && starts?&replacement_loaded:&loaded;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI register_key(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,EFI_KEY_DATA *Key,EFI_KEY_NOTIFY_FUNCTION Notify,VOID **Token){
  assert(This==keyboard && (Key->Key.ScanCode==SCAN_F12 || Key->Key.ScanCode==SCAN_ESC || Key->Key.ScanCode==SCAN_UP || Key->Key.ScanCode==SCAN_DOWN) && !Key->Key.UnicodeChar);
  if(Key->Key.ScanCode==SCAN_F12)key_callback=Notify;
  else if(Key->Key.ScanCode==SCAN_ESC){assert(scenario>=64);escape_callback=Notify;}
  else {assert(scenario>=89&&navigation);volume_callback=Notify;}
  *Token=(VOID *)++key_reg;
  if(Key->Key.ScanCode==SCAN_DOWN&&scenario==104){*Token=NULL;return EFI_DEVICE_ERROR;}
  if(Key->Key.ScanCode==SCAN_ESC&&scenario==69)return EFI_WARN_UNKNOWN_GLYPH;
  if(Key->Key.ScanCode==SCAN_ESC&&(scenario==78||scenario==79)){*Token=NULL;return scenario==78?EFI_DEVICE_ERROR:EFI_SUCCESS;}
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI register_uncertain(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,EFI_KEY_DATA *Key,EFI_KEY_NOTIFY_FUNCTION Notify,VOID **Token){
  assert(This==keyboard && Key->Key.ScanCode==SCAN_F12 && Notify);++key_reg;
  *Token=scenario==24?(VOID *)123:NULL;
  return scenario==23?EFI_WARN_UNKNOWN_GLYPH:scenario==24?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI changed_unregister(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,VOID *Token){assert(!"changed method must not retire former callback token");return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI unregister_key(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,VOID *Token){assert(This==keyboard && keyboard_live && Token);++key_unreg;return scenario==70&&(UINTN)Token==2?EFI_WARN_UNKNOWN_GLYPH:EFI_SUCCESS;}
static EFI_STATUS EFIAPI window_stall(UINTN Us){
  assert(Us==1000&&tpl==TPL_APPLICATION&&alive);++window_stalls;
  if(scenario!=68)window_clock+=Us;
  if(scenario==80&&window_stalls==2)window_clock=500;
  if(window_stalls==2){EFI_KEY_DATA Key={0};
    if(scenario==65){Key.Key.ScanCode=SCAN_F12;assert(key_callback(&Key)==EFI_SUCCESS);}
    if(scenario==66){Key.Key.ScanCode=SCAN_ESC;assert(escape_callback(&Key)==EFI_SUCCESS);}
    if(scenario==67)assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_RETURN_CORE)==EFI_SUCCESS);
    if(scenario==99){Key.Key.ScanCode=SCAN_F12;assert(key_callback(&Key)==EFI_SUCCESS);}
    if(scenario==100){Key.Key.ScanCode=SCAN_ESC;assert(escape_callback(&Key)==EFI_SUCCESS);}
    if(scenario==101||scenario==102){assert(navigation);Key.Key.ScanCode=scenario==101?SCAN_UP:SCAN_DOWN;assert(volume_callback(&Key)==EFI_SUCCESS);}
    if(scenario==72){for(UINTN I=0;I<16;++I)if(events[I].Live&&events[I].Group)events[I].Notify(&events[I],events[I].Context);alive=FALSE;assert(mprotect(gBS,4096,PROT_NONE)==0);}
    if(scenario==76||scenario==77){old_keyboard=keyboard;keyboard_live=FALSE;assert(mprotect(old_keyboard,4096,PROT_NONE)==0);
      if(scenario==77){keyboard=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(keyboard!=MAP_FAILED);keyboard->RegisterKeyNotify=register_key;keyboard->UnregisterKeyNotify=unregister_key;keyboard_live=TRUE;}}
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI read_section(CONST EFI_FIRMWARE_VOLUME2_PROTOCOL *This,CONST EFI_GUID *Guid,EFI_SECTION_TYPE Type,UINTN Instance,VOID **Out,UINTN *Bytes,UINT32 *Auth){
  assert(This==&fv && Type==EFI_SECTION_PE32 && !Instance);current_action=Guid->Data1==0x462caa21?2:3;*Bytes=4096;*Auth=scenario==34?EFI_AUTH_STATUS_TEST_FAILED:0;*Out=pool(*Bytes);memcpy(*Out,payload,*Bytes);return scenario==35?EFI_WARN_UNKNOWN_GLYPH:EFI_SUCCESS;
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI DevicePathFromHandle(EFI_HANDLE Handle){static EFI_DEVICE_PATH_PROTOCOL End={END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};assert(Handle==(VOID *)300);return &End;}
UINT16 EFIAPI SetDevicePathNodeLength(VOID *Node,UINTN Bytes){EFI_DEVICE_PATH_PROTOCOL *P=Node;P->Length[0]=(UINT8)Bytes;P->Length[1]=(UINT8)(Bytes>>8);return (UINT16)Bytes;}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI AppendDevicePathNode(CONST EFI_DEVICE_PATH_PROTOCOL *Base,CONST EFI_DEVICE_PATH_PROTOCOL *Node){assert(Base && Node && Node->SubType==MEDIA_PIWG_FW_FILE_DP);return pool(sizeof(MEDIA_FW_VOL_FILEPATH_DEVICE_PATH)+4);}
static EFI_STATUS EFIAPI load(BOOLEAN Policy,EFI_HANDLE Parent,EFI_DEVICE_PATH_PROTOCOL *Path,VOID *Source,UINTN Bytes,EFI_HANDLE *Out){
  assert(!Policy && Parent==(VOID *)555 && Bytes==4096 && tpl==TPL_APPLICATION);++loads;
  if(!Path){assert(loan && validates && Source==payload);current_action=1;}
  image_base=malloc(8192);assert(image_base);image_live=TRUE;
  loaded=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,.ImageCodeType=EfiLoaderCode,.ImageBase=image_base,.ImageSize=8192};*Out=(VOID *)400;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI start(EFI_HANDLE Handle,UINTN *Bytes,CHAR16 **Data){
  assert(Handle==(VOID *)400 && image_live && runtime && tpl==TPL_APPLICATION);++starts;*Bytes=0;*Data=NULL;
  assert(runtime->Pump(runtime,PIANO_PRODUCT_PUMP_GUI,1000)==EFI_SUCCESS);
  if(scenario==59){UINT32 A;UINT64 Seq,Next;assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_REQUEST_CONTINUE)==EFI_SUCCESS&&PianoBootPolicyReport()->RequestedCoreAction==PianoUsbServiceActionContinue);assert(runtime->GetPendingAction(runtime,&A,&Seq)==EFI_SUCCESS&&A==4);assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_REQUEST_CONTINUE)==EFI_SUCCESS&&runtime->GetPendingAction(runtime,&A,&Next)==EFI_SUCCESS&&Next==Seq);assert(runtime->RequestAction(runtime,5)==EFI_ACCESS_DENIED&&PianoBootPolicyReport()->RequestedCoreAction==PianoUsbServiceActionContinue);}
  if(scenario==61)assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_REQUEST_CONTINUE)==EFI_ACCESS_DENIED&&PianoBootPolicyReport()->RequestedCoreAction==PianoUsbServiceActionNone);
  if(scenario==62){assert(runtime->RequestAction(runtime,5)==EFI_SUCCESS&&runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_REQUEST_CONTINUE)==EFI_ACCESS_DENIED&&PianoBootPolicyReport()->RequestedCoreAction==PianoUsbServiceActionReboot);}
  if(scenario==63){((PIANO_BOOT_POLICY_REPORT*)PianoBootPolicyReport())->Sequence=MAX_UINT64;assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_REQUEST_CONTINUE)==EFI_OUT_OF_RESOURCES&&PianoBootPolicyReport()->RequestedCoreAction==PianoUsbServiceActionNone);}
  if(scenario>=55 && scenario<=58){
    if(starts==1){
      UINT32 A;UINT64 Seq,After;
      assert(runtime->GetPendingAction(runtime,&A,&Seq)==EFI_SUCCESS && A==0);
      assert(runtime->RequestAction(runtime,current_action)==EFI_SUCCESS && runtime->GetPendingAction(runtime,&A,&After)==EFI_SUCCESS && A==0 && After==Seq);
      UINT32 Next=scenario==55?2:3;assert(runtime->RequestAction(runtime,Next)==EFI_SUCCESS);
      assert(runtime->GetPendingAction(runtime,&A,&Seq)==EFI_SUCCESS && A==Next);
      assert(runtime->RequestAction(runtime,current_action)==EFI_SUCCESS && runtime->GetPendingAction(runtime,&A,&After)==EFI_SUCCESS && A==Next && After==Seq);
      assert(runtime->Pump(runtime,PIANO_PRODUCT_PUMP_WAIT_EVENT,1000)==EFI_ABORTED && !stops && PianoBootPolicyReport()->Usb.Started);
    }else if(starts==2 && scenario!=55){
      assert(current_action==3 && runtime->RequestAction(runtime,1)==EFI_SUCCESS);
      assert(runtime->Pump(runtime,PIANO_PRODUCT_PUMP_WAIT_EVENT,1000)==EFI_ABORTED && !stops && PianoBootPolicyReport()->Usb.Started);
    }
  }
  if(scenario==48 || scenario==49 || scenario==52 || scenario==54){
    if(scenario==52){EFI_KEY_DATA Key={0};Key.Key.ScanCode=SCAN_F12;assert(key_callback(&Key)==EFI_SUCCESS);}
    if(scenario==54)tpl=TPL_CALLBACK;
    UINT32 A;UINT64 Seq,After;
    assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_REQUEST_REBOOT)==EFI_SUCCESS && PianoBootPolicyReport()->RequestedCoreAction==PianoUsbServiceActionReboot);
    assert(runtime->GetPendingAction(runtime,&A,&Seq)==EFI_SUCCESS && A==PIANO_PRODUCT_ACTION_RETURN_CORE);
    assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_REQUEST_REBOOT)==EFI_SUCCESS && runtime->GetPendingAction(runtime,&A,&After)==EFI_SUCCESS && Seq==After);
    tpl=TPL_APPLICATION;
  }
  if(scenario==51){assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_REQUEST_REBOOT)==EFI_ACCESS_DENIED && PianoBootPolicyReport()->RequestedCoreAction==PianoUsbServiceActionNone);}
  if(scenario==53){((PIANO_BOOT_POLICY_REPORT *)PianoBootPolicyReport())->Sequence=MAX_UINT64;assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_REQUEST_REBOOT)==EFI_OUT_OF_RESOURCES && PianoBootPolicyReport()->RequestedCoreAction==PianoUsbServiceActionNone);}
  if(scenario==17)assert(PianoApplicationRunBuffer((VOID *)555,payload,4096,NULL,8192,(PIANO_FV_APPLICATION *)&PianoBootPolicyReport()->Application)==EFI_ALREADY_STARTED && image_live);
  if(current_action==1){++simple_runs;if(((scenario==1 || scenario==37) && simple_runs==1) || scenario==5 || scenario==6 || scenario==12){EFI_KEY_DATA Key={0};Key.Key.ScanCode=SCAN_F12;assert(key_callback(&Key)==EFI_SUCCESS);}}
  if(scenario==37 && current_action==2){EFI_KEY_DATA Key={0};Key.Key.ScanCode=SCAN_F12;assert(key_callback(&Key)==EFI_SUCCESS);UINT32 A;UINT64 Seq;assert(runtime->GetPendingAction(runtime,&A,&Seq)==EFI_SUCCESS && A==PIANO_PRODUCT_ACTION_NONE);}
  if(scenario>=38 && scenario<=41){UINT32 A;UINT64 Seq;assert(runtime->GetPendingAction(runtime,&A,&Seq)==EFI_SUCCESS && A==PIANO_PRODUCT_ACTION_RETURN_CORE);}
  if(scenario==5 && current_action==2){loaded.ImageBase=(VOID *)0x2222;return EFI_SUCCESS;}
  if(scenario==7){for(UINTN I=0;I<16;++I)if(events[I].Live && events[I].Group)events[I].Notify(&events[I],events[I].Context);alive=FALSE;assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_REQUEST_REBOOT)==EFI_ABORTED);assert(mprotect(gBS,4096,PROT_NONE)==0);return EFI_SUCCESS;}
  if(scenario==32){replacement_loaded=loaded;return EFI_SUCCESS;}
  if(scenario>=28 && scenario<=30)return EFI_SUCCESS;
  free(image_base);image_base=NULL;image_live=FALSE;return scenario==33?EFI_WARN_UNKNOWN_GLYPH:scenario==39 || (scenario==58 && starts==1)?EFI_ABORTED:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI unload(EFI_HANDLE Handle){assert(Handle==(VOID *)400 && image_live);++unloads;assert(!loaded.LoadOptions && !loaded.LoadOptionsSize);if(scenario==29)return EFI_WARN_UNKNOWN_GLYPH;if(scenario==30)return EFI_DEVICE_ERROR;free(image_base);image_base=NULL;image_live=FALSE;return EFI_SUCCESS;}
EFI_STATUS PianoProductAcquireSimpleInit(PIANO_PRODUCT_PAYLOAD_VIEW *V){++acquires;memset(V,0,sizeof(*V));if(scenario==2)return EFI_SECURITY_VIOLATION;assert(!loan);loan=TRUE;V->Image=payload;V->Bytes=sizeof(payload);V->Lease=(VOID *)123;return EFI_SUCCESS;}
EFI_STATUS PianoProductValidateSimpleInit(CONST PIANO_PRODUCT_PAYLOAD_VIEW *V){++validates;assert(loan && V->Image==payload && V->Lease==(VOID *)123);return scenario==3?EFI_MEDIA_CHANGED:EFI_SUCCESS;}
EFI_STATUS PianoProductReleaseSimpleInit(CONST PIANO_PRODUCT_PAYLOAD_VIEW *V){++releases;assert(loan && V->Lease==(VOID *)123 && !image_live);if(scenario==4)return EFI_DEVICE_ERROR;loan=FALSE;return EFI_SUCCESS;}
EFI_STATUS PianoUsbControllerServicePumpApp(UINT32 Reason,UINTN Budget){assert(tpl==TPL_APPLICATION && alive && Budget==1000);++pumps;if(scenario==11)assert(runtime->Pump(runtime,Reason,Budget)==EFI_NOT_READY);return scenario==14?EFI_UNSUPPORTED:EFI_SUCCESS;}
EFI_STATUS PianoUsbControllerServiceGetStatus(PIANO_DWC3_SERVICE_STATUS *S){
  *S=(PIANO_DWC3_SERVICE_STATUS){.Revision=scenario==47?2:1,.Started=scenario!=14,.Configured=TRUE,.Phase=PianoUsbServiceListening,.BulkActive=scenario==83};
  if((scenario==81||scenario==103)&&window_stalls>=2){S->Phase=PianoUsbServiceStopRequested;S->Action=PianoUsbServiceActionReboot;}
  if((scenario>=38 && scenario<=41) || scenario==44 || scenario==45 || scenario==46 || scenario==51 || scenario==61){S->Phase=PianoUsbServiceStopRequested;S->Action=scenario==46?PianoUsbServiceActionNone:scenario==51 || scenario==61?PianoUsbServiceActionBoot:scenario==44 || scenario==45?PianoUsbServiceActionReboot:(PIANO_USB_SERVICE_ACTION)(scenario-37);}
  return scenario==14?EFI_UNSUPPORTED:EFI_SUCCESS;
}
EFI_STATUS PianoUsbControllerServiceStop(EFI_STATUS Reason,PIANO_USB_SERVICE_RETIRE_REPORT *Report){(void)Reason;(void)Report;++stops;assert(!"UI return must not stop shared USB");return EFI_DEVICE_ERROR;}
int main(int argc,char **argv){
  assert(argc==2);scenario=(UINT32)strtoul(argv[1],NULL,10);assert(scenario<=106);
  gBS=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(gBS!=MAP_FAILED);
  *gBS=(EFI_BOOT_SERVICES){.RaiseTPL=raise,.RestoreTPL=restore,.CreateEventEx=create_ex,.CreateEvent=create,.CloseEvent=close_event,
    .RegisterProtocolNotify=notify,.InstallProtocolInterface=install,.UninstallProtocolInterface=uninstall,.LocateProtocol=locate,
    .LocateHandleBuffer=locate_handles,.HandleProtocol=handle,.AllocatePool=alloc,.FreePool=free_pool,.LoadImage=load,.StartImage=start,.UnloadImage=unload,.Stall=window_stall};
  keyboard=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(keyboard!=MAP_FAILED);
  keyboard->RegisterKeyNotify=register_key;keyboard->UnregisterKeyNotify=unregister_key;fv.ReadSection=read_section;
  if(scenario>=23 && scenario<=25)keyboard->RegisterKeyNotify=register_uncertain;
  EFI_STATUS S=PianoBootPolicyInitialize((VOID *)555);
  if(scenario==84||scenario==85){assert(S==(scenario==84?EFI_DEVICE_ERROR:EFI_DEVICE_ERROR)&&runtime&&idle&&PianoBootPolicyReport()->Retained&&!key_reg);assert(PianoBootPolicyStop()==EFI_ACCESS_DENIED);goto Done;}
  if(scenario==87){assert(S==EFI_ALREADY_STARTED&&!runtime&&!key_reg);goto Done;}
  if(scenario==8){assert(S==EFI_ALREADY_STARTED && !key_reg);goto Done;}
  if(scenario==13){assert(S==EFI_COMPROMISED_DATA && PianoBootPolicyReport()->Retained);goto Done;}
  if(scenario>=23 && scenario<=25){assert(S==EFI_COMPROMISED_DATA && PianoBootPolicyReport()->Retained && !starts);goto Done;}
  assert(S==EFI_SUCCESS && runtime && key_reg==1);
  if(scenario>=89) {
    CONST CHAR8 *Args="a=1 bootmonitor.bootmode=recovery b=2";UINTN Bytes=strlen(Args)+1;
    PIANO_BOOT_ENTRY Expected=PianoBootEntryRecovery;
    if(scenario==90){Args="bootmonitor.bootmode=normal";Bytes=strlen(Args)+1;Expected=PianoBootEntryNormal;}
    if(scenario==91){Args=NULL;Bytes=0;Expected=PianoBootEntryUnknown;}
    if(scenario==92){Args="xbootmonitor.bootmode=recovery";Bytes=strlen(Args)+1;Expected=PianoBootEntryUnknown;}
    if(scenario==93){Args="bootmonitor.bootmode=recovery bootmonitor.bootmode=recovery";Bytes=strlen(Args)+1;Expected=PianoBootEntryUnknown;}
    if(scenario==94){Args="bootmonitor.bootmode=recovery bootmonitor.bootmode=normal";Bytes=strlen(Args)+1;Expected=PianoBootEntryUnknown;}
    if(scenario==95){Args="bootmonitor.bootmode=recovery_extra";Bytes=strlen(Args)+1;Expected=PianoBootEntryUnknown;}
    if(scenario==96){Bytes=strlen(Args);Expected=PianoBootEntryUnknown;}
    if(scenario==97){Args="bootmonitor.bootmode=recovery\0hidden";Bytes=sizeof("bootmonitor.bootmode=recovery\0hidden");Expected=PianoBootEntryUnknown;}
    if(scenario==98){Args=(CONST CHAR8 *)1;Bytes=8193;Expected=PianoBootEntryUnknown;}
    if(scenario==105){Args="\tbootmonitor.bootmode=recovery\r\n";Bytes=strlen(Args)+1;}
    assert(PianoBootPolicySetEntryBootArgs(Args,Bytes)==EFI_SUCCESS);
    assert(PianoBootPolicyReport()->Entry==Expected);
    assert(PianoBootPolicyReport()->StartupDefaultAction==(Expected==PianoBootEntryRecovery?PIANO_PRODUCT_ACTION_REQUEST_BOOT_STABLE:PIANO_PRODUCT_ACTION_SIMPLEINIT));
    if(scenario==106){EFI_KEY_DATA Key={0};Key.Key.ScanCode=SCAN_F12;assert(key_callback(&Key)==EFI_SUCCESS);}
    S=PianoBootPolicyStartupWindow(3000);
    assert(!navigation&&!starts&&!loads&&!stops);
    if(scenario==104){assert(S==EFI_DEVICE_ERROR&&!PianoBootPolicyReport()->Retained&&key_reg==4&&key_unreg==2);assert(PianoBootPolicyStop()==EFI_SUCCESS&&key_unreg==3);goto Done;}
    assert(S==EFI_SUCCESS);
    UINT32 A;UINT64 Seq;assert(runtime->GetPendingAction(runtime,&A,&Seq)==EFI_SUCCESS);
    BOOLEAN Auto=Expected==PianoBootEntryRecovery&&scenario<99;
    Auto=Auto||scenario==105;
    assert(A==(Auto?PIANO_PRODUCT_ACTION_RETURN_CORE:scenario==99||scenario==106?PIANO_PRODUCT_ACTION_SETUP:scenario>=100&&scenario<=102?PIANO_PRODUCT_ACTION_SIMPLEINIT:scenario==103?PIANO_PRODUCT_ACTION_RETURN_CORE:PIANO_PRODUCT_ACTION_NONE));
    assert(PianoBootPolicyReport()->RequestedCoreAction==(Auto?PianoUsbServiceActionBoot:PianoUsbServiceActionNone));
    assert(PianoBootPolicySetEntryBootArgs(Args,Bytes)==EFI_ACCESS_DENIED);
    if(Auto){assert(window_stalls==3000&&pumps==3001);assert(PianoBootPolicyRun()==EFI_END_OF_FILE&&!starts&&!stops);assert(PianoBootPolicyCancelStable()==EFI_SUCCESS);assert(runtime->GetPendingAction(runtime,&A,&Seq)==EFI_SUCCESS&&A==PIANO_PRODUCT_ACTION_SIMPLEINIT);}
    assert(PianoBootPolicyStop()==EFI_SUCCESS);
    assert(key_unreg==key_reg&&key_reg==(scenario==106?1:Expected==PianoBootEntryRecovery?4:2));goto Done;
  }
  if(scenario==88) {
    UINT32 A;UINT64 Seq,Again;
    assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_REQUEST_BOOT_STABLE)==EFI_SUCCESS);
    assert(runtime->GetPendingAction(runtime,&A,&Seq)==EFI_SUCCESS&&A==PIANO_PRODUCT_ACTION_RETURN_CORE);
    assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_REQUEST_BOOT_STABLE)==EFI_SUCCESS);
    assert(runtime->GetPendingAction(runtime,&A,&Again)==EFI_SUCCESS&&Again==Seq);
    assert(PianoBootPolicyReport()->RequestedCoreAction==PianoUsbServiceActionBoot);
    assert(PianoBootPolicyCancelStable()==EFI_ACCESS_DENIED);
    assert(PianoBootPolicyRun()==EFI_END_OF_FILE&&!starts&&!stops);
    assert(PianoBootPolicyCancelStable()==EFI_SUCCESS);
    assert(PianoBootPolicyReport()->RequestedCoreAction==PianoUsbServiceActionNone);
    assert(runtime->GetPendingAction(runtime,&A,&Again)==EFI_SUCCESS&&A==PIANO_PRODUCT_ACTION_SIMPLEINIT&&Again>Seq);
    assert(PianoBootPolicyStop()==EFI_SUCCESS);goto Done;
  }
  if(scenario>=82){
    UINT64 Sample=0;BOOLEAN Active=TRUE;assert(idle&&idle->Runtime==runtime&&idle->Read(idle,runtime,&Sample,&Active)==EFI_NOT_READY);
    assert(runtime->Pump(runtime,PIANO_PRODUCT_PUMP_WAIT_EVENT,1000)==EFI_SUCCESS);
    assert(idle->Read(idle,runtime,&Sample,&Active)==EFI_SUCCESS&&Sample==1&&Active==(scenario==83));
    if(scenario==86){assert(PianoBootPolicyStop()==EFI_DEVICE_ERROR&&PianoBootPolicyReport()->Retained&&runtime&&idle);goto Done;}
    assert(PianoBootPolicyStop()==EFI_SUCCESS&&!runtime&&!idle&&!stops);goto Done;
  }
  if(scenario>=64){
    if(scenario==75){tpl=TPL_CALLBACK;assert(PianoBootPolicyStartupWindow(3000)==EFI_UNSUPPORTED);tpl=TPL_APPLICATION;}
    if(scenario==73){EFI_KEY_DATA Key={0};Key.Key.ScanCode=SCAN_F12;assert(key_callback(&Key)==EFI_SUCCESS);}
    BOOLEAN Fatal=FALSE;if(!setjmp(stop))S=PianoBootPolicyStartupWindow(3000);else Fatal=TRUE;
    if(scenario==72){assert(Fatal&&PianoBootPolicyReport()->ServicesLost&&!starts&&!stops);goto Done;}
    assert(!Fatal&&!loads&&!starts&&!stops);
    if(scenario==69||scenario==70||scenario==79){assert(S==EFI_DEVICE_ERROR||S==EFI_COMPROMISED_DATA);assert(PianoBootPolicyReport()->Retained&&PianoBootPolicyStop()==EFI_ACCESS_DENIED);goto Done;}
    if(scenario==78){assert(S==EFI_DEVICE_ERROR&&!PianoBootPolicyReport()->Retained&&!key_unreg&&key_reg==2);assert(PianoBootPolicyStop()==EFI_SUCCESS&&key_unreg==1);goto Done;}
    if(scenario==68)assert(S==EFI_TIMEOUT&&window_stalls==3100&&pumps==3100);
    else if(scenario==74){assert(S==EFI_UNSUPPORTED&&!pumps&&!window_stalls&&!key_unreg);assert(PianoBootPolicyStop()==EFI_SUCCESS);goto Done;}
    else if(scenario==80)assert(S==EFI_COMPROMISED_DATA&&!PianoBootPolicyReport()->Retained);
    else assert(S==EFI_SUCCESS);
    UINT32 A;UINT64 Seq;assert(runtime->GetPendingAction(runtime,&A,&Seq)==EFI_SUCCESS);
    assert(A==(scenario==65||scenario==73?2:scenario==66?1:scenario==67||scenario==81?4:0));
    if(scenario==73)assert(!pumps&&!window_stalls&&key_reg==1&&!key_unreg);
    else if(scenario==76)assert(pumps>0&&key_reg==2&&!key_unreg);
    else if(scenario==77)assert(pumps>0&&key_reg==4&&key_unreg==1);
    else assert(pumps>0&&window_stalls>0&&key_reg==2&&key_unreg==1);
    if(escape_callback){EFI_KEY_DATA Key={0};Key.Key.ScanCode=SCAN_ESC;assert(escape_callback(&Key)==EFI_SUCCESS);UINT32 After;UINT64 Next;assert(runtime->GetPendingAction(runtime,&After,&Next)==EFI_SUCCESS&&After==A&&Next==Seq);}
    assert(PianoBootPolicyStartupWindow(3000)==EFI_ACCESS_DENIED);
    assert(PianoBootPolicyStop()==EFI_SUCCESS);assert(key_unreg==(scenario==73?1:scenario==76?0:2));
    if(scenario==77){assert(munmap(old_keyboard,4096)==0);}goto Done;
  }
  if(scenario==60)assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_REQUEST_CONTINUE)==EFI_ACCESS_DENIED&&PianoBootPolicyReport()->RequestedCoreAction==PianoUsbServiceActionNone);
  if(scenario==50)assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_REQUEST_REBOOT)==EFI_ACCESS_DENIED && PianoBootPolicyReport()->RequestedCoreAction==PianoUsbServiceActionNone);
  if(scenario==26 || scenario==27){
    if(scenario==27)keyboard->UnregisterKeyNotify=changed_unregister;
    assert(runtime->Pump(runtime,PIANO_PRODUCT_PUMP_APP,1000)==EFI_COMPROMISED_DATA && PianoBootPolicyReport()->Retained && !pumps && !key_unreg);
    assert(PianoBootPolicyStop()==EFI_ACCESS_DENIED);goto Done;
  }
  if(scenario==47){assert(runtime->Pump(runtime,PIANO_PRODUCT_PUMP_APP,1000)==EFI_COMPROMISED_DATA && !PianoBootPolicyReport()->Usb.Started && PianoBootPolicyReport()->LastPump==EFI_COMPROMISED_DATA);assert(PianoBootPolicyStop()==EFI_SUCCESS && !stops);goto Done;}
  if(scenario==9){tpl=TPL_CALLBACK;assert(runtime->Pump(runtime,PIANO_PRODUCT_PUMP_GUI,1000)==EFI_UNSUPPORTED && !pumps);tpl=TPL_APPLICATION;}
  else if(scenario==10){
    tpl=TPL_CALLBACK;UINT32 A;UINT64 Seq,New;
    assert(runtime->RequestAction(runtime,2)==EFI_SUCCESS && runtime->GetPendingAction(runtime,&A,&Seq)==EFI_SUCCESS && A==2);
    assert(runtime->RequestAction(runtime,3)==EFI_SUCCESS && runtime->GetPendingAction(runtime,&A,&New)==EFI_SUCCESS && A==3 && New>Seq);
    assert(runtime->AckAction(runtime,Seq)==EFI_NOT_READY && runtime->GetPendingAction(runtime,&A,&New)==EFI_SUCCESS && A==3);
    assert(runtime->AckAction(runtime,New)==EFI_SUCCESS);tpl=TPL_APPLICATION;
  } else if(scenario==14){assert(runtime->Pump(runtime,PIANO_PRODUCT_PUMP_APP,1000)==EFI_UNSUPPORTED && !PianoBootPolicyReport()->Usb.Started);}
  else if(scenario==11){assert(runtime->Pump(runtime,PIANO_PRODUCT_PUMP_APP,1000)==EFI_SUCCESS && pumps==1);}
  else {
    BOOLEAN Fatal=FALSE;
    if(scenario==42 || scenario==43){
      assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_RETURN_CORE)==EFI_SUCCESS);
      assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_SETUP)==EFI_ACCESS_DENIED);
      assert(runtime->RequestAction(runtime,PIANO_PRODUCT_ACTION_NONE)==EFI_INVALID_PARAMETER && runtime->RequestAction(runtime,8)==EFI_INVALID_PARAMETER);
      if(scenario==43){tpl=TPL_CALLBACK;assert(PianoBootPolicyDispatchPending()==EFI_UNSUPPORTED && !starts && !stops);tpl=TPL_APPLICATION;}
    }
    if(scenario==44 || scenario==45){
      assert(runtime->Pump(runtime,PIANO_PRODUCT_PUMP_APP,1000)==EFI_SUCCESS);
      if(scenario==45){UINT32 A;UINT64 Seq,After;assert(runtime->GetPendingAction(runtime,&A,&Seq)==EFI_SUCCESS && A==PIANO_PRODUCT_ACTION_RETURN_CORE);
        assert(runtime->Pump(runtime,PIANO_PRODUCT_PUMP_APP,1000)==EFI_SUCCESS && runtime->GetPendingAction(runtime,&A,&After)==EFI_SUCCESS && After==Seq && A==PIANO_PRODUCT_ACTION_RETURN_CORE);}
    }
    if(scenario==18 || (scenario>=19 && scenario<=22) || scenario==28 || scenario==34 || scenario==35 || scenario==49 || scenario==57){EFI_KEY_DATA Key={0};Key.Key.ScanCode=SCAN_F12;assert(key_callback(&Key)==EFI_SUCCESS);}
    if(!setjmp(stop))S=PianoBootPolicyRun();else Fatal=TRUE;
    if(scenario==58){assert(!Fatal && S==EFI_ABORTED && !stops && PianoBootPolicyReport()->Usb.Started);assert(runtime->Pump(runtime,PIANO_PRODUCT_PUMP_APP,1000)==EFI_SUCCESS);S=PianoBootPolicyRun();}
    if(scenario==4 || scenario==5 || scenario==6 || scenario==7 || (scenario>=29 && scenario<=32)){assert(Fatal && (PianoBootPolicyReport()->Retained || scenario==7));if(scenario==29 || scenario==30)assert(loan && !releases && image_live);goto Done;}
    assert(!Fatal);
    if(scenario==2){assert(S==EFI_SECURITY_VIOLATION && !loads && !releases);}
    else if(scenario==3){assert(S==EFI_MEDIA_CHANGED && !loads && releases==1);}
    else if(scenario==12){assert(S==EFI_NOT_FOUND && starts==1);}
    else if(scenario>=19 && scenario<=21){assert(S==EFI_COMPROMISED_DATA && !starts && !loads && !loan);}
    else if(scenario==33){assert(S==EFI_DEVICE_ERROR && releases==1 && !loan && !image_live);}
    else if(scenario==34 || scenario==35){assert(S==(scenario==34?EFI_SECURITY_VIOLATION:EFI_NOT_FOUND) && !loads && !starts);}
    else if(scenario>=38 && scenario<=45){assert(S==EFI_END_OF_FILE && starts==(scenario<=41?1:0) && PianoBootPolicyReport()->AppRuns==starts && !loan && releases==acquires && !stops);}
    else if(scenario==48 || scenario==49 || scenario==51 || scenario==52 || scenario==54){assert(S==EFI_END_OF_FILE && starts==1 && PianoBootPolicyReport()->AppRuns==1 && !loan && releases==acquires && !stops && PianoBootPolicyReport()->RequestedCoreAction==(scenario==51?PianoUsbServiceActionNone:PianoUsbServiceActionReboot));}
    else if(scenario>=55 && scenario<=58){assert(S==EFI_SUCCESS && starts==3 && !loan && releases==acquires && !stops && PianoBootPolicyReport()->Usb.Started && PianoBootPolicyReport()->RequestedCoreAction==PianoUsbServiceActionNone);}
    else if(scenario==59 || scenario==61 || scenario==62){assert(S==EFI_END_OF_FILE&&starts==1&&!stops&&!loan&&PianoBootPolicyReport()->RequestedCoreAction==(scenario==59?PianoUsbServiceActionContinue:scenario==62?PianoUsbServiceActionReboot:PianoUsbServiceActionNone));}
    else {assert(S==EFI_SUCCESS && !loan && releases==acquires && unloads==(scenario==28?2:0));assert(starts==(scenario==1 || scenario==37?3:scenario==18 || scenario==22 || scenario==28?2:1));}
    if(scenario==36){
      PIANO_FV_APPLICATION Context={0};UINTN Before=starts;
      loan=TRUE;assert(PianoApplicationRunBuffer((VOID *)555,payload,sizeof(payload),NULL,8192,&Context)==EFI_SUCCESS);loan=FALSE;
      PIANO_FV_APPLICATION Saved=Context;
      assert(PianoApplicationRunBuffer((VOID *)555,payload,sizeof(payload),NULL,0,&Context)==EFI_INVALID_PARAMETER && !memcmp(&Saved,&Context,sizeof(Context)) && starts==Before+1);
    }
  }
  // Uninstall/replacement with inaccessible old protocol object must never be
  // dereferenced by APP refresh or Stop.
  old_keyboard=keyboard;keyboard_live=FALSE;assert(mprotect(old_keyboard,4096,PROT_NONE)==0);
  if(scenario==15 || scenario==16){
    if(scenario==16){assert(runtime->Pump(runtime,PIANO_PRODUCT_PUMP_APP,1000)==EFI_SUCCESS);assert(PianoBootPolicyReport()->KeyboardProviders==0);}
    keyboard=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(keyboard!=MAP_FAILED);
    keyboard->RegisterKeyNotify=register_key;keyboard->UnregisterKeyNotify=unregister_key;keyboard_live=TRUE;
    if(scenario==16)for(UINTN I=0;I<16;++I)if(events[I].Live && !events[I].Group)events[I].Notify(&events[I],events[I].Context);
    assert(runtime->Pump(runtime,PIANO_PRODUCT_PUMP_APP,1000)==EFI_SUCCESS && PianoBootPolicyReport()->KeyboardProviders==1 && key_reg==2);
    assert(PianoBootPolicyStop()==EFI_SUCCESS && key_unreg==1 && !stops);assert(munmap(old_keyboard,4096)==0);goto Done;
  }
  assert(runtime->Pump(runtime,PIANO_PRODUCT_PUMP_APP,1000)==(scenario==14?EFI_UNSUPPORTED:EFI_SUCCESS));
  assert(PianoBootPolicyReport()->KeyboardProviders==0 && !key_unreg);
  assert(PianoBootPolicyStop()==EFI_SUCCESS && !runtime && !key_unreg && !stops);
Done:
  if(!alive)assert(mprotect(gBS,4096,PROT_READ|PROT_WRITE)==0);
  free(image_base);for(UINTN I=0;I<64;++I)free(pools[I].Pointer);
  assert(munmap(keyboard,4096)==0 && munmap(gBS,4096)==0);
  printf("Product supervisor/FV actual source scenario %u passed\n",scenario);return 0;
}
