// SPDX-License-Identifier: BSD-2-Clause-Patent
// Protected control observations. No DPU load without real held-power evidence.
#include "PianoDisplayClockObserve.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
STATIC CONST PIANO_DISPLAY_CLOCK_SPEC mSpecs[]={
  {"gcc_disp_ahb",0x127004,PianoClockGcc},{"gcc_disp_hf_axi",0x127008,PianoClockGcc},
  {"mdss_gdscr",0xaf09000,PianoClockDispcc},{"mdss_cfg_gdscr",0xaf09004,PianoClockDispcc},
  {"mdss_ahb",0xaf080b0,PianoClockDispcc},{"mdss_mdp",0xaf08010,PianoClockDispcc},
  {"dsi_byte0",0xaf08034,PianoClockDispcc},{"dsi_byte1",0xaf0803c,PianoClockDispcc},
  {"dsi_pclk0",0xaf08004,PianoClockDispcc},{"dsi_pclk1",0xaf08008,PianoClockDispcc},
  {"intf1_status",0xae3626c,PianoClockDpu},{"intf2_status",0xae3726c,PianoClockDpu},
  {"intf1_frames",0xae360ac,PianoClockDpu},{"intf2_frames",0xae370ac,PianoClockDpu},
  {"vig0_src0",0xae05014,PianoClockDpu},{"dma0_src0",0xae25014,PianoClockDpu}};
STATIC PIANO_DISPLAY_CLOCK_REPORT mReport={.Revision=1};
STATIC PIANO_DISPLAY_CLOCK_ALIVE mAlive;STATIC BOOLEAN mBusy;
STATIC BOOLEAN GuardAlive(VOID *Context){return Context==&mReport&&!mReport.ServicesLost&&mAlive&&mAlive()==TRUE;}
STATIC VOID KeepGuard(PIANO_DISPLAY_CLOCK_SNAPSHOT *S,PIANO_GUARDED_REPORT *Out){
  CopyMem(Out,PianoGuardedReadReport(),sizeof(*Out));
  if(Out->Retained||Out->ServicesLost||Out->Fatal||Out->Active||Out->SyncOwned||Out->SErrorOwned){S->Retained=mReport.Retained=TRUE;}
  if(Out->ServicesLost){S->ServicesLost=mReport.ServicesLost=TRUE;S->Status=EFI_ABORTED;}
}
STATIC PIANO_GUARDED_CONFIG Config(BOOLEAN Dispcc){
  return (PIANO_GUARDED_CONFIG){.Context=&mReport,.Services=gBS,.DxeServices=gDS,.BootServicesAlive=GuardAlive,
    .Ranges={{Dispcc?0xaf08000ULL:0x127000ULL,Dispcc?0x2000ULL:0x1000ULL,EfiGcdMemoryTypeMemoryMappedIo,EFI_MEMORY_UC,0}},
    .RangeCount=1,.MaxReads=16,.MaxUsecs=100000};
}
STATIC VOID Emit(CONST PIANO_DISPLAY_CLOCK_SNAPSHOT *S){
  DEBUG((DEBUG_WARN,"PIANO_CLOCK_OBSERVE phase=%a status=%r gcc_pair=%u ahb_enable=%u dispcc_map=%u retained=%u lost=%u required_unfinished=1\n",
    S->Phase,S->Status,S->GccCoherent,S->GccAhbEnabled,S->DispccMappingQualified,S->Retained,S->ServicesLost));
  DEBUG((DEBUG_WARN,"PIANO_CLOCK_SESSION gcc=%r/%r dispcc=%r/%r controller_held=%u dpu_held=%u hardware_ready=0\n",
    S->GccBegin,S->GccEnd,S->DispccBegin,S->DispccEnd,S->ControllerBusHeld,S->DpuDomainClockHeld));
  for(UINT32 I=0;I<PIANO_DISPLAY_CLOCK_REGISTERS;++I){CONST PIANO_DISPLAY_CLOCK_REGISTER *R=&S->Register[I];
    DEBUG((DEBUG_WARN,"PIANO_CLOCK_REGISTER i=%u name=%a pa=%lx loads=%u value=%08x/%08x status=%r/%r skip=%u\n",
      I,mSpecs[I].Name,R->Address,R->Attempts,R->Value[0],R->Value[1],R->ReadStatus[0],R->ReadStatus[1],R->Skip));}
  CONST PIANO_GUARDED_REPORT *G[]={&S->GccGuard,&S->DispccGuard};
  for(UINT32 I=0;I<2;++I){DEBUG((DEBUG_WARN,"PIANO_CLOCK_GUARD group=%u status=%r clean=%r reads=%u pages=%u retained=%u handlers=%u/%u\n",
    I,G[I]->Status,G[I]->CleanupStatus,G[I]->Reads,G[I]->PagesValidated,G[I]->Retained,G[I]->SyncOwned,G[I]->SErrorOwned));
    DEBUG((DEBUG_WARN,"PIANO_CLOCK_MAPPING group=%u page=%lx par=%lx type=%u attrs=%lx status=%r\n",
      I,G[I]->LastMappingPage,G[I]->LastPar,G[I]->LastGcdType,G[I]->LastGcdAttributes,G[I]->MappingStatus));
    if(G[I]->RecoveredFaults||G[I]->Fatal)DEBUG((DEBUG_WARN,"PIANO_CLOCK_FAULT group=%u pc=%lx esr=%lx far=%lx recovered=%u fatal=%u\n",
      I,G[I]->Elr,G[I]->Esr,G[I]->Far,G[I]->RecoveredFaults,G[I]->Fatal));
  }
}
EFI_STATUS PianoDisplayClockObserve(CONST CHAR8 *Phase,PIANO_DISPLAY_CLOCK_ALIVE Alive){
  if(!Phase||!Alive)return EFI_INVALID_PARAMETER;UINTN N=0;while(N<32&&Phase[N])++N;if(!N||N==32)return EFI_INVALID_PARAMETER;
  if(mBusy)return EFI_ALREADY_STARTED;if(mReport.Retained||mReport.ServicesLost)return EFI_NOT_READY;
  if(mReport.Count==PIANO_DISPLAY_CLOCK_PHASES)return EFI_OUT_OF_RESOURCES;
  mBusy=TRUE;mAlive=Alive;PIANO_DISPLAY_CLOCK_SNAPSHOT *S=&mReport.Snapshot[mReport.Count++];ZeroMem(S,sizeof(*S));CopyMem(S->Phase,Phase,N);
  S->Status=EFI_NOT_READY;S->GccBegin=S->GccEnd=S->DispccBegin=S->DispccEnd=EFI_NOT_STARTED;
  S->GccGuard.Status=S->GccGuard.CleanupStatus=S->GccGuard.MappingStatus=EFI_NOT_STARTED;
  S->DispccGuard.Status=S->DispccGuard.CleanupStatus=S->DispccGuard.MappingStatus=EFI_NOT_STARTED;
  for(UINT32 I=0;I<PIANO_DISPLAY_CLOCK_REGISTERS;++I){S->Register[I].Address=mSpecs[I].Address;
    S->Register[I].ReadStatus[0]=S->Register[I].ReadStatus[1]=EFI_NOT_STARTED;
    S->Register[I].Skip=I<2?PianoClockNoGccObservation:I<10?PianoClockControllerBusUnproven:PianoClockDpuLeaseMissing;}
  PIANO_GUARDED_CONFIG C=Config(FALSE);VOID *Token=NULL;
  S->GccBegin=PianoGuardedReadBegin(&C,&Token);
  if(S->GccBegin!=EFI_SUCCESS){S->Status=S->GccBegin;KeepGuard(S,&S->GccGuard);goto Done;}
  if(!Token){S->Status=EFI_COMPROMISED_DATA;S->Retained=mReport.Retained=TRUE;goto Done;}
  EFI_STATUS Read=EFI_SUCCESS;
  for(UINT32 J=0;J<2&&Read==EFI_SUCCESS;++J)for(UINT32 I=0;I<2&&Read==EFI_SUCCESS;++I){
    PIANO_DISPLAY_CLOCK_REGISTER *R=&S->Register[I];R->Skip=PianoClockNotSkipped;R->Attempts++;
    R->ReadStatus[J]=Read=PianoGuardedRead32(Token,R->Address,&R->Value[J]);}
  S->GccEnd=PianoGuardedReadEnd(Token);KeepGuard(S,&S->GccGuard);
  if(S->GccEnd!=EFI_SUCCESS){S->Status=S->GccEnd;S->Retained=mReport.Retained=TRUE;goto Done;}
  if(Read!=EFI_SUCCESS){S->Status=Read;goto Done;}
  S->GccCoherent=S->Register[0].Value[0]==S->Register[0].Value[1]&&S->Register[1].Value[0]==S->Register[1].Value[1];
  S->GccAhbEnabled=S->GccCoherent&&(S->Register[0].Value[0]&1)!=0;
  if(!S->GccAhbEnabled){for(UINT32 I=2;I<10;++I)S->Register[I].Skip=S->GccCoherent?PianoClockGccAhbDisabled:PianoClockGccUnstable;goto Done;}
  // Mapping qualification is CPU/GCD/AT only. A stable GCC enable bit does
  // not prove controller rail/access safety or hold a clock/domain lease.
  C=Config(TRUE);Token=NULL;S->DispccBegin=PianoGuardedReadBegin(&C,&Token);
  if(S->DispccBegin==EFI_SUCCESS&&Token){S->DispccEnd=PianoGuardedReadEnd(Token);S->DispccMappingQualified=S->DispccEnd==EFI_SUCCESS;}
  else if(S->DispccBegin==EFI_SUCCESS){S->Status=EFI_COMPROMISED_DATA;S->Retained=mReport.Retained=TRUE;}
  KeepGuard(S,&S->DispccGuard);
  if(S->DispccBegin==EFI_SUCCESS&&S->DispccEnd!=EFI_SUCCESS){S->Status=S->DispccEnd;S->Retained=mReport.Retained=TRUE;}
  if(!S->DispccMappingQualified)for(UINT32 I=2;I<10;++I)S->Register[I].Skip=PianoClockMappingUnavailable;
  // No held-controller implementation is bound, and no DPU lease exists.
  // Keep the required eight+six declarations visible and never load them.
Done:
  if(S->ServicesLost||Alive()!=TRUE){S->ServicesLost=mReport.ServicesLost=TRUE;S->Retained=mReport.Retained=TRUE;S->Status=EFI_ABORTED;}
  if(!S->Retained)mAlive=NULL;if(!S->ServicesLost)Emit(S);mBusy=FALSE;return S->Status;
}
EFI_STATUS PianoDisplayClockReemit(PIANO_DISPLAY_CLOCK_ALIVE Alive){
  if(!Alive)return EFI_INVALID_PARAMETER;if(mBusy)return EFI_ALREADY_STARTED;
  if(mReport.ServicesLost||Alive()!=TRUE){mReport.Retained=mReport.ServicesLost=TRUE;return EFI_ABORTED;}
  mBusy=TRUE;for(UINT32 I=0;I<mReport.Count;++I){if(Alive()!=TRUE){mReport.Retained=mReport.ServicesLost=TRUE;mBusy=FALSE;return EFI_ABORTED;}Emit(&mReport.Snapshot[I]);}mBusy=FALSE;return EFI_SUCCESS;
}
BOOLEAN PianoDisplayClockRetained(VOID){return mReport.Retained;}
CONST PIANO_DISPLAY_CLOCK_REPORT *PianoDisplayClockGetReport(VOID){return &mReport;}
CONST PIANO_DISPLAY_CLOCK_SPEC *PianoDisplayClockSpecs(VOID){return mSpecs;}
