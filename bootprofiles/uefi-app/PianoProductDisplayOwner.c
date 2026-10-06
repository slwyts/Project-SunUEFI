// SPDX-License-Identifier: BSD-2-Clause-Patent
// Product binding of real native clock refs, bounded CPU reads and GCC guards.
#include "PianoProductDisplayOwner.h"
#include "PianoDisplayClockRead.h"
#include "PianoDisplayClockObserve.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
STATIC struct {
  PIANO_DISPLAY_CLOCK_LEASE Lease;
  PIANO_DISPLAY_CLOCK_READ Reader;
  PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT NonGdsc;
  EFI_STATUS NonGdscStatus;
  PIANO_PRODUCT_DISPLAY_ALIVE Alive;
  EFI_STATUS Status;
  BOOLEAN Attempted,Retained,ServicesLost;
} mDisplay;
STATIC BOOLEAN OutputAliasesState(CONST VOID *Output,UINTN Bytes){
  UINTN A=(UINTN)Output,B=(UINTN)&mDisplay;
  return !Output||!Bytes||Bytes>MAX_UINTN-A||sizeof(mDisplay)>MAX_UINTN-B||
    (A<B+sizeof(mDisplay)&&B<A+Bytes);
}
STATIC BOOLEAN ReadAlive(VOID *Context){
  return Context==&mDisplay&&!mDisplay.ServicesLost&&mDisplay.Alive&&mDisplay.Alive()==TRUE;
}
STATIC BOOLEAN LeaseAlive(VOID *Context){return Context==&mDisplay.Reader&&ReadAlive(&mDisplay);}
BOOLEAN PianoProductDisplayOwnerRetained(VOID){
  return mDisplay.Retained||mDisplay.ServicesLost||mDisplay.Reader.Report.Retained||
    mDisplay.Reader.Report.ServicesLost||mDisplay.Lease.Report.Retained||mDisplay.Lease.Report.ServicesLost;
}
VOID PianoProductDisplayFenceExit(VOID){
  if(!mDisplay.Attempted)return;
  mDisplay.ServicesLost=mDisplay.Retained=TRUE;
  PianoDisplayClockReadFenceExit(&mDisplay.Reader);
  mDisplay.Lease.BorrowToken=0;
  mDisplay.Lease.Report.ServicesLost=mDisplay.Lease.Report.Retained=TRUE;
}
EFI_STATUS PianoProductDisplayClockObserve(CONST CHAR8 *Phase,PIANO_PRODUCT_DISPLAY_ALIVE Alive){
  if(!Phase||!Alive)return EFI_INVALID_PARAMETER;
  if(PianoProductDisplayOwnerRetained())return EFI_NOT_READY;
  if(!mDisplay.Lease.Report.Held)return PianoDisplayClockObserve(Phase,Alive);
  if(Alive!=mDisplay.Alive||!ReadAlive(&mDisplay))return EFI_ABORTED;
  return PianoDisplayClockObserveHeld(Phase,Alive,&mDisplay.Lease);
}
STATIC EFI_STATUS ReadGcc(VOID *Context,PIANO_DISPLAY_CLOCK_LEASE_GCC *R){
  if(Context!=&mDisplay.Reader||!R)return EFI_INVALID_PARAMETER;
  ZeroMem(R,sizeof(*R));R->Status=R->EndStatus=EFI_NOT_STARTED;
  if(!LeaseAlive(Context)){R->ServicesLost=R->Retained=TRUE;return R->Status=EFI_ABORTED;}
  PIANO_GUARDED_CONFIG Config={.Context=Context,.Services=gBS,.DxeServices=gDS,.BootServicesAlive=LeaseAlive,
    .Ranges={{0x127000,0x1000,EfiGcdMemoryTypeMemoryMappedIo,EFI_MEMORY_UC,0}},
    .RangeCount=1,.MaxReads=4,.MaxUsecs=100000};
  VOID *Token=NULL;EFI_STATUS S=PianoGuardedReadBegin(&Config,&Token);
  if(S==EFI_SUCCESS){
    if(!Token){R->Retained=TRUE;S=EFI_COMPROMISED_DATA;}
    else{
      for(UINTN I=0;I<2&&S==EFI_SUCCESS;++I){
        S=PianoGuardedRead32(Token,0x127004,&R->Ahb[I]);
        if(S==EFI_SUCCESS)S=PianoGuardedRead32(Token,0x127008,&R->HfAxi[I]);
      }
      R->EndStatus=PianoGuardedReadEnd(Token);
      if(R->EndStatus!=EFI_SUCCESS){S=R->EndStatus;R->Retained=TRUE;}
    }
  }
  CONST PIANO_GUARDED_REPORT *G=PianoGuardedReadReport();R->Reads=G->Reads;R->Pages=G->PagesValidated;
  R->Retained|=G->Retained||G->Active||G->SyncOwned||G->SErrorOwned||G->Fatal;
  R->ServicesLost=G->ServicesLost||!LeaseAlive(Context);R->Retained|=R->ServicesLost;
  if(R->Retained)mDisplay.Retained=TRUE;
  return R->Status=R->ServicesLost?EFI_ABORTED:S;
}
EFI_STATUS PianoProductDisplayReplay(VOID){
  if(!mDisplay.Attempted)return EFI_NOT_STARTED;
  if(!ReadAlive(&mDisplay))return EFI_ABORTED;
  CONST PIANO_DISPLAY_CLOCK_LEASE_REPORT *R=&mDisplay.Lease.Report;
  DEBUG((DEBUG_WARN,"PIANO_DISPLAY_LEASE status=%r held=%u owned=%u retained=%u lost=%u id=%lx base=%lx\n",
    mDisplay.Status,R->Held,R->OwnedReferences,PianoProductDisplayOwnerRetained(),mDisplay.ServicesLost,(UINT64)R->ClockId,R->NativeBase));
  DEBUG((DEBUG_WARN,"PIANO_DISPLAY_LEASE_STAGES identity=%r before=%r get=%r enable=%r\n",
    R->Identity,R->Before,R->GetId,R->Enable));
  DEBUG((DEBUG_WARN,"PIANO_DISPLAY_LEASE_PROOF counter=%r on=%r after=%r cleanup=%r\n",
    R->CounterStatus,R->IsOn,R->After,R->Cleanup));
  DEBUG((DEBUG_WARN,"PIANO_DISPLAY_LEASE_ENABLED status=%r enabled=%u on=%u hwcg_idle_on_false_allowed=1\n",
    R->IsEnabled,R->EnabledObserved,R->OnObserved));
  DEBUG((DEBUG_WARN,"PIANO_DISPLAY_LEASE_REFS total=%u/%u client=%u/%u snapshots=%u/%u ahb=%08x/%08x\n",
    R->Baseline.Total[0],R->Acquired.Total[0],R->Baseline.PerClient[0],R->Acquired.PerClient[0],
    R->Baseline.MatchingSnapshots,R->Acquired.MatchingSnapshots,R->BeforeGcc.Ahb[0],R->AfterGcc.Ahb[0]));
  DEBUG((DEBUG_WARN,"PIANO_DISPLAY_READER status=%r role=%u address=%lx object=%lx/%lx\n",
    mDisplay.Reader.Report.Status,mDisplay.Reader.Report.Role,mDisplay.Reader.Report.Address,
    mDisplay.Reader.Report.ObjectBase,mDisplay.Reader.Report.ObjectBytes));
  DEBUG((DEBUG_WARN,"PIANO_DISPLAY_READER_ANCHOR anchor=%lx sessions=%u words=%u retained=%u\n",
    mDisplay.Reader.Report.ProducerAnchor,
    mDisplay.Reader.Report.Sessions,mDisplay.Reader.Report.Words,mDisplay.Reader.Report.Retained));
  CONST PIANO_DISPLAY_CLOCK_EFI_MAP_DIAGNOSTIC *Map=&mDisplay.Reader.Report.EfiMap;
  DEBUG((DEBUG_WARN,"PIANO_DISPLAY_EFI_MAP status=%r get=%r reason=%u cursor=%lx bytes=%lu stride=%lu version=%u\n",
    Map->Status,Map->GetMapStatus,Map->Reason,Map->Cursor,(UINT64)Map->MapBytes,(UINT64)Map->DescriptorBytes,Map->DescriptorVersion));
  DEBUG((DEBUG_WARN,"PIANO_DISPLAY_EFI_DESCRIPTOR i=%u type=%u base=%lx pages=%lx attrs=%lx virtual=%lx\n",
    Map->DescriptorIndex,Map->DescriptorType,Map->DescriptorBase,Map->DescriptorPages,Map->DescriptorAttributes,Map->DescriptorVirtual));
  DEBUG((DEBUG_WARN,"PIANO_DISPLAY_EFI_CONFLICT i=%u base=%lx pages=%lx\n",
    Map->ConflictIndex,Map->ConflictBase,Map->ConflictPages));
  DEBUG((DEBUG_WARN,"PIANO_DISPLAY_CLEAN_REFUSAL proof=%r accepted=%u seq=%lu address=%lx bytes=%lu sessions=%u\n",
    R->ReadFailureEvidenceStatus,R->CleanSourceRefusal,R->ReadFailureEvidence.Sequence,R->ReadFailureEvidence.Address,
    (UINT64)R->ReadFailureEvidence.Bytes,R->ReadFailureEvidence.Sessions));
  CONST PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT *N=&mDisplay.NonGdsc;
  DEBUG((DEBUG_WARN,"PIANO_DISPLAY_NON_GDSC status=%r snapshots=%u expected_id=%x node=%lx parent=%lx\n",
    mDisplay.NonGdscStatus,N->MatchingSnapshots,N->ExpectedClockId,N->Node,N->Parent));
  if(mDisplay.NonGdscStatus==EFI_SUCCESS){
    DEBUG((DEBUG_WARN,"PIANO_DISPLAY_NON_GDSC_REFS total=%u/%u client=%u/%u present=%u domain=%u/%u mask=%x\n",
      N->Total[0],N->Total[1],N->PerClient[0],N->PerClient[1],N->ClientRefPresent,N->ParentRefs[0],N->ParentRefs[1],N->ParentRailMask));
    DEBUG((DEBUG_WARN,"PIANO_DISPLAY_NON_GDSC_VOTE config=%lx kind=%u corner=%u cached=%u alternate=%u\n",
      N->ParentCurrentConfig,N->ConfigObservation,N->CurrentCorner,N->ParentCachedCorner,N->ParentVoteAlternate));
    DEBUG((DEBUG_WARN,"PIANO_DISPLAY_RAIL_CLIENTS mm=%lx mx=%lx observation_only=1 power_held=0\n",N->MmClient,N->MxClient));
  }
  return EFI_SUCCESS;
}
EFI_STATUS PianoProductDisplayStart(PIANO_PRODUCT_DISPLAY_ALIVE Alive){
  if(!Alive)return EFI_INVALID_PARAMETER;if(mDisplay.Attempted)return EFI_ALREADY_STARTED;
  mDisplay.Attempted=TRUE;mDisplay.Alive=Alive;mDisplay.Status=EFI_NOT_STARTED;
  mDisplay.NonGdscStatus=EFI_NOT_STARTED;
  PIANO_DISPLAY_CLOCK_LEASE_REPORT *Initial=&mDisplay.Lease.Report;
  Initial->Revision=1;Initial->ClockId=MAX_UINTN;
  Initial->Status=Initial->Identity=Initial->Before=Initial->GetId=Initial->Enable=
    Initial->IsOn=Initial->After=Initial->Disable=Initial->Cleanup=Initial->CounterStatus=
    Initial->ReleaseReadbackStatus=Initial->IsEnabled=Initial->ReadFailureEvidenceStatus=EFI_NOT_STARTED;
  PIANO_DISPLAY_CLOCK_READ_ENV Reader={.Context=&mDisplay,.Services=gBS,.DxeServices=gDS,
    .BootServicesAlive=ReadAlive,.Lease=&mDisplay.Lease};
  EFI_STATUS S=PianoDisplayClockReadInitialize(&mDisplay.Reader,&Reader);
  if(S==EFI_SUCCESS){
    PIANO_DISPLAY_CLOCK_LEASE_ENV Lease={.Context=&mDisplay.Reader,.Services=gBS,.BootServicesAlive=LeaseAlive,
      .ReadCpu=PianoDisplayClockReadCpu,.ReadGcc=ReadGcc,.GetReadFailureEvidence=PianoDisplayClockReadFailureEvidence};
    S=PianoDisplayClockLeaseAcquire(&mDisplay.Lease,&Lease);
  }
  if(S==EFI_SUCCESS&&!PianoProductDisplayOwnerRetained()){
    // One cached CPU-only observation. No second native reference or rail
    // request is acquired; oem log replay never repeats these target loads.
    mDisplay.NonGdscStatus=PianoDisplayClockReadSnapshotClock(&mDisplay.Reader,PianoClockSelectNonGdscAhb,&mDisplay.NonGdsc);
    if(PianoProductDisplayOwnerRetained())S=mDisplay.NonGdscStatus==EFI_SUCCESS?EFI_COMPROMISED_DATA:mDisplay.NonGdscStatus;
  }
  if(!ReadAlive(&mDisplay)){mDisplay.Retained=mDisplay.ServicesLost=TRUE;S=EFI_ABORTED;}
  if(S!=EFI_SUCCESS&&!PianoProductDisplayOwnerRetained()){
    if(mDisplay.Lease.Report.AcquireAttempted||mDisplay.Lease.Report.Held||mDisplay.Lease.Report.OwnedReferences||mDisplay.Lease.Exit||mDisplay.Lease.PinnedCopy)
      mDisplay.Retained=TRUE;
    else if(mDisplay.Reader.Signature){EFI_STATUS Close=PianoDisplayClockReadClose(&mDisplay.Reader);if(Close!=EFI_SUCCESS){mDisplay.Retained=TRUE;S=Close;}}
  }
  mDisplay.Status=S;PianoProductDisplayReplay();return S;
}
STATIC VOID CopyAcquire(PIANO_PRODUCT_DISPLAY_STARTUP_REPORT *R){
  CONST PIANO_DISPLAY_CLOCK_LEASE_REPORT *L=&mDisplay.Lease.Report;
  R->Revision=1;R->LeaseContext=&mDisplay;R->ClockId=L->ClockId;R->NativeBase=L->NativeBase;
  R->AcquireAttempted=L->AcquireAttempted;R->Held=L->Held;R->OwnedReferences=L->OwnedReferences;
  R->Retained=PianoProductDisplayOwnerRetained();R->ServicesLost=mDisplay.ServicesLost||L->ServicesLost||mDisplay.Reader.Report.ServicesLost;
  R->Status=mDisplay.Status;R->AcquireBeforeSnapshots=L->Baseline.MatchingSnapshots;R->AcquireAfterSnapshots=L->Acquired.MatchingSnapshots;
  CopyMem(R->AcquireBeforeTotal,L->Baseline.Total,sizeof(R->AcquireBeforeTotal));CopyMem(R->AcquireAfterTotal,L->Acquired.Total,sizeof(R->AcquireAfterTotal));
  CopyMem(R->AcquireBeforeClient,L->Baseline.PerClient,sizeof(R->AcquireBeforeClient));CopyMem(R->AcquireAfterClient,L->Acquired.PerClient,sizeof(R->AcquireAfterClient));
  R->KnownNoSideEffects=!R->Retained&&!R->ServicesLost&&!L->AcquireAttempted&&!L->Held&&!L->OwnedReferences&&!mDisplay.Lease.Exit&&!mDisplay.Lease.PinnedCopy&&!mDisplay.Reader.PinnedCopy;
}
EFI_STATUS PianoProductDisplayStartup(PIANO_PRODUCT_DISPLAY_STARTUP_REPORT *R){
  if(OutputAliasesState(R,sizeof(*R)))return EFI_INVALID_PARAMETER;ZeroMem(R,sizeof(*R));
  if(!mDisplay.Attempted)return EFI_NOT_STARTED;CopyAcquire(R);return mDisplay.Status;
}
EFI_STATUS PianoProductDisplayStop(VOID *Context,PIANO_PRODUCT_DISPLAY_RETIRE_REPORT *R){
  if(Context!=&mDisplay||OutputAliasesState(R,sizeof(*R)))return EFI_INVALID_PARAMETER;ZeroMem(R,sizeof(*R));
  CONST PIANO_DISPLAY_CLOCK_LEASE_REPORT *L=&mDisplay.Lease.Report;
  PIANO_PRODUCT_DISPLAY_STARTUP_REPORT Start={0};CopyAcquire(&Start);
  R->Revision=1;R->LeaseContext=Context;R->ClockId=L->ClockId;R->NativeBase=L->NativeBase;
  R->Started=L->Held;R->OwnedReferencesBefore=L->OwnedReferences;
  R->AcquireBeforeSnapshots=Start.AcquireBeforeSnapshots;R->AcquireAfterSnapshots=Start.AcquireAfterSnapshots;
  CopyMem(R->AcquireBeforeTotal,Start.AcquireBeforeTotal,sizeof(R->AcquireBeforeTotal));CopyMem(R->AcquireAfterTotal,Start.AcquireAfterTotal,sizeof(R->AcquireAfterTotal));
  CopyMem(R->AcquireBeforeClient,Start.AcquireBeforeClient,sizeof(R->AcquireBeforeClient));CopyMem(R->AcquireAfterClient,Start.AcquireAfterClient,sizeof(R->AcquireAfterClient));
  EFI_STATUS S=PianoDisplayClockLeaseRelease(&mDisplay.Lease);R->Returned=TRUE;R->Release=S;
  R->Cleanup=EFI_NOT_STARTED;
  if(S==EFI_SUCCESS&&!PianoProductDisplayOwnerRetained())R->Cleanup=PianoDisplayClockReadClose(&mDisplay.Reader);
  if(S==EFI_SUCCESS&&R->Cleanup!=EFI_SUCCESS)S=R->Cleanup;
  R->Retained=PianoProductDisplayOwnerRetained()||(R->Cleanup!=EFI_SUCCESS);R->ServicesLost=mDisplay.ServicesLost||L->ServicesLost||mDisplay.Reader.Report.ServicesLost;
  if(R->ServicesLost)S=EFI_ABORTED;
  R->ReleaseAttempted=L->ReleaseAttempted;R->HeldAfter=L->Held;R->Released=L->Released;R->ExitClosed=mDisplay.Lease.Exit==NULL;
  R->OwnedReferencesAfter=L->OwnedReferences;R->CounterStatus=L->CounterStatus;R->GccReadback=L->ReleaseReadbackStatus;R->GccReadbackEnd=L->ReleaseGcc.EndStatus;
  R->GccReads=L->ReleaseGcc.Reads;R->GccPages=L->ReleaseGcc.Pages;
  R->ReleaseBeforeSnapshots=L->ReleaseBefore.MatchingSnapshots;R->ReleaseAfterSnapshots=L->Retired.MatchingSnapshots;
  CopyMem(R->ReleaseBeforeTotal,L->ReleaseBefore.Total,sizeof(R->ReleaseBeforeTotal));CopyMem(R->ReleaseAfterTotal,L->Retired.Total,sizeof(R->ReleaseAfterTotal));
  CopyMem(R->ReleaseBeforeClient,L->ReleaseBefore.PerClient,sizeof(R->ReleaseBeforeClient));CopyMem(R->ReleaseAfterClient,L->Retired.PerClient,sizeof(R->ReleaseAfterClient));
  R->Clean=S==EFI_SUCCESS&&!R->Retained&&!R->ServicesLost&&!R->HeldAfter&&!R->OwnedReferencesAfter&&R->Released&&R->ExitClosed;
  if(R->Retained)mDisplay.Retained=TRUE;
  R->Status=mDisplay.Status=S;
  if(!R->ServicesLost)DEBUG((DEBUG_WARN,"PIANO_DISPLAY_RELEASE status=%r clean=%u owned=%u/%u refs=%u/%u retained=%u\n",
    S,R->Clean,R->OwnedReferencesBefore,R->OwnedReferencesAfter,R->ReleaseBeforeTotal[0],R->ReleaseAfterTotal[0],R->Retained));
  return S;
}
