// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real EFI input producer lifecycle; hardware remains unbound until Ready.
#include "PianoPogoDxe.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Guid/EventGroup.h>
#define SIGNATURE SIGNATURE_32('P','P','D','X')
STATIC PIANO_POGO_DXE *mOwner;
STATIC UINTN mNotifySequence; // core/producer module lifetime, across driver objects
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC BOOLEAN Live(PIANO_POGO_DXE *D){return !D->Report.ServicesLost&&gST&&gBS&&gST->BootServices==gBS&&gBS->Hdr.Signature==EFI_BOOT_SERVICES_SIGNATURE;}
STATIC BOOLEAN Active(PIANO_POGO_DXE *D){return D&&D->Signature==SIGNATURE&&Live(D)&&D->Report.Published&&!D->Report.Stopping&&!D->Report.Retained;}
STATIC EFI_STATUS AtApp(PIANO_POGO_DXE *D){if(!Live(D))return EFI_ABORTED;EFI_TPL T=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(T);if(!Live(D))return EFI_ABORTED;return T==TPL_APPLICATION?EFI_SUCCESS:EFI_UNSUPPORTED;}
STATIC EFI_STATUS Retain(PIANO_POGO_DXE *D,EFI_STATUS S){D->Report.Retained=TRUE;D->Report.WorkPending=FALSE;return D->Report.Status=Exact(S);}
#define DRIVER(This,Field) BASE_CR(BASE_CR(This,PIANO_POGO_ADAPTER,Field),PIANO_POGO_DXE,Adapter)
STATIC EFI_STATUS EFIAPI TextReset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *T,BOOLEAN V){if(!T)return EFI_INVALID_PARAMETER;PIANO_POGO_DXE *D=DRIVER(T,Text);return Active(D)?D->OriginalText.Reset(T,V):EFI_NOT_READY;}
STATIC EFI_STATUS EFIAPI TextRead(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *T,EFI_INPUT_KEY *K){if(!T||!K)return EFI_INVALID_PARAMETER;PIANO_POGO_DXE *D=DRIVER(T,Text);return Active(D)?D->OriginalText.ReadKeyStroke(T,K):EFI_NOT_READY;}
STATIC EFI_STATUS EFIAPI ExReset(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *T,BOOLEAN V){if(!T)return EFI_INVALID_PARAMETER;PIANO_POGO_DXE *D=DRIVER(T,TextEx);return Active(D)?D->OriginalEx.Reset(T,V):EFI_NOT_READY;}
STATIC EFI_STATUS EFIAPI ExRead(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *T,EFI_KEY_DATA *K){if(!T||!K)return EFI_INVALID_PARAMETER;PIANO_POGO_DXE *D=DRIVER(T,TextEx);return Active(D)?D->OriginalEx.ReadKeyStrokeEx(T,K):EFI_NOT_READY;}
STATIC EFI_STATUS EFIAPI ExState(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *T,EFI_KEY_TOGGLE_STATE *K){if(!T||!K)return EFI_INVALID_PARAMETER;PIANO_POGO_DXE *D=DRIVER(T,TextEx);return Active(D)?D->OriginalEx.SetState(T,K):EFI_NOT_READY;}
STATIC EFI_STATUS EFIAPI PointerReset(EFI_SIMPLE_POINTER_PROTOCOL *T,BOOLEAN V){if(!T)return EFI_INVALID_PARAMETER;PIANO_POGO_DXE *D=DRIVER(T,Pointer);return Active(D)?D->OriginalPointer.Reset(T,V):EFI_NOT_READY;}
STATIC EFI_STATUS EFIAPI PointerRead(EFI_SIMPLE_POINTER_PROTOCOL *T,EFI_SIMPLE_POINTER_STATE *K){if(!T||!K)return EFI_INVALID_PARAMETER;PIANO_POGO_DXE *D=DRIVER(T,Pointer);return Active(D)?D->OriginalPointer.GetState(T,K):EFI_NOT_READY;}
STATIC EFI_STATUS EFIAPI AbsReset(EFI_ABSOLUTE_POINTER_PROTOCOL *T,BOOLEAN V){if(!T)return EFI_INVALID_PARAMETER;PIANO_POGO_DXE *D=DRIVER(T,Absolute);return Active(D)?D->OriginalAbsolute.Reset(T,V):EFI_NOT_READY;}
STATIC EFI_STATUS EFIAPI AbsRead(EFI_ABSOLUTE_POINTER_PROTOCOL *T,EFI_ABSOLUTE_POINTER_STATE *K){if(!T||!K)return EFI_INVALID_PARAMETER;PIANO_POGO_DXE *D=DRIVER(T,Absolute);return Active(D)?D->OriginalAbsolute.GetState(T,K):EFI_NOT_READY;}
STATIC BOOLEAN Match(CONST EFI_KEY_DATA *A,CONST EFI_KEY_DATA *B){return A->Key.ScanCode==B->Key.ScanCode&&A->Key.UnicodeChar==B->Key.UnicodeChar&&
  (!(A->KeyState.KeyShiftState&EFI_SHIFT_STATE_VALID)||A->KeyState.KeyShiftState==B->KeyState.KeyShiftState)&&
  (!(A->KeyState.KeyToggleState&EFI_TOGGLE_STATE_VALID)||A->KeyState.KeyToggleState==B->KeyState.KeyToggleState);}
STATIC EFI_STATUS EFIAPI Register(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *T,EFI_KEY_DATA *K,EFI_KEY_NOTIFY_FUNCTION F,VOID **Token){
  if(!T||!K||!F||!Token)return EFI_INVALID_PARAMETER;PIANO_POGO_DXE *D=DRIVER(T,TextEx);*Token=NULL;if(!Active(D))return EFI_NOT_READY;
  for(UINTN I=0;I<PIANO_POGO_NOTIFIES;++I)if(D->Notify[I].Function==F&&D->Notify[I].Match.Key.ScanCode==K->Key.ScanCode&&D->Notify[I].Match.Key.UnicodeChar==K->Key.UnicodeChar&&D->Notify[I].Match.KeyState.KeyShiftState==K->KeyState.KeyShiftState&&D->Notify[I].Match.KeyState.KeyToggleState==K->KeyState.KeyToggleState){*Token=D->Notify[I].Token;return EFI_SUCCESS;}
  for(UINTN I=0;I<PIANO_POGO_NOTIFIES;++I)if(!D->Notify[I].Function){if(mNotifySequence==MAX_UINTN)return EFI_OUT_OF_RESOURCES;D->NotifySequence=++mNotifySequence;D->Notify[I]=(PIANO_POGO_DXE_NOTIFY){*K,F,(VOID *)D->NotifySequence};*Token=D->Notify[I].Token;return EFI_SUCCESS;}return EFI_OUT_OF_RESOURCES;
}
STATIC EFI_STATUS EFIAPI Unregister(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *T,VOID *Token){
  if(!T||!Token)return EFI_INVALID_PARAMETER;PIANO_POGO_DXE *D=DRIVER(T,TextEx);if(!Live(D))return EFI_ABORTED;
  for(UINTN I=0;I<PIANO_POGO_NOTIFIES;++I)if(D->Notify[I].Token==Token&&D->Notify[I].Function){ZeroMem(&D->Notify[I],sizeof(D->Notify[I]));return EFI_SUCCESS;}return EFI_INVALID_PARAMETER;
}
STATIC VOID Signal(VOID *Context,EFI_EVENT Event){PIANO_POGO_DXE *D=Context;if(!Live(D)||D->Report.Stopping||D->Report.Retained)return;EFI_STATUS S=gBS->SignalEvent(Event);if(!Live(D)){D->Report.ServicesLost=D->Report.Retained=TRUE;}else if(S!=EFI_SUCCESS)Retain(D,S);}
STATIC VOID EFIAPI WaitNotify(EFI_EVENT Event,VOID *Context){(VOID)Event;PIANO_POGO_DXE *D=Context;if(Active(D))PianoPogoSignalReady(&D->Adapter);}
STATIC VOID EFIAPI PollNotify(EFI_EVENT Event,VOID *Context){(VOID)Event;PIANO_POGO_DXE *D=Context;if(Active(D))D->Report.WorkPending=TRUE;}
STATIC VOID EFIAPI Ebs(EFI_EVENT Event,VOID *Context){(VOID)Event;PIANO_POGO_DXE *D=Context;D->Report.ServicesLost=D->Report.Retained=TRUE;D->Report.WorkPending=FALSE;}
EFI_STATUS PianoPogoDxeStart(PIANO_POGO_DXE *D,CONST PIANO_POGO_DXE_BACKEND *B,CONST EFI_SIMPLE_POINTER_MODE *Mode){
  if(!D||!B||!Mode)return EFI_INVALID_PARAMETER;
  if(mOwner||D->Signature)return EFI_ALREADY_STARTED;
  if(B->Revision!=1||!B->Ready||!B->DataReady||!B->NowUs||!B->Read68||!B->Stop)return EFI_INVALID_PARAMETER;
  if(!gBS||!gST||!gBS->RaiseTPL||!gBS->RestoreTPL||!gBS->CreateEvent||!gBS->CreateEventEx||!gBS->SetTimer||!gBS->CloseEvent||!gBS->SignalEvent||!gBS->InstallMultipleProtocolInterfaces||!gBS->UninstallMultipleProtocolInterfaces||!gBS->DisconnectController)return EFI_UNSUPPORTED;
  EFI_STATUS S=AtApp(D);if(S!=EFI_SUCCESS)return S;
  S=B->Ready(B->Context);if(!Live(D))return EFI_ABORTED;D->Report.Ready=S;if(S!=EFI_SUCCESS)return D->Report.Status=Exact(S);
  ZeroMem(D,sizeof(*D));D->Signature=SIGNATURE;D->Backend=*B;D->Report.Started=TRUE;D->Report.Ready=EFI_SUCCESS;mOwner=D;
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Ebs,D,&gEfiEventExitBootServicesGuid,&D->Exit);if(!Live(D))return Retain(D,EFI_ABORTED);if(S!=EFI_SUCCESS||!D->Exit){D->Report.EventsUnknown=TRUE;return Retain(D,S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);}
  for(UINTN I=0;I<4;++I){S=gBS->CreateEvent(EVT_NOTIFY_WAIT,TPL_CALLBACK,WaitNotify,D,&D->Wait[I]);if(!Live(D))return Retain(D,EFI_ABORTED);if(S!=EFI_SUCCESS||!D->Wait[I]){D->Report.EventsUnknown=TRUE;return Retain(D,S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);}}
  S=PianoPogoInitializeAdapter(&D->Adapter,Mode,D->Wait[0],D->Wait[1],D->Wait[2],D->Wait[3],Signal,D);if(S!=EFI_SUCCESS)return Retain(D,S);
  D->OriginalText=D->Adapter.Text;D->OriginalEx=D->Adapter.TextEx;D->OriginalPointer=D->Adapter.Pointer;D->OriginalAbsolute=D->Adapter.Absolute;
  D->Adapter.Text.Reset=TextReset;D->Adapter.Text.ReadKeyStroke=TextRead;D->Adapter.TextEx.Reset=ExReset;D->Adapter.TextEx.ReadKeyStrokeEx=ExRead;D->Adapter.TextEx.SetState=ExState;D->Adapter.TextEx.RegisterKeyNotify=Register;D->Adapter.TextEx.UnregisterKeyNotify=Unregister;
  D->Adapter.Pointer.Reset=PointerReset;D->Adapter.Pointer.GetState=PointerRead;D->Adapter.Absolute.Reset=AbsReset;D->Adapter.Absolute.GetState=AbsRead;
  D->Path.Vendor.Header=(EFI_DEVICE_PATH_PROTOCOL){HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH),0}};
  D->Path.Vendor.Guid=(EFI_GUID){0xB11D7800,0x74DE,0x47F7,{0x9B,0x11,0x5A,0x75,0x40,0x69,0x76,0x31}};
  D->Path.End=(EFI_DEVICE_PATH_PROTOCOL){END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};
  D->Report.InstallAttempted=TRUE;S=gBS->InstallMultipleProtocolInterfaces(&D->Handle,&gEfiDevicePathProtocolGuid,&D->Path,
    &gEfiSimpleTextInProtocolGuid,&D->Adapter.Text,&gEfiSimpleTextInputExProtocolGuid,&D->Adapter.TextEx,
    &gEfiSimplePointerProtocolGuid,&D->Adapter.Pointer,&gEfiAbsolutePointerProtocolGuid,&D->Adapter.Absolute,NULL);
  if(!Live(D))return Retain(D,EFI_ABORTED);if(S!=EFI_SUCCESS||!D->Handle)return Retain(D,S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);D->Report.Published=TRUE;
  if(gBS->ConnectController){S=gBS->ConnectController(D->Handle,NULL,NULL,TRUE);if(!Live(D))return Retain(D,EFI_ABORTED);D->Report.Connect=S;D->Report.Connected=S==EFI_SUCCESS;if(S!=EFI_SUCCESS&&!EFI_ERROR(S))return Retain(D,S);}
  S=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,PollNotify,D,&D->Timer);if(!Live(D))return Retain(D,EFI_ABORTED);if(S!=EFI_SUCCESS||!D->Timer){D->Report.EventsUnknown=TRUE;return Retain(D,S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);}
  S=gBS->SetTimer(D->Timer,TimerPeriodic,10000);if(!Live(D))return Retain(D,EFI_ABORTED);if(S!=EFI_SUCCESS)return Retain(D,S);
  D->Report.WorkPending=TRUE;return D->Report.Status=EFI_SUCCESS;
}
EFI_STATUS PianoPogoDxePump(PIANO_POGO_DXE *D){
  if(!D||D->Signature!=SIGNATURE)return EFI_INVALID_PARAMETER;EFI_STATUS S=AtApp(D);if(S!=EFI_SUCCESS)return S;
  if(!Active(D)||D->Report.Busy)return EFI_NOT_READY;if(!D->Report.WorkPending)return EFI_NOT_READY;
  D->Report.Busy=TRUE;D->Report.WorkPending=FALSE;UINT8 Frame[PIANO_POGO_FRAME_BYTES]={0};UINTN Bytes=0;
  S=D->Backend.Ready(D->Backend.Context);D->Report.Ready=S;if(!Live(D)){S=EFI_ABORTED;goto Done;}if(S!=EFI_SUCCESS){S=Exact(S);goto Done;}
  BOOLEAN Ready=FALSE;S=D->Backend.DataReady(D->Backend.Context,&Ready);if(!Live(D)){S=EFI_ABORTED;goto Done;}if(S!=EFI_SUCCESS||Ready!=TRUE){S=S==EFI_SUCCESS?EFI_NOT_READY:Exact(S);goto Done;}
  UINT64 Begin=D->Backend.NowUs(D->Backend.Context);if(!Live(D)){S=EFI_ABORTED;goto Done;}if(Begin<D->Report.LastTime||Begin>MAX_UINT64-10000){S=EFI_COMPROMISED_DATA;goto Done;}
  ++D->Report.Reads;S=D->Backend.Read68(D->Backend.Context,Begin+10000,Frame,&Bytes);
  if(!Live(D)){S=EFI_ABORTED;goto Done;}UINT64 End=D->Backend.NowUs(D->Backend.Context);if(!Live(D)){S=EFI_ABORTED;goto Done;}D->Report.LastTime=End;
  if(End<Begin||End>Begin+10000){S=EFI_TIMEOUT;goto Done;}if(S!=EFI_SUCCESS){S=Exact(S);goto Done;}if(Bytes!=sizeof(Frame)){S=EFI_BAD_BUFFER_SIZE;goto Done;}
  // Capture new keys before wait-event consumers can drain the queue. The
  // existing adapter's hardware-independent parser remains unchanged.
  UINTN Before=D->Adapter.Core.KeyCount;D->Adapter.Signal=NULL;
  S=PianoPogoFeedAdapter(&D->Adapter,Frame,Bytes);D->Adapter.Signal=Signal;
  if(!Live(D)){S=EFI_ABORTED;goto Done;}
  if(D->Report.Retained){S=D->Report.Status;goto Done;}
  if(S==EFI_SUCCESS){++D->Report.Frames;EFI_KEY_DATA New[PIANO_POGO_KEY_QUEUE];UINTN Count=0;
    for(UINTN I=Before;I<D->Adapter.Core.KeyCount;++I)New[Count++]=D->Adapter.Core.Keys[(D->Adapter.Core.KeyHead+I)%PIANO_POGO_KEY_QUEUE];
    for(UINTN I=0;I<Count;++I)for(UINTN J=0;J<PIANO_POGO_NOTIFIES;++J)if(D->Notify[J].Function&&Match(&D->Notify[J].Match,&New[I])){
      EFI_KEY_NOTIFY_FUNCTION F=D->Notify[J].Function;EFI_TPL T=gBS->RaiseTPL(TPL_CALLBACK);F(&New[I]);if(!Live(D)){S=EFI_ABORTED;goto Done;}gBS->RestoreTPL(T);if(!Live(D)){S=EFI_ABORTED;goto Done;}++D->Report.Notifications;
    }
    PianoPogoSignalReady(&D->Adapter);if(!Live(D)){S=EFI_ABORTED;goto Done;}if(D->Report.Retained)S=D->Report.Status;
  }
Done:
  for(UINTN I=0;I<sizeof(Frame);++I)((volatile UINT8 *)Frame)[I]=0;D->Report.Busy=FALSE;D->Report.Read=S;
  if(S!=EFI_SUCCESS&&S!=EFI_NOT_READY)return Retain(D,S);return D->Report.Status=S;
}
EFI_STATUS PianoPogoDxeStop(PIANO_POGO_DXE *D){
  if(!D||D->Signature!=SIGNATURE)return EFI_INVALID_PARAMETER;if(D->Report.Stopped)return EFI_SUCCESS;
  EFI_STATUS S=AtApp(D);if(S!=EFI_SUCCESS)return S;if(D->Report.Busy)return EFI_NOT_READY;
  if(D->Report.StopAttempted||D->Report.EventsUnknown||(D->Report.InstallAttempted&&!D->Report.Published))return Retain(D,EFI_ACCESS_DENIED);
  D->Report.StopAttempted=TRUE;D->Report.Stopping=TRUE;D->Report.WorkPending=FALSE;
  if(D->Timer){S=gBS->SetTimer(D->Timer,TimerCancel,0);if(!Live(D)||S!=EFI_SUCCESS)return Retain(D,!Live(D)?EFI_ABORTED:S);S=gBS->CloseEvent(D->Timer);if(!Live(D)||S!=EFI_SUCCESS)return Retain(D,!Live(D)?EFI_ABORTED:S);D->Timer=NULL;}
  S=D->Backend.Stop(D->Backend.Context,&D->StopReport);D->Report.BackendStop=S;
  if(!Live(D)||S!=EFI_SUCCESS||D->StopReport.Status!=EFI_SUCCESS||!D->StopReport.Quiet||!D->StopReport.Clean||D->StopReport.Retained)return Retain(D,!Live(D)?EFI_ABORTED:S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);
  if(D->Report.Published){S=gBS->DisconnectController(D->Handle,NULL,NULL);D->Report.Disconnect=S;if(!Live(D))return Retain(D,EFI_ABORTED);if(S!=EFI_SUCCESS&&S!=EFI_NOT_FOUND&&S!=EFI_NOT_STARTED)return Retain(D,S);
    S=gBS->UninstallMultipleProtocolInterfaces(D->Handle,&gEfiDevicePathProtocolGuid,&D->Path,
      &gEfiSimpleTextInProtocolGuid,&D->Adapter.Text,&gEfiSimpleTextInputExProtocolGuid,&D->Adapter.TextEx,
      &gEfiSimplePointerProtocolGuid,&D->Adapter.Pointer,&gEfiAbsolutePointerProtocolGuid,&D->Adapter.Absolute,NULL);
    D->Report.Uninstall=S;if(!Live(D)||S!=EFI_SUCCESS)return Retain(D,!Live(D)?EFI_ABORTED:S);D->Report.Published=FALSE;}
  ZeroMem(D->Notify,sizeof(D->Notify));PianoPogoReset(&D->Adapter.Core);
  for(UINTN I=0;I<4;++I)if(D->Wait[I]){S=gBS->CloseEvent(D->Wait[I]);D->Report.Events=S;if(!Live(D)||S!=EFI_SUCCESS)return Retain(D,!Live(D)?EFI_ABORTED:S);D->Wait[I]=NULL;}
  if(D->Exit){S=gBS->CloseEvent(D->Exit);D->Report.Events=S;if(!Live(D)||S!=EFI_SUCCESS)return Retain(D,!Live(D)?EFI_ABORTED:S);D->Exit=NULL;}
  D->Report.Stopped=TRUE;D->Report.Retained=FALSE;D->Report.Status=EFI_SUCCESS;mOwner=NULL;return EFI_SUCCESS;
}
CONST PIANO_POGO_DXE_REPORT *PianoPogoDxeReport(CONST PIANO_POGO_DXE *D){return D&&D->Signature==SIGNATURE?&D->Report:NULL;}
