// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoSmmuNativeIndependent.h"
#include <Library/BaseMemoryLib.h>
#define BASE 0x15000000U
#define CB(N) (BASE+0x80000U+((UINTN)(N)<<12))
STATIC CONST UINT32 BankOffsets[]={0,4,0x10,0x20,0x24,0x28,0x2c,0x30,0x38,0x3c,0x58,0x60,0x64,0x68};
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC EFI_STATUS Failure(PIANO_SMMU_INDEPENDENT *C,EFI_STATUS S){
  C->Status=Exact(S);if(C->WritesAttempted){C->Retained=TRUE;C->Tables->Quarantined=TRUE;}return C->Status;
}
STATIC EFI_STATUS ReadOp(CONST PIANO_SMMU_INDEPENDENT_OPS *Ops,UINTN Address,UINT32 *V){return Exact(Ops->Read32(Ops->Context,Address,V));}
STATIC EFI_STATUS Rd(PIANO_SMMU_INDEPENDENT *C,UINTN Address,UINT32 *V){return ReadOp(&C->Ops,Address,V);}
STATIC EFI_STATUS Wr(PIANO_SMMU_INDEPENDENT *C,UINTN Address,UINT32 V){
  BOOLEAN Allowed=Address==BASE+0x800+4*C->Slot || Address==BASE+0xc00+4*C->Slot || Address==BASE+0x1000+4*C->Bank || Address==BASE+0x1800+4*C->Bank;
  if(Address>=CB(C->Bank) && Address<CB(C->Bank)+0x1000){UINTN O=Address-CB(C->Bank);Allowed=O==0 || O==0x10 || O==0x20 || O==0x24 || O==0x28 || O==0x2c || O==0x30 || O==0x38 || O==0x3c || O==0x618 || O==0x7f0;}
  if(!Allowed)return Failure(C,EFI_ACCESS_DENIED);++C->WritesAttempted;
  EFI_STATUS S=Exact(C->Ops.Write32(C->Ops.Context,Address,V));if(S==EFI_SUCCESS)S=Exact(C->Ops.Fence(C->Ops.Context));return S==EFI_SUCCESS?S:Failure(C,S);
}
STATIC EFI_STATUS IdleOp(CONST PIANO_SMMU_INDEPENDENT_OPS *Ops,PIANO_SMMU_INDEPENDENT_MASTER Master){
  for(UINTN Sample=0;Sample<2;++Sample){UINT32 V=0;
    if(Master==PianoIndependentUfs){CONST UINTN Off[]={0x58,0x78,0x60,0x80,0x24};for(UINTN I=0;I<5;++I){EFI_STATUS S=ReadOp(Ops,0x1d84000+Off[I],&V);if(S!=EFI_SUCCESS)return S;if(V)return EFI_NOT_READY;}}
    else{EFI_STATUS S=ReadOp(Ops,0xa60c704,&V);if(S!=EFI_SUCCESS)return S;if(V&BIT31)return EFI_NOT_READY;S=ReadOp(Ops,0xa60c70c,&V);if(S!=EFI_SUCCESS)return S;if(!(V&BIT22))return EFI_NOT_READY;}
    EFI_STATUS S=Exact(Ops->Fence(Ops->Context));if(S!=EFI_SUCCESS)return S;
  }return EFI_SUCCESS;
}
STATIC EFI_STATUS Idle(PIANO_SMMU_INDEPENDENT *C){return IdleOp(&C->Ops,C->Master);}
STATIC EFI_STATUS RoutesOp(CONST PIANO_SMMU_INDEPENDENT_OPS *Ops,PIANO_SMMU_INDEPENDENT_SNAPSHOT *S){
  UINT32 *Globals[]={&S->Global,&S->Id0,&S->Id1,&S->Id2,&S->GlobalFault};CONST UINTN Off[]={0,0x20,0x24,0x28,0x48};
  for(UINTN I=0;I<5;++I){EFI_STATUS R=ReadOp(Ops,BASE+Off[I],Globals[I]);if(R!=EFI_SUCCESS)return R;}
  if(S->Id0!=0x4c017e7f || S->Id1!=0x60000053 || S->Id2!=0x5111 || (S->Global&(BIT0|BIT3)) || S->GlobalFault)return EFI_UNSUPPORTED;
  for(UINTN I=0;I<127;++I){EFI_STATUS R=ReadOp(Ops,BASE+0x800+I*4,&S->Smr[I]);if(R!=EFI_SUCCESS)return R;R=ReadOp(Ops,BASE+0xc00+I*4,&S->S2cr[I]);if(R!=EFI_SUCCESS)return R;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS BankOp(CONST PIANO_SMMU_INDEPENDENT_OPS *Ops,UINTN N,PIANO_SMMU_INDEPENDENT_BANK *B){
  EFI_STATUS R=ReadOp(Ops,BASE+0x1000+4*N,&B->Cbar);if(R!=EFI_SUCCESS)return R;R=ReadOp(Ops,BASE+0x1800+4*N,&B->Cba2r);if(R!=EFI_SUCCESS)return R;
  // Each pointer names one actual member. Pointer arithmetic across adjacent
  // struct members was undefined, even if the layout happened to be packed.
  UINT32 *Fields[]={&B->Sctlr,&B->Actlr,&B->Tcr2,&B->Ttbr0Low,&B->Ttbr0High,&B->Ttbr1Low,&B->Ttbr1High,&B->Tcr,&B->Mair0,&B->Mair1,&B->Fsr,&B->FarLow,&B->FarHigh,&B->Fsynr};
  for(UINTN I=0;I<ARRAY_SIZE(BankOffsets);++I){R=ReadOp(Ops,CB(N)+BankOffsets[I],Fields[I]);if(R!=EFI_SUCCESS)return R;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Snapshot(PIANO_SMMU_INDEPENDENT *C,PIANO_SMMU_INDEPENDENT_SNAPSHOT *S){
  ZeroMem(S,sizeof(*S));EFI_STATUS R=RoutesOp(&C->Ops,S);if(R!=EFI_SUCCESS)return R;
  for(UINTN N=0;N<83;++N){R=BankOp(&C->Ops,N,&S->Banks[N]);if(R!=EFI_SUCCESS)return R;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Stable(PIANO_SMMU_INDEPENDENT *C){PIANO_SMMU_INDEPENDENT_SNAPSHOT Now;EFI_STATUS S=Snapshot(C,&Now);if(S!=EFI_SUCCESS)return Failure(C,S);return CompareMem(&Now,&C->Expected,sizeof(Now))?Failure(C,EFI_COMPROMISED_DATA):EFI_SUCCESS;}
STATIC EFI_STATUS Set(PIANO_SMMU_INDEPENDENT *C,UINTN Address,UINT32 Value,UINT32 Ignore){
  EFI_STATUS S=Wr(C,Address,Value);if(S!=EFI_SUCCESS)return S;UINT32 Actual=0;S=Rd(C,Address,&Actual);if(S!=EFI_SUCCESS || ((Actual^Value)&~Ignore))return Failure(C,S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);return EFI_SUCCESS;
}
STATIC EFI_STATUS Flush(PIANO_SMMU_INDEPENDENT *C){
  EFI_STATUS S=Wr(C,CB(C->Bank)+0x618,0);if(S==EFI_SUCCESS)S=Wr(C,CB(C->Bank)+0x7f0,0);if(S!=EFI_SUCCESS)return S;
  UINT64 Start=0,Now=0;S=Exact(C->Ops.NowUs(C->Ops.Context,&Start));if(S!=EFI_SUCCESS)return Failure(C,S);
  for(UINTN I=0;I<4000;++I){UINT32 V=0;S=Rd(C,CB(C->Bank)+0x7f4,&V);if(S!=EFI_SUCCESS)return Failure(C,S);if(V&~1U)return Failure(C,EFI_COMPROMISED_DATA);
    if(!V){++C->Syncs;return Exact(C->Ops.Fence(C->Ops.Context));}
    S=Exact(C->Ops.NowUs(C->Ops.Context,&Now));if(S!=EFI_SUCCESS || Now<Start)return Failure(C,S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);if(Now-Start>=10000)break;
    S=Exact(C->Ops.Pause(C->Ops.Context));if(S!=EFI_SUCCESS)return Failure(C,S);
  }return Failure(C,EFI_TIMEOUT);
}
STATIC EFI_STATUS Configure(PIANO_SMMU_INDEPENDENT *C,CONST PIANO_SMMU_INDEPENDENT_BANK *B){
  EFI_STATUS S=Set(C,CB(C->Bank),0,0);if(S==EFI_SUCCESS)S=Set(C,BASE+0x1800+4*C->Bank,B->Cba2r,0);
  if(S==EFI_SUCCESS)S=Set(C,BASE+0x1000+4*C->Bank,B->Cbar,0);
  CONST UINT32 Off[]={0x10,0x30,0x20,0x24,0x28,0x2c,0x38,0x3c};CONST UINT32 Value[]={B->Tcr2,B->Tcr,B->Ttbr0Low,B->Ttbr0High,B->Ttbr1Low,B->Ttbr1High,B->Mair0,B->Mair1};
  for(UINTN I=0;S==EFI_SUCCESS && I<8;++I)S=Set(C,CB(C->Bank)+Off[I],Value[I],Off[I]==0x10?0x60:0);
  if(S==EFI_SUCCESS)S=Set(C,CB(C->Bank),B->Sctlr,0);return S;
}
STATIC BOOLEAN ValidTables(CONST PIANO_IO_PAGE_TABLE *PT,CONST PIANO_DMA_BUFFER *B){
  if(!B || !PT || B->Signature!=SIGNATURE_32('S','D','M','A') || !B->Device || !B->Cpu || B->Cpu!=PT->Tables ||
     B->Physical!=PT->Physical || ((UINTN)B->Cpu&4095) || (UINTN)B->Cpu>MAX_UINTN-PIANO_IO_PT_BYTES ||
     (B->Physical&4095) || B->Physical>0xfffffffffULL || PIANO_IO_PT_BYTES-1>0xfffffffffULL-B->Physical ||
     B->Bytes!=PIANO_IO_PT_BYTES || B->ReservedBytes!=PIANO_IO_PT_BYTES || B->Alignment!=4096 ||
     B->MemoryType!=EfiReservedMemoryType || !(B->MemoryAttributes&EFI_MEMORY_WB) || B->Direction!=PianoDmaToDevice ||
     B->Active || B->Quarantined || B->ExitRetained || B->Mapped || B->Mapping || !B->AllocationPages ||
     B->AllocationPages>MAX_UINT64/4096 || (B->Allocation&4095) || B->Allocation>B->Physical)return FALSE;
  UINT64 AllocBytes=(UINT64)B->AllocationPages*4096,Offset=B->Physical-B->Allocation;
  if(Offset>=AllocBytes || B->ReservedBytes>AllocBytes-Offset || B->Allocation>0xfffffffffULL || AllocBytes-1>0xfffffffffULL-B->Allocation)return FALSE;
  for(UINTN I=0;I<512;++I){UINT64 Expected=I==((PIANO_IOVA_BASE>>30)&511)?(B->Physical+4096)|3:0;if(PT->Tables[I]!=Expected)return FALSE;}
  for(UINTN I=0;I<512;++I){UINT64 Expected=I<8?(B->Physical+(2+I)*4096)|3:0;if(PT->Tables[512+I]!=Expected)return FALSE;}
  return TRUE;
}
EFI_STATUS PianoSmmuIndependentOpen(PIANO_SMMU_INDEPENDENT *C,PIANO_SMMU_INDEPENDENT_MASTER Master,CONST PIANO_SMMU_INDEPENDENT_OPS *Ops,PIANO_IO_PAGE_TABLE *PT,PIANO_DMA_BUFFER *Tables){
  if(!C || !Ops || !PT || !Tables || (Master!=PianoIndependentUfs && Master!=PianoIndependentUsb) || !Ops->Read32 || !Ops->Write32 || !Ops->Fence || !Ops->Clean || !Ops->NowUs || !Ops->Pause)return EFI_INVALID_PARAMETER;
  if(C->Signature)return EFI_ALREADY_STARTED;
  if(!ValidTables(PT,Tables))return EFI_NOT_READY;
  ZeroMem(C,sizeof(*C));C->Signature=PIANO_SMMU_INDEPENDENT_SIGNATURE;C->Ops=*Ops;C->Master=Master;C->Sid=Master==PianoIndependentUfs?0x60:0x40;C->PageTable=PT;C->Tables=Tables;
  EFI_STATUS S=Idle(C);if(S!=EFI_SUCCESS)return Failure(C,S);S=Snapshot(C,&C->Before);if(S!=EFI_SUCCESS)return Failure(C,S);C->Expected=C->Before;
  BOOLEAN Used[83]={0};UINTN Slot=127,Bank=83;
  for(UINTN I=0;I<127;++I){UINT32 Smr=C->Before.Smr[I],S2=C->Before.S2cr[I];if(Smr&BIT31){if(((C->Sid^(UINT16)Smr)&~((Smr>>16)&0x7fff))==0)return Failure(C,EFI_ALREADY_STARTED);if(((S2>>16)&3)==0){if((S2&255)>=83)return Failure(C,EFI_COMPROMISED_DATA);Used[S2&255]=TRUE;}}else if(Slot==127)Slot=I;}
  for(UINTN I=0;I<83;++I)if(!Used[I] && !(C->Before.Banks[I].Sctlr&1) && !(C->Before.Banks[I].Fsr&PIANO_SMMU_FSR_FAULT_MASK)){Bank=I;break;}
  if(Slot==127 || Bank==83)return Failure(C,EFI_OUT_OF_RESOURCES);C->Slot=(UINT16)Slot;C->Bank=(UINT8)Bank;
  S=Stable(C);if(S==EFI_SUCCESS)S=Idle(C);if(S!=EFI_SUCCESS)return Failure(C,S);
  S=Exact(Ops->Clean(Ops->Context,Tables->Cpu,Tables->ReservedBytes));if(S!=EFI_SUCCESS)return Failure(C,S);
  PIANO_SMMU_INDEPENDENT_BANK New=C->Before.Banks[Bank];New.Cbar=0x1f000;New.Cba2r=1;New.Tcr=0x802519;New.Tcr2=0x38001;
  New.Ttbr0Low=(UINT32)PT->Physical;New.Ttbr0High=(UINT32)(PT->Physical>>32);New.Ttbr1Low=New.Ttbr1High=0;New.Mair0=255;New.Mair1=0;New.Sctlr=0x1e5;
  S=Configure(C,&New);if(S!=EFI_SUCCESS)return Failure(C,S);
  // Normalize only own TCR2 architectural RES1 bits by observed readback.
  S=Rd(C,CB(Bank)+0x10,&New.Tcr2);if(S!=EFI_SUCCESS)return Failure(C,S);
  UINT32 Fsr=0;S=Rd(C,CB(Bank)+0x58,&Fsr);if(S!=EFI_SUCCESS)return Failure(C,S);
  // AArch64 format is architecturally 2 (bits10:9). Accept only that exact
  // owned readback, not arbitrary normalization of faults or reserved bits.
  if(Fsr!=BIT10)return Failure(C,EFI_COMPROMISED_DATA);New.Fsr=Fsr;C->Expected.Banks[Bank]=New;
  S=Flush(C);if(S==EFI_SUCCESS)S=Set(C,BASE+0xc00+4*Slot,(UINT32)Bank,0);if(S==EFI_SUCCESS)S=Set(C,BASE+0x800+4*Slot,BIT31|C->Sid,0);
  if(S!=EFI_SUCCESS)return Failure(C,S);C->Expected.Smr[Slot]=BIT31|C->Sid;C->Expected.S2cr[Slot]=(UINT32)Bank;
  S=Stable(C);if(S!=EFI_SUCCESS)return S;C->Attached=C->RegisterConfigurationVerified=TRUE;return C->Status=EFI_SUCCESS;
}
EFI_STATUS PianoSmmuIndependentSync(PIANO_SMMU_INDEPENDENT *C){
  if(!C || C->Signature!=PIANO_SMMU_INDEPENDENT_SIGNATURE || !C->Attached || C->Retained || C->Closed)return EFI_ACCESS_DENIED;
  EFI_STATUS S=Idle(C);if(S==EFI_SUCCESS)S=Stable(C);if(S==EFI_SUCCESS)S=Exact(C->Ops.Clean(C->Ops.Context,C->Tables->Cpu,C->Tables->ReservedBytes));if(S==EFI_SUCCESS)S=Flush(C);if(S==EFI_SUCCESS)S=Stable(C);return S==EFI_SUCCESS?(C->Status=S):Failure(C,S);
}
EFI_STATUS PianoSmmuIndependentClose(PIANO_SMMU_INDEPENDENT *C){
  if(!C || C->Signature!=PIANO_SMMU_INDEPENDENT_SIGNATURE || !C->Attached || C->Retained || C->Closed)return EFI_ACCESS_DENIED;
  EFI_STATUS S=Idle(C);if(S==EFI_SUCCESS)S=Stable(C);if(S!=EFI_SUCCESS)return Failure(C,S);
  S=Set(C,BASE+0x800+4*C->Slot,0,0);if(S==EFI_SUCCESS)S=Set(C,CB(C->Bank),0,0);if(S==EFI_SUCCESS)S=Flush(C);
  if(S==EFI_SUCCESS)S=Configure(C,&C->Before.Banks[C->Bank]);
  if(S==EFI_SUCCESS)S=Set(C,BASE+0xc00+4*C->Slot,C->Before.S2cr[C->Slot],0);
  if(S==EFI_SUCCESS)S=Set(C,BASE+0x800+4*C->Slot,C->Before.Smr[C->Slot],0);
  if(S!=EFI_SUCCESS)return Failure(C,S);C->Expected=C->Before;S=Stable(C);if(S!=EFI_SUCCESS)return S;
  C->Attached=FALSE;C->Closed=C->TablesUnreachable=TRUE;return C->Status=EFI_SUCCESS;
}

typedef struct {
  CONST PIANO_SMMU_INDEPENDENT_OPS *Source;
  PIANO_SMMU_PROBE_REPORT *Report;
  UINT32 MaxReads,MaxUs;
  UINT64 Start;
  BOOLEAN BudgetFailed,IoFailed;
} PROBE_TRACKER;
STATIC EFI_STATUS ProbeBudget(PROBE_TRACKER *T,UINTN Address,BOOLEAN CheckCount){
  UINT64 Now=0;EFI_STATUS S=Exact(T->Source->NowUs(T->Source->Context,&Now));
  if(S!=EFI_SUCCESS){T->IoFailed=TRUE;T->Report->FailureAddress=Address;return S;}
  if(Now<T->Start){T->IoFailed=TRUE;T->Report->FailureAddress=Address;return EFI_COMPROMISED_DATA;}
  T->Report->ElapsedUs=Now-T->Start;
  if((CheckCount && T->Report->ReadAttempts>=T->MaxReads) || Now-T->Start>=T->MaxUs){T->BudgetFailed=TRUE;T->Report->FailureAddress=Address;return EFI_TIMEOUT;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ProbeRead(VOID *Context,UINTN Address,UINT32 *Value){
  PROBE_TRACKER *T=Context;EFI_STATUS S=ProbeBudget(T,Address,TRUE);if(S!=EFI_SUCCESS)return S;
  ++T->Report->ReadAttempts;S=Exact(T->Source->Read32(T->Source->Context,Address,Value));
  if(S!=EFI_SUCCESS){T->IoFailed=TRUE;T->Report->FailureAddress=Address;return S;}return ProbeBudget(T,Address,FALSE);
}
STATIC EFI_STATUS ProbeFence(VOID *Context){
  PROBE_TRACKER *T=Context;EFI_STATUS S=Exact(T->Source->Fence(T->Source->Context));if(S!=EFI_SUCCESS)T->IoFailed=TRUE;return S;
}
STATIC EFI_STATUS ProbeError(PROBE_TRACKER *T,EFI_STATUS Status){
  T->Report->Reason=T->BudgetFailed?PianoProbeBudgetExceeded:PianoProbeReadFailed;
  return T->Report->ReadStatus=Exact(Status);
}
STATIC BOOLEAN SameRoutes(CONST PIANO_SMMU_INDEPENDENT_SNAPSHOT *A,CONST PIANO_SMMU_INDEPENDENT_SNAPSHOT *B){
  return A->Global==B->Global && A->Id0==B->Id0 && A->Id1==B->Id1 && A->Id2==B->Id2 && A->GlobalFault==B->GlobalFault &&
    !CompareMem(A->Smr,B->Smr,sizeof(A->Smr)) && !CompareMem(A->S2cr,B->S2cr,sizeof(A->S2cr));
}
EFI_STATUS PianoSmmuIndependentReadOnlyProbe(CONST PIANO_SMMU_INDEPENDENT_OPS *Source,PIANO_SMMU_INDEPENDENT_MASTER Master,
  CONST PIANO_SMMU_PROBE_CONFIG *Config,PIANO_SMMU_PROBE_REPORT *Report){
  if(!Report)return EFI_INVALID_PARAMETER;
  if(!Source || !Source->Read32 || !Source->Fence || !Source->NowUs || (Master!=PianoIndependentUfs && Master!=PianoIndependentUsb)){
    ZeroMem(Report,sizeof(*Report));Report->Revision=1;Report->ReadStatus=EFI_INVALID_PARAMETER;Report->Reason=PianoProbeInvalidArguments;return Report->ReadStatus;}
  PIANO_SMMU_PROBE_CONFIG Limits=Config?*Config:(PIANO_SMMU_PROBE_CONFIG){PianoProbeRoutesOnly,0,0,1024,100000};
  if((Limits.Phase!=PianoProbeRoutesOnly && Limits.Phase!=PianoProbeCandidates) || !Limits.MaxReads || Limits.MaxReads>PIANO_SMMU_PROBE_MAX_READS ||
     !Limits.MaxUs || Limits.MaxUs>PIANO_SMMU_PROBE_MAX_US || (Limits.Phase==PianoProbeCandidates && (!Limits.BankCount || Limits.FirstBank>=83 || Limits.BankCount>83-Limits.FirstBank))){
    ZeroMem(Report,sizeof(*Report));Report->Revision=1;Report->ReadStatus=EFI_INVALID_PARAMETER;Report->Reason=PianoProbeInvalidArguments;return Report->ReadStatus;}
  // Copy callbacks/config before clearing caller-supplied report storage.
  PIANO_SMMU_INDEPENDENT_OPS Original=*Source;
  ZeroMem(Report,sizeof(*Report));Report->Revision=1;Report->RequestedPhase=Limits.Phase;Report->Sid=Master==PianoIndependentUfs?0x60:0x40;
  Report->FirstInvalidSlot=127;Report->FirstBank=Limits.FirstBank;Report->BankCount=Limits.Phase==PianoProbeCandidates?Limits.BankCount:0;
  PROBE_TRACKER T={.Source=&Original,.Report=Report,.MaxReads=Limits.MaxReads,.MaxUs=Limits.MaxUs};
  EFI_STATUS S=Exact(Original.NowUs(Original.Context,&T.Start));if(S!=EFI_SUCCESS)return ProbeError(&T,S);
  // Intentionally omit every mutation/cache callback from the read view.
  PIANO_SMMU_INDEPENDENT_OPS ReadView={.Context=&T,.Read32=ProbeRead,.Fence=ProbeFence};
  S=IdleOp(&ReadView,Master);if(S!=EFI_SUCCESS){if(T.IoFailed || T.BudgetFailed)return ProbeError(&T,S);Report->Reason=PianoProbeMasterBusy;return Report->ReadStatus=EFI_SUCCESS;}
  Report->IdleObserved=TRUE;
  S=RoutesOp(&ReadView,&Report->Initial);if(S!=EFI_SUCCESS){if(T.IoFailed || T.BudgetFailed)return ProbeError(&T,S);Report->Reason=PianoProbeGlobalGeometry;return Report->ReadStatus=EFI_SUCCESS;}
  Report->RoutesObserved=TRUE;
  for(UINTN I=0;I<127;++I){UINT32 Smr=Report->Initial.Smr[I],S2=Report->Initial.S2cr[I];
    if(!(Smr&BIT31)){if(Report->FirstInvalidSlot==127)Report->FirstInvalidSlot=(UINT16)I;continue;}
    if(((Report->Sid^(UINT16)Smr)&~((Smr>>16)&0x7fff))==0)++Report->TargetMatches;else ++Report->PeerRoutes;
    if(((S2>>16)&3)==0){if((S2&255)>=83){Report->Reason=PianoProbeGlobalGeometry;return Report->ReadStatus=EFI_SUCCESS;}Report->Referenced[S2&255]=TRUE;}
  }
  if(Report->TargetMatches){Report->Reason=PianoProbeTargetSidExists;return Report->ReadStatus=EFI_SUCCESS;}
  if(Report->FirstInvalidSlot==127){Report->Reason=PianoProbeNoRouteSlot;return Report->ReadStatus=EFI_SUCCESS;}
  if(Limits.Phase==PianoProbeCandidates){
    UINT32 Referenced=0;
    for(UINTN N=Limits.FirstBank;N<Limits.FirstBank+Limits.BankCount;++N){
      // Active peer references are already a refusal. Do not read their CB
      // merely to discover that the visible route cannot be taken over.
      if(Report->Referenced[N]){++Referenced;continue;}
      S=BankOp(&ReadView,N,&Report->Initial.Banks[N]);if(S!=EFI_SUCCESS)return ProbeError(&T,S);
      Report->CandidateRead[N]=TRUE;++Report->BanksRead;
      CONST PIANO_SMMU_INDEPENDENT_BANK *B=&Report->Initial.Banks[N];
      if(!(B->Sctlr&1) && !(B->Fsr&PIANO_SMMU_FSR_FAULT_MASK)){Report->Candidate[N]=TRUE;++Report->CandidateCount;}
    }
    Report->CandidatesObserved=TRUE;
    Report->Reason=Report->CandidateCount?PianoProbeActlrUnknown:Referenced==Limits.BankCount?PianoProbePeerVisible:PianoProbeAllCbBusy;
  }else Report->Reason=PianoProbeObservationOnly;
  S=RoutesOp(&ReadView,&Report->Recheck);if(S!=EFI_SUCCESS){if(T.IoFailed || T.BudgetFailed)return ProbeError(&T,S);Report->Reason=PianoProbeGlobalGeometry;return Report->ReadStatus=EFI_SUCCESS;}
  if(!SameRoutes(&Report->Initial,&Report->Recheck)){Report->Reason=PianoProbeTopologyChanged;return Report->ReadStatus=EFI_SUCCESS;}
  if(Limits.Phase==PianoProbeCandidates)for(UINTN N=Limits.FirstBank;N<Limits.FirstBank+Limits.BankCount;++N)if(Report->CandidateRead[N]){
    S=BankOp(&ReadView,N,&Report->Recheck.Banks[N]);if(S!=EFI_SUCCESS)return ProbeError(&T,S);
    if(CompareMem(&Report->Initial.Banks[N],&Report->Recheck.Banks[N],sizeof(Report->Initial.Banks[N]))){Report->Reason=PianoProbeTopologyChanged;return Report->ReadStatus=EFI_SUCCESS;}
  }
  S=IdleOp(&ReadView,Master);if(S!=EFI_SUCCESS){if(T.IoFailed || T.BudgetFailed)return ProbeError(&T,S);Report->Reason=PianoProbeMasterBusy;return Report->ReadStatus=EFI_SUCCESS;}
  Report->RecheckObserved=TRUE;return Report->ReadStatus=EFI_SUCCESS;
}
CONST CHAR8 *PianoSmmuIndependentProbeReasonText(PIANO_SMMU_PROBE_REASON Reason){
  switch(Reason){
    case PianoProbeObservationOnly:return "observation-only";case PianoProbeMasterBusy:return "master-busy";
    case PianoProbeTargetSidExists:return "target-SID-exists";case PianoProbeAllCbBusy:return "all-scanned-CBs-busy";
    case PianoProbeReadFailed:return "read-failure";case PianoProbeGlobalGeometry:return "global-geometry-or-state";
    case PianoProbeNoRouteSlot:return "no-invalid-route-slot";case PianoProbePeerVisible:return "peer-visible-in-candidate-range";
    case PianoProbeActlrUnknown:return "ACTLR-TBU-secure-ownership-unverified";case PianoProbeBudgetExceeded:return "read-budget-exceeded";
    case PianoProbeTopologyChanged:return "topology-changed";case PianoProbeInvalidArguments:return "invalid-arguments";
    default:return "unknown-reason";
  }
}
