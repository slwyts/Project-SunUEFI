// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fixed superfloppy window only. No GPT creation and no parent LU WriteBlocks.
#include "PianoUfsBoundedBlock.h"
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#define WINDOW_SIGNATURE SIGNATURE_32('P','W','I','N')
STATIC PIANO_UFS_BOUNDED_BLOCK *Device(EFI_BLOCK_IO_PROTOCOL *This){
  if(This==NULL)return NULL;PIANO_UFS_BOUNDED_BLOCK *D=BASE_CR(This,PIANO_UFS_BOUNDED_BLOCK,Block);
  return D->Signature==WINDOW_SIGNATURE && This->Media==&D->Media?D:NULL;
}
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC VOID Fail(PIANO_UFS_BOUNDED_BLOCK *D,EFI_STATUS S){
  D->State.Quarantined=D->State.NeedsRecovery=TRUE;D->State.RestoreVerified=FALSE;D->Media.ReadOnly=TRUE;
  D->State.LastStatus=S;++D->State.Failures;
}
STATIC EFI_STATUS Quiet(PIANO_UFS_BOUNDED_BLOCK *D,CONST PIANO_UFS_WINDOW_IO *T){
  BOOLEAN Quiet=FALSE;EFI_STATUS S=Exact(T->Io.Quiesced(T->Io.Context,&Quiet));if(S==EFI_SUCCESS && Quiet!=TRUE)S=EFI_NOT_READY;
  D->State.LastQuietStatus=S;return S;
}
STATIC EFI_STATUS ReadExact(PIANO_UFS_BOUNDED_BLOCK *D,CONST PIANO_UFS_WINDOW_IO *T,EFI_LBA Lba,UINTN Bytes,VOID *Buffer){
  SetMem(Buffer,Bytes,0xCC);UINTN Got=0;EFI_STATUS S=Exact(T->Io.Read(T->Io.Context,4,Lba,Bytes,Buffer,&Got));
  if(S==EFI_SUCCESS && Got!=Bytes)S=EFI_BAD_BUFFER_SIZE;
  EFI_STATUS Q=Quiet(D,T);return Q!=EFI_SUCCESS?Q:S;
}
STATIC EFI_STATUS Gate(PIANO_UFS_BOUNDED_BLOCK *D,CONST PIANO_UFS_WINDOW_IO *T){
  EFI_STATUS S=PianoUfsWriteTestCheckBaseline(&D->Baseline);if(EFI_ERROR(S))return S;
  ZeroMem(&D->LastGuard,sizeof(D->LastGuard));
  S=Exact(T->Io.ReadGuard(T->Io.Context,4,&D->LastGuard));if(S==EFI_SUCCESS)S=PianoUfsWriteTestCheckGuard(&D->LastGuard);
  EFI_STATUS Q=Quiet(D,T);if(Q!=EFI_SUCCESS)return Q;if(S!=EFI_SUCCESS)return S;
  S=ReadExact(D,T,1,4096,D->Primary);if(S!=EFI_SUCCESS)return S;
  S=ReadExact(D,T,2,12288,D->Entries);if(S!=EFI_SUCCESS)return S;
  S=ReadExact(D,T,378879,4096,D->Backup);if(S!=EFI_SUCCESS)return S;
  S=ReadExact(D,T,378873,12288,D->BackupEntries);if(S!=EFI_SUCCESS)return S;
  PIANO_UFS_WRITE_BLOB P={D->Primary,4096},PE={D->Entries,12288},B={D->Backup,4096},BE={D->BackupEntries,12288};
  return PianoUfsWriteTestCheckLiveGpt(&D->Baseline,&P,&PE,&B,&BE);
}
STATIC EFI_STATUS WholeOriginal(PIANO_UFS_BOUNDED_BLOCK *D,CONST PIANO_UFS_WINDOW_IO *T){
  for(EFI_LBA Lba=PIANO_UFS_WINDOW_FIRST;Lba<=PIANO_UFS_WINDOW_LAST;++Lba){
    EFI_STATUS S=ReadExact(D,T,Lba,4096,D->Rx);if(S!=EFI_SUCCESS)return S;
    // The pinned complete PC gap is all-zero, with validated full-gap SHA.
    // Reading and matching each complete block proves that same original gap.
    if(CompareMem(D->Rx,D->Baseline.OriginalBlock.Data,4096))return EFI_CRC_ERROR;
  }
  return Gate(D,T); // Reject GPT/WP drift across the long read-only proof.
}
STATIC BOOLEAN ValidIo(CONST PIANO_UFS_WINDOW_IO *T){return T!=NULL && T->Acquire!=NULL && T->Release!=NULL && T->Io.Read!=NULL && T->Io.WriteFua!=NULL && T->Io.Sync!=NULL && T->Io.ReadGuard!=NULL && T->Io.Quiesced!=NULL;}
STATIC EFI_STATUS Enter(PIANO_UFS_BOUNDED_BLOCK *D,CONST PIANO_UFS_WINDOW_IO *T){
  if(D->State.Busy)return EFI_NOT_READY;D->State.Busy=TRUE;EFI_STATUS S=Exact(T->Acquire(T->Io.Context));
  if(S!=EFI_SUCCESS)D->State.Busy=FALSE;return S;
}
STATIC EFI_STATUS Leave(PIANO_UFS_BOUNDED_BLOCK *D,CONST PIANO_UFS_WINDOW_IO *T){D->State.Busy=FALSE;EFI_STATUS S=Exact(T->Release(T->Io.Context,D->State.Quarantined));if(S!=EFI_SUCCESS)Fail(D,S);return S;}
STATIC EFI_STATUS Request(PIANO_UFS_BOUNDED_BLOCK *D,UINT32 Id,EFI_LBA Lba,UINTN Bytes,VOID *Buffer){
  if(!D->Media.MediaPresent || D->State.Closed)return EFI_NO_MEDIA;
  if(Id!=D->Media.MediaId)return EFI_MEDIA_CHANGED;
  if(D->State.Quarantined)return EFI_DEVICE_ERROR;
  if(Bytes==0)return EFI_SUCCESS;
  if(Buffer==NULL || (UINTN)Buffer>MAX_UINTN-Bytes || ((UINTN)Buffer<(UINTN)D+sizeof(*D) && (UINTN)D<(UINTN)Buffer+Bytes))return EFI_INVALID_PARAMETER;if(Bytes%4096)return EFI_BAD_BUFFER_SIZE;
  UINTN Blocks=Bytes/4096;if(Lba>=PIANO_UFS_WINDOW_BLOCKS || Blocks>PIANO_UFS_WINDOW_BLOCKS-Lba)return EFI_INVALID_PARAMETER;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Reset(EFI_BLOCK_IO_PROTOCOL *This,BOOLEAN Extended){
  PIANO_UFS_BOUNDED_BLOCK *D=Device(This);if(D==NULL)return EFI_INVALID_PARAMETER;
  // Reset never clears dirty/quarantine/ledger or touches the controller.
  if(!D->Media.MediaPresent)return EFI_NO_MEDIA;return D->State.Quarantined?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Read(EFI_BLOCK_IO_PROTOCOL *This,UINT32 Id,EFI_LBA Lba,UINTN Bytes,VOID *Buffer){
  PIANO_UFS_BOUNDED_BLOCK *D=Device(This);if(D==NULL)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Request(D,Id,Lba,Bytes,Buffer);if(S!=EFI_SUCCESS || Bytes==0)return S;
  S=Enter(D,&D->Transport);if(S!=EFI_SUCCESS)return S;++D->State.Requests;
  for(UINTN O=0;O<Bytes;O+=4096){S=ReadExact(D,&D->Transport,PIANO_UFS_WINDOW_FIRST+Lba+O/4096,4096,D->Rx);if(S!=EFI_SUCCESS)break;CopyMem((UINT8 *)Buffer+O,D->Rx,4096);++D->State.ReadBlocks;}
  if(S!=EFI_SUCCESS)Fail(D,S);else D->State.LastStatus=S;EFI_STATUS Finish=Leave(D,&D->Transport);return S==EFI_SUCCESS?Finish:S;
}
STATIC EFI_STATUS EFIAPI Write(EFI_BLOCK_IO_PROTOCOL *This,UINT32 Id,EFI_LBA Lba,UINTN Bytes,VOID *Buffer){
  PIANO_UFS_BOUNDED_BLOCK *D=Device(This);if(D==NULL)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Request(D,Id,Lba,Bytes,Buffer);if(S!=EFI_SUCCESS || Bytes==0)return S;
  if(D->Media.ReadOnly)return EFI_WRITE_PROTECTED;
  S=Enter(D,&D->Transport);if(S!=EFI_SUCCESS)return S;++D->State.Requests;++D->State.WriteRequests;
  D->State.LastRequestBlocks=Bytes/4096;D->State.LastCompletedBlocks=0;
  for(UINTN O=0;O<Bytes;O+=4096){
    S=Gate(D,&D->Transport);if(S!=EFI_SUCCESS)break;
    D->State.LastLogical=Lba+O/4096;D->State.LastPhysical=PIANO_UFS_WINDOW_FIRST+D->State.LastLogical;
    D->State.LastAttempted=TRUE;D->State.LastReturned=FALSE;D->State.LastWriteStatus=D->State.LastSyncStatus=D->State.LastReadStatus=EFI_NOT_STARTED;
    D->State.Dirty=D->State.NeedsRecovery=TRUE;D->State.RestoreVerified=FALSE;++D->State.WriteAttempts;
    CopyMem(D->Tx,(UINT8 *)Buffer+O,4096);UINTN Done=0;S=Exact(D->Transport.Io.WriteFua(D->Transport.Io.Context,4,D->State.LastPhysical,4096,D->Tx,&Done));D->State.LastReturned=TRUE;
    if(S==EFI_SUCCESS && Done!=4096)S=EFI_BAD_BUFFER_SIZE;D->State.LastWriteStatus=S;
    EFI_STATUS Q=Quiet(D,&D->Transport);if(Q!=EFI_SUCCESS)S=Q;if(S!=EFI_SUCCESS)break;
    S=Exact(D->Transport.Io.Sync(D->Transport.Io.Context,4,PIANO_UFS_WINDOW_FIRST,PIANO_UFS_WINDOW_BYTES));D->State.LastSyncStatus=S;
    Q=Quiet(D,&D->Transport);if(Q!=EFI_SUCCESS)S=Q;if(S!=EFI_SUCCESS)break;
    S=ReadExact(D,&D->Transport,D->State.LastPhysical,4096,D->Rx);
    if(S==EFI_SUCCESS && CompareMem(D->Rx,D->Tx,4096))S=EFI_CRC_ERROR;D->State.LastReadStatus=S;if(S!=EFI_SUCCESS)break;
    ++D->State.LastCompletedBlocks;++D->State.VerifiedBlocks;
  }
  if(S!=EFI_SUCCESS)Fail(D,S);else D->State.LastStatus=S;EFI_STATUS Finish=Leave(D,&D->Transport);return S==EFI_SUCCESS?Finish:S;
}
STATIC EFI_STATUS EFIAPI Flush(EFI_BLOCK_IO_PROTOCOL *This){
  PIANO_UFS_BOUNDED_BLOCK *D=Device(This);if(D==NULL)return EFI_INVALID_PARAMETER;
  if(!D->Media.MediaPresent)return EFI_NO_MEDIA;if(D->State.Quarantined)return EFI_DEVICE_ERROR;
  EFI_STATUS S=Enter(D,&D->Transport);if(S!=EFI_SUCCESS)return S;++D->State.Flushes;
  S=Gate(D,&D->Transport);if(S==EFI_SUCCESS){S=Exact(D->Transport.Io.Sync(D->Transport.Io.Context,4,PIANO_UFS_WINDOW_FIRST,PIANO_UFS_WINDOW_BYTES));EFI_STATUS Q=Quiet(D,&D->Transport);if(Q!=EFI_SUCCESS)S=Q;}
  if(S!=EFI_SUCCESS)Fail(D,S);else D->State.LastStatus=S;EFI_STATUS Finish=Leave(D,&D->Transport);return S==EFI_SUCCESS?Finish:S;
}
EFI_STATUS PianoUfsBoundedBlockOpen(PIANO_UFS_BOUNDED_BLOCK *D,BOOLEAN Enabled,CONST PIANO_UFS_WRITE_BASELINE *B,CONST PIANO_UFS_WINDOW_IO *T){
  if(D==NULL || B==NULL || !ValidIo(T))return EFI_INVALID_PARAMETER;if(!Enabled)return EFI_ACCESS_DENIED;
  if(D->Signature!=0)return EFI_ACCESS_DENIED;
  EFI_STATUS S=PianoUfsWriteTestCheckBaseline(B);if(S!=EFI_SUCCESS)return S;
  ZeroMem(D,sizeof(*D));D->Signature=WINDOW_SIGNATURE;D->Baseline=*B;D->Transport=*T;
  D->State.DisconnectStatus=D->State.LastStatus=D->State.LastWriteStatus=D->State.LastSyncStatus=D->State.LastReadStatus=D->State.LastQuietStatus=EFI_NOT_STARTED;
  D->Media=(EFI_BLOCK_IO_MEDIA){.MediaId=1,.MediaPresent=FALSE,.ReadOnly=TRUE,.BlockSize=4096,.IoAlign=1,.LastBlock=3583,.LogicalBlocksPerPhysicalBlock=1};
  D->Block=(EFI_BLOCK_IO_PROTOCOL){.Revision=EFI_BLOCK_IO_PROTOCOL_REVISION3,.Media=&D->Media,.Reset=Reset,.ReadBlocks=Read,.WriteBlocks=Write,.FlushBlocks=Flush};
  S=Enter(D,T);if(S!=EFI_SUCCESS)return S;S=Gate(D,T);if(S==EFI_SUCCESS)S=WholeOriginal(D,T);
  if(S!=EFI_SUCCESS)Fail(D,S);else{D->State.Opened=TRUE;D->Media.MediaPresent=TRUE;D->Media.ReadOnly=FALSE;D->State.LastStatus=EFI_SUCCESS;}
  EFI_STATUS Finish=Leave(D,T);return S==EFI_SUCCESS?Finish:S;
}
EFI_STATUS PianoUfsBoundedBlockCloseForRecovery(PIANO_UFS_BOUNDED_BLOCK *D){
  if(D==NULL || D->Signature!=WINDOW_SIGNATURE)return EFI_INVALID_PARAMETER;if(D->State.Busy)return EFI_NOT_READY;
  D->State.Closed=TRUE;D->Media.MediaPresent=FALSE;D->Media.ReadOnly=TRUE;return EFI_SUCCESS;
}
EFI_STATUS PianoUfsBoundedBlockVerifyRestored(PIANO_UFS_BOUNDED_BLOCK *D,CONST PIANO_UFS_WINDOW_IO *T){
  if(D==NULL || D->Signature!=WINDOW_SIGNATURE || !ValidIo(T))return EFI_INVALID_PARAMETER;
  if(!D->State.Closed)return EFI_ACCESS_DENIED;EFI_STATUS S=Enter(D,T);if(S!=EFI_SUCCESS)return S;
  S=Gate(D,T);if(S==EFI_SUCCESS)S=WholeOriginal(D,T);
  if(S!=EFI_SUCCESS)Fail(D,S);else{D->State.Dirty=D->State.NeedsRecovery=D->State.Quarantined=FALSE;D->State.RestoreVerified=TRUE;D->State.LastStatus=EFI_SUCCESS;}
  EFI_STATUS Finish=Leave(D,T);return S==EFI_SUCCESS?Finish:S;
}
VOID PianoUfsBoundedBlockReport(CONST PIANO_UFS_BOUNDED_BLOCK *D){
  if(D==NULL || D->Signature!=WINDOW_SIGNATURE){DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WINDOW_REPORT initialized=0\n"));return;}
  CONST PIANO_UFS_WINDOW_STATE *S=&D->State;
  CONST PIANO_UFS_WRITE_GUARD *G=&D->LastGuard;
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WINDOW_GUARD collected=%02x capacity=%lu capacity_status=%r mode_status=%r unit_status=%r permanent_status=%r power_status=%r fua=%u mode_wp=%u unit_wp=%u permanent=%u power_on=%u\n",G->Collected,G->CapacityBytes,G->CapacityStatus,G->ModeSenseStatus,G->UnitStatus,G->PermanentFlagStatus,G->PowerOnFlagStatus,G->Fua,G->ModeWriteProtected,G->UnitWriteProtect,G->PermanentEnabled,G->PowerOnEnabled));
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WINDOW_CONSUMERS detached=%u disconnect_status=%r public_closed=%u\n",S->ConsumersDetached,S->DisconnectStatus,S->Closed));
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WINDOW_REPORT opened=%u closed=%u busy=%u dirty=%u needs_recovery=%u quarantine=%u restore_verified=%u requests=%lu write_requests=%lu attempts=%lu verified_blocks=%lu flushes=%lu failures=%lu reads=%lu\n",S->Opened,S->Closed,S->Busy,S->Dirty,S->NeedsRecovery,S->Quarantined,S->RestoreVerified,S->Requests,S->WriteRequests,S->WriteAttempts,S->VerifiedBlocks,S->Flushes,S->Failures,S->ReadBlocks));
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WINDOW_LAST logical=%lu physical=%lu request_blocks=%lu completed_blocks=%lu attempted=%u returned=%u status=%r write=%r sync=%r read=%r quiet=%r\n",S->LastLogical,S->LastPhysical,(UINT64)S->LastRequestBlocks,(UINT64)S->LastCompletedBlocks,S->LastAttempted,S->LastReturned,S->LastStatus,S->LastWriteStatus,S->LastSyncStatus,S->LastReadStatus,S->LastQuietStatus));
}
