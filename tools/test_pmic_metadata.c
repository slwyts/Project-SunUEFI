// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoPmicMetadata.c"
EFI_BOOT_SERVICES *gBS;
static EFI_BOOT_SERVICES bs;static UINT32 state=1;static unsigned writes,stalls;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){ }
VOID *EFIAPI CopyMem(VOID *P,CONST VOID *S,UINTN N){return memcpy(P,S,N);}
UINT32 EFIAPI MmioRead32(UINTN A){
  if(A==CORE+0x2000)return 0x701<<8;
  if(A==CFG)return 4; // Reads are observer EE0 regardless of write owner.
  assert(A==OBS+8 || A==OBS+0x18 || A==OBS+0x1c);
  if(A==OBS+8)return state;
  return A==OBS+0x18?0x02010000:0x00005651;
}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V){assert(A==OBS && V==((1U<<27)|6));++writes;return V;}
static EFI_STATUS EFIAPI stall(UINTN N){assert(N==1);++stalls;return EFI_SUCCESS;}
int main(void){
  gBS=&bs;bs.Stall=stall;UINT8 data[7];PM_INFO out;
  assert(ReadRevision(7,1,data)==EFI_SUCCESS && Decode(data,&out)==EFI_SUCCESS);
  assert(out.Model==0x56 && out.AllLayer==2 && out.Metal==1 && out.SlaveCount==2);
  mInfo[7]=out;mValid=0x80;
  assert(GetInfo(7,&out)==EFI_SUCCESS && GetInfo(0,&out)==EFI_NOT_FOUND);
  assert(GetInfo(14,&out)==EFI_NOT_FOUND && GetInfo(7,NULL)==EFI_INVALID_PARAMETER);
  UINT32 primary;assert(GetPrimary(&primary)==EFI_NOT_FOUND);
  mValid|=1;assert(GetPrimary(&primary)==EFI_SUCCESS && primary==0);
  data[4]=0;assert(Decode(data,&out)==EFI_COMPROMISED_DATA);data[4]=0x51;
  data[5]=0x35;assert(Decode(data,&out)==EFI_SUCCESS && out.SlaveCount==1);
  data[5]=0x54;assert(Decode(data,&out)==EFI_SUCCESS && out.SlaveCount==1);
  data[6]=3;assert(Decode(data,&out)==EFI_SUCCESS && out.SlaveCount==3);
  unsigned before=writes;assert(ReadRevision(8,1,data)==EFI_ACCESS_DENIED && writes==before);
  state=5;assert(ReadRevision(7,1,data)==EFI_DEVICE_ERROR);
  state=0;assert(ReadRevision(7,1,data)==EFI_TIMEOUT && stalls==1000);
  assert(mProtocol.Revision==0x10006 && sizeof(PM_INFO)==16 && sizeof(PM_VERSION)==24);
  puts("PMIC metadata: native two-callback ABI, real revision decode, partial-data errors and fixed observer reads passed.");
  return 0;
}
