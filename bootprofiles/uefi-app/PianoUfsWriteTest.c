// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure controlled transaction. No registered protocol and no real transport.
#include "PianoUfsWriteTest.h"
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include "PianoGpt.h"

STATIC CONST CHAR8 *mPrimaryHash="22c6c0349cd190bdc6f5e13ac3cba0f337f58729144d90b4281d90530184e565";
STATIC CONST CHAR8 *mBackupHash="b76a916e296fefa2048c19f38f2ae86da4f01b61f46bac739eb65baf75a1ac8f";
STATIC CONST CHAR8 *mArrayHash="fd10bb4f7142eccb28a6acc5fc6e928c599883fb44a0338b521473c5669a3506";
STATIC CONST CHAR8 *mOriginalHash="ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7";
STATIC CONST CHAR8 *mGapHash="e86bae8c0598c4ff83c695f467daa4a1e8fa01d57f9140372993366204022a4d";
#define LAST_LBA (PIANO_UFS_WRITE_TEST_CAPACITY/4096-1)
#define BACKUP_ARRAY_LBA 378873ULL
#define WORK_SIGNATURE SIGNATURE_32('P','W','T','X')
STATIC UINT32 Le32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
STATIC UINT64 Le64(CONST UINT8 *P){return Le32(P)|((UINT64)Le32(P+4)<<32);}
STATIC VOID PutLe32(UINT8 *P,UINT32 V){for(UINTN I=0;I<4;++I)P[I]=(UINT8)(V>>(8*I));}
STATIC VOID PutBe32(UINT8 *P,UINT32 V){for(UINTN I=0;I<4;++I)P[3-I]=(UINT8)(V>>(8*I));}
STATIC UINT8 Hex(CHAR8 C){return C<='9'?(UINT8)(C-'0'):(UINT8)(C-'a'+10);}
STATIC BOOLEAN DigestMatches(CONST UINT8 *Digest,CONST CHAR8 *Expected) {
  for(UINTN I=0;I<32;++I)if(Digest[I]!=(UINT8)((Hex(Expected[I*2])<<4)|Hex(Expected[I*2+1])))return FALSE;
  return TRUE;
}
STATIC BOOLEAN HashMatches(CONST VOID *Data,UINTN Bytes,CONST CHAR8 *Expected) {
  UINT8 Digest[32];return Sha256HashAll(Data,Bytes,Digest) && DigestMatches(Digest,Expected);
}
STATIC BOOLEAN AllZero(CONST VOID *Data,UINTN Bytes){CONST UINT8 *P=Data;for(UINTN I=0;I<Bytes;++I)if(P[I])return FALSE;return TRUE;}
STATIC BOOLEAN ExactTarget(UINT8 Lun,EFI_LBA Lba,UINTN Bytes){return Lun==4 && Lba==375040 && Bytes==4096;}
STATIC BOOLEAN Overlap(CONST VOID *A,UINTN ABytes,CONST VOID *B,UINTN BBytes) {
  UINTN X=(UINTN)A,Y=(UINTN)B;
  if(X>MAX_UINTN-ABytes || Y>MAX_UINTN-BBytes)return TRUE;
  return X<Y+BBytes && Y<X+ABytes;
}
STATIC EFI_STATUS Normalize(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC VOID InitResult(PIANO_UFS_WRITE_RESULT *R) {
  ZeroMem(R,sizeof(*R));R->Outcome=PianoWriteRefused;
  R->GateStatus=R->FirstFailure=R->FinalStatus=EFI_NOT_STARTED;
  for(UINTN I=0;I<PianoWriteStepCount;++I)R->Step[I].Status=R->Step[I].QuietStatus=EFI_NOT_STARTED;
  R->Write[0].Status=R->Write[1].Status=EFI_NOT_STARTED;
}
STATIC EFI_STATUS BaselineGate(CONST PIANO_UFS_WRITE_REQUEST *Q,BOOLEAN WriteAuthorized) {
  CONST PIANO_UFS_WRITE_BASELINE *B=&Q->Baseline;
  if(!ExactTarget(Q->Lun,Q->Lba,Q->Bytes) || (WriteAuthorized && Q->Authorized!=TRUE) || B->ExternalArchiveVerified!=TRUE)return EFI_ACCESS_DENIED;
  if(B->ExternalGapBytes!=14680064 || !DigestMatches(B->ExternalGapSha256,mGapHash))return EFI_SECURITY_VIOLATION;
  CONST PIANO_UFS_WRITE_BLOB *Blobs[]={&B->PrimaryHeader,&B->PrimaryEntries,&B->BackupHeader,&B->BackupEntries,&B->OriginalBlock};
  STATIC CONST UINTN Sizes[]={4096,12288,4096,12288,4096};
  CONST CHAR8 *Hashes[]={mPrimaryHash,mArrayHash,mBackupHash,mArrayHash,mOriginalHash};
  for(UINTN I=0;I<ARRAY_SIZE(Blobs);++I)
    if(Blobs[I]->Data==NULL || Blobs[I]->Bytes!=Sizes[I] || !HashMatches(Blobs[I]->Data,Blobs[I]->Bytes,Hashes[I]))return EFI_SECURITY_VIOLATION;
  return AllZero(B->OriginalBlock.Data,4096)?EFI_SUCCESS:EFI_SECURITY_VIOLATION;
}
STATIC EFI_STATUS GuardGate(CONST PIANO_UFS_WRITE_GUARD *G) {
  if(G->Lun!=4 || G->Collected!=0x1F || G->CapacityBytes!=PIANO_UFS_WRITE_TEST_CAPACITY ||
     G->CapacityStatus!=EFI_SUCCESS || G->ModeSenseStatus!=EFI_SUCCESS || G->UnitStatus!=EFI_SUCCESS ||
     G->PermanentFlagStatus!=EFI_SUCCESS || G->PowerOnFlagStatus!=EFI_SUCCESS)return EFI_NOT_READY;
  if(G->Fua!=TRUE || G->ModeWriteProtected!=FALSE || G->UnitWriteProtect>2 ||
     G->PermanentEnabled!=FALSE || G->PowerOnEnabled!=FALSE)return EFI_WRITE_PROTECTED;
  return EFI_SUCCESS;
}
STATIC VOID Quarantine(PIANO_UFS_WRITE_RESULT *R,EFI_STATUS S) {
  R->Quarantined=R->RequiresRecovery=TRUE;R->SafeToContinue=R->DataUnchanged=FALSE;
  R->Outcome=PianoWriteQuarantined;R->FinalStatus=S;
}
STATIC EFI_STATUS Quiet(CONST PIANO_UFS_WRITE_IO *Io,PIANO_UFS_WRITE_RESULT *R,PIANO_UFS_WRITE_STEP *Step) {
  BOOLEAN Confirmed=FALSE;Step->QuietAttempted=TRUE;
  EFI_STATUS S=Normalize(Io->Quiesced(Io->Context,&Confirmed));
  if(S==EFI_SUCCESS && Confirmed!=TRUE)S=EFI_NOT_READY;
  Step->QuietStatus=S;Step->Quiet=S==EFI_SUCCESS;
  if(S!=EFI_SUCCESS)Quarantine(R,S);return S;
}
STATIC EFI_STATUS Read(CONST PIANO_UFS_WRITE_IO *Io,PIANO_UFS_WRITE_RESULT *R,PIANO_UFS_WRITE_STEP_ID Id,EFI_LBA Lba,UINTN Bytes,VOID *Buffer) {
  PIANO_UFS_WRITE_STEP *Step=&R->Step[Id];Step->Attempted=TRUE;++Step->Calls;
  // Poison RX so a callback that returns a success/length without refreshing
  // the independent buffer cannot accidentally pass a byte comparison.
  SetMem(Buffer,Bytes,0xCC);UINTN Got=0;
  EFI_STATUS S=Normalize(Io->Read(Io->Context,4,Lba,Bytes,Buffer,&Got));Step->Transferred+=Got;
  if(S==EFI_SUCCESS && Got!=Bytes)S=EFI_BAD_BUFFER_SIZE;
  Step->Status=S;if(Quiet(Io,R,Step)!=EFI_SUCCESS)return Step->QuietStatus;return S;
}
STATIC EFI_STATUS Guard(CONST PIANO_UFS_WRITE_IO *Io,PIANO_UFS_WRITE_RESULT *R,PIANO_UFS_WRITE_STEP_ID Id) {
  PIANO_UFS_WRITE_GUARD G;ZeroMem(&G,sizeof(G));PIANO_UFS_WRITE_STEP *Step=&R->Step[Id];Step->Attempted=TRUE;++Step->Calls;
  EFI_STATUS S=Normalize(Io->ReadGuard(Io->Context,4,&G));if(S==EFI_SUCCESS)S=GuardGate(&G);
  Step->Status=S;if(Quiet(Io,R,Step)!=EFI_SUCCESS)return Step->QuietStatus;return S;
}
EFI_STATUS PianoUfsWriteTestCheckBaseline(CONST PIANO_UFS_WRITE_BASELINE *Baseline) {
  if(Baseline==NULL)return EFI_INVALID_PARAMETER;
  PIANO_UFS_WRITE_REQUEST Q={.Lun=4,.Lba=375040,.Bytes=4096,.Baseline=*Baseline};return BaselineGate(&Q,FALSE);
}
EFI_STATUS PianoUfsWriteTestCheckGuard(CONST PIANO_UFS_WRITE_GUARD *G){return G==NULL?EFI_INVALID_PARAMETER:GuardGate(G);}
EFI_STATUS PianoUfsWriteTestCheckLiveGpt(CONST PIANO_UFS_WRITE_BASELINE *Baseline,
  CONST PIANO_UFS_WRITE_BLOB *Primary,CONST PIANO_UFS_WRITE_BLOB *PrimaryEntries,
  CONST PIANO_UFS_WRITE_BLOB *Backup,CONST PIANO_UFS_WRITE_BLOB *BackupEntries) {
  if(Baseline==NULL || Primary==NULL || PrimaryEntries==NULL || Backup==NULL || BackupEntries==NULL)return EFI_INVALID_PARAMETER;
  EFI_STATUS Checked=PianoUfsWriteTestCheckBaseline(Baseline);if(EFI_ERROR(Checked))return Checked;
  CONST PIANO_UFS_WRITE_BLOB *Data[]={Primary,PrimaryEntries,Backup,BackupEntries};
  CONST PIANO_UFS_WRITE_BLOB *Expected[]={&Baseline->PrimaryHeader,&Baseline->PrimaryEntries,&Baseline->BackupHeader,&Baseline->BackupEntries};
  for(UINTN I=0;I<4;++I)if(Data[I]->Data==NULL || Data[I]->Bytes!=Expected[I]->Bytes || CompareMem(Data[I]->Data,Expected[I]->Data,Data[I]->Bytes))return EFI_SECURITY_VIOLATION;
  CONST UINT8 *P=Primary->Data,*B=Backup->Data;
  PIANO_GPT_HEADER H;EFI_STATUS S=PianoGptParseHeader(P,4096,LAST_LBA,4096,&H);if(EFI_ERROR(S))return S;
  UINT8 BackupClean[4096];CopyMem(BackupClean,B,4096);UINT32 HeaderBytes=Le32(B+12),Crc=Le32(B+16);
  if(CompareMem(B,"EFI PART",8) || HeaderBytes!=92 || Le64(B+24)!=LAST_LBA || Le64(B+32)!=1 ||
     Le32(B+8)!=0x10000 || Le32(B+20)!=0 || Le64(B+40)!=H.FirstUsable || Le64(B+48)!=H.LastUsable ||
     CompareMem(P+56,B+56,16) || Le64(B+72)!=BACKUP_ARRAY_LBA || Le32(B+80)!=H.Entries || Le32(B+84)!=H.EntryBytes || Le32(B+88)!=H.ArrayCrc)return EFI_COMPROMISED_DATA;
  PutLe32(BackupClean+16,0);
  if(PianoGptCrc32(BackupClean,HeaderBytes)!=Crc || H.EntryLba!=2 || H.Entries!=96 || H.EntryBytes!=128 || H.ArrayBytes!=12288 ||
     H.FirstUsable>375040 || H.LastUsable<378623 || H.LastUsable>=BACKUP_ARRAY_LBA || BACKUP_ARRAY_LBA+3>LAST_LBA)return EFI_COMPROMISED_DATA;
  UINTN Active=0;S=PianoGptCheckEntries(PrimaryEntries->Data,12288,&H,&Active);if(EFI_ERROR(S) || Active!=79)return EFI_COMPROMISED_DATA;
  if(CompareMem(PrimaryEntries->Data,BackupEntries->Data,12288) || PianoGptCrc32(BackupEntries->Data,12288)!=H.ArrayCrc)return EFI_CRC_ERROR;
  for(UINTN I=0;I<96;++I) {
    CONST UINT8 *A=(CONST UINT8 *)PrimaryEntries->Data+I*128;if(AllZero(A,16))continue;
    UINT64 Start=Le64(A+32),End=Le64(A+40);
    if(Start<=378623 && End>=375040)return EFI_ACCESS_DENIED;
    for(UINTN J=I+1;J<96;++J) {
      CONST UINT8 *Other=(CONST UINT8 *)PrimaryEntries->Data+J*128;if(AllZero(Other,16))continue;
      if(Start<=Le64(Other+40) && End>=Le64(Other+32))return EFI_COMPROMISED_DATA;
    }
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS GptGate(CONST PIANO_UFS_WRITE_REQUEST *Q,CONST PIANO_UFS_WRITE_IO *Io,PIANO_UFS_WRITE_WORK *W,PIANO_UFS_WRITE_RESULT *R,BOOLEAN Restore) {
  PIANO_UFS_WRITE_STEP_ID First=Restore?PianoWriteRestorePrimaryHeader:PianoWritePrimaryHeader;
  VOID *Buffers[]={W->PrimaryHeader,W->PrimaryEntries,W->BackupHeader,W->BackupEntries};
  CONST PIANO_UFS_WRITE_BLOB *Baseline[]={&Q->Baseline.PrimaryHeader,&Q->Baseline.PrimaryEntries,&Q->Baseline.BackupHeader,&Q->Baseline.BackupEntries};
  STATIC CONST EFI_LBA Lbas[]={1,2,LAST_LBA,BACKUP_ARRAY_LBA};
  STATIC CONST UINTN Sizes[]={4096,12288,4096,12288};
  for(UINTN I=0;I<4;++I) {
    EFI_STATUS S=Read(Io,R,(PIANO_UFS_WRITE_STEP_ID)(First+I),Lbas[I],Sizes[I],Buffers[I]);if(S!=EFI_SUCCESS)return S;
    if(CompareMem(Buffers[I],Baseline[I]->Data,Sizes[I]))return EFI_SECURITY_VIOLATION;
  }
  PIANO_UFS_WRITE_BLOB P={W->PrimaryHeader,4096},PE={W->PrimaryEntries,12288},B={W->BackupHeader,4096},BE={W->BackupEntries,12288};
  return PianoUfsWriteTestCheckLiveGpt(&Q->Baseline,&P,&PE,&B,&BE);
}

STATIC EFI_STATUS Write(CONST PIANO_UFS_WRITE_IO *Io,PIANO_UFS_WRITE_RESULT *R,BOOLEAN Restore,CONST VOID *Data) {
  PIANO_UFS_WRITE_STEP *Step=&R->Step[Restore?PianoWriteRestoreWrite:PianoWriteTestWrite];
  PIANO_UFS_WRITE_ATTEMPT *A=&R->Write[Restore?1:0];
  // Ledger is visible before entering a callback that may time out/not return.
  A->Attempted=TRUE;A->Restore=Restore;A->Lun=4;A->Lba=375040;A->Bytes=4096;++R->WriteAttempts;
  R->RequiresRecovery=TRUE;Step->Attempted=TRUE;++Step->Calls;
  UINTN Got=0;EFI_STATUS S=Normalize(Io->WriteFua(Io->Context,4,375040,4096,Data,&Got));
  A->Returned=TRUE;A->Transferred=Got;Step->Transferred=Got;
  if(S==EFI_SUCCESS && Got!=4096)S=EFI_BAD_BUFFER_SIZE;
  A->Status=Step->Status=S;
  if(Quiet(Io,R,Step)!=EFI_SUCCESS)return Step->QuietStatus;return S;
}
STATIC EFI_STATUS Sync(CONST PIANO_UFS_WRITE_IO *Io,PIANO_UFS_WRITE_RESULT *R,BOOLEAN Restore) {
  PIANO_UFS_WRITE_STEP *Step=&R->Step[Restore?PianoWriteRestoreSync:PianoWriteTestSync];Step->Attempted=TRUE;++Step->Calls;
  Step->Status=Normalize(Io->Sync(Io->Context,4,375040,4096));
  if(Quiet(Io,R,Step)!=EFI_SUCCESS)return Step->QuietStatus;return Step->Status;
}
STATIC EFI_STATUS Execute(CONST PIANO_UFS_WRITE_REQUEST *Q,CONST PIANO_UFS_WRITE_IO *Io,PIANO_UFS_WRITE_WORK *W,PIANO_UFS_WRITE_RESULT *R,BOOLEAN Preflight) {
  if(Q==NULL || Io==NULL || W==NULL || R==NULL)return EFI_INVALID_PARAMETER;
  if(W->StateSignature!=0 && W->StateSignature!=WORK_SIGNATURE)return EFI_INVALID_PARAMETER;
  if(W->StateSignature==WORK_SIGNATURE && (W->Running || W->NeedsRecovery))return EFI_ACCESS_DENIED;
  if(Overlap(W,sizeof(*W),R,sizeof(*R)) || Overlap(W,sizeof(*W),Q,sizeof(*Q)) || Overlap(R,sizeof(*R),Q,sizeof(*Q)) ||
     Overlap(W,sizeof(*W),Io,sizeof(*Io)) || Overlap(R,sizeof(*R),Io,sizeof(*Io)))return EFI_INVALID_PARAMETER;
  InitResult(R);
  if(Io->Read==NULL || (!Preflight && (Io->WriteFua==NULL || Io->Sync==NULL)) || Io->ReadGuard==NULL || Io->Quiesced==NULL){R->GateStatus=R->FinalStatus=EFI_INVALID_PARAMETER;return R->FinalStatus;}
  CONST PIANO_UFS_WRITE_BLOB *Blobs[]={&Q->Baseline.PrimaryHeader,&Q->Baseline.PrimaryEntries,&Q->Baseline.BackupHeader,&Q->Baseline.BackupEntries,&Q->Baseline.OriginalBlock};
  for(UINTN I=0;I<5;++I)if(Overlap(W,sizeof(*W),Blobs[I]->Data,Blobs[I]->Bytes) || Overlap(R,sizeof(*R),Blobs[I]->Data,Blobs[I]->Bytes)) {
    R->GateStatus=R->FinalStatus=EFI_INVALID_PARAMETER;return R->FinalStatus;
  }
  EFI_STATUS S=BaselineGate(Q,!Preflight);R->GateStatus=S;
  if(S!=EFI_SUCCESS){R->FirstFailure=R->FinalStatus=S;return S;}
  W->StateSignature=WORK_SIGNATURE;W->Running=TRUE;
  CopyMem(W->Original,Q->Baseline.OriginalBlock.Data,4096);
  S=Guard(Io,R,PianoWriteGuard);if(S!=EFI_SUCCESS)goto BeforeWriteFailure;
  S=GptGate(Q,Io,W,R,FALSE);if(S!=EFI_SUCCESS)goto BeforeWriteFailure;
  for(EFI_LBA Lba=375040;Lba<=378623;++Lba) {
    S=Read(Io,R,PianoWriteGapScan,Lba,4096,W->GapScratch);if(S!=EFI_SUCCESS)goto BeforeWriteFailure;
    if(!AllZero(W->GapScratch,4096)){S=EFI_SECURITY_VIOLATION;goto BeforeWriteFailure;}
  }
  S=Read(Io,R,PianoWriteInitialReadA,375040,4096,W->InitialA);if(S!=EFI_SUCCESS)goto BeforeWriteFailure;
  S=Read(Io,R,PianoWriteInitialReadB,375040,4096,W->InitialB);if(S!=EFI_SUCCESS)goto BeforeWriteFailure;
  if(CompareMem(W->InitialA,W->Original,4096) || CompareMem(W->InitialB,W->Original,4096) || !HashMatches(W->InitialA,4096,mOriginalHash)) {
    S=EFI_SECURITY_VIOLATION;goto BeforeWriteFailure;
  }
  // Re-query the complete gate after the full-gap scan, immediately before the
  // first WRITE. Never rely on capabilities/GPT cached at an earlier test.
  S=Guard(Io,R,PianoWriteGuard);if(S!=EFI_SUCCESS)goto BeforeWriteFailure;
  S=GptGate(Q,Io,W,R,FALSE);if(S!=EFI_SUCCESS)goto BeforeWriteFailure;
  if(Preflight) {
    R->Outcome=PianoWritePreflightPassed;R->FinalStatus=R->FirstFailure=EFI_SUCCESS;
    R->DataUnchanged=R->SafeToContinue=TRUE;goto Done;
  }
  for(UINTN I=0;I<4096;++I)W->TestPattern[I]=(UINT8)((I*73U+(I>>3))^0xA5U);
  STATIC CONST CHAR8 Marker[]="PIANO-UFS-SINGLE-BLOCK-TEST-v1";CopyMem(W->TestPattern,Marker,sizeof(Marker));
  if(!Sha256HashAll(W->TestPattern,4096,R->TestSha256)){S=EFI_SECURITY_VIOLATION;goto BeforeWriteFailure;}
  W->NeedsRecovery=TRUE;S=Write(Io,R,FALSE,W->TestPattern);
  if(S==EFI_SUCCESS)S=Sync(Io,R,FALSE);
  if(S==EFI_SUCCESS)S=Read(Io,R,PianoWriteTestRead,375040,4096,W->TestRead);
  if(S==EFI_SUCCESS){R->TestReadMatched=CompareMem(W->TestPattern,W->TestRead,4096)==0;if(!R->TestReadMatched)S=EFI_CRC_ERROR;}
  R->FirstFailure=S;
  if(R->Quarantined)goto Done;
  // Full live dual GPT + fresh WP/capacity queries must still match before the
  // second WRITE. A changed gate stops rather than overwriting a new owner.
  EFI_STATUS Restore=Guard(Io,R,PianoWriteRestoreGuard);
  if(Restore==EFI_SUCCESS)Restore=GptGate(Q,Io,W,R,TRUE);
  if(Restore!=EFI_SUCCESS){Quarantine(R,Restore);goto Done;}
  Restore=Write(Io,R,TRUE,W->Original);
  if(Restore==EFI_SUCCESS)Restore=Sync(Io,R,TRUE);
  if(Restore==EFI_SUCCESS)Restore=Read(Io,R,PianoWriteRestoreRead,375040,4096,W->RestoreRead);
  if(Restore==EFI_SUCCESS) {
    if(!Sha256HashAll(W->RestoreRead,4096,R->RestoreSha256))Restore=EFI_SECURITY_VIOLATION;
    else {
      R->RestoreReadMatched=CompareMem(W->RestoreRead,W->Original,4096)==0 && DigestMatches(R->RestoreSha256,mOriginalHash);
      if(!R->RestoreReadMatched)Restore=EFI_CRC_ERROR;
    }
  }
  if(Restore!=EFI_SUCCESS) {
    if(!R->Quarantined){R->Outcome=PianoWriteRestoreFailed;R->FinalStatus=Restore;}
    R->RequiresRecovery=TRUE;R->DataUnchanged=R->SafeToContinue=FALSE;goto Done;
  }
  R->RestoredVerified=R->DataUnchanged=R->SafeToContinue=TRUE;R->RequiresRecovery=FALSE;
  R->Outcome=S==EFI_SUCCESS?PianoWriteRestored:PianoWriteTestFailedRestored;R->FinalStatus=S;goto Done;
BeforeWriteFailure:
  R->GateStatus=R->FirstFailure=S;if(!R->Quarantined)R->FinalStatus=S;
Done:
  W->NeedsRecovery=R->RequiresRecovery || R->Quarantined;W->Running=FALSE;return R->FinalStatus;
}
EFI_STATUS PianoUfsWriteTestRun(CONST PIANO_UFS_WRITE_REQUEST *Q,CONST PIANO_UFS_WRITE_IO *Io,PIANO_UFS_WRITE_WORK *W,PIANO_UFS_WRITE_RESULT *R){return Execute(Q,Io,W,R,FALSE);}
EFI_STATUS PianoUfsWriteTestPreflight(CONST PIANO_UFS_WRITE_REQUEST *Q,CONST PIANO_UFS_WRITE_IO *Io,PIANO_UFS_WRITE_WORK *W,PIANO_UFS_WRITE_RESULT *R){return Execute(Q,Io,W,R,TRUE);}

STATIC EFI_STATUS InitWire(VOID *Trd,UINTN TrdBytes,VOID *Ucd,UINTN UcdBytes,UINT64 Iova,UINT8 Lun,EFI_LBA Lba,UINTN Bytes) {
  if(!ExactTarget(Lun,Lba,Bytes) || Trd==NULL || Ucd==NULL || TrdBytes<32 || UcdBytes<1024 ||
     Overlap(Trd,32,Ucd,1024) || (Iova&4095) || Iova>MAX_UINT32-1023)return EFI_INVALID_PARAMETER;
  ZeroMem(Trd,32);ZeroMem(Ucd,1024);UINT8 *T=Trd;
  PutLe32(T,0x11000000);PutLe32(T+8,15);PutLe32(T+16,(UINT32)Iova);
  PutLe32(T+24,0x00100010);UINT8 *R=Ucd;R[0]=1;R[2]=4;return EFI_SUCCESS;
}
EFI_STATUS PianoUfsWriteTestBuildWrite10(VOID *Trd,UINTN TrdBytes,VOID *Ucd,UINTN UcdBytes,UINT64 UcdIova,UINT64 DataIova,UINT8 Tag,UINT8 Lun,EFI_LBA Lba,UINTN Bytes) {
  if((DataIova&4095) || DataIova>MAX_UINT32-4095 || (DataIova<UcdIova+4096 && UcdIova<DataIova+4096))return EFI_INVALID_PARAMETER;
  EFI_STATUS S=InitWire(Trd,TrdBytes,Ucd,UcdBytes,UcdIova,Lun,Lba,Bytes);if(EFI_ERROR(S))return S;
  UINT8 *T=Trd,*R=Ucd;PutLe32(T,0x13000000);PutLe32(T+28,0x00400001);
  R[1]=0x20;R[3]=Tag;PutBe32(R+12,4096);R[16]=0x2A;R[17]=0x08;PutBe32(R+18,375040);R[24]=1;
  PutLe32(R+256,(UINT32)DataIova);PutLe32(R+268,4095);return EFI_SUCCESS;
}
EFI_STATUS PianoUfsWriteTestBuildSync10(VOID *Trd,UINTN TrdBytes,VOID *Ucd,UINTN UcdBytes,UINT64 UcdIova,UINT8 Tag,UINT8 Lun,EFI_LBA Lba,UINTN Bytes) {
  EFI_STATUS S=InitWire(Trd,TrdBytes,Ucd,UcdBytes,UcdIova,Lun,Lba,Bytes);if(EFI_ERROR(S))return S;
  UINT8 *R=Ucd;R[3]=Tag;R[16]=0x35;PutBe32(R+18,375040);R[24]=1;return EFI_SUCCESS;
}
