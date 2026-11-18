// SPDX-License-Identifier: BSD-2-Clause-Patent
// QMP orientation changes through the production PHY code with fake MMIO.
#include "../../uefi/core/usb/PianoUsbPhy.c"
#include <assert.h>
#include <stdio.h>

static EFI_BOOT_SERVICES BootServices;
EFI_BOOT_SERVICES *gBS=&BootServices;
static UINT8 Ss[0x2000/4],Hs[0x200/4];
static UINTN Selections;
static UINT32 Dctl,Dsts;
static UINT8 FailingOrientation;
static BOOLEAN FailAll,IgnoreStop,IgnoreSelect,AllowHs;

VOID EFIAPI MemoryFence(VOID){}
static EFI_STATUS EFIAPI Stall(UINTN Us){
  assert(Us==100);
  return EFI_SUCCESS;
}
UINT32 EFIAPI MmioRead32(UINTN Address){
  if(Address==0xA60C704)return Dctl;
  if(Address==0xA60C70C)return Dsts;
  if(Address==SS+0x1E14)
    return FailAll || FailingOrientation==1+(Ss[0x10/4]&1)?BIT6:0;
  if(Address>=HS && Address<HS+4*sizeof(Hs)){
    assert(AllowHs);
    return (UINT32)Hs[(Address-HS)/4]*0x01010101U;
  }
  assert(Address>=SS && Address<SS+4*sizeof(Ss));
  return (UINT32)Ss[(Address-SS)/4]*0x01010101U;
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value){
  assert(!(Dctl&BIT31) && (Dsts&BIT22));
  if(Address>=HS && Address<HS+4*sizeof(Hs)){
    assert(AllowHs);
    Hs[(Address-HS)/4]=(UINT8)Value;
    return Value;
  }
  assert(Address>=SS && Address<SS+4*sizeof(Ss));
  UINTN Offset=Address-SS;
  if(Offset==0x10){
    assert(!(Ss[0x1E44/4]&3) && (Ss[0x1E00/4]&1) && (Ss[4/4]&1) && !(Ss[8/4]&1));
    ++Selections;
    if(IgnoreSelect)return Value;
  }
  if(Offset==0x1E44 && !(Value&3) && IgnoreStop)return Value;
  Ss[Offset/4]=(UINT8)Value;
  return Value;
}
static VOID StartQmp(VOID){
  mPhyOwned=TRUE;
  BootServices.Stall=Stall;
  Dsts=BIT22;
  assert(QmpStop()==EFI_SUCCESS && PianoUsbPhyOrientation()==0);
  assert(QmpStart(1)==EFI_SUCCESS && PianoUsbPhyOrientation()==1);
}
int main(void){
  StartQmp();
  assert(Selections==1 && (Ss[0x10/4]&3)==2 && (Ss[0x1E44/4]&3)==3);
  assert(PianoUsbPhySetOrientation(0)==EFI_INVALID_PARAMETER);
  assert(PianoUsbPhySetOrientation(1)==EFI_SUCCESS && Selections==1);
  Dctl=BIT31;
  assert(PianoUsbPhySetOrientation(2)==EFI_NOT_READY && Selections==1 && PianoUsbPhyOrientation()==1);
  Dctl=Dsts=0;
  assert(PianoUsbPhySetOrientation(2)==EFI_NOT_READY && Selections==1);
  Dsts=BIT22;
  assert(PianoUsbPhySetOrientation(2)==EFI_SUCCESS && Selections==2 && (Ss[0x10/4]&3)==3);
  assert(PianoUsbPhyOrientation()==2);

  FailingOrientation=1;
  assert(PianoUsbPhySetOrientation(1)==EFI_TIMEOUT && Selections==4);
  assert(PianoUsbPhyOrientation()==2 && (Ss[0x10/4]&3)==3 && (Ss[0x1E44/4]&3)==3);
  FailingOrientation=0;
  IgnoreSelect=TRUE;
  assert(PianoUsbPhySetOrientation(1)==EFI_DEVICE_ERROR && Selections==6 && PianoUsbPhyOrientation()==2);
  IgnoreSelect=FALSE;
  FailAll=TRUE;
  assert(PianoUsbPhySetOrientation(1)==EFI_TIMEOUT && Selections==8 && PianoUsbPhyOrientation()==0);
  assert(PianoUsbPhySetOrientation(1)==EFI_NOT_READY && Selections==8);

  FailAll=FALSE;
  StartQmp();
  IgnoreStop=TRUE;
  assert(PianoUsbPhySetOrientation(2)==EFI_DEVICE_ERROR && Selections==9 && PianoUsbPhyOrientation()==0);
  IgnoreStop=FALSE;
  AllowHs=TRUE;
  assert(PianoUsbPhyStop()==EFI_SUCCESS && !mPhyOwned && PianoUsbPhyOrientation()==0);
  puts("QMP only selects lanes while stopped; readback, timeout rollback, and failed stop prevent unsafe restart.");
  return 0;
}
