// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fixed, protected MDSS SMMUv2 observation. No writes, reset or DMA authority.
#include "PianoDisplaySmmuObserve.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#define BASE 0x15000000ULL
#define BANK (BASE+0x82000)
STATIC PIANO_DISPLAY_SMMU_REPORT mReport={.Revision=1};
STATIC PIANO_DISPLAY_SMMU_REGISTERS mSecond;
STATIC PIANO_DISPLAY_SMMU_ALIVE mAlive;
STATIC BOOLEAN mBusy;
STATIC BOOLEAN GuardAlive(VOID *Context){
  return Context==&mReport && !mReport.ServicesLost && mAlive && mAlive()==TRUE;
}
STATIC EFI_STATUS Read32(VOID *Token,UINT64 Address,UINT32 *Value){
  return PianoGuardedRead32(Token,Address,Value);
}
STATIC EFI_STATUS Global(VOID *Token,PIANO_DISPLAY_SMMU_REGISTERS *R){
  UINT32 *Out[]={&R->Control,&R->Id0,&R->Id1,&R->Id2,&R->GlobalFault};
  CONST UINT32 Off[]={0,0x20,0x24,0x28,0x48};
  for(UINTN I=0;I<ARRAY_SIZE(Off);++I){EFI_STATUS S=Read32(Token,BASE+Off[I],Out[I]);if(S!=EFI_SUCCESS)return S;}
  return EFI_SUCCESS;
}
STATIC BOOLEAN Geometry(CONST PIANO_DISPLAY_SMMU_REGISTERS *R){
  // Exact local SM8750 capture: 127 groups/83 CBs/4K pages/CB offset80000.
  // Refuse extended-ID interpretation or globally disabled/bypass translation.
  return R->Id0==0x4c017e7f && R->Id1==0x60000053 && R->Id2==0x5111 && !(R->Control&(BIT0|BIT3));
}
STATIC EFI_STATUS Routes(VOID *Token,PIANO_DISPLAY_SMMU_REGISTERS *R){
  R->Slot=MAX_UINT32;
  for(UINT32 I=0;I<PIANO_DISPLAY_SMMU_ROUTES;++I){
    EFI_STATUS S=Read32(Token,BASE+0x800+I*4,&R->Smr[I]);if(S!=EFI_SUCCESS)return S;
    S=Read32(Token,BASE+0xc00+I*4,&R->S2cr[I]);if(S!=EFI_SUCCESS)return S;
    UINT32 Smr=R->Smr[I],Mask=(Smr>>16)&0x7fff;
    // Include any live route that aliases either SID800 or SID802; only the
    // single exact800/mask2 shape may authorize fixed CB2 observation below.
    if((Smr&BIT31) && (((((UINT16)Smr)^0x800)&~Mask)==0 ||
                            ((((UINT16)Smr)^0x802)&~Mask)==0)){
      ++R->Matches;R->Slot=I;R->SelectedSmr=Smr;R->SelectedS2cr=R->S2cr[I];
    }
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Bank(VOID *Token,PIANO_DISPLAY_SMMU_REGISTERS *R){
  UINT32 *Out[]={&R->Cbar,&R->Cba2r,&R->Sctlr,&R->Tcr2,&R->Tcr,&R->Mair0,&R->Mair1,&R->Fsr,&R->Fsynr};
  CONST UINT64 Address[]={BASE+0x1008,BASE+0x1808,BANK,BANK+0x10,BANK+0x30,BANK+0x38,BANK+0x3c,BANK+0x58,BANK+0x68};
  for(UINTN I=0;I<ARRAY_SIZE(Address);++I){EFI_STATUS S=Read32(Token,Address[I],Out[I]);if(S!=EFI_SUCCESS)return S;}
  UINT64 *LongOut[]={&R->Ttbr0,&R->Ttbr1,&R->Far};CONST UINT32 Off[]={0x20,0x28,0x60};
  for(UINTN I=0;I<ARRAY_SIZE(Off);++I){UINT32 Lo,Hi;EFI_STATUS S=Read32(Token,BANK+Off[I],&Lo);if(S!=EFI_SUCCESS)return S;
    S=Read32(Token,BANK+Off[I]+4,&Hi);if(S!=EFI_SUCCESS)return S;*LongOut[I]=Lo|((UINT64)Hi<<32);}
  return EFI_SUCCESS;
}
STATIC VOID Emit(CONST PIANO_DISPLAY_SMMU_SNAPSHOT *S){
  CONST PIANO_DISPLAY_SMMU_REGISTERS *R=&S->Registers;
  DEBUG((DEBUG_WARN,"PIANO_MDSS_SMMU phase=%a status=%r reason=%u bank_read=%u coherent=%u retained=%u lost=%u observe_only=1\n",
    S->Phase,S->Status,S->Reason,S->BankRead,S->Coherent,S->Retained,S->ServicesLost));
  DEBUG((DEBUG_WARN,"PIANO_MDSS_GLOBAL ctl=%08x id0=%08x id1=%08x id2=%08x gfsr=%08x matches=%u slot=%u\n",
    R->Control,R->Id0,R->Id1,R->Id2,R->GlobalFault,R->Matches,R->Slot));
  DEBUG((DEBUG_WARN,"PIANO_MDSS_ROUTE smr=%08x s2cr=%08x fixed_cb=2 sctlr=%08x cbar=%08x cba2r=%08x fsr=%08x\n",
    R->SelectedSmr,R->SelectedS2cr,R->Sctlr,R->Cbar,R->Cba2r,R->Fsr));
  DEBUG((DEBUG_WARN,"PIANO_MDSS_CONTEXT ttbr0=%lx ttbr1=%lx tcr=%08x tcr2=%08x mair=%08x:%08x far=%lx fsynr=%08x\n",
    R->Ttbr0,R->Ttbr1,R->Tcr,R->Tcr2,R->Mair0,R->Mair1,R->Far,R->Fsynr));
  DEBUG((DEBUG_WARN,"PIANO_MDSS_GUARD begin=%r end=%r reads=%u recovered=%u pages=%u active=%u handlers=%u/%u fatal=%u\n",
    S->BeginStatus,S->EndStatus,S->Guard.Reads,S->Guard.RecoveredFaults,S->Guard.PagesValidated,
    S->Guard.Active,S->Guard.SyncOwned,S->Guard.SErrorOwned,S->Guard.Fatal));
  DEBUG((DEBUG_WARN,"PIANO_MDSS_MAPPING page=%lx par=%lx gcd_type=%u attrs=%lx status=%r\n",
    S->Guard.LastMappingPage,S->Guard.LastPar,S->Guard.LastGcdType,S->Guard.LastGcdAttributes,S->Guard.MappingStatus));
  if(S->Guard.RecoveredFaults||S->Guard.Fatal)DEBUG((DEBUG_WARN,"PIANO_MDSS_FAULT pc=%lx esr=%lx far=%lx spsr=%lx resume=%lx\n",
    S->Guard.Elr,S->Guard.Esr,S->Guard.Far,S->Guard.Spsr,S->Guard.Resume));
}
EFI_STATUS PianoDisplaySmmuObserve(CONST CHAR8 *Phase,PIANO_DISPLAY_SMMU_ALIVE Alive){
  if(Phase==NULL || Alive==NULL)return EFI_INVALID_PARAMETER;
  UINTN Length=0;while(Length<32&&Phase[Length])++Length;if(!Length||Length==32)return EFI_INVALID_PARAMETER;
  if(mBusy)return EFI_ALREADY_STARTED;if(mReport.Retained||mReport.ServicesLost)return EFI_NOT_READY;
  if(mReport.Count==PIANO_DISPLAY_SMMU_PHASES)return EFI_OUT_OF_RESOURCES;
  mBusy=TRUE;mAlive=Alive;
  PIANO_DISPLAY_SMMU_SNAPSHOT *S=&mReport.Snapshot[mReport.Count++];ZeroMem(S,sizeof(*S));CopyMem(S->Phase,Phase,Length);
  S->Status=S->BeginStatus=S->EndStatus=EFI_NOT_STARTED;S->Reason=PianoDisplaySmmuGuard;
  PIANO_GUARDED_CONFIG Config={.Context=&mReport,.Services=gBS,.DxeServices=gDS,.BootServicesAlive=GuardAlive,
    .Ranges={{BASE,0xe00,EfiGcdMemoryTypeMemoryMappedIo,EFI_MEMORY_UC,0},
      {BASE+0x1000,0x1000,EfiGcdMemoryTypeMemoryMappedIo,EFI_MEMORY_UC,0},
      {BANK,0x70,EfiGcdMemoryTypeMemoryMappedIo,EFI_MEMORY_UC,0}},
    .RangeCount=3,.MaxReads=1024,.MaxUsecs=100000};
  VOID *Token=NULL;S->BeginStatus=PianoGuardedReadBegin(&Config,&Token);S->Status=S->BeginStatus;
  if(S->BeginStatus!=EFI_SUCCESS)goto Finish;
  if(Token==NULL){S->Status=EFI_COMPROMISED_DATA;mReport.Retained=TRUE;goto Finish;}
  S->Reason=PianoDisplaySmmuRead;
  S->Status=Global(Token,&S->Registers);if(S->Status!=EFI_SUCCESS)goto End;
  if(!Geometry(&S->Registers)){S->Status=EFI_NOT_READY;S->Reason=PianoDisplaySmmuGeometry;goto End;}
  S->Status=Routes(Token,&S->Registers);if(S->Status!=EFI_SUCCESS)goto End;
  if(S->Registers.Matches!=1){S->Status=EFI_NOT_READY;S->Reason=S->Registers.Matches?PianoDisplaySmmuAmbiguous:PianoDisplaySmmuMissing;goto End;}
  if(S->Registers.SelectedSmr!=0x80020800 || (S->Registers.SelectedS2cr&0x000300ff)!=2){
    S->Status=EFI_NOT_READY;S->Reason=PianoDisplaySmmuShape;goto End;}
  S->Status=Bank(Token,&S->Registers);if(S->Status!=EFI_SUCCESS)goto End;S->BankRead=TRUE;
  ZeroMem(&mSecond,sizeof(mSecond));S->Status=Global(Token,&mSecond);
  if(S->Status==EFI_SUCCESS)S->Status=Routes(Token,&mSecond);
  if(S->Status==EFI_SUCCESS && (!Geometry(&mSecond) || mSecond.Matches!=1 ||
     mSecond.SelectedSmr!=0x80020800 || (mSecond.SelectedS2cr&0x000300ff)!=2)){
    S->Status=EFI_NOT_READY;S->Reason=PianoDisplaySmmuChanged;goto End;}
  if(S->Status==EFI_SUCCESS)S->Status=Bank(Token,&mSecond);
  if(S->Status!=EFI_SUCCESS)goto End;
  if(CompareMem(&S->Registers,&mSecond,sizeof(mSecond))){S->Status=EFI_NOT_READY;S->Reason=PianoDisplaySmmuChanged;goto End;}
  S->Coherent=TRUE;S->Reason=PianoDisplaySmmuComplete;
End:
  // Even a failed data read must attempt exact End; the guard's retained/EBS
  // path itself performs no BS cleanup. Parent will halt on any uncertainty.
  S->EndStatus=PianoGuardedReadEnd(Token);
  if(S->EndStatus!=EFI_SUCCESS){S->Status=S->EndStatus;S->Reason=PianoDisplaySmmuCleanup;mReport.Retained=TRUE;}
Finish:
  CopyMem(&S->Guard,PianoGuardedReadReport(),sizeof(S->Guard));
  S->Retained=mReport.Retained||S->Guard.Retained||S->Guard.ServicesLost||S->Guard.Fatal||
    S->Guard.Active||S->Guard.SyncOwned||S->Guard.SErrorOwned;
  S->ServicesLost=S->Guard.ServicesLost||Alive()!=TRUE;
  if(S->ServicesLost){S->Status=EFI_ABORTED;S->Reason=PianoDisplaySmmuLost;S->Retained=TRUE;mReport.ServicesLost=TRUE;}
  if(S->Retained)mReport.Retained=TRUE;
  if(!S->Retained)mAlive=NULL;
  ZeroMem(&mSecond,sizeof(mSecond));
  if(!S->ServicesLost)Emit(S);
  mBusy=FALSE;return S->Status;
}
EFI_STATUS PianoDisplaySmmuReemit(PIANO_DISPLAY_SMMU_ALIVE Alive){
  if(Alive==NULL)return EFI_INVALID_PARAMETER;if(mBusy)return EFI_ALREADY_STARTED;
  if(mReport.ServicesLost||Alive()!=TRUE){mReport.Retained=mReport.ServicesLost=TRUE;return EFI_ABORTED;}
  mBusy=TRUE;
  for(UINT32 I=0;I<mReport.Count;++I){if(Alive()!=TRUE){mReport.Retained=mReport.ServicesLost=TRUE;mBusy=FALSE;return EFI_ABORTED;}Emit(&mReport.Snapshot[I]);}
  mBusy=FALSE;return EFI_SUCCESS;
}
BOOLEAN PianoDisplaySmmuRetained(VOID){return mReport.Retained;}
CONST PIANO_DISPLAY_SMMU_REPORT *PianoDisplaySmmuGetReport(VOID){return &mReport;}
