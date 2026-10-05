// Actual singleton adapter; EFI/architectural boundary fixture, no target read.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/wait.h>
#include <unistd.h>
#undef NULL
#define PIANO_GUARDED_HOST_TEST 1
#include "../bootprofiles/guarded-read/PianoGuardedRead.c"
EFI_GUID gEfiCpuArchProtocolGuid={.Data1=1},gEfiEventExitBootServicesGuid=EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
static jmp_buf FatalJump;static BOOLEAN ExpectFatal,Services=TRUE;
VOID EFIAPI CpuDeadLoop(VOID){assert(ExpectFatal);longjmp(FatalJump,1);}
static UINTN Case,Calls,Loads,Unregisters,EventCloses,StateCalls,AtCalls,GcdCalls;
static EFI_BOOT_SERVICES Bs;static EFI_DXE_SERVICES Ds;static EFI_CPU_ARCH_PROTOCOL Cpu,Other;
static EFI_CPU_INTERRUPT_HANDLER Handlers[4];static EFI_TPL Tpl=TPL_APPLICATION;
static EFI_EVENT_NOTIFY FenceNotify;static VOID *FenceContext;static UINT64 Counter=100;
static BOOLEAN CorruptCpu,ReplaceCpu;
static VOID Check(VOID){assert(Services);++Calls;}
static BOOLEAN Alive(VOID *Context){assert(Context==(VOID *)99);return Services;}
static VOID Lost(VOID){assert(FenceNotify);FenceNotify((VOID *)77,FenceContext);Services=FALSE;}
static EFI_TPL EFIAPI Raise(EFI_TPL New){Check();EFI_TPL Old=Tpl;Tpl=New;return Old;}
static VOID EFIAPI Restore(EFI_TPL Old){Check();Tpl=Old;}
static VOID EFIAPI Foreign(EFI_EXCEPTION_TYPE Type,EFI_SYSTEM_CONTEXT Context){(VOID)Type;(VOID)Context;assert(FALSE);}
static EFI_STATUS EFIAPI Register(EFI_CPU_ARCH_PROTOCOL *This,EFI_EXCEPTION_TYPE Type,EFI_CPU_INTERRUPT_HANDLER Handler){
  Check();assert(This==&Cpu&&(Type==0||Type==3));
  if(Handler){if(Handlers[Type])return EFI_ALREADY_STARTED;Handlers[Type]=Handler;
    if((Case==3&&Type==0)||(Case==4&&Type==3))return EFI_WARN_STALE_DATA;
  }else{++Unregisters;assert(Handlers[Type]==Exception);
    if(Case==5&&Type==3)return EFI_WARN_STALE_DATA;if(Case==6&&Type==0)return EFI_DEVICE_ERROR;Handlers[Type]=NULL;
  }return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Out){Check();assert(Guid==&gEfiCpuArchProtocolGuid&&!Registration);*Out=ReplaceCpu?&Other:&Cpu;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL NotifyTpl,EFI_EVENT_NOTIFY Notify,CONST VOID *Context,CONST EFI_GUID *Guid,EFI_EVENT *Event){
  Check();assert(Type==EVT_NOTIFY_SIGNAL&&NotifyTpl==TPL_NOTIFY&&Guid==&gEfiEventExitBootServicesGuid);FenceNotify=Notify;FenceContext=(VOID *)Context;*Event=(VOID *)77;return Case==8?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close(EFI_EVENT Event){Check();assert(Event==(VOID *)77);EventCloses++;return Case==7?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *First,UINT64 *End){*First=0;*End=MAX_UINT64;return 1000000;}
UINT64 EFIAPI GetPerformanceCounter(VOID){return ++Counter;}
EFI_STATUS PianoGuardedHostCpuState(VOID *Pointer){
  CPU_STATE *S=Pointer;StateCalls++;*S=(CPU_STATE){4,1,0x480803514ULL,0xd7fff000,0,0xff44};
  if(Case==9)S->El=8;if(Case==10)S->Tcr|=1ULL<<14;if(CorruptCpu)S->Mair^=1;return EFI_SUCCESS;
}
UINT64 PianoGuardedHostCurrentEl(VOID){return Case==40?8:4;}
static VOID Inject(UINTN Type,UINT64 Pc,UINT64 Far,UINT64 Esr,UINT64 Spsr){
  EFI_SYSTEM_CONTEXT_AARCH64 C={0};C.ELR=Pc;C.FAR=Far;C.ESR=Esr;C.SPSR=Spsr;EFI_SYSTEM_CONTEXT Context={.SystemContextAArch64=&C};
  assert(Handlers[Type]);Handlers[Type]((EFI_EXCEPTION_TYPE)Type,Context);assert(C.ELR==0x1004);
}
EFI_STATUS PianoGuardedHostAt(UINTN Address,UINT64 *Par){
  AtCalls++;UINT64 Attr=Address==0x1fd4000?0:0x44;*Par=Address|(Attr<<56);
  if(Case==11)*Par|=1;if(Case==12)*Par+=4096;if(Case==13)*Par^=1ULL<<56;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Gcd(EFI_PHYSICAL_ADDRESS Address,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
  Check();GcdCalls++;ZeroMem(D,sizeof(*D));D->BaseAddress=Address;D->Length=4096;D->GcdMemoryType=Address==0x1fd4000?EfiGcdMemoryTypeMemoryMappedIo:EfiGcdMemoryTypeReserved;D->Attributes=EFI_MEMORY_UC;
  if(Case==14)D->Attributes=EFI_MEMORY_WB;if(Case==15)D->Length=4;if(Case==16)Counter=200000;
  if(Case==31)Lost();if(Case==36&&Loads)D->Attributes=EFI_MEMORY_WB;return EFI_SUCCESS;
}
UINT32 PianoGuardedHostLoad(UINTN Address){
  Loads++;assert(m.Armed&&Address>=0x81d00000&&Address<0x81d02000);
  if(Case==20)CorruptCpu=TRUE;
  if(Case==21&&Loads==2)Inject(0,0x1000,Address,0x96000010,5);
  if((Case>=22&&Case<=28)||Case==40){
    UINT64 Pc=Case==22?0x1008:0x1000,Far=Case==23?Address+4:Address,Esr=0x96000010,Spsr=5;
    if(Case==24)Esr|=BIT6;if(Case==25)Esr|=BIT10;if(Case==26)Esr=0x92000010;if(Case==27)Spsr=0;
    Inject(Case==28?3:0,Pc,Far,Esr,Spsr);
  }
  if(Case==30)Lost();if(Case==35)assert(PianoGuardedRead32(m.Token,Address,NULL)==EFI_ACCESS_DENIED);
  return (UINT32)(Address^0x12345678);
}
static PIANO_GUARDED_CONFIG Config(VOID){
  Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Bs.LocateProtocol=Locate;Bs.CreateEventEx=Create;Bs.CloseEvent=Close;
  Ds.GetMemorySpaceDescriptor=Gcd;Cpu.RegisterInterruptHandler=Register;Other.RegisterInterruptHandler=Register;
  return (PIANO_GUARDED_CONFIG){.Context=(VOID *)99,.Services=&Bs,.DxeServices=&Ds,.BootServicesAlive=Alive,
    .Ranges={{0x81d00000,8192,EfiGcdMemoryTypeReserved,EFI_MEMORY_UC,0x44},{0x1fd4000,8,EfiGcdMemoryTypeMemoryMappedIo,EFI_MEMORY_UC,0}},.RangeCount=2,.MaxReads=20,.MaxUsecs=100000};
}
static VOID Run(UINTN Number){
  Case=Number;PIANO_GUARDED_CONFIG C=Config();VOID *Token=NULL;UINT32 Out[4]={0x55555555,0x55555555,0x55555555,0x55555555};
  if(Case==38){
    EFI_BOOT_SERVICES OriginalBs=Bs;EFI_DXE_SERVICES OriginalDs=Ds;
    assert(PianoGuardedReadBegin(&C,(VOID **)&Bs.RaiseTPL)==EFI_INVALID_PARAMETER);
    assert(PianoGuardedReadBegin(&C,(VOID **)&Ds.GetMemorySpaceDescriptor)==EFI_INVALID_PARAMETER);
    assert(!memcmp(&Bs,&OriginalBs,sizeof(Bs))&&!memcmp(&Ds,&OriginalDs,sizeof(Ds))&&!Calls&&!Loads);return;
  }
  if(Case==39){C.Services=(VOID *)1;C.DxeServices=(VOID *)1;Services=FALSE;Token=(VOID *)123;
    assert(PianoGuardedReadBegin(&C,&Token)==EFI_NOT_READY&&Token==(VOID *)123&&!Calls);return;
  }
  if(Case==1)Handlers[0]=Foreign;if(Case==2)Handlers[3]=Foreign;
  if(Case==33)C.Ranges[1]=C.Ranges[0];if(Case==34)Tpl=TPL_CALLBACK;if(Case==17)C.MaxReads=2;
  EFI_STATUS S=PianoGuardedReadBegin(&C,&Token);
  if((Case>=1&&Case<=4)||(Case>=8&&Case<=16)||Case==31||Case==33||Case==34){
    assert(S!=EFI_SUCCESS&&!Token&&!Loads&&!m.Report.MemoryOwnershipGranted);
    if(Case==1)assert(Handlers[0]==Foreign&&!Unregisters&&EventCloses==1);
    else if(Case==2)assert(!Handlers[0]&&Handlers[3]==Foreign&&Unregisters==1&&EventCloses==1);
    else if(Case==3||Case==4||Case==8||Case==31){assert(m.Report.Retained);UINTN Before=Calls;assert(PianoGuardedReadBegin(&C,&Token)==EFI_ACCESS_DENIED&&Calls==Before);}
    else if(Case==9||Case==10||Case==33||Case==34)assert(!Handlers[0]&&!Handlers[3]&&!EventCloses);
    else assert(!Handlers[0]&&!Handlers[3]&&EventCloses==1);
    if(Case>=11&&Case<=15){assert(m.Report.LastMappingPage==0x81d00000&&m.Report.MappingStatus==EFI_NOT_READY&&m.Report.LastGcdType==EfiGcdMemoryTypeReserved);
      assert(m.Report.LastGcdAttributes==(Case==14?EFI_MEMORY_WB:EFI_MEMORY_UC));
      if(Case<=13)assert(m.Report.LastPar!=MAX_UINT64);else assert(m.Report.LastPar==MAX_UINT64);
    }
    return;
  }
  assert(S==EFI_SUCCESS&&Token&&m.Report.PagesValidated==3);
  if(Case==18)Counter+=200000;if(Case==19)CorruptCpu=TRUE;if(Case==32)ReplaceCpu=TRUE;
  if((Case>=22&&Case<=29)||Case==40){ExpectFatal=TRUE;if(!setjmp(FatalJump)){
      if(Case==29)Inject(3,0x9999,0,0,5);else PianoGuardedTryRead(Token,0x81d00000,16,Out);assert(FALSE);
    }assert(m.Report.Fatal&&m.Report.Retained&&!m.Report.MemoryOwnershipGranted&&Out[0]==0x55555555);return;
  }
  if(Case==37){
    assert(PianoGuardedTryRead(Token,0x81d00001,4,Out)==EFI_INVALID_PARAMETER);
    assert(PianoGuardedTryRead(Token,0x81d00000,3,Out)==EFI_INVALID_PARAMETER);
    assert(PianoGuardedRead32(Token,0x81d02000,Out)==EFI_ACCESS_DENIED);
    assert(PianoGuardedRead32(Token,0x81d00000,(UINT32 *)&m.Report.Reads)==EFI_INVALID_PARAMETER);
    assert(PianoGuardedRead32(Token,0x81d00000,(UINT32 *)0x81d00000)==EFI_INVALID_PARAMETER);
    assert(PianoGuardedTryRead(Token,0x81d00000,16,(VOID *)(MAX_UINTN-4))==EFI_INVALID_PARAMETER);assert(!Loads);
  }else{
    S=PianoGuardedTryRead(Token,0x81d00000,16,Out);
    if(Case==17||Case==18||Case==19||Case==20||Case==21||Case==30||Case==32||Case==36){assert(S!=EFI_SUCCESS&&Out[0]==0x55555555);
      if(Case==21)assert(m.Report.RecoveredFaults==1&&m.Report.Reads==2&&m.Report.Resume==0x1004);
      if(Case==17)assert(Loads==2);if(Case==18||Case==19||Case==32)assert(!Loads);
    }else assert(S==EFI_SUCCESS&&Loads==4&&Out[0]==(0x81d00000U^0x12345678));
  }
  if(Case==30||Case==32){UINTN Before=Calls;assert(m.Report.Retained&&PianoGuardedReadEnd(Token)==EFI_ACCESS_DENIED&&Calls==Before);return;}
  S=PianoGuardedReadEnd(Token);
  if(Case==5||Case==6||Case==7||Case==19||Case==20){assert(S!=EFI_SUCCESS&&m.Report.Retained);UINTN Before=Calls;assert(PianoGuardedReadEnd(Token)==EFI_ACCESS_DENIED&&Calls==Before);return;}
  assert(S==EFI_SUCCESS&&!Handlers[0]&&!Handlers[3]&&EventCloses==1&&!m.Report.Active&&!m.Report.MemoryOwnershipGranted);
  if(Case==0){VOID *New=NULL;assert(PianoGuardedReadBegin(&C,&New)==EFI_SUCCESS&&New!=Token);assert(PianoGuardedRead32(Token,0x81d00000,Out)==EFI_ACCESS_DENIED);assert(PianoGuardedReadEnd(New)==EFI_SUCCESS);}
}
int main(VOID){for(UINTN I=0;I<41;++I){pid_t P=fork();assert(P>=0);if(!P){Run(I);_exit(0);}int Status;waitpid(P,&Status,0);if(!WIFEXITED(Status)||WEXITSTATUS(Status)){fprintf(stderr,"guarded-read case %lu failed\n",(unsigned long)I);return 1;}}puts("Actual GuardedRead:41 range/GCD/AT/CPU/handler/fault/SError/EBS/budget/alias/retained cases; no hardware");return 0;}
