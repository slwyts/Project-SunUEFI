// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#define PIANO_UFS_BLOCKIO 1
#define PIANO_UFS_SHELL 1
#include "../bootprofiles/uefi-app/PianoUfsReadOnlyDma.c"
EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;
EFI_GUID gEfiBlockIoProtocolGuid,gEfiDevicePathProtocolGuid;
static EFI_BOOT_SERVICES bs;
static EFI_RUNTIME_SERVICES rt;
static struct {UINT32 tr_bell,tm_bell,tr_run,tm_run,base,upper,irq,mcq;} regs;
static unsigned disconnected,uninstalled,freed,closed,clocks_stopped,resets,shell_reports,owned_closed,retained,owned_retained,pauses;
static UINT32 tr_clear,tm_clear;
static BOOLEAN fail_disconnect,stuck_tr_run,stuck_tm_run,stuck_tr_clear,stuck_tm_clear,fail_base,fail_irq,reset_returns;
static jmp_buf reset_jump;
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
VOID PianoUfsStopClocks(VOID){assert(freed==3 && owned_closed==1 && closed==1 && regs.tr_run==mSavedRun && regs.tm_run==mSavedTaskRun);++clocks_stopped;}
EFI_STATUS PianoDmaFree(PIANO_DMA_BUFFER *Buffer){
  assert(disconnected==1 && uninstalled==2 && !mBlockLive && QueuesStopped());
  assert(regs.base==mSavedBase && regs.upper==mSavedUpper && regs.irq==mSavedInterrupt);
  assert(!Buffer->ExitRetained);++freed;return EFI_SUCCESS;
}
EFI_STATUS PianoOwnedSmmuClose(PIANO_OWNED_SMMU *Context){assert(freed==3 && QueuesStopped());++owned_closed;return EFI_SUCCESS;}
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
  ++disconnected;return fail_disconnect?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI uninstall(EFI_HANDLE Handle,...){
  assert(!mBlockLive && !mBlocks[0].Media.MediaPresent && disconnected==1 && !freed);++uninstalled;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI close_event(EFI_EVENT Event){assert(!mBlockLive && disconnected==1 && !uninstalled);++closed;return EFI_SUCCESS;}
static VOID EFIAPI reset(EFI_RESET_TYPE Type,EFI_STATUS Status,UINTN Size,VOID *Data){
  assert(Type==EfiResetCold && !freed && !owned_closed);++resets;if(!reset_returns)longjmp(reset_jump,1);
}
static VOID initialize(VOID){
  disconnected=uninstalled=freed=closed=clocks_stopped=resets=shell_reports=owned_closed=retained=owned_retained=pauses=0;
  fail_disconnect=stuck_tr_run=stuck_tm_run=stuck_tr_clear=stuck_tm_clear=fail_base=fail_irq=reset_returns=FALSE;
  tr_clear=tm_clear=MAX_UINT32;ZeroMem(&regs,sizeof(regs));regs.tr_run=regs.tm_run=1;regs.base=0x40000000;
  mSavedBase=0x12345000;mSavedUpper=0;mSavedInterrupt=0x20;mSavedRun=mSavedTaskRun=1;
  mLunCount=1;mBlockLive=TRUE;mInstalled=TRUE;mBlockBusy=FALSE;mExitRetained=FALSE;
  mBlockHandles[0]=(void *)1;mShutdownHandle=(void *)2;mExitBootEvent=(void *)3;
  mBlocks[0].Media.MediaPresent=TRUE;ZeroMem(&mData,sizeof(mData));ZeroMem(&mTrl,sizeof(mTrl));ZeroMem(&mUcd,sizeof(mUcd));ZeroMem(&mContext,sizeof(mContext));
  mData.Signature=mTrl.Signature=mUcd.Signature=1;
  mData.MemoryType=mTrl.MemoryType=mUcd.MemoryType=EfiReservedMemoryType;gBS=&bs;
}
int main(void){
  gRT=&rt;bs.DisconnectController=disconnect;bs.UninstallMultipleProtocolInterfaces=uninstall;bs.CloseEvent=close_event;rt.ResetSystem=reset;
  initialize();regs.tr_bell=0x80000100;regs.tm_bell=0x80000002;
  assert(Quiesce()==EFI_SUCCESS && QueuesStopped() && tr_clear==~0x80000100U && tm_clear==~0x80000002U && !pauses);
  initialize();stuck_tr_run=TRUE;assert(Quiesce()==EFI_TIMEOUT && pauses==QUIESCE_POLLS);
  initialize();stuck_tm_run=TRUE;assert(Quiesce()==EFI_TIMEOUT && pauses==QUIESCE_POLLS);
  initialize();regs.tr_bell=BIT31;stuck_tr_clear=TRUE;assert(Quiesce()==EFI_TIMEOUT && pauses==QUIESCE_POLLS);
  initialize();regs.tm_bell=BIT31;stuck_tm_clear=TRUE;assert(Quiesce()==EFI_TIMEOUT && pauses==QUIESCE_POLLS);
  initialize();regs.mcq=1;assert(Quiesce()==EFI_UNSUPPORTED && !pauses && regs.tr_run==1 && regs.tm_run==1);
  initialize();PianoUfsBlockIoStop();assert(disconnected==1 && uninstalled==2 && freed==3 && owned_closed==1 && clocks_stopped==1 && !resets && shell_reports==1);
  initialize();fail_disconnect=TRUE;
  if(setjmp(reset_jump)==0){PianoUfsBlockIoStop();assert(!"Failed disconnect must recover");}
  assert(resets==1 && !freed && !uninstalled && !closed && !shell_reports);
  initialize();fail_base=TRUE;
  if(setjmp(reset_jump)==0){PianoUfsBlockIoStop();assert(!"Failed base restore must retain buffers");}
  assert(resets==1 && !freed && !owned_closed);
  initialize();fail_irq=TRUE;
  if(setjmp(reset_jump)==0){PianoUfsBlockIoStop();assert(!"Failed IRQ restore must retain buffers");}
  assert(resets==1 && !freed && !owned_closed);
  initialize();gBS=NULL;PianoUfsExitBoot(NULL,NULL);
  assert(mExitRetained && !mBlockLive && !mBlocks[0].Media.MediaPresent && !disconnected && !uninstalled && !freed && !closed && !clocks_stopped && shell_reports==1);
  assert(retained==3 && owned_retained==1 && QueuesStopped() && regs.irq==0);
  assert(Cleanup()==EFI_ACCESS_DENIED);
  if(setjmp(reset_jump)==0){PianoUfsBlockIoStop();assert(!"Stop after EBS must not call BS teardown");}
  assert(resets==1 && !disconnected && !uninstalled && !freed);
  initialize();gBS=NULL;mData.MemoryType=EfiBootServicesData;
  if(setjmp(reset_jump)==0){PianoUfsExitBoot(NULL,NULL);assert(!"Wrong memory type must recover");}
  assert(resets==1 && retained==2 && !owned_retained && !freed);
  initialize();gBS=NULL;regs.tm_bell=BIT31;stuck_tm_clear=TRUE;
  if(setjmp(reset_jump)==0){PianoUfsExitBoot(NULL,NULL);assert(!"Unhalted task queue must recover");}
  assert(resets==1 && !retained && !freed);
  initialize();fail_disconnect=reset_returns=TRUE;
  int code=setjmp(reset_jump);if(code==0)PianoUfsBlockIoStop();assert(code==2 && resets==1 && !freed);
  puts("UFS lifetime: all high-slot/TMR doorbells and run readbacks, bounded direct polls, restore-before-restart, no-BS EBS Reserved retention, failure reset and reset fallthrough fence passed.");
}
