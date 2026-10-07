// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual pure proof/contract helpers and close ledger. No real MMIO/native HAL.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/PianoOwnedSmmu.c"
static PIANO_SMMU_SNAPSHOT capture;
static UINT32 detach_code,destroy_code;
static EFI_STATUS capture_status,free_status;
static UINTN detach_calls,destroy_calls,free_calls;
static UINTN api[15];
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){abort();}
UINTN EFIAPI AsciiSPrint(CHAR8 *Buffer,UINTN Bytes,CONST CHAR8 *Format,...){abort();return 0;}
VOID *EFIAPI ZeroMem(VOID *Buffer,UINTN Bytes){return memset(Buffer,0,Bytes);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN Bytes){return memcmp(A,B,Bytes);}
VOID EFIAPI MemoryFence(VOID){}
UINT32 EFIAPI MmioRead32(UINTN Address){UINTN Offset=Address-0x15000000;if(Offset>=0x800 && Offset<0xc00)return capture.RawSmr[(Offset-0x800)/4];if(Offset>=0xc00 && Offset<0x1000)return capture.RawS2cr[(Offset-0xc00)/4];abort();return 0;}
static UINT32 detach(VOID *Domain,CONST CHAR8 *Name,UINT32 Arid,UINT32 Flags){
  assert(!Flags && ((Domain==(VOID *)123 && !strcmp(Name,"USB0") && Arid==0x03000000) ||
    (Domain==(VOID *)321 && !strcmp(Name,"UFS_MEM") && Arid==0)));++detach_calls;return detach_code;
}
static UINT32 destroy(VOID *Domain){assert(Domain==(VOID *)123 || Domain==(VOID *)321);++destroy_calls;return destroy_code;}
EFI_STATUS PianoDmaFree(PIANO_DMA_BUFFER *Buffer){++free_calls;assert(!Buffer->Quarantined);if(free_status==EFI_SUCCESS)ZeroMem(Buffer,sizeof(*Buffer));return free_status;}
EFI_STATUS PianoSmmuCapture(CONST VOID *Fdt,CONST CHAR8 *Phase,PIANO_SMMU_SNAPSHOT *Out){assert(!strcmp(Phase,"owned-detached"));*Out=capture;return capture_status;}
static PIANO_OWNED_SMMU *usb,*ufs;
static PIANO_SMMU_SNAPSHOT now,detached;
static PIANO_SMMU_USB_RETIRE_EVIDENCE execution;
static PIANO_SMMU_RETIRED_USB_PROOF proof;
static PIANO_SMMU_RETIRED_USB_CONTRACT contract;
static PIANO_SMMU_DEVICE identity(UINT16 Sid,UINT16 Slot,UINT8 Bank,UINT64 Root){
  return (PIANO_SMMU_DEVICE){.Sid=Sid,.StreamIndex=Slot,.ContextBank=Bank,.Present=TRUE,.Enabled=TRUE,.Type=0,
    .Smr=Sid,.S2cr=BIT10|Bank,.Sctlr=0x1e5,.Cbar=0x10000,.Cba2r=1,.Tcr=0x802519,.Tcr2=0x38001,.Mair0=0xff,.Ttbr0=Root};
}
static void setup(void){
  ZeroMem(usb,sizeof(*usb));ZeroMem(ufs,sizeof(*ufs));ZeroMem(&now,sizeof(now));
  ZeroMem(&proof,sizeof(proof));ZeroMem(&contract,sizeof(contract));
  now.Valid=TRUE;now.ExtendedIds=TRUE;now.Base=0x15000000;now.Window=0x100000;now.ContextBase=0x80000;
  now.PageShift=12;now.Groups=127;now.Banks=83;now.Id0=BIT8|127;now.Id1=83;now.Id2=7;now.GlobalControl=BIT3;
  now.RawSmr[1]=0x540;now.RawS2cr[1]=0x20001;now.RawS2cr[113]=0x0300006e;
  now.Device[0]=(PIANO_SMMU_DEVICE){.Sid=0x60};now.Device[1]=(PIANO_SMMU_DEVICE){.Sid=0x40};
  now.Device[2]=identity(0xb6,3,2,0x83000000);now.RawSmr[3]=now.Device[2].Smr;now.RawS2cr[3]=now.Device[2].S2cr;
  ufs->Before=now;now.Device[0]=identity(0x60,0,0,0x81000000);now.RawSmr[0]=0x60;now.RawS2cr[0]=BIT10;
  ufs->DeviceIndex=0;ufs->Attached=ufs->Verified=ufs->OwnedIdentitySaved=TRUE;ufs->Domain=(VOID *)321;
  ufs->OwnedTablePhysical=0x81000000;ufs->TableMemory.Signature=1;ufs->After=ufs->AttachedSnapshot=now;
  ufs->Api=api;
  usb->Before=now;usb->DeviceIndex=1;usb->ResourceName="USB0";usb->Domain=(VOID *)123;usb->Api=api;
  usb->Attached=usb->Verified=usb->OwnedIdentitySaved=TRUE;usb->OwnedTablePhysical=0x82000000;
  usb->TableMemory.Signature=1;usb->TableMemory.Physical=0x82000000;
  now.Device[1]=identity(0x40,1,1,0x82000000);now.RawSmr[1]=0x40;now.RawS2cr[1]=BIT10|1;
  usb->After=usb->AttachedSnapshot=now;
  now.Device[1]=(PIANO_SMMU_DEVICE){.Sid=0x40};now.RawSmr[1]=now.RawS2cr[1]=0;capture=now;
  detached=now;detached.Device[0]=(PIANO_SMMU_DEVICE){.Sid=0x60};detached.RawSmr[0]=detached.RawS2cr[0]=0;
  execution=(PIANO_SMMU_USB_RETIRE_EVIDENCE){.Revision=1,.DeviceCleanupStatus=EFI_SUCCESS,.ControllerCleanupStatus=EFI_SUCCESS,
    .DeviceHalted=TRUE,.DmaFreed=TRUE,.ClocksReleased=TRUE,.GdscReleased=TRUE,.DmaBuffersFreed=9,.ClockReleaseMask=0xff};
  detach_calls=destroy_calls=free_calls=0;detach_code=destroy_code=0;capture_status=free_status=EFI_SUCCESS;
}
static void close_usb(void){assert(PianoOwnedSmmuClose(usb)==EFI_SUCCESS && usb->CloseLedger.ExactClose);assert(detach_calls==1 && destroy_calls==1 && free_calls==1);}
static void valid(void){
  assert(PianoOwnedSmmuMakeRetiredUsbProof(usb,&execution,&now,&proof)==EFI_SUCCESS && proof.Valid && proof.Slot==1);
  assert(PianoOwnedSmmuPrepareRetiredUsbContract(ufs,&proof,&now,&contract)==EFI_SUCCESS && contract.Valid);
  assert(contract.PeerSlot==1 && contract.OwnerSlot==0 && contract.BaselineSmr==0x540 && contract.BaselineS2cr==0x20001);
  assert(!contract.RetiredSmr && !contract.RetiredS2cr);
  assert(PianoOwnedSmmuCheckRetiredUsbContract(ufs,&contract,&detached)==EFI_SUCCESS);
}
int main(void){
  usb=calloc(1,sizeof(*usb));ufs=calloc(1,sizeof(*ufs));assert(usb&&ufs);api[3]=(UINTN)detach;api[1]=(UINTN)destroy;
  setup();assert(PianoOwnedSmmuMakeRetiredUsbProof(usb,&execution,&now,&proof)!=EFI_SUCCESS && !proof.Valid);
  close_usb();valid();
  // Pure contract never changes either baseline nor the original strict close.
  assert(ufs->Before.RawSmr[1]==0x540 && ufs->Before.RawS2cr[1]==0x20001);
  for(UINTN Case=0;Case<24;Case++){
    setup();close_usb();
    switch(Case){
      case 0:execution.DeviceHalted=FALSE;break;case 1:execution.DmaBuffersFreed=8;break;
      case 2:execution.DmaFreed=FALSE;break;case 3:execution.ClockReleaseMask=0xfe;break;
      case 4:execution.GdscReleased=FALSE;break;case 5:execution.ControllerCleanupStatus=EFI_WARN_STALE_DATA;break;
      case 6:usb->CloseLedger.Uncertain=TRUE;break;case 7:usb->CloseLedger.TableFreeAttempted=FALSE;break;
      case 8:usb->CloseLedger.TableFreeStatus=EFI_WARN_STALE_DATA;break;case 9:usb->Domain=(VOID *)123;break;
      case 10:usb->Mapping[0].Used=TRUE;break;case 11:usb->Used[0]=1;break;
      case 12:usb->AttachedSnapshot.Device[1].Sid=0x41;break;case 13:usb->AttachedSnapshot.Device[1].Mask=1;break;
      case 14:usb->AttachedSnapshot.Device[1].Ttbr0^=4096;break;case 15:usb->OwnedIdentitySaved=FALSE;break;
      case 16:now.RawSmr[1]=0x540;break;case 17:now.RawS2cr[1]=0x20001;break;
      case 18:now.RawS2cr[113]^=1;break;case 19:now.Device[0].Tcr^=1;break;
      case 20:now.Device[2].Mair0^=1;break;case 21:now.Groups--;break;
      case 22:now.Device[1].Present=TRUE;break;case 23:usb->TableMemory.Signature=1;break;
    }
    assert(PianoOwnedSmmuMakeRetiredUsbProof(usb,&execution,&now,&proof)!=EFI_SUCCESS && !proof.Valid);
  }
  for(UINTN Case=0;Case<10;Case++){
    setup();close_usb();assert(PianoOwnedSmmuMakeRetiredUsbProof(usb,&execution,&now,&proof)==EFI_SUCCESS);
    switch(Case){case 0:ufs->Before.RawS2cr[1]^=1;break;case 1:ufs->Before.RawSmr[1]^=1;break;
      case 2:ufs->OwnedTablePhysical^=4096;break;case 3:ufs->Attached=FALSE;break;
      case 4:now.Device[0].Tcr^=1;break;case 5:now.RawS2cr[113]^=1;break;case 6:now.RawS2cr[1]=1;break;
      case 7:ufs->After.Device[0].StreamIndex=1;break;case 8:proof.Slot=113;break;case 9:proof.Valid=FALSE;break;}
    assert(PianoOwnedSmmuPrepareRetiredUsbContract(ufs,&proof,&now,&contract)!=EFI_SUCCESS && !contract.Valid);
  }
  setup();close_usb();valid();detached.RawS2cr[113]^=1;assert(PianoOwnedSmmuCheckRetiredUsbContract(ufs,&contract,&detached)!=EFI_SUCCESS);
  setup();close_usb();valid();detached.RawS2cr[1]=0x20001;assert(PianoOwnedSmmuCheckRetiredUsbContract(ufs,&contract,&detached)!=EFI_SUCCESS);
  setup();close_usb();valid();ufs->Before.RawSmr[1]^=1;assert(PianoOwnedSmmuCheckRetiredUsbContract(ufs,&contract,&detached)!=EFI_SUCCESS);
  setup();close_usb();valid();contract.PeerSlot=113;contract.BaselineSmr=0;contract.BaselineS2cr=0x0300006e;
  assert(PianoOwnedSmmuCheckRetiredUsbContract(ufs,&contract,&detached)!=EFI_SUCCESS);
  setup();close_usb();valid();contract.PeerTablePhysical^=4096;assert(PianoOwnedSmmuCheckRetiredUsbContract(ufs,&contract,&detached)!=EFI_SUCCESS);
  // Execute the actual close loop: only an explicit validated contract allows
  // this single retired peer; unknown other row changes still retain tables.
  setup();close_usb();valid();ufs->RetiredUsbContract=&contract;capture=detached;
  assert(PianoOwnedSmmuClose(ufs)==EFI_SUCCESS && ufs->CloseLedger.ExactClose && !ufs->Domain && !ufs->TableMemory.Signature);
  assert(detach_calls==2 && destroy_calls==2 && free_calls==2 && ufs->Before.RawSmr[1]==0x540 && ufs->Before.RawS2cr[1]==0x20001);
  setup();close_usb();valid();capture=detached;
  assert(PianoOwnedSmmuClose(ufs)==EFI_COMPROMISED_DATA && ufs->TableMemory.Quarantined && destroy_calls==1 && free_calls==1);
  setup();close_usb();valid();ufs->RetiredUsbContract=&contract;capture=detached;capture.RawS2cr[113]^=1;
  assert(PianoOwnedSmmuClose(ufs)==EFI_COMPROMISED_DATA && ufs->TableMemory.Quarantined && destroy_calls==1 && free_calls==1);
  setup();close_usb();valid();ufs->RetiredUsbContract=&contract;capture=detached;capture.RawS2cr[1]=0x20001;
  assert(PianoOwnedSmmuClose(ufs)==EFI_COMPROMISED_DATA && destroy_calls==1 && free_calls==1);
  // A device-produced pre-DMA CSFTRST rollback changes only the allocation
  // accounting. Its exact close, identity and live peer checks stay identical.
  setup();close_usb();execution.Kind=PIANO_USB_RETIRE_STARTUP_FAILED_CLEAN;execution.StartupStatus=EFI_TIMEOUT;
  execution.DmaBuffersFreed=0;valid();ufs->RetiredUsbContract=&contract;capture=detached;
  assert(PianoOwnedSmmuClose(ufs)==EFI_SUCCESS && ufs->CloseLedger.ExactClose && destroy_calls==2 && free_calls==2);
  for(UINTN Case=0;Case<10;++Case){
    setup();close_usb();execution.Kind=PIANO_USB_RETIRE_STARTUP_FAILED_CLEAN;execution.StartupStatus=EFI_TIMEOUT;execution.DmaBuffersFreed=0;
    switch(Case){case 0:execution.Kind=PIANO_USB_RETIRE_RUNNING;break;case 1:execution.Kind=2;break;
      case 2:execution.StartupStatus=EFI_SUCCESS;break;case 3:execution.DmaBuffersAllocated=1;break;
      case 4:execution.DmaBuffersFreed=1;break;case 5:execution.DeviceCleanupStatus=EFI_TIMEOUT;break;
      case 6:execution.ClockReleaseMask=0xfe;break;case 7:usb->CloseLedger.Uncertain=TRUE;break;
      case 8:now.RawS2cr[113]^=1;break;case 9:now.Device[0].Tcr^=1;break;}
    assert(PianoOwnedSmmuMakeRetiredUsbProof(usb,&execution,&now,&proof)!=EFI_SUCCESS && !proof.Valid);
  }
  free(usb);free(ufs);puts("Actual retired USB proof/contract: exact real close ledger, Halt+9DMA+clock/GDSC evidence, SID/slot/CB/root identity, equal UFS/USB baseline, unique zero/zero row, strict other slots/contexts and no normalization passed.");return 0;
}
