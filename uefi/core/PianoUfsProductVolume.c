// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoUfsProductVolume.h"
#include "PianoGpt.h"
#include "PianoUfsWriteGuard.h"
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#define PRODUCT_SIGNATURE SIGNATURE_32('P','V','O','L')
#define DISK_LAST 378879ULL
STATIC UINT32 Le32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
STATIC UINT64 Le64(CONST UINT8 *P){return Le32(P)|((UINT64)Le32(P+4)<<32);}
STATIC VOID Put32(UINT8 *P,UINT32 V){for(UINTN I=0;I<4;++I)P[I]=(UINT8)(V>>(8*I));}
STATIC BOOLEAN Zero(CONST VOID *Data,UINTN N){CONST UINT8 *P=Data;for(UINTN I=0;I<N;++I)if(P[I])return FALSE;return TRUE;}
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC UINTN FirstDifference(CONST UINT8 *A,CONST UINT8 *B,UINTN Bytes){for(UINTN I=0;I<Bytes;++I)if(A[I]!=B[I])return I;return Bytes;}
// Diagnostics only: index is zero-based; offset is within the named header or
// entry array. MAX_UINTN means this failure has no single differing byte.
STATIC EFI_STATUS GptReject(PIANO_UFS_PRODUCT_VOLUME *D,CONST CHAR8 *Phase,EFI_STATUS Status,UINTN Index,UINTN Offset){
  DEBUG((DEBUG_WARN,"SUNUEFI_PRODUCT_GPT_REJECT phase=%a status=%r has_index=%u index=%lu has_offset=%u offset=%lu\n",Phase,Status,Index!=MAX_UINTN,(UINT64)Index,Offset!=MAX_UINTN,(UINT64)Offset));
  for(UINTN I=12;I<=46;I+=34)DEBUG((DEBUG_WARN,"SUNUEFI_PRODUCT_GPT_BOOT_ATTRS entry=%lu current=%016lx\n",(UINT64)I,Le64(D->Entries+I*128+48)));
  return Status;
}
STATIC EFI_STATUS GptDifference(PIANO_UFS_PRODUCT_VOLUME *D,CONST CHAR8 *Phase,EFI_STATUS Status,CONST UINT8 *Current,CONST UINT8 *Expected,UINTN Bytes,UINTN Index,UINTN Base){
  UINTN I=FirstDifference(Current,Expected,Bytes);
  if(I<Bytes)DEBUG((DEBUG_WARN,"SUNUEFI_PRODUCT_GPT_BYTE phase=%a offset=%lu current=%02x expected=%02x\n",Phase,(UINT64)(Base+I),Current[I],Expected[I]));
  return GptReject(D,Phase,Status,Index,I<Bytes?Base+I:MAX_UINTN);
}
STATIC VOID Fail(PIANO_UFS_PRODUCT_VOLUME *D,EFI_STATUS S){D->State.Quarantined=D->State.NeedsRecovery=TRUE;D->Media.ReadOnly=TRUE;D->State.LastStatus=Exact(S);if(D->State.FirstFailure==EFI_SUCCESS)D->State.FirstFailure=Exact(S);++D->State.Failures;}
STATIC EFI_STATUS Quiet(PIANO_UFS_PRODUCT_VOLUME *D){BOOLEAN Q=FALSE;EFI_STATUS S=Exact(D->Transport.Io.Quiesced(D->Transport.Io.Context,&Q));if(S==EFI_SUCCESS && !Q)S=EFI_NOT_READY;D->State.LastQuietStatus=S;return S;}
STATIC EFI_STATUS ReadExact(PIANO_UFS_PRODUCT_VOLUME *D,EFI_LBA Lba,UINTN Bytes,VOID *Buffer){
  SetMem(Buffer,Bytes,0xCC);UINTN Got=0;EFI_STATUS S=Exact(D->Transport.Io.Read(D->Transport.Io.Context,4,Lba,Bytes,Buffer,&Got));if(S==EFI_SUCCESS && Got!=Bytes)S=EFI_BAD_BUFFER_SIZE;EFI_STATUS Q=Quiet(D);return Q!=EFI_SUCCESS?Q:S;
}
STATIC EFI_STATUS CheckGpt(PIANO_UFS_PRODUCT_VOLUME *D){
  PIANO_GPT_HEADER H;EFI_STATUS S=PianoGptParseHeader(D->Primary,4096,DISK_LAST,4096,&H);if(S!=EFI_SUCCESS){DEBUG((DEBUG_WARN,"SUNUEFI_PRODUCT_GPT_PRIMARY revision=%08x header_bytes=%u header_crc=%08x current_lba=%lx backup_lba=%lx\n",Le32(D->Primary+8),Le32(D->Primary+12),Le32(D->Primary+16),Le64(D->Primary+24),Le64(D->Primary+32)));return GptReject(D,"primary-header-parse",Exact(S),MAX_UINTN,MAX_UINTN);}
  UINTN Active=0;S=PianoGptCheckEntries(D->Entries,12288,&H,&Active);if(S!=EFI_SUCCESS){
    DEBUG((DEBUG_WARN,"SUNUEFI_PRODUCT_GPT_ARRAY expected_crc=%08x current_crc=%08x active=%lu\n",H.ArrayCrc,PianoGptCrc32(D->Entries,12288),(UINT64)Active));
    return GptReject(D,"entries-parse",Exact(S),MAX_UINTN,MAX_UINTN);
  }
  if(H.Entries!=96 || H.EntryBytes!=128 || H.ArrayBytes!=12288 || H.EntryLba!=2 ||
     CompareMem(D->Entries,D->BackupEntries,12288)){
    if(H.Entries!=96 || H.EntryBytes!=128 || H.ArrayBytes!=12288 || H.EntryLba!=2){
      DEBUG((DEBUG_WARN,"SUNUEFI_PRODUCT_GPT_LAYOUT entries=%u entry_bytes=%u array_bytes=%lu entry_lba=%lx\n",H.Entries,H.EntryBytes,(UINT64)H.ArrayBytes,H.EntryLba));return GptReject(D,"entries-layout",EFI_COMPROMISED_DATA,MAX_UINTN,MAX_UINTN);
    }
    UINTN I=FirstDifference(D->Entries,D->BackupEntries,12288);return GptDifference(D,"entries-mirror",EFI_COMPROMISED_DATA,D->Entries,D->BackupEntries,12288,I/128,0);
  }
  // Validate the live backup GPT independently before deciding whether this
  // disk contains our writable volume. An ordinary valid GPT needs no match
  // against the factory snapshot and no device write capabilities.
  UINT8 B[4096];UINT32 BackupSize=Le32(D->Backup+12);
  if(CompareMem(D->Backup,"EFI PART",8) || Le32(D->Backup+8)!=0x10000 ||
     BackupSize<92 || BackupSize>4096 || Le32(D->Backup+20)!=0 ||
     Le64(D->Backup+24)!=DISK_LAST || Le64(D->Backup+32)!=1)
    return GptReject(D,"backup-header-parse",EFI_COMPROMISED_DATA,MAX_UINTN,MAX_UINTN);
  CopyMem(B,D->Backup,4096);Put32(B+16,0);
  if(PianoGptCrc32(B,BackupSize)!=Le32(D->Backup+16)){
    DEBUG((DEBUG_WARN,"SUNUEFI_PRODUCT_GPT_BACKUP expected_crc=%08x current_crc=%08x\n",Le32(D->Backup+16),PianoGptCrc32(B,BackupSize)));return GptReject(D,"backup-header-crc",EFI_CRC_ERROR,MAX_UINTN,16);
  }
  if(BackupSize!=Le32(D->Primary+12) || Le64(D->Backup+72)!=378873 ||
     Le64(D->Backup+72)<=H.LastUsable || H.ArrayBytes/4096+(H.ArrayBytes%4096!=0)>DISK_LAST-Le64(D->Backup+72) ||
     CompareMem(D->Backup+40,D->Primary+40,32) || CompareMem(D->Backup+80,D->Primary+80,12)){
    DEBUG((DEBUG_WARN,"SUNUEFI_PRODUCT_GPT_BACKUP header_bytes=%u expected_bytes=%u entry_lba=%lx first_usable=%lx last_usable=%lx entries=%u entry_bytes=%u array_crc=%08x\n",BackupSize,Le32(D->Primary+12),Le64(D->Backup+72),Le64(D->Backup+40),Le64(D->Backup+48),Le32(D->Backup+80),Le32(D->Backup+84),Le32(D->Backup+88)));
    return GptReject(D,"backup-header-shared-fields",EFI_COMPROMISED_DATA,MAX_UINTN,MAX_UINTN);
  }
  CONST UINT8 *E=D->Entries+PIANO_PRODUCT_VOLUME_GPT_INDEX*128;EFI_GUID Type=PIANO_PRODUCT_STORAGE_TYPE_GUID;
  if(CompareMem(E,&Type,16))return GptReject(D,"owned-entry-absent",EFI_NOT_FOUND,PIANO_PRODUCT_VOLUME_GPT_INDEX,PIANO_PRODUCT_VOLUME_GPT_INDEX*128);
  if(Zero(E+16,16) || Le64(E+32)!=PIANO_UFS_WINDOW_FIRST ||
     Le64(E+40)!=PIANO_UFS_WINDOW_LAST || Le64(E+48)!=2){
    UINTN Offset=Zero(E+16,16)?16:Le64(E+32)!=PIANO_UFS_WINDOW_FIRST?32:Le64(E+40)!=PIANO_UFS_WINDOW_LAST?40:48;
    DEBUG((DEBUG_WARN,"SUNUEFI_PRODUCT_GPT_OWNED active=%lu first=%lx last=%lx attrs=%lx expected_first=%lx expected_last=%lx expected_attrs=2\n",(UINT64)Active,Le64(E+32),Le64(E+40),Le64(E+48),PIANO_UFS_WINDOW_FIRST,PIANO_UFS_WINDOW_LAST));
    return GptReject(D,"owned-entry-fields",EFI_SECURITY_VIOLATION,PIANO_PRODUCT_VOLUME_GPT_INDEX,Offset==MAX_UINTN?Offset:PIANO_PRODUCT_VOLUME_GPT_INDEX*128+Offset);
  }
  STATIC CONST CHAR16 Name[]=L"PianoUEFI Storage";
  if(CompareMem(E+56,Name,sizeof(Name)) || !Zero(E+56+sizeof(Name),72-sizeof(Name))){
    UINTN Offset=FirstDifference(E+56,(CONST UINT8 *)Name,sizeof(Name));if(Offset==sizeof(Name))while(Offset<72 && E[56+Offset]==0)++Offset;
    return GptReject(D,"owned-entry-name",EFI_SECURITY_VIOLATION,PIANO_PRODUCT_VOLUME_GPT_INDEX,PIANO_PRODUCT_VOLUME_GPT_INDEX*128+56+Offset);
  }
  for(UINTN I=0;I<95;++I){CONST UINT8 *A=D->Entries+I*128;if(Zero(A,16))continue;
    if(!CompareMem(A+16,E+16,16) || (Le64(A+32)<=PIANO_UFS_WINDOW_LAST && Le64(A+40)>=PIANO_UFS_WINDOW_FIRST)){DEBUG((DEBUG_WARN,"SUNUEFI_PRODUCT_GPT_CONFLICT entry=%lu first=%lx last=%lx duplicate_uuid=%u\n",(UINT64)I,Le64(A+32),Le64(A+40),!CompareMem(A+16,E+16,16)));return GptReject(D,"owned-entry-conflict",EFI_SECURITY_VIOLATION,I,I*128+(!CompareMem(A+16,E+16,16)?16:32));}}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS CheckHeader(PIANO_UFS_PRODUCT_VOLUME *D){
  CONST UINT8 *H=D->Header;EFI_GUID Type=PIANO_PRODUCT_STORAGE_TYPE_GUID;
  if(CompareMem(H,D->HeaderCopy,4096) || CompareMem(H,"PIANO-VOLUME-v1",16) || Le32(H+16)!=1 || Le32(H+20)!=128 ||
     Le32(H+28)!=PIANO_PRODUCT_VOLUME_LAYOUT || Zero(H+32,16) || CompareMem(H+32,D->Entries+95*128+16,16) ||
     CompareMem(H+48,D->Primary+56,16) || CompareMem(H+64,&Type,16) || Le32(H+80)!=4 || Le32(H+84)!=4096 ||
     Le64(H+88)!=375040 || Le32(H+96)!=3584 || Le32(H+100)!=2 || Le32(H+104)!=2046 || Le32(H+108)!=2048 ||
     Le32(H+112)!=2816 || Le32(H+116)!=768 || Le32(H+120)!=95 || !Zero(H+124,4096-124))return EFI_SECURITY_VIOLATION;
  UINT8 Copy[4096];CopyMem(Copy,H,4096);Put32(Copy+24,0);return PianoGptCrc32(Copy,4096)==Le32(H+24)?EFI_SUCCESS:EFI_CRC_ERROR;
}
STATIC EFI_STATUS Gate(PIANO_UFS_PRODUCT_VOLUME *D){
  ZeroMem(&D->LastGuard,sizeof(D->LastGuard));EFI_STATUS S;
  VOID *Buffers[]={D->Primary,D->Entries,D->Backup,D->BackupEntries};CONST EFI_LBA Lba[]={1,2,378879,378873};CONST UINTN Bytes[]={4096,12288,4096,12288};
  for(UINTN I=0;I<4;++I){S=ReadExact(D,Lba[I],Bytes[I],Buffers[I]);if(S!=EFI_SUCCESS)return S;}
  S=CheckGpt(D);if(S!=EFI_SUCCESS)return S;
  S=Exact(D->Transport.Io.ReadGuard(D->Transport.Io.Context,4,&D->LastGuard));
  if(S==EFI_SUCCESS)S=PianoUfsWriteGuardCheck(&D->LastGuard);EFI_STATUS Q=Quiet(D);if(Q!=EFI_SUCCESS)return Q;if(S!=EFI_SUCCESS)return S;
  S=ReadExact(D,375040,4096,D->Header);if(S==EFI_SUCCESS)S=ReadExact(D,375041,4096,D->HeaderCopy);if(S!=EFI_SUCCESS)return S;
  S=CheckHeader(D);if(S==EFI_SUCCESS && D->State.Provisioned && CompareMem(D->NvIo.VolumeUuid,D->Header+32,16))S=EFI_MEDIA_CHANGED;
  D->State.LastGuardStatus=S;return S;
}
EFI_STATUS PianoUfsProductVolumeRefreshReservation(PIANO_UFS_PRODUCT_VOLUME *D){
  if(!D || D->Signature!=PRODUCT_SIGNATURE || !D->State.Busy || D->State.Quarantined || D->State.Closed)return EFI_ACCESS_DENIED;
  EFI_STATUS S=Gate(D);if(S!=EFI_SUCCESS)Fail(D,S);return S;
}
STATIC EFI_STATUS Enter(PIANO_UFS_PRODUCT_VOLUME *D){
  if(D->State.Busy)return EFI_NOT_READY;D->State.Busy=TRUE;EFI_STATUS S=Exact(D->Transport.Acquire(D->Transport.Io.Context));
  if(S!=EFI_SUCCESS){D->State.Busy=FALSE;if(S!=EFI_UNSUPPORTED && S!=EFI_NOT_READY)Fail(D,S);}return S;
}
STATIC EFI_STATUS Leave(PIANO_UFS_PRODUCT_VOLUME *D){
  D->State.Busy=FALSE;EFI_STATUS S=Exact(D->Transport.Release(D->Transport.Io.Context,D->State.Quarantined));if(S!=EFI_SUCCESS)Fail(D,S);return S;
}
STATIC BOOLEAN Overlap(PIANO_UFS_PRODUCT_VOLUME *D,CONST VOID *Buffer,UINTN Bytes){
  UINTN P=(UINTN)Buffer,A=(UINTN)D;return !Buffer || P>MAX_UINTN-Bytes || (P<A+sizeof(*D) && A<P+Bytes);
}
STATIC EFI_STATUS Request(PIANO_UFS_PRODUCT_VOLUME *D,UINTN Blocks,UINT32 Block,UINTN Bytes,CONST VOID *Buffer){
  if(!D || D->Signature!=PRODUCT_SIGNATURE)return EFI_INVALID_PARAMETER;
  if(!D->State.Provisioned || D->State.Closed || !D->Media.MediaPresent)return EFI_NO_MEDIA;
  if(D->State.Quarantined)return EFI_DEVICE_ERROR;
  if(Bytes==0)return EFI_SUCCESS;
  if(Overlap(D,Buffer,Bytes))return EFI_INVALID_PARAMETER;
  if(Bytes%4096)return EFI_BAD_BUFFER_SIZE;
  if(Block>=Blocks || Bytes/4096>Blocks-Block)return EFI_INVALID_PARAMETER;return EFI_SUCCESS;
}
STATIC EFI_STATUS Transfer(PIANO_UFS_PRODUCT_VOLUME *D,UINT32 First,UINT32 Blocks,UINT32 Block,UINTN Bytes,VOID *Buffer,BOOLEAN WriteData,BOOLEAN Nv){
  EFI_STATUS S=Request(D,Blocks,Block,Bytes,Buffer);if(S!=EFI_SUCCESS || !Bytes)return S;
  D->State.Scope=Nv?PianoProductIoNv:PianoProductIoFat;S=Enter(D);if(S!=EFI_SUCCESS)return S;S=Gate(D);
  for(UINTN Offset=0;S==EFI_SUCCESS && Offset<Bytes;Offset+=4096){
    EFI_LBA Physical=375040+First+Block+Offset/4096;D->State.LastPhysical=Physical;
    if(WriteData){
      S=Gate(D);if(S!=EFI_SUCCESS)break;CopyMem(D->Tx,(UINT8 *)Buffer+Offset,4096);
      D->State.PendingWrite=D->State.NeedsRecovery=TRUE;++D->State.WriteAttempts;UINTN Done=0;
      S=Exact(D->Transport.Io.WriteFua(D->Transport.Io.Context,4,Physical,4096,D->Tx,&Done));if(S==EFI_SUCCESS && Done!=4096)S=EFI_BAD_BUFFER_SIZE;
      EFI_STATUS Q=Quiet(D);if(Q!=EFI_SUCCESS)S=Q;if(S!=EFI_SUCCESS)break;
      S=Exact(D->Transport.Io.Sync(D->Transport.Io.Context,4,375040,14680064));Q=Quiet(D);if(Q!=EFI_SUCCESS)S=Q;if(S!=EFI_SUCCESS)break;
      for(UINTN I=0;I<4096;++I)D->Rx[I]=(UINT8)~D->Tx[I];UINTN ReadBytes=0;
      S=Exact(D->Transport.Io.Read(D->Transport.Io.Context,4,Physical,4096,D->Rx,&ReadBytes));
      if(S==EFI_SUCCESS && ReadBytes!=4096)S=EFI_BAD_BUFFER_SIZE;Q=Quiet(D);if(Q!=EFI_SUCCESS)S=Q;
      if(S==EFI_SUCCESS && CompareMem(D->Rx,D->Tx,4096))S=EFI_CRC_ERROR;if(S!=EFI_SUCCESS)break;
      S=Gate(D);if(S!=EFI_SUCCESS)break;
      D->State.PendingWrite=D->State.NeedsRecovery=FALSE;++D->State.VerifiedWrites;++D->State.Writes;
    }else{S=ReadExact(D,Physical,4096,D->Rx);if(S!=EFI_SUCCESS)break;CopyMem((UINT8 *)Buffer+Offset,D->Rx,4096);++D->State.Reads;}
  }
  if(S!=EFI_SUCCESS)Fail(D,S);else D->State.LastStatus=S;EFI_STATUS End=Leave(D);return S==EFI_SUCCESS?End:S;
}
STATIC EFI_STATUS Flush(PIANO_UFS_PRODUCT_VOLUME *D,PIANO_UFS_PRODUCT_IO_SCOPE Scope){
  if(!D || D->Signature!=PRODUCT_SIGNATURE || !D->State.Provisioned || D->State.Closed)return EFI_NO_MEDIA;
  if(D->State.Quarantined || D->State.PendingWrite)return EFI_ACCESS_DENIED;
  D->State.Scope=Scope;EFI_STATUS S=Enter(D);if(S!=EFI_SUCCESS)return S;S=Gate(D);
  if(S==EFI_SUCCESS){S=Exact(D->Transport.Io.Sync(D->Transport.Io.Context,4,375040,14680064));EFI_STATUS Q=Quiet(D);if(Q!=EFI_SUCCESS)S=Q;}
  if(S==EFI_SUCCESS)S=Gate(D);if(S!=EFI_SUCCESS)Fail(D,S);else{++D->State.Flushes;D->State.LastStatus=S;}EFI_STATUS End=Leave(D);return S==EFI_SUCCESS?End:S;
}
STATIC PIANO_UFS_PRODUCT_VOLUME *FromBlock(EFI_BLOCK_IO_PROTOCOL *B){if(!B)return NULL;PIANO_UFS_PRODUCT_VOLUME *D=BASE_CR(B,PIANO_UFS_PRODUCT_VOLUME,Block);return D->Signature==PRODUCT_SIGNATURE && B->Media==&D->Media?D:NULL;}
STATIC EFI_STATUS EFIAPI ResetBlock(EFI_BLOCK_IO_PROTOCOL *B,BOOLEAN Extended){(VOID)Extended;PIANO_UFS_PRODUCT_VOLUME *D=FromBlock(B);if(!D)return EFI_INVALID_PARAMETER;return D->State.Quarantined?EFI_DEVICE_ERROR:D->Media.MediaPresent?EFI_SUCCESS:EFI_NO_MEDIA;}
STATIC EFI_STATUS EFIAPI ReadBlock(EFI_BLOCK_IO_PROTOCOL *B,UINT32 Id,EFI_LBA Lba,UINTN Bytes,VOID *Buffer){PIANO_UFS_PRODUCT_VOLUME *D=FromBlock(B);if(!D || Lba>MAX_UINT32)return EFI_INVALID_PARAMETER;if(Id!=D->Media.MediaId)return EFI_MEDIA_CHANGED;return Transfer(D,2,2046,(UINT32)Lba,Bytes,Buffer,FALSE,FALSE);}
STATIC EFI_STATUS EFIAPI WriteBlock(EFI_BLOCK_IO_PROTOCOL *B,UINT32 Id,EFI_LBA Lba,UINTN Bytes,VOID *Buffer){PIANO_UFS_PRODUCT_VOLUME *D=FromBlock(B);if(!D || Lba>MAX_UINT32)return EFI_INVALID_PARAMETER;if(Id!=D->Media.MediaId)return EFI_MEDIA_CHANGED;if(D->Media.ReadOnly)return EFI_WRITE_PROTECTED;return Transfer(D,2,2046,(UINT32)Lba,Bytes,Buffer,TRUE,FALSE);}
STATIC EFI_STATUS EFIAPI FlushBlock(EFI_BLOCK_IO_PROTOCOL *B){PIANO_UFS_PRODUCT_VOLUME *D=FromBlock(B);return D?Flush(D,PianoProductIoFat):EFI_INVALID_PARAMETER;}
STATIC PIANO_UFS_PRODUCT_VOLUME *FromNv(VOID *Context){PIANO_UFS_PRODUCT_VOLUME *D=Context;return D && D->Signature==PRODUCT_SIGNATURE && D->NvIo.Context==D?D:NULL;}
STATIC EFI_STATUS ReadNv(VOID *C,UINT32 Slot,UINT32 Block,UINTN Bytes,VOID *Buffer){PIANO_UFS_PRODUCT_VOLUME *D=FromNv(C);if(!D || Slot>1)return EFI_INVALID_PARAMETER;return Transfer(D,Slot?2816:2048,768,Block,Bytes,Buffer,FALSE,TRUE);}
STATIC EFI_STATUS WriteNv(VOID *C,UINT32 Slot,UINT32 Block,UINTN Bytes,CONST VOID *Buffer){PIANO_UFS_PRODUCT_VOLUME *D=FromNv(C);if(!D || Slot>1)return EFI_INVALID_PARAMETER;return Transfer(D,Slot?2816:2048,768,Block,Bytes,(VOID *)Buffer,TRUE,TRUE);}
STATIC EFI_STATUS FlushNv(VOID *C){PIANO_UFS_PRODUCT_VOLUME *D=FromNv(C);return D?Flush(D,PianoProductIoNv):EFI_INVALID_PARAMETER;}
CONST PIANO_UFS_PRODUCT_NV_IO *PianoUfsProductVolumeNvIo(PIANO_UFS_PRODUCT_VOLUME *D){return D && D->Signature==PRODUCT_SIGNATURE && D->State.Provisioned && !D->State.Closed && !D->State.Quarantined?&D->NvIo:NULL;}
EFI_STATUS PianoUfsProductVolumeOpen(PIANO_UFS_PRODUCT_VOLUME *D,CONST PIANO_UFS_WINDOW_IO *T){
  if(!D || !T || !T->Acquire || !T->Release || !T->Io.Read || !T->Io.WriteFua || !T->Io.Sync || !T->Io.ReadGuard || !T->Io.Quiesced)return EFI_INVALID_PARAMETER;
  if(D->Signature)return EFI_ALREADY_STARTED;
  ZeroMem(D,sizeof(*D));D->Signature=PRODUCT_SIGNATURE;D->Transport=*T;
  D->Media=(EFI_BLOCK_IO_MEDIA){.MediaId=1,.MediaPresent=FALSE,.ReadOnly=TRUE,.BlockSize=4096,.IoAlign=1,.LastBlock=2045,.LogicalPartition=TRUE,.LogicalBlocksPerPhysicalBlock=1};
  D->Block=(EFI_BLOCK_IO_PROTOCOL){.Revision=EFI_BLOCK_IO_PROTOCOL_REVISION3,.Media=&D->Media,.Reset=ResetBlock,.ReadBlocks=ReadBlock,.WriteBlocks=WriteBlock,.FlushBlocks=FlushBlock};
  D->State.Scope=PianoProductIoProbe;EFI_STATUS S=Enter(D);if(S!=EFI_SUCCESS)return S;S=Gate(D);
  if(S==EFI_SUCCESS){D->State.Opened=D->State.Provisioned=TRUE;D->Media.MediaPresent=TRUE;D->Media.ReadOnly=FALSE;D->NvIo=(PIANO_UFS_PRODUCT_NV_IO){.Context=D,.LayoutId=1,.Read=ReadNv,.Write=WriteNv,.Flush=FlushNv};CopyMem(D->NvIo.VolumeUuid,D->Header+32,16);}
  else if(S!=EFI_NOT_FOUND)Fail(D,S);
  D->State.LastStatus=S;EFI_STATUS End=Leave(D);return S==EFI_SUCCESS?End:S;
}
EFI_STATUS PianoUfsProductVolumeClose(PIANO_UFS_PRODUCT_VOLUME *D){
  if(!D || D->Signature!=PRODUCT_SIGNATURE)return EFI_INVALID_PARAMETER;
  if(D->State.Busy || D->State.Quarantined || D->State.NeedsRecovery || D->State.PendingWrite)return EFI_ACCESS_DENIED;
  if(D->State.Provisioned && !D->State.Closed){EFI_STATUS S=Flush(D,PianoProductIoClose);if(S!=EFI_SUCCESS)return S;}
  D->State.Closed=TRUE;D->Media.MediaPresent=FALSE;D->Media.ReadOnly=TRUE;return EFI_SUCCESS;
}
