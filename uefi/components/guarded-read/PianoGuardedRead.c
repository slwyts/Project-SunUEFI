// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real DXE exception guard. No target write, PTE walk, cache operation or DMA.
#include "PianoGuardedRead.h"
#include <Protocol/Cpu.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/TimerLib.h>
#define CACHE_TYPES (EFI_MEMORY_UC|EFI_MEMORY_WC|EFI_MEMORY_WT|EFI_MEMORY_WB|EFI_MEMORY_UCE)
typedef struct {UINT64 El,Sctlr,Tcr,Ttbr0,Ttbr1,Mair;} CPU_STATE;
typedef struct {
  PIANO_GUARDED_CONFIG Config;PIANO_GUARDED_REPORT Report;CPU_STATE CpuState;
  EFI_CPU_ARCH_PROTOCOL *Cpu;EFI_CPU_REGISTER_INTERRUPT_HANDLER Register;
  EFI_EVENT Exit;VOID *Token;
  UINT64 Frequency,Start,Last,CounterFirst,CounterEnd,Limit;
  BOOLEAN Busy,Blocked;
  volatile BOOLEAN Armed,Faulted,InHandler;
  volatile UINTN FaultPc,ResumePc,Address;
  UINT32 Scratch[PIANO_GUARDED_COPY_MAX/4];
} PRODUCER;
STATIC PRODUCER m;STATIC UINTN mSequence;
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
STATIC BOOLEAN Live(VOID){return !m.Report.ServicesLost&&m.Config.BootServicesAlive&&m.Config.BootServicesAlive(m.Config.Context)==TRUE;}
STATIC EFI_STATUS Retain(EFI_STATUS S){m.Blocked=m.Report.Retained=TRUE;m.Report.Active=FALSE;return m.Report.Status=Exact(S);}
STATIC VOID Fatal(VOID){m.Report.Fatal=TRUE;Retain(EFI_ABORTED);
#if defined(__aarch64__) && !defined(PIANO_GUARDED_HOST_TEST)
  __asm__ volatile("msr daifset, #15":::"memory");
#endif
  CpuDeadLoop();while(TRUE){}
}
STATIC VOID EFIAPI ExitNotify(EFI_EVENT Event,VOID *Context){(VOID)Event;(VOID)Context;m.Report.ServicesLost=TRUE;Retain(EFI_ABORTED);}
STATIC VOID EFIAPI Exception(EFI_EXCEPTION_TYPE Type,EFI_SYSTEM_CONTEXT Context){
  if(m.InHandler)Fatal();m.InHandler=TRUE;
  EFI_SYSTEM_CONTEXT_AARCH64 *C=Context.SystemContextAArch64;if(!C)Fatal();
  m.Report.Elr=C->ELR;m.Report.Esr=C->ESR;m.Report.Far=C->FAR;m.Report.Spsr=C->SPSR;m.Report.Resume=m.ResumePc;
  UINT64 El;
#ifdef PIANO_GUARDED_HOST_TEST
  extern UINT64 PianoGuardedHostCurrentEl(VOID);El=PianoGuardedHostCurrentEl();
#elif defined(__aarch64__)
  __asm__ volatile("mrs %0, CurrentEL":"=r"(El));
#else
  El=0;
#endif
  if(El==4&&!m.Report.ServicesLost&&m.Report.Active&&Type==EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS&&m.Armed&&
     C->ELR==m.FaultPc&&C->FAR==m.Address&&((C->ESR>>26)&63)==0x25&&(C->ESR&BIT25)&&
     !(C->ESR&(BIT6|BIT10))&&((C->SPSR&15)==4||(C->SPSR&15)==5)){
    m.Faulted=TRUE;++m.Report.RecoveredFaults;C->ELR=m.ResumePc;m.InHandler=FALSE;return;
  }Fatal();
}
STATIC EFI_STATUS CpuState(CPU_STATE *State){
#ifdef PIANO_GUARDED_HOST_TEST
  extern EFI_STATUS PianoGuardedHostCpuState(VOID *);return PianoGuardedHostCpuState(State);
#elif defined(__aarch64__)
  __asm__ volatile("mrs %0, CurrentEL":"=r"(State->El));if(State->El!=4)return EFI_UNSUPPORTED;
  __asm__ volatile("mrs %0, sctlr_el1\n mrs %1, tcr_el1\n mrs %2, ttbr0_el1\n mrs %3, ttbr1_el1\n mrs %4, mair_el1"
    :"=r"(State->Sctlr),"=r"(State->Tcr),"=r"(State->Ttbr0),"=r"(State->Ttbr1),"=r"(State->Mair)::"memory");return EFI_SUCCESS;
#else
  (VOID)State;return EFI_UNSUPPORTED;
#endif
}
STATIC BOOLEAN ValidCpu(CONST CPU_STATE *S){
  STATIC CONST UINT8 Width[]={32,36,40,42,44,48};UINT32 T0=(UINT32)(S->Tcr&63),Ips=(UINT32)((S->Tcr>>32)&7);
  return S->El==4&&(S->Sctlr&1)&&!(S->Sctlr&BIT25)&&T0>=16&&T0<=39&&Ips<=5&&
    !(S->Tcr&(BIT7|BIT37|BIT38|BIT39|BIT40|BIT59))&&((S->Tcr>>14)&3)==0&&!(S->Ttbr0&0xffe)&&
    (S->Ttbr0&0x0000fffffffff000ULL)<(1ULL<<Width[Ips]);
}
STATIC EFI_STATUS At(UINTN Address,UINT64 *Par){
#ifdef PIANO_GUARDED_HOST_TEST
  extern EFI_STATUS PianoGuardedHostAt(UINTN,UINT64 *);return PianoGuardedHostAt(Address,Par);
#elif defined(__aarch64__)
  UINT64 Mask,Old;__asm__ volatile("mrs %0, daif\n msr daifset, #15\n mrs %1, par_el1\n at s1e1r, %3\n isb\n mrs %2, par_el1\n msr par_el1, %1\n msr daif, %0"
    :"=&r"(Mask),"=&r"(Old),"=&r"(*Par):"r"(Address):"memory");return EFI_SUCCESS;
#else
  (VOID)Address;(VOID)Par;return EFI_UNSUPPORTED;
#endif
}
STATIC EFI_STATUS Budget(BOOLEAN Count){
  if(!Live())return Retain(EFI_ABORTED);
  UINT64 Now=GetPerformanceCounter();if(!Live())return Retain(EFI_ABORTED);
  if(Now<MIN(m.CounterFirst,m.CounterEnd)||Now>MAX(m.CounterFirst,m.CounterEnd))return EFI_NOT_READY;
  UINT64 Elapsed;
  if(m.CounterEnd>m.CounterFirst){if(Now<m.Last)return EFI_NOT_READY;Elapsed=Now-m.Start;}
  else{if(Now>m.Last)return EFI_NOT_READY;Elapsed=m.Start-Now;}m.Last=Now;
  return Elapsed>=m.Limit||(Count&&m.Report.Reads>=m.Config.MaxReads)?EFI_TIMEOUT:EFI_SUCCESS;
}
STATIC EFI_STATUS App(VOID){
  if(!Live())return Retain(EFI_ABORTED);
  EFI_TPL Old=m.Config.Services->RaiseTPL(TPL_HIGH_LEVEL);
  if(!Live())return Retain(EFI_ABORTED);m.Config.Services->RestoreTPL(Old);
  if(!Live())return Retain(EFI_ABORTED);return Old==TPL_APPLICATION?EFI_SUCCESS:EFI_UNSUPPORTED;
}
STATIC EFI_STATUS Fresh(VOID){
  if(!Live())return Retain(EFI_ABORTED);EFI_CPU_ARCH_PROTOCOL *Cpu=NULL;
  EFI_STATUS S=m.Config.Services->LocateProtocol(&gEfiCpuArchProtocolGuid,NULL,(VOID **)&Cpu);
  if(!Live())return Retain(EFI_ABORTED);
  if(S!=EFI_SUCCESS||Cpu!=m.Cpu||!Cpu||Cpu->RegisterInterruptHandler!=m.Register)return Retain(S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);
  CPU_STATE Now;S=CpuState(&Now);if(!Live())return Retain(EFI_ABORTED);if(S!=EFI_SUCCESS||CompareMem(&Now,&m.CpuState,sizeof(Now)))return EFI_NOT_READY;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Mapping(CONST PIANO_GUARDED_RANGE *R,UINT64 Page){
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D={0};UINT64 Par=1;
  m.Report.LastMappingPage=Page;m.Report.LastPar=MAX_UINT64;m.Report.LastGcdType=MAX_UINT32;m.Report.LastGcdAttributes=0;
  EFI_STATUS S=m.Config.DxeServices->GetMemorySpaceDescriptor(Page,&D);
  m.Report.LastGcdType=(UINT32)D.GcdMemoryType;m.Report.LastGcdAttributes=D.Attributes;
  if(!Live()){S=Retain(EFI_ABORTED);goto Done;}if(S!=EFI_SUCCESS){S=Exact(S);goto Done;}
  if(D.GcdMemoryType!=R->GcdType||(D.Attributes&CACHE_TYPES)!=R->Cache||(D.Attributes&EFI_MEMORY_RP)||
     !D.Length||D.BaseAddress>Page||D.BaseAddress>MAX_UINT64-D.Length||D.Length<4096||Page-D.BaseAddress>D.Length-4096){S=EFI_NOT_READY;goto Done;}
  S=At((UINTN)Page,&Par);m.Report.LastPar=Par;if(!Live()){S=Retain(EFI_ABORTED);goto Done;}if(S!=EFI_SUCCESS){S=Exact(S);goto Done;}
  if((Par&1)||(Par&0x000ffffffffff000ULL)!=Page||(UINT8)(Par>>56)!=R->ParAttribute)S=EFI_NOT_READY;
Done:
  return m.Report.MappingStatus=S;
}
STATIC BOOLEAN Span(UINTN A,UINTN N,UINTN *End){if(!A||!N||N-1>MAX_UINTN-A)return FALSE;*End=A+N-1;return TRUE;}
STATIC BOOLEAN Overlap(UINTN A,UINTN N,UINTN B,UINTN BN){UINTN AE,BE;return !Span(A,N,&AE)||!Span(B,BN,&BE)||(A<=BE&&B<=AE);}
STATIC BOOLEAN Output(CONST VOID *P,UINTN N){
  if(Overlap((UINTN)P,N,(UINTN)&m,sizeof(m)))return FALSE;
  for(UINT32 I=0;I<m.Config.RangeCount;++I)if(Overlap((UINTN)P,N,(UINTN)m.Config.Ranges[I].Base,(UINTN)m.Config.Ranges[I].Bytes))return FALSE;
  if(Overlap((UINTN)P,N,(UINTN)m.Config.Services,sizeof(*m.Config.Services))||Overlap((UINTN)P,N,(UINTN)m.Config.DxeServices,sizeof(*m.Config.DxeServices)))return FALSE;
  return !m.Cpu||!Overlap((UINTN)P,N,(UINTN)m.Cpu,sizeof(*m.Cpu));
}
STATIC CONST PIANO_GUARDED_RANGE *Allowed(UINT64 Address,UINTN Bytes){
  for(UINT32 I=0;I<m.Config.RangeCount;++I){CONST PIANO_GUARDED_RANGE *R=&m.Config.Ranges[I];if(Address>=R->Base&&Address-R->Base<=R->Bytes&&Bytes<=R->Bytes-(Address-R->Base))return R;}return NULL;
}
STATIC EFI_STATUS Load32(UINTN Address,UINT32 *Value){
  EFI_STATUS S=Budget(TRUE);if(S!=EFI_SUCCESS)return S;S=Fresh();if(S!=EFI_SUCCESS)return S;
  m.Report.LastAddress=m.Address=Address;m.Faulted=FALSE;m.Armed=TRUE;UINT32 V=0;
#ifdef PIANO_GUARDED_HOST_TEST
  extern UINT32 PianoGuardedHostLoad(UINTN);m.FaultPc=0x1000;m.ResumePc=0x1004;V=PianoGuardedHostLoad(Address);
#elif defined(__aarch64__)
  UINT64 Mask;__asm__ volatile("mrs %0, daif\n msr daifset, #3\n msr daifclr, #4\n adr x9, 1f\n str x9, [%2]\n adr x9, 2f\n str x9, [%3]\n dsb sy\n isb\n 1: ldr %w1, [%4]\n 2: dsb sy\n isb\n msr daif, %0"
    :"=&r"(Mask),"+r"(V):"r"(&m.FaultPc),"r"(&m.ResumePc),"r"(Address):"x9","memory");
#else
  m.Armed=FALSE;return EFI_UNSUPPORTED;
#endif
  m.Armed=FALSE;++m.Report.Reads;if(!Live())return Retain(EFI_ABORTED);if(m.Faulted)return EFI_NOT_READY;
  S=Budget(FALSE);if(S!=EFI_SUCCESS)return S;S=Fresh();if(S!=EFI_SUCCESS)return S;*Value=V;return EFI_SUCCESS;
}
STATIC EFI_STATUS Cleanup(VOID){
  if(m.Blocked||m.Report.Retained||!Live())return Retain(EFI_ABORTED);
  if(m.Report.SyncOwned||m.Report.SErrorOwned){EFI_STATUS S=Fresh();if(S!=EFI_SUCCESS)return Retain(S);
    EFI_TPL Old=m.Config.Services->RaiseTPL(TPL_CALLBACK);if(!Live())return Retain(EFI_ABORTED);
    if(m.Report.SErrorOwned){S=m.Register(m.Cpu,EXCEPT_AARCH64_SERROR,NULL);if(!Live()||S!=EFI_SUCCESS)return Retain(!Live()?EFI_ABORTED:S);m.Report.SErrorOwned=FALSE;}
    if(m.Report.SyncOwned){S=m.Register(m.Cpu,EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS,NULL);if(!Live()||S!=EFI_SUCCESS)return Retain(!Live()?EFI_ABORTED:S);m.Report.SyncOwned=FALSE;}
    m.Config.Services->RestoreTPL(Old);if(!Live())return Retain(EFI_ABORTED);
  }
  if(m.Exit){EFI_STATUS S=m.Config.Services->CloseEvent(m.Exit);if(!Live()||S!=EFI_SUCCESS)return Retain(!Live()?EFI_ABORTED:S);m.Exit=NULL;}
  m.Report.Active=FALSE;m.Token=NULL;return EFI_SUCCESS;
}
EFI_STATUS PianoGuardedReadBegin(CONST PIANO_GUARDED_CONFIG *Config,VOID **Context){
  if(!Config||!Context||Overlap((UINTN)Config,sizeof(*Config),(UINTN)&m,sizeof(m))||Overlap((UINTN)Context,sizeof(*Context),(UINTN)&m,sizeof(m))||Overlap((UINTN)Context,sizeof(*Context),(UINTN)Config,sizeof(*Config)))return EFI_INVALID_PARAMETER;
  if(m.Blocked||m.Report.Retained)return EFI_ACCESS_DENIED;if(m.Busy||m.Report.Active)return EFI_ALREADY_STARTED;
  if(!Config->Services||!Config->DxeServices||!Config->BootServicesAlive)return EFI_INVALID_PARAMETER;
  if(Config->BootServicesAlive(Config->Context)!=TRUE)return EFI_NOT_READY;
  if(!Config->Services->LocateProtocol||!Config->Services->RaiseTPL||!Config->Services->RestoreTPL||!Config->Services->CreateEventEx||!Config->Services->CloseEvent||!Config->DxeServices->GetMemorySpaceDescriptor||
     !Config->RangeCount||Config->RangeCount>PIANO_GUARDED_RANGE_MAX||!Config->MaxReads||Config->MaxReads>PIANO_GUARDED_READ_MAX||!Config->MaxUsecs||Config->MaxUsecs>PIANO_GUARDED_USECS_MAX)return EFI_INVALID_PARAMETER;
  if(Overlap((UINTN)Context,sizeof(*Context),(UINTN)Config->Services,sizeof(*Config->Services))||Overlap((UINTN)Context,sizeof(*Context),(UINTN)Config->DxeServices,sizeof(*Config->DxeServices)))return EFI_INVALID_PARAMETER;
  for(UINT32 I=0;I<Config->RangeCount;++I){CONST PIANO_GUARDED_RANGE *R=&Config->Ranges[I];
    BOOLEAN Attr=R->Cache==EFI_MEMORY_UC?(R->ParAttribute==0||R->ParAttribute==4||R->ParAttribute==8||R->ParAttribute==12||R->ParAttribute==0x44):R->Cache==EFI_MEMORY_WB&&R->ParAttribute==0xff;
    if(!R->Base||!R->Bytes||(R->Base&3)||(R->Bytes&3)||R->Base>MAX_UINT64-R->Bytes||R->Base+R->Bytes>MAX_UINTN||!Attr||
       !(R->GcdType==EfiGcdMemoryTypeReserved||R->GcdType==EfiGcdMemoryTypeSystemMemory||R->GcdType==EfiGcdMemoryTypeMemoryMappedIo))return EFI_INVALID_PARAMETER;
    if(Overlap((UINTN)Context,sizeof(*Context),(UINTN)R->Base,(UINTN)R->Bytes))return EFI_INVALID_PARAMETER;
    for(UINT32 J=0;J<I;++J)if(Overlap((UINTN)R->Base,(UINTN)R->Bytes,(UINTN)Config->Ranges[J].Base,(UINTN)Config->Ranges[J].Bytes))return EFI_INVALID_PARAMETER;
  }
  ZeroMem(&m,sizeof(m));m.Config=*Config;Config=&m.Config;m.Busy=TRUE;m.Report.Status=EFI_NOT_READY;
  EFI_STATUS S=App();if(S!=EFI_SUCCESS)goto Done;S=CpuState(&m.CpuState);if(S!=EFI_SUCCESS||!ValidCpu(&m.CpuState)){S=EFI_UNSUPPORTED;goto Done;}
  STATIC CONST UINT8 Width[]={32,36,40,42,44,48};
  UINT64 AddressLimit=MIN(1ULL<<(64-(m.CpuState.Tcr&63)),1ULL<<Width[(m.CpuState.Tcr>>32)&7]);
  for(UINT32 I=0;I<Config->RangeCount;++I)if(Config->Ranges[I].Base+Config->Ranges[I].Bytes>AddressLimit){S=EFI_UNSUPPORTED;goto Done;}
  m.Frequency=GetPerformanceCounterProperties(&m.CounterFirst,&m.CounterEnd);
  if(m.Frequency<1000000||m.Frequency>MAX_UINT64/Config->MaxUsecs||m.CounterFirst==m.CounterEnd){S=EFI_UNSUPPORTED;goto Done;}
  m.Limit=(m.Frequency*Config->MaxUsecs)/1000000;m.Start=m.Last=GetPerformanceCounter();S=Budget(FALSE);if(S!=EFI_SUCCESS)goto Done;
  S=Config->Services->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ExitNotify,&m,&gEfiEventExitBootServicesGuid,&m.Exit);
  if(!Live()||S!=EFI_SUCCESS||!m.Exit){S=Retain(!Live()?EFI_ABORTED:S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S);goto Done;}
  S=Config->Services->LocateProtocol(&gEfiCpuArchProtocolGuid,NULL,(VOID **)&m.Cpu);if(!Live()){S=Retain(EFI_ABORTED);goto Done;}
  if(S!=EFI_SUCCESS||!m.Cpu||!m.Cpu->RegisterInterruptHandler){S=S==EFI_SUCCESS?EFI_UNSUPPORTED:Exact(S);goto Remove;}m.Register=m.Cpu->RegisterInterruptHandler;
  EFI_TPL Old=Config->Services->RaiseTPL(TPL_CALLBACK);if(!Live()){S=Retain(EFI_ABORTED);goto Done;}
  S=m.Register(m.Cpu,EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS,Exception);if(!Live()){S=Retain(EFI_ABORTED);goto Done;}
  if(S==EFI_SUCCESS)m.Report.SyncOwned=TRUE;else if(S!=EFI_ALREADY_STARTED){S=Retain(S);goto Done;}
  if(S==EFI_SUCCESS){S=m.Register(m.Cpu,EXCEPT_AARCH64_SERROR,Exception);if(!Live()){S=Retain(EFI_ABORTED);goto Done;}
    if(S==EFI_SUCCESS)m.Report.SErrorOwned=TRUE;else if(S!=EFI_ALREADY_STARTED){S=Retain(S);goto Done;}}
  Config->Services->RestoreTPL(Old);if(!Live()){S=Retain(EFI_ABORTED);goto Done;}if(S!=EFI_SUCCESS)goto Remove;
  m.Report.Active=TRUE;
  for(UINT32 I=0;I<Config->RangeCount;++I){CONST PIANO_GUARDED_RANGE *R=&m.Config.Ranges[I];
    UINT64 Last=(R->Base+R->Bytes-1)&~0xfffULL;
    for(UINT64 Page=R->Base&~0xfffULL;;Page+=4096){if(m.Report.PagesValidated>=PIANO_GUARDED_PAGE_MAX){S=EFI_BAD_BUFFER_SIZE;goto Remove;}
      S=Budget(FALSE);if(S==EFI_SUCCESS)S=Fresh();if(S==EFI_SUCCESS)S=Mapping(R,Page);if(S!=EFI_SUCCESS)goto Remove;
      ++m.Report.PagesValidated;if(Page==Last)break;}
  }
  if(!Output(Context,sizeof(*Context))){S=EFI_INVALID_PARAMETER;goto Remove;}
  if(mSequence==MAX_UINTN){S=EFI_OUT_OF_RESOURCES;goto Remove;}m.Token=(VOID *)++mSequence;*Context=m.Token;S=EFI_SUCCESS;goto Done;
Remove:
  m.Report.Status=Exact(S);m.Report.CleanupStatus=Cleanup();if(m.Report.CleanupStatus!=EFI_SUCCESS)S=m.Report.CleanupStatus;
Done:
  m.Busy=FALSE;m.Report.Status=Exact(S);return m.Report.Status;
}
EFI_STATUS EFIAPI PianoGuardedTryRead(VOID *Context,UINT64 Address,UINTN Bytes,VOID *Destination){
  if(!Context||Context!=m.Token||!m.Report.Active||m.Blocked||m.Busy)return EFI_ACCESS_DENIED;
  if(!Bytes||Bytes>PIANO_GUARDED_COPY_MAX||(Address&3)||(Bytes&3)||!Output(Destination,Bytes))return EFI_INVALID_PARAMETER;
  CONST PIANO_GUARDED_RANGE *R=Allowed(Address,Bytes);if(!R)return EFI_ACCESS_DENIED;
  m.Busy=TRUE;EFI_STATUS S=App();
  for(UINTN I=0;S==EFI_SUCCESS&&I<Bytes/4;++I){S=Fresh();if(S==EFI_SUCCESS)S=Mapping(R,(Address+I*4)&~0xfffULL);if(S==EFI_SUCCESS)S=Load32((UINTN)Address+I*4,&m.Scratch[I]);}
  if(S==EFI_SUCCESS){S=Fresh();if(S==EFI_SUCCESS)CopyMem(Destination,m.Scratch,Bytes);}ZeroMem(m.Scratch,sizeof(m.Scratch));m.Busy=FALSE;return m.Report.Status=Exact(S);
}
EFI_STATUS EFIAPI PianoGuardedRead32(VOID *Context,UINT64 Address,UINT32 *Value){return PianoGuardedTryRead(Context,Address,4,Value);}
EFI_STATUS PianoGuardedReadEnd(VOID *Context){
  if(m.Blocked||m.Report.Retained)return EFI_ACCESS_DENIED;if(!Context||Context!=m.Token||m.Busy||!m.Report.Active)return EFI_INVALID_PARAMETER;
  m.Busy=TRUE;EFI_STATUS S=App();if(S==EFI_SUCCESS)S=Cleanup();m.Busy=FALSE;return m.Report.CleanupStatus=Exact(S);
}
CONST PIANO_GUARDED_REPORT *PianoGuardedReadReport(VOID){return &m.Report;}
