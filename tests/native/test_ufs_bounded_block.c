// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual production BlockIO/gates/builders, memory-backed transport only.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
#undef NULL
#include "../../uefi/core/PianoUfsBoundedBlock.h"
#include "../../uefi/core/PianoUfsBoundedLayout.h"
#include "PianoUfsWriteTestBaseline.h"
#include <IndustryStandard/UfsHci.h>
static PIANO_UFS_BOUNDED_BLOCK volume;
static UINT8 disk[PIANO_UFS_WINDOW_BYTES],payload[8193],output[8193];
static unsigned acquires,releases,guards,reads,gap_reads,writes,syncs,quiet,cases;
static int fail_write_at,short_write_at,fail_sync,fail_read,stale_read,bad_gpt,bad_wp,unknown_queue,fail_release;
static BOOLEAN held;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN N){return FALSE;}VOID EFIAPI DebugPrint(UINTN N,CONST CHAR8 *F,...){ }
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *P,UINTN N,UINT8 *D){return SHA256(P,N,D)!=NULL;}
static EFI_STATUS acquire(VOID *C){assert(C==&volume && !held && volume.State.Busy);held=TRUE;++acquires;return EFI_SUCCESS;}
static EFI_STATUS release(VOID *C,BOOLEAN Quarantined){assert(C==&volume && held && !volume.State.Busy);held=FALSE;++releases;return fail_release?EFI_TIMEOUT:EFI_SUCCESS;}
static EFI_STATUS guard(VOID *C,UINT8 Lun,PIANO_UFS_WRITE_GUARD *G){assert(held && C==&volume && Lun==4);++guards;*G=(PIANO_UFS_WRITE_GUARD){.Lun=4,.Collected=31,.CapacityBytes=1551892480,.Fua=!bad_wp,.UnitWriteProtect=1};return EFI_SUCCESS;}
static EFI_STATUS read(VOID *C,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,VOID *Buffer,UINTN *Got){
  assert(held && C==&volume && Lun==4);++reads;*Got=Bytes;
  if(Lba==1){assert(Bytes==4096);memcpy(Buffer,mPianoUfsWriteTestPrimaryHeader,Bytes);if(bad_gpt)((UINT8 *)Buffer)[56]^=1;}
  else if(Lba==2){assert(Bytes==12288);memcpy(Buffer,mPianoUfsWriteTestPrimaryEntries,Bytes);}
  else if(Lba==378879){assert(Bytes==4096);memcpy(Buffer,mPianoUfsWriteTestBackupHeader,Bytes);}
  else if(Lba==378873){assert(Bytes==12288);memcpy(Buffer,mPianoUfsWriteTestBackupEntries,Bytes);}
  else {assert(Lba>=375040 && Lba<=378623 && Bytes==4096 && Buffer!=volume.Tx);++gap_reads;
    if(fail_read)return EFI_DEVICE_ERROR;if(!stale_read)memcpy(Buffer,disk+(Lba-375040)*4096,4096);
  }return EFI_SUCCESS;
}
static EFI_STATUS write(VOID *C,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,CONST VOID *Data,UINTN *Got){
  assert(held && C==&volume && Lun==4 && Lba>=375040 && Lba<=378623 && Bytes==4096 && Data==volume.Tx);
  ++writes;assert(volume.State.Dirty && volume.State.NeedsRecovery && volume.State.LastAttempted && !volume.State.LastReturned && volume.State.WriteAttempts==writes);
  *Got=short_write_at==(int)writes?512:4096;memcpy(disk+(Lba-375040)*4096,Data,*Got);return fail_write_at==(int)writes?EFI_TIMEOUT:EFI_SUCCESS;
}
static EFI_STATUS sync(VOID *C,UINT8 Lun,EFI_LBA Lba,UINTN Bytes){assert(held && C==&volume && Lun==4 && Lba==375040 && Bytes==14680064);++syncs;return fail_sync?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS quiesced(VOID *C,BOOLEAN *Q){assert(held && C==&volume);++quiet;*Q=!unknown_queue;return EFI_SUCCESS;}
static PIANO_UFS_WRITE_BASELINE baseline;
static PIANO_UFS_WINDOW_IO io;
static void fresh(void){
  ++cases;memset(&volume,0,sizeof(volume));memset(disk,0,sizeof(disk));memset(payload,0x71,sizeof(payload));
  acquires=releases=guards=reads=gap_reads=writes=syncs=quiet=0;fail_write_at=short_write_at=fail_sync=fail_read=stale_read=bad_gpt=bad_wp=unknown_queue=fail_release=0;held=FALSE;
  baseline=(PIANO_UFS_WRITE_BASELINE){.PrimaryHeader={mPianoUfsWriteTestPrimaryHeader,4096},.PrimaryEntries={mPianoUfsWriteTestPrimaryEntries,12288},.BackupHeader={mPianoUfsWriteTestBackupHeader,4096},.BackupEntries={mPianoUfsWriteTestBackupEntries,12288},.OriginalBlock={mPianoUfsWriteTestOriginalBlock,4096},.ExternalArchiveVerified=TRUE,.ExternalGapBytes=14680064};memcpy(baseline.ExternalGapSha256,mPianoUfsWriteTestGapSha256,32);
  io=(PIANO_UFS_WINDOW_IO){.Io={&volume,read,write,sync,guard,quiesced},.Acquire=acquire,.Release=release};
}
static void open_ok(void){assert(PianoUfsBoundedBlockOpen(&volume,TRUE,&baseline,&io)==EFI_SUCCESS && volume.Media.MediaPresent && !volume.Media.ReadOnly && volume.Media.LastBlock==3583 && volume.Media.BlockSize==4096 && volume.Media.IoAlign==1);assert(gap_reads==3584 && guards==2 && !writes && !syncs && !volume.State.Dirty && acquires==releases);}
static EFI_STATUS put(EFI_LBA Lba,UINTN Bytes){return volume.Block.WriteBlocks(&volume.Block,1,Lba,Bytes,payload+1);}
static void fenced(void){assert(volume.State.Quarantined && volume.State.NeedsRecovery && volume.Media.ReadOnly && !volume.State.RestoreVerified && !held);unsigned before=writes;assert(put(0,4096)==(volume.Media.MediaPresent?EFI_DEVICE_ERROR:EFI_NO_MEDIA) && writes==before);assert(volume.Block.Reset(&volume.Block,FALSE)==(volume.Media.MediaPresent?EFI_DEVICE_ERROR:EFI_NO_MEDIA));}
int main(void){
  fresh();assert(PianoUfsBoundedBlockOpen(&volume,FALSE,&baseline,&io)==EFI_ACCESS_DENIED && !reads && !acquires);
  fresh();baseline.ExternalArchiveVerified=FALSE;assert(PianoUfsBoundedBlockOpen(&volume,TRUE,&baseline,&io)!=EFI_SUCCESS && !acquires);
  fresh();disk[14680063]=1;assert(PianoUfsBoundedBlockOpen(&volume,TRUE,&baseline,&io)==EFI_CRC_ERROR && !writes);fenced();
  fresh();open_ok();assert(PianoUfsBoundedBlockOpen(&volume,TRUE,&baseline,&io)==EFI_ACCESS_DENIED && gap_reads==3584);
  unsigned before=reads;assert(put(3584,4096)==EFI_INVALID_PARAMETER && put(MAX_UINT64,4096)==EFI_INVALID_PARAMETER && put(3583,8192)==EFI_INVALID_PARAMETER && put(0,512)==EFI_BAD_BUFFER_SIZE && reads==before && !writes);
  assert(volume.Block.WriteBlocks(&volume.Block,2,0,4096,payload)==EFI_MEDIA_CHANGED);assert(volume.Block.WriteBlocks(&volume.Block,1,0,0,NULL)==EFI_SUCCESS);
  assert(volume.Block.WriteBlocks(&volume.Block,1,0,4096,volume.Rx)==EFI_INVALID_PARAMETER);
  assert(put(0,8192)==EFI_SUCCESS && writes==2 && syncs==2 && volume.State.LastCompletedBlocks==2 && volume.State.VerifiedBlocks==2 && gap_reads==3586 && guards==4);
  assert(volume.State.Dirty && volume.State.NeedsRecovery && !volume.State.RestoreVerified && !volume.State.Quarantined);
  assert(put(3583,4096)==EFI_SUCCESS && writes==3 && volume.State.LastPhysical==378623 && gap_reads==3587);
  assert(volume.Block.ReadBlocks(&volume.Block,1,0,8192,output+1)==EFI_SUCCESS && !memcmp(output+1,payload+1,8192));
  assert(volume.Block.FlushBlocks(&volume.Block)==EFI_SUCCESS && syncs==4 && volume.State.Dirty);
  assert(PianoUfsBoundedBlockVerifyRestored(&volume,&io)==EFI_ACCESS_DENIED);
  assert(PianoUfsBoundedBlockCloseForRecovery(&volume)==EFI_SUCCESS && put(0,4096)==EFI_NO_MEDIA);
  assert(PianoUfsBoundedBlockVerifyRestored(&volume,&io)==EFI_CRC_ERROR && volume.State.Dirty && volume.State.NeedsRecovery);
  memset(disk,0,sizeof(disk));assert(PianoUfsBoundedBlockVerifyRestored(&volume,&io)==EFI_SUCCESS && !volume.State.Dirty && !volume.State.NeedsRecovery && !volume.State.Quarantined && volume.State.RestoreVerified && !volume.Media.MediaPresent);
  fresh();open_ok();fail_write_at=2;assert(put(0,8192)==EFI_TIMEOUT && writes==2 && volume.State.LastCompletedBlocks==1 && volume.State.LastRequestBlocks==2 && volume.State.VerifiedBlocks==1 && volume.State.LastReturned);fenced();
  fresh();open_ok();short_write_at=1;assert(put(0,4096)==EFI_BAD_BUFFER_SIZE);fenced();
  fresh();open_ok();fail_sync=1;assert(put(0,4096)==EFI_DEVICE_ERROR && writes==1 && !volume.State.VerifiedBlocks);fenced();
  fresh();open_ok();stale_read=1;assert(put(0,4096)==EFI_CRC_ERROR);fenced();
  fresh();open_ok();fail_read=1;assert(put(0,4096)==EFI_DEVICE_ERROR);fenced();
  fresh();open_ok();unknown_queue=1;assert(put(0,4096)==EFI_NOT_READY && !writes);fenced();
  fresh();open_ok();bad_wp=1;assert(put(0,4096)==EFI_WRITE_PROTECTED && !writes && !volume.State.Dirty);fenced();
  fresh();open_ok();bad_gpt=1;assert(put(0,4096)==EFI_SECURITY_VIOLATION && !writes);fenced();
  fresh();open_ok();fail_sync=1;assert(volume.Block.FlushBlocks(&volume.Block)==EFI_DEVICE_ERROR && !writes);fenced();
  fresh();open_ok();fail_release=1;assert(put(0,4096)==EFI_TIMEOUT && writes==1);fenced();
  fresh();open_ok();volume.State.Busy=TRUE;assert(put(0,4096)==EFI_NOT_READY && !writes);assert(PianoUfsBoundedBlockCloseForRecovery(&volume)==EFI_NOT_READY);
  UINT8 Trd[32],Ucd[1024];
  assert(PianoUfsBoundedBuildWrite10(Trd,32,Ucd,1024,0x40001000,0x40002000,3,4,378623,4096)==EFI_SUCCESS);
  UTP_TRD Ref={0};Ref.Int=1;Ref.Dd=UfsDataOut;Ref.Ct=1;Ref.Ocs=15;Ref.UcdBa=0x40001000>>7;Ref.RuL=Ref.RuO=16;Ref.PrdtL=1;Ref.PrdtO=64;assert(!memcmp(&Ref,Trd,32));assert(Ucd[16]==0x2A && Ucd[17]==8 && Ucd[24]==1);
  assert(PianoUfsBoundedBuildWrite10(Trd,32,Ucd,1024,0x40001000,0x40002000,3,3,375040,4096)==EFI_INVALID_PARAMETER);
  assert(PianoUfsBoundedBuildWrite10(Trd,32,Ucd,1024,0x40001000,0x40002000,3,4,375039,4096)==EFI_INVALID_PARAMETER);
  assert(PianoUfsBoundedBuildWrite10(Trd,32,Ucd,1024,0x40001000,0x40002000,3,4,378624,4096)==EFI_INVALID_PARAMETER);
  assert(PianoUfsBoundedBuildWrite10(Trd,32,Ucd,1024,0x40001000,0x40002000,3,4,375040,8192)==EFI_INVALID_PARAMETER);
  assert(PianoUfsBoundedBuildSync10(Trd,32,Ucd,1024,0x40001000,4,4,375040,14680064)==EFI_SUCCESS && Ucd[16]==0x35 && Ucd[23]==14 && Ucd[24]==0);
  assert(PianoUfsBoundedBuildSync10(Trd,32,Ucd,1024,0x40001000,4,4,375040,4096)==EFI_INVALID_PARAMETER);
  printf("Bounded BlockIO: %u actual C fixture scenarios plus fixed-gap ABI/bounds passed; only memory-backed writes.\n",cases);
}
