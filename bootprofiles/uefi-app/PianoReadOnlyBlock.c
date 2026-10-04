// SPDX-License-Identifier: BSD-2-Clause-Patent
// Standard BlockIO with an unconditional write barrier. No WRITE CDB exists.
#include "PianoReadOnlyBlock.h"
#include <Library/BaseMemoryLib.h>
#define BLOCK_SIGNATURE SIGNATURE_32('P','R','O','B')
STATIC PIANO_READ_ONLY_BLOCK *Device(EFI_BLOCK_IO_PROTOCOL *This) {
  if(This==NULL)return NULL;
  PIANO_READ_ONLY_BLOCK *D=BASE_CR(This,PIANO_READ_ONLY_BLOCK,Block);
  return D->Signature==BLOCK_SIGNATURE && This->Media==&D->Media?D:NULL;
}
STATIC EFI_STATUS EFIAPI Reset(EFI_BLOCK_IO_PROTOCOL *This,BOOLEAN Extended) {
  PIANO_READ_ONLY_BLOCK *D=Device(This);
  return D==NULL?EFI_INVALID_PARAMETER:D->Media.MediaPresent?EFI_SUCCESS:EFI_NO_MEDIA;
}
STATIC EFI_STATUS EFIAPI Read(EFI_BLOCK_IO_PROTOCOL *This,UINT32 MediaId,EFI_LBA Lba,UINTN Bytes,VOID *Buffer) {
  PIANO_READ_ONLY_BLOCK *D=Device(This);if(D==NULL)return EFI_INVALID_PARAMETER;
  if(!D->Media.MediaPresent)return EFI_NO_MEDIA;
  if(MediaId!=D->Media.MediaId)return EFI_MEDIA_CHANGED;
  if(Bytes==0)return EFI_SUCCESS;
  if(Buffer==NULL)return EFI_INVALID_PARAMETER;
  if(Bytes%D->Media.BlockSize)return EFI_BAD_BUFFER_SIZE;
  UINTN Blocks=Bytes/D->Media.BlockSize;
  if(Lba>D->Media.LastBlock || Blocks-1>D->Media.LastBlock-Lba)return EFI_INVALID_PARAMETER;
  return D->Read(D->Context,D->Lun,Lba,Bytes,Buffer);
}
STATIC EFI_STATUS EFIAPI Write(EFI_BLOCK_IO_PROTOCOL *This,UINT32 MediaId,EFI_LBA Lba,UINTN Bytes,VOID *Buffer) {
  PIANO_READ_ONLY_BLOCK *D=Device(This);if(D==NULL)return EFI_INVALID_PARAMETER;
  if(!D->Media.MediaPresent)return EFI_NO_MEDIA;
  if(MediaId!=D->Media.MediaId)return EFI_MEDIA_CHANGED;
  return EFI_WRITE_PROTECTED;
}
STATIC EFI_STATUS EFIAPI Flush(EFI_BLOCK_IO_PROTOCOL *This) {
  PIANO_READ_ONLY_BLOCK *D=Device(This);
  return D==NULL?EFI_INVALID_PARAMETER:D->Media.MediaPresent?EFI_SUCCESS:EFI_NO_MEDIA;
}
EFI_STATUS PianoReadOnlyBlockInit(PIANO_READ_ONLY_BLOCK *D,UINT8 Lun,UINT32 BlockBytes,EFI_LBA LastLba,
                                 VOID *Context,PIANO_BLOCK_READ ReadCallback) {
  if(D==NULL || ReadCallback==NULL || Lun>7 || (BlockBytes!=512 && BlockBytes!=4096) ||
     LastLba<1 || LastLba==MAX_UINT64)return EFI_INVALID_PARAMETER;
  ZeroMem(D,sizeof(*D));D->Signature=BLOCK_SIGNATURE;D->Lun=Lun;D->Context=Context;D->Read=ReadCallback;
  D->Media=(EFI_BLOCK_IO_MEDIA){.MediaId=1,.MediaPresent=TRUE,.ReadOnly=TRUE,.BlockSize=BlockBytes,
    .IoAlign=1,.LastBlock=LastLba,.LogicalBlocksPerPhysicalBlock=1};
  D->Block=(EFI_BLOCK_IO_PROTOCOL){.Revision=EFI_BLOCK_IO_PROTOCOL_REVISION3,.Media=&D->Media,
    .Reset=Reset,.ReadBlocks=Read,.WriteBlocks=Write,.FlushBlocks=Flush};return EFI_SUCCESS;
}
