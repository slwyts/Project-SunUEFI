// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual Submit/guard/read/FUA/sync/quiet glue, entirely memory-backed MMIO/DMA.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <openssl/sha.h>
#undef NULL
#define PIANO_UFS_BLOCKIO 1
#define PIANO_UFS_WRITE_TEST 1
#ifndef PIANO_TEST_PREFLIGHT
#define PIANO_UFS_WRITE_RESTORE_TEST 1
#else
#define PIANO_UFS_WRITE_PREFLIGHT 1
#endif
#include "../../uefi/core/PianoUfsDmaLayout.c"
#include "../../uefi/core/PianoUfsReadOnlyDma.c"
EFI_BOOT_SERVICES *gBS;EFI_RUNTIME_SERVICES *gRT;
static EFI_BOOT_SERVICES bs;
static UINT8 trl[1024],ucd[1024],data[4096],disk[4096];
static UINT32 tr_bell,tm_bell,tr_run,tm_run,irq;
static unsigned begins,completes,write_commands,sync_commands,read_commands,guards,raised,lowered,deadloops,stalls,cases;
static BOOLEAN active_timeout,stuck_queue,stuck_after_write,transient_unquiet,bad_restore,bad_write,residual_write,bad_segment,short_read,fua_missing,gpt_changed,power_wp,wrong_lun_response;
static unsigned pauses;
static unsigned report_begins,out_dma_logs,in_dma_logs;
static EFI_TPL tpl;
static jmp_buf fence_jump;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return TRUE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){
  if(strstr(Format,"SUNUEFI_UFS_WRITE_REPORT_BEGIN"))++report_begins;
  if(strstr(Format,"SUNUEFI_UFS_DATA_DMA")){
    va_list Args;va_start(Args,Format);va_arg(Args,CONST CHAR8 *);va_arg(Args,UINT64);va_arg(Args,UINT64);
    va_arg(Args,UINT32);va_arg(Args,UINT64);CONST CHAR8 *Direction=va_arg(Args,CONST CHAR8 *);
    if(!strcmp(Direction,"to-device"))++out_dma_logs;else{assert(!strcmp(Direction,"from-device"));++in_dma_logs;}va_end(Args);
  }
}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *P,UINTN N,UINT8 *Digest){return SHA256(P,N,Digest)!=NULL;}
VOID EFIAPI MemoryFence(VOID){ }
VOID EFIAPI CpuPause(VOID){if(++pauses==QUIESCE_POLLS && transient_unquiet)stuck_queue=FALSE;}
VOID EFIAPI CpuDeadLoop(VOID){++deadloops;assert(tpl==TPL_CALLBACK && !lowered && mBlockBusy);longjmp(fence_jump,1);}
VOID PianoSmmuLogFaults(CONST PIANO_SMMU_SNAPSHOT *Snapshot){ }
EFI_STATUS PianoDmaFree(PIANO_DMA_BUFFER *B){assert(!"Quarantined transaction must never free DMA");return EFI_ACCESS_DENIED;}
EFI_STATUS PianoOwnedSmmuClose(PIANO_OWNED_SMMU *C){assert(!"Quarantined transaction must never detach SMMU");return EFI_ACCESS_DENIED;}
EFI_STATUS PianoDmaBegin(PIANO_DMA_BUFFER *B,CONST CHAR8 *Name){
  assert(B->Mapped && !B->Active && !B->Quarantined && !B->ExitRetained);B->Active=TRUE;++begins;return EFI_SUCCESS;
}
EFI_STATUS PianoDmaComplete(PIANO_DMA_BUFFER *B,EFI_STATUS Status,BOOLEAN Quiet){
  assert(B->Active);++completes;if(!Quiet){B->Quarantined=TRUE;return EFI_DEVICE_ERROR;}
  B->Active=B->Quarantined=FALSE;return Status;
}
UINT32 EFIAPI MmioRead32(UINTN Address){
  if(Address==HCI+0x58)return tr_bell;if(Address==HCI+0x78)return tm_bell;
  if(Address==HCI+0x60)return tr_run;if(Address==HCI+0x80)return tm_run;
  if(Address==HCI+0x24)return irq;if(Address==HCI+0x34)return 1;
  if(Address==HCI+0x30)return 15;if(Address==HCI+0x20)return 1;
  if(Address==HCI+0x300)return 0;assert(!"Unexpected MMIO read");return 0;
}
static void block_read(UINT32 Lba){
  ++read_commands;assert(ucd[1]==0x40 && mData.Direction==PianoDmaBidirectional);
  // RX bounce is cleared and begins before every hardware receive; never TX.
  for(unsigned I=0;I<4096;++I)assert(data[I]==0);
  if(Lba==1){memcpy(data,mPianoUfsWriteTestPrimaryHeader,4096);if(gpt_changed && guards==2)data[56]^=1;}
  else if(Lba>=2 && Lba<=4)memcpy(data,mPianoUfsWriteTestPrimaryEntries+(Lba-2)*4096,4096);
  else if(Lba==378879)memcpy(data,mPianoUfsWriteTestBackupHeader,4096);
  else if(Lba>=378873 && Lba<=378875)memcpy(data,mPianoUfsWriteTestBackupEntries+(Lba-378873)*4096,4096);
  else {assert(Lba>=375040 && Lba<=378623);if(Lba==375040)memcpy(data,disk,4096);}
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value){
  if(Address==HCI+0x60){if(!stuck_queue || Value)tr_run=Value;return Value;}
  if(Address==HCI+0x80){if(!stuck_queue)tm_run=Value;return Value;}
  if(Address==HCI+0x5C){if(!stuck_queue)tr_bell&=Value;return Value;}
  if(Address==HCI+0x7C){if(!stuck_queue)tm_bell&=Value;return Value;}
  if(Address==HCI+0x24){irq=Value;return Value;}
  assert(Address==HCI+0x58 && Value==1 && tr_run==1);
  assert(mTrl.Active && mUcd.Active && mWriteExecuting && mBlockBusy && tpl==TPL_CALLBACK);
  tr_bell=1;Le32(trl+8,0);UINT8 *R=ucd+64;memset(R,0,64);R[3]=ucd[3];R[2]=ucd[2];
  if(ucd[0]==1){
    R[0]=0x21;
    if(ucd[16]==0x28){assert(mData.Active);block_read(ReadBe32(ucd+18));if(short_read){R[1]=0x20;Be32(R+12,1);}}
    else if(ucd[16]==0x9E){++guards;assert(mData.Active);Be32(data+4,378879);Be32(data+8,4096);}
    else if(ucd[16]==0x5A){
      assert(mData.Active);data[1]=26;data[3]=fua_missing?0:0x10;data[8]=8;data[9]=18;data[10]=4;
      R[1]=0x20;Be32(R+12,4096-28);
    } else if(ucd[16]==0x2A){
      assert(mData.Active && ucd[1]==0x20 && ucd[17]==8 && ReadBe32(ucd+18)==375040 && ucd[24]==1);
      ++write_commands;assert(write_commands<=2 && mWriteResult.WriteAttempts==write_commands && mWriteResult.Write[write_commands-1].Attempted);
      assert(!mWriteResult.Write[write_commands-1].Returned && mWriteWork.NeedsRecovery);
      assert(!memcmp(data,write_commands==1?mWriteWork.TestPattern:mWriteWork.Original,4096));
      memcpy(disk,data,4096);
      if((bad_write && write_commands==1)||(bad_restore && write_commands==2))R[7]=2;
      if(residual_write && write_commands==1){R[1]=0x20;Be32(R+12,1);}
      if(bad_segment && write_commands==1)R[11]=18;
      if(write_commands==1 && (stuck_after_write || transient_unquiet))stuck_queue=TRUE;
      if(active_timeout && write_commands==1)return Value;
    } else {
      assert(ucd[16]==0x35 && ucd[1]==0 && ReadBe32(ucd+18)==375040 && ucd[24]==1 && ReadLe32(trl+28)==0);
      assert(!mData.Active);++sync_commands;
    }
    if(wrong_lun_response)R[2]=3;
  } else {
    assert(ucd[0]==0x16);R[0]=0x36;R[12]=ucd[12];R[13]=ucd[13];
    if(ucd[12]==1){assert(ucd[13]==2 && ucd[14]==4);R[14]=4;R[11]=45;R[32]=45;R[33]=2;R[34]=4;R[37]=1;}
    else {assert(ucd[12]==5 && (ucd[13]==2 || ucd[13]==3));Be32(R+20,power_wp && guards==2 && ucd[13]==3?1:0);}
  }
  tr_bell=0;return Value;
}
static EFI_STATUS EFIAPI stall(UINTN N){assert(N==100);++stalls;return EFI_SUCCESS;}
static EFI_TPL EFIAPI raise_tpl(EFI_TPL New){assert(tpl==TPL_APPLICATION && New==TPL_CALLBACK);EFI_TPL Old=tpl;tpl=New;++raised;return Old;}
static VOID EFIAPI lower_tpl(EFI_TPL Old){assert(tpl==TPL_CALLBACK && Old==TPL_APPLICATION && !mWriteExecuting && !mBlockBusy);tpl=Old;++lowered;}
static void fresh(void){
  ++cases;memset(trl,0,sizeof(trl));memset(ucd,0,sizeof(ucd));memset(data,0,sizeof(data));memset(disk,0,sizeof(disk));
  ZeroMem(&mTrl,sizeof(mTrl));ZeroMem(&mUcd,sizeof(mUcd));ZeroMem(&mData,sizeof(mData));ZeroMem(&mWriteWork,sizeof(mWriteWork));ZeroMem(&mWriteResult,sizeof(mWriteResult));ZeroMem(&mContext,sizeof(mContext));
  ZeroMem(mWriteGuards,sizeof(mWriteGuards));mWriteGuardCount=0;
  mTrl=(PIANO_DMA_BUFFER){.Cpu=trl,.Bytes=1024,.DeviceAddress=0x40000000,.Mapped=TRUE};
  mUcd=(PIANO_DMA_BUFFER){.Cpu=ucd,.Bytes=1024,.DeviceAddress=0x40001000,.Mapped=TRUE};
  mData=(PIANO_DMA_BUFFER){.Cpu=data,.Bytes=4096,.DeviceAddress=0x40002000,.Mapped=TRUE,.Direction=PianoDmaBidirectional};
  mWriteStarted=mWriteExecuting=mWriteReturned=mWriteProfileEntered=mExitRetained=mBlockBusy=FALSE;mBlockLive=mInstalled=TRUE;
  mWriteDoorbells=mSyncDoorbells=0;mWriteTransportStatus=EFI_NOT_STARTED;mServiceTag=128;
  begins=completes=write_commands=sync_commands=read_commands=guards=raised=lowered=deadloops=stalls=pauses=0;
  active_timeout=stuck_queue=stuck_after_write=transient_unquiet=bad_restore=bad_write=residual_write=bad_segment=short_read=fua_missing=gpt_changed=power_wp=wrong_lun_response=FALSE;
  tr_bell=tm_bell=tr_run=tm_run=irq=0;tpl=TPL_APPLICATION;gBS=&bs;
  report_begins=out_dma_logs=in_dma_logs=0;
}
static void safe_run(void){RunWriteTransaction();assert(!mBlockBusy && !mWriteExecuting && mWriteReturned && raised==1 && lowered==1 && !deadloops && QueuesStopped() && !irq && begins==completes);}
static void refused(void){safe_run();assert(mWriteResult.Outcome==PianoWriteRefused && !write_commands && !sync_commands && !mWriteDoorbells && !mSyncDoorbells && !mWriteWork.NeedsRecovery);}
int main(void){
  bs.Stall=stall;bs.RaiseTPL=raise_tpl;bs.RestoreTPL=lower_tpl;
  fresh();safe_run();
#ifdef PIANO_TEST_PREFLIGHT
  assert(read_commands==3602 && guards==2);
  assert(mWriteResult.Outcome==PianoWritePreflightPassed && !write_commands && !sync_commands && !mWriteResult.WriteAttempts && mWriteResult.DataUnchanged && !mWriteResult.RestoredVerified);
  assert(!out_dma_logs);
#else
  // One extra dual-GPT set and two independent post-write reads.
  assert(read_commands==3612 && guards==3 && write_commands==2 && sync_commands==2 && mWriteDoorbells==2 && mSyncDoorbells==2);
  assert(mWriteResult.RestoredVerified && mWriteResult.TestReadMatched && mWriteResult.RestoreReadMatched && !memcmp(disk,mPianoUfsWriteTestOriginalBlock,4096));
  assert(out_dma_logs==2);
#endif
  assert(in_dma_logs>3600 && report_begins==1 && mWriteGuardCount==guards);
  PIANO_UFS_WRITE_RESULT SavedResult=mWriteResult;PIANO_UFS_WRITE_WORK SavedWork=mWriteWork;
  assert(HaltService()==EFI_SUCCESS && report_begins==2 && !memcmp(&SavedResult,&mWriteResult,sizeof(SavedResult)) && !memcmp(&SavedWork,&mWriteWork,sizeof(SavedWork)));
  fresh();fua_missing=TRUE;refused();fresh();power_wp=TRUE;refused();fresh();gpt_changed=TRUE;refused();fresh();short_read=TRUE;refused();fresh();wrong_lun_response=TRUE;refused();
  fresh();assert(PianoUfsWriteTestBuildWrite10(trl,1024,ucd,1024,mUcd.DeviceAddress,mData.DeviceAddress,9,4,375040,4096)==EFI_SUCCESS);
  assert(Submit("OUTSIDE_TRANSACTION",9,FALSE)==EFI_ACCESS_DENIED && !begins && !write_commands);
  mWriteExecuting=mBlockBusy=TRUE;tpl=TPL_CALLBACK;ucd[19]^=1;
  assert(Submit("ALTERED_TARGET",9,FALSE)==EFI_ACCESS_DENIED && !begins && !write_commands);
  UINTN Done=73;assert(WriteTestWriteFua(&mWriteWork,3,375040,4096,disk,&Done)==EFI_ACCESS_DENIED && Done==0);
  assert(WriteTestWriteFua(&mWriteWork,4,375041,4096,disk,&Done)==EFI_ACCESS_DENIED && !begins);
  assert(WriteTestSync(&mWriteWork,4,375040,8192)==EFI_ACCESS_DENIED && !begins);
#ifdef PIANO_TEST_PREFLIGHT
  ucd[19]^=1;assert(Submit("PREFLIGHT_WRITE_FORBIDDEN",9,FALSE)==EFI_ACCESS_DENIED);
  assert(PianoUfsWriteTestBuildSync10(trl,1024,ucd,1024,mUcd.DeviceAddress,9,4,375040,4096)==EFI_SUCCESS);
  assert(Submit("PREFLIGHT_SYNC_FORBIDDEN",9,FALSE)==EFI_ACCESS_DENIED && !begins);
#else
  fresh();bad_write=TRUE;safe_run();assert(mWriteResult.Outcome==PianoWriteTestFailedRestored && mWriteResult.RestoredVerified && write_commands==2 && sync_commands==1);
  fresh();residual_write=TRUE;safe_run();assert(mWriteResult.Outcome==PianoWriteTestFailedRestored && mWriteResult.RestoredVerified);
  fresh();bad_segment=TRUE;safe_run();assert(mWriteResult.Outcome==PianoWriteTestFailedRestored && mWriteResult.RestoredVerified);
  fresh();active_timeout=TRUE;safe_run();assert(mWriteResult.FirstFailure==EFI_TIMEOUT && mWriteResult.RestoredVerified && write_commands==2 && sync_commands==1 && stalls==20000);
  fresh();active_timeout=transient_unquiet=TRUE;RunWriteTransaction();
  assert(mWriteResult.FirstFailure==EFI_TIMEOUT && mWriteResult.RestoredVerified && write_commands==2 && sync_commands==1 && lowered==1 && !mData.Active && !mData.Quarantined);
  fresh();bad_restore=TRUE;
  if(!setjmp(fence_jump)){RunWriteTransaction();assert(!"Unverified restore must not return");}
  assert(mWriteResult.Outcome==PianoWriteRestoreFailed && mWriteResult.RequiresRecovery && !mWriteResult.DataUnchanged && !lowered && mTrl.Quarantined && mContext.TableMemory.Quarantined);
  assert(Cleanup()==EFI_ACCESS_DENIED);
  fresh();active_timeout=stuck_after_write=TRUE;
  if(!setjmp(fence_jump)){RunWriteTransaction();assert(!"Unknown queue must not continue");}
  assert(mWriteResult.Quarantined && write_commands==1 && !sync_commands && !lowered && mWriteWork.NeedsRecovery && mData.Active && mWriteResult.Write[0].Returned);
#endif
  fresh();mWriteExecuting=mBlockBusy=TRUE;tpl=TPL_CALLBACK;tr_bell=BIT31|BIT4;tm_bell=BIT30;tr_run=tm_run=1;irq=5;
  BOOLEAN Quiet=FALSE;assert(WriteTestQuiet(&mWriteWork,&Quiet)==EFI_SUCCESS && Quiet && QueuesStopped() && !irq);
  fresh();mWriteExecuting=mBlockBusy=TRUE;tpl=TPL_CALLBACK;tm_bell=BIT31;tm_run=1;stuck_queue=TRUE;
  Quiet=TRUE;assert(WriteTestQuiet(&mWriteWork,&Quiet)==EFI_TIMEOUT && !Quiet && !write_commands && !sync_commands);
  printf("Actual UFS write transport (%s): %u memory-only Submit/guard/read/FUA/sync/quiet/fence cases passed.\n",
#ifdef PIANO_TEST_PREFLIGHT
    "preflight",
#else
    "restore-test",
#endif
    cases);
}
