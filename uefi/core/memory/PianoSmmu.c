// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only SM8750 apps SMMU-v500 snapshot. Never detach/reset/bypass a stream.
#include "PianoSmmu.h"
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>

#define BASE 0x15000000U
#define WINDOW 0x100000U
STATIC UINT32 Be32(CONST UINT8 *P){return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];}
BOOLEAN PianoSmmuStreamMatches(UINT32 Smr,UINT32 S2cr,BOOLEAN Extended,UINT16 Sid) {
  BOOLEAN Valid=Extended?(S2cr&BIT10)!=0:(Smr&BIT31)!=0;
  UINT16 Mask=(UINT16)((Smr>>16)&0x7FFF),Id=(UINT16)Smr;
  return Valid && ((Sid^Id)&~Mask)==0;
}
STATIC EFI_STATUS Validate(CONST VOID *Fdt) {
  INT32 Node=FdtPathOffset(Fdt,"/soc/apps-smmu@15000000"),Len;
  if(Node<0)return EFI_NOT_FOUND;
  CONST UINT8 *P=FdtGetProp(Fdt,Node,"reg",&Len);
  if(P==NULL || Len!=8 || Be32(P)!=BASE || Be32(P+4)!=WINDOW)return EFI_UNSUPPORTED;
  STATIC CONST struct {CONST CHAR8 *Path;UINT16 Sid;} Devices[]={
    {"/soc/ufshc@1d84000",0x60},{"/soc/ssusb@a600000/dwc3@a600000",0x40},
    {"/soc/qcom,gpi-dma@a00000",0xB6},{"/soc/qcom,qupv3_1_geni_se@ac0000",0xA3}
  };
  for(UINTN I=0;I<ARRAY_SIZE(Devices);++I) {
    Node=FdtPathOffset(Fdt,Devices[I].Path);if(Node<0)return EFI_NOT_FOUND;
    P=FdtGetProp(Fdt,Node,"iommus",&Len);
    if(P==NULL || Len!=12 || Be32(P+4)!=Devices[I].Sid || Be32(P+8)!=0)return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}
STATIC UINT32 Read(UINTN Offset) {
  // Register accesses are bounded to the exact DT SMMU window.
  ASSERT(Offset<=WINDOW-4 && (Offset&3)==0);
  return MmioRead32(BASE+Offset);
}
STATIC UINT64 Read64(UINTN Offset){UINT64 Lo=Read(Offset);return Lo|((UINT64)Read(Offset+4)<<32);}
STATIC VOID ReadBank(PIANO_SMMU_SNAPSHOT *S,PIANO_SMMU_DEVICE *D,BOOLEAN Verbose) {
  UINTN B=S->ContextBase+((UINTN)D->ContextBank<<S->PageShift),G=1U<<S->PageShift;
  D->Cbar=Read(G+4*D->ContextBank);D->Cba2r=Read(G+0x800+4*D->ContextBank);
  D->Sctlr=Read(B);D->Enabled=(D->Sctlr&1)!=0;
  D->Tcr2=Read(B+0x10);D->Ttbr0=Read64(B+0x20);D->Ttbr1=Read64(B+0x28);
  D->Tcr=Read(B+0x30);D->Mair0=Read(B+0x38);D->Mair1=Read(B+0x3C);
  D->Fsr=Read(B+0x58);D->Far=Read64(B+0x60);D->Fsynr=Read(B+0x68);
  if(Verbose)DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_CB dev=%a sid=%x bank=%u sctlr=%08x cbar=%08x cba2r=%08x ttbr0=%lx ttbr1=%lx tcr=%08x tcr2=%08x mair=%08x:%08x fsr=%08x far=%lx fsynr=%08x\n",
    D->Name,D->Sid,D->ContextBank,D->Sctlr,D->Cbar,D->Cba2r,D->Ttbr0,D->Ttbr1,D->Tcr,D->Tcr2,D->Mair0,D->Mair1,D->Fsr,D->Far,D->Fsynr));
}
STATIC EFI_STATUS Capture(CONST VOID *Fdt,CONST CHAR8 *Phase,PIANO_SMMU_SNAPSHOT *S,BOOLEAN Verbose) {
  if(S==NULL || Phase==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(S,sizeof(*S));EFI_STATUS Status=Validate(Fdt);
  if(EFI_ERROR(Status))return Status;
  S->Base=BASE;S->Window=WINDOW;
  STATIC CONST CHAR8 *Names[]={"ufs","usb","gpi1","qup1"};
  STATIC CONST UINT16 Sids[]={0x60,0x40,0xB6,0xA3};
  for(UINTN I=0;I<PIANO_SMMU_DEVICE_COUNT;++I){S->Device[I].Name=Names[I];S->Device[I].Sid=Sids[I];}
  if(Verbose)DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_BEGIN phase=%a base=%08x read_only=1\n",Phase,BASE));
  S->Id0=Read(0x20);S->Id1=Read(0x24);S->Id2=Read(0x28);
  S->Groups=S->Id0&0xFF;S->Banks=S->Id1&0xFF;S->PageShift=(S->Id1&BIT31)?16:12;
  S->GlobalControl=Read(0);S->GlobalFault=Read(0x48);
  S->ExtendedIds=(S->Id0&BIT8)!=0 && (S->GlobalControl&BIT3)!=0;
  S->ContextBase=(UINTN)(1U<<(((S->Id1>>28)&7)+1))<<S->PageShift;
  if(Verbose)DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_ID phase=%a id0=%08x id1=%08x id2=%08x groups=%u banks=%u pgshift=%u exids=%u\n",
    Phase,S->Id0,S->Id1,S->Id2,S->Groups,S->Banks,S->PageShift,S->ExtendedIds));
  if(S->Groups==0 || S->Groups>256 || S->Banks==0 || S->Banks>256 ||
     (UINT64)S->ContextBase+((UINT64)S->Banks<<S->PageShift)>WINDOW)return EFI_UNSUPPORTED;
  if(Verbose)DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_GLOBAL phase=%a control=%08x gfsr=%08x\n",Phase,S->GlobalControl,S->GlobalFault));
  for(UINTN G=0;G<S->Groups;++G) {
    UINT32 Smr=Read(0x800+4*G),S2cr=Read(0xC00+4*G);
    S->RawSmr[G]=Smr;S->RawS2cr[G]=S2cr;
    if((Smr&BIT31) || (S2cr&BIT10) || Smr || S2cr) {
      if(Verbose)DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_RAW phase=%a idx=%u smr=%08x s2cr=%08x legacy_valid=%u exid_valid=%u\n",
        Phase,(UINT32)G,Smr,S2cr,(Smr&BIT31)!=0,(S2cr&BIT10)!=0));
    }
    for(UINTN I=0;I<PIANO_SMMU_DEVICE_COUNT;++I) {
      PIANO_SMMU_DEVICE *D=&S->Device[I];
      if(!PianoSmmuStreamMatches(Smr,S2cr,S->ExtendedIds,D->Sid))continue;
      // Multiple matches are ambiguous and cannot authorize a DMA mapping.
      if(D->Present){if(Verbose)DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_AMBIGUOUS dev=%a sid=%x\n",D->Name,D->Sid));return EFI_COMPROMISED_DATA;}
      D->Present=TRUE;D->StreamIndex=(UINT16)G;D->Smr=Smr;D->S2cr=S2cr;
      D->Mask=(UINT16)((Smr>>16)&0x7FFF);D->Type=(UINT8)((S2cr>>16)&3);D->ContextBank=(UINT8)S2cr;
      if(Verbose)DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_STREAM phase=%a dev=%a sid=%x idx=%u smr=%08x s2cr=%08x type=%u cb=%u\n",Phase,D->Name,D->Sid,D->StreamIndex,Smr,S2cr,D->Type,D->ContextBank));
      if(D->Type==0) {
        if(D->ContextBank>=S->Banks)return EFI_COMPROMISED_DATA;
        ReadBank(S,D,Verbose);
      }
    }
  }
  for(UINTN I=0;I<PIANO_SMMU_DEVICE_COUNT;++I)if(!S->Device[I].Present)
    if(Verbose)DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_NO_STREAM phase=%a dev=%a sid=%x\n",Phase,S->Device[I].Name,S->Device[I].Sid));
  S->Valid=TRUE;if(Verbose)DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_END phase=%a status=Success changed_registers=0\n",Phase));return EFI_SUCCESS;
}
EFI_STATUS PianoSmmuCapture(CONST VOID *Fdt,CONST CHAR8 *Phase,PIANO_SMMU_SNAPSHOT *S){return Capture(Fdt,Phase,S,TRUE);}
EFI_STATUS PianoSmmuCaptureQuiet(CONST VOID *Fdt,PIANO_SMMU_SNAPSHOT *S){return Capture(Fdt,"quiet",S,FALSE);}
VOID PianoSmmuLogFaults(CONST PIANO_SMMU_SNAPSHOT *S) {
  if(S==NULL || !S->Valid)return;
  UINT32 Global=Read(0x48);
  DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_FAULT global=%08x\n",Global));
  for(UINTN I=0;I<PIANO_SMMU_DEVICE_COUNT;++I) {
    CONST PIANO_SMMU_DEVICE *D=&S->Device[I];if(!D->Present || D->Type!=0)continue;
    UINTN B=S->ContextBase+((UINTN)D->ContextBank<<S->PageShift);UINT32 Fsr=Read(B+0x58);
    DEBUG((DEBUG_WARN,"SUNUEFI_SMMU_FAULT dev=%a sid=%x cb=%u fsr=%08x far=%lx fsynr=%08x format=%x fault_bits=%08x\n",
      D->Name,D->Sid,D->ContextBank,Fsr,Read64(B+0x60),Read(B+0x68),Fsr&PIANO_SMMU_FSR_FORMAT_MASK,Fsr&PIANO_SMMU_FSR_FAULT_MASK));
  }
}
