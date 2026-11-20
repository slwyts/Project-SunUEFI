// SPDX-License-Identifier: BSD-2-Clause-Patent
#define MDEPKG_NDEBUG
#include "../../uefi/components/reset/PianoReset.c"
#include <assert.h>
#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#undef FDT_TAGSIZE
#include <libfdt.h>

EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;
static jmp_buf Jump;
static EFI_RESET_TYPE Expected;
static EFI_STATUS ExitStatus;
static UINTN Resets, MapReads, Exits;
static BOOLEAN UsbRetired, Exited;
static EFI_MEMORY_DESCRIPTOR Map[17];
static UINTN ImageIdentity;
static BOOLEAN Gunyah;
static UINTN SmcCalls, FailSmc;

VOID *EFIAPI ZeroMem(VOID *P,UINTN Bytes){return memset(P,0,Bytes);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN Bytes){return memcmp(A,B,Bytes);}
UINT32 EFIAPI MmioRead32(UINTN Address){(VOID)Address;assert(!"ordinary reset must not access target-specific registers");return 0;}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value){(VOID)Address;assert(!"ordinary reset must not access target-specific registers");return Value;}
VOID EFIAPI MemoryFence(VOID){}
VOID ArmCallSmc(ARM_SMC_ARGS *Args){
  // A cold reboot must call PSCI even when the secondary timer is unavailable.
  assert(Exited && !gBS && !gRT && Expected==EfiResetCold);
  switch(++SmcCalls){
    case 1:
      assert(Args->Arg0==0x8600ff01 && !Args->Arg1 && !Args->Arg2);
      Args->Arg0=Gunyah?0xcd8fd5c1:MAX_UINT32;
      Args->Arg1=0xdb5f53a4;Args->Arg2=0x36ce6592;Args->Arg3=0x145f3d67;
      return;
    case 2:assert(Args->Arg0==0x86000006 && !Args->Arg1 && !Args->Arg2);break;
    case 3:assert(Args->Arg0==0x86000005 && Args->Arg1==2 && !Args->Arg2);break;
    case 4:
      assert(Args->Arg0==0x86000008 && Args->Arg1==Args->Arg2 &&
             Args->Arg1>0 && Args->Arg1<=32000);break;
    case 5:assert(Args->Arg0==0x86000005 && Args->Arg1==3 && !Args->Arg2);break;
    case 6:assert(Args->Arg0==0x86000007 && !Args->Arg1 && !Args->Arg2);break;
    default:assert(!"unexpected hypercall");
  }
  Args->Arg0=SmcCalls==FailSmc?MAX_UINT32:0;
}
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
int main(int Argc,char **Argv){
  assert(Argc==2);
  FILE *Source=fopen(Argv[1],"rb");assert(Source);
  assert(!fseek(Source,0,SEEK_END));long Bytes=ftell(Source);assert(Bytes>0);rewind(Source);
  VOID *Tree=malloc(Bytes);assert(Tree && fread(Tree,1,Bytes,Source)==(size_t)Bytes);fclose(Source);
  (VOID)PianoResetInitialize(Tree);
  assert(mSmcConduit);
  INT32 Psci=fdt_node_offset_by_compatible(Tree,-1,"arm,psci-1.0");assert(Psci>=0);
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
  for(UINTN I=0;I<4;++I){
    assert(!fdt_setprop_inplace(Tree,Psci,"method",I==3?"hvc":"smc",4));
    (VOID)PianoResetInitialize(Tree);
    Owners.Report.Origin=PianoProductRequestUi;
    Owners.Report.ResetTarget=I==1?PIANO_PRODUCT_RESET_SHUTDOWN:PIANO_PRODUCT_RESET_NORMAL;
    if(I==2)Owners.Report.AllowedAction=PianoUsbServiceActionContinue;
    Expected=I==1?EfiResetShutdown:EfiResetCold;
    gBS=&Services;gRT=&Runtime;MapReads=Exits=SmcCalls=0;Exited=FALSE;ExitStatus=EFI_SUCCESS;
    Gunyah=I==0;
    int Stopped=setjmp(Jump);
    if(!Stopped){PianoResetExecute(&Owners,&ImageIdentity);assert(!"ResetSystem must be called");}
    assert(Stopped==1 && Exits==2 && SmcCalls==(I==0?6:I==2?1:0));
  }
  assert(Resets==4);
  assert(!fdt_setprop_inplace(Tree,Psci,"method","smc",4));
  (VOID)PianoResetInitialize(Tree);free(Tree);
  for(UINTN I=0;I<2;++I){
    gBS=&Services;gRT=&Runtime;MapReads=Exits=SmcCalls=0;Exited=FALSE;
    ExitStatus=I?EFI_DEVICE_ERROR:EFI_INVALID_PARAMETER;
    int Stopped=setjmp(Jump);
    if(!Stopped){PianoResetExecute(&Owners,&ImageIdentity);assert(!"failed exit must halt");}
    assert(Stopped==2 && !Exited && !SmcCalls && Resets==4 && Exits==(I?1:2));
  }
  Gunyah=TRUE;Expected=EfiResetCold;
  for(FailSmc=2;FailSmc<=6;++FailSmc){
    gBS=&Services;gRT=&Runtime;MapReads=Exits=SmcCalls=0;Exited=FALSE;ExitStatus=EFI_SUCCESS;
    int Stopped=setjmp(Jump);
    if(!Stopped)PianoResetExecute(&Owners,&ImageIdentity);
    assert(Stopped==1 && Exits==2 && SmcCalls==FailSmc);
  }
  puts("ResetSystem requires a clean handoff and successful ExitBootServices.");
  return 0;
}
