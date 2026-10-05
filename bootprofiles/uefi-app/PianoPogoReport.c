// SPDX-License-Identifier: BSD-2-Clause-Patent
// Independent implementation of documented Piano protocol facts. Descriptors
// and Linux code are not embedded. Review source: blu-sharky/linux-piano 7a33,
// drivers/hid/hid-nanosic-wn8030.c. Incoming framing has no verified CRC here.
#include "PianoPogoReport.h"

STATIC UINT16 Le16(CONST UINT8 *P) { return (UINT16)(P[0] | ((UINT16)P[1] << 8)); }
STATIC INT16 S16(CONST UINT8 *P) { UINT16 V=Le16(P); return (INT16)(V<0x8000?V:(INT32)V-65536); }
STATIC INT32 AddBounded(INT32 A, INT32 B) {
  INT64 Sum=(INT64)A+B;
  return Sum>2147483647?2147483647:Sum<(-2147483647-1)?(-2147483647-1):(INT32)Sum;
}
STATIC BOOLEAN KeyPresent(CONST UINT8 *Keys, UINT8 Usage);

VOID PianoPogoReset(PIANO_POGO_INPUT *Input) {
  if (Input!=NULL) { PIANO_POGO_INPUT Empty={0}; *Input=Empty; Input->Toggle=EFI_TOGGLE_STATE_VALID; }
}

VOID PianoPogoGetKeyState(CONST PIANO_POGO_INPUT *Input, EFI_KEY_STATE *State) {
  STATIC CONST UINT32 Bits[8]={EFI_LEFT_CONTROL_PRESSED,EFI_LEFT_SHIFT_PRESSED,
    EFI_LEFT_ALT_PRESSED,EFI_LEFT_LOGO_PRESSED,EFI_RIGHT_CONTROL_PRESSED,
    EFI_RIGHT_SHIFT_PRESSED,EFI_RIGHT_ALT_PRESSED,EFI_RIGHT_LOGO_PRESSED};
  UINTN I;
  if (Input==NULL||State==NULL) { return; }
  State->KeyShiftState=EFI_SHIFT_STATE_VALID;
  for (I=0;I<8;I++) { if (Input->Modifiers & (1U<<I)) { State->KeyShiftState|=Bits[I]; } }
  if (KeyPresent(Input->PreviousKeys,0x65)) { State->KeyShiftState|=EFI_MENU_KEY_PRESSED; }
  if (KeyPresent(Input->PreviousKeys,0x46)) { State->KeyShiftState|=EFI_SYS_REQ_PRESSED; }
  State->KeyToggleState=Input->Toggle;
}

STATIC BOOLEAN KeyPresent(CONST UINT8 *Keys, UINT8 Usage) {
  UINTN I; for (I=0;I<6;I++) { if (Keys[I]==Usage) { return TRUE; } } return FALSE;
}

STATIC BOOLEAN PushKey(PIANO_POGO_INPUT *Input, EFI_KEY_DATA Key) {
  if (Input->KeyCount==PIANO_POGO_KEY_QUEUE) { return FALSE; }
  Input->Keys[(Input->KeyHead+Input->KeyCount)%PIANO_POGO_KEY_QUEUE]=Key;
  Input->KeyCount++; return TRUE;
}

STATIC BOOLEAN PushControl(PIANO_POGO_INPUT *Input, PIANO_POGO_CONTROL_EVENT Event) {
  if (Input->ControlCount==PIANO_POGO_CONTROL_QUEUE) {
    Input->ControlHead=(Input->ControlHead+1)%PIANO_POGO_CONTROL_QUEUE;
    Input->ControlCount--; Input->DroppedControls++;
  }
  Input->Controls[(Input->ControlHead+Input->ControlCount)%PIANO_POGO_CONTROL_QUEUE]=Event;
  Input->ControlCount++; return TRUE;
}

STATIC EFI_INPUT_KEY TranslateKey(UINT8 Usage, BOOLEAN Shift, BOOLEAN Caps, BOOLEAN Num) {
  EFI_INPUT_KEY Key={0};
  STATIC CONST CHAR8 Digits[]="1234567890", ShiftDigits[]="!@#$%^&*()";
  STATIC CONST CHAR8 Plain[]="-=[]\\#;'`,./", Shifted[]="_+{}|~:\"~<>?";
  // Slots 0x2d..0x38 include ISO hash (0x32); keyboard layout is US bootstrap.
  if (Usage>=0x04 && Usage<=0x1d) { Key.UnicodeChar=(CHAR16)('a'+Usage-4); if (Shift!=Caps) { Key.UnicodeChar-=32; } }
  else if (Usage>=0x1e && Usage<=0x27) { Key.UnicodeChar=(CHAR16)(Shift?ShiftDigits[Usage-0x1e]:Digits[Usage-0x1e]); }
  else if (Usage>=0x2d && Usage<=0x38) { Key.UnicodeChar=(CHAR16)(Shift?Shifted[Usage-0x2d]:Plain[Usage-0x2d]); }
  else if (Usage>=0x3a && Usage<=0x45) { Key.ScanCode=(UINT16)(SCAN_F1+Usage-0x3a); }
  else if (Usage>=0x68 && Usage<=0x73) { Key.ScanCode=(UINT16)(SCAN_F13+Usage-0x68); }
  else if (Usage>=0x59 && Usage<=0x63) {
    STATIC CONST UINT16 Scans[11]={SCAN_END,SCAN_DOWN,SCAN_PAGE_DOWN,SCAN_LEFT,0,SCAN_RIGHT,SCAN_HOME,SCAN_UP,SCAN_PAGE_UP,SCAN_INSERT,SCAN_DELETE};
    STATIC CONST CHAR8 Number[]="1234567890.";
    if (Num&&!Shift) { Key.UnicodeChar=Number[Usage-0x59]; } else { Key.ScanCode=Scans[Usage-0x59]; }
  } else {
    switch(Usage) {
      case 0x28: case 0x58: Key.UnicodeChar=CHAR_CARRIAGE_RETURN; break;
      case 0x29: Key.ScanCode=SCAN_ESC; break;
      case 0x2a: Key.UnicodeChar=CHAR_BACKSPACE; break;
      case 0x2b: Key.UnicodeChar=CHAR_TAB; break;
      case 0x2c: Key.UnicodeChar=' '; break;
      case 0x48: Key.ScanCode=SCAN_PAUSE; break;
      case 0x49: Key.ScanCode=SCAN_INSERT; break;
      case 0x4a: Key.ScanCode=SCAN_HOME; break;
      case 0x4b: Key.ScanCode=SCAN_PAGE_UP; break;
      case 0x4c: Key.ScanCode=SCAN_DELETE; break;
      case 0x4d: Key.ScanCode=SCAN_END; break;
      case 0x4e: Key.ScanCode=SCAN_PAGE_DOWN; break;
      case 0x4f: Key.ScanCode=SCAN_RIGHT; break;
      case 0x50: Key.ScanCode=SCAN_LEFT; break;
      case 0x51: Key.ScanCode=SCAN_DOWN; break;
      case 0x52: Key.ScanCode=SCAN_UP; break;
      case 0x54: Key.UnicodeChar='/'; break;
      case 0x55: Key.UnicodeChar='*'; break;
      case 0x56: Key.UnicodeChar='-'; break;
      case 0x57: Key.UnicodeChar='+'; break;
      default: break;
    }
  }
  return Key;
}

STATIC BOOLEAN Keyboard(PIANO_POGO_INPUT *Input, CONST UINT8 *P) {
  UINTN I,J; UINT8 OldKeys[6]; BOOLEAN Changed=Input->Modifiers!=P[1];
  for (I=3;I<9;I++) { if (P[I]>=1&&P[I]<=3) { return TRUE; } } // rollover: retain prior state.
  Input->Modifiers=P[1];
  for(I=0;I<6;I++) { OldKeys[I]=Input->PreviousKeys[I]; Input->PreviousKeys[I]=P[I+3]; if(OldKeys[I]!=P[I+3]) { Changed=TRUE; } }
  for (I=3;I<9;I++) {
    UINT8 Usage=P[I]; EFI_KEY_DATA Key={0}; BOOLEAN Duplicate=FALSE,Shift;
    if (!Usage || KeyPresent(OldKeys,Usage)) { continue; }
    for (J=3;J<I;J++) { if (P[J]==Usage) { Duplicate=TRUE; } }
    if (Duplicate) { continue; }
    Changed=TRUE;
    if (Usage==0x39) { Input->Toggle^=EFI_CAPS_LOCK_ACTIVE; continue; }
    if (Usage==0x53) { Input->Toggle^=EFI_NUM_LOCK_ACTIVE; continue; }
    if (Usage==0x47) { Input->Toggle^=EFI_SCROLL_LOCK_ACTIVE; continue; }
    Shift=(Input->Modifiers&0x22)!=0;
    Key.Key=TranslateKey(Usage,Shift,(Input->Toggle&EFI_CAPS_LOCK_ACTIVE)!=0,(Input->Toggle&EFI_NUM_LOCK_ACTIVE)!=0);
    PianoPogoGetKeyState(Input,&Key.KeyState);
    if (Shift && Key.Key.UnicodeChar>=0x20) { Key.KeyState.KeyShiftState&=~(EFI_LEFT_SHIFT_PRESSED|EFI_RIGHT_SHIFT_PRESSED); }
    if ((Input->Modifiers&0x11) && Key.Key.UnicodeChar>='a' && Key.Key.UnicodeChar<='z') { Key.Key.UnicodeChar-=('a'-1); }
    if ((Input->Modifiers&0x11) && Key.Key.UnicodeChar>='A' && Key.Key.UnicodeChar<='Z') { Key.Key.UnicodeChar-=('A'-1); }
    if ((Key.Key.ScanCode||Key.Key.UnicodeChar) && !PushKey(Input,Key)) { return FALSE; }
  }
  if (Changed && (Input->Toggle&EFI_KEY_STATE_EXPOSED)) {
    EFI_KEY_DATA Partial={0}; PianoPogoGetKeyState(Input,&Partial.KeyState);
    if (!PushKey(Input,Partial)) { return FALSE; }
  }
  return TRUE;
}

STATIC BOOLEAN Consumer(PIANO_POGO_INPUT *Input, CONST UINT8 *P) {
  UINT16 Usage=Le16(P+1); EFI_KEY_DATA Key={0}; PIANO_POGO_CONTROL_EVENT Event={0};
  if (Usage==Input->ConsumerUsage) { return TRUE; }
  Event.Kind=PianoPogoConsumer; Event.Value=Usage;
  if (!PushControl(Input,Event)) { return FALSE; }
  Input->ConsumerUsage=Usage;
  switch(Usage) {
    case 0xe2: Key.Key.ScanCode=SCAN_MUTE; break;
    case 0xe9: Key.Key.ScanCode=SCAN_VOLUME_UP; break;
    case 0xea: Key.Key.ScanCode=SCAN_VOLUME_DOWN; break;
    case 0x6f: Key.Key.ScanCode=SCAN_BRIGHTNESS_UP; break;
    case 0x70: Key.Key.ScanCode=SCAN_BRIGHTNESS_DOWN; break;
    default: break;
  }
  PianoPogoGetKeyState(Input,&Key.KeyState);
  return !Key.Key.ScanCode||PushKey(Input,Key);
}

STATIC VOID Mouse(PIANO_POGO_INPUT *Input, CONST UINT8 *P) {
  Input->Relative.RelativeMovementX=AddBounded(Input->Relative.RelativeMovementX,S16(P+2));
  Input->Relative.RelativeMovementY=AddBounded(Input->Relative.RelativeMovementY,S16(P+4));
  Input->Relative.RelativeMovementZ=AddBounded(Input->Relative.RelativeMovementZ,S16(P+6));
  Input->Relative.LeftButton=(P[1]&1)!=0; Input->Relative.RightButton=(P[1]&2)!=0;
  Input->RelativePending=TRUE;
}

STATIC EFI_STATUS Touch(PIANO_POGO_INPUT *Input, CONST UINT8 *P) {
  UINTN I; INTN Pick=-1;
  if (P[26]>3) { return EFI_UNSUPPORTED; } // no invented fourth/eighth contact.
  for (I=0;I<3;I++) {
    CONST UINT8 *C=P+2+I*8;
    PIANO_POGO_CONTACT *Out=&Input->Contacts[I];
    Out->Flags=C[0]; Out->Id=C[1]; Out->Pressure=Le16(C+2); Out->X=Le16(C+4); Out->Y=Le16(C+6);
    if ((Out->Flags&7)==7) {
      if (Out->X>PIANO_POGO_TOUCH_MAX_X||Out->Y>PIANO_POGO_TOUCH_MAX_Y) { return EFI_COMPROMISED_DATA; }
      if (Pick<0) { Pick=(INTN)I; }
    }
  }
  if (Input->PrimaryContactKnown) {
    for (I=0;I<3;I++) { if ((Input->Contacts[I].Flags&7)==7 && Input->Contacts[I].Id==Input->PrimaryContactId) { Pick=(INTN)I; break; } }
  }
  Input->ContactCount=P[26];
  if (Pick>=0 && P[26]) {
    Input->Absolute.CurrentX=Input->Contacts[Pick].X; Input->Absolute.CurrentY=Input->Contacts[Pick].Y;
    Input->PrimaryContactKnown=TRUE; Input->PrimaryContactId=Input->Contacts[Pick].Id;
    Input->Absolute.ActiveButtons=EFI_ABSP_TouchActive|((P[1]&2)?EFI_ABS_AltActive:0);
  } else { Input->PrimaryContactKnown=FALSE; Input->Absolute.ActiveButtons=0; }
  Input->Absolute.CurrentZ=0; // source descriptor says constant pressure; no fake pressure axis.
  Input->AbsolutePending=TRUE; return EFI_SUCCESS;
}

STATIC INT16 Axis12(UINT8 Low, UINT8 High) {
  UINT16 Value=(UINT16)(((UINT16)High<<4)|(Low>>4)); return (INT16)(Value<2048?Value:(INT32)Value-4096);
}

STATIC BOOLEAN Vendor(PIANO_POGO_INPUT *Input, CONST UINT8 *P, UINTN Len) {
  PIANO_POGO_CONTROL_EVENT Event={0};
  if (Len<12||P[2]!=0x38||P[3]!=0x80) { return TRUE; }
  Event.Command=P[4];
  switch(P[4]) {
    case 0xa2:
      if ((P[9]&3)!=0 && (P[9]&3)!=3) { return TRUE; }
      if (Input->AttachKnown&&Input->Attached==((P[9]&3)==3)) { return TRUE; }
      Input->AttachKnown=TRUE; Input->Attached=(P[9]&3)==3;
      Event.Kind=PianoPogoAttachment; Event.Value=Input->Attached;
      if (!Input->Attached) {
        UINTN I; Input->KeyHead=0; Input->KeyCount=0; Input->Modifiers=0; Input->ConsumerUsage=0;
        for(I=0;I<6;I++) { Input->PreviousKeys[I]=0; }
        Input->Relative.RelativeMovementX=Input->Relative.RelativeMovementY=Input->Relative.RelativeMovementZ=0;
        Input->Relative.LeftButton=Input->Relative.RightButton=FALSE; Input->RelativePending=TRUE;
        Input->Absolute.ActiveButtons=0; Input->AbsolutePending=TRUE; Input->PrimaryContactKnown=FALSE;
      }
      break;
    case 0x64:
      if (P[5]!=6) { return TRUE; }
      Event.Kind=PianoPogoHinge; Event.X=Axis12(P[6],P[7]); Event.Y=(INT16)-Axis12(P[8],P[9]); Event.Z=(INT16)-Axis12(P[10],P[11]); break;
    case 0x24: Event.Kind=PianoPogoAuthRequired; break;
    case 0x31: if(Len<24) { return TRUE; } Event.Kind=PianoPogoAuthUidAvailable; break;
    case 0x32: if(Len<38) { return TRUE; } Event.Kind=PianoPogoAuthChallengeAvailable; break;
    default: Event.Kind=PianoPogoVendorUnknown; break;
  }
  return PushControl(Input,Event); // no UID/challenge/token copied or logged.
}

EFI_STATUS PianoPogoFeedFrame(PIANO_POGO_INPUT *Input, CONST UINT8 *Frame, UINTN Bytes) {
  PIANO_POGO_INPUT Work; UINTN Offset=3,Reports=0; EFI_STATUS Status=EFI_SUCCESS;
  if (Input==NULL||Frame==NULL) { return EFI_INVALID_PARAMETER; }
  if (Bytes!=PIANO_POGO_FRAME_BYTES||Frame[0]!=0x57||!Frame[2]) { Input->RejectedFrames++; return EFI_COMPROMISED_DATA; }
  Work=*Input;
  while (Offset<Bytes && Reports++<PIANO_POGO_MAX_REPORTS) {
    CONST UINT8 *P=Frame+Offset; UINTN Size=0,I;
    if (!P[0]) {
      for(I=Offset;I<Bytes;I++) { if(Frame[I]) { Status=EFI_COMPROMISED_DATA; break; } }
      break;
    }
    switch(P[0]) {
      case 2: Size=8; break; case 5: Size=9; break; case 6: Size=5; break;
      case 0x19: Size=27; break; case 0x22: Size=16; break; case 0x23: Size=32; break;
      case 0x24: case 0x26: Size=Bytes-Offset; break;
      default: Status=EFI_UNSUPPORTED; break;
    }
    if(EFI_ERROR(Status)) { break; }
    if(Size>Bytes-Offset) { Status=EFI_COMPROMISED_DATA; break; }
    if (P[0]==0x22||P[0]==0x23||P[0]==0x24||P[0]==0x26) {
      if (!Vendor(&Work,P,Size)) { Status=EFI_OUT_OF_RESOURCES; break; }
    } else if (!Work.AttachKnown||Work.Attached) {
      if (P[0]==5 && !Keyboard(&Work,P)) { Status=EFI_OUT_OF_RESOURCES; break; }
      if (P[0]==6 && !Consumer(&Work,P)) { Status=EFI_OUT_OF_RESOURCES; break; }
      if (P[0]==2) { Mouse(&Work,P); }
      if (P[0]==0x19) { Status=Touch(&Work,P); if(EFI_ERROR(Status)) { break; } }
    }
    Offset+=Size;
  }
  if(EFI_ERROR(Status)) { Input->RejectedFrames++; if(Status==EFI_OUT_OF_RESOURCES) { Input->QueueOverflows++; } return Status; }
  Work.AcceptedFrames++; *Input=Work; return EFI_SUCCESS;
}

EFI_STATUS PianoPogoReadKeyEx(PIANO_POGO_INPUT *Input, EFI_KEY_DATA *Key) {
  if(Input==NULL||Key==NULL) { return EFI_INVALID_PARAMETER; }
  if(!Input->KeyCount) { return EFI_NOT_READY; }
  *Key=Input->Keys[Input->KeyHead]; Input->KeyHead=(Input->KeyHead+1)%PIANO_POGO_KEY_QUEUE; Input->KeyCount--; return EFI_SUCCESS;
}
EFI_STATUS PianoPogoReadControl(PIANO_POGO_INPUT *Input, PIANO_POGO_CONTROL_EVENT *Event) {
  if(Input==NULL||Event==NULL) { return EFI_INVALID_PARAMETER; }
  if(!Input->ControlCount) { return EFI_NOT_READY; }
  *Event=Input->Controls[Input->ControlHead]; Input->ControlHead=(Input->ControlHead+1)%PIANO_POGO_CONTROL_QUEUE; Input->ControlCount--; return EFI_SUCCESS;
}
EFI_STATUS PianoPogoReadPointer(PIANO_POGO_INPUT *Input, EFI_SIMPLE_POINTER_STATE *State) {
  if(Input==NULL||State==NULL) { return EFI_INVALID_PARAMETER; } if(!Input->RelativePending) { return EFI_NOT_READY; }
  *State=Input->Relative; Input->Relative.RelativeMovementX=Input->Relative.RelativeMovementY=Input->Relative.RelativeMovementZ=0;
  Input->RelativePending=FALSE; return EFI_SUCCESS;
}
EFI_STATUS PianoPogoReadAbsolute(PIANO_POGO_INPUT *Input, EFI_ABSOLUTE_POINTER_STATE *State) {
  if(Input==NULL||State==NULL) { return EFI_INVALID_PARAMETER; } if(!Input->AbsolutePending) { return EFI_NOT_READY; }
  *State=Input->Absolute; Input->AbsolutePending=FALSE; return EFI_SUCCESS;
}
EFI_STATUS PianoPogoSetToggle(PIANO_POGO_INPUT *Input, EFI_KEY_TOGGLE_STATE Toggle) {
  if(Input==NULL) { return EFI_INVALID_PARAMETER; }
  if(!(Toggle&EFI_TOGGLE_STATE_VALID)||(Toggle&~(EFI_TOGGLE_STATE_VALID|EFI_KEY_STATE_EXPOSED|7))) { return EFI_UNSUPPORTED; }
  Input->Toggle=Toggle; return EFI_SUCCESS; // LED transport/auth remains separate.
}
