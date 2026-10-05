// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoPogoInput.h"

#define OWNER(This, Field) BASE_CR(This, PIANO_POGO_ADAPTER, Field)

STATIC EFI_STATUS EFIAPI ResetText(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN Verify) {
  if(This==NULL) { return EFI_INVALID_PARAMETER; } if(Verify) { return EFI_UNSUPPORTED; }
  PianoPogoReset(&OWNER(This,Text)->Core); return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ResetEx(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, BOOLEAN Verify) {
  if(This==NULL) { return EFI_INVALID_PARAMETER; } if(Verify) { return EFI_UNSUPPORTED; }
  PianoPogoReset(&OWNER(This,TextEx)->Core); return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ReadText(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key) {
  EFI_KEY_DATA Data; UINTN I; PIANO_POGO_ADAPTER *Adapter;
  if(This==NULL||Key==NULL) { return EFI_INVALID_PARAMETER; } Adapter=OWNER(This,Text);
  for(I=0;I<PIANO_POGO_KEY_QUEUE;I++) {
    EFI_STATUS Status=PianoPogoReadKeyEx(&Adapter->Core,&Data);
    if(EFI_ERROR(Status)) { return Status; }
    if(Data.Key.ScanCode||Data.Key.UnicodeChar) { *Key=Data.Key; return EFI_SUCCESS; }
  }
  return EFI_NOT_READY;
}
STATIC EFI_STATUS EFIAPI ReadEx(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_DATA *Key) {
  if(This==NULL) { return EFI_INVALID_PARAMETER; } return PianoPogoReadKeyEx(&OWNER(This,TextEx)->Core,Key);
}
STATIC EFI_STATUS EFIAPI SetState(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_TOGGLE_STATE *State) {
  if(This==NULL||State==NULL) { return EFI_INVALID_PARAMETER; } return PianoPogoSetToggle(&OWNER(This,TextEx)->Core,*State);
}
STATIC BOOLEAN MatchKey(CONST EFI_KEY_DATA *Filter, CONST EFI_KEY_DATA *Key) {
  if(Filter->Key.ScanCode!=Key->Key.ScanCode||Filter->Key.UnicodeChar!=Key->Key.UnicodeChar) { return FALSE; }
  if((Filter->KeyState.KeyShiftState&EFI_SHIFT_STATE_VALID)&&Filter->KeyState.KeyShiftState!=Key->KeyState.KeyShiftState) { return FALSE; }
  if((Filter->KeyState.KeyToggleState&EFI_TOGGLE_STATE_VALID)&&Filter->KeyState.KeyToggleState!=Key->KeyState.KeyToggleState) { return FALSE; }
  return TRUE;
}
STATIC EFI_STATUS EFIAPI RegisterNotify(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_DATA *Key,
                                       EFI_KEY_NOTIFY_FUNCTION Function, VOID **Handle) {
  UINTN I; PIANO_POGO_ADAPTER *Adapter;
  if(This==NULL||Key==NULL||Function==NULL||Handle==NULL) { return EFI_INVALID_PARAMETER; } Adapter=OWNER(This,TextEx);
  for(I=0;I<PIANO_POGO_NOTIFIES;I++) {
    if(Adapter->Notifies[I].Used&&Adapter->Notifies[I].Function==Function&&
       Adapter->Notifies[I].Match.Key.ScanCode==Key->Key.ScanCode&&Adapter->Notifies[I].Match.Key.UnicodeChar==Key->Key.UnicodeChar&&
       Adapter->Notifies[I].Match.KeyState.KeyShiftState==Key->KeyState.KeyShiftState&&
       Adapter->Notifies[I].Match.KeyState.KeyToggleState==Key->KeyState.KeyToggleState) { *Handle=&Adapter->Notifies[I]; return EFI_SUCCESS; }
  }
  for(I=0;I<PIANO_POGO_NOTIFIES;I++) { if(!Adapter->Notifies[I].Used) {
    Adapter->Notifies[I].Used=TRUE; Adapter->Notifies[I].Match=*Key; Adapter->Notifies[I].Function=Function;
    *Handle=&Adapter->Notifies[I]; return EFI_SUCCESS;
  } }
  return EFI_OUT_OF_RESOURCES;
}
STATIC EFI_STATUS EFIAPI UnregisterNotify(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, VOID *Handle) {
  UINTN I; PIANO_POGO_ADAPTER *Adapter;
  if(This==NULL||Handle==NULL) { return EFI_INVALID_PARAMETER; } Adapter=OWNER(This,TextEx);
  for(I=0;I<PIANO_POGO_NOTIFIES;I++) { if(Handle==&Adapter->Notifies[I]&&Adapter->Notifies[I].Used) {
    Adapter->Notifies[I].Used=FALSE; Adapter->Notifies[I].Function=NULL; return EFI_SUCCESS;
  } }
  return EFI_INVALID_PARAMETER;
}
STATIC EFI_STATUS EFIAPI ResetPointer(EFI_SIMPLE_POINTER_PROTOCOL *This, BOOLEAN Verify) {
  PIANO_POGO_ADAPTER *Adapter; EFI_SIMPLE_POINTER_STATE Empty={0};
  if(This==NULL) { return EFI_INVALID_PARAMETER; } if(Verify) { return EFI_UNSUPPORTED; } Adapter=OWNER(This,Pointer);
  Adapter->Core.Relative=Empty; Adapter->Core.RelativePending=FALSE; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI GetPointer(EFI_SIMPLE_POINTER_PROTOCOL *This, EFI_SIMPLE_POINTER_STATE *State) {
  if(This==NULL) { return EFI_INVALID_PARAMETER; } return PianoPogoReadPointer(&OWNER(This,Pointer)->Core,State);
}
STATIC EFI_STATUS EFIAPI ResetAbsolute(EFI_ABSOLUTE_POINTER_PROTOCOL *This, BOOLEAN Verify) {
  PIANO_POGO_ADAPTER *Adapter; EFI_ABSOLUTE_POINTER_STATE Empty={0};
  if(This==NULL) { return EFI_INVALID_PARAMETER; } if(Verify) { return EFI_UNSUPPORTED; } Adapter=OWNER(This,Absolute);
  Adapter->Core.Absolute=Empty; Adapter->Core.AbsolutePending=FALSE; Adapter->Core.PrimaryContactKnown=FALSE; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI GetAbsolute(EFI_ABSOLUTE_POINTER_PROTOCOL *This, EFI_ABSOLUTE_POINTER_STATE *State) {
  if(This==NULL) { return EFI_INVALID_PARAMETER; } return PianoPogoReadAbsolute(&OWNER(This,Absolute)->Core,State);
}

VOID PianoPogoSignalReady(PIANO_POGO_ADAPTER *Adapter) {
  UINTN I; BOOLEAN Printable=FALSE;
  if(Adapter==NULL||Adapter->Signal==NULL) { return; }
  if(Adapter->Core.KeyCount) {
    for(I=0;I<Adapter->Core.KeyCount;I++) {
      CONST EFI_INPUT_KEY *Key=&Adapter->Core.Keys[(Adapter->Core.KeyHead+I)%PIANO_POGO_KEY_QUEUE].Key;
      if(Key->ScanCode||Key->UnicodeChar) { Printable=TRUE; break; }
    }
    if(Printable&&Adapter->Text.WaitForKey!=NULL) { Adapter->Signal(Adapter->SignalContext,Adapter->Text.WaitForKey); }
    if(Adapter->TextEx.WaitForKeyEx!=NULL) { Adapter->Signal(Adapter->SignalContext,Adapter->TextEx.WaitForKeyEx); }
  }
  if(Adapter->Core.RelativePending&&Adapter->Pointer.WaitForInput!=NULL) { Adapter->Signal(Adapter->SignalContext,Adapter->Pointer.WaitForInput); }
  if(Adapter->Core.AbsolutePending&&Adapter->Absolute.WaitForInput!=NULL) { Adapter->Signal(Adapter->SignalContext,Adapter->Absolute.WaitForInput); }
}

EFI_STATUS PianoPogoInitializeAdapter(PIANO_POGO_ADAPTER *Adapter, CONST EFI_SIMPLE_POINTER_MODE *Mode,
                                     EFI_EVENT KeyEvent, EFI_EVENT KeyExEvent, EFI_EVENT PointerEvent, EFI_EVENT AbsoluteEvent,
                                     PIANO_POGO_SIGNAL Signal, VOID *SignalContext) {
  PIANO_POGO_ADAPTER Empty={0};
  if(Adapter==NULL||Mode==NULL||!Mode->ResolutionX||!Mode->ResolutionY) { return EFI_INVALID_PARAMETER; }
  *Adapter=Empty; PianoPogoReset(&Adapter->Core); Adapter->Signal=Signal; Adapter->SignalContext=SignalContext;
  Adapter->Text.Reset=ResetText; Adapter->Text.ReadKeyStroke=ReadText; Adapter->Text.WaitForKey=KeyEvent;
  Adapter->TextEx.Reset=ResetEx; Adapter->TextEx.ReadKeyStrokeEx=ReadEx; Adapter->TextEx.SetState=SetState;
  Adapter->TextEx.RegisterKeyNotify=RegisterNotify; Adapter->TextEx.UnregisterKeyNotify=UnregisterNotify; Adapter->TextEx.WaitForKeyEx=KeyExEvent;
  Adapter->PointerMode=*Mode; Adapter->Pointer.Mode=&Adapter->PointerMode; Adapter->Pointer.Reset=ResetPointer;
  Adapter->Pointer.GetState=GetPointer; Adapter->Pointer.WaitForInput=PointerEvent;
  Adapter->AbsoluteMode.AbsoluteMaxX=PIANO_POGO_TOUCH_MAX_X; Adapter->AbsoluteMode.AbsoluteMaxY=PIANO_POGO_TOUCH_MAX_Y;
  Adapter->AbsoluteMode.Attributes=EFI_ABSP_SupportsAltActive; Adapter->Absolute.Mode=&Adapter->AbsoluteMode;
  Adapter->Absolute.Reset=ResetAbsolute; Adapter->Absolute.GetState=GetAbsolute; Adapter->Absolute.WaitForInput=AbsoluteEvent;
  return EFI_SUCCESS;
}

EFI_STATUS PianoPogoFeedAdapter(PIANO_POGO_ADAPTER *Adapter, CONST UINT8 *Frame, UINTN Bytes) {
  EFI_STATUS Status; EFI_KEY_DATA NewKeys[PIANO_POGO_KEY_QUEUE]; UINTN Before,NewCount=0,I,J;
  if(Adapter==NULL) { return EFI_INVALID_PARAMETER; } if(Adapter->Feeding) { return EFI_NOT_READY; }
  Adapter->Feeding=TRUE; Before=Adapter->Core.KeyCount;
  Status=PianoPogoFeedFrame(&Adapter->Core,Frame,Bytes);
  if(!EFI_ERROR(Status)) {
    // Detach can flush old keys; only a nondecreasing queue carries new notifications.
    for(I=Before;I<Adapter->Core.KeyCount;I++) { NewKeys[NewCount++]=Adapter->Core.Keys[(Adapter->Core.KeyHead+I)%PIANO_POGO_KEY_QUEUE]; }
    for(I=0;I<NewCount;I++) { for(J=0;J<PIANO_POGO_NOTIFIES;J++) {
      if(Adapter->Notifies[J].Used&&MatchKey(&Adapter->Notifies[J].Match,&NewKeys[I])) {
        EFI_KEY_NOTIFY_FUNCTION Function=Adapter->Notifies[J].Function;
        Function(&NewKeys[I]);
      }
    } }
    PianoPogoSignalReady(Adapter);
  }
  Adapter->Feeding=FALSE; return Status;
}
