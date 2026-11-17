// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included after partition resolution. Validate the sparse stream before IO.
#define FLASH_PROGRESS_BYTES 0x1000000U
#define FLASH_SCAN_RECORDS 128U
struct PIANO_FB_FLASH {
  PIANO_FB_PARTITION_INFO Info;
  UINT8 Expected[PIANO_FASTBOOT_MAX_BLOCK_IO_BYTES],Check[PIANO_FASTBOOT_MAX_BLOCK_IO_BYTES];
  UINT64 Offset,Remaining,Written,Reported,SparseBytes;
  UINT64 StartedUs;
  PIANO_FB_FLASH_TIMING Timing;
  UINTN Source,At;
  UINT32 Chunks,Block;
  UINT16 Header;
  BOOLEAN Sparse,Filling,Prepared;
  enum {FlashValidate,FlashPrepare,FlashData,FlashFlush} Phase;
  EFI_STATUS Status;
};
STATIC UINT16 FlashLe16(CONST UINT8 *P){return (UINT16)(P[0]|((UINT16)P[1]<<8));}
STATIC UINT32 FlashLe32(CONST UINT8 *P){return (UINT32)FlashLe16(P)|((UINT32)FlashLe16(P+2)<<16);}
STATIC BOOLEAN SparseImage(CONST UINT8 *Data,UINTN Bytes){
  return Bytes>=4 && FlashLe32(Data)==0xed26ff3a;
}
STATIC BOOLEAN SparseHeaderValid(CONST UINT8 *Data,UINTN Bytes,CONST PIANO_FB_PARTITION_INFO *Info){
  if(Bytes<28 || FlashLe16(Data+4)!=1)return FALSE;
  UINTN Header=FlashLe16(Data+8);
  UINT32 Block=FlashLe32(Data+12);
  UINT64 Total=(UINT64)FlashLe32(Data+16)*Block;
  return Header>=28 && Header<=Bytes && FlashLe16(Data+10)>=12 &&
    Block && !(Block%Info->BlockSize) && Total && Total<=Info->Bytes;
}
STATIC BOOLEAN ValidateSparseStep(CONST PIANO_FASTBOOT *S,PIANO_FB_FLASH *F){
  CONST UINT8 *Data=S->Download;
  UINTN Bytes=S->Received;
  for(UINTN I=0;I<FLASH_SCAN_RECORDS && F->Chunks;++I){
    if(F->At>Bytes || F->Header>Bytes-F->At)return FALSE;
    CONST UINT8 *Chunk=Data+F->At;
    UINT32 Length=FlashLe32(Chunk+8);
    UINT64 Expanded=(UINT64)FlashLe32(Chunk+4)*F->Block;
    if(Length<F->Header || Length>Bytes-F->At || Expanded>F->SparseBytes-F->Offset)return FALSE;
    UINTN Payload=Length-F->Header;
    switch(FlashLe16(Chunk)){
      case 0xcac1:if(!Expanded || Expanded!=Payload)return FALSE;break;
      case 0xcac2:if(!Expanded || Payload!=4)return FALSE;break;
      case 0xcac3:if(!Expanded || Payload)return FALSE;break;
      // Like fastboot's uploader, validate the CRC record layout without
      // expanding skipped ranges. Readback verifies the actual writes.
      case 0xcac4:if(Expanded || Payload!=4)return FALSE;break;
      default:return FALSE;
    }
    F->Offset+=Expanded;F->At+=Length;--F->Chunks;
  }
  if(F->Chunks)return TRUE;
  if(F->At!=Bytes || F->Offset!=F->SparseBytes)return FALSE;
  // Rewind only after every record has been checked against input and output bounds.
  F->At=FlashLe16(Data+8);F->Offset=0;F->Chunks=FlashLe32(Data+20);F->Phase=FlashPrepare;
  return TRUE;
}
BOOLEAN PianoFastbootFlashPending(CONST PIANO_FASTBOOT *S){return S && S->Flash!=NULL;}
STATIC VOID FlashFree(PIANO_FASTBOOT *S){
  if(S->Flash){ZeroMem(S->Flash,sizeof(*S->Flash));FreePool(S->Flash);S->Flash=NULL;}
}
STATIC VOID CancelFlash(PIANO_FASTBOOT *S){
  if(!S->Flash || S->BootTransferFrozen)return;
  // USB reset/disconnect is dispatched at APP, between synchronous IO steps.
  // Complete the storage barrier without emitting a reply on the lost session.
  S->FlashExecuting=TRUE;
  if(S->Flash->Prepared)S->Storage.Flush(S->Storage.Context,&S->Flash->Info);
  if(!S->BootTransferFrozen)FlashFree(S);
  S->FlashExecuting=FALSE;S->FlashCancelRequested=FALSE;
}
STATIC BOOLEAN FlashInterrupted(CONST PIANO_FASTBOOT *S){
  return S->FlashCancelRequested || S->BootTransferFrozen;
}
STATIC UINT64 FlashNow(CONST PIANO_FASTBOOT *S){
  return S->NowUs?S->NowUs(S->Context):0;
}
STATIC UINT64 FlashElapsed(CONST PIANO_FASTBOOT *S,UINT64 Start){
  UINT64 End=FlashNow(S);
  return End>=Start?End-Start:0;
}
STATIC EFI_STATUS FlashFinish(PIANO_FASTBOOT *S,CONST CHAR8 *Message){
  PIANO_FB_FLASH *F=S->Flash;
  if(S->NowUs && S->ReportFlash){
    F->Timing.Bytes=F->Written;
    F->Timing.TotalUs=FlashElapsed(S,F->StartedUs);
    S->ReportFlash(F->Info.Name,&F->Timing,Message);
  }
  FlashFree(S);S->FlashExecuting=FALSE;
  return Reply(S,Message);
}
STATIC EFI_STATUS FlashProgress(PIANO_FASTBOOT *S,UINT64 Written){
  CHAR8 Text[64]="INFOverified ",Digits[20];
  UINTN Count=0,At=sizeof("INFOverified ")-1;
  UINT64 MiB=Written>>20;
  do {
    Digits[Count++]=(CHAR8)('0'+MiB%10);
    MiB/=10;
  }while(MiB);
  while(Count)Text[At++]=Digits[--Count];
  CopyMem(Text+At," MiB written",sizeof(" MiB written"));
  return Reply(S,Text);
}
STATIC EFI_STATUS FlashWriteStep(PIANO_FASTBOOT *S,PIANO_FB_FLASH *F){
  UINTN Bytes=(UINTN)MIN((UINT64)sizeof(F->Expected),F->Remaining);
  UINTN Block=F->Info.BlockSize;
  UINTN Whole=Bytes-Bytes%Block;
  UINT64 Lba=F->Offset/Block;
  EFI_STATUS Status;
  if(Whole){
    Bytes=Whole; // Leave an unaligned raw tail for its own read/modify/write.
  }else{
    Whole=Block;
    UINT64 Start=FlashNow(S);
    Status=S->Storage.ReadBlocks(S->Storage.Context,&F->Info,Lba,Whole,F->Expected);
    F->Timing.ReadUs+=FlashElapsed(S,Start);
    if(Status!=EFI_SUCCESS || FlashInterrupted(S))return Status;
  }
  if(!F->Filling)CopyMem(F->Expected,S->Download+F->Source,Bytes);
  UINT64 Start=FlashNow(S);
  Status=S->Storage.WriteBlocks(S->Storage.Context,&F->Info,Lba,Whole,F->Expected);
  F->Timing.WriteUs+=FlashElapsed(S,Start);
  if(Status!=EFI_SUCCESS || FlashInterrupted(S))return Status;
  Start=FlashNow(S);
  Status=S->Storage.ReadBlocks(S->Storage.Context,&F->Info,Lba,Whole,F->Check);
  F->Timing.ReadUs+=FlashElapsed(S,Start);
  if(Status!=EFI_SUCCESS || FlashInterrupted(S))return Status;
  if(CompareMem(F->Expected,F->Check,Whole))return EFI_CRC_ERROR;
  F->Remaining-=Bytes;F->Offset+=Bytes;F->Written+=Bytes;
  if(!F->Filling)F->Source+=Bytes;
  return EFI_SUCCESS;
}
STATIC VOID NextSparseRange(CONST PIANO_FASTBOOT *S,PIANO_FB_FLASH *F){
  for(UINTN I=0;I<FLASH_SCAN_RECORDS && F->Chunks;++I){
    CONST UINT8 *Chunk=S->Download+F->At;
    UINT16 Type=FlashLe16(Chunk);
    UINT64 Expanded=(UINT64)FlashLe32(Chunk+4)*F->Block;
    F->At+=FlashLe32(Chunk+8);--F->Chunks;
    if(Type==0xcac1 || Type==0xcac2){
      F->Source=(UINTN)(Chunk-S->Download)+F->Header;
      F->Remaining=Expanded;F->Filling=Type==0xcac2;
      if(F->Filling){
        // Fill only the range this record can write in one batch.
        UINTN Bytes=(UINTN)MIN(Expanded,(UINT64)sizeof(F->Expected));
        CopyMem(F->Expected,Chunk+F->Header,4);
        for(UINTN N=4;N<Bytes;N*=2)
          CopyMem(F->Expected+N,F->Expected,MIN(N,Bytes-N));
      }
      break;
    }
    F->Offset+=Expanded; // Preserve holes across fastboot's resparsed downloads.
  }
}
EFI_STATUS PianoFastbootFlashStep(PIANO_FASTBOOT *S){
  if(!S || !S->Flash)return EFI_NOT_READY;
  if(S->BootTransferFrozen)return EFI_ABORTED;
  if(S->FlashExecuting)return EFI_ALREADY_STARTED;
  S->FlashExecuting=TRUE;PIANO_FB_FLASH *F=S->Flash;
  if(F->Phase==FlashValidate){
    if(!ValidateSparseStep(S,F))return FlashFinish(S,"FAILinvalid or unsupported sparse image");
  }else if(F->Phase==FlashPrepare){
    UINT64 Start=FlashNow(S);
    F->Status=S->Storage.PrepareWrite(S->Storage.Context,&F->Info);
    F->Timing.PrepareUs+=FlashElapsed(S,Start);
    F->Prepared=F->Status==EFI_SUCCESS;
    if(!F->Prepared && !FlashInterrupted(S))
      return FlashFinish(S,"FAILcannot prepare partition; restart and retry");
    F->Phase=FlashData;
  }else if(F->Phase==FlashFlush){
    UINT64 Start=FlashNow(S);
    EFI_STATUS Flush=S->Storage.Flush(S->Storage.Context,&F->Info);
    F->Timing.FlushUs+=FlashElapsed(S,Start);
    F->Prepared=FALSE;
    if(F->Status==EFI_SUCCESS)F->Status=Flush;
    if(!FlashInterrupted(S))
      return FlashFinish(S,F->Status==EFI_SUCCESS?"OKAY":"FAILpartition write, sync or readback failed");
  }else if(F->Remaining){
    F->Status=FlashWriteStep(S,F);
    if(F->Status!=EFI_SUCCESS)F->Phase=FlashFlush;
    else if(!FlashInterrupted(S) && F->Written-F->Reported>=FLASH_PROGRESS_BYTES){
      F->Reported=F->Written;
      EFI_STATUS ReplyStatus=FlashProgress(S,F->Written);
      if(ReplyStatus!=EFI_SUCCESS){
        S->FlashExecuting=FALSE;
        CancelFlash(S);
        return ReplyStatus;
      }
    }
  }else if(F->Sparse && F->Chunks){
    NextSparseRange(S,F);
  }else F->Phase=FlashFlush;
  S->FlashExecuting=FALSE;
  if(S->BootTransferFrozen)return EFI_ABORTED;
  if(S->FlashCancelRequested){
    PianoFastbootReset(S);return EFI_ABORTED;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Flash(PIANO_FASTBOOT *S,CONST CHAR8 *Name,BOOLEAN Erase){
  if(!PianoFastbootStorageWritable(S))
    return Reply(S,"FAILstorage write backend not ready");
  PIANO_FB_PARTITION_INFO Info;EFI_STATUS Status=PartitionInfo(S,Name,&Info);
  if(Status!=EFI_SUCCESS)return Reply(S,Status==EFI_NOT_FOUND?"FAILunknown partition":"FAILinvalid partition information");
  if(Info.ReadOnly)return Reply(S,"FAILdevice reports hardware write protection");
  if(!Erase && (!S->Complete || !S->Download || !S->Received))return Reply(S,"FAILno complete RAM payload");
  BOOLEAN Sparse=!Erase && SparseImage(S->Download,S->Received);
  if(Sparse){
    if(!SparseHeaderValid(S->Download,S->Received,&Info))
      return Reply(S,"FAILinvalid or unsupported sparse image");
  }else if(!Erase && S->Received>Info.Bytes)return Reply(S,"FAILimage exceeds partition size");
  PIANO_FB_FLASH *F=AllocateZeroPool(sizeof(*F));if(!F)return Reply(S,"FAILnot enough RAM");
  F->Info=Info;F->Sparse=Sparse;F->Phase=FlashPrepare;
  F->StartedUs=FlashNow(S);
  if(Sparse){
    F->At=FlashLe16(S->Download+8);F->Header=FlashLe16(S->Download+10);
    F->Block=FlashLe32(S->Download+12);F->Chunks=FlashLe32(S->Download+20);
    F->SparseBytes=(UINT64)FlashLe32(S->Download+16)*F->Block;
    F->Phase=FlashValidate;
  }else{F->Remaining=Erase?Info.Bytes:S->Received;F->Filling=Erase;}
  S->Flash=F;S->FlashCancelRequested=FALSE;
  Status=Reply(S,"INFOpreparing partition write; keep USB connected");
  if(Status!=EFI_SUCCESS)CancelFlash(S);
  return Status;
}
