// SPDX-License-Identifier: BSD-2-Clause-Patent
// No allocation or disk I/O; optional pre-validated read-only SMMU diagnostics.
#include <Uefi.h>
#include <Protocol/Cpu.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/SerialPortLib.h>
#include <Library/PrintLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseMemoryLib.h>

STATIC EFI_CPU_ARCH_PROTOCOL *mCpu;
STATIC BOOLEAN mSync,mSError;
STATIC volatile BOOLEAN mInException;
STATIC VOID (*mDiagnostic)(VOID);
VOID PianoFaultSetDiagnostic(VOID (*Diagnostic)(VOID)){mDiagnostic=Diagnostic;}
STATIC VOID EFIAPI Recover(EFI_EXCEPTION_TYPE Type,EFI_SYSTEM_CONTEXT Context) {
  if(!mInException) {
    mInException=TRUE;CHAR8 Line[256];UINTN Bytes;
    EFI_SYSTEM_CONTEXT_AARCH64 *C=Context.SystemContextAArch64;
    Bytes=AsciiSPrint(Line,sizeof(Line),
      "SUNUEFI_FAULT_RECOVERY type=%u PC=0x%lx ESR=0x%lx FAR=0x%lx SPSR=0x%lx\n",
      (UINT32)Type,C->ELR,C->ESR,C->FAR,C->SPSR);
    SerialPortWrite((UINT8 *)Line,Bytes);
    // Only a pre-validated read-only snapshot callback is registered. A
    // recursive fault skips this hook and resets, preserving the first log.
    if(mDiagnostic!=NULL)mDiagnostic();
  }
  // The standard recovery timer cannot run if a fault/ASSERT disabled IRQs.
  // Reset directly, retaining the cache-flushed ramoops log for Android.
  gRT->ResetSystem(EfiResetCold,EFI_ABORTED,0,NULL);
  ARM_SMC_ARGS Args;ZeroMem(&Args,sizeof(Args));Args.Arg0=0x84000009;ArmCallSmc(&Args);
  while(TRUE) { }
}
VOID PianoStopFaultRecovery(VOID) {
  if(mCpu!=NULL) {
    if(mSync)mCpu->RegisterInterruptHandler(mCpu,EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS,NULL);
    if(mSError)mCpu->RegisterInterruptHandler(mCpu,EXCEPT_AARCH64_SERROR,NULL);
  }
  mSync=mSError=FALSE;mCpu=NULL;mInException=FALSE;
  mDiagnostic=NULL;
}
EFI_STATUS PianoStartFaultRecovery(VOID) {
  EFI_STATUS Status=gBS->LocateProtocol(&gEfiCpuArchProtocolGuid,NULL,(VOID **)&mCpu);
  if(EFI_ERROR(Status))return Status;
  Status=mCpu->RegisterInterruptHandler(mCpu,EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS,Recover);
  if(EFI_ERROR(Status)){mCpu=NULL;return Status;}
  mSync=TRUE;
  Status=mCpu->RegisterInterruptHandler(mCpu,EXCEPT_AARCH64_SERROR,Recover);
  if(EFI_ERROR(Status)){PianoStopFaultRecovery();return Status;}
  mSError=TRUE;mInException=FALSE;return EFI_SUCCESS;
}
EFI_STATUS PianoTestFaultRecovery(VOID) {
  if(!mSync || !mSError)return EFI_NOT_READY;
  STATIC CONST CHAR8 Marker[]="SUNUEFI_FAULT_TEST_TRIGGER undefined instruction; no device MMIO\n";
  SerialPortWrite((UINT8 *)Marker,sizeof(Marker)-1);
#ifdef __aarch64__
  __asm__ volatile(".inst 0x00000000" ::: "memory");
#endif
  return EFI_ABORTED;
}
