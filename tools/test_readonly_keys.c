// SPDX-License-Identifier: GPL-2.0-only
// Host tests execute the actual transport/input source with mocked MMIO.
#include <assert.h>
#include <stdio.h>
#include "../bootprofiles/uefi-app/PianoKeys.c"

EFI_BOOT_SERVICES *gBS;
STATIC EFI_BOOT_SERVICES Services;
STATIC UINT32 Maps[8],Owners[8],ReadStatus=1;
STATIC UINT8 PonValue,GpioValue=1;
STATIC UINTN Writes,Stalls,Signals;

BOOLEAN EFIAPI DebugPrintEnabled(VOID) {return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) {return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...) { }

UINT32 EFIAPI MmioRead32(UINTN Address) {
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
STATIC EFI_STATUS EFIAPI SignalMock(EFI_EVENT Event) {++Signals;return EFI_SUCCESS;}
STATIC EFI_TPL EFIAPI RaiseMock(EFI_TPL Tpl) {return TPL_APPLICATION;}
STATIC VOID EFIAPI RestoreMock(EFI_TPL Tpl) { }
STATIC EFI_STATUS EFIAPI TimerMock(EFI_EVENT Event,EFI_TIMER_DELAY Type,UINT64 Trigger) {
  assert(Type==TimerCancel);return EFI_SUCCESS;
}

int main(void) {
  Services.Stall=StallMock;Services.SignalEvent=SignalMock;
  Services.RaiseTPL=RaiseMock;Services.RestoreTPL=RestoreMock;Services.SetTimer=TimerMock;
  gBS=&Services;
  Maps[0]=0x013<<8;Owners[0]=3;
  Maps[1]=0x18D<<8;Owners[1]=0;
  Maps[2]=0x013<<8;Owners[2]=0;
  Maps[3]=0x013<<8;Owners[3]=2;
  assert(FindApid(0x013,4,&mPonApid)==EFI_SUCCESS && mPonApid==2);
  assert(FindApid(0x18D,4,&mGpioApid)==EFI_SUCCESS && mGpioApid==1);
  UINT16 Missing;assert(FindApid(0x071,4,&Missing)==EFI_ACCESS_DENIED);
  Owners[2]=3;assert(FindApid(0x013,4,&Missing)==EFI_ACCESS_DENIED);Owners[2]=0;

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
  printf("Read-only MMIO, APID ownership, errors/timeouts, debounce and key mapping passed (%lu read commands).\n",(unsigned long)Writes);
  return 0;
}
