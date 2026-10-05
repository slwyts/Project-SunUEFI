// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real protocol source and fake readonly partitions, never device I/O.
#define main fastboot_command_suite
#include "test_fastboot.c"
#undef main
static UINT8 frames[3][65537];static UINTN lengths[3],frame_count,reads,info_calls;
static BOOLEAN ready,bad_info,read_only,fail_read;static INTN fail_frame;
static UINT64 partition_bytes;static UINT32 block_size;
static UINT8 byte_at(UINT64 Offset){return (UINT8)(((Offset^(Offset>>8)^(Offset>>32)^(Offset>>56))*73)+19);}
static EFI_STATUS storage_ready(VOID *Context){assert(Context==(VOID *)456);return ready?EFI_SUCCESS:EFI_NOT_READY;}
static EFI_STATUS storage_info(VOID *Context,CONST CHAR8 *Name,PIANO_FB_PARTITION_INFO *Info) {
  assert(Context==(VOID *)456 && ready);++info_calls;
  if(strcmp(Name,"xbl_config_a"))return EFI_NOT_FOUND;
  memcpy(Info->Name,Name,strlen(Name)+1);Info->Bytes=partition_bytes;Info->BlockSize=block_size;
  Info->ReadOnly=read_only;Info->Token=bad_info?NULL:(VOID *)789;return EFI_SUCCESS;
}
static EFI_STATUS storage_read(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info,UINT64 Lba,UINTN Bytes,VOID *Buffer) {
  assert(Context==(VOID *)456 && ready && Info->Token==(VOID *)789 && !strcmp(Info->Name,"xbl_config_a"));
  assert(Bytes && !(Bytes%Info->BlockSize) && Lba<=Info->Bytes/Info->BlockSize && Bytes/Info->BlockSize<=Info->Bytes/Info->BlockSize-Lba);
  ++reads;if(fail_read)return EFI_DEVICE_ERROR;
  UINT64 Offset=Lba*Info->BlockSize;for(UINTN I=0;I<Bytes;++I)((UINT8 *)Buffer)[I]=byte_at(Offset+I);
  return EFI_SUCCESS;
}
static EFI_STATUS send_frame(VOID *Context,CONST VOID *Data,UINTN Bytes) {
  assert(Context==(VOID *)123 && frame_count<3 && Bytes<=65536);
  if((INTN)frame_count==fail_frame)return EFI_DEVICE_ERROR;
  CopyMem(frames[frame_count],Data,Bytes);frames[frame_count][Bytes]=0;lengths[frame_count++]=Bytes;return EFI_SUCCESS;
}
static EFI_STATUS packet(PIANO_FASTBOOT *S,CONST CHAR8 *Cmd) {
  frame_count=reads=0;return PianoFastbootPacket(S,Cmd,strlen(Cmd));
}
static VOID reply(PIANO_FASTBOOT *S,CONST CHAR8 *Cmd,CONST CHAR8 *Expected) {
  assert(packet(S,Cmd)==EFI_SUCCESS && frame_count==1 && lengths[0]==strlen(Expected) && !memcmp(frames[0],Expected,lengths[0]));
  assert(!allocation);
}
static VOID fetch(PIANO_FASTBOOT *S,UINT64 Offset,UINTN Bytes) {
  CHAR8 Cmd[65];snprintf(Cmd,sizeof(Cmd),"fetch:xbl_config_a:0x%llx:0x%lx",(unsigned long long)Offset,(unsigned long)Bytes);
  assert(packet(S,Cmd)==EFI_SUCCESS && frame_count==3 && lengths[0]==12 && lengths[1]==Bytes && lengths[2]==4 && !memcmp(frames[2],"OKAY",4));
  CHAR8 Size[13]="DATA";Hex32((UINT32)Bytes,Size+4);assert(!memcmp(frames[0],Size,12));
  for(UINTN I=0;I<Bytes;++I)assert(frames[1][I]==byte_at(Offset+I));
  assert(!allocation && !S->Upload && !S->Download); // Live partition read never masquerades as staged RAM.
}
int main(void) {
  PIANO_FASTBOOT S;assert(PianoFastbootInit(&S,(VOID *)123,send_frame,NULL)==EFI_SUCCESS);
  fail_frame=-1;block_size=4096;partition_bytes=0x0000020000040000ULL;read_only=TRUE;
  reply(&S,"getvar:max-fetch-size","FAILstorage backend not ready");
  reply(&S,"fetch:xbl_config_a:0x0:0x1000","FAILstorage backend not ready");assert(!info_calls);
  PIANO_FB_STORAGE Storage={.Context=(VOID *)456,.Ready=storage_ready,.Info=storage_info,.ReadBlocks=storage_read};
  assert(PianoFastbootSetStorage(NULL,&Storage)==EFI_INVALID_PARAMETER);
  PIANO_FB_STORAGE Broken=Storage;Broken.ReadBlocks=NULL;assert(PianoFastbootSetStorage(&S,&Broken)==EFI_INVALID_PARAMETER);
  assert(PianoFastbootSetStorage(&S,&Storage)==EFI_SUCCESS);
  reply(&S,"getvar:max-fetch-size","FAILstorage backend not ready");ready=TRUE;
  reply(&S,"getvar:max-fetch-size","OKAY0x00010000");
  reply(&S,"getvar:partition-size:xbl_config_a","OKAY0x0000020000040000");
  fetch(&S,0,65536);assert(reads==1);
  fetch(&S,4094,5);assert(reads==2);
  fetch(&S,1,65536);assert(reads==3);
  fetch(&S,0x100000001ULL,19);assert(reads==1); // Above 32-bit offsets.
  fetch(&S,partition_bytes-1,1);assert(reads==1);
  partition_bytes=0xfffffffffffff000ULL;fetch(&S,partition_bytes-1,1); // No offset+size overflow.
  const CHAR8 *Bad[]={"fetch:xbl_config_a", "fetch:xbl_config_a:0x0", "fetch:xbl_config_a:0x0:0x0", "fetch:xbl_config_a:0x0:0x10001",
    "fetch:xbl_config_a:0x0:0xffffffffffffffff", "fetch:xbl_config_a:0x10000000000000000:0x1", "fetch:xbl_config_a:0x:0x1",
    "fetch:xbl_config_a:-1:0x1", "fetch:xbl_config_a:0x0:0x1:0x2", "fetch:../xbl_config_a:0x0:0x1", "fetch:xbl_config_a:0x1g:0x1"};
  for(UINTN I=0;I<ARRAY_SIZE(Bad);++I){assert(packet(&S,Bad[I])==EFI_SUCCESS && frame_count==1 && !memcmp(frames[0],"FAIL",4) && !reads && !allocation);}
  reply(&S,"fetch:xbl_config_a:0xffffffffffffffff:0x1","FAILfetch outside partition");
  reply(&S,"fetch:XBL_config_a:0x0:0x1","FAILunknown partition");
  reply(&S,"getvar:partition-size:xbl_config_b","FAILunknown partition");
  bad_info=TRUE;reply(&S,"fetch:xbl_config_a:0x0:0x1","FAILinvalid partition information");bad_info=FALSE;
  read_only=FALSE;reply(&S,"fetch:xbl_config_a:0x0:0x1","FAILinvalid partition information");read_only=TRUE;
  ++partition_bytes;reply(&S,"getvar:partition-size:xbl_config_a","FAILinvalid partition information");--partition_bytes;
  fail_read=TRUE;reply(&S,"fetch:xbl_config_a:0x0:0x1","FAILpartition read failed");assert(reads==1);fail_read=FALSE;
  fail_alloc=1;reply(&S,"fetch:xbl_config_a:0x0:0x1","FAILnot enough RAM");fail_alloc=0;
  for(INTN Frame=0;Frame<3;++Frame){fail_frame=Frame;assert(packet(&S,"fetch:xbl_config_a:0x1:0x1000")==EFI_DEVICE_ERROR && !allocation);}fail_frame=-1;
  PianoFastbootReset(&S);assert(StorageReady(&S)); // RAM discard does not detach an independently owned backend.
  assert(PianoFastbootSetStorage(&S,NULL)==EFI_SUCCESS);reply(&S,"getvar:max-fetch-size","FAILstorage backend not ready");
  reply(&S,"flash:xbl_config_a","FAILcommand disabled by RAM-only policy");
  puts("Fastboot fetch actual source: readiness advertisement, exact names/tokens/readonly metadata, 64-bit bounds, 64KiB DATA/raw/OKAY, partial 4K, read/alloc/send failure zero-free, no RAM-stage alias and flash denial passed.");
  return 0;
}
