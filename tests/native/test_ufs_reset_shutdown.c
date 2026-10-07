// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#define PIANO_UFS_BLOCKIO 1
#define PIANO_UFS_SHELL 1
#include "../../uefi/core/PianoUfsReadOnlyDma.c"
EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;
EFI_GUID gEfiBlockIoProtocolGuid,gEfiDevicePathProtocolGuid;
static EFI_BOOT_SERVICES bs;
static EFI_RUNTIME_SERVICES rt;
static struct {UINT32 tr_bell,tm_bell,tr_run,tm_run,base,upper,irq,mcq;} regs;
static unsigned disconnected,uninstalled,freed,closed,clocks_stopped,resets,shell_reports,owned_closed,retained,owned_retained,pauses;
static UINT32 tr_clear,tm_clear;
static BOOLEAN fail_disconnect,stuck_tr_run,stuck_tm_run,stuck_tr_clear,stuck_tm_clear,fail_base,fail_irq,reset_returns;
static jmp_buf reset_jump;static unsigned raised,typed_clocks;static int free_fail,close_fail,event_fail,uninstall_fail,clock_fail,warn_disconnect;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){ }
VOID EFIAPI MemoryFence(VOID){ }
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID EFIAPI CpuPause(VOID){++pauses;}
VOID EFIAPI CpuDeadLoop(VOID){assert(reset_returns && resets==1 && !freed);longjmp(reset_jump,2);}
UINT32 EFIAPI MmioRead32(UINTN Address){
  if(Address==HCI+0x58)return regs.tr_bell;
  if(Address==HCI+0x78)return regs.tm_bell;
  if(Address==HCI+0x60)return regs.tr_run;
  if(Address==HCI+0x80)return regs.tm_run;
  if(Address==HCI+0x50)return regs.base;
  if(Address==HCI+0x54)return regs.upper;
  if(Address==HCI+0x24)return regs.irq;
  if(Address==HCI+0x300)return regs.mcq;
  assert(!"Unexpected UFS register read");return 0;
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value){
  if(Address==HCI+0x60){if(!stuck_tr_run)regs.tr_run=Value;if(Value)assert(freed==3 && owned_closed==1);}
  else if(Address==HCI+0x80){if(!stuck_tm_run)regs.tm_run=Value;if(Value)assert(freed==3 && owned_closed==1);}
  else if(Address==HCI+0x5C){tr_clear=Value;if(!stuck_tr_clear)regs.tr_bell&=Value;}
  else if(Address==HCI+0x7C){tm_clear=Value;if(!stuck_tm_clear)regs.tm_bell&=Value;}
  else if(Address==HCI+0x50){if(!fail_base)regs.base=Value;}
  else if(Address==HCI+0x54)regs.upper=Value;
  else if(Address==HCI+0x24){if(!fail_irq)regs.irq=Value;}
  else assert(!"Unexpected UFS register write");
  return Value;
}
VOID PianoSmmuLogFaults(CONST PIANO_SMMU_SNAPSHOT *S){ }
VOID PianoFaultSetDiagnostic(VOID (*Diagnostic)(VOID)){assert(Diagnostic==NULL);}
VOID PianoReportShellDiagnostics(VOID){assert(mBlockLive);++shell_reports;}
EFI_STATUS PianoUfsStopClocksForReset(VOID){assert(QueuesStopped() && regs.irq==0 && !resets && freed==3 && owned_closed==1);++typed_clocks;return clock_fail?EFI_DEVICE_ERROR:EFI_SUCCESS;}
VOID PianoUfsStopClocks(VOID){assert(freed==3 && owned_closed==1 && closed==1 && regs.tr_run==mSavedRun && regs.tm_run==mSavedTaskRun);++clocks_stopped;}
EFI_STATUS PianoDmaFree(PIANO_DMA_BUFFER *Buffer){
  assert(disconnected==1 && !mBlockLive && QueuesStopped());
  assert(regs.base==mSavedBase && regs.upper==mSavedUpper && regs.irq==0);
  assert(!Buffer->ExitRetained);if(free_fail && freed==0)return EFI_DEVICE_ERROR;++freed;ZeroMem(Buffer,sizeof(*Buffer));return EFI_SUCCESS;
}
EFI_STATUS PianoOwnedSmmuClose(PIANO_OWNED_SMMU *Context){assert(freed==3 && QueuesStopped());if(close_fail)return EFI_DEVICE_ERROR;++owned_closed;Context->Attached=FALSE;Context->Domain=NULL;Context->TableMemory.Signature=0;return EFI_SUCCESS;}
EFI_STATUS PianoDmaRetainForExit(PIANO_DMA_BUFFER *Buffer){
  assert(gBS==NULL && QueuesStopped() && !freed && !owned_closed);
  if(Buffer->MemoryType!=EfiReservedMemoryType)return EFI_ACCESS_DENIED;
  Buffer->ExitRetained=TRUE;++retained;return EFI_SUCCESS;
}
EFI_STATUS PianoOwnedSmmuRetainForExit(PIANO_OWNED_SMMU *Context){
  assert(gBS==NULL && retained==3 && QueuesStopped() && !owned_closed);
  Context->ExitRetained=TRUE;++owned_retained;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI disconnect(EFI_HANDLE Handle,EFI_HANDLE Driver,EFI_HANDLE Child){
  assert(Handle==mBlockHandles[0] && mBlockLive && mBlocks[0].Media.MediaPresent && !freed && !uninstalled);
  ++disconnected;return warn_disconnect?EFI_WARN_UNKNOWN_GLYPH:fail_disconnect?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI uninstall(EFI_HANDLE Handle,...){
  assert(!mBlockLive && !mBlocks[0].Media.MediaPresent && disconnected==1 && freed==3);if(uninstall_fail)return EFI_DEVICE_ERROR;++uninstalled;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI close_event(EFI_EVENT Event){assert(!mBlockLive && disconnected==1 && !uninstalled && freed==3);if(event_fail)return EFI_DEVICE_ERROR;++closed;return EFI_SUCCESS;}
static VOID EFIAPI reset(EFI_RESET_TYPE Type,EFI_STATUS Status,UINTN Size,VOID *Data){
  assert(Type==EfiResetCold && !freed && !owned_closed);++resets;if(!reset_returns)longjmp(reset_jump,1);
}
static EFI_TPL EFIAPI raise_tpl(EFI_TPL T){assert(T==TPL_CALLBACK);++raised;return TPL_APPLICATION;}
static VOID initialize(VOID){
  disconnected=uninstalled=freed=closed=clocks_stopped=resets=shell_reports=owned_closed=retained=owned_retained=pauses=0;
  fail_disconnect=stuck_tr_run=stuck_tm_run=stuck_tr_clear=stuck_tm_clear=fail_base=fail_irq=reset_returns=FALSE;
  raised=typed_clocks=0;free_fail=close_fail=event_fail=uninstall_fail=clock_fail=warn_disconnect=0;ZeroMem(&mResetReport,sizeof(mResetReport));
  tr_clear=tm_clear=MAX_UINT32;ZeroMem(&regs,sizeof(regs));regs.tr_run=regs.tm_run=1;regs.base=0x40000000;
  mSavedBase=0x12345000;mSavedUpper=0;mSavedInterrupt=0x20;mSavedRun=mSavedTaskRun=1;
  mLunCount=1;mBlockLive=TRUE;mInstalled=TRUE;mBlockBusy=FALSE;mExitRetained=FALSE;
  mBlockHandles[0]=(void *)1;mShutdownHandle=(void *)2;mExitBootEvent=(void *)3;
  mBlocks[0].Media.MediaPresent=TRUE;ZeroMem(&mData,sizeof(mData));ZeroMem(&mTrl,sizeof(mTrl));ZeroMem(&mUcd,sizeof(mUcd));ZeroMem(&mContext,sizeof(mContext));
  mData.Signature=mTrl.Signature=mUcd.Signature=1;mTrl.DeviceAddress=regs.base;mEverInstalled=TRUE;mContext.Attached=TRUE;mContext.Domain=(VOID *)4;mContext.TableMemory.Signature=1;
  mData.MemoryType=mTrl.MemoryType=mUcd.MemoryType=EfiReservedMemoryType;gBS=&bs;
}
int main(void){
  gRT=&rt;bs.RaiseTPL=raise_tpl;bs.DisconnectController=disconnect;bs.UninstallMultipleProtocolInterfaces=uninstall;bs.CloseEvent=close_event;rt.ResetSystem=reset;
  initialize();regs.tr_bell=BIT31;regs.tm_bell=BIT30;
  assert(PianoUfsBlockIoPrepareForReset()==EFI_SUCCESS && disconnected==1 && !mBlockLive && !freed && !owned_closed && !typed_clocks && QueuesStopped() && regs.irq==0 && mResetReport.Prepared && mResetReport.TplHeld && raised==1);
  assert(PianoUfsBlockIoPrepareForReset()==EFI_SUCCESS && disconnected==1 && raised==1);
  assert(PianoUfsBlockIoShutdownForReset()==EFI_SUCCESS && freed==3 && owned_closed==1 && closed==1 && uninstalled==2 && typed_clocks==1 && QueuesStopped() && !regs.irq && regs.base==mSavedBase && !resets && !mInstalled);
  assert(mResetReport.Clean && mResetReport.DmaFreed==3 && mResetReport.ProtocolsRemoved==2 && mResetReport.Result==EFI_SUCCESS);
  assert(PianoUfsBlockIoShutdownForReset()==EFI_SUCCESS && typed_clocks==1 && raised==1);
  initialize();fail_disconnect=TRUE;assert(PianoUfsBlockIoPrepareForReset()==EFI_DEVICE_ERROR && !freed && !typed_clocks && mResetReport.Failed && !resets);
  assert(PianoUfsBlockIoShutdownForReset()==EFI_DEVICE_ERROR && disconnected==1);
  initialize();warn_disconnect=1;assert(PianoUfsBlockIoPrepareForReset()==EFI_DEVICE_ERROR && !freed && !resets);
  initialize();regs.base^=4096;assert(PianoUfsBlockIoPrepareForReset()==EFI_COMPROMISED_DATA && !disconnected && !freed && !resets);
  initialize();stuck_tm_run=TRUE;assert(PianoUfsBlockIoPrepareForReset()==EFI_TIMEOUT && !freed && !owned_closed && !resets);
  initialize();fail_irq=TRUE;regs.irq=1;assert(PianoUfsBlockIoPrepareForReset()==EFI_DEVICE_ERROR && !freed && !resets);
  initialize();assert(PianoUfsBlockIoPrepareForReset()==EFI_SUCCESS);fail_base=TRUE;assert(PianoUfsBlockIoShutdownForReset()==EFI_DEVICE_ERROR && !freed && !owned_closed && !typed_clocks);
  initialize();free_fail=1;assert(PianoUfsBlockIoShutdownForReset()==EFI_DEVICE_ERROR && !freed && !owned_closed && !typed_clocks);
  initialize();close_fail=1;assert(PianoUfsBlockIoShutdownForReset()==EFI_DEVICE_ERROR && freed==3 && !owned_closed && !typed_clocks && !uninstalled);
  initialize();event_fail=1;assert(PianoUfsBlockIoShutdownForReset()==EFI_DEVICE_ERROR && freed==3 && owned_closed==1 && !uninstalled && !typed_clocks);
  initialize();uninstall_fail=1;assert(PianoUfsBlockIoShutdownForReset()==EFI_DEVICE_ERROR && freed==3 && owned_closed==1 && !typed_clocks);
  initialize();clock_fail=1;assert(PianoUfsBlockIoShutdownForReset()==EFI_DEVICE_ERROR && !mResetReport.Clean && mResetReport.Clocks==EFI_DEVICE_ERROR && QueuesStopped() && !resets);
  initialize();mData.Active=TRUE;assert(PianoUfsBlockIoShutdownForReset()==EFI_ACCESS_DENIED && !freed && !owned_closed && !typed_clocks);
  initialize();mBlockBusy=TRUE;assert(PianoUfsBlockIoPrepareForReset()==EFI_NOT_READY && !raised && !disconnected);
  initialize();mExitRetained=TRUE;assert(PianoUfsBlockIoPrepareForReset()==EFI_ACCESS_DENIED && !raised && !freed);
  initialize();mInstalled=FALSE;assert(PianoUfsBlockIoPrepareForReset()==EFI_NOT_READY && !freed && !resets);
  puts("Typed UFS cold-reset retirement: live disconnect, all queue/IRQ readback, bases restored without inherited-run restart, post-free/domain checks, exact-status failures, retain/no-reset and idempotent phases passed.");
}
