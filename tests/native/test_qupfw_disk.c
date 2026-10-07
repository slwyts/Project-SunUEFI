// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/PianoQupFwDisk.c"
EFI_BOOT_SERVICES *gBS;
BOOLEAN EFIAPI DebugPrintEnabled(VOID) {return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) {return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...) { }
VOID *EFIAPI CopyMem(VOID *A,CONST VOID *B,UINTN Bytes) {return memcpy(A,B,Bytes);}
int main(void) {
  mFirmware=malloc(0x20000);UINT8 *Output=malloc(0x20000);assert(mFirmware && Output);mFirmwareBytes=0x20000;
  for(UINTN I=0;I<mFirmwareBytes;++I)mFirmware[I]=(UINT8)(I*7);
  assert(Read(&mBlock,1,0,0x20000,Output)==EFI_SUCCESS && memcmp(Output,mFirmware,0x20000)==0);
  assert(Read(&mBlock,1,255,512,Output)==EFI_SUCCESS && memcmp(Output,mFirmware+255*512,512)==0);
  memset(Output,0x5A,0x20000);
  assert(Read(&mBlock,1,256,512,Output)==EFI_INVALID_PARAMETER);
  assert(Read(&mBlock,1,255,1024,Output)==EFI_INVALID_PARAMETER);
  assert(Read(&mBlock,1,0,513,Output)==EFI_BAD_BUFFER_SIZE);
  assert(Read(&mBlock,2,0,512,Output)==EFI_MEDIA_CHANGED);
  assert(Read(&mBlock,1,0,512,NULL)==EFI_INVALID_PARAMETER);
  assert(Read(&mBlock,1,0,0,NULL)==EFI_SUCCESS);
  for(UINTN I=0;I<0x20000;++I)assert(Output[I]==0x5A);
  mMedia.ReadOnly=FALSE; // Prove a caller changing the flag cannot enable writes.
  assert(Write(&mBlock,1,0,0x20000,Output)==EFI_WRITE_PROTECTED);
  for(UINTN I=0;I<0x20000;++I)assert(mFirmware[I]==(UINT8)(I*7));
  assert(mPath.Partition.PartitionSize==256 && mPath.Partition.SignatureType==SIGNATURE_TYPE_GUID);
  puts("Firmware RAM BlockIO bounds, media checks and unconditional underlying write protection passed.");
  free(Output);free(mFirmware);return 0;
}
