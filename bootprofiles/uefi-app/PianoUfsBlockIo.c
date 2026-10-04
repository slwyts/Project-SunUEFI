// SPDX-License-Identifier: BSD-2-Clause-Patent
// Normal UFS data reads/writes, with a caller-selected write range. No erase,
// FORMAT UNIT, UNMAP, RPMB, provisioning, device descriptor or attribute writes.
#include "PianoUfsBlockIo.h"
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

#define UFS_BLOCK_SIGNATURE  SIGNATURE_32('P','U','F','S')
#define MAX_IO_BYTES  65536U
STATIC VOID Be(UINT8 *Out,UINT64 Value,UINTN Bytes) {
  for(UINTN I=0;I<Bytes;++I)Out[Bytes-1-I]=(UINT8)(Value>>(I*8));
}
STATIC UINT64 FromBe(CONST UINT8 *In,UINTN Bytes) {
  UINT64 Value=0;for(UINTN I=0;I<Bytes;++I)Value=(Value<<8)|In[I];return Value;
}
STATIC PIANO_UFS_BLOCK *DeviceFrom(EFI_BLOCK_IO_PROTOCOL *Block) {
  if(Block==NULL)return NULL;
  PIANO_UFS_BLOCK *D=BASE_CR(Block,PIANO_UFS_BLOCK,Block);
  return D->Signature==UFS_BLOCK_SIGNATURE?D:NULL;
}
STATIC EFI_STATUS Execute(PIANO_UFS_BLOCK *D,UINT8 *Cdb,UINT8 CdbBytes,
                          BOOLEAN Write,VOID *Data,UINT32 Bytes) {
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET P;UINT8 Target[TARGET_MAX_BYTES];
  ZeroMem(&P,sizeof(P));ZeroMem(Target,sizeof(Target));
  P.Timeout=50000000; // Five seconds, never an unbounded storage wait.
  P.Cdb=Cdb;P.CdbLength=CdbBytes;
  P.DataDirection=Write?1:0;
  if(Write){P.OutDataBuffer=Data;P.OutTransferLength=Bytes;}
  else {P.InDataBuffer=Data;P.InTransferLength=Bytes;}
  EFI_STATUS Status=D->Transport->PassThru(D->Transport,Target,D->ScsiLun,&P,NULL);
  if(!EFI_ERROR(Status) && (P.HostAdapterStatus!=0 || P.TargetStatus!=0 ||
     (Write?P.OutTransferLength:P.InTransferLength)!=Bytes))Status=EFI_DEVICE_ERROR;
  D->LastStatus=Status;return Status;
}
STATIC EFI_STATUS EFIAPI Reset(EFI_BLOCK_IO_PROTOCOL *Block,BOOLEAN Extended) {
  // Do not turn a filesystem's Reset call into a UFS controller/device reset.
  PIANO_UFS_BLOCK *D=DeviceFrom(Block);
  return D==NULL?EFI_INVALID_PARAMETER:(Extended?EFI_UNSUPPORTED:EFI_SUCCESS);
}
STATIC EFI_STATUS Transfer(EFI_BLOCK_IO_PROTOCOL *Block,UINT32 MediaId,
                          EFI_LBA Lba,UINTN Bytes,VOID *Buffer,BOOLEAN Write) {
  PIANO_UFS_BLOCK *D=DeviceFrom(Block);
  if(D==NULL)return EFI_INVALID_PARAMETER;
  if(MediaId!=D->Media.MediaId)return EFI_MEDIA_CHANGED;
  if(!D->Media.MediaPresent)return EFI_NO_MEDIA;
  if(Bytes==0)return EFI_SUCCESS;
  if(Buffer==NULL || (D->Media.IoAlign>1 && ((UINTN)Buffer&(D->Media.IoAlign-1))))return EFI_INVALID_PARAMETER;
  if(Bytes%D->Media.BlockSize)return EFI_BAD_BUFFER_SIZE;
  UINTN Blocks=Bytes/D->Media.BlockSize;
  if(Lba>D->Media.LastBlock || Blocks-1>D->Media.LastBlock-Lba)return EFI_INVALID_PARAMETER;
  if(Write && (!D->WritesEnabled || Lba<D->WriteFirst || Lba>D->WriteLast ||
     Blocks-1>D->WriteLast-Lba))return EFI_WRITE_PROTECTED;
  // Check the complete request before issuing any chunk; an out-of-range
  // suffix must not cause a partial write before rejection.
  while(Bytes!=0) {
    UINT32 Chunk=(UINT32)MIN(Bytes,(UINTN)MAX_IO_BYTES);
    Chunk-=Chunk%D->Media.BlockSize;
    UINT8 Cdb[16]={0};Cdb[0]=Write?0x8A:0x88; // WRITE(16) / READ(16).
    if(Write)Cdb[1]=0x08; // Force Unit Access: report completion after stable media.
    Be(Cdb+2,Lba,8);Be(Cdb+10,Chunk/D->Media.BlockSize,4);
    EFI_STATUS Status=Execute(D,Cdb,sizeof(Cdb),Write,Buffer,Chunk);
    if(EFI_ERROR(Status))return Status;
    Buffer=(UINT8 *)Buffer+Chunk;Lba+=Chunk/D->Media.BlockSize;Bytes-=Chunk;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Read(EFI_BLOCK_IO_PROTOCOL *B,UINT32 M,EFI_LBA L,UINTN N,VOID *Data) {
  return Transfer(B,M,L,N,Data,FALSE);
}
STATIC EFI_STATUS EFIAPI Write(EFI_BLOCK_IO_PROTOCOL *B,UINT32 M,EFI_LBA L,UINTN N,VOID *Data) {
  return Transfer(B,M,L,N,Data,TRUE);
}
STATIC EFI_STATUS EFIAPI Flush(EFI_BLOCK_IO_PROTOCOL *Block) {
  PIANO_UFS_BLOCK *D=DeviceFrom(Block);if(D==NULL)return EFI_INVALID_PARAMETER;
  if(!D->WritesEnabled)return EFI_SUCCESS;
  UINT8 Cdb[10]={0x35}; // SYNCHRONIZE CACHE(10); never an erase operation.
  return Execute(D,Cdb,sizeof(Cdb),FALSE,NULL,0);
}
VOID PianoUfsDisableWrites(PIANO_UFS_BLOCK *D) {
  if(D==NULL || D->Signature!=UFS_BLOCK_SIGNATURE)return;
  D->WritesEnabled=FALSE;D->WriteFirst=D->WriteLast=0;D->Media.ReadOnly=TRUE;
}
EFI_STATUS PianoUfsEnableWrites(PIANO_UFS_BLOCK *D,EFI_LBA First,EFI_LBA Last) {
  if(D==NULL || D->Signature!=UFS_BLOCK_SIGNATURE)return EFI_INVALID_PARAMETER;
  // Reserve both GPT ends. The caller must supply a separately reviewed
  // disposable data range; no implicit 'all disk writable' configuration.
  if(D->Media.LastBlock<67 || First<34 || Last>D->Media.LastBlock-33 || First>Last)
    return EFI_INVALID_PARAMETER;
  D->WriteFirst=First;D->WriteLast=Last;D->WritesEnabled=TRUE;D->Media.ReadOnly=FALSE;
  return EFI_SUCCESS;
}
EFI_STATUS PianoUfsBlockInit(PIANO_UFS_BLOCK *D,EFI_EXT_SCSI_PASS_THRU_PROTOCOL *T,UINT8 Lun) {
  if(D==NULL || T==NULL || T->Mode==NULL || T->PassThru==NULL || Lun>7)return EFI_INVALID_PARAMETER;
  UINT32 Align=T->Mode->IoAlign;
  if(Align>4096 || (Align>1 && (Align&(Align-1))))return EFI_UNSUPPORTED;
  ZeroMem(D,sizeof(*D));D->Transport=T;
  D->ScsiLun=(UINT64)Lun<<8; // EDK2 UFS normal-LUN SAM address, not a WLUN.
  UINT8 *Capacity=AllocateAlignedPages(1,MAX(Align,(UINT32)EFI_PAGE_SIZE));
  if(Capacity==NULL)return EFI_OUT_OF_RESOURCES;
  ZeroMem(Capacity,32);UINT8 Cdb[16]={0x9E,0x10};Be(Cdb+10,32,4); // READ CAPACITY(16).
  EFI_STATUS Status=Execute(D,Cdb,sizeof(Cdb),FALSE,Capacity,32);
  if(!EFI_ERROR(Status)) {
    D->Media.LastBlock=FromBe(Capacity,8);D->Media.BlockSize=(UINT32)FromBe(Capacity+8,4);
    if(D->Media.LastBlock==MAX_UINT64 || D->Media.LastBlock==0 ||
       (D->Media.BlockSize!=512 && D->Media.BlockSize!=4096))Status=EFI_UNSUPPORTED;
  }
  FreeAlignedPages(Capacity,1);
  if(EFI_ERROR(Status)){D->Transport=NULL;return Status;}
  D->Signature=UFS_BLOCK_SIGNATURE;D->Media.MediaId=1;D->Media.MediaPresent=TRUE;
  D->Media.ReadOnly=TRUE;D->Media.IoAlign=Align;D->Media.WriteCaching=FALSE;
  D->Block.Revision=EFI_BLOCK_IO_PROTOCOL_REVISION;D->Block.Media=&D->Media;
  D->Block.Reset=Reset;D->Block.ReadBlocks=Read;D->Block.WriteBlocks=Write;D->Block.FlushBlocks=Flush;
  return EFI_SUCCESS;
}
