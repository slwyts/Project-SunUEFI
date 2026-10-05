// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual USB diagnostic router; capture backend contract is a CPU-only mock.
#define PIANO_USB_SCREENSHOT 1
#define USB_FASTBOOT_TEST_MAIN usb_bulk_suite
#include "test_usb_fastboot.c"
static BOOLEAN capture_fail;
static UINT8 bmp[70];
EFI_STATUS PianoFastbootCaptureScreen(PIANO_FASTBOOT *State,UINT32 *Width,UINT32 *Height,UINT32 *Bytes,UINT32 *Crc) {
  *Width=*Height=*Bytes=*Crc=0;if(capture_fail)return EFI_DEVICE_ERROR;
  EFI_STATUS S=PianoFastbootStageCopy(State,bmp,sizeof(bmp));
  if(S==EFI_SUCCESS){*Width=*Height=2;*Bytes=sizeof(bmp);*Crc=DiagCrc(bmp,sizeof(bmp));}return S;
}
int main(void) {
  for(UINTN I=0;I<sizeof(bmp);++I)bmp[I]=(UINT8)(I*73+19);
  bmp[0]='B';bmp[1]='M';
  model_init();configure();cmd("oem screenshot","OKAY");
  assert(mScreenWidth==2 && mScreenHeight==2 && mScreenBytes==70 && mScreenCrc==DiagCrc(bmp,70) && mScreenGeneration==1);
  cmd("getvar:SunUEFI:screen-width","OKAY00000002");cmd("getvar:SunUEFI:screen-height","OKAY00000002");
  cmd("getvar:SunUEFI:screen-size","OKAY00000046");cmd("getvar:SunUEFI:screen-generation","OKAY00000001");
  CHAR8 Crc[13]="OKAY";FastHex(mScreenCrc,Crc+4);cmd("getvar:SunUEFI:screen-crc32",Crc);
  out("upload",6);drain();assert(wire_bytes==86 && !memcmp(wire,"DATA00000046",12) && !memcmp(wire+12,bmp,70) && !memcmp(wire+82,"OKAY",4));
  cmd("oem screenshot","OKAY");assert(mScreenGeneration==2);cmd("oem discard","OKAY");
  assert(!mScreenWidth && !mScreenHeight && !mScreenBytes && !mScreenCrc && !mScreenGeneration && !mFastboot.Upload);
  capture_fail=TRUE;cmd("oem screenshot","FAILGOP screenshot unavailable");assert(!mFastboot.Upload && !mScreenBytes);capture_fail=FALSE;
  cmd("oem screenshot","OKAY");cmd("download:00000001","DATA00000001");assert(!mScreenBytes && !mScreenGeneration);
  out("X",1);drain();cmd("oem discard","OKAY");cmd("oem screenshot","OKAY");
  out("upload",6);assert(mPending[3]);UINT8 Active[12];CopyMem(Active,mBulkTx.Cpu,12);
  assert(Event(0x101)==EFI_SUCCESS);assert(!mScreenBytes && !mScreenGeneration && !mFastboot.Upload && !mFrames && mEnding[3]);
  assert(mBulkTx.Active && !memcmp(Active,mBulkTx.Cpu,12));end(3);
  ClearFastboot();for(UINTN I=0;I<ARRAY_SIZE(all);++I){if(all[I]->Active)assert(PianoDmaComplete(all[I],EFI_SUCCESS,TRUE)==EFI_SUCCESS);assert(PianoDmaFree(all[I])==EFI_SUCCESS);}
  puts("USB screenshot route actual source: capture-only success, five metadata variables, independent staged upload, failed capture, reset/discard/download clearing and active IN bounce preservation passed.");
  return 0;
}
