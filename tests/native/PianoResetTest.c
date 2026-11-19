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
static EFI_STATUS ExitStatus;
static UINTN Resets, MapReads, Exits;
static BOOLEAN UsbRetired, Exited;
static EFI_MEMORY_DESCRIPTOR Map[17];
static UINTN ImageIdentity;

VOID *EFIAPI ZeroMem(VOID *P,UINTN Bytes){return memset(P,0,Bytes);}
UINT32 EFIAPI MmioRead32(UINTN Address){(VOID)Address;assert(!"ordinary reset must not access target-specific registers");return 0;}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value){(VOID)Address;assert(!"ordinary reset must not access target-specific registers");return Value;}
VOID EFIAPI MemoryFence(VOID){}
VOID ArmCallSmc(ARM_SMC_ARGS *Args){(VOID)Args;assert(!"ordinary reset must not invoke download mode");}
BOOLEAN PianoProductOwnersUsbRetired(CONST PIANO_PRODUCT_OWNERS *Owners){
  assert(Owners);return UsbRetired;
}
VOID *EFIAPI AllocatePool(UINTN Bytes){
  assert(Bytes==sizeof(Map) && !Exits);return Map;
}
VOID EFIAPI FreePool(VOID *Buffer){assert(Buffer==Map && !Exits);}
VOID EFIAPI CpuDeadLoop(VOID){longjmp(Jump,2);}
static EFI_STATUS EFIAPI GetMap(UINTN *Size,EFI_MEMORY_DESCRIPTOR *Memory,UINTN *Key,
                               UINTN *Stride,UINT32 *Version){
  assert(!Exited);
  if(!Memory){
    assert(!*Size && !MapReads && !Exits);
    *Stride=*Size=sizeof(*Map);
    return EFI_BUFFER_TOO_SMALL;
  }
  assert(Memory==Map && *Size==sizeof(Map) && MapReads==Exits);
  *Stride=*Size=sizeof(*Map);*Version=1;*Key=++MapReads;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Exit(EFI_HANDLE Image,UINTN Key){
  assert(Image==&ImageIdentity && Key==MapReads && !Exited);
  if(++Exits==1 && ExitStatus==EFI_SUCCESS)return EFI_INVALID_PARAMETER;
  if(ExitStatus==EFI_SUCCESS){Exited=TRUE;gBS=NULL;gRT=NULL;}
  return ExitStatus;
}
static VOID EFIAPI Reset(EFI_RESET_TYPE Type,EFI_STATUS Status,UINTN Bytes,VOID *Data){
  assert(Exited && !gBS && !gRT && Type==Expected && Status==EFI_SUCCESS && !Bytes && !Data);
  ++Resets;longjmp(Jump,1);
}
int main(VOID){
  EFI_RUNTIME_SERVICES Runtime={.ResetSystem=Reset};gRT=&Runtime;
  EFI_BOOT_SERVICES Services={.GetMemoryMap=GetMap,.ExitBootServices=Exit};gBS=&Services;
  PIANO_PRODUCT_OWNERS Owners={0};
  assert(PianoResetExecute(&Owners,&ImageIdentity)==EFI_ACCESS_DENIED && !Resets);
  Owners.Report.Clean=Owners.Report.UfsStopped=Owners.Report.InputStopped=TRUE;
  Owners.Report.AllowedAction=PianoUsbServiceActionReboot;
  assert(PianoResetExecute(&Owners,&ImageIdentity)==EFI_ACCESS_DENIED && !Exits);
  UsbRetired=TRUE;Owners.Report.Retained=TRUE;
  assert(PianoResetExecute(&Owners,&ImageIdentity)==EFI_ACCESS_DENIED && !Exits);
  Owners.Report.Retained=FALSE;
  for(UINTN I=0;I<3;++I){
    Owners.Report.Origin=PianoProductRequestUi;
    Owners.Report.ResetTarget=I==1?PIANO_PRODUCT_RESET_SHUTDOWN:PIANO_PRODUCT_RESET_NORMAL;
    if(I==2)Owners.Report.AllowedAction=PianoUsbServiceActionContinue;
    Expected=I==1?EfiResetShutdown:EfiResetCold;
    gBS=&Services;gRT=&Runtime;MapReads=Exits=0;Exited=FALSE;ExitStatus=EFI_SUCCESS;
    int Stopped=setjmp(Jump);
    if(!Stopped){PianoResetExecute(&Owners,&ImageIdentity);assert(!"ResetSystem must be called");}
    assert(Stopped==1 && Exits==2);
  }
  assert(Resets==3);
  for(UINTN I=0;I<2;++I){
    gBS=&Services;gRT=&Runtime;MapReads=Exits=0;Exited=FALSE;
    ExitStatus=I?EFI_DEVICE_ERROR:EFI_INVALID_PARAMETER;
    int Stopped=setjmp(Jump);
    if(!Stopped){PianoResetExecute(&Owners,&ImageIdentity);assert(!"failed exit must halt");}
    assert(Stopped==2 && !Exited && Resets==3 && Exits==(I?1:2));
  }
  puts("ResetSystem requires a clean handoff and successful ExitBootServices.");
  return 0;
}
