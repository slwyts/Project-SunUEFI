// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual command parser and sparse writer against a memory-backed partition.
#define main fastboot_base_suite
#include "test_fastboot.c"
#undef main
#define BLOCK_BYTES 4096U
#define BATCH_BYTES PIANO_FASTBOOT_MAX_BLOCK_IO_BYTES
static UINT8 disk[4*BATCH_BYTES];
static UINTN prepared,written,flushed,read_count;
static UINTN write_bytes[8],read_bytes[8];
static UINT64 write_lba[8],read_lba[8];
static BOOLEAN protected,corrupt,write_failure,flush_failure;
static PIANO_FASTBOOT *cancel_state;
static UINTN cancel_at;
static EFI_STATUS ready_flash(VOID *Context){(VOID)Context;return EFI_SUCCESS;}
static EFI_STATUS info_flash(VOID *Context,CONST CHAR8 *Name,PIANO_FB_PARTITION_INFO *Info){
  (VOID)Context;if(strcmp(Name,"xbl_a"))return EFI_NOT_FOUND;
  memcpy(Info->Name,Name,strlen(Name)+1);Info->Bytes=sizeof(disk);Info->BlockSize=BLOCK_BYTES;
  Info->ReadOnly=protected;Info->Token=disk;return EFI_SUCCESS;
}
static EFI_STATUS prepare_flash(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info){
  (VOID)Context;assert(Info->Token==disk);++prepared;
  if(cancel_at==1)PianoFastbootReset(cancel_state);
  return EFI_SUCCESS;
}
static EFI_STATUS read_flash(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info,UINT64 Lba,UINTN Bytes,VOID *Buffer){
  (VOID)Context;assert(Info->Token==disk && read_count<ARRAY_SIZE(read_bytes) && Lba<sizeof(disk)/BLOCK_BYTES);
  assert(Bytes && Bytes<=BATCH_BYTES && !(Bytes%BLOCK_BYTES) && Bytes<=sizeof(disk)-Lba*BLOCK_BYTES);
  read_bytes[read_count]=Bytes;read_lba[read_count++]=Lba;
  memcpy(Buffer,disk+Lba*BLOCK_BYTES,Bytes);if(corrupt)((UINT8 *)Buffer)[Bytes-1]^=1;return EFI_SUCCESS;
}
static EFI_STATUS write_flash(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info,UINT64 Lba,UINTN Bytes,CONST VOID *Buffer){
  (VOID)Context;assert(Info->Token==disk && written<ARRAY_SIZE(write_bytes) && Lba<sizeof(disk)/BLOCK_BYTES);
  assert(Bytes && Bytes<=BATCH_BYTES && !(Bytes%BLOCK_BYTES) && Bytes<=sizeof(disk)-Lba*BLOCK_BYTES);
  write_bytes[written]=Bytes;write_lba[written++]=Lba;
  if(write_failure)return EFI_DEVICE_ERROR;
  memcpy(disk+Lba*BLOCK_BYTES,Buffer,Bytes);
  if(cancel_at==2)PianoFastbootReset(cancel_state);
  return EFI_SUCCESS;
}
static EFI_STATUS flush_flash(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info){
  (VOID)Context;assert(Info->Token==disk);++flushed;
  if(cancel_at==3)PianoFastbootReset(cancel_state);
  return flush_failure?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static void flash_command(PIANO_FASTBOOT *S,const char *Text,const char *Expected){
  calls=0;assert(PianoFastbootPacket(S,Text,strlen(Text))==EFI_SUCCESS);
  UINTN Steps=0;
  while(PianoFastbootFlashPending(S)){
    UINTN Before=written;
    assert(PianoFastbootFlashStep(S)==EFI_SUCCESS);
    assert(written-Before<=1 && ++Steps<100);
  }
  assert(calls && !strcmp(replies[calls-1],Expected));
}
#define command flash_command
static VOID stage(PIANO_FASTBOOT *S,CONST UINT8 *Data,UINTN Bytes){
  CHAR8 Cmd[32];snprintf(Cmd,sizeof(Cmd),"download:%08x",(unsigned)Bytes);calls=0;
  assert(PianoFastbootPacket(S,Cmd,strlen(Cmd))==EFI_SUCCESS);
  assert(PianoFastbootPacket(S,Data,Bytes)==EFI_SUCCESS && S->Complete);
  prepared=written=flushed=read_count=0;
}
static VOID le16(UINT8 *P,UINT16 V){P[0]=(UINT8)V;P[1]=(UINT8)(V>>8);}
static VOID le32(UINT8 *P,UINT32 V){le16(P,(UINT16)V);le16(P+2,(UINT16)(V>>16));}
static UINTN sparse(UINT8 *P){
  memset(P,0,8192);le32(P,0xed26ff3a);le16(P+4,1);le16(P+8,28);le16(P+10,12);
  le32(P+12,4096);le32(P+16,3);le32(P+20,3);
  UINTN At=28;
  le16(P+At,0xcac1);le32(P+At+4,1);le32(P+At+8,4108);memset(P+At+12,0x71,4096);At+=4108;
  le16(P+At,0xcac3);le32(P+At+4,1);le32(P+At+8,12);At+=12;
  le16(P+At,0xcac2);le32(P+At+4,1);le32(P+At+8,16);le32(P+At+12,0x12345678);return At+16;
}
static UINTN sparse_second(UINT8 *P){
  // A later resparsed segment has a leading hole for an already written block.
  UINTN Size=sparse(P);
  le16(P+28,0xcac3);
  le32(P+36,12);
  le16(P+40,0xcac2);le32(P+44,1);le32(P+48,16);le32(P+52,0x18365472);
  CopyMem(P+56,P+Size-16,16);
  return 72;
}
static UINTN sparse_batches(UINT8 *P){
  const UINTN RawBytes=BATCH_BYTES+BLOCK_BYTES;
  UINTN At=0;
  memset(P,0,28);le32(P,0xed26ff3a);le16(P+4,1);le16(P+8,28);le16(P+10,12);
  // A raw range and a fill range each cross the batch boundary.
  le32(P+12,BLOCK_BYTES);le32(P+16,2*RawBytes/BLOCK_BYTES+3);le32(P+20,3);At=28;
  le16(P+At,0xcac1);le32(P+At+4,RawBytes/BLOCK_BYTES);le32(P+At+8,12+RawBytes);
  for(UINTN I=0;I<RawBytes;++I)P[At+12+I]=(UINT8)(I*19+I/251);
  At+=12+RawBytes;
  le16(P+At,0xcac3);le32(P+At+4,3);le32(P+At+8,12);At+=12;
  le16(P+At,0xcac2);le32(P+At+4,RawBytes/BLOCK_BYTES);le32(P+At+8,16);
  le32(P+At+12,0x12345678);
  return At+16;
}
int main(void){
  PIANO_FASTBOOT S;PIANO_FB_STORAGE Storage={
    .Ready=ready_flash,.Info=info_flash,.ReadBlocks=read_flash,.PrepareWrite=prepare_flash,
    .WriteBlocks=write_flash,.Flush=flush_flash
  };
  assert(PianoFastbootInit(&S,(VOID *)123,send_reply,NULL)==EFI_SUCCESS);
  assert(PianoFastbootSetStorage(&S,&Storage)==EFI_SUCCESS);
  command(&S,"flash:xbl_a","FAILno complete RAM payload");
  static UINT8 Data[sizeof(disk)+1];
  memset(Data,0x58,sizeof(Data));memset(disk,0xa5,sizeof(disk));
  stage(&S,Data,4097);command(&S,"flash:xbl_a","OKAY");
  assert(prepared==1 && written==2 && flushed==1 && !memcmp(disk,Data,4097));
  for(UINTN I=4097;I<sizeof(disk);++I)assert(disk[I]==0xa5);
  const UINTN RawBytes=BATCH_BYTES+BLOCK_BYTES+3;
  for(UINTN I=0;I<RawBytes;++I)Data[I]=(UINT8)(I*73+I/251);
  stage(&S,Data,RawBytes);command(&S,"flash:xbl_a","OKAY");
  assert(prepared==1 && written==3 && flushed==1 && read_count==4 && !memcmp(disk,Data,RawBytes));
  assert(write_bytes[0]==BATCH_BYTES && write_lba[0]==0 && read_bytes[0]==BATCH_BYTES && read_lba[0]==0);
  assert(write_bytes[1]==BLOCK_BYTES && write_lba[1]==BATCH_BYTES/BLOCK_BYTES && read_bytes[1]==BLOCK_BYTES);
  assert(write_bytes[2]==BLOCK_BYTES && write_lba[2]==BATCH_BYTES/BLOCK_BYTES+1);
  assert(read_bytes[2]==BLOCK_BYTES && read_lba[2]==write_lba[2] && read_bytes[3]==BLOCK_BYTES && read_lba[3]==write_lba[2]);
  for(UINTN I=RawBytes;I<sizeof(disk);++I)assert(disk[I]==0xa5);
  protected=TRUE;stage(&S,Data,4096);command(&S,"flash:xbl_a","FAILdevice reports hardware write protection");
  assert(!prepared && !written);protected=FALSE;
  command(&S,"flash:xbl_b","FAILunknown partition");assert(!prepared && !written);
  command(&S,"flash:../xbl_a","FAILinvalid partition information");assert(!prepared && !written);
  stage(&S,Data,sizeof(Data));command(&S,"flash:xbl_a","FAILimage exceeds partition size");assert(!prepared && !written);
  UINTN Size=sparse(Data);memset(disk,0xa5,sizeof(disk));stage(&S,Data,Size);command(&S,"flash:xbl_a","OKAY");
  assert(prepared==1 && written==2 && flushed==1);
  for(UINTN I=0;I<4096;++I)assert(disk[I]==0x71 && disk[I+4096]==0xa5 && disk[I+8192]==Data[Size-4+I%4]);
  Size=sparse_second(Data);stage(&S,Data,Size);command(&S,"flash:xbl_a","OKAY");
  assert(prepared==1 && written==2 && flushed==1);
  for(UINTN I=0;I<4096;++I)assert(disk[I]==0x71 && disk[I+4096]==Data[52+I%4] && disk[I+8192]==Data[Size-4+I%4]);
  Size=sparse_batches(Data);memset(disk,0xa5,sizeof(disk));stage(&S,Data,Size);command(&S,"flash:xbl_a","OKAY");
  assert(prepared==1 && written==4 && read_count==4 && flushed==1);
  const UINTN Range=BATCH_BYTES+BLOCK_BYTES,Fill=Range+3*BLOCK_BYTES;
  assert(write_lba[0]==0 && write_lba[1]==BATCH_BYTES/BLOCK_BYTES);
  assert(write_lba[2]==Fill/BLOCK_BYTES && write_lba[3]==(Fill+BATCH_BYTES)/BLOCK_BYTES);
  for(UINTN I=0;I<written;++I)
    assert(write_bytes[I]==(I%2?BLOCK_BYTES:BATCH_BYTES) && read_bytes[I]==write_bytes[I] && read_lba[I]==write_lba[I]);
  assert(!memcmp(disk,Data+40,Range));
  for(UINTN I=Range;I<Fill;++I)assert(disk[I]==0xa5);
  for(UINTN I=0;I<Range;++I)assert(disk[Fill+I]==Data[Size-4+I%4]);
  for(UINTN I=Fill+Range;I<sizeof(disk);++I)assert(disk[I]==0xa5);
  // A valid first range must not be written before discovering an overrun
  // later in the sparse stream.
  le32(Data+Size-12,(UINT32)(sizeof(disk)/BLOCK_BYTES));stage(&S,Data,Size);
  command(&S,"flash:xbl_a","FAILinvalid or unsupported sparse image");assert(!prepared && !written && !read_count && !flushed);
  // A malformed record beyond the first scan slice must not prepare or write.
  Size=sparse(Data);
  for(UINTN I=0;I<FLASH_SCAN_RECORDS;++I){
    le16(Data+Size,0xcac4);le32(Data+Size+8,16);Size+=16;
  }
  le32(Data+20,3+FLASH_SCAN_RECORDS);
  le32(Data+Size-8,17);
  stage(&S,Data,Size);calls=0;
  assert(PianoFastbootPacket(&S,"flash:xbl_a",11)==EFI_SUCCESS && S.Flash && calls==1);
  assert(PianoFastbootFlashStep(&S)==EFI_SUCCESS && S.Flash && !prepared && !written && calls==1);
  assert(PianoFastbootFlashStep(&S)==EFI_SUCCESS && !S.Flash && !prepared && !written && !flushed);
  assert(!strcmp(replies[calls-1],"FAILinvalid or unsupported sparse image"));
  // Every structural failure is rejected before PrepareWrite or any IO.
  for(UINTN Case=0;Case<7;++Case){
    Size=sparse(Data);
    switch(Case){
      case 0:le32(Data+20,4);break;
      case 1:le32(Data+16,0xffffffff);break;
      case 2:le32(Data+28+8,0xffffffff);break;
      case 3:le16(Data+28,0xffff);break;
      case 4:le32(Data+28+4,2);break;
      case 5:--Size;break;
      case 6:le32(Data+12,1);break;
    }
    stage(&S,Data,Size);command(&S,"flash:xbl_a","FAILinvalid or unsupported sparse image");assert(!prepared && !written);
  }
  memset(Data,0x58,BATCH_BYTES);stage(&S,Data,BATCH_BYTES);corrupt=TRUE;
  command(&S,"flash:xbl_a","FAILpartition write, sync or readback failed");
  assert(written==1 && read_count==1 && read_bytes[0]==BATCH_BYTES && flushed==1);corrupt=FALSE;
  stage(&S,Data,4096);write_failure=TRUE;
  command(&S,"flash:xbl_a","FAILpartition write, sync or readback failed");assert(written==1 && flushed==1);write_failure=FALSE;
  stage(&S,Data,4096);flush_failure=TRUE;
  command(&S,"flash:xbl_a","FAILpartition write, sync or readback failed");assert(written==1 && flushed==1);flush_failure=FALSE;
  prepared=written=flushed=read_count=0;command(&S,"erase:xbl_a","OKAY");
  assert(prepared==1 && written==sizeof(disk)/BATCH_BYTES && flushed==1);
  for(UINTN I=0;I<sizeof(disk);++I)assert(!disk[I]);
  // If even the first reply cannot be queued, no write has started.
  stage(&S,Data,4096);calls=0;fail_send=1;
  assert(PianoFastbootPacket(&S,"flash:xbl_a",11)==EFI_DEVICE_ERROR && !S.Flash && S.Download);
  assert(!calls && !prepared && !written && !flushed);fail_send=0;
  // Pending commands retain their source, refuse replacement, and do no IO in Packet.
  stage(&S,Data,8192);calls=0;
  assert(PianoFastbootPacket(&S,"flash:xbl_a",11)==EFI_SUCCESS && S.Flash && !prepared && !written);
  UINT8 *Source=S.Download;
  assert(PianoFastbootPacket(&S,"oem discard",11)==EFI_NOT_READY && S.Download==Source);
  assert(PianoFastbootStageCopy(&S,Data,1)==EFI_NOT_READY);
  assert(PianoFastbootSetStorage(&S,NULL)==EFI_NOT_READY);
  assert(PianoFastbootFlashStep(&S)==EFI_SUCCESS && prepared==1 && !written);
  assert(PianoFastbootFlashStep(&S)==EFI_SUCCESS && written==1);
  PianoFastbootReset(&S);assert(!S.Flash && !S.Download && flushed==1 && calls==1);
  // A reset requested from any backend callback is deferred until it returns.
  for(cancel_at=1;cancel_at<=3;++cancel_at){
    stage(&S,Data,8192);cancel_state=&S;calls=0;
    assert(PianoFastbootPacket(&S,"flash:xbl_a",11)==EFI_SUCCESS);
    EFI_STATUS Result=EFI_SUCCESS;
    while(S.Flash && Result==EFI_SUCCESS)Result=PianoFastbootFlashStep(&S);
    assert(Result==EFI_ABORTED && !S.Flash && !S.Download && !S.FlashExecuting && calls==1);
    assert(flushed==1);
  }
  cancel_at=0;stage(&S,Data,4096);calls=0;
  assert(PianoFastbootPacket(&S,"flash:xbl_a",11)==EFI_SUCCESS);
  assert(PianoFastbootRestrictToDiagnostics(&S,TRUE)==EFI_SUCCESS && S.DiagnosticsOnly && !S.Flash && !S.Download);
  assert(!strcmp(replies[calls-1],"FAILpartition write interrupted") && !written);
  PianoFastbootReset(&S);assert(!allocation);
  puts("Fastboot flash: 1 MiB write/readback, raw tails, cross-batch sparse holes, preflight bounds, reset and final sync passed.");
}
