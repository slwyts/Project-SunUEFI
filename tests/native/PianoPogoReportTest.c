// SPDX-License-Identifier: BSD-2-Clause-Patent
// Compile the real parser/adapter against the repository's real UEFI headers.
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "PianoPogoInput.h"
#include "PianoPogoI2c.h"

STATIC VOID Frame(UINT8 *F) { memset(F,0,68); F[0]=0x57; F[2]=1; }
STATIC VOID Word(UINT8 *P, UINT16 V) { P[0]=(UINT8)V; P[1]=(UINT8)(V>>8); }
STATIC EFI_STATUS FeedKey(PIANO_POGO_INPUT *C, UINT8 Mod, UINT8 Key) {
  UINT8 F[68]; Frame(F); F[3]=5; F[4]=Mod; F[6]=Key; return PianoPogoFeedFrame(C,F,68);
}
STATIC VOID EmptyKeys(PIANO_POGO_INPUT *C) { assert(FeedKey(C,0,0)==EFI_SUCCESS); }
STATIC EFI_KEY_DATA Get(PIANO_POGO_INPUT *C) { EFI_KEY_DATA K; assert(PianoPogoReadKeyEx(C,&K)==EFI_SUCCESS); return K; }

STATIC VOID TestKeys(VOID) {
  PIANO_POGO_INPUT C; EFI_KEY_DATA K; EFI_KEY_STATE S; PianoPogoReset(&C);
  assert(FeedKey(&C,0x02,0x04)==EFI_SUCCESS); K=Get(&C);
  assert(K.Key.UnicodeChar=='A' && !(K.KeyState.KeyShiftState&EFI_LEFT_SHIFT_PRESSED));
  assert(FeedKey(&C,0x02,0x04)==EFI_SUCCESS); assert(PianoPogoReadKeyEx(&C,&K)==EFI_NOT_READY);
  EmptyKeys(&C); assert(FeedKey(&C,0,0x39)==EFI_SUCCESS); EmptyKeys(&C);
  assert(FeedKey(&C,0,4)==EFI_SUCCESS); K=Get(&C); assert(K.Key.UnicodeChar=='A'); EmptyKeys(&C);
  assert(FeedKey(&C,2,4)==EFI_SUCCESS); K=Get(&C); assert(K.Key.UnicodeChar=='a'); EmptyKeys(&C);
  assert(FeedKey(&C,1,4)==EFI_SUCCESS); K=Get(&C); assert(K.Key.UnicodeChar==1 && (K.KeyState.KeyShiftState&EFI_LEFT_CONTROL_PRESSED));
  EmptyKeys(&C); assert(FeedKey(&C,0,0x52)==EFI_SUCCESS); assert(Get(&C).Key.ScanCode==SCAN_UP);
  EmptyKeys(&C); assert(FeedKey(&C,0,0x44)==EFI_SUCCESS); assert(Get(&C).Key.ScanCode==SCAN_F11);
  EmptyKeys(&C); assert(FeedKey(&C,0,0x65)==EFI_SUCCESS); PianoPogoGetKeyState(&C,&S); assert(S.KeyShiftState&EFI_MENU_KEY_PRESSED);
  EmptyKeys(&C); assert(FeedKey(&C,0,0x53)==EFI_SUCCESS); EmptyKeys(&C);
  assert(FeedKey(&C,0,0x59)==EFI_SUCCESS); assert(Get(&C).Key.UnicodeChar=='1'); EmptyKeys(&C);
  assert(FeedKey(&C,2,0x59)==EFI_SUCCESS); assert(Get(&C).Key.ScanCode==SCAN_END);
  assert(PianoPogoSetToggle(&C,EFI_CAPS_LOCK_ACTIVE)==EFI_UNSUPPORTED);
  assert(PianoPogoSetToggle(&C,EFI_TOGGLE_STATE_VALID|EFI_KEY_STATE_EXPOSED)==EFI_SUCCESS);
  EmptyKeys(&C); while(PianoPogoReadKeyEx(&C,&K)==EFI_SUCCESS) { }
  assert(FeedKey(&C,0x80,0)==EFI_SUCCESS); K=Get(&C); assert(!K.Key.UnicodeChar && (K.KeyState.KeyShiftState&EFI_RIGHT_LOGO_PRESSED));
}

STATIC VOID TestPackedAndAtomic(VOID) {
  UINT8 F[68]; PIANO_POGO_INPUT C,Before; EFI_KEY_DATA K; EFI_SIMPLE_POINTER_STATE P;
  PianoPogoReset(&C); Frame(F); F[3]=5; F[6]=4; F[12]=2; F[13]=3; Word(F+14,0xffff); Word(F+16,0x8000); Word(F+18,2);
  F[20]=6; Word(F+21,0xe9);
  assert(PianoPogoFeedFrame(&C,F,68)==EFI_SUCCESS); K=Get(&C); assert(K.Key.UnicodeChar=='a'); assert(Get(&C).Key.ScanCode==SCAN_VOLUME_UP);
  assert(PianoPogoReadPointer(&C,&P)==EFI_SUCCESS); assert(P.RelativeMovementX==-1 && P.RelativeMovementY==-32768 && P.RelativeMovementZ==2 && P.LeftButton && P.RightButton);
  Before=C; F[25]=0xfe;
  assert(PianoPogoFeedFrame(&C,F,68)==EFI_UNSUPPORTED); assert(C.KeyCount==Before.KeyCount && C.AcceptedFrames==Before.AcceptedFrames);
  assert(PianoPogoFeedFrame(&C,F,67)==EFI_COMPROMISED_DATA);
  Frame(F); F[3]=5; F[6]=4; F[67]=2; Before=C;
  assert(PianoPogoFeedFrame(&C,F,68)==EFI_COMPROMISED_DATA); assert(C.KeyCount==Before.KeyCount);
  assert(PianoPogoFeedFrame(NULL,F,68)==EFI_INVALID_PARAMETER);
  assert(PianoPogoReadKeyEx(&C,NULL)==EFI_INVALID_PARAMETER);
  C.Relative.RelativeMovementX=INT_MAX-1; Frame(F); F[3]=2; Word(F+5,100);
  assert(PianoPogoFeedFrame(&C,F,68)==EFI_SUCCESS); assert(C.Relative.RelativeMovementX==INT_MAX);
}

STATIC VOID TestRolloverAndQueues(VOID) {
  PIANO_POGO_INPUT C; UINT8 F[68]; UINTN I; EFI_KEY_DATA K;
  PianoPogoReset(&C); assert(FeedKey(&C,0,4)==EFI_SUCCESS); K=Get(&C); (void)K;
  assert(FeedKey(&C,0,1)==EFI_SUCCESS); assert(C.PreviousKeys[0]==4); assert(FeedKey(&C,0,4)==EFI_SUCCESS); assert(!C.KeyCount);
  EmptyKeys(&C); Frame(F); F[3]=5; F[6]=4; F[7]=4;
  assert(PianoPogoFeedFrame(&C,F,68)==EFI_SUCCESS); assert(C.KeyCount==1); Get(&C);
  for(I=0;I<PIANO_POGO_KEY_QUEUE;I++) { EmptyKeys(&C); assert(FeedKey(&C,0,4)==EFI_SUCCESS); }
  EmptyKeys(&C); assert(FeedKey(&C,0,5)==EFI_OUT_OF_RESOURCES); assert(C.KeyCount==64 && C.QueueOverflows==1 && C.PreviousKeys[0]==0);
  Get(&C); assert(FeedKey(&C,0,5)==EFI_SUCCESS);
  PianoPogoReset(&C);
  for(I=0;I<32;I++) { Frame(F); F[3]=6; Word(F+4,(UINT16)(I+1)); assert(PianoPogoFeedFrame(&C,F,68)==EFI_SUCCESS); }
  assert(C.ControlCount==16 && C.DroppedControls==16);
}

STATIC VOID Contact(UINT8 *P, UINT8 Id, UINT16 X, UINT16 Y) {
  P[0]=7; P[1]=Id; Word(P+2,32); Word(P+4,X); Word(P+6,Y);
}
STATIC VOID TestTouchAndDetach(VOID) {
  PIANO_POGO_INPUT C; UINT8 F[68]; EFI_ABSOLUTE_POINTER_STATE A; EFI_SIMPLE_POINTER_STATE M;
  PIANO_POGO_CONTROL_EVENT E; PianoPogoReset(&C); Frame(F); F[3]=0x19;
  Contact(F+5,4,3199,2135); F[29]=1;
  assert(PianoPogoFeedFrame(&C,F,68)==EFI_SUCCESS); assert(PianoPogoReadAbsolute(&C,&A)==EFI_SUCCESS);
  assert(A.CurrentX==3199 && A.CurrentY==2135 && !A.CurrentZ && (A.ActiveButtons&EFI_ABSP_TouchActive));
  Contact(F+5,5,10,20); Contact(F+13,4,100,200); F[29]=2;
  assert(PianoPogoFeedFrame(&C,F,68)==EFI_SUCCESS); PianoPogoReadAbsolute(&C,&A); assert(A.CurrentX==100 && C.PrimaryContactId==4);
  Contact(F+21,6,3200,0); F[29]=3;
  assert(PianoPogoFeedFrame(&C,F,68)==EFI_COMPROMISED_DATA); assert(C.PrimaryContactId==4);
  F[29]=4; assert(PianoPogoFeedFrame(&C,F,68)==EFI_UNSUPPORTED);
  Frame(F); F[3]=0x19; assert(PianoPogoFeedFrame(&C,F,68)==EFI_SUCCESS); PianoPogoReadAbsolute(&C,&A); assert(!A.ActiveButtons);
  assert(FeedKey(&C,0,4)==EFI_SUCCESS); Frame(F); F[3]=0x22; F[5]=0x38; F[6]=0x80; F[7]=0xa2; F[12]=0;
  assert(PianoPogoFeedFrame(&C,F,68)==EFI_SUCCESS); assert(C.AttachKnown&&!C.Attached&&!C.KeyCount);
  assert(PianoPogoReadPointer(&C,&M)==EFI_SUCCESS&&!M.LeftButton&&!M.RightButton);
  assert(PianoPogoReadControl(&C,&E)==EFI_SUCCESS && E.Kind==PianoPogoAttachment && !E.Value);
  assert(FeedKey(&C,0,5)==EFI_SUCCESS&&!C.KeyCount);
  F[12]=3; assert(PianoPogoFeedFrame(&C,F,68)==EFI_SUCCESS); assert(C.Attached); PianoPogoReadControl(&C,&E);
  assert(PianoPogoFeedFrame(&C,F,68)==EFI_SUCCESS&&!C.ControlCount); // same status coalesces.
}

STATIC PIANO_POGO_ADAPTER *Active;
STATIC UINTN Notified,Signals;
STATIC EFI_STATUS EFIAPI Notify(EFI_KEY_DATA *Key) {
  UINT8 F[68]; assert(Key->Key.UnicodeChar=='a'); Notified++; Frame(F);
  assert(PianoPogoFeedAdapter(Active,F,68)==EFI_NOT_READY); return EFI_SUCCESS;
}
STATIC VOID Signal(VOID *Context, EFI_EVENT Event) { assert(Context==Active&&Event!=NULL); Signals++; }
STATIC VOID TestProtocols(VOID) {
  PIANO_POGO_ADAPTER C; EFI_SIMPLE_POINTER_MODE Mode={8,8,1,TRUE,TRUE}; EFI_KEY_DATA Match={0},K;
  EFI_INPUT_KEY Plain; EFI_KEY_TOGGLE_STATE Toggle; VOID *Handle,*Duplicate; UINT8 F[68];
  Active=&C; Match.Key.UnicodeChar='a';
  assert(PianoPogoInitializeAdapter(&C,&Mode,(VOID *)1,(VOID *)2,(VOID *)3,(VOID *)4,Signal,&C)==EFI_SUCCESS);
  assert(C.Absolute.Mode->AbsoluteMaxX==3199&&C.Absolute.Mode->AbsoluteMaxY==2135);
  assert(C.TextEx.RegisterKeyNotify(&C.TextEx,&Match,Notify,&Handle)==EFI_SUCCESS);
  assert(C.TextEx.RegisterKeyNotify(&C.TextEx,&Match,Notify,&Duplicate)==EFI_SUCCESS&&Handle==Duplicate);
  Frame(F); F[3]=5; F[6]=4; assert(PianoPogoFeedAdapter(&C,F,68)==EFI_SUCCESS);
  assert(Notified==1&&Signals==2); assert(C.TextEx.ReadKeyStrokeEx(&C.TextEx,&K)==EFI_SUCCESS&&K.Key.UnicodeChar=='a');
  assert(C.Text.ReadKeyStroke(&C.Text,&Plain)==EFI_NOT_READY);
  assert(C.TextEx.UnregisterKeyNotify(&C.TextEx,Handle)==EFI_SUCCESS);
  assert(C.TextEx.UnregisterKeyNotify(&C.TextEx,Handle)==EFI_INVALID_PARAMETER);
  assert(C.TextEx.Reset(&C.TextEx,TRUE)==EFI_UNSUPPORTED);
  assert(C.TextEx.Reset(&C.TextEx,FALSE)==EFI_SUCCESS);
  Toggle=EFI_TOGGLE_STATE_VALID|EFI_KEY_STATE_EXPOSED; assert(C.TextEx.SetState(&C.TextEx,&Toggle)==EFI_SUCCESS);
  Frame(F); F[3]=5; F[4]=2; assert(PianoPogoFeedAdapter(&C,F,68)==EFI_SUCCESS);
  assert(C.Text.ReadKeyStroke(&C.Text,&Plain)==EFI_NOT_READY); // no fabricated char for modifiers.
}

STATIC VOID TestAuthAndPiRead(VOID) {
  PIANO_POGO_INPUT C; PIANO_POGO_CONTROL_EVENT E; UINT8 F[68],Reg; PIANO_POGO_I2C_REQUEST R;
  PianoPogoReset(&C); Frame(F); F[3]=0x24; F[5]=0x38; F[6]=0x80; F[7]=0x32;
  memset(F+25,0xab,16); assert(PianoPogoFeedFrame(&C,F,68)==EFI_SUCCESS);
  assert(PianoPogoReadControl(&C,&E)==EFI_SUCCESS&&E.Kind==PianoPogoAuthChallengeAvailable);
  assert(E.Value==0 && E.X==0 && E.Y==0 && E.Z==0);
  assert(PianoPogoMakeRuntimeRead(&R,&Reg,F,sizeof(F))==EFI_SUCCESS);
  assert(Reg==0x4c&&R.OperationCount==2&&R.Operation[0].LengthInBytes==1&&!R.Operation[0].Flags);
  assert(R.Operation[1].Flags==I2C_FLAG_READ&&R.Operation[1].LengthInBytes==68&&R.Operation[1].Buffer==F);
  assert(PianoPogoMakeRuntimeRead(&R,&Reg,F,67)==EFI_INVALID_PARAMETER);
}

int main(void) {
  TestKeys(); TestPackedAndAtomic(); TestRolloverAndQueues(); TestTouchAndDetach(); TestProtocols(); TestAuthAndPiRead();
  puts("Piano Pogo real UEFI ABI: packed/key/pointer/touch/control/bounds/queue/notify/PI read PASS"); return 0;
}
