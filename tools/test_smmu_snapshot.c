// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoSmmu.c"
static UINT32 regs[WINDOW/4];static int bad_dt;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){ }
VOID EFIAPI DebugAssert(CONST CHAR8 *File,UINTN Line,CONST CHAR8 *Description){assert(!Description);}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
UINT32 EFIAPI MmioRead32(UINTN A){assert(A>=BASE && A<=BASE+WINDOW-4 && !(A&3));return regs[(A-BASE)/4];}
INT32 EFIAPI FdtPathOffset(CONST VOID *F,CONST CHAR8 *Path){
  if(strstr(Path,"apps-smmu"))return 1;
  if(strstr(Path,"ufshc"))return 2;
  if(strstr(Path,"dwc3"))return 3;
  if(strstr(Path,"gpi-dma"))return 4;
  return 5;
}
CONST VOID *EFIAPI FdtGetProp(CONST VOID *F,INT32 Node,CONST CHAR8 *Name,INT32 *Length){
  static UINT8 reg[]={0x15,0,0,0,0,0x10,0,0},iommu[12];
  if(Node==1){*Length=8;return reg;}
  static const unsigned sid[]={0,0,0x60,0x40,0xb6,0xa3};
  memset(iommu,0,sizeof(iommu));iommu[7]=bad_dt?0xff:sid[Node];*Length=12;return iommu;
}
static void initial(void){
  memset(regs,0,sizeof(regs));regs[0x20/4]=BIT27|4;regs[0x24/4]=(6U<<28)|128;
  regs[0x800/4]=BIT31|0x60;regs[0xc00/4]=2;
  regs[0x804/4]=BIT31|0x40;regs[0xc04/4]=1U<<16;
  regs[0x808/4]=BIT31|0xb6;regs[0xc08/4]=2U<<16;
  unsigned b=0x80000+2*0x1000;regs[b/4]=1;regs[(b+0x20)/4]=0xc0001000;
  regs[(b+0x58)/4]=4;regs[(b+0x60)/4]=0xdead0000;
}
int main(void){
  assert(PianoSmmuStreamMatches(BIT31|0x60,0,FALSE,0x60));
  assert(!PianoSmmuStreamMatches(0x60,0,FALSE,0x60));
  assert(PianoSmmuStreamMatches((0x1fU<<16)|0x60,BIT10,TRUE,0x7f));
  assert(!PianoSmmuStreamMatches(BIT31|0x60,0,TRUE,0x60));
  initial();PIANO_SMMU_SNAPSHOT s;
  assert(PianoSmmuCapture(NULL,"test",&s)==EFI_SUCCESS && s.Valid);
  assert(s.ContextBase==0x80000 && s.Device[0].ContextBank==2 && s.Device[0].Enabled);
  assert(s.Device[0].Ttbr0==0xc0001000 && s.Device[0].Fsr==4 && s.Device[0].Far==0xdead0000);
  assert(s.Device[1].Type==1 && s.Device[2].Type==2 && !s.Device[3].Present);
  UINT32 copy[8];memcpy(copy,regs+0x800/4,sizeof(copy));PianoSmmuLogFaults(&s);
  assert(memcmp(copy,regs+0x800/4,sizeof(copy))==0);
  regs[0x80c/4]=BIT31|0x60;regs[0xc0c/4]=0;
  assert(PianoSmmuCapture(NULL,"duplicate",&s)==EFI_COMPROMISED_DATA && !s.Valid);
  initial();regs[0xc00/4]=128;
  assert(PianoSmmuCapture(NULL,"bad-bank",&s)==EFI_COMPROMISED_DATA);
  initial();bad_dt=1;assert(PianoSmmuCapture(NULL,"bad-sid",&s)==EFI_UNSUPPORTED);bad_dt=0;
  initial();regs[0x20/4]|=BIT8;regs[0]|=BIT3;regs[0xc00/4]|=BIT10;
  assert(PianoSmmuCapture(NULL,"extended",&s)==EFI_SUCCESS && s.Device[0].Present && s.ExtendedIds);
  puts("Read-only SMMU snapshot: stream masks, active extended IDs, register bounds, context geometry and ambiguity rejection passed.");
  return 0;
}
