// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only milestone: NOP, descriptor, LUNs, capacity, metadata LBA and GPT.
// Every transfer uses the shared allocator, owned SMMU and cache lifecycle.
// An explicit default-off profile adds only a fixed one-block test/restore.
#include "PianoOwnedSmmu.h"
#include "PianoUfsDmaLayout.h"
#include "PianoGpt.h"
#ifdef PIANO_UFS_WRITE_TEST
#include "PianoUfsWriteTest.h"
#ifndef PIANO_UFS_WRITE_TEST_BASELINE_HEADER
#define PIANO_UFS_WRITE_TEST_BASELINE_HEADER "PianoUfsWriteTestBaseline.h"
#endif
#include PIANO_UFS_WRITE_TEST_BASELINE_HEADER
#if !defined(PIANO_UFS_BLOCKIO) || (defined(PIANO_UFS_WRITE_PREFLIGHT) == defined(PIANO_UFS_WRITE_RESTORE_TEST))
#error A write-test profile requires readonly BlockIO and exactly one explicit mode
#endif
STATIC PIANO_UFS_WRITE_WORK mWriteWork;
STATIC PIANO_UFS_WRITE_RESULT mWriteResult;
STATIC BOOLEAN mWriteProfileEntered,mWriteStarted,mWriteExecuting,mWriteReturned;
STATIC UINTN mWriteDoorbells,mSyncDoorbells;
STATIC EFI_STATUS mWriteTransportStatus=EFI_NOT_STARTED;
STATIC PIANO_UFS_WRITE_GUARD mWriteGuards[3];
STATIC UINTN mWriteGuardCount;
STATIC VOID ReportWriteTransaction(VOID);
#define UFS_WRITE_EVIDENCE "write_doorbells=%lu"
#define UFS_WRITE_EVIDENCE_ARG ,(UINT64)mWriteDoorbells
#else
#define UFS_WRITE_EVIDENCE "physical_ufs_writes=0"
#define UFS_WRITE_EVIDENCE_ARG
#endif
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#ifdef PIANO_UFS_BLOCKIO
#include "PianoReadOnlyBlock.h"
#include "PianoUfsShutdown.h"
#include <Protocol/DevicePath.h>
#include <Protocol/DiskIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/EventGroup.h>
#endif
#define HCI 0x01D84000U
STATIC PIANO_OWNED_SMMU mContext;
STATIC PIANO_DMA_DEVICE mDevice;
STATIC PIANO_DMA_BUFFER mTrl,mUcd,mData;
STATIC UINT32 mTransferred,mPowerMode;
STATIC UINT32 mFlagValue;
STATIC EFI_STATUS mPermanentWpStatus=EFI_NOT_STARTED,mPowerOnWpStatus=EFI_NOT_STARTED;
STATIC BOOLEAN mPermanentWp,mPowerOnWp;
STATIC struct {UINT8 Id;UINT64 Last;UINT32 Block;} mLuns[8];
STATIC struct {EFI_STATUS CacheStatus,UnitStatus;BOOLEAN WriteProtected,Fua,WriteCache,ReadCacheDisabled;UINT8 UnitWriteProtect;} mCapabilities[8];
#ifdef PIANO_UFS_BLOCKIO
STATIC PIANO_GPT_HEADER mGpts[8];
STATIC UINTN mPartitions[8],mIoReads[8],mIoBytes[8],mPublishedHandles;
STATIC EFI_STATUS mTests[8],mWriteTests[8];
#endif
STATIC UINTN mLunCount;
STATIC UINT32 mSavedBase,mSavedUpper,mSavedRun,mSavedTaskRun,mSavedInterrupt;
STATIC BOOLEAN mInstalled;
STATIC BOOLEAN mExitRetained;
#ifdef PIANO_UFS_BLOCKIO
STATIC PIANO_READ_ONLY_BLOCK mBlocks[8];
STATIC EFI_HANDLE mBlockHandles[8],mShutdownHandle;
STATIC EFI_EVENT mExitBootEvent;
#pragma pack(1)
STATIC struct {VENDOR_DEVICE_PATH Vendor;UFS_DEVICE_PATH Ufs;EFI_DEVICE_PATH_PROTOCOL End;} mPaths[8];
#pragma pack()
STATIC BOOLEAN mBlockLive,mBlockBusy;
STATIC UINT8 mServiceTag=128;
VOID PianoUfsRetainClocks(VOID);
VOID PianoUfsStopClocks(VOID);
#ifdef PIANO_UFS_FILESYSTEMS
VOID PianoUfsProbeFileSystems(EFI_HANDLE *Parents,UINTN ParentCount);
VOID PianoUfsReportFileSystems(VOID);
#endif
#ifdef PIANO_UFS_SHELL
VOID PianoReportShellDiagnostics(VOID);
#endif
#ifdef PIANO_UFS_SETUP
VOID PianoReportSetupDiagnostics(VOID);
#endif
#endif
VOID PianoFaultSetDiagnostic(VOID (*Diagnostic)(VOID));
STATIC VOID FaultDiagnostic(VOID){
#ifdef PIANO_UFS_WRITE_TEST
  if(mWriteStarted)ReportWriteTransaction();
  // The exception recovery hook normally cold-resets. Once a WRITE was
  // attempted, an unverified restore must fence that reset/next boot as well.
  if(mWriteWork.NeedsRecovery || mWriteResult.RequiresRecovery)CpuDeadLoop();
#endif
  PianoSmmuLogFaults(&mContext.After);
}
STATIC UINT32 Read(UINT32 Offset){return MmioRead32(HCI+Offset);}
STATIC VOID Write(UINT32 Offset,UINT32 Value){MmioWrite32(HCI+Offset,Value);MemoryFence();}
STATIC UINT32 EngineLe32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
STATIC UINT32 EngineBe32(CONST UINT8 *P){return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];}
STATIC VOID LogUpiu(CONST CHAR8 *Name,CONST CHAR8 *Kind,CONST UINT8 *P) {
  for(UINTN I=0;I<32;I+=8)
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_UPIU command=%a kind=%a offset=%u bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
      Name,Kind,(UINT32)I,P[I],P[I+1],P[I+2],P[I+3],P[I+4],P[I+5],P[I+6],P[I+7]));
}
STATIC EFI_STATUS LinkReady(VOID) {
  UINT32 MemoryConfig=Read(0x300);
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_QUEUE_MODE mem_cfg=%08x mcq=%u\n",MemoryConfig,(MemoryConfig&1)!=0));
  if(MemoryConfig&1)return EFI_UNSUPPORTED;
  // Read local M-PHY TX FSM before using the inherited link. DME GET is not
  // a storage command and does not change a device descriptor or user block.
  if(!(Read(0x30)&8))return EFI_NOT_READY;
  Write(0x20,0x400);Write(0x94,0x00410000);Write(0x98,0);Write(0x9C,0);Write(0x90,1);
  BOOLEAN Done=FALSE;
  for(UINTN I=0;I<10000;++I){if(Read(0x20)&0x400){Done=TRUE;break;}gBS->Stall(100);}
  UINT32 Result=Read(0x98)&0xFF,State=Read(0x9C);
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_TX_FSM dme_done=%u result=%x state=%x\n",Done,Result,State));
  if(!Done)return EFI_TIMEOUT;
  if(Result)return EFI_DEVICE_ERROR;
  if(State!=1)return EFI_SUCCESS;
  // Exit Hibern8 only if the readback says the retained link is sleeping.
  Write(0x20,0x420);Write(0x94,0);Write(0x98,0);Write(0x9C,0);Write(0x90,0x18);Done=FALSE;
  for(UINTN I=0;I<10000;++I){if((Read(0x20)&0x420)==0x420){Done=TRUE;break;}gBS->Stall(100);}
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_HIBERN8_EXIT done=%u result=%x is=%x\n",Done,Read(0x98)&0xFF,Read(0x20)));
  return Done && !(Read(0x98)&0xFF)?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
STATIC VOID EFIAPI DmaFaultReset(VOID) {
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_TIMEOUT_REGS is=%08x hcs=%08x hce=%08x dbr=%08x rsr=%08x mem_cfg=%08x ahit=%08x uic_result=%08x\n",
    Read(0x20),Read(0x30),Read(0x34),Read(0x58),Read(0x60),Read(0x300),Read(0x18),Read(0x98)));
  PianoSmmuLogFaults(&mContext.After);
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_UNQUIESCED_RESET buffers_retained=1\n"));
  gRT->ResetSystem(EfiResetCold,EFI_DEVICE_ERROR,0,NULL);
  // ResetSystem must not return. Never proceed to free/EBS on a failed reset.
  CpuDeadLoop();
}
// A fixed iteration bound, not a millisecond timer. No Boot Services or delayed
// timer callbacks are needed after EBS begins.
#define QUIESCE_POLLS 100000U
STATIC BOOLEAN QueuesStopped(VOID) {
  UINT32 Transfer=Read(0x58),Task=Read(0x78),TransferRun=Read(0x60),TaskRun=Read(0x80);
  return (Transfer|Task|TransferRun|TaskRun)==0;
}
STATIC EFI_STATUS Quiesce(VOID) {
  if(Read(0x300)&1)return EFI_UNSUPPORTED; // Legacy queues only; never claim MCQ halt.
  Write(0x60,0);Write(0x80,0);
  UINT32 Transfer=Read(0x58),Task=Read(0x78);
  // These legacy list-clear registers use write-zero-to-clear. Clear every
  // reported pending slot, not only our ordinary transfer slot zero.
  if(Transfer)Write(0x5C,~Transfer);
  if(Task)Write(0x7C,~Task);
  for(UINTN I=0;I<QUIESCE_POLLS;++I){if(QueuesStopped())return EFI_SUCCESS;CpuPause();}
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_QUIESCE_FAILED tr_dbr=%08x tm_dbr=%08x tr_run=%08x tm_run=%08x\n",
    Read(0x58),Read(0x78),Read(0x60),Read(0x80)));
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS Submit(CONST CHAR8 *Name,UINT8 Tag,BOOLEAN Nop) {
  if(Read(0x58) || Read(0x78) || !(Read(0x34)&1) || (Read(0x30)&0xF)!=0xF)return EFI_NOT_READY;
  LogUpiu(Name,"request",mUcd.Cpu);
  UINT8 *Request=mUcd.Cpu;
  if(!(Request[0]==0 || Request[0]==1 || Request[0]==0x16))return EFI_ACCESS_DENIED;
  if(Request[0]==1 && !(Request[16]==0xA0 || Request[16]==0x9E || Request[16]==0x28 || Request[16]==0x5A ||
     Request[16]==0x1B || Request[16]==0x2A || Request[16]==0x35))return EFI_ACCESS_DENIED;
  if(Request[0]==0x16 && (Request[5]!=1 || !(Request[12]==1 || Request[12]==3 || Request[12]==5)))return EFI_ACCESS_DENIED;
  UINT32 Requested=Request[0]==1?EngineBe32(Request+12):0;
  BOOLEAN Out=Request[0]==1 && (Request[1]&0x20)!=0;
  BOOLEAN Mutation=Request[0]==1 && (Out || Request[16]==0x2A || Request[16]==0x35);
  // The default and preflight images cannot ring a WRITE/SYNC doorbell. The
  // restore-test image admits only the exact builder output during its one
  // fenced transaction; no protocol exposes this transport to consumers.
  if(Mutation) {
#if defined(PIANO_UFS_WRITE_TEST) && defined(PIANO_UFS_WRITE_RESTORE_TEST)
    UINT8 ExpectedTrd[32],ExpectedUcd[1024];EFI_STATUS Gate=EFI_ACCESS_DENIED;
    if(mWriteExecuting && mData.Direction==PianoDmaBidirectional) {
      if(Request[16]==0x2A)Gate=PianoUfsWriteTestBuildWrite10(ExpectedTrd,sizeof(ExpectedTrd),ExpectedUcd,sizeof(ExpectedUcd),
        mUcd.DeviceAddress,mData.DeviceAddress,Tag,4,375040,4096);
      else if(Request[16]==0x35)Gate=PianoUfsWriteTestBuildSync10(ExpectedTrd,sizeof(ExpectedTrd),ExpectedUcd,sizeof(ExpectedUcd),
        mUcd.DeviceAddress,Tag,4,375040,4096);
    }
    if(EFI_ERROR(Gate) || CompareMem(ExpectedTrd,mTrl.Cpu,32) || CompareMem(ExpectedUcd,mUcd.Cpu,1024))return EFI_ACCESS_DENIED;
#else
    return EFI_ACCESS_DENIED;
#endif
  }
  BOOLEAN HasData=Requested!=0;mTransferred=0;
  if(HasData && (Request[1]&0x60)!=(Out?0x20:0x40))return EFI_COMPROMISED_DATA;
  EFI_STATUS Status;
  if(HasData) {
    if(!mData.Mapped || Requested>mData.Bytes)return EFI_BAD_BUFFER_SIZE;
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_DATA_DMA command=%a dev=ufs pa=%lx iova=%lx transfer_bytes=%u buffer_bytes=%lu direction=%a alignment=%lu\n",
      Name,mData.Physical,mData.DeviceAddress,Requested,(UINT64)mData.Bytes,Out?"to-device":"from-device",(UINT64)mData.Alignment));
    Status=PianoDmaBegin(&mData,Name);if(EFI_ERROR(Status))return Status;
  }
  Status=PianoDmaBegin(&mUcd,Name);
  if(EFI_ERROR(Status)){if(HasData)PianoDmaComplete(&mData,Status,TRUE);return Status;}
  Status=PianoDmaBegin(&mTrl,Name);
  if(EFI_ERROR(Status)) {
    PianoDmaComplete(&mUcd,Status,TRUE);if(HasData)PianoDmaComplete(&mData,Status,TRUE);return Status;
  }
  Write(0x60,1);
#ifdef PIANO_UFS_WRITE_TEST
  if(Mutation){if(Out)++mWriteDoorbells;else ++mSyncDoorbells;}
#endif
  Write(0x58,1);
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_DMA_DOORBELL command=%a tag=%u trl_iova=%lx ucd_iova=%lx\n",Name,Tag,mTrl.DeviceAddress,mUcd.DeviceAddress));
  BOOLEAN Done=FALSE;
  UINTN Polls=(Request[0]==1 && Request[16]==0x1B)?100000:20000;
  for(UINTN I=0;I<Polls;++I){if(!(Read(0x58)&1)){Done=TRUE;break;}gBS->Stall(100);}
  Status=Done?EFI_SUCCESS:EFI_TIMEOUT;
#ifdef PIANO_UFS_WRITE_TEST
  mWriteTransportStatus=Status;
#endif
  BOOLEAN Quiet=!EFI_ERROR(Quiesce());
  PianoDmaComplete(&mUcd,Status,Quiet);PianoDmaComplete(&mTrl,Status,Quiet);
  if(HasData)PianoDmaComplete(&mData,Status,Quiet);
  if(!Quiet) {
#ifdef PIANO_UFS_WRITE_TEST
    // Let the helper retain the already-marked attempt and quarantine. A
    // second bounded quiet readback may retire DMA, never blindly reset/retry.
    if(mWriteExecuting)return EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;
#endif
    DmaFaultReset();return EFI_DEVICE_ERROR;
  }
  UINT8 *T=mTrl.Cpu,*R=(UINT8 *)mUcd.Cpu+PIANO_UFS_RESPONSE_OFFSET;
  UINT32 Ocs=EngineLe32(T+8)&0xFF;
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_DMA_COMPLETE command=%a status=%r ocs=%02x response_type=%02x tag=%u response=%02x is=%08x hcs=%08x\n",
    Name,Status,Ocs,R[0],R[3],R[6],Read(0x20),Read(0x30)));
  LogUpiu(Name,"response",R);
  if(EFI_ERROR(Status))return Status;
  if(Nop)return PianoUfsCheckNop(T,mUcd.Cpu,Tag);
  if(Request[0]==1) {
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_SCSI_RESPONSE command=%a lun=%u status=%02x flags=%02x residual=%u sense_length=%u key=%02x asc=%02x ascq=%02x\n",
      Name,R[2],R[7],R[1],EngineBe32(R+12),((UINT16)R[32]<<8)|R[33],R[36]&15,R[46],R[47]));
    EFI_STATUS S=PianoUfsCheckReadResponse(T,mUcd.Cpu,Tag,Requested,&mTransferred);
    // READ diagnostics allow legal underflow; fixed WRITE and SYNC require
    // GOOD, correct tag/LUN, no sense/residual/overflow/underflow and exact count.
    if(Mutation && (!EFI_ERROR(S) && ((R[1]&0x60) || EngineBe32(R+12)!=0 || R[2]!=4 || R[10] || R[11] || R[32] || R[33] || mTransferred!=Requested)))S=EFI_DEVICE_ERROR;
    return S;
  }
  if(Ocs!=0 || (R[0]&0x3F)!=0x36 || R[3]!=Tag || R[6]!=0)return EFI_DEVICE_ERROR;
  if(Request[12]==5) {
    if(R[12]!=5 || R[13]!=Request[13] || R[14]!=0 || R[15]!=0 || R[10]!=0 || R[11]!=0)return EFI_COMPROMISED_DATA;
    mFlagValue=EngineBe32(R+20);
    if(mFlagValue>1)return EFI_COMPROMISED_DATA;
    return EFI_SUCCESS;
  }
  if(((UINT8 *)mUcd.Cpu)[12]==3) {
    if(R[12]!=3 || R[13]!=2 || R[14]!=0 || R[15]!=0 || R[10]!=0 || R[11]!=0)return EFI_COMPROMISED_DATA;
    mPowerMode=EngineBe32(R+20);
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_CURRENT_POWER_MODE value=%08x\n",mPowerMode));return EFI_SUCCESS;
  }
  if(R[12]!=1)return EFI_DEVICE_ERROR;
  UINT16 Bytes=((UINT16)R[10]<<8)|R[11];
  UINT16 QueryBytes=((UINT16)Request[18]<<8)|Request[19];
  if(Bytes<2 || Bytes>QueryBytes || R[32]<2 || R[33]!=R[13] || (QueryBytes!=2 && R[32]>Bytes))return EFI_COMPROMISED_DATA;
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_DESCRIPTOR idn=%u index=%u bytes=%u descriptor_length=%u descriptor_type=%u\n",R[13],R[14],Bytes,R[32],R[33]));
  if(R[13]==0 && Bytes>=0x1E)DEBUG((DEBUG_WARN,"SUNUEFI_UFS_DEVICE_DESC luns=%u well_known_luns=%u\n",R[32+6],R[32+7]));
  return EFI_SUCCESS;
}
STATIC EFI_STATUS QueryDescriptor(UINT8 Tag,UINT16 Bytes) {
  EFI_STATUS Status=PianoUfsBuildReadDescriptor(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,Tag,0,0,Bytes);
  return EFI_ERROR(Status)?Status:Submit("QUERY_READ_DEVICE_DESCRIPTOR",Tag,FALSE);
}
STATIC EFI_STATUS ReadScsi(CONST CHAR8 *Name,UINT8 Tag,UINT8 Lun,PIANO_UFS_READ_COMMAND Command,UINT32 Lba,UINT32 Bytes) {
  ZeroMem(mData.Cpu,mData.Bytes);
  EFI_STATUS Status=PianoUfsBuildReadCommand(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,
    mData.DeviceAddress,Bytes,Tag,Lun,Command,Lba,Command==PianoUfsReadLba10?1:0);
  return EFI_ERROR(Status)?Status:Submit(Name,Tag,FALSE);
}
STATIC EFI_STATUS ReadLunCapacity(VOID) {
  EFI_STATUS Status=PianoDmaAllocate(&mDevice,4096,4096,32,
#ifdef PIANO_UFS_WRITE_TEST
    PianoDmaBidirectional,
#else
    PianoDmaFromDevice,
#endif
    &mData);if(EFI_ERROR(Status))return Status;
  Status=PianoDmaMap(&mData);if(EFI_ERROR(Status))return Status;
  Status=ReadScsi("REPORT_LUNS",10,0,PianoUfsReportLuns,0,4096);
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_REPORT_LUNS_RESULT %r transferred=%u\n",Status,mTransferred));if(EFI_ERROR(Status))return Status;
  UINT8 Luns[8];UINTN Count;
  Status=PianoUfsParseLuns(mData.Cpu,mTransferred,Luns,&Count);if(EFI_ERROR(Status))return Status;
  mLunCount=Count;
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_LUN_COUNT count=%u\n",(UINT32)Count));
  for(UINTN I=0;I<Count;++I) {
    UINT64 Last;UINT32 Block;
    Status=ReadScsi("READ_CAPACITY_16",20+I,Luns[I],PianoUfsReadCapacity16,0,32);
    if(!EFI_ERROR(Status))Status=PianoUfsParseCapacity(mData.Cpu,mTransferred,&Last,&Block);
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_CAPACITY lun=%u status=%r last_lba=%lx block_bytes=%u\n",Luns[I],Status,
      EFI_ERROR(Status)?0:Last,EFI_ERROR(Status)?0:Block));
    if(EFI_ERROR(Status))return Status;
    mLuns[I].Id=Luns[I];mLuns[I].Last=Last;mLuns[I].Block=Block;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ReadBlock(UINT8 *Tag,UINTN LunIndex,UINT32 Lba) {
  UINT32 Block=mLuns[LunIndex].Block;
  if((UINT64)Lba>mLuns[LunIndex].Last)return EFI_INVALID_PARAMETER;
  EFI_STATUS Status=ReadScsi("READ_LBA_10",(*Tag)++,mLuns[LunIndex].Id,PianoUfsReadLba10,Lba,Block);
  if(!EFI_ERROR(Status) && mTransferred!=Block)Status=EFI_BAD_BUFFER_SIZE;
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_LBA_READ lun=%u lba=%u bytes=%u status=%r crc32=%08x physical_ufs_writes=0\n",
    mLuns[LunIndex].Id,Lba,mTransferred,Status,EFI_ERROR(Status)?0:PianoGptCrc32(mData.Cpu,Block)));
  return Status;
}
STATIC VOID ReportCapabilities(VOID) {
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WP_FLAGS permanent_status=%r permanent_enabled=%u power_on_status=%r power_on_enabled=%u " UFS_WRITE_EVIDENCE "\n",
    mPermanentWpStatus,mPermanentWp,mPowerOnWpStatus,mPowerOnWp UFS_WRITE_EVIDENCE_ARG));
  for(UINTN I=0;I<mLunCount;++I)
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_CACHE_CAPS lun=%u mode_sense=%r wp=%u dpofua=%u wce=%u rcd=%u unit_descriptor=%r unit_wp=%u " UFS_WRITE_EVIDENCE "\n",
      mLuns[I].Id,mCapabilities[I].CacheStatus,mCapabilities[I].WriteProtected,mCapabilities[I].Fua,
      mCapabilities[I].WriteCache,mCapabilities[I].ReadCacheDisabled,mCapabilities[I].UnitStatus,mCapabilities[I].UnitWriteProtect UFS_WRITE_EVIDENCE_ARG));
}
STATIC VOID ReadCapabilities(VOID) {
  // Read current caching mode and LU descriptor. No MODE SELECT, write query,
  // WRITE CDB or SYNCHRONIZE CACHE is submitted by this diagnostic.
  ZeroMem(mCapabilities,sizeof(mCapabilities));
  EFI_STATUS Flag=PianoUfsBuildReadWriteProtectFlag(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,30,2);
  if(!EFI_ERROR(Flag))Flag=Submit("QUERY_READ_PERMANENT_WRITE_PROTECT_FLAG",30,FALSE);
  mPermanentWpStatus=Flag;mPermanentWp=!EFI_ERROR(Flag) && mFlagValue!=0;
  Flag=PianoUfsBuildReadWriteProtectFlag(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,31,3);
  if(!EFI_ERROR(Flag))Flag=Submit("QUERY_READ_POWER_ON_WRITE_PROTECT_FLAG",31,FALSE);
  mPowerOnWpStatus=Flag;mPowerOnWp=!EFI_ERROR(Flag) && mFlagValue!=0;
  for(UINTN I=0;I<mLunCount;++I) {
    EFI_STATUS S=ReadScsi("MODE_SENSE_10_CACHE",32+I,mLuns[I].Id,PianoUfsModeSense10,0,4096);
    if(!EFI_ERROR(S))S=PianoUfsParseCacheMode(mData.Cpu,mTransferred,&mCapabilities[I].WriteProtected,
      &mCapabilities[I].Fua,&mCapabilities[I].WriteCache,&mCapabilities[I].ReadCacheDisabled);
    mCapabilities[I].CacheStatus=S;
    S=PianoUfsBuildReadDescriptor(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,40+I,2,mLuns[I].Id,255);
    if(!EFI_ERROR(S))S=Submit("QUERY_READ_UNIT_DESCRIPTOR",40+I,FALSE);
    UINT8 *R=(UINT8 *)mUcd.Cpu+PIANO_UFS_RESPONSE_OFFSET;
    if(!EFI_ERROR(S) && (R[14]!=mLuns[I].Id || R[32]<6 || R[34]!=mLuns[I].Id))S=EFI_COMPROMISED_DATA;
    if(!EFI_ERROR(S))mCapabilities[I].UnitWriteProtect=R[37];
    mCapabilities[I].UnitStatus=S;
  }
  ReportCapabilities();
}
#ifdef PIANO_UFS_WRITE_TEST
// One CPU-owned workspace and shared bidirectional bounce buffer. Every RX
// clears the bounce, submits a fresh READ, completes/invalidate DMA, then copies
// to a separate helper RX buffer. A prior TX buffer is never readback evidence.
STATIC BOOLEAN WriteCallbackContext(VOID *Context){return Context==&mWriteWork && mWriteExecuting && mBlockBusy && !mExitRetained;}
STATIC EFI_STATUS WriteTestRead(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,VOID *Buffer,UINTN *Got) {
  if(Got==NULL)return EFI_INVALID_PARAMETER;*Got=0;
  if(!WriteCallbackContext(Context) || Lun!=4 || Buffer==NULL || Bytes==0 || Bytes%4096 ||
     Lba>PIANO_UFS_WRITE_TEST_CAPACITY/4096-1 || Bytes/4096>PIANO_UFS_WRITE_TEST_CAPACITY/4096-Lba)return EFI_ACCESS_DENIED;
  for(UINTN Offset=0;Offset<Bytes;Offset+=4096) {
    EFI_STATUS S=ReadScsi("WRITE_TEST_READ10",mServiceTag++,4,PianoUfsReadLba10,(UINT32)(Lba+Offset/4096),4096);
    if(EFI_ERROR(S))return S;
    UINT8 *Response=(UINT8 *)mUcd.Cpu+PIANO_UFS_RESPONSE_OFFSET;
    if(mTransferred!=4096 || Response[2]!=4 || (Response[1]&0x60) || EngineBe32(Response+12)!=0)return EFI_BAD_BUFFER_SIZE;
    CopyMem((UINT8 *)Buffer+Offset,mData.Cpu,4096);*Got+=4096;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS WriteTestWriteFua(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,CONST VOID *Buffer,UINTN *Done) {
  if(Done==NULL)return EFI_INVALID_PARAMETER;*Done=0;
#ifdef PIANO_UFS_WRITE_RESTORE_TEST
  if(!WriteCallbackContext(Context) || Lun!=4 || Lba!=375040 || Bytes!=4096 || Buffer==NULL)return EFI_ACCESS_DENIED;
  CopyMem(mData.Cpu,Buffer,4096);UINT8 Tag=mServiceTag++;
  EFI_STATUS S=PianoUfsWriteTestBuildWrite10(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,mData.DeviceAddress,Tag,Lun,Lba,Bytes);
  if(!EFI_ERROR(S))S=Submit("WRITE_TEST_FIXED_WRITE10_FUA",Tag,FALSE);
  if(!EFI_ERROR(S))*Done=mTransferred;
  return S;
#else
  return EFI_ACCESS_DENIED;
#endif
}
STATIC EFI_STATUS WriteTestSync(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes) {
#ifdef PIANO_UFS_WRITE_RESTORE_TEST
  if(!WriteCallbackContext(Context) || Lun!=4 || Lba!=375040 || Bytes!=4096)return EFI_ACCESS_DENIED;
  UINT8 Tag=mServiceTag++;
  EFI_STATUS S=PianoUfsWriteTestBuildSync10(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,Tag,Lun,Lba,Bytes);
  return EFI_ERROR(S)?S:Submit("WRITE_TEST_FIXED_SYNC10",Tag,FALSE);
#else
  return EFI_ACCESS_DENIED;
#endif
}
STATIC EFI_STATUS WriteTestCollectGuard(VOID *Context,UINT8 Lun,PIANO_UFS_WRITE_GUARD *G) {
  if(!WriteCallbackContext(Context) || Lun!=4 || G==NULL)return EFI_ACCESS_DENIED;
  ZeroMem(G,sizeof(*G));G->Lun=4;
  G->CapacityStatus=G->ModeSenseStatus=G->UnitStatus=G->PermanentFlagStatus=G->PowerOnFlagStatus=EFI_NOT_STARTED;
  UINT64 Last=0;UINT32 Block=0;UINT8 Tag=mServiceTag++;
  EFI_STATUS S=ReadScsi("WRITE_TEST_FRESH_CAPACITY",Tag,4,PianoUfsReadCapacity16,0,32);
  UINT8 *Response=(UINT8 *)mUcd.Cpu+PIANO_UFS_RESPONSE_OFFSET;
  if(!EFI_ERROR(S) && (mTransferred!=32 || Response[2]!=4 || (Response[1]&0x60) || EngineBe32(Response+12)))S=EFI_COMPROMISED_DATA;
  if(!EFI_ERROR(S))S=PianoUfsParseCapacity(mData.Cpu,mTransferred,&Last,&Block);
  if(!EFI_ERROR(S) && (Block!=4096 || Last!=PIANO_UFS_WRITE_TEST_CAPACITY/4096-1))S=EFI_COMPROMISED_DATA;
  G->CapacityStatus=S;if(EFI_ERROR(S))return S;G->CapacityBytes=(Last+1)*Block;G->Collected|=1;
  S=ReadScsi("WRITE_TEST_FRESH_MODE_SENSE",mServiceTag++,4,PianoUfsModeSense10,0,4096);
  if(!EFI_ERROR(S) && Response[2]!=4)S=EFI_COMPROMISED_DATA;
  BOOLEAN Wce=FALSE,Rcd=FALSE;
  if(!EFI_ERROR(S))S=PianoUfsParseCacheMode(mData.Cpu,mTransferred,&G->ModeWriteProtected,&G->Fua,&Wce,&Rcd);
  G->ModeSenseStatus=S;if(EFI_ERROR(S))return S;G->Collected|=2;
  Tag=mServiceTag++;S=PianoUfsBuildReadDescriptor(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,Tag,2,4,255);
  if(!EFI_ERROR(S))S=Submit("WRITE_TEST_FRESH_UNIT_DESCRIPTOR",Tag,FALSE);
  UINT8 *R=(UINT8 *)mUcd.Cpu+PIANO_UFS_RESPONSE_OFFSET;
  if(!EFI_ERROR(S) && (R[13]!=2 || R[14]!=4 || R[32]<6 || R[33]!=2 || R[34]!=4 || R[37]>2))S=EFI_COMPROMISED_DATA;
  G->UnitStatus=S;if(EFI_ERROR(S))return S;G->UnitWriteProtect=R[37];G->Collected|=4;
  Tag=mServiceTag++;S=PianoUfsBuildReadWriteProtectFlag(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,Tag,2);
  if(!EFI_ERROR(S))S=Submit("WRITE_TEST_FRESH_PERMANENT_WP_FLAG",Tag,FALSE);
  G->PermanentFlagStatus=S;if(EFI_ERROR(S))return S;G->PermanentEnabled=mFlagValue!=0;G->Collected|=8;
  Tag=mServiceTag++;S=PianoUfsBuildReadWriteProtectFlag(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,Tag,3);
  if(!EFI_ERROR(S))S=Submit("WRITE_TEST_FRESH_POWER_ON_WP_FLAG",Tag,FALSE);
  G->PowerOnFlagStatus=S;if(EFI_ERROR(S))return S;G->PowerOnEnabled=mFlagValue!=0;G->Collected|=16;
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WRITE_FRESH_GUARD collected=%02x capacity=%lu fua=%u mode_wp=%u unit_wp=%u permanent=%u power_on=%u wce=%u\n",
    G->Collected,G->CapacityBytes,G->Fua,G->ModeWriteProtected,G->UnitWriteProtect,G->PermanentEnabled,G->PowerOnEnabled,Wce));
  return EFI_SUCCESS;
}
STATIC EFI_STATUS WriteTestGuard(VOID *Context,UINT8 Lun,PIANO_UFS_WRITE_GUARD *G) {
  EFI_STATUS S=WriteTestCollectGuard(Context,Lun,G);
  if(G!=NULL && mWriteGuardCount<ARRAY_SIZE(mWriteGuards))mWriteGuards[mWriteGuardCount++]=*G;
  return S;
}
STATIC EFI_STATUS WriteTestQuiet(VOID *Context,BOOLEAN *Quiet) {
  if(Quiet==NULL)return EFI_INVALID_PARAMETER;*Quiet=FALSE;
  if(!WriteCallbackContext(Context))return EFI_ACCESS_DENIED;
  EFI_STATUS S=Quiesce();
  if(!EFI_ERROR(S)){Write(0x24,0);if(Read(0x24)!=0 || !QueuesStopped())S=EFI_DEVICE_ERROR;}
  if(EFI_ERROR(S))return S;
  // A failed first halt leaves buffers Active/quarantined. Retire only after
  // the second complete readback proves all engines and IRQ are stopped.
  PIANO_DMA_BUFFER *Buffers[]={&mUcd,&mTrl,&mData};
  for(UINTN I=0;I<ARRAY_SIZE(Buffers);++I)if(Buffers[I]->Active) {
    PianoDmaComplete(Buffers[I],mWriteTransportStatus,TRUE);
    if(Buffers[I]->Active || Buffers[I]->Quarantined)return EFI_DEVICE_ERROR;
  }
  *Quiet=TRUE;return EFI_SUCCESS;
}
STATIC VOID ReportWriteTransaction(VOID) {
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WRITE_REPORT_BEGIN mode=%a started=%u returned=%u executing=%u lun=4 lba=375040 bytes=4096 write_doorbells=%lu sync_doorbells=%lu\n",
#ifdef PIANO_UFS_WRITE_PREFLIGHT
    "preflight",
#else
    "restore-test",
#endif
    mWriteStarted,mWriteReturned,mWriteExecuting,(UINT64)mWriteDoorbells,(UINT64)mSyncDoorbells));
  if(!mWriteStarted)return;
  PIANO_UFS_WRITE_RESULT *R=&mWriteResult;
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WRITE_RESULT outcome=%u gate=%r first_failure=%r final=%r attempts=%lu test_match=%u restore_match=%u restored_verified=%u data_unchanged=%u safe_to_continue=%u requires_recovery=%u quarantined=%u\n",
    R->Outcome,R->GateStatus,R->FirstFailure,R->FinalStatus,(UINT64)R->WriteAttempts,R->TestReadMatched,R->RestoreReadMatched,
    R->RestoredVerified,R->DataUnchanged,R->SafeToContinue,R->RequiresRecovery,R->Quarantined));
  for(UINTN I=0;I<mWriteGuardCount;++I) {
    PIANO_UFS_WRITE_GUARD *G=&mWriteGuards[I];
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WRITE_GUARD id=%u collected=%02x lun=%u capacity=%lu capacity_status=%r mode_status=%r unit_status=%r permanent_status=%r power_status=%r fua=%u mode_wp=%u unit_wp=%u permanent=%u power_on=%u\n",
      (UINT32)I,G->Collected,G->Lun,G->CapacityBytes,G->CapacityStatus,G->ModeSenseStatus,G->UnitStatus,G->PermanentFlagStatus,G->PowerOnFlagStatus,
      G->Fua,G->ModeWriteProtected,G->UnitWriteProtect,G->PermanentEnabled,G->PowerOnEnabled));
  }
  for(UINTN I=0;I<PianoWriteStepCount;++I) {
    PIANO_UFS_WRITE_STEP *S=&R->Step[I];
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WRITE_STEP id=%u attempted=%u calls=%lu bytes=%lu status=%r quiet_attempted=%u quiet=%u quiet_status=%r\n",
      (UINT32)I,S->Attempted,(UINT64)S->Calls,(UINT64)S->Transferred,S->Status,S->QuietAttempted,S->Quiet,S->QuietStatus));
  }
  for(UINTN I=0;I<2;++I) {
    PIANO_UFS_WRITE_ATTEMPT *A=&R->Write[I];
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WRITE_ATTEMPT id=%u attempted=%u returned=%u restore=%u lun=%u lba=%lu bytes=%lu transferred=%lu status=%r\n",
      (UINT32)I,A->Attempted,A->Returned,A->Restore,A->Lun,A->Lba,(UINT64)A->Bytes,(UINT64)A->Transferred,A->Status));
  }
  for(UINTN I=0;I<32;I+=8)DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WRITE_HASH offset=%u test=%02x%02x%02x%02x%02x%02x%02x%02x restore=%02x%02x%02x%02x%02x%02x%02x%02x\n",
    (UINT32)I,R->TestSha256[I],R->TestSha256[I+1],R->TestSha256[I+2],R->TestSha256[I+3],R->TestSha256[I+4],R->TestSha256[I+5],R->TestSha256[I+6],R->TestSha256[I+7],
    R->RestoreSha256[I],R->RestoreSha256[I+1],R->RestoreSha256[I+2],R->RestoreSha256[I+3],R->RestoreSha256[I+4],R->RestoreSha256[I+5],R->RestoreSha256[I+6],R->RestoreSha256[I+7]));
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WRITE_REPORT_END boot_blocked=1 complete_os_handoff_verified=0\n"));
}
STATIC VOID RunWriteTransaction(VOID) {
  if(mWriteStarted || mBlockBusy){DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WRITE_REENTRY_DENIED ledger_retained=1\n"));CpuDeadLoop();return;}
  PIANO_UFS_WRITE_REQUEST Q={.Lun=4,.Lba=375040,.Bytes=4096,
#ifdef PIANO_UFS_WRITE_RESTORE_TEST
    .Authorized=TRUE,
#endif
    .Baseline={.PrimaryHeader={mPianoUfsWriteTestPrimaryHeader,sizeof(mPianoUfsWriteTestPrimaryHeader)},
      .PrimaryEntries={mPianoUfsWriteTestPrimaryEntries,sizeof(mPianoUfsWriteTestPrimaryEntries)},
      .BackupHeader={mPianoUfsWriteTestBackupHeader,sizeof(mPianoUfsWriteTestBackupHeader)},
      .BackupEntries={mPianoUfsWriteTestBackupEntries,sizeof(mPianoUfsWriteTestBackupEntries)},
      .OriginalBlock={mPianoUfsWriteTestOriginalBlock,sizeof(mPianoUfsWriteTestOriginalBlock)},
      .ExternalArchiveVerified=PIANO_UFS_WRITE_BASELINE_EXTERNAL_ARCHIVE_VERIFIED,.ExternalGapBytes=PIANO_UFS_WRITE_BASELINE_GAP_BYTES}};
  CopyMem(Q.Baseline.ExternalGapSha256,mPianoUfsWriteTestGapSha256,32);
  PIANO_UFS_WRITE_IO Io={&mWriteWork,WriteTestRead,WriteTestWriteFua,WriteTestSync,WriteTestGuard,WriteTestQuiet};
  // Hold the recovery timer (TPL_CALLBACK) throughout WRITE through verified
  // restore. The controller polls use bounded Stall calls, not timer callbacks.
  EFI_TPL Old=gBS->RaiseTPL(TPL_CALLBACK);mBlockBusy=TRUE;mWriteExecuting=TRUE;mWriteStarted=TRUE;
#ifdef PIANO_UFS_WRITE_PREFLIGHT
  PianoUfsWriteTestPreflight(&Q,&Io,&mWriteWork,&mWriteResult);
#else
  PianoUfsWriteTestRun(&Q,&Io,&mWriteWork,&mWriteResult);
#endif
  mWriteReturned=TRUE;mWriteExecuting=FALSE;ReportWriteTransaction();
  if(mWriteResult.RequiresRecovery || mWriteResult.Quarantined || mWriteWork.NeedsRecovery) {
    mTrl.Quarantined=mUcd.Quarantined=mData.Quarantined=TRUE;mContext.TableMemory.Quarantined=TRUE;
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_WRITE_RECOVERY_REQUIRED timer_held=1 dma_retained=1 next_boot_blocked=1\n"));CpuDeadLoop();return;
  }
  mBlockBusy=FALSE;gBS->RestoreTPL(Old);
}
#endif
STATIC EFI_STATUS ReadGpts(VOID) {
  UINT8 Tag=64;UINTN Valid=0;
  for(UINTN I=0;I<mLunCount;++I) {
    EFI_STATUS Status=ReadBlock(&Tag,I,0);if(EFI_ERROR(Status))return Status;
    UINT8 *Data=mData.Cpu;
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_MBR lun=%u signature_55aa=%u\n",mLuns[I].Id,Data[510]==0x55 && Data[511]==0xAA));
    Status=ReadBlock(&Tag,I,1);if(EFI_ERROR(Status))return Status;
    PIANO_GPT_HEADER Header;
    Status=PianoGptParseHeader(mData.Cpu,mTransferred,mLuns[I].Last,mLuns[I].Block,&Header);
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_GPT_HEADER lun=%u status=%r\n",mLuns[I].Id,Status));
    if(Status==EFI_NOT_FOUND)continue;
    if(EFI_ERROR(Status))return Status;
    UINT8 *Entries=AllocatePool(Header.ArrayBytes);if(Entries==NULL)return EFI_OUT_OF_RESOURCES;
    for(UINTN Offset=0;Offset<Header.ArrayBytes;Offset+=mLuns[I].Block) {
      Status=ReadBlock(&Tag,I,(UINT32)(Header.EntryLba+Offset/mLuns[I].Block));
      if(EFI_ERROR(Status))break;
      UINTN Copy=Header.ArrayBytes-Offset;if(Copy>mLuns[I].Block)Copy=mLuns[I].Block;
      CopyMem(Entries+Offset,mData.Cpu,Copy);
    }
    UINTN Active=0;
    if(!EFI_ERROR(Status))Status=PianoGptCheckEntries(Entries,Header.ArrayBytes,&Header,&Active);
    FreePool(Entries);
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_GPT_VERIFIED lun=%u status=%r header_crc=%08x array_crc=%08x entry_lba=%lx array_bytes=%u entries=%u active_partitions=%u\n",
      mLuns[I].Id,Status,Header.HeaderCrc,Header.ArrayCrc,Header.EntryLba,Header.ArrayBytes,Header.Entries,(UINT32)Active));
    if(EFI_ERROR(Status))return Status;
#ifdef PIANO_UFS_BLOCKIO
    mGpts[I]=Header;mPartitions[I]=Active;
#endif
    ++Valid;
  }
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_GPT_MILESTONE luns=%u valid_gpts=%u physical_ufs_writes=0\n",(UINT32)mLunCount,(UINT32)Valid));
  return Valid?EFI_SUCCESS:EFI_NOT_FOUND;
}
STATIC EFI_STATUS Cleanup(VOID) {
  if(mExitRetained)return EFI_ACCESS_DENIED;
#ifdef PIANO_UFS_WRITE_TEST
  if(mWriteWork.NeedsRecovery || mWriteResult.RequiresRecovery || mWriteResult.Quarantined)return EFI_ACCESS_DENIED;
#endif
  BOOLEAN Restore=mInstalled;
  if(mInstalled) {
    EFI_STATUS Status=Quiesce();if(EFI_ERROR(Status)){DmaFaultReset();return Status;}
    // Restore/verify bases and IRQ while both engines remain stopped. Do not
    // restart the inherited list while our mapping/tables are being released.
    Write(0x50,mSavedBase);Write(0x54,mSavedUpper);Write(0x24,mSavedInterrupt);
    if(Read(0x50)!=mSavedBase || Read(0x54)!=mSavedUpper || Read(0x24)!=mSavedInterrupt || !QueuesStopped())return EFI_DEVICE_ERROR;
  }
  if(mUcd.Signature){EFI_STATUS S=PianoDmaFree(&mUcd);if(EFI_ERROR(S))return S;}
  if(mTrl.Signature){EFI_STATUS S=PianoDmaFree(&mTrl);if(EFI_ERROR(S))return S;}
  if(mData.Signature){EFI_STATUS S=PianoDmaFree(&mData);if(EFI_ERROR(S))return S;}
  EFI_STATUS Status=PianoOwnedSmmuClose(&mContext);if(EFI_ERROR(Status))return Status;
  if(Restore) {
    Write(0x80,mSavedTaskRun);Write(0x60,mSavedRun);
    if(Read(0x80)!=mSavedTaskRun || Read(0x60)!=mSavedRun)return EFI_DEVICE_ERROR;
    mInstalled=FALSE;
  }
  return EFI_SUCCESS;
}
#ifdef PIANO_UFS_BLOCKIO
STATIC EFI_STATUS ServiceRead(VOID *Context,UINT8 Lun,EFI_LBA Lba,UINTN Bytes,VOID *Buffer) {
  if(!mBlockLive || mBlockBusy)return EFI_NOT_READY;
  UINTN I=0;while(I<mLunCount && mLuns[I].Id!=Lun)++I;
  if(I==mLunCount || Lba>MAX_UINT32 || Bytes/mLuns[I].Block-1>MAX_UINT32-Lba)return EFI_UNSUPPORTED;
  EFI_TPL Old=gBS->RaiseTPL(TPL_CALLBACK);mBlockBusy=TRUE;EFI_STATUS Status=EFI_SUCCESS;
  for(UINTN Offset=0;Offset<Bytes;Offset+=mLuns[I].Block) {
    Status=ReadScsi("BLOCKIO_READ",mServiceTag++,Lun,PianoUfsReadLba10,(UINT32)(Lba+Offset/mLuns[I].Block),mLuns[I].Block);
    if(EFI_ERROR(Status) || mTransferred!=mLuns[I].Block){if(!EFI_ERROR(Status))Status=EFI_BAD_BUFFER_SIZE;break;}
    CopyMem((UINT8 *)Buffer+Offset,mData.Cpu,mLuns[I].Block);
    ++mIoReads[I];mIoBytes[I]+=mLuns[I].Block;
  }
  mBlockBusy=FALSE;gBS->RestoreTPL(Old);return Status;
}
STATIC EFI_STATUS HaltService(VOID) {
  if(mBlockLive) {
    // Re-emit compact final evidence: filesystem probes can wrap the bounded
    // console ring. This marks a report segment without erasing the capture.
    DEBUG((DEBUG_WARN,"SUNUEFI_BLOCKIO_REPORT_BEGIN\n"));
    ReportCapabilities();
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_BLOCKIO_PUBLISHED parents=%u all_block_handles=%u\n",(UINT32)mLunCount,(UINT32)mPublishedHandles));
    for(UINTN I=0;I<mLunCount;++I) {
      DEBUG((DEBUG_WARN,"SUNUEFI_UFS_BLOCKIO_TEST lun=%u read=%r gpt_signature=1 write=%r readonly=1 last=%lx block=%u reads=%lu bytes=%lu\n",
        mLuns[I].Id,mTests[I],mWriteTests[I],mLuns[I].Last,mLuns[I].Block,(UINT64)mIoReads[I],(UINT64)mIoBytes[I]));
      DEBUG((DEBUG_WARN,"SUNUEFI_UFS_GPT_VERIFIED lun=%u status=Success header_crc=%08x array_crc=%08x entry_lba=%lx array_bytes=%u entries=%u active_partitions=%u\n",
        mLuns[I].Id,mGpts[I].HeaderCrc,mGpts[I].ArrayCrc,mGpts[I].EntryLba,mGpts[I].ArrayBytes,mGpts[I].Entries,(UINT32)mPartitions[I]));
    }
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_GPT_MILESTONE luns=6 valid_gpts=6 " UFS_WRITE_EVIDENCE "\n" UFS_WRITE_EVIDENCE_ARG));
#ifdef PIANO_UFS_FILESYSTEMS
    PianoUfsReportFileSystems();
#endif
#ifdef PIANO_UFS_SHELL
    PianoReportShellDiagnostics();
#endif
#ifdef PIANO_UFS_SETUP
    PianoReportSetupDiagnostics();
#endif
  }
#ifdef PIANO_UFS_WRITE_TEST
  ReportWriteTransaction();
  if(mWriteWork.NeedsRecovery || mWriteResult.RequiresRecovery || mWriteResult.Quarantined){CpuDeadLoop();return EFI_ACCESS_DENIED;}
#endif
  mBlockLive=FALSE;for(UINTN I=0;I<mLunCount;++I)mBlocks[I].Media.MediaPresent=FALSE;
  if(!mInstalled)return EFI_SUCCESS;
  EFI_STATUS Status=Quiesce();
  if(!EFI_ERROR(Status)){Write(0x24,0);if(Read(0x24)!=0)Status=EFI_DEVICE_ERROR;}
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_BLOCKIO_HALT %r tr_dbr=%08x tm_dbr=%08x tr_run=%08x tm_run=%08x irq=%08x\n",
    Status,Read(0x58),Read(0x78),Read(0x60),Read(0x80),Read(0x24)));return Status;
}
STATIC PIANO_UFS_SHUTDOWN mShutdown={1,HaltService};
STATIC VOID EFIAPI PianoUfsExitBoot(EFI_EVENT Event,VOID *Context) {
  // Halt and retain already-Reserved memory only; no native HAL or Boot
  // Services calls, allocation/free, protocol teardown, or final-map mutation.
  if(EFI_ERROR(HaltService()))DmaFaultReset();
  if(!mInstalled)return;
  EFI_STATUS Status=PianoDmaRetainForExit(&mTrl);
  if(!EFI_ERROR(Status))Status=PianoDmaRetainForExit(&mUcd);
  if(!EFI_ERROR(Status))Status=PianoDmaRetainForExit(&mData);
  if(!EFI_ERROR(Status))Status=PianoOwnedSmmuRetainForExit(&mContext);
  if(EFI_ERROR(Status)){DmaFaultReset();return;}
  mExitRetained=TRUE;
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_EBS_RETAIN success=1 efi_map_required=1 raw_dtb_reservation_verified=0 os_handoff_verified=0\n"));
}
VOID PianoUfsBlockIoStop(VOID) {
#ifdef PIANO_UFS_WRITE_TEST
  if(mWriteWork.NeedsRecovery || mWriteResult.RequiresRecovery || mWriteResult.Quarantined){ReportWriteTransaction();CpuDeadLoop();return;}
#endif
  if(mExitRetained){DmaFaultReset();return;}
  // Normal teardown gives FAT/DiskIo/Partition their Stop callbacks while the
  // parent media is still usable. Timer/EBS recovery uses HaltService directly.
  for(UINTN I=0;I<mLunCount;++I)if(mBlockHandles[I]!=NULL) {
    EFI_STATUS Status=gBS->DisconnectController(mBlockHandles[I],NULL,NULL);
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_BLOCKIO_DISCONNECT lun=%u %r\n",mLuns[I].Id,Status));
    if(EFI_ERROR(Status)){DmaFaultReset();return;}
  }
  if(EFI_ERROR(HaltService())){DmaFaultReset();return;}
  if(mExitBootEvent!=NULL){gBS->CloseEvent(mExitBootEvent);mExitBootEvent=NULL;}
  for(UINTN I=0;I<mLunCount;++I)if(mBlockHandles[I]!=NULL) {
    EFI_STATUS Status=gBS->UninstallMultipleProtocolInterfaces(mBlockHandles[I],&gEfiBlockIoProtocolGuid,&mBlocks[I].Block,
      &gEfiDevicePathProtocolGuid,&mPaths[I],NULL);
    if(EFI_ERROR(Status)){DmaFaultReset();return;}
    mBlockHandles[I]=NULL;
  }
  if(mShutdownHandle!=NULL) {
    EFI_GUID Guid=PIANO_UFS_SHUTDOWN_GUID;
    EFI_STATUS Status=gBS->UninstallMultipleProtocolInterfaces(mShutdownHandle,&Guid,&mShutdown,NULL);
    if(EFI_ERROR(Status)){DmaFaultReset();return;}mShutdownHandle=NULL;
  }
  EFI_STATUS Status=Cleanup();if(EFI_ERROR(Status)){DmaFaultReset();return;}
  PianoFaultSetDiagnostic(NULL);PianoUfsStopClocks();
}
STATIC EFI_STATUS PublishBlocks(VOID) {
  mBlockLive=TRUE;mBlockBusy=FALSE;
  EFI_GUID Vendor={0xA8675600,0x87D0,0x4A29,{0x9B,0x40,0x60,0,0,0,0,1}};
  for(UINTN I=0;I<mLunCount;++I) {
    EFI_STATUS Status=PianoReadOnlyBlockInit(&mBlocks[I],mLuns[I].Id,mLuns[I].Block,mLuns[I].Last,NULL,ServiceRead);
    if(EFI_ERROR(Status))return Status;
    ZeroMem(&mPaths[I],sizeof(mPaths[I]));
    mPaths[I].Vendor.Header=(EFI_DEVICE_PATH_PROTOCOL){HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH),0}};
    mPaths[I].Vendor.Guid=Vendor;
    mPaths[I].Ufs.Header=(EFI_DEVICE_PATH_PROTOCOL){MESSAGING_DEVICE_PATH,MSG_UFS_DP,{sizeof(UFS_DEVICE_PATH),0}};
    mPaths[I].Ufs.Pun=0;mPaths[I].Ufs.Lun=mLuns[I].Id;
    mPaths[I].End=(EFI_DEVICE_PATH_PROTOCOL){END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};
    Status=gBS->InstallMultipleProtocolInterfaces(&mBlockHandles[I],&gEfiBlockIoProtocolGuid,&mBlocks[I].Block,
      &gEfiDevicePathProtocolGuid,&mPaths[I],NULL);if(EFI_ERROR(Status))return Status;
    EFI_BLOCK_IO_PROTOCOL *Block=NULL;Status=gBS->HandleProtocol(mBlockHandles[I],&gEfiBlockIoProtocolGuid,(VOID **)&Block);
    if(EFI_ERROR(Status) || Block==NULL)return EFI_DEVICE_ERROR;
    UINT8 Test[8192];
    if(!EFI_ERROR(Status))Status=Block->ReadBlocks(Block,Block->Media->MediaId,0,mLuns[I].Block*2,Test);
    BOOLEAN Signature=!EFI_ERROR(Status) && !CompareMem(Test+mLuns[I].Block,"EFI PART",8);
    EFI_STATUS WriteStatus=Block->WriteBlocks(Block,Block->Media->MediaId,0,mLuns[I].Block,Test);
    mTests[I]=Status;mWriteTests[I]=WriteStatus;
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_BLOCKIO_TEST lun=%u read=%r gpt_signature=%u write=%r readonly=%u last=%lx block=%u\n",
      mLuns[I].Id,Status,Signature,WriteStatus,Block->Media->ReadOnly,Block->Media->LastBlock,Block->Media->BlockSize));
    if(EFI_ERROR(Status) || !Signature || WriteStatus!=EFI_WRITE_PROTECTED)return EFI_DEVICE_ERROR;
  }
  EFI_GUID Guid=PIANO_UFS_SHUTDOWN_GUID;
  EFI_STATUS Status=gBS->InstallMultipleProtocolInterfaces(&mShutdownHandle,&Guid,&mShutdown,NULL);
  if(EFI_ERROR(Status))return Status;
  Status=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,PianoUfsExitBoot,NULL,&gEfiEventExitBootServicesGuid,&mExitBootEvent);
  if(EFI_ERROR(Status))return Status;
  PianoUfsRetainClocks();
  for(UINTN I=0;I<mLunCount;++I) {
    Status=gBS->ConnectController(mBlockHandles[I],NULL,NULL,TRUE);
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_BLOCKIO_CONNECT lun=%u %r\n",mLuns[I].Id,Status));
  }
  EFI_HANDLE *Handles=NULL;UINTN Count=0;
  Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiBlockIoProtocolGuid,NULL,&Count,&Handles);
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_BLOCKIO_PUBLISHED parents=%u all_block_handles=%u status=%r\n",(UINT32)mLunCount,(UINT32)Count,Status));
  mPublishedHandles=Count;
  if(Handles!=NULL)FreePool(Handles);
#ifdef PIANO_UFS_FILESYSTEMS
  PianoUfsProbeFileSystems(mBlockHandles,mLunCount);
#endif
  return EFI_SUCCESS;
}
#endif
EFI_STATUS PianoUfsReadOnlyDmaExperiment(CONST VOID *Fdt) {
  if(mExitRetained)return EFI_ACCESS_DENIED;
#ifdef PIANO_UFS_WRITE_TEST
  // Never zero the DMA/workspace or ledger on a second call in this image.
  if(mWriteProfileEntered){ReportWriteTransaction();return EFI_ACCESS_DENIED;}mWriteProfileEntered=TRUE;
#endif
  mDevice=(PIANO_DMA_DEVICE){.Name="ufs",.StreamId=0x60,.AddressBits=32,.CacheLine=64};
  ZeroMem(&mTrl,sizeof(mTrl));ZeroMem(&mUcd,sizeof(mUcd));ZeroMem(&mData,sizeof(mData));mInstalled=FALSE;mPowerMode=MAX_UINT32;mLunCount=0;
  ZeroMem(&mContext,sizeof(mContext));
  EFI_STATUS Status=LinkReady();DEBUG((DEBUG_WARN,"SUNUEFI_UFS_LINK_READY %r\n",Status));
  if(EFI_ERROR(Status))goto Exit;
  Status=PianoOwnedSmmuOpen(Fdt,&mContext,&mDevice);
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_NOP_SMMU_OPEN %r\n",Status));
  if(EFI_ERROR(Status))goto Exit;
  PianoFaultSetDiagnostic(FaultDiagnostic);
  Status=PianoDmaAllocate(&mDevice,1024,1024,32,PianoDmaBidirectional,&mTrl);if(EFI_ERROR(Status))goto Exit;
  Status=PianoDmaAllocate(&mDevice,PIANO_UFS_UCD_BYTES,128,32,PianoDmaBidirectional,&mUcd);if(EFI_ERROR(Status))goto Exit;
  Status=PianoDmaMap(&mTrl);if(EFI_ERROR(Status))goto Exit;
  Status=PianoDmaMap(&mUcd);if(EFI_ERROR(Status))goto Exit;
  // Replace only an idle transfer list. Preserve the inherited link/PHY/HCE.
  if(Read(0x58) || Read(0x78)){Status=EFI_NOT_READY;goto Exit;}
  mSavedBase=Read(0x50);mSavedUpper=Read(0x54);mSavedRun=Read(0x60);mSavedTaskRun=Read(0x80);mSavedInterrupt=Read(0x24);
  Write(0x60,0);Write(0x24,0);Write(0x50,(UINT32)mTrl.DeviceAddress);Write(0x54,(UINT32)(mTrl.DeviceAddress>>32));
  mInstalled=TRUE;
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_DMA_LAYOUT trl_pa=%lx trl_iova=%lx ucd_pa=%lx ucd_iova=%lx utrd_bytes=32 response_offset=%u\n",
    mTrl.Physical,mTrl.DeviceAddress,mUcd.Physical,mUcd.DeviceAddress,PIANO_UFS_RESPONSE_OFFSET));
  Status=PianoUfsBuildNop(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,1);
  if(!EFI_ERROR(Status))Status=Submit("NOP",1,TRUE);
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_NOP_RESULT %r\n",Status));if(EFI_ERROR(Status))goto Exit;
  // Like the kernel's descriptor query path, allow three bounded read retries.
  // Never retry a timed-out or malformed transport, only a completed general
  // query failure. Each retry uses a fresh tag and cleaned command buffers.
  for(UINT8 Attempt=0;Attempt<3;++Attempt) {
    UINT8 Tag=2+Attempt;
    Status=PianoUfsBuildReadDescriptor(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,Tag,0,0,2);
    if(!EFI_ERROR(Status))Status=Submit("QUERY_READ_DEVICE_DESCRIPTOR",Tag,FALSE);
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_QUERY_ATTEMPT attempt=%u status=%r\n",Attempt+1,Status));
    UINT8 *R=(UINT8 *)mUcd.Cpu+PIANO_UFS_RESPONSE_OFFSET;
    if(Status!=EFI_DEVICE_ERROR || (EngineLe32((UINT8 *)mTrl.Cpu+8)&0xFF)!=0 ||
       (R[0]&0x3F)!=0x36 || R[3]!=Tag || R[6]!=0xFF)break;
    gBS->Stall(10000);
  }
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_QUERY_RESULT %r\n",Status));
  if(!EFI_ERROR(Status)) {
    UINT8 DescriptorBytes=((UINT8 *)mUcd.Cpu)[PIANO_UFS_RESPONSE_OFFSET+32];
    Status=PianoUfsBuildReadDescriptor(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,5,0,0,DescriptorBytes);
    if(!EFI_ERROR(Status))Status=Submit("QUERY_READ_FULL_DEVICE_DESCRIPTOR",5,FALSE);
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_QUERY_FULL_RESULT %r\n",Status));
  }
  if(Status==EFI_DEVICE_ERROR) {
    EFI_STATUS Diagnostic=PianoUfsBuildReadPowerMode(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,6);
    if(!EFI_ERROR(Diagnostic))Diagnostic=Submit("QUERY_READ_CURRENT_POWER_MODE",6,FALSE);
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_POWER_DIAGNOSTIC %r descriptor_stage_failed=1\n",Diagnostic));
    if(!EFI_ERROR(Diagnostic) && (mPowerMode==0x22 || mPowerMode==0x33)) {
      Status=PianoUfsBuildResumeActive(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,7);
      if(!EFI_ERROR(Status))Status=Submit("RESUME_DEVICE_ACTIVE_NO_DATA",7,FALSE);
      DEBUG((DEBUG_WARN,"SUNUEFI_UFS_RESUME_ACTIVE_RESULT %r physical_ufs_writes=0\n",Status));
      if(!EFI_ERROR(Status)) {
        Status=PianoUfsBuildReadPowerMode(mTrl.Cpu,mTrl.Bytes,mUcd.Cpu,mUcd.Bytes,mUcd.DeviceAddress,8);
        if(!EFI_ERROR(Status))Status=Submit("QUERY_READ_CURRENT_POWER_MODE",8,FALSE);
        if(!EFI_ERROR(Status) && mPowerMode!=0x11 && mPowerMode!=0)Status=EFI_NOT_READY;
      }
      if(!EFI_ERROR(Status))Status=QueryDescriptor(9,2);
      if(!EFI_ERROR(Status))Status=QueryDescriptor(10,((UINT8 *)mUcd.Cpu)[PIANO_UFS_RESPONSE_OFFSET+32]);
      DEBUG((DEBUG_WARN,"SUNUEFI_UFS_QUERY_AFTER_RESUME %r\n",Status));
    }
  }
  if(!EFI_ERROR(Status))Status=ReadLunCapacity();
  if(!EFI_ERROR(Status))Status=ReadGpts();
  if(!EFI_ERROR(Status))ReadCapabilities();
#ifdef PIANO_UFS_BLOCKIO
  if(!EFI_ERROR(Status))Status=PublishBlocks();
  if(!EFI_ERROR(Status)) {
#ifdef PIANO_UFS_WRITE_TEST
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_BLOCKIO_READY persistent=1 readonly_protocols=1\n"));
    RunWriteTransaction();
#else
    DEBUG((DEBUG_WARN,"SUNUEFI_UFS_BLOCKIO_READY persistent=1 writes=0\n"));
#endif
    return Status;
  }
  PianoUfsBlockIoStop();return Status;
#endif
Exit:
  {EFI_STATUS S=Cleanup();if(EFI_ERROR(S))Status=S;}
  PianoFaultSetDiagnostic(NULL);
  DEBUG((DEBUG_WARN,"SUNUEFI_UFS_READONLY_DMA_END status=%r physical_ufs_writes=0\n",Status));return Status;
}
