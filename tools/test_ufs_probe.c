// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoUfsProbe.c"
EFI_BOOT_SERVICES *gBS;static EFI_BOOT_SERVICES bs;static EFI_CLOCK_PROTOCOL clock;
static unsigned reads,enables,disables,ids;static int reset_failure;static UINTN fail_resource;static int fail_ice,valid_dt=1;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){ }
UINT32 EFIAPI MmioRead32(UINTN A){assert(A>=HCI && A<=HCI+0x300 && !(A&3));++reads;return 0x1234;}
INT32 EFIAPI FdtPathOffset(CONST VOID *Fdt,CONST CHAR8 *Path){return valid_dt?1:-1;}
CONST VOID *EFIAPI FdtGetProp(CONST VOID *Fdt,INT32 Node,CONST CHAR8 *Name,INT32 *Length){
  static UINT8 reg[32],iommu[12];UINT32 values[]={HCI,0x3000,0x01d88000,0x18000,0x01da5000,0x2000,0x01da4000,0x10};
  for(unsigned i=0;i<8;++i)for(unsigned j=0;j<4;++j)reg[i*4+j]=(UINT8)(values[i]>>((3-j)*8));
  iommu[7]=0x60;
  if(strcmp(Name,"reg")==0){*Length=32;return reg;}
  *Length=12;return iommu;
}
static EFI_STATUS EFIAPI locate(EFI_GUID *G,VOID *Registration,VOID **Out){*Out=&clock;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI id(EFI_CLOCK_PROTOCOL *C,CONST CHAR8 *Name,UINTN *Id){
  if(fail_ice && strcmp(Name,"gcc_ufs_phy_ice_core_clk")==0)return EFI_NOT_FOUND;
  *Id=++ids;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI enable(EFI_CLOCK_PROTOCOL *C,UINTN Id){++enables;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI disable(EFI_CLOCK_PROTOCOL *C,UINTN Id){++disables;return reset_failure && (!fail_resource || Id==fail_resource)?reset_failure==1?EFI_DEVICE_ERROR:EFI_WARN_UNKNOWN_GLYPH:EFI_SUCCESS;}
int main(void){
  gBS=&bs;bs.LocateProtocol=locate;clock.Version=0x1000b;
  clock.GetClockID=id;clock.EnableClock=enable;clock.DisableClock=disable;
  clock.GetClockPowerDomainID=id;clock.EnableClockPowerDomain=enable;clock.DisableClockPowerDomain=disable;
  UINT32 v;assert(ReadRegister(0x100,&v)==EFI_ACCESS_DENIED && !reads);
  assert(ReadRegister(0,NULL)==EFI_ACCESS_DENIED && !reads);
  PianoProbeUfs(NULL);assert(reads==17 && enables==11 && disables==11);
  reads=enables=disables=ids=0;fail_ice=1;
  PianoProbeUfs(NULL);assert(!reads && enables==5 && disables==5);
  fail_ice=0;valid_dt=0;enables=disables=0;
  PianoProbeUfs(NULL);assert(!reads && !enables && !disables);
  valid_dt=1;fail_ice=0;reads=enables=disables=ids=0;
  mHeldClock=&clock;mHeldDomain=10;mDomainHeld=TRUE;mHeldCount=2;mHeldIds[0]=11;mHeldIds[1]=12;
  assert(PianoUfsStopClocksForReset()==EFI_SUCCESS && !mHeldClock && !mDomainHeld && !mHeldCount && disables==3);
  assert(PianoUfsStopClocksForReset()==EFI_SUCCESS && disables==3);
  mResetClockStarted=mResetClockClean=FALSE;mHeldClock=&clock;mDomainHeld=TRUE;mHeldCount=2;reset_failure=1;
  assert(PianoUfsStopClocksForReset()==EFI_DEVICE_ERROR && mHeldClock && mDomainHeld && mHeldCount==2);
  unsigned before=disables;assert(PianoUfsStopClocksForReset()==EFI_DEVICE_ERROR && disables==before);
  mResetClockStarted=mResetClockClean=FALSE;reset_failure=2;
  assert(PianoUfsStopClocksForReset()==EFI_DEVICE_ERROR && mHeldCount==2 && mDomainHeld);
  mResetClockStarted=mResetClockClean=FALSE;reset_failure=1;fail_resource=mHeldIds[0];
  assert(PianoUfsStopClocksForReset()==EFI_DEVICE_ERROR && mHeldCount==1 && mDomainHeld);
  before=disables;PianoUfsStopClocks();assert(disables==before && mHeldCount==1 && mHeldClock);
  mResetClockStarted=mResetClockClean=FALSE;mHeldCount=0;fail_resource=mHeldDomain;
  assert(PianoUfsStopClocksForReset()==EFI_DEVICE_ERROR && mHeldCount==0 && mDomainHeld && mHeldClock);
  puts("UFS controller probe: exact DT/SID, read-only HCI register allowlist and balanced clock/domain cleanup passed.");
  return 0;
}
