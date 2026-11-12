// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoPogoProbe.h"
#include <PiDxe.h>
#include <Protocol/Cpu.h>
#include <Protocol/EFIClock.h>
#include <Protocol/MemoryAttribute.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#define CACHE_TYPES (EFI_MEMORY_UC|EFI_MEMORY_WC|EFI_MEMORY_WT|EFI_MEMORY_WB|EFI_MEMORY_UCE)

typedef struct {UINT64 El,Sctlr,Tcr,Ttbr,Mair;} CPU_STATE;
STATIC PIANO_POGO_PROBE_REPORT mReport={.Status=EFI_NOT_STARTED};
STATIC CPU_STATE mState;
STATIC EFI_CPU_ARCH_PROTOCOL *mCpu;
STATIC EFI_CLOCK_PROTOCOL *mClock;
STATIC BOOLEAN mBusy,mSync,mSError;
STATIC UINT32 mSequence;
STATIC UINT64 mFrequency,mStart,mLast,mCounterFirst,mCounterEnd;
STATIC volatile BOOLEAN mArmed,mFaulted,mInHandler;
STATIC volatile UINTN mFaultPc,mResumePc,mAddress;
STATIC EFI_TPL mOldTpl;
STATIC UINT32 Crc(CONST CHAR8 *P,UINTN N) {
  UINT32 C=MAX_UINT32;for(UINTN I=0;I<N;++I){C^=(UINT8)P[I];for(UINTN J=0;J<8;++J)C=(C>>1)^((C&1)?0xedb88320U:0);}return ~C;
}
STATIC VOID Emit(CONST CHAR8 *Body,UINTN Bytes) {
  UINT32 Hash=Crc(Body,Bytes);
  DEBUG((DEBUG_WARN,"SUNUEFI_POGO_PROBE %a crc32=%08x\n",Body,Hash));
  DEBUG((DEBUG_WARN,"SUNUEFI_POGO_PROBE_COPY %a crc32=%08x\n",Body,Hash));
}
VOID PianoPogoProbeReemit(VOID) {
  CHAR8 Body[512];UINTN N=AsciiSPrint(Body,sizeof(Body),
    "phase=summary seq=%u gate=%u status=%lx complete=%u reads=%u attempted=%u recovered=%u last=%lx elr=%lx esr=%lx far=%lx handlers_retained=%u fatal=%u memory_attr_present=%u memory_attr_called=0 pte_walk=0 clock_writes=0 gpio_writes=0 fifo_pop=0 pio_permitted=0 hard_bus_timeout_verified=0",
    mSequence++,mReport.Gate,(UINT64)mReport.Status,mReport.Complete,mReport.Reads,mReport.MmioAttempted,mReport.RecoveredFaults,
    (UINT64)mReport.LastAddress,mReport.Elr,mReport.Esr,mReport.Far,mReport.HandlersRetained,mReport.Fatal,mReport.MemoryAttributePresent);
  Emit(Body,N);
}
CONST PIANO_POGO_PROBE_REPORT *PianoPogoProbeReport(VOID){return &mReport;}
STATIC VOID EFIAPI Exception(EFI_EXCEPTION_TYPE Type,EFI_SYSTEM_CONTEXT Context) {
  if(mInHandler){mReport.Fatal=mReport.HandlersRetained=TRUE;CpuDeadLoop();}
  mInHandler=TRUE;
  EFI_SYSTEM_CONTEXT_AARCH64 *C=Context.SystemContextAArch64;
  mReport.Status=EFI_NOT_READY;mReport.Complete=FALSE;mReport.Elr=C->ELR;mReport.Esr=C->ESR;mReport.Far=C->FAR;
  // Recover only the one exact 32-bit LDR, valid FAR, same-EL data READ abort.
  // An asynchronous error has no reliable instruction attribution.
  if(Type==EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS && mArmed && C->ELR==mFaultPc &&
     C->FAR==mAddress && ((C->ESR>>26)&63)==0x25 && (C->ESR&BIT25) &&
     ((C->SPSR&15)==4 || (C->SPSR&15)==5) && !(C->ESR&(BIT6|BIT10))) {
    mFaulted=TRUE;++mReport.RecoveredFaults;
    CHAR8 Body[256];UINTN N=AsciiSPrint(Body,sizeof(Body),"phase=fault seq=%u type=%u pc=%lx esr=%lx far=%lx spsr=%lx resume=%lx recovered=1 status=%lx",
      mSequence++,(UINT32)Type,C->ELR,C->ESR,C->FAR,C->SPSR,(UINT64)mResumePc,(UINT64)EFI_NOT_READY);Emit(Body,N);
    C->ELR=mResumePc;mInHandler=FALSE;return;
  }
  mReport.Fatal=mReport.HandlersRetained=TRUE;PianoPogoProbeReemit();CpuDeadLoop();
}
STATIC EFI_STATUS CpuState(CPU_STATE *C) {
#ifdef PIANO_POGO_PROBE_HOST_TEST
  extern EFI_STATUS PianoPogoHostCpuState(VOID *);return PianoPogoHostCpuState(C);
#elif defined(__aarch64__)
  __asm__ volatile("mrs %0, CurrentEL":"=r"(C->El));if(C->El!=4)return EFI_UNSUPPORTED;
  __asm__ volatile("mrs %0, sctlr_el1\n mrs %1, tcr_el1\n mrs %2, ttbr0_el1\n mrs %3, mair_el1"
    :"=r"(C->Sctlr),"=r"(C->Tcr),"=r"(C->Ttbr),"=r"(C->Mair)::"memory");return EFI_SUCCESS;
#else
  (VOID)C;return EFI_UNSUPPORTED;
#endif
}
STATIC BOOLEAN ValidCpu(CONST CPU_STATE *C) {
  STATIC CONST UINT8 Widths[]={32,36,40,42,44,48};
  UINT32 T0=(UINT32)(C->Tcr&63),Ips=(UINT32)((C->Tcr>>32)&7);
  return C->El==4 && (C->Sctlr&1) && !(C->Sctlr&BIT25) && T0>=16 && T0<=39 && Ips<=5 &&
    !(C->Tcr&(BIT7|BIT39|BIT40|BIT59)) && ((C->Tcr>>14)&3)==0 && !(C->Ttbr&0xffe) &&
    (C->Ttbr&0x0000fffffffff000ULL)<(1ULL<<Widths[Ips]);
}
STATIC EFI_STATUS At(UINTN Address,UINT64 *Par) {
#ifdef PIANO_POGO_PROBE_HOST_TEST
  extern EFI_STATUS PianoPogoHostAt(UINTN,UINT64 *);return PianoPogoHostAt(Address,Par);
#elif defined(__aarch64__)
  UINT64 Mask,Old;
  __asm__ volatile("mrs %0, daif\n msr daifset, #15\n mrs %1, par_el1\n at s1e1r, %3\n isb\n mrs %2, par_el1\n msr par_el1, %1\n msr daif, %0"
    :"=&r"(Mask),"=&r"(Old),"=&r"(*Par):"r"(Address):"memory");return EFI_SUCCESS;
#else
  (VOID)Address;(VOID)Par;return EFI_UNSUPPORTED;
#endif
}
STATIC EFI_STATUS Budget(VOID) {
  UINT64 Now=GetPerformanceCounter();
  if(mCounterEnd>=mCounterFirst){if(Now<mLast)return EFI_NOT_READY;mLast=Now;if(Now-mStart>=mFrequency/100)return EFI_NOT_READY;}
  else {if(Now>mLast)return EFI_NOT_READY;mLast=Now;if(mStart-Now>=mFrequency/100)return EFI_NOT_READY;}
  return EFI_SUCCESS; // 10ms absolute read-session cap, plus a fixed 34-read count.
}
STATIC UINT32 Be32(CONST UINT8 *P){return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];}
STATIC BOOLEAN Reg(CONST VOID *Fdt,CONST CHAR8 *Path,UINT32 Base,UINT32 Bytes) {
  INT32 Node=FdtPathOffset(Fdt,Path),Length=0;if(Node<0)return FALSE;
  CONST UINT8 *P=FdtGetProp(Fdt,Node,"reg",&Length);
  return P && Length==8 && Be32(P)==Base && Be32(P+4)==Bytes;
}
STATIC EFI_STATUS Mapping(UINTN Base,UINTN Bytes) {
  for(UINTN Page=Base;Page<Base+Bytes;Page+=4096) {
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR D={0};UINT64 Par=1;
    EFI_STATUS S=gDS->GetMemorySpaceDescriptor(Page,&D);
    CHAR8 Body[256];UINTN N=AsciiSPrint(Body,sizeof(Body),"phase=gcd seq=%u page=%lx native=%lx base=%lx bytes=%lx type=%u attrs=%lx",
      mSequence++,(UINT64)Page,(UINT64)S,D.BaseAddress,D.Length,(UINT32)D.GcdMemoryType,D.Attributes);Emit(Body,N);
    if(S!=EFI_SUCCESS || D.GcdMemoryType!=EfiGcdMemoryTypeMemoryMappedIo ||
       D.BaseAddress>Page || !D.Length || D.BaseAddress>MAX_UINT64-D.Length ||
       D.Length<4096 || Page-D.BaseAddress>D.Length-4096 ||
       (D.Attributes&CACHE_TYPES)!=EFI_MEMORY_UC || (D.Attributes&EFI_MEMORY_RP))return EFI_NOT_READY;
    S=At(Page,&Par);UINT8 Attr=(UINT8)(Par>>56);
    N=AsciiSPrint(Body,sizeof(Body),"phase=mapping seq=%u page=%lx gcd_type=%u attrs=%lx par=%lx at=%lx device=%u",
      mSequence++,(UINT64)Page,(UINT32)D.GcdMemoryType,D.Attributes,Par,(UINT64)S,Attr==0 || Attr==4 || Attr==8 || Attr==12);Emit(Body,N);
    if(S!=EFI_SUCCESS || (Par&1) || (Par&0x000ffffffffFF000ULL)!=Page || !(Attr==0 || Attr==4 || Attr==8 || Attr==12))return EFI_NOT_READY;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Clocks(VOID) {
  STATIC CONST CHAR8 *Names[]={"gcc_qupv3_wrap1_core_clk","gcc_qupv3_wrap1_core_2x_clk",
    "gcc_qupv3_wrap_1_m_ahb_clk","gcc_qupv3_wrap_1_s_ahb_clk","gcc_qupv3_wrap1_s6_clk"};
  if(mClock->Version!=0x1000b || !mClock->GetClockID || !mClock->IsClockEnabled || !mClock->IsClockOn || !mClock->GetClockFreqHz)return EFI_NOT_READY;
  for(UINTN I=0;I<ARRAY_SIZE(Names);++I) {
    UINTN Id=0;BOOLEAN Enabled=FALSE,On=FALSE;UINT32 Hz=0;
    EFI_STATUS S=mClock->GetClockID(mClock,Names[I],&Id);
    if(S==EFI_SUCCESS)S=mClock->IsClockEnabled(mClock,Id,&Enabled);
    if(S==EFI_SUCCESS)S=mClock->IsClockOn(mClock,Id,&On);
    if(S==EFI_SUCCESS)S=mClock->GetClockFreqHz(mClock,Id,&Hz);
    CHAR8 Body[256];UINTN N=AsciiSPrint(Body,sizeof(Body),"phase=clock seq=%u name=%a id=%lu native=%lx enabled=%u on=%u reported_hz=%u writes=0",
      mSequence++,Names[I],(UINT64)Id,(UINT64)S,Enabled,On,Hz);Emit(Body,N);
    if(S!=EFI_SUCCESS || Enabled!=TRUE || On!=TRUE || !Hz)return EFI_NOT_READY;
  }
  return EFI_SUCCESS;
}
STATIC BOOLEAN Offset(UINT32 O) {
  STATIC CONST UINT32 Allowed[]={0x68,0x64,0x40,0x258,0xe18,0xe1c,0x7c,0x48,0x278,0x254,0x260,0x264,0x284,0x288,
    0xe24,0xe28,0x908,0x610,0x640,0x614,0x604,0x634,0x800,0x804,0x80c,0x810,0x26c,0x270,0x600,0x630};
  for(UINTN I=0;I<ARRAY_SIZE(Allowed);++I)if(O==Allowed[I])return TRUE;
  return FALSE;
}
STATIC EFI_STATUS Guarded(UINTN Address,UINT32 *Value) {
  CPU_STATE Now;UINT32 V=0;
  if(!Value || !mSync || !mSError || mReport.Reads>=34 || Budget()!=EFI_SUCCESS ||
     CpuState(&Now)!=EFI_SUCCESS || CompareMem(&Now,&mState,sizeof(Now)))return EFI_NOT_READY;
  mReport.MmioAttempted=TRUE;mReport.LastAddress=mAddress=Address;mFaulted=FALSE;mArmed=TRUE;
  CHAR8 Body[256];UINTN N=AsciiSPrint(Body,sizeof(Body),"phase=read-next seq=%u addr=%lx status=%lx protected=1 writes=0",
    mSequence++,(UINT64)Address,(UINT64)EFI_NOT_READY);Emit(Body,N);
#ifdef PIANO_POGO_PROBE_HOST_TEST
  extern UINT32 PianoPogoHostLoad(UINTN);mFaultPc=0x1000;mResumePc=0x1004;V=PianoPogoHostLoad(Address);
#elif defined(__aarch64__)
  UINT64 Mask;
  // Mask IRQ/FIQ only, deliver SError inside the armed guard, restore DAIF.
  __asm__ volatile("mrs %0, daif\n msr daifset, #3\n msr daifclr, #4\n adr x9, 1f\n str x9, [%2]\n adr x9, 2f\n str x9, [%3]\n dsb sy\n isb\n 1: ldr %w1, [%4]\n 2: dsb sy\n isb\n msr daif, %0"
    :"=&r"(Mask),"+r"(V):"r"(&mFaultPc),"r"(&mResumePc),"r"(Address):"x9","memory");
#else
  mArmed=FALSE;return EFI_UNSUPPORTED;
#endif
  mArmed=FALSE;++mReport.Reads;
  EFI_STATUS S=mFaulted || Budget()!=EFI_SUCCESS || CpuState(&Now)!=EFI_SUCCESS ||
    CompareMem(&Now,&mState,sizeof(Now))?EFI_NOT_READY:EFI_SUCCESS;
  if(S==EFI_SUCCESS)*Value=V;
  N=AsciiSPrint(Body,sizeof(Body),"phase=read seq=%u addr=%lx value=%08x valid=%u status=%lx recovered=%u writes=0",
    mSequence++,(UINT64)Address,S==EFI_SUCCESS?V:0,S==EFI_SUCCESS,(UINT64)S,mFaulted);Emit(Body,N);
  return S;
}
STATIC EFI_STATUS EFIAPI SeRead(VOID *Context,UINT32 O,UINT32 *Value) {
  if(Context!=&mReport || !Offset(O))return EFI_ACCESS_DENIED;
  return Guarded(PIANO_GENI_SE6_BASE+O,Value);
}
STATIC EFI_STATUS Unregister(VOID) {
  EFI_STATUS S=EFI_SUCCESS;
  if(mSError){S=mCpu->RegisterInterruptHandler(mCpu,EXCEPT_AARCH64_SERROR,NULL);if(S!=EFI_SUCCESS)return S;mSError=FALSE;}
  if(mSync){S=mCpu->RegisterInterruptHandler(mCpu,EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS,NULL);if(S!=EFI_SUCCESS)return S;mSync=FALSE;}
  return S;
}
EFI_STATUS PianoProbePogo(CONST VOID *Fdt) {
  if(!PIANO_POGO_PROBE_EXPERIMENT){mReport.Status=EFI_UNSUPPORTED;PianoPogoProbeReemit();return EFI_UNSUPPORTED;}
  if(mBusy || mReport.Started)return EFI_ALREADY_STARTED;
  ZeroMem(&mReport,sizeof(mReport));mReport.Started=TRUE;mReport.Status=EFI_NOT_READY;mBusy=TRUE;mReport.Gate=1;
  if(!Fdt || !gBS || !gBS->LocateProtocol || !gBS->RaiseTPL || !gBS->RestoreTPL || !gDS || !gDS->GetMemorySpaceDescriptor ||
     !Reg(Fdt,"/soc/qcom,qupv3_1_geni_se@ac0000/i2c@a98000",0xa98000,0x4000) ||
     !Reg(Fdt,"/soc/qcom,qupv3_1_geni_se@ac0000",0xac0000,0x2000))goto End;
  mReport.Gate=2;
  if(CpuState(&mState)!=EFI_SUCCESS || !ValidCpu(&mState))goto End;
  CHAR8 Body[256];UINTN N=AsciiSPrint(Body,sizeof(Body),"phase=cpu seq=%u el=%lx sctlr=%lx tcr=%lx ttbr=%lx mair=%lx pte_walk=0 stage2_state_observed=0",
    mSequence++,mState.El,mState.Sctlr,mState.Tcr,mState.Ttbr,mState.Mair);Emit(Body,N);
  VOID *Attributes=NULL;
  mReport.MemoryAttributePresent=gBS->LocateProtocol(&gEfiMemoryAttributeProtocolGuid,NULL,&Attributes)==EFI_SUCCESS && Attributes!=NULL;
  // Mu GetMemoryAttributes recursively dereferences physical PTE tables.
  // Presence is recorded only; first probe never calls this indirect walk.
  mReport.Gate=3;EFI_GUID ClockGuid=EFI_CLOCK_PROTOCOL_GUID;
  if(gBS->LocateProtocol(&gEfiCpuArchProtocolGuid,NULL,(VOID **)&mCpu)!=EFI_SUCCESS || !mCpu || !mCpu->RegisterInterruptHandler ||
     gBS->LocateProtocol(&ClockGuid,NULL,(VOID **)&mClock)!=EFI_SUCCESS || !mClock)goto End;
  mReport.Gate=4;mOldTpl=gBS->RaiseTPL(TPL_CALLBACK);
  if(mCpu->RegisterInterruptHandler(mCpu,EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS,Exception)!=EFI_SUCCESS)goto RestoreTpl;
  mSync=TRUE;
  if(mCpu->RegisterInterruptHandler(mCpu,EXCEPT_AARCH64_SERROR,Exception)!=EFI_SUCCESS)goto Remove;
  mSError=TRUE;
  mReport.Gate=5;
  if(Mapping(0xa98000,0x4000)!=EFI_SUCCESS || Mapping(0xac0000,0x2000)!=EFI_SUCCESS)goto Remove;
  mReport.Gate=6;if(Clocks()!=EFI_SUCCESS)goto Remove;
  mReport.Gate=7;
  mFrequency=GetPerformanceCounterProperties(&mCounterFirst,&mCounterEnd);
  if(mFrequency<100 || mCounterFirst==mCounterEnd)goto Remove;
  mStart=mLast=GetPerformanceCounter();mReport.Gate=8;
  STATIC CONST UINT32 WrapperOffsets[]={4,0x118,0x120,0x21c};
  for(UINTN I=0;I<ARRAY_SIZE(WrapperOffsets);++I)if(Guarded(0xac0000+WrapperOffsets[I],&mReport.Wrapper[I])!=EFI_SUCCESS)goto Remove;
  PIANO_GENI_PIO_IO Io={.Base=PIANO_GENI_SE6_BASE,.Bytes=PIANO_GENI_SE6_BYTES,.Context=&mReport,.Read32=SeRead};
  mReport.Status=PianoGeniI2cPioCapture(&Io,&mReport.Se6);mReport.Complete=mReport.Status==EFI_SUCCESS;
Remove:
  if(Unregister()!=EFI_SUCCESS){mReport.Gate=9;mReport.Status=EFI_NOT_READY;mReport.Complete=FALSE;mReport.Fatal=mReport.HandlersRetained=TRUE;PianoPogoProbeReemit();CpuDeadLoop();}
RestoreTpl:
  gBS->RestoreTPL(mOldTpl);
End:
  mBusy=FALSE;PianoPogoProbeReemit();return mReport.Status;
}
