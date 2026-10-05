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
STATIC EFI_STATUS Rd(PIANO_SMMU_INDEPENDENT *C,UINTN Address,UINT32 *V){return Exact(C->Ops.Read32(C->Ops.Context,Address,V));}
STATIC EFI_STATUS Wr(PIANO_SMMU_INDEPENDENT *C,UINTN Address,UINT32 V){
  BOOLEAN Allowed=Address==BASE+0x800+4*C->Slot || Address==BASE+0xc00+4*C->Slot || Address==BASE+0x1000+4*C->Bank || Address==BASE+0x1800+4*C->Bank;
  if(Address>=CB(C->Bank) && Address<CB(C->Bank)+0x1000){UINTN O=Address-CB(C->Bank);Allowed=O==0 || O==0x10 || O==0x20 || O==0x24 || O==0x28 || O==0x2c || O==0x30 || O==0x38 || O==0x3c || O==0x618 || O==0x7f0;}
  if(!Allowed)return Failure(C,EFI_ACCESS_DENIED);++C->WritesAttempted;
  EFI_STATUS S=Exact(C->Ops.Write32(C->Ops.Context,Address,V));if(S==EFI_SUCCESS)S=Exact(C->Ops.Fence(C->Ops.Context));return S==EFI_SUCCESS?S:Failure(C,S);
}
STATIC EFI_STATUS Idle(PIANO_SMMU_INDEPENDENT *C){
  for(UINTN Sample=0;Sample<2;++Sample){UINT32 V=0;
    if(C->Master==PianoIndependentUfs){CONST UINTN Off[]={0x58,0x78,0x60,0x80,0x24};for(UINTN I=0;I<5;++I){EFI_STATUS S=Rd(C,0x1d84000+Off[I],&V);if(S!=EFI_SUCCESS)return S;if(V)return EFI_NOT_READY;}}
    else{EFI_STATUS S=Rd(C,0xa60c704,&V);if(S!=EFI_SUCCESS)return S;if(V&BIT31)return EFI_NOT_READY;S=Rd(C,0xa60c70c,&V);if(S!=EFI_SUCCESS)return S;if(!(V&BIT22))return EFI_NOT_READY;}
    EFI_STATUS S=Exact(C->Ops.Fence(C->Ops.Context));if(S!=EFI_SUCCESS)return S;
  }return EFI_SUCCESS;
}
STATIC EFI_STATUS Snapshot(PIANO_SMMU_INDEPENDENT *C,PIANO_SMMU_INDEPENDENT_SNAPSHOT *S){
  ZeroMem(S,sizeof(*S));UINT32 *Globals[]={&S->Global,&S->Id0,&S->Id1,&S->Id2,&S->GlobalFault};CONST UINTN Off[]={0,0x20,0x24,0x28,0x48};
  for(UINTN I=0;I<5;++I){EFI_STATUS R=Rd(C,BASE+Off[I],Globals[I]);if(R!=EFI_SUCCESS)return R;}
  if(S->Id0!=0x4c017e7f || S->Id1!=0x60000053 || S->Id2!=0x5111 || (S->Global&(BIT0|BIT3)) || S->GlobalFault)return EFI_UNSUPPORTED;
  for(UINTN I=0;I<127;++I){EFI_STATUS R=Rd(C,BASE+0x800+I*4,&S->Smr[I]);if(R!=EFI_SUCCESS)return R;R=Rd(C,BASE+0xc00+I*4,&S->S2cr[I]);if(R!=EFI_SUCCESS)return R;}
  for(UINTN N=0;N<83;++N){PIANO_SMMU_INDEPENDENT_BANK *B=&S->Banks[N];EFI_STATUS R=Rd(C,BASE+0x1000+4*N,&B->Cbar);if(R!=EFI_SUCCESS)return R;R=Rd(C,BASE+0x1800+4*N,&B->Cba2r);if(R!=EFI_SUCCESS)return R;
    UINT32 *Fields=&B->Sctlr;for(UINTN I=0;I<ARRAY_SIZE(BankOffsets);++I){R=Rd(C,CB(N)+BankOffsets[I],Fields+I);if(R!=EFI_SUCCESS)return R;}}
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
EFI_STATUS PianoSmmuIndependentOpen(PIANO_SMMU_INDEPENDENT *C,PIANO_SMMU_INDEPENDENT_MASTER Master,CONST PIANO_SMMU_INDEPENDENT_OPS *Ops,PIANO_IO_PAGE_TABLE *PT,PIANO_DMA_BUFFER *Tables){
  if(!C || !Ops || !PT || !Tables || (Master!=PianoIndependentUfs && Master!=PianoIndependentUsb) || !Ops->Read32 || !Ops->Write32 || !Ops->Fence || !Ops->Clean || !Ops->NowUs || !Ops->Pause)return EFI_INVALID_PARAMETER;
  if(C->Signature)return EFI_ALREADY_STARTED;
  if(!Tables->Signature || Tables->Cpu!=PT->Tables || Tables->Physical!=PT->Physical || (Tables->Physical&4095) || Tables->Physical>0xfffffffffULL ||
     Tables->ReservedBytes<PIANO_IO_PT_BYTES || Tables->MemoryType!=EfiReservedMemoryType || !(Tables->MemoryAttributes&EFI_MEMORY_WB) || Tables->Active || Tables->Quarantined)return EFI_NOT_READY;
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
