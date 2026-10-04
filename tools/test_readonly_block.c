// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoReadOnlyBlock.c"
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
static unsigned calls;static EFI_STATUS failure;
static EFI_STATUS transport(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,VOID *Buffer){
  assert(Context==(void *)123 && Lun==4);++calls;
  if(failure)return failure;
  memset(Buffer,(unsigned)Lba,Bytes);return EFI_SUCCESS;
}
int main(void){
  PIANO_READ_ONLY_BLOCK d;UINT8 data[8193];EFI_BLOCK_IO_PROTOCOL *b=&d.Block;
  assert(PianoReadOnlyBlockInit(&d,4,4096,0x100000000ULL,(void *)123,transport)==EFI_SUCCESS);
  assert(b->Media->ReadOnly && b->Media->IoAlign==1 && !b->Media->LogicalPartition);
  assert(b->ReadBlocks(b,1,MAX_UINT64,0,NULL)==EFI_SUCCESS && !calls);
  assert(b->ReadBlocks(b,2,0,4096,data)==EFI_MEDIA_CHANGED && !calls);
  assert(b->ReadBlocks(b,1,0,512,data)==EFI_BAD_BUFFER_SIZE && !calls);
  assert(b->ReadBlocks(b,1,0x100000000ULL,8192,data)==EFI_INVALID_PARAMETER && !calls);
  assert(b->ReadBlocks(b,1,0,4096,NULL)==EFI_INVALID_PARAMETER && !calls);
  assert(b->ReadBlocks(b,1,7,8192,data+1)==EFI_SUCCESS && calls==1 && data[1]==7);
  assert(b->ReadBlocks(b,1,0x100000000ULL,4096,data)==EFI_SUCCESS && calls==2);
  failure=EFI_DEVICE_ERROR;assert(b->ReadBlocks(b,1,0,4096,data)==EFI_DEVICE_ERROR);
  assert(b->WriteBlocks(b,1,0,4096,data)==EFI_WRITE_PROTECTED && calls==3);
  d.Media.MediaPresent=FALSE;assert(b->ReadBlocks(b,1,0,4096,data)==EFI_NO_MEDIA);
  assert(b->Reset(b,FALSE)==EFI_NO_MEDIA && b->FlushBlocks(b)==EFI_NO_MEDIA);
  puts("Read-only BlockIO: ABI, unaligned bounce clients, 64-bit/range bounds, MediaId, error propagation and unconditional write rejection passed.");
}
