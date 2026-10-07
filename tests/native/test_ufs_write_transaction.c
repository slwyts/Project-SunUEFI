// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual production helper + actual SHA256, entirely memory-backed callbacks.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#undef NULL
#include "../../uefi/core/PianoUfsWriteTest.h"
#include <IndustryStandard/Ufs.h>
#include <IndustryStandard/UfsHci.h>

VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
static CONST VOID *hash_fail_buffer;
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *P,UINTN N,UINT8 *Digest){BOOLEAN Hashed=SHA256(P,N,Digest)!=NULL;return Hashed && P!=hash_fail_buffer;}

static UINT8 primary[4096],backup[4096],entries[12288],backup_entries[12288],original[4096];
static PIANO_UFS_WRITE_WORK work;
static PIANO_UFS_WRITE_RESULT result;
static unsigned cases,preflight_cases;
enum {OP_GUARD,OP_READ,OP_TEST_WRITE,OP_RESTORE_WRITE,OP_TEST_SYNC,OP_RESTORE_SYNC};
typedef struct {
  UINT8 block[4096];PIANO_UFS_WRITE_GUARD guard;
  unsigned guards,reads,gap_reads,target_reads,writes,syncs,quiet_calls,last;
  int guard_bad_call,live_bad,live_bad_before_test,live_bad_restore,gap_nonzero,initial_changed,short_read;
  int test_write_error,test_write_short,test_sync_error,test_read_error,test_read_bad,test_read_stale;
  int restore_write_error,restore_write_short,restore_sync_error,restore_read_error,restore_read_bad,restore_read_stale;
  int unknown_test_queue,quiet_error,unknown_restore_queue,unknown_read_queue;
  int test_reentry;
} DEVICE;
static DEVICE device;
static PIANO_UFS_WRITE_REQUEST request;
static PIANO_UFS_WRITE_IO io;
static EFI_STATUS guard_cb(VOID *Context,UINT8 Lun,PIANO_UFS_WRITE_GUARD *G){
  DEVICE *D=Context;assert(D==&device && Lun==4);D->last=OP_GUARD;++D->guards;*G=D->guard;
  if(D->guard_bad_call==(int)D->guards)G->PowerOnEnabled=TRUE;
  return EFI_SUCCESS;
}
static EFI_STATUS read_cb(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,VOID *Buffer,UINTN *Got){
  DEVICE *D=Context;assert(D==&device && Lun==4);D->last=OP_READ;++D->reads;
  assert(Buffer!=work.TestPattern && Buffer!=work.Original && Buffer!=original);*Got=Bytes;
  if(Lba==1){assert(Bytes==4096);memcpy(Buffer,primary,4096);}
  else if(Lba==2){assert(Bytes==12288);memcpy(Buffer,entries,12288);}
  else if(Lba==378879){assert(Bytes==4096);memcpy(Buffer,backup,4096);}
  else if(Lba==378873){assert(Bytes==12288);memcpy(Buffer,backup_entries,12288);}
  else {
    assert(Bytes==4096 && Lba>=375040 && Lba<=378623);
    if(Buffer==work.GapScratch)++D->gap_reads;
    if(Lba==375040){
      ++D->target_reads;
      if(D->writes==1){
        assert(Buffer==work.TestRead && Buffer!=work.InitialA && Buffer!=work.InitialB);
        if(D->test_read_error){*Got=0;return EFI_DEVICE_ERROR;}
        if(!D->test_read_stale)memcpy(Buffer,D->block,4096);
        if(D->test_read_bad)((UINT8 *)Buffer)[7]^=0x51;
      } else if(D->writes==2){
        assert(Buffer==work.RestoreRead && Buffer!=work.TestRead);
        if(D->restore_read_error){*Got=0;return EFI_DEVICE_ERROR;}
        if(!D->restore_read_stale)memcpy(Buffer,D->block,4096);
        if(D->restore_read_bad)((UINT8 *)Buffer)[5]^=0x91;
      } else {
        memcpy(Buffer,D->block,4096);
        if(D->initial_changed && D->target_reads==3)((UINT8 *)Buffer)[0]=0x71;
      }
    } else memset(Buffer,0,4096);
    if(D->gap_nonzero && Lba==378623)((UINT8 *)Buffer)[4095]=7;
  }
  if((D->live_bad || (D->live_bad_before_test && D->guards==2) || (D->live_bad_restore && D->writes==1)) && Lba==1)((UINT8 *)Buffer)[56]^=1;
  if(D->short_read && Lba==1)*Got=4095;
  return EFI_SUCCESS;
}
static EFI_STATUS write_cb(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,CONST VOID *Data,UINTN *Done){
  DEVICE *D=Context;assert(D==&device && Lun==4 && Lba==375040 && Bytes==4096 && D->writes<2);
  unsigned Index=D->writes++;assert(result.WriteAttempts==D->writes && result.Write[Index].Attempted && !result.Write[Index].Returned);
  if(D->test_reentry)assert(PianoUfsWriteTestRun(&request,&io,&work,&result)==EFI_ACCESS_DENIED);
  assert(result.Write[Index].Status==EFI_NOT_STARTED && result.Write[Index].Lun==4 && result.Write[Index].Lba==375040 && result.Write[Index].Bytes==4096);
  if(Index==0){
    assert(Data==work.TestPattern && D->guards==2 && D->gap_reads==3584 && D->target_reads==3);
    D->last=OP_TEST_WRITE;*Done=D->test_write_error || D->test_write_short?1024:4096;memcpy(D->block,Data,*Done);
    return D->test_write_error?EFI_TIMEOUT:EFI_SUCCESS;
  }
  assert(Data==work.Original && D->guards==3 && memcmp(Data,original,4096)==0);
  D->last=OP_RESTORE_WRITE;*Done=D->restore_write_error || D->restore_write_short?1024:4096;memcpy(D->block,Data,*Done);
  return D->restore_write_error?EFI_TIMEOUT:EFI_SUCCESS;
}
static EFI_STATUS sync_cb(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes){
  DEVICE *D=Context;assert(D==&device && Lun==4 && Lba==375040 && Bytes==4096 && D->writes>=1);++D->syncs;
  D->last=D->writes==1?OP_TEST_SYNC:OP_RESTORE_SYNC;
  return (D->writes==1 && D->test_sync_error)||(D->writes==2 && D->restore_sync_error)?EFI_UNSUPPORTED:EFI_SUCCESS;
}
static EFI_STATUS quiet_cb(VOID *Context,BOOLEAN *Quiet){
  DEVICE *D=Context;assert(D==&device);++D->quiet_calls;*Quiet=TRUE;
  if((D->last==OP_TEST_WRITE && D->unknown_test_queue)||(D->last==OP_RESTORE_WRITE && D->unknown_restore_queue))*Quiet=FALSE;
  if(D->last==OP_READ && D->unknown_read_queue)*Quiet=FALSE;
  return D->quiet_error && D->last==OP_TEST_WRITE?EFI_TIMEOUT:EFI_SUCCESS;
}
static VOID fresh(VOID){
  hash_fail_buffer=NULL;
  memset(&device,0,sizeof(device));memset(&request,0,sizeof(request));memset(&work,0,sizeof(work));memset(&result,0,sizeof(result));
  device.guard=(PIANO_UFS_WRITE_GUARD){.Lun=4,.Collected=0x1F,.CapacityBytes=1551892480ULL,.Fua=TRUE,.UnitWriteProtect=1};
  request.Lun=4;request.Lba=375040;request.Bytes=4096;request.Authorized=TRUE;request.Baseline.ExternalArchiveVerified=TRUE;
  request.Baseline.PrimaryHeader=(PIANO_UFS_WRITE_BLOB){primary,4096};request.Baseline.PrimaryEntries=(PIANO_UFS_WRITE_BLOB){entries,12288};
  request.Baseline.BackupHeader=(PIANO_UFS_WRITE_BLOB){backup,4096};request.Baseline.BackupEntries=(PIANO_UFS_WRITE_BLOB){backup_entries,12288};
  request.Baseline.OriginalBlock=(PIANO_UFS_WRITE_BLOB){original,4096};request.Baseline.ExternalGapBytes=14680064;
  const char *hex="e86bae8c0598c4ff83c695f467daa4a1e8fa01d57f9140372993366204022a4d";
  for(unsigned I=0;I<32;++I){unsigned V;assert(sscanf(hex+I*2,"%2x",&V)==1);request.Baseline.ExternalGapSha256[I]=(UINT8)V;}
  io=(PIANO_UFS_WRITE_IO){&device,read_cb,write_cb,sync_cb,guard_cb,quiet_cb};
}
static EFI_STATUS run(VOID){++cases;return PianoUfsWriteTestRun(&request,&io,&work,&result);}
static EFI_STATUS preflight(VOID){++cases;++preflight_cases;return PianoUfsWriteTestPreflight(&request,&io,&work,&result);}
static VOID no_write_callbacks(VOID){
  assert(!device.writes && !device.syncs && !result.WriteAttempts);
  for(unsigned I=0;I<2;++I) {
    assert(!result.Write[I].Attempted && !result.Write[I].Returned && !result.Write[I].Transferred);
    assert(result.Write[I].Status==EFI_NOT_STARTED);
  }
}
static VOID no_write(VOID){no_write_callbacks();assert(!result.RestoredVerified && !result.DataUnchanged);}
static VOID refused(VOID){assert(run()!=EFI_SUCCESS);no_write();}
static VOID success(VOID){
  assert(run()==EFI_SUCCESS && result.Outcome==PianoWriteRestored && result.TestReadMatched && result.RestoreReadMatched);
  assert(result.RestoredVerified && result.DataUnchanged && result.SafeToContinue && !result.Quarantined && !result.RequiresRecovery);
  assert(device.writes==2 && device.syncs==2 && memcmp(device.block,original,4096)==0);
  assert(result.Step[PianoWriteGapScan].Calls==3584 && result.Step[PianoWriteGapScan].Transferred==14680064);
  assert(result.Step[PianoWritePrimaryHeader].Calls==2 && result.Step[PianoWriteRestorePrimaryHeader].Calls==1);
}
static VOID restored_error(EFI_STATUS Expected){
  assert(run()==Expected && result.Outcome==PianoWriteTestFailedRestored && result.FirstFailure==Expected);
  assert(device.writes==2 && result.RestoredVerified && result.DataUnchanged && result.SafeToContinue && !result.RequiresRecovery);
  assert(memcmp(device.block,original,4096)==0);
}
static VOID recovery_required(VOID){
  assert(run()!=EFI_SUCCESS && result.RequiresRecovery && !result.RestoredVerified && !result.DataUnchanged && !result.SafeToContinue);
}
static VOID preflight_success(VOID){
  assert(preflight()==EFI_SUCCESS && result.Outcome==PianoWritePreflightPassed);
  assert(result.GateStatus==EFI_SUCCESS && result.FirstFailure==EFI_SUCCESS && result.FinalStatus==EFI_SUCCESS);
  no_write_callbacks();
  assert(result.DataUnchanged && result.SafeToContinue && !result.RequiresRecovery && !result.Quarantined);
  assert(!result.RestoredVerified && !result.TestReadMatched && !result.RestoreReadMatched);
  assert(!work.Running && !work.NeedsRecovery);
  assert(device.guards==2 && device.reads==3594 && device.gap_reads==3584 && device.target_reads==3 && device.quiet_calls==3596);
  assert(memcmp(device.block,original,4096)==0 && memcmp(work.Original,original,4096)==0);
  assert(memcmp(work.InitialA,original,4096)==0 && memcmp(work.InitialB,original,4096)==0);
  assert(result.Step[PianoWriteGuard].Calls==2 && result.Step[PianoWriteGuard].Status==EFI_SUCCESS);
  assert(result.Step[PianoWriteGapScan].Calls==3584 && result.Step[PianoWriteGapScan].Transferred==14680064);
  for(unsigned I=PianoWritePrimaryHeader;I<=PianoWriteBackupEntries;++I) {
    assert(result.Step[I].Calls==2 && result.Step[I].Transferred==(I==PianoWritePrimaryEntries || I==PianoWriteBackupEntries?24576:8192));
  }
  for(unsigned I=PianoWriteInitialReadA;I<=PianoWriteInitialReadB;++I) {
    assert(result.Step[I].Calls==1 && result.Step[I].Transferred==4096);
  }
  for(unsigned I=PianoWriteGuard;I<=PianoWriteInitialReadB;++I) {
    assert(result.Step[I].Attempted && result.Step[I].Status==EFI_SUCCESS);
    assert(result.Step[I].QuietAttempted && result.Step[I].Quiet && result.Step[I].QuietStatus==EFI_SUCCESS);
  }
  for(unsigned I=PianoWriteTestWrite;I<PianoWriteStepCount;++I) {
    assert(!result.Step[I].Attempted && !result.Step[I].Calls && !result.Step[I].Transferred);
    assert(result.Step[I].Status==EFI_NOT_STARTED && result.Step[I].QuietStatus==EFI_NOT_STARTED);
    assert(!result.Step[I].QuietAttempted && !result.Step[I].Quiet);
  }
  for(unsigned I=0;I<32;++I)assert(!result.TestSha256[I] && !result.RestoreSha256[I]);
  for(unsigned I=0;I<4096;++I)assert(!work.TestPattern[I] && !work.TestRead[I] && !work.RestoreRead[I]);
}
static VOID fresh_preflight(VOID){fresh();request.Authorized=FALSE;io.WriteFua=NULL;io.Sync=NULL;}
static VOID preflight_refused(EFI_STATUS Expected){
  assert(preflight()==Expected && result.Outcome==PianoWriteRefused);
  assert(result.GateStatus==Expected && result.FinalStatus==Expected);
  no_write();
  assert(!result.SafeToContinue && !result.RequiresRecovery && !result.Quarantined && !work.Running && !work.NeedsRecovery);
}
static VOID preflight_tests(VOID){
  // Both installed callbacks and NULL callbacks must be harmless in Preflight.
  fresh();preflight_success();
  fresh();request.Authorized=FALSE;preflight_success();assert(request.Authorized==FALSE);
  unsigned reads_before=device.reads,guards_before=device.guards;
  // Passing read-only preflight never grants WRITE authorization to Run.
  assert(run()==EFI_ACCESS_DENIED && device.reads==reads_before && device.guards==guards_before);no_write();
  fresh_preflight();preflight_success();assert(request.Authorized==FALSE && !io.WriteFua && !io.Sync);
  fresh();request.Authorized=FALSE;io.WriteFua=NULL;preflight_success();
  fresh();request.Authorized=FALSE;io.Sync=NULL;preflight_success();

  fresh_preflight();request.Baseline.ExternalArchiveVerified=FALSE;preflight_refused(EFI_ACCESS_DENIED);assert(!device.reads && !device.guards);
  fresh_preflight();request.Lun=3;preflight_refused(EFI_ACCESS_DENIED);assert(!device.reads && !device.guards);
  fresh_preflight();request.Lba=375041;preflight_refused(EFI_ACCESS_DENIED);assert(!device.reads && !device.guards);
  fresh_preflight();request.Baseline.ExternalGapSha256[0]^=1;preflight_refused(EFI_SECURITY_VIOLATION);assert(!device.reads && !device.guards);
  fresh_preflight();io.Read=NULL;preflight_refused(EFI_INVALID_PARAMETER);assert(!device.reads && !device.guards);
  fresh_preflight();io.ReadGuard=NULL;preflight_refused(EFI_INVALID_PARAMETER);assert(!device.reads && !device.guards);
  fresh_preflight();io.Quiesced=NULL;preflight_refused(EFI_INVALID_PARAMETER);assert(!device.reads && !device.guards);
  fresh_preflight();device.guard.Fua=FALSE;preflight_refused(EFI_WRITE_PROTECTED);assert(device.guards==1 && !device.reads);
  fresh_preflight();device.live_bad=1;preflight_refused(EFI_SECURITY_VIOLATION);assert(device.guards==1 && device.reads==1);

  // The very last gap block must be checked before success can be reported.
  fresh_preflight();device.gap_nonzero=1;preflight_refused(EFI_SECURITY_VIOLATION);
  assert(device.gap_reads==3584 && device.guards==1 && device.reads==3588 && device.target_reads==1);
  assert(result.Step[PianoWriteGapScan].Calls==3584 && result.Step[PianoWriteGapScan].Transferred==14680064);
  assert(!result.Step[PianoWriteInitialReadA].Attempted && !result.Step[PianoWriteInitialReadB].Attempted);

  fresh_preflight();device.initial_changed=1;preflight_refused(EFI_SECURITY_VIOLATION);
  assert(device.gap_reads==3584 && device.guards==1 && device.target_reads==3 && device.reads==3590);
  assert(result.Step[PianoWriteInitialReadA].Calls==1 && result.Step[PianoWriteInitialReadB].Calls==1);

  fresh_preflight();device.guard_bad_call=2;preflight_refused(EFI_WRITE_PROTECTED);
  assert(device.gap_reads==3584 && device.guards==2 && device.target_reads==3 && device.reads==3590);
  assert(result.Step[PianoWriteGuard].Calls==2 && result.Step[PianoWriteGuard].Status==EFI_WRITE_PROTECTED);
  assert(result.Step[PianoWritePrimaryHeader].Calls==1 && result.FirstFailure==EFI_WRITE_PROTECTED);

  fresh_preflight();device.live_bad_before_test=1;preflight_refused(EFI_SECURITY_VIOLATION);
  assert(device.gap_reads==3584 && device.guards==2 && device.target_reads==3 && device.reads==3591);
  assert(result.Step[PianoWritePrimaryHeader].Calls==2 && result.Step[PianoWritePrimaryEntries].Calls==1);
  assert(result.FirstFailure==EFI_SECURITY_VIOLATION);

  fresh_preflight();device.unknown_read_queue=1;
  assert(preflight()==EFI_NOT_READY && result.Outcome==PianoWriteQuarantined);
  no_write();assert(result.RequiresRecovery && result.Quarantined && !result.SafeToContinue && work.NeedsRecovery && !work.Running);
  assert(device.reads==1 && device.guards==1 && device.quiet_calls==2);
  reads_before=device.reads;guards_before=device.guards;
  assert(preflight()==EFI_ACCESS_DENIED && device.reads==reads_before && device.guards==guards_before && work.NeedsRecovery);
  no_write();
}
static VOID load(CONST CHAR8 *Folder,CONST CHAR8 *Name,VOID *Buffer,size_t Bytes){
  char path[1024];assert(snprintf(path,sizeof(path),"%s/%s",Folder,Name)>0);FILE *File=fopen(path,"rb");
  if(!File){fprintf(stderr,"Required PC baseline fixture absent: %s\n",path);exit(2);}
  assert(fread(Buffer,1,Bytes,File)==Bytes && fgetc(File)==EOF);fclose(File);
}
static VOID wire_tests(VOID){
  UINT8 Trd[32],Ucd[1024];
  assert(PianoUfsWriteTestBuildWrite10(Trd,32,Ucd,1024,0x40001000,0x40002000,77,4,375040,4096)==EFI_SUCCESS);
  UTP_TRD Ref={0};Ref.Int=1;Ref.Dd=UfsDataOut;Ref.Ct=1;Ref.Ocs=15;Ref.UcdBa=0x40001000>>7;Ref.RuL=16;Ref.RuO=16;Ref.PrdtL=1;Ref.PrdtO=64;
  assert(sizeof(Ref)==32 && memcmp(Trd,&Ref,32)==0);
  UTP_COMMAND_UPIU Cmd={0};Cmd.TransCode=1;Cmd.Flags=0x20;Cmd.Lun=4;Cmd.TaskTag=77;Cmd.ExpDataTranLen=0x00100000;
  Cmd.Cdb[0]=0x2A;Cmd.Cdb[1]=8;Cmd.Cdb[2]=0;Cmd.Cdb[3]=5;Cmd.Cdb[4]=0xB9;Cmd.Cdb[5]=0;Cmd.Cdb[8]=1;
  assert(sizeof(Cmd)==32 && memcmp(Ucd,&Cmd,32)==0);
  UINT32 expected_prdt[4]={0x40002000,0,0,4095};assert(memcmp(Ucd+256,expected_prdt,16)==0);
  assert(PianoUfsWriteTestBuildSync10(Trd,32,Ucd,1024,0x40001000,78,4,375040,4096)==EFI_SUCCESS);
  Ref.Dd=UfsNoData;Ref.PrdtL=Ref.PrdtO=0;assert(memcmp(Trd,&Ref,32)==0);
  memset(&Cmd,0,sizeof(Cmd));Cmd.TransCode=1;Cmd.Lun=4;Cmd.TaskTag=78;Cmd.Cdb[0]=0x35;Cmd.Cdb[3]=5;Cmd.Cdb[4]=0xB9;Cmd.Cdb[8]=1;
  assert(memcmp(Ucd,&Cmd,32)==0);
  const UINT8 Luns[]={0,3,5,7,0xD0};
  for(unsigned I=0;I<sizeof(Luns);++I)assert(PianoUfsWriteTestBuildWrite10(Trd,32,Ucd,1024,0x40001000,0x40002000,1,Luns[I],375040,4096)==EFI_INVALID_PARAMETER);
  assert(PianoUfsWriteTestBuildWrite10(Trd,32,Ucd,1024,0x40001000,0x40002000,1,4,375041,4096)==EFI_INVALID_PARAMETER);
  assert(PianoUfsWriteTestBuildSync10(Trd,32,Ucd,1024,0x40001000,1,4,375040,8192)==EFI_INVALID_PARAMETER);
  assert(PianoUfsWriteTestBuildWrite10(Trd,31,Ucd,1024,0x40001000,0x40002000,1,4,375040,4096)==EFI_INVALID_PARAMETER);
  assert(PianoUfsWriteTestBuildWrite10(Trd,32,Ucd,1023,0x40001000,0x40002000,1,4,375040,4096)==EFI_INVALID_PARAMETER);
  assert(PianoUfsWriteTestBuildWrite10(Ucd,32,Ucd,1024,0x40001000,0x40002000,1,4,375040,4096)==EFI_INVALID_PARAMETER);
  assert(PianoUfsWriteTestBuildWrite10(Trd,32,Ucd,1024,0x40001000,0x40001000,1,4,375040,4096)==EFI_INVALID_PARAMETER);
  assert(PianoUfsWriteTestBuildWrite10(Trd,32,Ucd,1024,0x40001001,0x40002000,1,4,375040,4096)==EFI_INVALID_PARAMETER);
  assert(PianoUfsWriteTestBuildWrite10(Trd,32,Ucd,1024,0x40001000,MAX_UINT64-4095,1,4,375040,4096)==EFI_INVALID_PARAMETER);
}
int main(int argc,char **argv){
  assert(argc==2);load(argv[1],"primary-header.bin",primary,4096);load(argv[1],"backup-header.bin",backup,4096);
  load(argv[1],"primary-entries.bin",entries,12288);load(argv[1],"backup-entries.bin",backup_entries,12288);load(argv[1],"first-block-original.bin",original,4096);
  fresh();success();fresh();device.guard.UnitWriteProtect=0;success();fresh();device.guard.UnitWriteProtect=2;success();
  fresh();request.Authorized=FALSE;refused();assert(!device.guards && !device.reads);
  fresh();request.Baseline.ExternalArchiveVerified=FALSE;refused();assert(!device.reads);
  for(unsigned I=0;I<8;++I)if(I!=4){fresh();request.Lun=(UINT8)I;refused();}
  fresh();request.Lba=375039;refused();fresh();request.Lba=375041;refused();fresh();request.Bytes=512;refused();fresh();request.Bytes=8192;refused();
  fresh();request.Baseline.ExternalGapBytes-=4096;refused();fresh();request.Baseline.ExternalGapSha256[0]^=1;refused();
  fresh();request.Baseline.PrimaryHeader.Bytes=4095;refused();fresh();request.Baseline.BackupEntries.Data=NULL;refused();
  fresh();request.Baseline.OriginalBlock.Data=work.InitialA;refused();assert(result.FinalStatus==EFI_INVALID_PARAMETER);
  fresh();io.WriteFua=NULL;refused();fresh();io.Quiesced=NULL;refused();
  fresh();device.guard.Collected=0;refused();fresh();device.guard.CapacityBytes+=4096;refused();fresh();device.guard.CapacityStatus=EFI_NOT_STARTED;refused();
  fresh();device.guard.ModeSenseStatus=EFI_DEVICE_ERROR;refused();fresh();device.guard.UnitStatus=EFI_NOT_STARTED;refused();
  fresh();device.guard.PermanentFlagStatus=EFI_NOT_STARTED;refused();fresh();device.guard.PowerOnFlagStatus=EFI_NOT_STARTED;refused();
  fresh();device.guard.Fua=FALSE;refused();fresh();device.guard.ModeWriteProtected=TRUE;refused();fresh();device.guard.UnitWriteProtect=3;refused();
  fresh();device.guard.PowerOnEnabled=TRUE;refused();fresh();device.guard.PermanentEnabled=TRUE;refused();
  fresh();device.live_bad=1;refused();fresh();device.live_bad_before_test=1;refused();
  fresh();device.gap_nonzero=1;refused();fresh();device.initial_changed=1;refused();fresh();device.short_read=1;refused();
  fresh();device.guard_bad_call=2;refused();
  fresh();device.test_write_error=1;restored_error(EFI_TIMEOUT);assert(result.Write[0].Transferred==1024);
  fresh();device.test_write_short=1;restored_error(EFI_BAD_BUFFER_SIZE);
  fresh();device.test_sync_error=1;restored_error(EFI_UNSUPPORTED);
  fresh();device.test_read_error=1;restored_error(EFI_DEVICE_ERROR);
  fresh();device.test_read_bad=1;restored_error(EFI_CRC_ERROR);
  fresh();device.test_read_stale=1;restored_error(EFI_CRC_ERROR);
  fresh();device.unknown_test_queue=1;recovery_required();assert(result.Quarantined && device.writes==1 && !device.syncs);
  fresh();device.test_write_error=device.unknown_test_queue=1;recovery_required();assert(result.Quarantined && device.writes==1);
  fresh();device.quiet_error=1;recovery_required();assert(result.Quarantined && device.writes==1);
  fresh();device.guard_bad_call=3;recovery_required();assert(result.Quarantined && device.writes==1);
  fresh();device.live_bad_restore=1;recovery_required();assert(result.Quarantined && device.writes==1);
  fresh();device.restore_write_error=1;recovery_required();assert(result.Outcome==PianoWriteRestoreFailed && device.writes==2);
  fresh();device.restore_write_short=1;recovery_required();assert(result.Outcome==PianoWriteRestoreFailed);
  fresh();device.restore_sync_error=1;recovery_required();assert(result.Outcome==PianoWriteRestoreFailed);
  fresh();device.restore_read_error=1;recovery_required();assert(result.Outcome==PianoWriteRestoreFailed);
  fresh();device.restore_read_bad=1;recovery_required();assert(result.Outcome==PianoWriteRestoreFailed);
  fresh();device.restore_read_stale=1;recovery_required();assert(result.Outcome==PianoWriteRestoreFailed);
  fresh();hash_fail_buffer=work.RestoreRead;recovery_required();assert(result.FinalStatus==EFI_SECURITY_VIOLATION && !result.RestoreReadMatched);
  fresh();device.unknown_restore_queue=1;recovery_required();assert(result.Quarantined && device.writes==2);
  unsigned writes_before=device.writes,reads_before=device.reads;
  assert(PianoUfsWriteTestRun(&request,&io,&work,&result)==EFI_ACCESS_DENIED && device.writes==writes_before && device.reads==reads_before && work.NeedsRecovery);
  fresh();device.test_reentry=1;success();
  preflight_tests();
  wire_tests();
  printf("Controlled one-block transaction: %u actual C cases (%u complete read-only Preflight cases) plus packed UFS wire ABI passed; helper callbacks were memory-only, no device access.\n",cases,preflight_cases);
}
