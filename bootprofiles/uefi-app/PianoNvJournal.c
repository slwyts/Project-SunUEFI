// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoNvJournal.h"
#include <Library/BaseMemoryLib.h>
#define SIG SIGNATURE_32('P','N','V','J')
STATIC CONST UINT8 HMagic[16]="PIANO-NVJRNL-v1",FMagic[16]="PIANO-NVCMIT-v1";
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC UINT32 U32(CONST UINT8 *P){return (UINT32)P[0]|(UINT32)P[1]<<8|(UINT32)P[2]<<16|(UINT32)P[3]<<24;}
STATIC UINT64 U64(CONST UINT8 *P){return U32(P)|((UINT64)U32(P+4)<<32);}
STATIC VOID W32(UINT8 *P,UINT32 V){for(UINTN I=0;I<4;++I)P[I]=(UINT8)(V>>(8*I));}
STATIC VOID W64(UINT8 *P,UINT64 V){W32(P,(UINT32)V);W32(P+4,(UINT32)(V>>32));}
UINT32 PianoNvJournalCrc(CONST VOID *Data,UINTN Bytes){CONST UINT8 *P=Data;UINT32 C=MAX_UINT32;for(UINTN I=0;I<Bytes;++I){C^=P[I];for(UINTN J=0;J<8;++J)C=C&1?(C>>1)^0xedb88320U:C>>1;}return ~C;}
STATIC BOOLEAN Blank(CONST UINT8 *P){BOOLEAN Zero=TRUE,Erased=TRUE;for(UINTN I=0;I<4096;++I){if(P[I])Zero=FALSE;if(P[I]!=0xff)Erased=FALSE;}return Zero||Erased;}
STATIC BOOLEAN CrcValid(UINT8 *P){UINT32 C=U32(P+24);W32(P+24,0);UINT32 Actual=PianoNvJournalCrc(P,4096);W32(P+24,C);return C==Actual;}
STATIC BOOLEAN Metadata(PIANO_NV_JOURNAL *S,UINT8 *P,UINT32 Slot,BOOLEAN Commit){
  if(CompareMem(P,Commit?FMagic:HMagic,16)||U32(P+16)!=1||U32(P+20)!=128||U32(P+28)!=S->Io.LayoutId||U32(P+32)!=Slot||
     U32(P+36)!=(Commit?2U:1U)||!U64(P+40)||U32(P+48)!=PIANO_NV_SNAPSHOT_BYTES||CompareMem(P+64,S->Io.VolumeUuid,16)||!CrcValid(P))return FALSE;
  for(UINTN I=80;I<4096;++I)if(P[I])return FALSE;
  if(U32(P+60)!=0||(!Commit&&U32(P+56)!=0))return FALSE;
  return TRUE;
}
STATIC EFI_STATUS Failed(PIANO_NV_JOURNAL *S,EFI_STATUS E,BOOLEAN Mutated){++S->Failures;S->LastStatus=Exact(E);S->Busy=FALSE;S->Ready=FALSE;if(Mutated){S->Dirty=S->Quarantined=TRUE;S->Ready=FALSE;}return S->LastStatus;}
EFI_STATUS PianoNvJournalBind(PIANO_NV_JOURNAL *S,CONST PIANO_UFS_PRODUCT_NV_IO *Io,UINT8 *Mirror,UINT8 *Scratch){
  if(!S||!Io||!Io->Context||!Io->Read||!Io->Write||!Io->Flush||Io->LayoutId!=1||!Mirror||!Scratch||Mirror==Scratch)return EFI_INVALID_PARAMETER;
  if(S->Signature)return EFI_ALREADY_STARTED;
  BOOLEAN Any=FALSE;for(UINTN I=0;I<16;++I)if(Io->VolumeUuid[I])Any=TRUE;if(!Any)return EFI_INVALID_PARAMETER;
  if((UINTN)Mirror>MAX_UINTN-PIANO_NV_SNAPSHOT_BYTES||(UINTN)Scratch>MAX_UINTN-PIANO_NV_SNAPSHOT_BYTES||
     ((UINTN)Mirror<(UINTN)Scratch+PIANO_NV_SNAPSHOT_BYTES&&(UINTN)Scratch<(UINTN)Mirror+PIANO_NV_SNAPSHOT_BYTES))return EFI_INVALID_PARAMETER;
  ZeroMem(S,sizeof(*S));S->Signature=SIG;S->Io=*Io;S->Mirror=Mirror;S->Scratch=Scratch;S->Opened=TRUE;S->ActiveSlot=MAX_UINT32;return EFI_SUCCESS;
}
EFI_STATUS PianoNvJournalRecover(PIANO_NV_JOURNAL *S){
  if(!S||S->Signature!=SIG||!S->Opened)return EFI_INVALID_PARAMETER;
  if(S->Runtime)return EFI_UNSUPPORTED;if(S->Busy||S->Quarantined||S->Dirty)return EFI_ACCESS_DENIED;
  S->Busy=TRUE;S->Ready=S->ScanComplete=FALSE;BOOLEAN Found=FALSE,Torn=FALSE;
  for(UINT32 Slot=0;Slot<2;++Slot){
    EFI_STATUS E=Exact(S->Io.Read(S->Io.Context,Slot,0,4096,S->Header));if(E!=EFI_SUCCESS)return Failed(S,E,FALSE);
    E=Exact(S->Io.Read(S->Io.Context,Slot,PIANO_NV_COMMIT_BLOCK,4096,S->Footer));if(E!=EFI_SUCCESS)return Failed(S,E,FALSE);
    if(Blank(S->Header)&&Blank(S->Footer))continue;
    if(!Metadata(S,S->Header,Slot,FALSE)||!Metadata(S,S->Footer,Slot,TRUE)||
       U64(S->Header+40)!=U64(S->Footer+40)||U32(S->Header+52)!=U32(S->Footer+52)||U32(S->Header+24)!=U32(S->Footer+56)){Torn=TRUE;continue;}
    for(UINT32 I=0;I<PIANO_NV_SNAPSHOT_BLOCKS;++I){E=Exact(S->Io.Read(S->Io.Context,Slot,I+1,4096,S->Scratch+I*4096));if(E!=EFI_SUCCESS)return Failed(S,E,FALSE);}
    if(PianoNvJournalCrc(S->Scratch,PIANO_NV_SNAPSHOT_BYTES)!=U32(S->Header+52)){Torn=TRUE;continue;}
    UINT64 Seq=U64(S->Header+40);
    if(Found&&Seq==S->Sequence&&CompareMem(S->Mirror,S->Scratch,PIANO_NV_SNAPSHOT_BYTES)){S->Quarantined=TRUE;return Failed(S,EFI_COMPROMISED_DATA,FALSE);}
    if(!Found||Seq>S->Sequence){CopyMem(S->Mirror,S->Scratch,PIANO_NV_SNAPSHOT_BYTES);S->Sequence=Seq;S->ActiveSlot=Slot;Found=TRUE;}
  }
  S->Busy=FALSE;S->Ready=Found;S->RecoveredTornSlot=Torn;S->ScanComplete=Found||!Torn;if(!Found&&Torn)S->Quarantined=TRUE;
  return S->LastStatus=Found?EFI_SUCCESS:Torn?EFI_VOLUME_CORRUPTED:EFI_NOT_FOUND;
}
STATIC VOID Make(PIANO_NV_JOURNAL *S,UINT8 *P,UINT32 Slot,UINT64 Seq,UINT32 PayloadCrc,BOOLEAN Commit,UINT32 HeaderCrc){
  ZeroMem(P,4096);CopyMem(P,Commit?FMagic:HMagic,16);W32(P+16,1);W32(P+20,128);W32(P+28,S->Io.LayoutId);W32(P+32,Slot);
  W32(P+36,Commit?2:1);W64(P+40,Seq);W32(P+48,PIANO_NV_SNAPSHOT_BYTES);W32(P+52,PayloadCrc);W32(P+56,HeaderCrc);CopyMem(P+64,S->Io.VolumeUuid,16);
  W32(P+24,PianoNvJournalCrc(P,4096));
}
STATIC EFI_STATUS Write(PIANO_NV_JOURNAL *S,UINT32 Slot,UINT32 Block,CONST VOID *Data){return Exact(S->Io.Write(S->Io.Context,Slot,Block,4096,Data));}
STATIC EFI_STATUS Barrier(PIANO_NV_JOURNAL *S){return Exact(S->Io.Flush(S->Io.Context));}
EFI_STATUS PianoNvJournalCommit(PIANO_NV_JOURNAL *S,CONST VOID *Candidate){
  if(!S||S->Signature!=SIG||!S->Opened||!Candidate)return EFI_INVALID_PARAMETER;
  if(S->Runtime)return EFI_UNSUPPORTED;if(S->Busy||S->Quarantined||S->Dirty)return EFI_ACCESS_DENIED;
  if(!S->ScanComplete)return EFI_NOT_READY;
  if(S->Sequence==MAX_UINT64)return EFI_OUT_OF_RESOURCES;
  UINT32 Slot=S->ActiveSlot==MAX_UINT32?0:S->ActiveSlot^1U;UINT64 Seq=S->Sequence+1;
  CopyMem(S->Scratch,Candidate,PIANO_NV_SNAPSHOT_BYTES);UINT32 Crc=PianoNvJournalCrc(S->Scratch,PIANO_NV_SNAPSHOT_BYTES);
  Make(S,S->Header,Slot,Seq,Crc,FALSE,0);Make(S,S->Footer,Slot,Seq,Crc,TRUE,U32(S->Header+24));
  S->Busy=S->Dirty=TRUE;ZeroMem(S->Block,4096);
  EFI_STATUS E=Write(S,Slot,PIANO_NV_COMMIT_BLOCK,S->Block);if(E!=EFI_SUCCESS)return Failed(S,E,TRUE);
  E=Barrier(S);if(E!=EFI_SUCCESS)return Failed(S,E,TRUE);
  E=Exact(S->Io.Read(S->Io.Context,Slot,PIANO_NV_COMMIT_BLOCK,4096,S->Block));if(E!=EFI_SUCCESS||!Blank(S->Block))return Failed(S,E==EFI_SUCCESS?EFI_CRC_ERROR:E,TRUE);
  E=Write(S,Slot,0,S->Header);if(E!=EFI_SUCCESS)return Failed(S,E,TRUE);
  for(UINT32 I=0;I<PIANO_NV_SNAPSHOT_BLOCKS;++I){E=Write(S,Slot,I+1,S->Scratch+I*4096);if(E!=EFI_SUCCESS)return Failed(S,E,TRUE);}
  E=Barrier(S);if(E!=EFI_SUCCESS)return Failed(S,E,TRUE);
  E=Exact(S->Io.Read(S->Io.Context,Slot,0,4096,S->Block));if(E!=EFI_SUCCESS||CompareMem(S->Block,S->Header,4096))return Failed(S,E==EFI_SUCCESS?EFI_CRC_ERROR:E,TRUE);
  for(UINT32 I=0;I<PIANO_NV_SNAPSHOT_BLOCKS;++I){E=Exact(S->Io.Read(S->Io.Context,Slot,I+1,4096,S->Block));if(E!=EFI_SUCCESS||CompareMem(S->Block,S->Scratch+I*4096,4096))return Failed(S,E==EFI_SUCCESS?EFI_CRC_ERROR:E,TRUE);}
  E=Write(S,Slot,PIANO_NV_COMMIT_BLOCK,S->Footer);if(E!=EFI_SUCCESS)return Failed(S,E,TRUE);
  E=Barrier(S);if(E!=EFI_SUCCESS)return Failed(S,E,TRUE);
  E=Exact(S->Io.Read(S->Io.Context,Slot,PIANO_NV_COMMIT_BLOCK,4096,S->Block));if(E!=EFI_SUCCESS||CompareMem(S->Block,S->Footer,4096))return Failed(S,E==EFI_SUCCESS?EFI_CRC_ERROR:E,TRUE);
  CopyMem(S->Mirror,S->Scratch,PIANO_NV_SNAPSHOT_BYTES);S->Sequence=Seq;S->ActiveSlot=Slot;++S->Commits;
  S->Dirty=S->Busy=FALSE;S->Ready=TRUE;return S->LastStatus=EFI_SUCCESS;
}
EFI_STATUS PianoNvJournalFlush(PIANO_NV_JOURNAL *S){if(!S||S->Signature!=SIG)return EFI_INVALID_PARAMETER;if(S->Runtime)return EFI_UNSUPPORTED;if(S->Busy||S->Dirty||S->Quarantined)return EFI_ACCESS_DENIED;return S->LastStatus=Exact(S->Io.Flush(S->Io.Context));}
VOID PianoNvJournalFenceRuntime(PIANO_NV_JOURNAL *S){if(S&&S->Signature==SIG)S->Runtime=TRUE;}
