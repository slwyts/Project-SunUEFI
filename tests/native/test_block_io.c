// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/storage/PianoBlockIo.c"
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
static unsigned calls;static EFI_STATUS failure;
static EFI_STATUS transport(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,VOID *Buffer){
  assert(Context==(void *)123 && Lun==4);++calls;
  if(failure)return failure;
  memset(Buffer,(unsigned)Lba,Bytes);return EFI_SUCCESS;
}
static unsigned writes,flushes;
static EFI_STATUS write_transport(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,VOID *Buffer){
  assert(Context==(void *)123 && Lun==4 && Lba==7 && Bytes==4096 && Buffer);++writes;return failure;
}
static EFI_STATUS flush_transport(VOID *Context,UINT8 Lun){
  assert(Context==(void *)123 && Lun==4);++flushes;return failure;
}
int main(void){
  PIANO_BLOCK_IO d;UINT8 data[8193];EFI_BLOCK_IO_PROTOCOL *b=&d.Block;
  assert(PianoBlockIoInit(&d,4,4096,0x100000000ULL,(void *)123,transport,NULL,NULL)==EFI_SUCCESS);
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
  failure=EFI_SUCCESS;
  assert(PianoBlockIoInit(&d,4,4096,31,(void *)123,transport,write_transport,flush_transport)==EFI_SUCCESS);
  assert(!d.Media.ReadOnly && b->WriteBlocks(b,1,7,4096,data)==EFI_SUCCESS && writes==1);
  assert(b->WriteBlocks(b,2,7,4096,data)==EFI_MEDIA_CHANGED && writes==1);
  assert(b->WriteBlocks(b,1,31,8192,data)==EFI_INVALID_PARAMETER && writes==1);
  assert(b->WriteBlocks(b,1,0,1,data)==EFI_BAD_BUFFER_SIZE && writes==1);
  assert(b->WriteBlocks(b,1,0,4096,NULL)==EFI_INVALID_PARAMETER && writes==1);
  assert(b->WriteBlocks(b,1,MAX_UINT64,0,NULL)==EFI_SUCCESS && writes==1);
  assert(b->FlushBlocks(b)==EFI_SUCCESS && flushes==1);
  failure=EFI_DEVICE_ERROR;
  assert(b->WriteBlocks(b,1,7,4096,data)==EFI_DEVICE_ERROR && writes==2);
  assert(b->FlushBlocks(b)==EFI_DEVICE_ERROR && flushes==2);
  assert(PianoBlockIoInit(&d,4,4096,31,(void *)123,transport,write_transport,NULL)==EFI_INVALID_PARAMETER);
  puts("BlockIO: read/write bounds, media changes, read-only fallback, flush and hardware error propagation passed.");
}
