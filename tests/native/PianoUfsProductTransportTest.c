// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual Submit/shared adapter + actual provider; simulated HCI/storage only.
#include <stdlib.h>
#define main offline_provider_fixture_suite
#include "PianoUfsProductVolumeTest.c"
#undef main
#define PIANO_UFS_BLOCKIO 1
#define PIANO_UFS_PRODUCT_STORAGE 1
#include "../../uefi/core/storage/PianoUfsDmaLayout.c"
#include "../../uefi/core/storage/PianoUfsReadOnlyDma.c"
EFI_BOOT_SERVICES *gBS;EFI_RUNTIME_SERVICES *gRT;
EFI_GUID gEfiBlockIoProtocolGuid,gEfiDevicePathProtocolGuid,gEfiPartitionInfoProtocolGuid;
EFI_GUID gEfiSimpleFileSystemProtocolGuid,gEfiDiskIoProtocolGuid;
static EFI_BOOT_SERVICES Bs;static EFI_RUNTIME_SERVICES Rt;
#include <stdarg.h>
static UINT8 Trl[1024],Ucd[1024],Data[PIANO_UFS_MAX_TRANSFER_BYTES];static UINT32 Regs[0x400/4];
static PIANO_SMMU_SNAPSHOT Hardware;
static EFI_TPL CurrentTpl;static UINTN CaptureCalls,Doorbells,DmaBegins,DmaCompletes,Allocations,Installs,Connects,NativeWrites,NativeSyncs;
static UINTN Freed,Disconnected,Uninstalled,ClockStops;
static UINTN NativeReads,LastReadBytes,LastWriteBytes,LargestRead,LargestWrite;
static UINTN CorruptCapture;static UINT64 CorruptPage;
static BOOLEAN TranslateBad,PartialWrite,PartialRead,WrongReadLun,RetainOnRead,ResetAttempted;
static UINT64 FakeUs,ReadDelay,WriteDelay,SyncDelay,DoorbellReadyUs;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){(void)Level;return FALSE;}VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){(void)Level;(void)Format;}
VOID EFIAPI MemoryFence(VOID){}VOID EFIAPI CpuPause(VOID){}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
INTN EFIAPI StrnCmp(CONST CHAR16 *A,CONST CHAR16 *B,UINTN N){while(N--){if(*A!=*B)return *A<*B?-1:1;if(!*A)return 0;++A;++B;}return 0;}
INTN EFIAPI StrCmp(CONST CHAR16 *A,CONST CHAR16 *B){while(*A&&*A==*B){++A;++B;}return *A==*B?0:*A<*B?-1:1;}
BOOLEAN EFIAPI CompareGuid(CONST GUID *A,CONST GUID *B){return memcmp(A,B,sizeof(*A))==0;}
UINTN EFIAPI DevicePathNodeLength(CONST VOID *Node){CONST EFI_DEVICE_PATH_PROTOCOL *P=Node;return P->Length[0]|((UINTN)P->Length[1]<<8);}
VOID EFIAPI CpuDeadLoop(VOID){assert(!"Unexpected fail-stop in successful simulated source path");}
VOID PianoFaultSetDiagnostic(VOID (*F)(VOID)){(void)F;}
VOID PianoSmmuLogFaults(CONST PIANO_SMMU_SNAPSHOT *S){(void)S;}
static EFI_TPL EFIAPI raise_tpl(EFI_TPL New){assert(New>=CurrentTpl);EFI_TPL Old=CurrentTpl;CurrentTpl=New;return Old;}
static VOID EFIAPI restore_tpl(EFI_TPL Old){assert(Old<=CurrentTpl);CurrentTpl=Old;}
static EFI_STATUS EFIAPI stall(UINTN Us){assert(Us==10 || Us==100);FakeUs+=Us;return EFI_SUCCESS;}
UINT64 EFIAPI GetPerformanceCounter(VOID){return FakeUs;}
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 Ticks){return Ticks*1000;}
static VOID EFIAPI reset(EFI_RESET_TYPE Type,EFI_STATUS Status,UINTN Bytes,VOID *Arg){(void)Type;(void)Status;(void)Bytes;(void)Arg;ResetAttempted=TRUE;assert(!"Product backend may not reset");}
EFI_STATUS PianoDmaAllocate(PIANO_DMA_DEVICE *D,UINTN Bytes,UINTN Align,UINT8 Bits,PIANO_DMA_DIRECTION Direction,PIANO_DMA_BUFFER *Buffer){(void)D;(void)Bytes;(void)Align;(void)Bits;(void)Direction;(void)Buffer;++Allocations;assert(!"Shared adapter must not allocate DMA");return EFI_UNSUPPORTED;}
EFI_STATUS PianoDmaBegin(PIANO_DMA_BUFFER *B,CONST CHAR8 *Name){(void)Name;assert(!B->Active && !B->Quarantined);B->Active=TRUE;++DmaBegins;return EFI_SUCCESS;}
EFI_STATUS PianoDmaComplete(PIANO_DMA_BUFFER *B,EFI_STATUS Status,BOOLEAN Quiet){assert(B->Active && Quiet && !Regs[0x58/4]);B->Active=FALSE;++DmaCompletes;return Status;}
EFI_STATUS PianoIoPageTableTranslate(PIANO_IO_PAGE_TABLE *T,UINT64 Iova,BOOLEAN Write,UINT64 *Physical){
  assert(T==&mContext.PageTable && Write);
  *Physical=0x81000000+Iova-0x40000000+((TranslateBad || Iova==CorruptPage)?4096:0);
  return EFI_SUCCESS;
}
EFI_STATUS PianoSmmuCapture(CONST VOID *Fdt,CONST CHAR8 *Phase,PIANO_SMMU_SNAPSHOT *Snapshot){assert(Fdt==(VOID *)0x123 && !strcmp(Phase,"product-volume-lease"));++CaptureCalls;*Snapshot=Hardware;if(CaptureCalls==CorruptCapture)Snapshot->RawS2cr[3]^=1;return EFI_SUCCESS;}
EFI_STATUS PianoSmmuRefreshSnapshot(CONST PIANO_SMMU_SNAPSHOT *Registered,PIANO_SMMU_SNAPSHOT *Snapshot){
  assert(Registered==&mContext.AttachedSnapshot && Registered->Valid);
  return PianoSmmuCapture(mContext.Fdt,"product-volume-lease",Snapshot);
}
UINT32 EFIAPI MmioRead32(UINTN Address){
  assert(Address>=HCI && Address<HCI+sizeof(Regs) && !(Address&3));
  UINTN Offset=Address-HCI;
  if(Offset==0x58 && DoorbellReadyUs && FakeUs>=DoorbellReadyUs){
    Regs[0x58/4]=0;DoorbellReadyUs=0;
  }
  return Regs[Offset/4];
}
static VOID physical_read(UINT32 Lba,UINT8 *Destination){
  if(Lba==1)memcpy(Destination,primary,4096);
  else if(Lba>=2 && Lba<=4)memcpy(Destination,entries+(Lba-2)*4096,4096);
  else if(Lba==378879)memcpy(Destination,backup,4096);
  else if(Lba>=378873 && Lba<=378875)memcpy(Destination,backup_entries+(Lba-378873)*4096,4096);
  else{assert(Lba>=375040 && Lba<=378623);memcpy(Destination,disk+(Lba-375040)*4096,4096);}
}
static UINTN block_command_bytes(VOID){
  UINTN Blocks=(UINT16)(Ucd[23]<<8)|Ucd[24];
  UINT32 Bytes=ReadBe32(Ucd+12);
  assert(Blocks && Bytes==Blocks*4096 && Bytes<=sizeof(Data));
  UINT32 Dwords=ReadLe32(Trl+28),Remaining=Bytes;
  assert((Dwords>>16)==64 && (Dwords&0xffff)<=4);
  for(UINTN I=0;I<(Dwords&0xffff);++I){
    CONST UINT8 *Entry=Ucd+256+16*I;
    UINT32 Chunk=ReadLe32(Entry+12)+1;
    assert(Chunk==MIN(Remaining,PIANO_UFS_PRDT_MAX_DATA_BYTES) &&
      ReadLe32(Entry)==mData.DeviceAddress+Bytes-Remaining &&
      !ReadLe32(Entry+4) && !ReadLe32(Entry+8));
    Remaining-=Chunk;
  }
  assert(!Remaining);
  assert(mData.Active && mUcd.Active && mTrl.Active);
  return Bytes;
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value){
  assert(Address>=HCI && Address<HCI+sizeof(Regs) && !(Address&3));UINTN Off=Address-HCI;
  if(Off==0x20){Regs[Off/4]&=~Value;return Value;}
  if(Off==0x5c){Regs[0x58/4]&=Value;return Value;}
  if(Off==0x7c){Regs[0x78/4]&=Value;return Value;}
  Regs[Off/4]=Value;if(Off!=0x58)return Value;
  assert(Value==1 && Regs[0x60/4]==1 && CurrentTpl>=TPL_CALLBACK && mBlockBusy);
  assert(mProductExecuting || (Ucd[0]==1 && Ucd[16]==0x28));
  ++Doorbells;
  UINT8 *R=Ucd+64;Le32(Trl+8,0);R[0]=Ucd[0]==1?0x21:0x36;R[2]=Ucd[2];R[3]=Ucd[3];
  if(Ucd[0]==0x16){
    if(Ucd[12]==5){R[12]=5;R[13]=Ucd[13];Be32(R+20,0);}
    else{assert(Ucd[12]==1 && Ucd[13]==2 && Ucd[14]==4);R[12]=1;R[13]=2;R[14]=4;R[11]=32;R[32]=32;R[33]=2;R[34]=4;R[37]=1;}
  }else{
    assert(Ucd[0]==1 && Ucd[2]==4);
    if(Ucd[16]==0x9e){Be32(Data,0);Be32(Data+4,378879);Be32(Data+8,4096);}
    else if(Ucd[16]==0x5a){Data[1]=26;Data[3]=0x10;Data[8]=8;Data[9]=0x12;Data[10]=4;R[1]=0x20;Be32(R+12,4096-28);}
    else if(Ucd[16]==0x28){
      UINT32 Lba=ReadBe32(Ucd+18);
      LastReadBytes=block_command_bytes();
      LargestRead=MAX(LargestRead,LastReadBytes);
      ++NativeReads;
      for(UINTN Offset=0;Offset<LastReadBytes;Offset+=4096)physical_read(Lba+(UINT32)(Offset/4096),Data+Offset);
      if(PartialRead){R[1]=0x20;Be32(R+12,4096);}
      if(WrongReadLun)++R[2];
      if(RetainOnRead){
        mData.ExitRetained=mUcd.ExitRetained=mTrl.ExitRetained=TRUE;
        mExitRetained=TRUE;
      }
    }else if(Ucd[16]==0x2a){
      UINT32 Lba=ReadBe32(Ucd+18);
      LastWriteBytes=block_command_bytes();
      LargestWrite=MAX(LargestWrite,LastWriteBytes);
      assert(Lba>=375042 && Lba<=378623 && LastWriteBytes/4096<=378624-Lba && Ucd[17]==8);
      assert(mPartitionExecuting || (LastWriteBytes==4096 && volume.State.PendingWrite));
      ++NativeWrites;memcpy(disk+(Lba-375040)*4096,Data,LastWriteBytes);
      if(PartialWrite){R[1]=0x20;Be32(R+12,1);}
    }else{
      assert(Ucd[16]==0x35);
      if(mPartitionExecuting)assert(!ReadBe32(Ucd+18) && !Ucd[23] && !Ucd[24]);
      else assert(ReadBe32(Ucd+18)==375040 && Ucd[23]==14 && Ucd[24]==0);
      ++NativeSyncs;
    }
  }
  UINT64 Delay=Ucd[0]!=1?0:Ucd[16]==0x28?ReadDelay:Ucd[16]==0x2a?WriteDelay:Ucd[16]==0x35?SyncDelay:0;
  if(Delay)DoorbellReadyUs=FakeUs+Delay;
  else Regs[0x58/4]=0;
  return Value;
}
static EFI_STATUS EFIAPI install(EFI_HANDLE *Handle,...){assert(CurrentTpl==TPL_APPLICATION && Handle==&mBlockHandles[7] && !*Handle && volume.State.Provisioned && !volume.Media.ReadOnly);++Installs;*Handle=(VOID *)0x888;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI connect(EFI_HANDLE Handle,EFI_HANDLE *Drivers,EFI_DEVICE_PATH_PROTOCOL *Path,BOOLEAN Recursive){(void)Drivers;(void)Path;assert(Handle==(VOID *)0x888 && Recursive && CurrentTpl==TPL_APPLICATION);++Connects;return EFI_NOT_FOUND;}
EFI_STATUS PianoDmaFree(PIANO_DMA_BUFFER *B){assert(CurrentTpl==TPL_CALLBACK && !B->Active && !B->Quarantined && !Regs[0x58/4] && !volume.Media.MediaPresent);++Freed;memset(B,0,sizeof(*B));return EFI_SUCCESS;}
EFI_STATUS PianoOwnedSmmuClose(PIANO_OWNED_SMMU *C){assert(C==&mContext && Freed==3 && !volume.Media.MediaPresent);C->Attached=C->Verified=FALSE;C->Domain=NULL;C->TableMemory.Signature=0;return EFI_SUCCESS;}
EFI_STATUS PianoUfsStopClocksForReset(VOID){assert(Freed==3 && !mContext.Domain && !mProductPublished && CurrentTpl==TPL_CALLBACK);++ClockStops;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI disconnect(EFI_HANDLE Handle,EFI_HANDLE Driver,EFI_HANDLE Child){(void)Driver;(void)Child;assert(CurrentTpl==TPL_CALLBACK && !Freed && !volume.State.Closed);++Disconnected;
  if(Handle==(VOID *)0x888)assert(volume.Block.FlushBlocks(&volume.Block)==EFI_SUCCESS);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI uninstall(EFI_HANDLE Handle,...){assert(CurrentTpl==TPL_CALLBACK && Freed==3 && !mContext.Domain);++Uninstalled;
  va_list A;va_start(A,Handle);EFI_GUID *Guid=va_arg(A,EFI_GUID *);VOID *Interface=va_arg(A,VOID *);
  if(Handle==(VOID *)0x888){assert(Guid==&gEfiBlockIoProtocolGuid && Interface==&volume.Block);assert(va_arg(A,EFI_GUID *)==&gEfiDevicePathProtocolGuid && va_arg(A,VOID *)==&mProductPath && va_arg(A,VOID *)==NULL);}
  va_end(A);return EFI_SUCCESS;}
static VOID native_fresh(BOOLEAN Provisioned){
  fresh(Provisioned);memset(Regs,0,sizeof(Regs));memset(Trl,0,sizeof(Trl));memset(Ucd,0,sizeof(Ucd));memset(Data,0,sizeof(Data));memset(&mContext,0,sizeof(mContext));memset(&Hardware,0,sizeof(Hardware));memset(&mResetReport,0,sizeof(mResetReport));memset(mBlockHandles,0,sizeof(mBlockHandles));
  CaptureCalls=Doorbells=DmaBegins=DmaCompletes=Allocations=Installs=Connects=NativeWrites=NativeSyncs=Freed=Disconnected=Uninstalled=ClockStops=0;CorruptCapture=0;TranslateBad=PartialWrite=ResetAttempted=FALSE;CurrentTpl=TPL_APPLICATION;
  NativeReads=LastReadBytes=LastWriteBytes=LargestRead=LargestWrite=0;
  CorruptPage=0;PartialRead=WrongReadLun=RetainOnRead=mPartitionExecuting=mPartitionFailed=FALSE;
  FakeUs=ReadDelay=WriteDelay=SyncDelay=DoorbellReadyUs=0;
  mStorageIoTiming=NULL;
  mProductVolume=NULL;mProductExecuting=mProductRetained=mProductPublished=mBlockBusy=mExitRetained=FALSE;mBlockLive=mInstalled=mEverInstalled=TRUE;mLunCount=6;mShutdownHandle=mExitBootEvent=NULL;mSavedBase=0x12345000;mSavedUpper=0;
  mDevice=(PIANO_DMA_DEVICE){.Name="ufs",.Context=&mContext,.StreamId=0x60};
  mTrl=(PIANO_DMA_BUFFER){.Signature=1,.Device=&mDevice,.Cpu=Trl,.Bytes=1024,.Physical=0x81000000,.DeviceAddress=0x40000000,.Mapped=TRUE,.Direction=PianoDmaBidirectional};
  mUcd=(PIANO_DMA_BUFFER){.Signature=1,.Device=&mDevice,.Cpu=Ucd,.Bytes=1024,.Physical=0x81001000,.DeviceAddress=0x40001000,.Mapped=TRUE,.Direction=PianoDmaBidirectional};
  mData=(PIANO_DMA_BUFFER){.Signature=1,.Device=&mDevice,.Cpu=Data,.Bytes=sizeof(Data),.Physical=0x81002000,.DeviceAddress=0x40002000,.Mapped=TRUE,.Direction=PianoDmaBidirectional};
  PIANO_DMA_BUFFER *Buffers[]={&mTrl,&mUcd,&mData};for(UINTN I=0;I<3;++I){Buffers[I]->ReservedBytes=ALIGN_VALUE(Buffers[I]->Bytes,4096);Buffers[I]->Alignment=4096;Buffers[I]->MemoryType=EfiReservedMemoryType;Buffers[I]->MemoryAttributes=EFI_MEMORY_WB;}
  Regs[0x50/4]=(UINT32)mTrl.DeviceAddress;Regs[0x34/4]=1;Regs[0x30/4]=15;
  Hardware=(PIANO_SMMU_SNAPSHOT){.Valid=TRUE,.Base=0x15000000,.Window=0x100000,.ContextBase=0x80000,.PageShift=12,.Groups=4,.Banks=2};
  Hardware.Device[0]=(PIANO_SMMU_DEVICE){.Present=TRUE,.Enabled=TRUE,.Sid=0x60,.Smr=0x80000060,.Sctlr=0x1e5,.Ttbr0=0x81200000};Hardware.RawSmr[0]=Hardware.Device[0].Smr;
  Hardware.Device[1]=(PIANO_SMMU_DEVICE){.Present=TRUE,.Enabled=TRUE,.Sid=0x40,.StreamIndex=1,.ContextBank=1,.Smr=0x80000040,.S2cr=1,.Sctlr=0x1e5,.Ttbr0=0x81210000};Hardware.RawSmr[1]=Hardware.Device[1].Smr;Hardware.RawS2cr[1]=1;
  mContext.Attached=mContext.Verified=TRUE;mContext.Domain=(VOID *)0x777;mContext.Fdt=(VOID *)0x123;mContext.TableMemory.Signature=1;mContext.TableMemory.Physical=0x81200000;mContext.AttachedSnapshot=Hardware;
  mLuns[4].Id=4;mLuns[4].Last=378879;mLuns[4].Block=4096;
  mPermanentWpStatus=mPowerOnWpStatus=mCapabilities[4].CacheStatus=mCapabilities[4].UnitStatus=EFI_SUCCESS;
  mPermanentWp=mPowerOnWp=mCapabilities[4].WriteProtected=FALSE;
  mCapabilities[4].Fua=TRUE;mCapabilities[4].UnitWriteProtect=0;
  for(UINTN I=0;I<6;++I)mBlocks[I].Media.ReadOnly=TRUE;
  memset(&Bs,0,sizeof(Bs));Bs.RaiseTPL=raise_tpl;Bs.RestoreTPL=restore_tpl;Bs.Stall=stall;Bs.InstallMultipleProtocolInterfaces=install;Bs.ConnectController=connect;Bs.DisconnectController=disconnect;Bs.UninstallMultipleProtocolInterfaces=uninstall;gBS=&Bs;Rt.ResetSystem=reset;gRT=&Rt;
  assert(PianoUfsProductTransportIo(&volume,&io)==EFI_SUCCESS);
}
static VOID native_open(void){assert(PianoUfsProductVolumeOpen(&volume,&io)==EFI_SUCCESS && !NativeWrites && !NativeSyncs && !Allocations && CurrentTpl==TPL_APPLICATION && DmaBegins==DmaCompletes);}
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL TestFilesystem;
static EFI_HANDLE TargetMount=(VOID *)0x1234,OtherMount=(VOID *)0x1235,FilesystemDriver=(VOID *)0xabcd;
static BOOLEAN TargetMounted,TargetBound,TargetBusy,OtherMounted,MarkerInstalled;
static UINTN MarkerInstalls,MarkerRemovals,Unmounts;
static EFI_STATUS EFIAPI fs_handle(EFI_HANDLE Handle,EFI_GUID *Guid,VOID **Interface){
  assert(Guid==&gEfiSimpleFileSystemProtocolGuid && (Handle==TargetMount || Handle==OtherMount));
  *Interface=NULL;
  if(!(Handle==TargetMount?TargetMounted:OtherMounted))return EFI_UNSUPPORTED;
  *Interface=&TestFilesystem;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fs_open_info(EFI_HANDLE Handle,EFI_GUID *Guid,
                                     EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **Entries,UINTN *Count){
  assert(Handle==TargetMount && Guid==&gEfiDiskIoProtocolGuid && CurrentTpl==TPL_APPLICATION);
  *Entries=NULL;*Count=0;
  if(!TargetBound)return EFI_SUCCESS;
  *Entries=malloc(sizeof(**Entries));assert(*Entries);
  **Entries=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){
    .AgentHandle=FilesystemDriver,.ControllerHandle=TargetMount,.Attributes=EFI_OPEN_PROTOCOL_BY_DRIVER,.OpenCount=1
  };
  *Count=1;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fs_free(VOID *Buffer){free(Buffer);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI fs_mark(EFI_HANDLE *Handle,EFI_GUID *Guid,EFI_INTERFACE_TYPE Type,VOID *Interface){
  assert(*Handle==TargetMount && Guid==&mRawPartitionWriteGuid && Type==EFI_NATIVE_INTERFACE && !Interface && !MarkerInstalled);
  ++MarkerInstalls;MarkerInstalled=TRUE;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fs_unmark(EFI_HANDLE Handle,EFI_GUID *Guid,VOID *Interface){
  assert(Handle==TargetMount && Guid==&mRawPartitionWriteGuid && !Interface && MarkerInstalled);
  ++MarkerRemovals;MarkerInstalled=FALSE;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI fs_disconnect(EFI_HANDLE Handle,EFI_HANDLE Driver,EFI_HANDLE Child){
  assert(CurrentTpl==TPL_APPLICATION && Handle==TargetMount && Driver==FilesystemDriver && !Child &&
    TargetMounted && TargetBound && MarkerInstalled);
  if(TargetBusy)return EFI_ACCESS_DENIED;
  ++Unmounts;TargetMounted=TargetBound=FALSE;return EFI_SUCCESS;
}
static VOID filesystem_write_preparation(void){
  native_fresh(FALSE);
  Bs.HandleProtocol=fs_handle;Bs.OpenProtocolInformation=fs_open_info;Bs.FreePool=fs_free;
  Bs.InstallProtocolInterface=fs_mark;Bs.UninstallProtocolInterface=fs_unmark;Bs.DisconnectController=fs_disconnect;
  TargetMounted=TargetBound=TargetBusy=OtherMounted=TRUE;
  assert(PianoStorageCheckFilesystemClosed(TargetMount)==EFI_ACCESS_DENIED);
  assert(PianoStorageCloseFilesystemForWrite(TargetMount)==EFI_ACCESS_DENIED && TargetMounted && TargetBound &&
    OtherMounted && !Unmounts && !MarkerInstalled && MarkerInstalls==1 && MarkerRemovals==1);
  TargetBusy=FALSE;
  assert(PianoStorageCloseFilesystemForWrite(TargetMount)==EFI_SUCCESS && !TargetMounted && !TargetBound &&
    OtherMounted && Unmounts==1 && !MarkerInstalled && MarkerInstalls==2 && MarkerRemovals==2);
  assert(PianoStorageCheckFilesystemClosed(TargetMount)==EFI_SUCCESS);
  TargetBound=TRUE; // A failed mount can hold DiskIo without publishing SFS.
  assert(PianoStorageCheckFilesystemClosed(TargetMount)==EFI_ACCESS_DENIED);
  assert(PianoStorageCloseFilesystemForWrite(TargetMount)==EFI_ACCESS_DENIED && Unmounts==1);
  CurrentTpl=TPL_CALLBACK;
  assert(PianoStorageCloseFilesystemForWrite(TargetMount)==EFI_UNSUPPORTED && Unmounts==1);
  assert(OtherMounted && !NativeWrites && !NativeSyncs && !Doorbells);
}
static VOID partition_batches(VOID){
  const UINT32 Batch=PIANO_UFS_MAX_TRANSFER_BYTES;
  static UINT8 Payload[PIANO_UFS_MAX_TRANSFER_BYTES+4096],Readback[sizeof(Payload)];
  PIANO_STORAGE_PARTITION Range={.Lun=4,.BlockBytes=4096,.First=375168,.Blocks=sizeof(Payload)/4096};
  for(UINTN Index=0;Index<sizeof(Payload);++Index)Payload[Index]=(UINT8)(Index*31+Index/251);
  native_fresh(TRUE);
  PIANO_STORAGE_IO_TIMING Timing={0};
  ReadDelay=700;WriteDelay=1300;SyncDelay=900;
  assert(PianoStorageSetIoTiming(&Timing)==NULL);
  assert(StorageTransfer(&Range,1,sizeof(Payload),Payload,TRUE)==EFI_INVALID_PARAMETER && !Doorbells);
  assert(ServiceRead(NULL,4,mLuns[4].Last-1,Batch,Readback)==EFI_INVALID_PARAMETER && !Doorbells);
  assert(StorageTransfer(&Range,0,sizeof(Payload),Payload,TRUE)==EFI_SUCCESS);
  assert(Doorbells==2 && NativeWrites==2 && LargestWrite==Batch && LastWriteBytes==4096);
  assert(ServiceRead(NULL,4,Range.First,sizeof(Readback),Readback)==EFI_SUCCESS && !memcmp(Readback,Payload,sizeof(Payload)));
  assert(Doorbells==4 && NativeReads==2 && LargestRead==Batch && LastReadBytes==4096);
  assert(StorageFlush(&Range)==EFI_SUCCESS && Doorbells==5 && NativeSyncs==1);
  assert(PianoStorageSetIoTiming(NULL)==&Timing);
  assert(Timing.Write.Submitted==2 && Timing.Write.Completed==2 && Timing.Write.WaitUs==2*WriteDelay);
  assert(Timing.Read.Submitted==2 && Timing.Read.Completed==2 && Timing.Read.WaitUs==2*ReadDelay);
  assert(Timing.Sync.Submitted==1 && Timing.Sync.Completed==1 && Timing.Sync.WaitUs==SyncDelay);
  assert(DmaBegins==DmaCompletes && !Allocations && !mBlockBusy && CurrentTpl==TPL_APPLICATION);

  native_fresh(TRUE);
  mBlockBusy=mPartitionExecuting=mProductExecuting=TRUE;
  mPartitionLun=Range.Lun;mPartitionFirst=Range.First;mPartitionBlocks=Batch/4096;
  assert(BuildPartitionWrite(Trl,sizeof(Trl),Ucd,sizeof(Ucd),1,4,(UINT32)Range.First+1,Batch)==EFI_SUCCESS);
  assert(Submit("PARTITION_WRITE_FUA",1,FALSE)==EFI_ACCESS_DENIED && !Doorbells);
  assert(BuildPartitionWrite(Trl,sizeof(Trl),Ucd,sizeof(Ucd),1,3,(UINT32)Range.First,Batch)==EFI_SUCCESS);
  assert(Submit("PARTITION_WRITE_FUA",1,FALSE)==EFI_ACCESS_DENIED && !Doorbells);
  assert(BuildPartitionWrite(Trl,sizeof(Trl),Ucd,sizeof(Ucd),1,4,(UINT32)Range.First,Batch)==EFI_SUCCESS);
  --Ucd[24]; // The CDB count must match the entire PRDT and write plan.
  assert(Submit("PARTITION_WRITE_FUA",1,FALSE)==EFI_ACCESS_DENIED && !Doorbells);
  ++Ucd[24];++Ucd[284];
  assert(Submit("PARTITION_WRITE_FUA",1,FALSE)==EFI_ACCESS_DENIED && !Doorbells);
  assert(BuildPartitionWrite(Trl,sizeof(Trl),Ucd,sizeof(Ucd),1,4,(UINT32)Range.First,Batch+4096)==EFI_INVALID_PARAMETER);
  assert(!DmaBegins && !Allocations);

  native_fresh(TRUE);mBlockBusy=TRUE;
  assert(PianoUfsBuildReadCommand(Trl,sizeof(Trl),Ucd,sizeof(Ucd),mUcd.DeviceAddress,mData.DeviceAddress,Batch,
    1,4,PianoUfsReadLba10,(UINT32)Range.First,Batch/4096)==EFI_SUCCESS);
  ++Ucd[288];
  assert(Submit("BLOCKIO_READ",1,FALSE)==EFI_COMPROMISED_DATA && !Doorbells && !DmaBegins);

  native_fresh(TRUE);CorruptPage=mData.DeviceAddress+PIANO_UFS_PRDT_MAX_DATA_BYTES+8*4096;
  assert(StorageTransfer(&Range,0,Batch,Payload,TRUE)==EFI_COMPROMISED_DATA);
  assert(mProductRetained && !Doorbells && !DmaBegins);
  native_fresh(TRUE);CorruptPage=mData.DeviceAddress+2*PIANO_UFS_PRDT_MAX_DATA_BYTES+8*4096;
  assert(ServiceRead(NULL,4,Range.First,Batch,Readback)==EFI_COMPROMISED_DATA);
  assert(mProductRetained && !Doorbells && !DmaBegins);

  native_fresh(TRUE);PartialWrite=TRUE;
  assert(StorageTransfer(&Range,0,sizeof(Payload),Payload,TRUE)==EFI_DEVICE_ERROR);
  assert(mPartitionFailed && NativeWrites==1 && LastWriteBytes==Batch && DmaBegins==DmaCompletes);
  assert(StorageTransfer(&Range,0,Batch,Payload,TRUE)==EFI_NOT_READY && NativeWrites==1 && !NativeSyncs);
  native_fresh(TRUE);PartialRead=TRUE;
  memset(Readback,0xb6,sizeof(Readback));
  assert(ServiceRead(NULL,4,Range.First,sizeof(Readback),Readback)==EFI_BAD_BUFFER_SIZE);
  assert(NativeReads==1 && LastReadBytes==Batch && DmaBegins==DmaCompletes);
  for(UINTN Index=0;Index<sizeof(Readback);++Index)assert(Readback[Index]==0xb6);
  native_fresh(TRUE);WrongReadLun=TRUE;
  assert(ServiceRead(NULL,4,Range.First,Batch,Readback)==EFI_DEVICE_ERROR && DmaBegins==DmaCompletes);

  native_fresh(TRUE);RetainOnRead=TRUE;
  Timing=(PIANO_STORAGE_IO_TIMING){0};assert(PianoStorageSetIoTiming(&Timing)==NULL);
  assert(ServiceRead(NULL,4,Range.First,sizeof(Readback),Readback)==EFI_ABORTED);
  assert(mExitRetained && DmaBegins==3 && !DmaCompletes && mData.Active && mUcd.Active && mTrl.Active && CurrentTpl==TPL_CALLBACK);
  assert(Doorbells==1 && ServiceRead(NULL,4,Range.First,Batch,Readback)==EFI_ACCESS_DENIED && Doorbells==1);
  assert(PianoStorageSetIoTiming(NULL)==&Timing && Timing.Read.Submitted==1 && !Timing.Read.Completed && !Timing.Read.WaitUs);
  puts("Actual product UFS: 1 MiB READ(10)/WRITE(10) FUA, timed doorbells, whole-range gate, interior DMA map and exit retention passed.");
}
int main(void){
  filesystem_write_preparation();
  partition_batches();
  assert(PIANO_SMMU_FSR_FORMAT_MASK==0x600 && PIANO_SMMU_FSR_FAULT_MASK==0xC00001FE);
  native_fresh(FALSE);assert(PianoUfsProductVolumeOpen(&volume,&io)==EFI_NOT_FOUND && !NativeWrites && !NativeSyncs && !Allocations && !Installs && !mProductRetained && CurrentTpl==TPL_APPLICATION);
  native_fresh(TRUE);Hardware.Device[0].Fsr=0x400;mContext.AttachedSnapshot.Device[0].Fsr=0x400;native_open();
  assert(volume.Block.WriteBlocks(&volume.Block,1,0,4096,pattern)==EFI_SUCCESS && NativeWrites==1 && Hardware.Device[0].Fsr==0x400);
  native_fresh(TRUE);Hardware.Device[0].Fsr=0x402;mContext.AttachedSnapshot.Device[0].Fsr=0x400;
  assert(PianoUfsProductVolumeOpen(&volume,&io)!=EFI_SUCCESS && !Doorbells && mProductRetained && !NativeWrites && !NativeSyncs);
  native_fresh(TRUE);native_open();assert(volume.Block.WriteBlocks(&volume.Block,1,0,4096,pattern)==EFI_SUCCESS && NativeWrites==1 && NativeSyncs==1 && volume.State.VerifiedWrites==1 && !volume.State.NeedsRecovery && CurrentTpl==TPL_APPLICATION);
  CurrentTpl=TPL_CALLBACK;assert(volume.Block.WriteBlocks(&volume.Block,1,1,4096,pattern)==EFI_SUCCESS && CurrentTpl==TPL_CALLBACK && NativeWrites==2 && NativeSyncs==2);CurrentTpl=TPL_APPLICATION;
  const PIANO_UFS_PRODUCT_NV_IO *Nv=PianoUfsProductVolumeNvIo(&volume);assert(Nv);CurrentTpl=TPL_NOTIFY;
  assert(Nv->Write(Nv->Context,1,767,4096,pattern)==EFI_SUCCESS && CurrentTpl==TPL_NOTIFY && NativeWrites==3 && NativeSyncs==3 && volume.State.LastPhysical==378623);
  assert(Nv->Read(Nv->Context,1,767,4096,received)==EFI_SUCCESS && CurrentTpl==TPL_NOTIFY && !memcmp(pattern,received,4096));assert(Nv->Flush(Nv->Context)==EFI_SUCCESS && CurrentTpl==TPL_NOTIFY);
  UINTN Before=Doorbells;assert(volume.Block.ReadBlocks(&volume.Block,1,0,4096,received)==EFI_UNSUPPORTED && Doorbells==Before && CurrentTpl==TPL_NOTIFY && !volume.State.Quarantined);
  native_fresh(TRUE);native_open();assert(PianoUfsProductTransportPublish(&volume)==EFI_SUCCESS && Installs==1 && Connects==1 && !NativeWrites && !NativeSyncs);for(UINTN I=0;I<6;++I)assert(mBlocks[I].Media.ReadOnly);assert(PianoUfsProductTransportPublish(&volume)==EFI_NOT_READY);
  native_fresh(TRUE);native_open();CorruptCapture=CaptureCalls+2;assert(volume.Block.WriteBlocks(&volume.Block,1,0,4096,pattern)!=EFI_SUCCESS && !NativeWrites && mProductRetained && volume.State.Quarantined && CurrentTpl==TPL_CALLBACK);
  native_fresh(TRUE);TranslateBad=TRUE;assert(PianoUfsProductVolumeOpen(&volume,&io)!=EFI_SUCCESS && !Doorbells && mProductRetained && !Allocations);
  native_fresh(TRUE);native_open();PartialWrite=TRUE;assert(volume.Block.WriteBlocks(&volume.Block,1,0,4096,pattern)!=EFI_SUCCESS && volume.State.FirstFailure==EFI_DEVICE_ERROR && NativeWrites==1 && !NativeSyncs && volume.State.PendingWrite && volume.State.NeedsRecovery && mProductRetained && !ResetAttempted);
  assert(PianoUfsBlockIoPrepareForReset()!=EFI_SUCCESS && !ResetAttempted);
  native_fresh(TRUE);native_open();assert(PianoUfsProductTransportPublish(&volume)==EFI_SUCCESS);for(UINTN I=0;I<6;++I){mBlockHandles[I]=(VOID *)(UINTN)(100+I);mBlocks[I].Media.MediaPresent=TRUE;}mShutdownHandle=(VOID *)0x999;
  Hardware.Device[1].Present=Hardware.Device[1].Enabled=FALSE;Hardware.RawSmr[1]=Hardware.RawS2cr[1]=0;
  assert(PianoUfsBlockIoPrepareForReset()==EFI_SUCCESS && Disconnected==7 && volume.State.Closed && NativeWrites==0 && NativeSyncs==2 && CurrentTpl==TPL_CALLBACK);
  assert(PianoUfsBlockIoShutdownForReset()==EFI_SUCCESS && Freed==3 && Uninstalled==8 && ClockStops==1 && !mProductPublished && !mInstalled);
  assert(mResetReport.Clean && mResetReport.Disconnected==7 && mResetReport.ProtocolsRemoved==8 && mResetReport.DmaFreed==3 && !PianoUfsProductVolumeNvIo(&volume) && !ResetAttempted);
  printf("Actual shared UFS product transport: APP/NOTIFY lease, real golden FUA/SYNC/independentREAD, strict SMMU/PA/peer checks, readonly originals + guarded publication, unknown/partial retention, no extraDMA/no reset PASS.\n");
  return 0;
}
