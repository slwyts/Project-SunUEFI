// SPDX-License-Identifier: GPL-2.0-only
// Host tests execute the actual transport/input source with mocked MMIO.
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/input/PianoKeys.c"

EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiSimpleTextInProtocolGuid={0};EFI_GUID gEfiSimpleTextInputExProtocolGuid={0};
EFI_GUID gEfiConsoleInDeviceGuid={0};EFI_GUID gEfiDevicePathProtocolGuid={0};
STATIC EFI_BOOT_SERVICES Services;
STATIC UINT32 Maps[8],Owners[8],ReadStatus=1;
STATIC UINT8 PonValue,GpioValue=1;
STATIC UINTN Writes,Stalls,Signals;
STATIC UINT8 Tree;
STATIC EFI_EVENT_NOTIFY WaitCallback;
STATIC BOOLEAN Signalled;
STATIC UINTN Notices,Closed;

BOOLEAN EFIAPI DebugPrintEnabled(VOID) {return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) {return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...) { }
VOID *EFIAPI ZeroMem(VOID *Buffer,UINTN Bytes) {return memset(Buffer,0,Bytes);}
INT32 EFIAPI FdtPathOffset(CONST VOID *Fdt,CONST CHAR8 *Path) {
  assert(Fdt==&Tree && !strcmp(Path,"/soc/qcom,spmi@c42d000"));return 1;
}
CONST VOID *EFIAPI FdtGetProp(CONST VOID *Fdt,INT32 Node,CONST CHAR8 *Name,INT32 *Length) {
  static UINT32 Reg[]={ARB_CFG,0x4000,ARB_CORE,0x3000,0x0C500000,0x400000,ARB_OBS,0x80000,0x0C4C0000,0x10000};
  static UINT32 Encoded[ARRAY_SIZE(Reg)],Zero;
  assert(Fdt==&Tree && Node==1);
  if(!strcmp(Name,"reg")) {
    for(UINTN I=0;I<ARRAY_SIZE(Reg);++I)Encoded[I]=__builtin_bswap32(Reg[I]);
    *Length=sizeof(Encoded);return Encoded;
  }
  assert(!strcmp(Name,"qcom,ee") || !strcmp(Name,"qcom,bus-id"));*Length=4;return &Zero;
}

UINT32 EFIAPI MmioRead32(UINTN Address) {
  if(Address==ARB_CORE)return 0x70000000;
  if(Address==ARB_CORE+4)return ARRAY_SIZE(Maps);
  if(Address>=ARB_CORE+0x2000 && Address<ARB_CORE+0x2020)
    return Maps[(Address-ARB_CORE-0x2000)/4];
  if(Address>=ARB_CFG && Address<ARB_CFG+0x20)
    return Owners[(Address-ARB_CFG)/4];
  if(Address==ARB_OBS+0x20*mPonApid+8 || Address==ARB_OBS+0x20*mGpioApid+8)
    return ReadStatus;
  if(Address==ARB_OBS+0x20*mPonApid+0x18)return PonValue;
  if(Address==ARB_OBS+0x20*mGpioApid+0x18)return GpioValue;
  assert(!"Unexpected MMIO read");return 0;
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value) {
  assert(Address==ARB_OBS+0x20*mPonApid || Address==ARB_OBS+0x20*mGpioApid);
  // Only EXT_READL, one byte, offset 0x10 is permitted, including every poll.
  assert(Value==((1U<<27)|(0x10U<<4)));
  ++Writes;return Value;
}
STATIC EFI_STATUS EFIAPI StallMock(UINTN Us) {assert(Us==1);++Stalls;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI SignalMock(EFI_EVENT Event) {assert(Event==(VOID*)1);++Signals;Signalled=TRUE;return EFI_SUCCESS;}
STATIC EFI_TPL EFIAPI RaiseMock(EFI_TPL Tpl) {return TPL_APPLICATION;}
STATIC VOID EFIAPI RestoreMock(EFI_TPL Tpl) { }
STATIC EFI_STATUS EFIAPI TimerMock(EFI_EVENT Event,EFI_TIMER_DELAY Type,UINT64 Trigger) {
  assert(Event==(VOID*)2);
  assert((Type==TimerCancel && !Trigger) || (Type==TimerPeriodic && Trigger==200000));return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI CreateMock(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,VOID *Context,EFI_EVENT *Event) {
  assert(!Context && Notify);
  if(Type==EVT_NOTIFY_WAIT) {assert(Tpl==TPL_NOTIFY);WaitCallback=Notify;*Event=(VOID*)1;}
  else {assert(Type==(EVT_TIMER|EVT_NOTIFY_SIGNAL) && Tpl==TPL_CALLBACK);*Event=(VOID*)2;}
  return EFI_SUCCESS;
}
STATIC VOID CheckProtocols(va_list Args) {
  assert(va_arg(Args,EFI_GUID*)==&gEfiSimpleTextInProtocolGuid && va_arg(Args,VOID*)==&mInput);
  assert(va_arg(Args,EFI_GUID*)==&gEfiSimpleTextInputExProtocolGuid && va_arg(Args,VOID*)==&mInputEx);
  assert(va_arg(Args,EFI_GUID*)==&gEfiConsoleInDeviceGuid && va_arg(Args,VOID*)==NULL);
  assert(va_arg(Args,EFI_GUID*)==&gEfiDevicePathProtocolGuid && va_arg(Args,VOID*)==&mPath);
  assert(va_arg(Args,VOID*)==NULL);
}
STATIC EFI_STATUS EFIAPI InstallMock(EFI_HANDLE *Handle,...) {
  va_list Args;va_start(Args,Handle);CheckProtocols(Args);va_end(Args);
  assert(mInputEx.WaitForKeyEx==mInput.WaitForKey && mInputEx.Reset && mInputEx.ReadKeyStrokeEx &&
    mInputEx.SetState && mInputEx.RegisterKeyNotify && mInputEx.UnregisterKeyNotify);
  *Handle=(VOID*)3;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI UninstallMock(EFI_HANDLE Handle,...) {
  assert(Handle==(VOID*)3);va_list Args;va_start(Args,Handle);CheckProtocols(Args);va_end(Args);return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ConnectMock(EFI_HANDLE Handle,EFI_HANDLE *Drivers,EFI_DEVICE_PATH_PROTOCOL *Path,BOOLEAN Recursive) {
  assert(Handle==(VOID*)3 && !Drivers && !Path && Recursive);return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI DisconnectMock(EFI_HANDLE Handle,EFI_HANDLE Driver,EFI_HANDLE Child) {
  assert(Handle==(VOID*)3 && !Driver && !Child);return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI CloseMock(EFI_EVENT Event) {
  assert(Event==(VOID*)1 || Event==(VOID*)2);++Closed;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Notice(EFI_KEY_DATA *Key) {
  assert(Key->Key.ScanCode==SCAN_UP && !Key->Key.UnicodeChar && !Key->KeyState.KeyShiftState && !Key->KeyState.KeyToggleState);
  ++Notices;return EFI_SUCCESS;
}

int main(void) {
  Services.Stall=StallMock;Services.SignalEvent=SignalMock;
  Services.RaiseTPL=RaiseMock;Services.RestoreTPL=RestoreMock;Services.SetTimer=TimerMock;
  Services.CreateEvent=CreateMock;Services.InstallMultipleProtocolInterfaces=InstallMock;
  Services.UninstallMultipleProtocolInterfaces=UninstallMock;Services.ConnectController=ConnectMock;
  Services.DisconnectController=DisconnectMock;Services.CloseEvent=CloseMock;
  gBS=&Services;
  Maps[0]=0x013<<8;Owners[0]=3;
  Maps[1]=0x18D<<8;Owners[1]=0;
  Maps[2]=0x013<<8;Owners[2]=0;
  Maps[3]=0x013<<8;Owners[3]=2;
  assert(FindApid(0x013,4,&mPonApid)==EFI_SUCCESS && mPonApid==2);
  assert(FindApid(0x18D,4,&mGpioApid)==EFI_SUCCESS && mGpioApid==1);
  UINT16 Missing;assert(FindApid(0x071,4,&Missing)==EFI_ACCESS_DENIED);
  Owners[2]=3;assert(FindApid(0x013,4,&Missing)==EFI_ACCESS_DENIED);Owners[2]=0;
  assert(PianoStartKeys(&Tree)==EFI_SUCCESS && WaitCallback && mHandle==(VOID*)3);

  UINT8 Value;
  UINTN Before=Writes;assert(ReadKeyByte(0x7100,&Value)==EFI_ACCESS_DENIED && Writes==Before);
  assert(ReadKeyByte(0x1310,&Value)==EFI_SUCCESS && Value==0);
  ReadStatus=5;assert(ReadKeyByte(0x1310,&Value)==EFI_DEVICE_ERROR);
  ReadStatus=0;Stalls=0;assert(ReadKeyByte(0x1310,&Value)==EFI_TIMEOUT && Stalls==1000);
  ReadStatus=1;
  UINT8 State;assert(Sample(&State)==EFI_SUCCESS && State==0);
  PonValue=0xC0;GpioValue=0;assert(Sample(&State)==EFI_SUCCESS && State==7);

  mStable=mCandidate=0;mSamples=2;mHead=mTail=0;
  PonValue=0;GpioValue=0;
  Poll(NULL,NULL);assert(mHead==mTail); // One sample is not a press.
  Poll(NULL,NULL);assert(mTail==1 && mQueue[0].ScanCode==SCAN_VOLUME_UP);
  Poll(NULL,NULL);assert(mTail==1); // Holding does not duplicate the event.
  GpioValue=1;Poll(NULL,NULL);Poll(NULL,NULL);
  PonValue=0x40;Poll(NULL,NULL);Poll(NULL,NULL);
  assert(mTail==2 && mQueue[1].ScanCode==SCAN_VOLUME_DOWN);
  PonValue=0;Poll(NULL,NULL);Poll(NULL,NULL);
  PonValue=0x80;Poll(NULL,NULL);Poll(NULL,NULL);
  assert(mTail==2); // Power emits Enter on release, not press.
  PonValue=0;Poll(NULL,NULL);Poll(NULL,NULL);
  assert(mTail==3 && mQueue[2].UnicodeChar==CHAR_CARRIAGE_RETURN);
  PonValue=0x80;Poll(NULL,NULL);Poll(NULL,NULL);
  for(int I=0;I<55;++I)Poll(NULL,NULL);
  PonValue=0;Poll(NULL,NULL);Poll(NULL,NULL);assert(mTail==3);
  // A failed physical read stops polling instead of inventing input.
  ReadStatus=5;Poll(NULL,NULL);assert(mFailed);
  EFI_INPUT_KEY Key;assert(Read(&mInput,&Key)==EFI_SUCCESS && Key.ScanCode==SCAN_VOLUME_UP);
  assert(Read(&mInput,&Key)==EFI_SUCCESS && Key.ScanCode==SCAN_VOLUME_DOWN);
  assert(Read(&mInput,&Key)==EFI_SUCCESS && Key.UnicodeChar==CHAR_CARRIAGE_RETURN);
  assert(Read(&mInput,&Key)==EFI_DEVICE_ERROR);
  ReadStatus=1;mFailed=FALSE;PonValue=0;GpioValue=1;
  mStable=mCandidate=0;mSamples=2;
  assert(PianoSetStandardKeyNavigation(TRUE)==FALSE);
  GpioValue=0;Poll(NULL,NULL);Poll(NULL,NULL);
  assert(Read(&mInput,&Key)==EFI_SUCCESS && Key.ScanCode==SCAN_UP);
  GpioValue=1;Poll(NULL,NULL);Poll(NULL,NULL);
  PonValue=0x40;Poll(NULL,NULL);Poll(NULL,NULL);
  assert(Read(&mInput,&Key)==EFI_SUCCESS && Key.ScanCode==SCAN_DOWN);
  GpioValue=0;Poll(NULL,NULL);Poll(NULL,NULL);
  assert(Read(&mInput,&Key)==EFI_SUCCESS && Key.ScanCode==SCAN_ESC);
  Poll(NULL,NULL);assert(Read(&mInput,&Key)==EFI_NOT_READY);
  GpioValue=1;Poll(NULL,NULL);Poll(NULL,NULL);assert(Read(&mInput,&Key)==EFI_NOT_READY);
  PonValue=0;Poll(NULL,NULL);Poll(NULL,NULL);
  assert(PianoSetStandardKeyNavigation(FALSE)==TRUE);
  // ConSplitter can signal its Ex wait from the simple input event. Reading
  // through Ex must consume that same key instead of repeatedly waking empty.
  EFI_KEY_DATA Data;memset(&Data,0xff,sizeof(Data));
  Push(SCAN_VOLUME_UP,0);assert(Signalled);Signalled=FALSE;
  WaitCallback(mInput.WaitForKey,NULL);assert(Signalled);Signalled=FALSE;
  assert(mInputEx.ReadKeyStrokeEx(&mInputEx,&Data)==EFI_SUCCESS && Data.Key.ScanCode==SCAN_VOLUME_UP);
  assert(!Data.KeyState.KeyShiftState && !Data.KeyState.KeyToggleState);
  WaitCallback(mInputEx.WaitForKeyEx,NULL);assert(!Signalled);
  assert(Read(&mInput,&Key)==EFI_NOT_READY);
  Push(SCAN_DOWN,0);assert(Read(&mInput,&Key)==EFI_SUCCESS && Key.ScanCode==SCAN_DOWN);
  assert(mInputEx.ReadKeyStrokeEx(&mInputEx,&Data)==EFI_NOT_READY && !Data.Key.ScanCode);
  Push(0,CHAR_CARRIAGE_RETURN);
  assert(mInputEx.Reset(&mInputEx,FALSE)==EFI_SUCCESS && ReadEx(&mInputEx,&Data)==EFI_NOT_READY);
  assert(ReadEx(NULL,&Data)==EFI_INVALID_PARAMETER && ReadEx(&mInputEx,NULL)==EFI_INVALID_PARAMETER);
  EFI_KEY_TOGGLE_STATE Toggle=EFI_TOGGLE_STATE_VALID;
  assert(mInputEx.SetState(&mInputEx,&Toggle)==EFI_UNSUPPORTED && SetStateEx(&mInputEx,NULL)==EFI_INVALID_PARAMETER);
  EFI_KEY_DATA Match={.Key={SCAN_UP,0}};VOID *Handles[KEY_NOTIFIES],*Full=(VOID*)1;
  VOID *Duplicate;
  assert(RegisterNotify(&mInputEx,&Match,Notice,&Handles[0])==EFI_SUCCESS);
  assert(RegisterNotify(&mInputEx,&Match,Notice,&Duplicate)==EFI_SUCCESS && Duplicate==Handles[0]);
  assert(UnregisterNotify(&mInputEx,Duplicate)==EFI_SUCCESS);
  assert(UnregisterNotify(&mInputEx,Handles[0])==EFI_INVALID_PARAMETER);
  for(UINTN I=0;I<ARRAY_SIZE(Handles);++I) {
    Match.Key.ScanCode=SCAN_UP+(UINT16)I;
    assert(RegisterNotify(&mInputEx,&Match,Notice,&Handles[I])==EFI_SUCCESS);
    for(UINTN J=0;J<I;++J)assert(Handles[I]!=Handles[J]);
  }
  Match.Key.ScanCode=SCAN_UP+KEY_NOTIFIES;
  assert(RegisterNotify(&mInputEx,&Match,Notice,&Full)==EFI_OUT_OF_RESOURCES && !Full);
  Push(SCAN_UP,0);assert(Notices==1 && ReadEx(&mInputEx,&Data)==EFI_SUCCESS);
  for(UINTN I=0;I<ARRAY_SIZE(Handles);++I)assert(UnregisterNotify(&mInputEx,Handles[I])==EFI_SUCCESS);
  assert(UnregisterNotify(&mInputEx,Handles[0])==EFI_INVALID_PARAMETER);
  Push(SCAN_UP,0);assert(Notices==1 && Read(&mInput,&Key)==EFI_SUCCESS);
  PianoStopKeys();assert(Closed==2 && !mInput.WaitForKey && !mInputEx.WaitForKeyEx && !mHandle && !mPoll);
  printf("Read-only MMIO, APID ownership, errors/timeouts, debounce and key mapping passed (%lu read commands).\n",(unsigned long)Writes);
  return 0;
}
