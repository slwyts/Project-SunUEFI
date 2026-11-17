// SPDX-License-Identifier: BSD-2-Clause-Patent
// Standard BlockIO; transport callbacks own hardware permissions and DMA.
#include "PianoBlockIo.h"
#include <Library/BaseMemoryLib.h>
#define BLOCK_SIGNATURE SIGNATURE_32('P','R','O','B')
STATIC PIANO_BLOCK_IO *Device(EFI_BLOCK_IO_PROTOCOL *This) {
  if(This==NULL)return NULL;
  PIANO_BLOCK_IO *D=BASE_CR(This,PIANO_BLOCK_IO,Block);
  return D->Signature==BLOCK_SIGNATURE && This->Media==&D->Media?D:NULL;
}
STATIC EFI_STATUS EFIAPI Reset(EFI_BLOCK_IO_PROTOCOL *This,BOOLEAN Extended) {
  PIANO_BLOCK_IO *D=Device(This);
  return D==NULL?EFI_INVALID_PARAMETER:D->Media.MediaPresent?EFI_SUCCESS:EFI_NO_MEDIA;
}
STATIC EFI_STATUS EFIAPI Read(EFI_BLOCK_IO_PROTOCOL *This,UINT32 MediaId,EFI_LBA Lba,UINTN Bytes,VOID *Buffer) {
  PIANO_BLOCK_IO *D=Device(This);if(D==NULL)return EFI_INVALID_PARAMETER;
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
  PIANO_BLOCK_IO *D=Device(This);if(D==NULL)return EFI_INVALID_PARAMETER;
  if(!D->Media.MediaPresent)return EFI_NO_MEDIA;
  if(MediaId!=D->Media.MediaId)return EFI_MEDIA_CHANGED;
  if(Bytes==0)return EFI_SUCCESS;
  if(D->Media.ReadOnly || D->Write==NULL)return EFI_WRITE_PROTECTED;
  if(Buffer==NULL)return EFI_INVALID_PARAMETER;
  if(Bytes%D->Media.BlockSize)return EFI_BAD_BUFFER_SIZE;
  UINTN Blocks=Bytes/D->Media.BlockSize;
  if(Lba>D->Media.LastBlock || Blocks-1>D->Media.LastBlock-Lba)return EFI_INVALID_PARAMETER;
  return D->Write(D->Context,D->Lun,Lba,Bytes,Buffer);
}
STATIC EFI_STATUS EFIAPI Flush(EFI_BLOCK_IO_PROTOCOL *This) {
  PIANO_BLOCK_IO *D=Device(This);
  if(D==NULL)return EFI_INVALID_PARAMETER;
  if(!D->Media.MediaPresent)return EFI_NO_MEDIA;
  return D->Flush==NULL?EFI_SUCCESS:D->Flush(D->Context,D->Lun);
}
EFI_STATUS PianoBlockIoInit(PIANO_BLOCK_IO *D,UINT8 Lun,UINT32 BlockBytes,EFI_LBA LastLba,
                                 VOID *Context,PIANO_BLOCK_READ ReadCallback,
                                 PIANO_BLOCK_READ WriteCallback,PIANO_BLOCK_FLUSH FlushCallback) {
  if(D==NULL || ReadCallback==NULL || Lun>7 || (BlockBytes!=512 && BlockBytes!=4096) ||
     LastLba<1 || LastLba==MAX_UINT64 || ((WriteCallback==NULL)!=(FlushCallback==NULL)))return EFI_INVALID_PARAMETER;
  ZeroMem(D,sizeof(*D));D->Signature=BLOCK_SIGNATURE;D->Lun=Lun;D->Context=Context;D->Read=ReadCallback;
  D->Write=WriteCallback;D->Flush=FlushCallback;
  D->Media=(EFI_BLOCK_IO_MEDIA){.MediaId=1,.MediaPresent=TRUE,.ReadOnly=WriteCallback==NULL,.BlockSize=BlockBytes,
    .IoAlign=1,.LastBlock=LastLba,.LogicalBlocksPerPhysicalBlock=1};
  D->Block=(EFI_BLOCK_IO_PROTOCOL){.Revision=EFI_BLOCK_IO_PROTOCOL_REVISION3,.Media=&D->Media,
    .Reset=Reset,.ReadBlocks=Read,.WriteBlocks=Write,.FlushBlocks=Flush};return EFI_SUCCESS;
}
