// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual bridge + fetch protocol, fake EFI children. Never hardware access.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#undef NULL
#include "../../uefi/core/usb/PianoFastbootBlockRead.c"
#include "../../uefi/core/usb/PianoFastboot.c"
EFI_GUID gEfiBlockIoProtocolGuid={.Data1=1},gEfiPartitionInfoProtocolGuid={.Data1=2},gEfiDevicePathProtocolGuid={.Data1=3};
EFI_BOOT_SERVICES bs,*gBS=&bs;
typedef struct {VENDOR_DEVICE_PATH Vendor;UFS_DEVICE_PATH Ufs;HARDDRIVE_DEVICE_PATH Hd;EFI_DEVICE_PATH_PROTOCOL End;} PATH;
_Static_assert(sizeof(PATH)==sizeof(VENDOR_DEVICE_PATH)+sizeof(UFS_DEVICE_PATH)+sizeof(HARDDRIVE_DEVICE_PATH)+4,"fixed path must not contain padding");
typedef struct {
  EFI_BLOCK_IO_PROTOCOL Block;EFI_BLOCK_IO_MEDIA Media;EFI_PARTITION_INFO_PROTOCOL Info;PATH Path;UINT32 Missing;
#if PIANO_UFS_PRODUCT_STORAGE
  BOOLEAN Mounted,OpenFiles;
#endif
} CHILD;
static CHILD children[MAX_PARTITIONS+1];static UINTN child_count,read_calls,write_calls,last_lba,last_bytes;
static EFI_STATUS locate_status,read_status;static BOOLEAN null_handles;static UINTN pools;
static UINTN revoke_on_read;
static struct {VOID *P;UINTN Bytes;BOOLEAN Zero;} allocated[16];
#if PIANO_UFS_PRODUCT_STORAGE
static UINT8 written_block[PIANO_FASTBOOT_MAX_BLOCK_IO_BYTES];
static UINTN written_bytes;
static UINTN flush_calls,close_calls,timing_reports;
static CHILD *written_child,*expected_close;
static BOOLEAN revoke_on_write,revoke_on_flush;
static PIANO_STORAGE_IO_TIMING *timing;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return Level==DEBUG_WARN;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){
  if(!strncmp(Format,"SUNUEFI_FASTBOOT_FLASH_UFS ",sizeof("SUNUEFI_FASTBOOT_FLASH_UFS ")-1))++timing_reports;
}
PIANO_STORAGE_IO_TIMING *PianoStorageSetIoTiming(PIANO_STORAGE_IO_TIMING *Now){
  PIANO_STORAGE_IO_TIMING *Previous=timing;
  timing=Now;
  return Previous;
}
INTN EFIAPI StrnCmp(CONST CHAR16 *A,CONST CHAR16 *B,UINTN N){
  for(UINTN I=0;I<N;++I){if(A[I]!=B[I])return A[I]<B[I]?-1:1;if(!A[I])return 0;}return 0;
}
UINTN EFIAPI StrSize(CONST CHAR16 *S){UINTN N=0;while(S[N])++N;return (N+1)*sizeof(CHAR16);}
EFI_STATUS PianoStorageCloseFilesystemForWrite(EFI_HANDLE Partition){
  CHILD *C=Partition;assert(C==expected_close && C->Mounted);
  if(C->OpenFiles)return EFI_ACCESS_DENIED;
  ++close_calls;C->Mounted=FALSE;return EFI_SUCCESS;
}
EFI_STATUS PianoStorageCheckFilesystemClosed(EFI_HANDLE Partition){
  CHILD *C=Partition;assert(C>=children && C<children+child_count);
  return C->Mounted?EFI_ACCESS_DENIED:EFI_SUCCESS;
}
#endif
VOID *EFIAPI CopyMem(VOID *A,CONST VOID *B,UINTN N){return memcpy(A,B,N);}
VOID *EFIAPI ZeroMem(VOID *A,UINTN N){return memset(A,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
BOOLEAN EFIAPI CompareGuid(CONST GUID *A,CONST GUID *B){return !memcmp(A,B,sizeof(*A));}
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S){return strlen(S);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
INTN EFIAPI AsciiStrnCmp(CONST CHAR8 *A,CONST CHAR8 *B,UINTN N){return strncmp(A,B,N);}
static VOID *alloc(UINTN Bytes,BOOLEAN Zero) {
  for(UINTN I=0;I<ARRAY_SIZE(allocated);++I)if(!allocated[I].P){VOID *P=calloc(1,Bytes);assert(P);allocated[I]=(typeof(allocated[0])){P,Bytes,Zero};++pools;return P;}
  assert(FALSE);return NULL;
}
VOID *EFIAPI AllocateZeroPool(UINTN Bytes){return alloc(Bytes,TRUE);}
#if PIANO_UFS_PRODUCT_STORAGE
VOID *EFIAPI AllocatePool(UINTN Bytes){return alloc(Bytes,FALSE);}
#endif
VOID EFIAPI FreePool(VOID *P) {
  assert(P);for(UINTN I=0;I<ARRAY_SIZE(allocated);++I)if(allocated[I].P==P) {
    if(allocated[I].Zero)for(UINTN N=0;N<allocated[I].Bytes;++N)assert(!((UINT8 *)P)[N]);
    free(P);allocated[I].P=NULL;--pools;return;
  }
  assert(FALSE);
}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *P,UINTN N,UINT8 *Hash){return SHA256(P,N,Hash)!=NULL;}
static EFI_STATUS EFIAPI locate(EFI_LOCATE_SEARCH_TYPE Type,EFI_GUID *Guid,VOID *Key,UINTN *Count,EFI_HANDLE **Handles) {
  assert(Type==ByProtocol && Guid==&gEfiBlockIoProtocolGuid && !Key);*Count=child_count;*Handles=NULL;
  if(child_count && !null_handles){*Handles=alloc(child_count*sizeof(**Handles),FALSE);for(UINTN I=0;I<child_count;++I)(*Handles)[I]=&children[I];}
  return locate_status;
}
static EFI_STATUS EFIAPI handle(EFI_HANDLE Handle,EFI_GUID *Guid,VOID **Out) {
  CHILD *C=Handle;assert(C>=children && C<children+child_count);*Out=NULL;
  if(Guid==&gEfiBlockIoProtocolGuid){if(C->Missing&1)return EFI_NOT_FOUND;*Out=&C->Block;}
  else if(Guid==&gEfiPartitionInfoProtocolGuid){if(C->Missing&2)return EFI_NOT_FOUND;*Out=&C->Info;}
  else {assert(Guid==&gEfiDevicePathProtocolGuid);if(C->Missing&4)return EFI_NOT_FOUND;*Out=&C->Path;}
  return EFI_SUCCESS;
}
static UINT8 at(UINT64 Byte){return (UINT8)(Byte*73+(Byte>>8)+(Byte>>32)+19);}
static EFI_STATUS EFIAPI read(EFI_BLOCK_IO_PROTOCOL *Block,UINT32 MediaId,EFI_LBA Lba,UINTN Bytes,VOID *Buffer) {
  CHILD *C=NULL;for(UINTN I=0;I<child_count;++I)if(&children[I].Block==Block)C=&children[I];assert(C);
  assert(MediaId==C->Media.MediaId && Bytes && !(Bytes%4096) && Lba<=C->Media.LastBlock && Bytes/4096<=C->Media.LastBlock+1-Lba);
  ++read_calls;last_lba=Lba;last_bytes=Bytes;
  if(read_status!=EFI_SUCCESS)return read_status;
#if PIANO_UFS_PRODUCT_STORAGE
  if(timing)++timing->Read.Submitted;
  if(C==written_child){assert(!Lba && Bytes==written_bytes);memcpy(Buffer,written_block,Bytes);return EFI_SUCCESS;}
#endif
  UINT64 Physical=(C->Info.Info.Gpt.StartingLBA+Lba)*4096;
  for(UINTN I=0;I<Bytes;++I)((UINT8 *)Buffer)[I]=at(Physical+I);
  if(revoke_on_read==1)++C->Media.MediaId;
  if(revoke_on_read==2)PianoFastbootBlockReadStop();
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI write_block(EFI_BLOCK_IO_PROTOCOL *Block,UINT32 MediaId,EFI_LBA Lba,UINTN Bytes,VOID *Buffer){
  assert(FALSE);
  return EFI_WRITE_PROTECTED;
}
#if PIANO_UFS_PRODUCT_STORAGE
EFI_STATUS PianoStorageOpen(CONST CHAR16 *Name,PIANO_STORAGE_PARTITION *Storage){
  for(UINTN I=0;I<child_count;++I){
    CHILD *C=&children[I];
    if(StrnCmp(Name,C->Info.Info.Gpt.PartitionName,36))continue;
    *Storage=(PIANO_STORAGE_PARTITION){
      C->Path.Ufs.Lun,4096,C->Media.MediaId,C->Info.Info.Gpt.StartingLBA,
      C->Media.LastBlock+1,C->Info.Info.Gpt.UniquePartitionGUID,C
    };
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}
EFI_STATUS PianoStorageWrite(CONST PIANO_STORAGE_PARTITION *Storage,UINT64 Lba,UINTN Bytes,CONST VOID *Buffer){
  CHILD *C=Storage->Identity;
  assert(!C->Mounted && C->Media.ReadOnly && Storage->MediaId==C->Media.MediaId && !Lba);
  assert(Bytes && Bytes<=sizeof(written_block) && !(Bytes%4096));
  ++write_calls;
  if(timing)++timing->Write.Submitted;
  written_child=C;written_bytes=Bytes;memcpy(written_block,Buffer,Bytes);
  if(revoke_on_write){PianoFastbootBlockReadStop();assert(PianoFastbootBlockReadInit()==EFI_SUCCESS);}
  return EFI_SUCCESS;
}
EFI_STATUS PianoStorageFlush(CONST PIANO_STORAGE_PARTITION *Storage){
  assert(Storage->Identity==written_child);++flush_calls;
  if(timing)++timing->Sync.Submitted;
  if(revoke_on_flush){PianoFastbootBlockReadStop();assert(PianoFastbootBlockReadInit()==EFI_SUCCESS);}
  return EFI_SUCCESS;
}
#endif
static VOID node(EFI_DEVICE_PATH_PROTOCOL *H,UINT8 Type,UINT8 Sub,UINTN Bytes){H->Type=Type;H->SubType=Sub;H->Length[0]=(UINT8)Bytes;H->Length[1]=(UINT8)(Bytes>>8);}
static VOID gpt_name(CHILD *C,CONST CHAR8 *Name){ZeroMem(C->Info.Info.Gpt.PartitionName,sizeof(C->Info.Info.Gpt.PartitionName));for(UINTN I=0;Name[I];++I)C->Info.Info.Gpt.PartitionName[I]=(UINT8)Name[I];}
static VOID valid(UINTN Count) {
  PianoFastbootBlockReadStop();ZeroMem(children,sizeof(children));child_count=Count;read_calls=write_calls=0;locate_status=read_status=EFI_SUCCESS;null_handles=FALSE;revoke_on_read=0;
  bs.LocateHandleBuffer=locate;bs.HandleProtocol=handle;
  for(UINTN I=0;I<Count;++I) {
    CHILD *C=&children[I];C->Block.Revision=EFI_BLOCK_IO_PROTOCOL_REVISION;C->Block.Media=&C->Media;C->Block.ReadBlocks=read;C->Block.WriteBlocks=write_block;
    C->Media=(EFI_BLOCK_IO_MEDIA){.MediaId=17,.MediaPresent=TRUE,.LogicalPartition=TRUE,.ReadOnly=TRUE,.BlockSize=4096,.IoAlign=1,.LastBlock=31};
    C->Info.Revision=EFI_PARTITION_INFO_PROTOCOL_REVISION;C->Info.Type=PARTITION_TYPE_GPT;
    C->Info.Info.Gpt=(EFI_PARTITION_ENTRY){.PartitionTypeGUID={.Data1=0x123},.UniquePartitionGUID={.Data1=(UINT32)(0x456+I)},.StartingLBA=4096+I*256,.EndingLBA=4096+I*256+31};
    CHAR8 Name[37];snprintf(Name,sizeof(Name),I?"part_%lu":"xbl_config_a",(unsigned long)I);gpt_name(C,Name);
    node(&C->Path.Vendor.Header,HARDWARE_DEVICE_PATH,HW_VENDOR_DP,sizeof(C->Path.Vendor));C->Path.Vendor.Guid=mVendor;
    node(&C->Path.Ufs.Header,MESSAGING_DEVICE_PATH,MSG_UFS_DP,sizeof(C->Path.Ufs));C->Path.Ufs.Pun=0;C->Path.Ufs.Lun=I%6;
    node(&C->Path.Hd.Header,MEDIA_DEVICE_PATH,MEDIA_HARDDRIVE_DP,sizeof(C->Path.Hd));C->Path.Hd.PartitionNumber=(UINT32)I+1;
    C->Path.Hd.PartitionStart=C->Info.Info.Gpt.StartingLBA;C->Path.Hd.PartitionSize=32;CopyMem(C->Path.Hd.Signature,&C->Info.Info.Gpt.UniquePartitionGUID,16);
    C->Path.Hd.MBRType=MBR_TYPE_EFI_PARTITION_TABLE_HEADER;C->Path.Hd.SignatureType=SIGNATURE_TYPE_GUID;
    node(&C->Path.End,END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,4);
  }
}
static VOID reject(VOID){assert(PianoFastbootBlockReadInit()==EFI_NOT_FOUND && !PianoFastbootBlockReadStorage() && !pools && !read_calls && !write_calls);}
static UINT8 frames[3][PIANO_FASTBOOT_MAX_FETCH_BYTES];static UINTN frame_bytes[3],frame_count;
static EFI_STATUS send(VOID *Context,CONST VOID *Data,UINTN Bytes){assert(!Context && frame_count<3 && Bytes<=sizeof(frames[0]));CopyMem(frames[frame_count],Data,Bytes);frame_bytes[frame_count++]=Bytes;return EFI_SUCCESS;}
#if PIANO_UFS_PRODUCT_STORAGE
static VOID test_partition_write_scope(VOID){
  valid(3);
  for(UINTN I=0;I<3;++I){
    children[I].Mounted=TRUE;
  }
  gpt_name(&children[1],"sunuefi_esp");gpt_name(&children[2],"persist");
  UINTN EspBlocks=2*PIANO_FASTBOOT_MAX_BLOCK_IO_BYTES/4096;
  children[1].Media.LastBlock=EspBlocks-1;
  children[1].Info.Info.Gpt.EndingLBA=children[1].Info.Info.Gpt.StartingLBA+EspBlocks-1;
  children[1].Path.Hd.PartitionSize=EspBlocks;
  assert(PianoFastbootBlockReadInit()==EFI_SUCCESS);
  CONST PIANO_FB_STORAGE *S=PianoFastbootBlockReadStorage();
  PIANO_FB_PARTITION_INFO Esp,Factory,Persist;
  assert(S->Info(NULL,"sunuefi_esp",&Esp)==EFI_SUCCESS && S->Info(NULL,"xbl_config_a",&Factory)==EFI_SUCCESS &&
    S->Info(NULL,"persist",&Persist)==EFI_SUCCESS && !write_calls && !close_calls);
  expected_close=&children[1];children[1].OpenFiles=TRUE;
  assert(S->PrepareWrite(NULL,&Esp)==EFI_ACCESS_DENIED && !close_calls && !write_calls);
  children[1].OpenFiles=FALSE;

  // One full batch and another FILL block. A remount stops the second
  // write, but the first verified batch must still be flushed.
  UINT8 Sparse[44]={0x3a,0xff,0x26,0xed,1,0,0,0,28,0,12,0,0,16,0,0,0,0,0,0,1,0,0,0,
    0,0,0,0,0xc2,0xca,0,0,0,0,0,0,16,0,0,0,0x41,0x42,0x43,0x44};
  UINT32 Blocks=PIANO_FASTBOOT_MAX_BLOCK_IO_BYTES/4096+1;
  for(UINTN I=0;I<4;++I)Sparse[16+I]=Sparse[32+I]=(UINT8)(Blocks>>(8*I));
  PIANO_FASTBOOT Fb;assert(PianoFastbootInit(&Fb,NULL,send,NULL)==EFI_SUCCESS && PianoFastbootSetStorage(&Fb,S)==EFI_SUCCESS);
  frame_count=0;
  CONST CHAR8 Limit[]="getvar:max-fetch-size",Fetch[]="fetch:sunuefi_esp:0x0:0x40000";
  assert(PianoFastbootPacket(&Fb,Limit,sizeof(Limit)-1)==EFI_SUCCESS && frame_count==1 && !memcmp(frames[0],"OKAY0x00040000",14));
  frame_count=0;assert(PianoFastbootPacket(&Fb,Fetch,sizeof(Fetch)-1)==EFI_SUCCESS);
  assert(frame_count==3 && frame_bytes[1]==256*1024 && !memcmp(frames[0],"DATA00040000",12));
  assert(read_calls==1 && last_bytes==frame_bytes[1] && !timing && !timing_reports && !write_calls);
  UINT64 Start=children[1].Info.Info.Gpt.StartingLBA*4096;
  for(UINTN I=0;I<frame_bytes[1];++I)assert(frames[1][I]==at(Start+I));
  read_calls=frame_count=0;
  assert(PianoFastbootPacket(&Fb,"download:0000002c",17)==EFI_SUCCESS &&
    PianoFastbootPacket(&Fb,Sparse,sizeof(Sparse))==EFI_SUCCESS);
  frame_count=0;
  assert(PianoFastbootPacket(&Fb,"flash:sunuefi_esp",sizeof("flash:sunuefi_esp")-1)==EFI_SUCCESS);
  while(!write_calls)assert(PianoFastbootFlashPending(&Fb) && PianoFastbootFlashStep(&Fb)==EFI_SUCCESS);
  assert(write_calls==1 && read_calls==1 && last_bytes==sizeof(written_block) && !flush_calls && close_calls==1 &&
    children[0].Mounted && !children[1].Mounted && children[2].Mounted);
  assert(!timing && !timing_reports && mWriteTiming.Read.Submitted==1 && mWriteTiming.Write.Submitted==1);
  children[1].Mounted=TRUE;
  while(PianoFastbootFlashPending(&Fb))assert(PianoFastbootFlashStep(&Fb)==EFI_SUCCESS);
  assert(write_calls==1 && flush_calls==1 && frame_count==2 && !memcmp(frames[1],"FAIL",4));
  assert(!timing && timing_reports==1 && mWriteTiming.Sync.Submitted==1 && !mWriteTimingToken);
  PianoFastbootReset(&Fb);

  UINT8 Data[4096]={0};
  assert(Factory.ReadOnly && Persist.ReadOnly && !Esp.ReadOnly);
  assert(S->PrepareWrite(NULL,&Factory)==EFI_WRITE_PROTECTED);
  assert(S->WriteBlocks(NULL,&Factory,0,sizeof(Data),Data)==EFI_WRITE_PROTECTED);
  assert(S->PrepareWrite(NULL,&Persist)==EFI_WRITE_PROTECTED);
  assert(S->Flush(NULL,&Persist)==EFI_WRITE_PROTECTED);
  assert(close_calls==1 && write_calls==1 && flush_calls==1 && children[0].Mounted && children[2].Mounted);

  expected_close=&children[1];
  assert(S->PrepareWrite(NULL,&Esp)==EFI_SUCCESS);
  revoke_on_write=TRUE;
  assert(S->WriteBlocks(NULL,&Esp,0,sizeof(Data),Data)==EFI_MEDIA_CHANGED);
  assert(S->Info(NULL,"sunuefi_esp",&Esp)==EFI_SUCCESS);
  revoke_on_flush=TRUE;
  assert(S->Flush(NULL,&Esp)==EFI_MEDIA_CHANGED && flush_calls==2);
  PianoFastbootBlockReadStop();
  assert(!pools);puts("Target-only Fastboot writes: factory and persist read-only, open files, sparse remount, final sync and write/flush token revocation passed.");
}
#endif
int main(void) {
  valid(0);assert(PianoFastbootBlockReadInit()==EFI_NOT_FOUND && !pools);
  valid(1);null_handles=TRUE;assert(PianoFastbootBlockReadInit()==EFI_COMPROMISED_DATA && !pools);
  valid(1);locate_status=EFI_DEVICE_ERROR;assert(PianoFastbootBlockReadInit()==EFI_DEVICE_ERROR && !pools);
  valid(1);assert(PianoFastbootBlockReadInit()==EFI_SUCCESS && !pools);assert(PianoFastbootBlockReadInit()==EFI_ALREADY_STARTED);
  CONST PIANO_FB_STORAGE *S=PianoFastbootBlockReadStorage();assert(S && S->Ready(S->Context)==EFI_SUCCESS);
  PIANO_FB_PARTITION_INFO Info;assert(S->Info(NULL,"xbl_config_a",&Info)==EFI_SUCCESS && Info.Bytes==131072 && Info.BlockSize==4096 && Info.ReadOnly && Info.Token);
  assert(S->Info(NULL,"XBL_config_a",&Info)==EFI_NOT_FOUND);assert(S->Info(NULL,"xbl_config_a",&Info)==EFI_SUCCESS);
  UINT8 Data[65536];assert(S->ReadBlocks(NULL,&Info,1,4096,Data)==EFI_SUCCESS && last_lba==1 && last_bytes==4096);
  for(UINTN I=0;I<4096;++I)assert(Data[I]==at((4096ULL+1)*4096+I));
  assert(S->ReadBlocks(NULL,&Info,31,8192,Data)==EFI_INVALID_PARAMETER && S->ReadBlocks(NULL,&Info,0,4095,Data)==EFI_INVALID_PARAMETER);
  assert(S->ReadBlocks(NULL,&Info,0,65537,Data)==EFI_INVALID_PARAMETER);
  PIANO_FB_PARTITION_INFO Bad=Info;Bad.Token=&children[0];assert(S->ReadBlocks(NULL,&Bad,0,4096,Data)==EFI_INVALID_PARAMETER);
  Bad=Info;Bad.Name[0]='X';assert(S->ReadBlocks(NULL,&Bad,0,4096,Data)==EFI_INVALID_PARAMETER);
  read_status=EFI_WARN_UNKNOWN_GLYPH;assert(S->ReadBlocks(NULL,&Info,0,4096,Data)==EFI_DEVICE_ERROR);read_status=EFI_SUCCESS;
  PIANO_FASTBOOT Fb;assert(PianoFastbootInit(&Fb,NULL,send,NULL)==EFI_SUCCESS && PianoFastbootSetStorage(&Fb,S)==EFI_SUCCESS);
  frame_count=0;read_calls=0;CONST CHAR8 *Cmd="fetch:xbl_config_a:0x00000ffe:0x00000005";
  assert(PianoFastbootPacket(&Fb,Cmd,strlen(Cmd))==EFI_SUCCESS && frame_count==3 && read_calls==2 && frame_bytes[1]==5 && !memcmp(frames[0],"DATA00000005",12));
  for(UINTN I=0;I<5;++I)assert(frames[1][I]==at(4096ULL*4096+4094+I));
  assert(!pools);
  PIANO_FB_PARTITION_INFO Old=Info;PianoFastbootBlockReadStop();assert(!PianoFastbootBlockReadStorage() && S->Ready(NULL)==EFI_NOT_READY && S->ReadBlocks(NULL,&Old,0,4096,Data)==EFI_NOT_READY);
  valid(1);assert(PianoFastbootBlockReadInit()==EFI_SUCCESS && S->Info(NULL,"xbl_config_a",&Info)==EFI_SUCCESS && Info.Token!=Old.Token);
  assert(S->ReadBlocks(NULL,&Old,0,4096,Data)==EFI_INVALID_PARAMETER); // Stop/reinit ABA rejected.
  // Every observed property is rechecked before issuing a real block read.
  CHILD Saved=children[0];
  for(UINTN Case=0;Case<12;++Case) {
    children[0]=Saved;
    switch(Case) {
      case 0:++children[0].Media.MediaId;break;case 1:children[0].Media.ReadOnly=FALSE;break;
      case 2:children[0].Media.MediaPresent=FALSE;break;case 3:children[0].Media.BlockSize=512;break;
      case 4:++children[0].Media.LastBlock;break;case 5:children[0].Media.IoAlign=16;break;
      case 6:children[0].Block.ReadBlocks=NULL;break;case 7:++children[0].Path.Hd.PartitionStart;break;
      case 8:++children[0].Path.Hd.PartitionNumber;break;case 9:children[0].Path.Hd.Signature[0]^=1;break;
      case 10:children[0].Info.Info.Gpt.PartitionName[0]='X';break;case 11:children[0].Missing=1;break;
    }
    UINTN Before=read_calls;assert(S->Ready(NULL)!=EFI_SUCCESS && S->ReadBlocks(NULL,&Info,0,4096,Data)!=EFI_SUCCESS && read_calls==Before);
  }
  children[0]=Saved;revoke_on_read=1;assert(S->ReadBlocks(NULL,&Info,0,4096,Data)==EFI_MEDIA_CHANGED);children[0]=Saved;
  revoke_on_read=2;assert(S->ReadBlocks(NULL,&Info,0,4096,Data)==EFI_MEDIA_CHANGED);assert(S->Ready(NULL)==EFI_NOT_READY);
  for(UINTN Case=0;Case<28;++Case) {
    valid(1);CHILD *C=&children[0];
    switch(Case) {
      case 0:C->Path.Vendor.Guid.Data1^=1;break;case 1:C->Path.Vendor.Header.Length[0]--;break;
      case 2:C->Path.Ufs.Header.Length[0]++;break;case 3:C->Path.Ufs.Lun=6;break;case 4:C->Path.Ufs.Pun=1;break;
      case 5:C->Path.Hd.Header.Length[0]=4;break;case 6:C->Path.Hd.PartitionNumber=0;break;
      case 7:C->Path.Hd.PartitionStart++;break;case 8:C->Path.Hd.PartitionSize++;break;case 9:C->Path.Hd.Signature[0]^=1;break;
      case 10:C->Path.Hd.SignatureType=SIGNATURE_TYPE_MBR;break;case 11:C->Path.End.SubType=END_INSTANCE_DEVICE_PATH_SUBTYPE;break;
      case 12:C->Path.End.Type=MESSAGING_DEVICE_PATH;break;case 13:C->Path.End.Length[0]=8;break;
      case 14:ZeroMem(&C->Info.Info.Gpt.UniquePartitionGUID,16);ZeroMem(C->Path.Hd.Signature,16);break;
      case 15:ZeroMem(&C->Info.Info.Gpt.PartitionTypeGUID,16);break;case 16:C->Info.Info.Gpt.EndingLBA=C->Info.Info.Gpt.StartingLBA-1;break;
      case 17:C->Media.IoAlign=4096;break;case 18:C->Block.ReadBlocks=NULL;break;case 19:C->Media.LogicalPartition=FALSE;break;
      case 20:C->Info.Info.Gpt.PartitionName[0]=0x100;break;case 21:C->Info.Info.Gpt.PartitionName[30]='Z';break;case 22:C->Block.Revision=0;break;
      case 23:C->Info.Revision=0;break;
      case 24:C->Media.LastBlock=MAX_UINT64/4096;break;
      case 25:C->Info.Info.Gpt.StartingLBA=0;C->Info.Info.Gpt.EndingLBA=31;C->Path.Hd.PartitionStart=0;break;
      case 26:C->Info.Info.Gpt.EndingLBA=MAX_UINT64;break;
      case 27:C->Path.Hd.MBRType=1;break;
    }
    reject();
  }
  valid(2);gpt_name(&children[1],"xbl_config_a");assert(PianoFastbootBlockReadInit()==EFI_SUCCESS && S->Info(NULL,"xbl_config_a",&Info)==EFI_NO_MAPPING);
  valid(1);CHAR8 Full[37];memset(Full,'A',36);Full[36]=0;gpt_name(&children[0],Full);assert(PianoFastbootBlockReadInit()==EFI_SUCCESS && S->Info(NULL,Full,&Info)==EFI_SUCCESS);
  valid(MAX_PARTITIONS+1);assert(PianoFastbootBlockReadInit()==EFI_OUT_OF_RESOURCES && !PianoFastbootBlockReadStorage() && !pools);
#if PIANO_UFS_PRODUCT_STORAGE
  test_partition_write_scope();
#endif
  valid(1);mNextToken=MAX_UINTN;assert(PianoFastbootBlockReadInit()==EFI_OUT_OF_RESOURCES && !pools);PianoFastbootBlockReadStop();
  assert(!write_calls && !pools);puts("Actual EFI BlockIO bridge + fetch: owned path/HD-GPT geometry/GUID, canonical names/ambiguity, MediaId/read-only/function revocation, stale-cookie ABA, alignment, partial4K, failure cleanup and zero writes passed.");
  return 0;
}
