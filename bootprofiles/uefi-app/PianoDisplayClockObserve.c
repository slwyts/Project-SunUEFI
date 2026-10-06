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
STATIC BOOLEAN HeldScope(CONST PIANO_DISPLAY_CLOCK_LEASE_HELD_PROOF *P,CONST PIANO_DISPLAY_CLOCK_LEASE *Lease){
  CONST PIANO_DISPLAY_CLOCK_LEASE_REFS *R=&P->Refs;CONST PIANO_DISPLAY_CLOCK_LEASE_GCC *G=&P->Gcc;
  return P->Revision==PIANO_DISPLAY_CLOCK_LEASE_HELD_PROOF_REVISION&&P->Token&&P->LeaseContext==Lease&&
    P->NativeBase&&P->NativeBase<=MAX_UINT64-0x44000&&P->ClockId==0x04010033&&
    P->Status==EFI_SUCCESS&&P->Identity==EFI_SUCCESS&&P->CounterStatus==EFI_SUCCESS&&
    P->IsEnabled==EFI_SUCCESS&&P->EnabledObserved==TRUE&&P->OwnedReferences==1&&P->GccStatus==EFI_SUCCESS&&
    R->MatchingSnapshots==2&&R->Global==P->NativeBase+0x283a0&&R->Module==P->NativeBase+0x28678&&
    R->Node==P->NativeBase+0x33a68&&R->Client&&R->ClientRef&&R->Provider==4&&R->Index==51&&R->ModuleCount==9&&R->ClockCount==149&&
    !(R->GlobalFlags&(BIT8|BIT11))&&!(R->NodeFlags&(BIT8|BIT9|BIT14))&&R->Total[0]&&R->PerClient[0]&&
    R->PerClient[0]<=R->Total[0]&&R->PerClient[1]<=R->Total[1]&&
    G->Status==EFI_SUCCESS&&G->EndStatus==EFI_SUCCESS&&G->Retained==FALSE&&G->ServicesLost==FALSE&&
    G->Reads==4&&G->Pages==1&&G->Ahb[0]==G->Ahb[1]&&(G->Ahb[0]&BIT0)&&G->HfAxi[0]==G->HfAxi[1];
}
STATIC BOOLEAN SameHeldScope(CONST PIANO_DISPLAY_CLOCK_LEASE_HELD_PROOF *A,CONST PIANO_DISPLAY_CLOCK_LEASE_HELD_PROOF *B){
  CONST PIANO_DISPLAY_CLOCK_LEASE_REFS *X=&A->Refs,*Y=&B->Refs;
  return A->Token==B->Token&&A->LeaseContext==B->LeaseContext&&A->ClockId==B->ClockId&&A->NativeBase==B->NativeBase&&
    X->Global==Y->Global&&X->Client==Y->Client&&X->Module==Y->Module&&X->Node==Y->Node&&X->ClientRef==Y->ClientRef&&
    X->Provider==Y->Provider&&X->Index==Y->Index&&X->ModuleCount==Y->ModuleCount&&X->ClockCount==Y->ClockCount&&
    X->GlobalFlags==Y->GlobalFlags&&X->NodeFlags==Y->NodeFlags&&X->ClientFlags==Y->ClientFlags;
}
STATIC VOID Emit(CONST PIANO_DISPLAY_CLOCK_SNAPSHOT *S){
  DEBUG((DEBUG_WARN,"PIANO_CLOCK_OBSERVE phase=%a status=%r gcc_pair=%u ahb_enable=%u dispcc_map=%u retained=%u lost=%u required_unfinished=1\n",
    S->Phase,S->Status,S->GccCoherent,S->GccAhbEnabled,S->DispccMappingQualified,S->Retained,S->ServicesLost));
  DEBUG((DEBUG_WARN,"PIANO_CLOCK_SESSION gcc=%r/%r dispcc=%r/%r controller_held=%u dpu_held=%u hardware_ready=0\n",
    S->GccBegin,S->GccEnd,S->DispccBegin,S->DispccEnd,S->ControllerBusHeld,S->DpuDomainClockHeld));
  DEBUG((DEBUG_WARN,"PIANO_CLOCK_HELD borrow=%r scope=%r return=%r ref_held=%u token=%lu power_lease_missing=1\n",
    S->HeldBorrow,S->HeldScope,S->HeldReturn,S->ClockReferenceHeld,S->HeldBefore.Token));
  if(S->HeldBefore.Revision){DEBUG((DEBUG_WARN,"PIANO_CLOCK_HELD_REFS before=%u/%u after=%u/%u snapshots=%u/%u id=%lx base=%lx\n",
    S->HeldBefore.Refs.Total[0],S->HeldBefore.Refs.PerClient[0],S->HeldAfter.Refs.Total[0],S->HeldAfter.Refs.PerClient[0],
    S->HeldBefore.Refs.MatchingSnapshots,S->HeldAfter.Refs.MatchingSnapshots,(UINT64)S->HeldBefore.ClockId,S->HeldBefore.NativeBase));}
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
STATIC EFI_STATUS Observe(CONST CHAR8 *Phase,PIANO_DISPLAY_CLOCK_ALIVE Alive,PIANO_DISPLAY_CLOCK_LEASE *Lease){
  if(!Phase||!Alive)return EFI_INVALID_PARAMETER;UINTN N=0;while(N<32&&Phase[N])++N;if(!N||N==32)return EFI_INVALID_PARAMETER;
  if(mBusy)return EFI_ALREADY_STARTED;if(mReport.Retained||mReport.ServicesLost)return EFI_NOT_READY;
  if(mReport.Count==PIANO_DISPLAY_CLOCK_PHASES)return EFI_OUT_OF_RESOURCES;
  mBusy=TRUE;mAlive=Alive;PIANO_DISPLAY_CLOCK_SNAPSHOT *S=&mReport.Snapshot[mReport.Count++];ZeroMem(S,sizeof(*S));CopyMem(S->Phase,Phase,N);
  S->Status=EFI_NOT_READY;S->GccBegin=S->GccEnd=S->DispccBegin=S->DispccEnd=EFI_NOT_STARTED;
  S->HeldBorrow=S->HeldScope=S->HeldReturn=EFI_NOT_STARTED;
  S->GccGuard.Status=S->GccGuard.CleanupStatus=S->GccGuard.MappingStatus=EFI_NOT_STARTED;
  S->DispccGuard.Status=S->DispccGuard.CleanupStatus=S->DispccGuard.MappingStatus=EFI_NOT_STARTED;
  for(UINT32 I=0;I<PIANO_DISPLAY_CLOCK_REGISTERS;++I){S->Register[I].Address=mSpecs[I].Address;
    S->Register[I].ReadStatus[0]=S->Register[I].ReadStatus[1]=EFI_NOT_STARTED;
    S->Register[I].Skip=I<2?PianoClockNoGccObservation:I<10?PianoClockControllerBusUnproven:PianoClockDpuLeaseMissing;}
  if(Lease){
    if(!GuardAlive(&mReport)){S->Status=EFI_ABORTED;goto Done;}
    S->HeldBorrow=PianoDisplayClockLeaseBorrowHeld(Lease,&S->HeldBefore);
    if(!GuardAlive(&mReport)){S->Status=EFI_ABORTED;goto Done;}
    if(S->HeldBorrow!=EFI_SUCCESS){S->Status=EFI_ERROR(S->HeldBorrow)?S->HeldBorrow:EFI_DEVICE_ERROR;
      S->Retained=mReport.Retained=Lease->Report.Retained||Lease->Report.ServicesLost||!EFI_ERROR(S->HeldBorrow);
      for(UINT32 I=2;I<10;++I)S->Register[I].Skip=PianoClockHeldProofUnavailable;goto Done;}
    S->HeldScope=HeldScope(&S->HeldBefore,Lease)?EFI_SUCCESS:EFI_COMPROMISED_DATA;
    if(S->HeldScope!=EFI_SUCCESS){S->Status=S->HeldScope;goto Done;}
    S->ClockReferenceHeld=TRUE;
    for(UINT32 I=2;I<10;++I)S->Register[I].Skip=PianoClockControllerPowerUnproven;
  }
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
  // Even a freshly borrowed gcc_disp_ahb ref does not hold ROM vdd_mm/vdd_mx
  // (mainline MMCX). Keep all eight control LDRs gated until that real owner
  // exists; mapping qualification is not a substitute for its access lifetime.
Done:
  if(S->ServicesLost||Alive()!=TRUE){S->ServicesLost=mReport.ServicesLost=TRUE;S->Retained=mReport.Retained=TRUE;S->Status=EFI_ABORTED;}
  if(Lease&&S->HeldBorrow==EFI_SUCCESS){
    if(S->Retained||S->ServicesLost||!S->HeldBefore.Token){S->Retained=mReport.Retained=TRUE;}
    else{
      // No native Clock query runs while this observer owns a Guard handler.
      S->HeldReturn=PianoDisplayClockLeaseReturnHeld(Lease,S->HeldBefore.Token,&S->HeldAfter);
      if(Alive()!=TRUE){S->ServicesLost=mReport.ServicesLost=TRUE;S->Status=EFI_ABORTED;S->Retained=mReport.Retained=TRUE;}
      else if(S->HeldReturn!=EFI_SUCCESS){S->Status=EFI_ERROR(S->HeldReturn)?S->HeldReturn:EFI_DEVICE_ERROR;S->Retained=mReport.Retained=TRUE;}
      else if(!HeldScope(&S->HeldAfter,Lease)||!SameHeldScope(&S->HeldBefore,&S->HeldAfter)){
        S->HeldScope=S->Status=EFI_COMPROMISED_DATA;S->Retained=mReport.Retained=TRUE;}
    }
  }
  if(!S->Retained)mAlive=NULL;if(!S->ServicesLost)Emit(S);mBusy=FALSE;return S->Status;
}
EFI_STATUS PianoDisplayClockObserve(CONST CHAR8 *Phase,PIANO_DISPLAY_CLOCK_ALIVE Alive){return Observe(Phase,Alive,NULL);}
EFI_STATUS PianoDisplayClockObserveHeld(CONST CHAR8 *Phase,PIANO_DISPLAY_CLOCK_ALIVE Alive,PIANO_DISPLAY_CLOCK_LEASE *Lease){
  return Lease?Observe(Phase,Alive,Lease):EFI_INVALID_PARAMETER;
}
EFI_STATUS PianoDisplayClockReemit(PIANO_DISPLAY_CLOCK_ALIVE Alive){
  if(!Alive)return EFI_INVALID_PARAMETER;if(mBusy)return EFI_ALREADY_STARTED;
  if(mReport.ServicesLost||Alive()!=TRUE){mReport.Retained=mReport.ServicesLost=TRUE;return EFI_ABORTED;}
  mBusy=TRUE;for(UINT32 I=0;I<mReport.Count;++I){if(Alive()!=TRUE){mReport.Retained=mReport.ServicesLost=TRUE;mBusy=FALSE;return EFI_ABORTED;}Emit(&mReport.Snapshot[I]);}mBusy=FALSE;return EFI_SUCCESS;
}
BOOLEAN PianoDisplayClockRetained(VOID){return mReport.Retained;}
CONST PIANO_DISPLAY_CLOCK_REPORT *PianoDisplayClockGetReport(VOID){return &mReport;}
CONST PIANO_DISPLAY_CLOCK_SPEC *PianoDisplayClockSpecs(VOID){return mSpecs;}
