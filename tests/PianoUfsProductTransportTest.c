// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual Submit/shared adapter + actual provider; simulated HCI/storage only.
#define main offline_provider_fixture_suite
#include "PianoUfsProductVolumeTest.c"
#undef main
#define PIANO_UFS_BLOCKIO 1
#define PIANO_UFS_PRODUCT_STORAGE 1
#include "../bootprofiles/uefi-app/PianoUfsDmaLayout.c"
#include "../bootprofiles/uefi-app/PianoUfsReadOnlyDma.c"
EFI_BOOT_SERVICES *gBS;EFI_RUNTIME_SERVICES *gRT;
EFI_GUID gEfiBlockIoProtocolGuid,gEfiDevicePathProtocolGuid;
static EFI_BOOT_SERVICES Bs;static EFI_RUNTIME_SERVICES Rt;
#include <stdarg.h>
static UINT8 Trl[1024],Ucd[1024],Data[4096];static UINT32 Regs[0x400/4];
static PIANO_SMMU_SNAPSHOT Hardware;
static EFI_TPL CurrentTpl;static UINTN CaptureCalls,Doorbells,DmaBegins,DmaCompletes,Allocations,Installs,Connects,NativeWrites,NativeSyncs;
static UINTN Freed,Disconnected,Uninstalled,ClockStops;
static UINTN CorruptCapture;static BOOLEAN TranslateBad,PartialWrite,ResetAttempted;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){(void)Level;return FALSE;}VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){(void)Level;(void)Format;}
VOID EFIAPI MemoryFence(VOID){}VOID EFIAPI CpuPause(VOID){}
VOID EFIAPI CpuDeadLoop(VOID){assert(!"Unexpected fail-stop in successful simulated source path");}
VOID PianoFaultSetDiagnostic(VOID (*F)(VOID)){(void)F;}
VOID PianoSmmuLogFaults(CONST PIANO_SMMU_SNAPSHOT *S){(void)S;}
static EFI_TPL EFIAPI raise_tpl(EFI_TPL New){assert(New>=CurrentTpl);EFI_TPL Old=CurrentTpl;CurrentTpl=New;return Old;}
static VOID EFIAPI restore_tpl(EFI_TPL Old){assert(Old<=CurrentTpl);CurrentTpl=Old;}
static EFI_STATUS EFIAPI stall(UINTN Us){assert(Us==10 || Us==100);return EFI_SUCCESS;}
static VOID EFIAPI reset(EFI_RESET_TYPE Type,EFI_STATUS Status,UINTN Bytes,VOID *Arg){(void)Type;(void)Status;(void)Bytes;(void)Arg;ResetAttempted=TRUE;assert(!"Product backend may not reset");}
EFI_STATUS PianoDmaAllocate(PIANO_DMA_DEVICE *D,UINTN Bytes,UINTN Align,UINT8 Bits,PIANO_DMA_DIRECTION Direction,PIANO_DMA_BUFFER *Buffer){(void)D;(void)Bytes;(void)Align;(void)Bits;(void)Direction;(void)Buffer;++Allocations;assert(!"Shared adapter must not allocate DMA");return EFI_UNSUPPORTED;}
EFI_STATUS PianoDmaBegin(PIANO_DMA_BUFFER *B,CONST CHAR8 *Name){(void)Name;assert(!B->Active && !B->Quarantined);B->Active=TRUE;++DmaBegins;return EFI_SUCCESS;}
EFI_STATUS PianoDmaComplete(PIANO_DMA_BUFFER *B,EFI_STATUS Status,BOOLEAN Quiet){assert(B->Active && Quiet && !Regs[0x58/4]);B->Active=FALSE;++DmaCompletes;return Status;}
EFI_STATUS PianoIoPageTableTranslate(PIANO_IO_PAGE_TABLE *T,UINT64 Iova,BOOLEAN Write,UINT64 *Physical){assert(T==&mContext.PageTable && Write);*Physical=0x81000000+Iova-0x40000000+(TranslateBad?1:0);return EFI_SUCCESS;}
EFI_STATUS PianoSmmuCapture(CONST VOID *Fdt,CONST CHAR8 *Phase,PIANO_SMMU_SNAPSHOT *Snapshot){assert(Fdt==(VOID *)0x123 && !strcmp(Phase,"product-volume-lease"));++CaptureCalls;*Snapshot=Hardware;if(CaptureCalls==CorruptCapture)Snapshot->RawS2cr[3]^=1;return EFI_SUCCESS;}
UINT32 EFIAPI MmioRead32(UINTN Address){assert(Address>=HCI && Address<HCI+sizeof(Regs) && !(Address&3));return Regs[(Address-HCI)/4];}
static VOID physical_read(UINT32 Lba){
  if(Lba==1)memcpy(Data,primary,4096);
  else if(Lba>=2 && Lba<=4)memcpy(Data,entries+(Lba-2)*4096,4096);
  else if(Lba==378879)memcpy(Data,backup,4096);
  else if(Lba>=378873 && Lba<=378875)memcpy(Data,backup_entries+(Lba-378873)*4096,4096);
  else{assert(Lba>=375040 && Lba<=378623);memcpy(Data,disk+(Lba-375040)*4096,4096);}
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value){
  assert(Address>=HCI && Address<HCI+sizeof(Regs) && !(Address&3));UINTN Off=Address-HCI;
  if(Off==0x20){Regs[Off/4]&=~Value;return Value;}
  if(Off==0x5c){Regs[0x58/4]&=Value;return Value;}
  if(Off==0x7c){Regs[0x78/4]&=Value;return Value;}
  Regs[Off/4]=Value;if(Off!=0x58)return Value;
  assert(Value==1 && Regs[0x60/4]==1 && CurrentTpl>=TPL_CALLBACK && mProductExecuting && mBlockBusy);++Doorbells;
  UINT8 *R=Ucd+64;Le32(Trl+8,0);R[0]=Ucd[0]==1?0x21:0x36;R[2]=Ucd[2];R[3]=Ucd[3];
  if(Ucd[0]==0x16){
    if(Ucd[12]==5){R[12]=5;R[13]=Ucd[13];Be32(R+20,0);}
    else{assert(Ucd[12]==1 && Ucd[13]==2 && Ucd[14]==4);R[12]=1;R[13]=2;R[14]=4;R[11]=32;R[32]=32;R[33]=2;R[34]=4;R[37]=1;}
  }else{
    assert(Ucd[0]==1 && Ucd[2]==4);
    if(Ucd[16]==0x9e){Be32(Data,0);Be32(Data+4,378879);Be32(Data+8,4096);}
    else if(Ucd[16]==0x5a){Data[1]=26;Data[3]=0x10;Data[8]=8;Data[9]=0x12;Data[10]=4;R[1]=0x20;Be32(R+12,4096-28);}
    else if(Ucd[16]==0x28){assert(Ucd[24]==1);physical_read(ReadBe32(Ucd+18));}
    else if(Ucd[16]==0x2a){UINT32 Lba=ReadBe32(Ucd+18);assert(Lba>=375042 && Lba<=378623 && Ucd[17]==8 && Ucd[24]==1 && volume.State.PendingWrite);++NativeWrites;memcpy(disk+(Lba-375040)*4096,Data,4096);if(PartialWrite){R[1]=0x20;Be32(R+12,1);}}
    else{assert(Ucd[16]==0x35 && ReadBe32(Ucd+18)==375040 && Ucd[23]==14 && Ucd[24]==0);++NativeSyncs;}
  }
  Regs[0x58/4]=0;return Value;
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
  mProductVolume=NULL;mProductExecuting=mProductRetained=mProductPublished=mBlockBusy=mExitRetained=FALSE;mBlockLive=mInstalled=mEverInstalled=TRUE;mLunCount=6;mShutdownHandle=mExitBootEvent=NULL;mSavedBase=0x12345000;mSavedUpper=0;
  mDevice=(PIANO_DMA_DEVICE){.Name="ufs",.Context=&mContext,.StreamId=0x60};
  mTrl=(PIANO_DMA_BUFFER){.Signature=1,.Device=&mDevice,.Cpu=Trl,.Bytes=1024,.Physical=0x81000000,.DeviceAddress=0x40000000,.Mapped=TRUE,.Direction=PianoDmaBidirectional};
  mUcd=(PIANO_DMA_BUFFER){.Signature=1,.Device=&mDevice,.Cpu=Ucd,.Bytes=1024,.Physical=0x81001000,.DeviceAddress=0x40001000,.Mapped=TRUE,.Direction=PianoDmaBidirectional};
  mData=(PIANO_DMA_BUFFER){.Signature=1,.Device=&mDevice,.Cpu=Data,.Bytes=4096,.Physical=0x81002000,.DeviceAddress=0x40002000,.Mapped=TRUE,.Direction=PianoDmaBidirectional};
  PIANO_DMA_BUFFER *Buffers[]={&mTrl,&mUcd,&mData};for(UINTN I=0;I<3;++I){Buffers[I]->ReservedBytes=Buffers[I]->Alignment=4096;Buffers[I]->MemoryType=EfiReservedMemoryType;Buffers[I]->MemoryAttributes=EFI_MEMORY_WB;}
  Regs[0x50/4]=(UINT32)mTrl.DeviceAddress;Regs[0x34/4]=1;Regs[0x30/4]=15;
  Hardware=(PIANO_SMMU_SNAPSHOT){.Valid=TRUE,.Base=0x15000000,.Window=0x100000,.ContextBase=0x80000,.PageShift=12,.Groups=4,.Banks=2};
  Hardware.Device[0]=(PIANO_SMMU_DEVICE){.Present=TRUE,.Enabled=TRUE,.Sid=0x60,.Smr=0x80000060,.Sctlr=0x1e5,.Ttbr0=0x81008000};Hardware.RawSmr[0]=Hardware.Device[0].Smr;
  Hardware.Device[1]=(PIANO_SMMU_DEVICE){.Present=TRUE,.Enabled=TRUE,.Sid=0x40,.StreamIndex=1,.ContextBank=1,.Smr=0x80000040,.S2cr=1,.Sctlr=0x1e5,.Ttbr0=0x81009000};Hardware.RawSmr[1]=Hardware.Device[1].Smr;Hardware.RawS2cr[1]=1;
  mContext.Attached=mContext.Verified=TRUE;mContext.Domain=(VOID *)0x777;mContext.Fdt=(VOID *)0x123;mContext.TableMemory.Signature=1;mContext.TableMemory.Physical=0x81008000;mContext.AttachedSnapshot=Hardware;
  for(UINTN I=0;I<6;++I)mBlocks[I].Media.ReadOnly=TRUE;
  memset(&Bs,0,sizeof(Bs));Bs.RaiseTPL=raise_tpl;Bs.RestoreTPL=restore_tpl;Bs.Stall=stall;Bs.InstallMultipleProtocolInterfaces=install;Bs.ConnectController=connect;Bs.DisconnectController=disconnect;Bs.UninstallMultipleProtocolInterfaces=uninstall;gBS=&Bs;Rt.ResetSystem=reset;gRT=&Rt;
  assert(PianoUfsProductTransportIo(&volume,&io)==EFI_SUCCESS);
}
static VOID native_open(void){assert(PianoUfsProductVolumeOpen(&volume,&original,&io)==EFI_SUCCESS && !NativeWrites && !NativeSyncs && !Allocations && CurrentTpl==TPL_APPLICATION && DmaBegins==DmaCompletes);}
int main(void){
  native_fresh(FALSE);assert(PianoUfsProductVolumeOpen(&volume,&original,&io)==EFI_NOT_FOUND && !NativeWrites && !NativeSyncs && !Allocations && !Installs && !mProductRetained && CurrentTpl==TPL_APPLICATION);
  native_fresh(TRUE);native_open();assert(volume.Block.WriteBlocks(&volume.Block,1,0,4096,pattern)==EFI_SUCCESS && NativeWrites==1 && NativeSyncs==1 && volume.State.VerifiedWrites==1 && !volume.State.NeedsRecovery && CurrentTpl==TPL_APPLICATION);
  CurrentTpl=TPL_CALLBACK;assert(volume.Block.WriteBlocks(&volume.Block,1,1,4096,pattern)==EFI_SUCCESS && CurrentTpl==TPL_CALLBACK && NativeWrites==2 && NativeSyncs==2);CurrentTpl=TPL_APPLICATION;
  const PIANO_UFS_PRODUCT_NV_IO *Nv=PianoUfsProductVolumeNvIo(&volume);assert(Nv);CurrentTpl=TPL_NOTIFY;
  assert(Nv->Write(Nv->Context,1,767,4096,pattern)==EFI_SUCCESS && CurrentTpl==TPL_NOTIFY && NativeWrites==3 && NativeSyncs==3 && volume.State.LastPhysical==378623);
  assert(Nv->Read(Nv->Context,1,767,4096,received)==EFI_SUCCESS && CurrentTpl==TPL_NOTIFY && !memcmp(pattern,received,4096));assert(Nv->Flush(Nv->Context)==EFI_SUCCESS && CurrentTpl==TPL_NOTIFY);
  UINTN Before=Doorbells;assert(volume.Block.ReadBlocks(&volume.Block,1,0,4096,received)==EFI_UNSUPPORTED && Doorbells==Before && CurrentTpl==TPL_NOTIFY && !volume.State.Quarantined);
  native_fresh(TRUE);native_open();assert(PianoUfsProductTransportPublish(&volume)==EFI_SUCCESS && Installs==1 && Connects==1 && !NativeWrites && !NativeSyncs);for(UINTN I=0;I<6;++I)assert(mBlocks[I].Media.ReadOnly);assert(PianoUfsProductTransportPublish(&volume)==EFI_NOT_READY);
  native_fresh(TRUE);native_open();CorruptCapture=CaptureCalls+2;assert(volume.Block.WriteBlocks(&volume.Block,1,0,4096,pattern)!=EFI_SUCCESS && !NativeWrites && mProductRetained && volume.State.Quarantined && CurrentTpl==TPL_CALLBACK);
  native_fresh(TRUE);TranslateBad=TRUE;assert(PianoUfsProductVolumeOpen(&volume,&original,&io)!=EFI_SUCCESS && !Doorbells && mProductRetained && !Allocations);
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
