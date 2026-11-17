// SPDX-License-Identifier: BSD-2-Clause-Patent
#define MDEPKG_NDEBUG
#include "../../uefi/components/reset/PianoReset.c"
#include <assert.h>
#include <setjmp.h>
#include <string.h>
#include <stdio.h>

EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;
static jmp_buf Jump;
static EFI_RESET_TYPE Expected;
static UINTN Resets;

VOID *EFIAPI ZeroMem(VOID *P,UINTN Bytes){return memset(P,0,Bytes);}
UINT32 EFIAPI MmioRead32(UINTN Address){(VOID)Address;assert(!"ordinary reset must not access target-specific registers");return 0;}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value){(VOID)Address;assert(!"ordinary reset must not access target-specific registers");return Value;}
VOID EFIAPI MemoryFence(VOID){}
VOID ArmCallSmc(ARM_SMC_ARGS *Args){(VOID)Args;assert(!"ordinary reset must not invoke download mode");}
static VOID EFIAPI Reset(EFI_RESET_TYPE Type,EFI_STATUS Status,UINTN Bytes,VOID *Data){
  assert(Type==Expected && Status==EFI_SUCCESS && !Bytes && !Data);
  ++Resets;longjmp(Jump,1);
}
int main(VOID){
  EFI_RUNTIME_SERVICES Runtime={.ResetSystem=Reset};gRT=&Runtime;
  // No Boot Services functions are supplied: resetting must not get a memory
  // map, signal ExitBootServices or release retained device allocations.
  EFI_BOOT_SERVICES Services={0};gBS=&Services;
  PIANO_PRODUCT_OWNERS Owners={0};
  assert(PianoResetExecute(&Owners)==EFI_ACCESS_DENIED && !Resets);
  Owners.Report.ResetPrepared=TRUE;Owners.Report.AllowedAction=PianoUsbServiceActionReboot;
  Owners.Report.Retained=TRUE;Owners.Report.Touch.Retained=TRUE;
  for(UINTN I=0;I<3;++I){
    Owners.Report.Origin=PianoProductRequestUi;
    Owners.Report.ResetTarget=I==1?PIANO_PRODUCT_RESET_SHUTDOWN:PIANO_PRODUCT_RESET_NORMAL;
    if(I==2)Owners.Report.AllowedAction=PianoUsbServiceActionContinue;
    Expected=I==1?EfiResetShutdown:EfiResetCold;
    if(!setjmp(Jump)){PianoResetExecute(&Owners);assert(!"ResetSystem must be called");}
  }
  assert(Resets==3 && !Owners.Report.Clean && !Owners.Report.UfsStopped);
  Owners.Report.ServicesLost=TRUE;
  assert(PianoResetExecute(&Owners)==EFI_ACCESS_DENIED && Resets==3);
  puts("ResetSystem: storage preparation required; retained peripherals and active Boot Services do not block full reset.");
  return 0;
}
