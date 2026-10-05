// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual probe source/EFI signatures; host-only architecture/load substitutes.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#undef NULL
#define PIANO_POGO_PROBE_HOST_TEST 1
#include "../bootprofiles/uefi-app/PianoPogoProbe.c"
EFI_BOOT_SERVICES *gBS;EFI_DXE_SERVICES *gDS;
EFI_GUID gEfiCpuArchProtocolGuid={.Data1=1},gEfiMemoryAttributeProtocolGuid={.Data1=2};
static EFI_BOOT_SERVICES bs;static EFI_DXE_SERVICES ds;static EFI_CPU_ARCH_PROTOCOL cpu;
static EFI_CLOCK_PROTOCOL clock;static EFI_MEMORY_ATTRIBUTE_PROTOCOL memory;
static EFI_CPU_INTERRUPT_HANDLER handlers[4];
static UINT32 scenario,loads,locates,attributes_calls,unregisters,state_calls;
static UINT64 counter;static BOOLEAN tpl_held;static jmp_buf stop;
static UINT32 records;static CHAR8 logs[256][640];
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return TRUE;}
UINTN EFIAPI AsciiSPrint(CHAR8 *Out,UINTN Bytes,CONST CHAR8 *Format,...){
  CHAR8 F[1024];UINTN J=0;
  for(UINTN I=0;Format[I];++I){assert(J+2<sizeof(F));if(Format[I]=='%' && Format[I+1]=='a'){F[J++]='%';F[J++]='s';++I;}else F[J++]=Format[I];}
  F[J]=0;va_list A;va_start(A,Format);int N=vsnprintf(Out,Bytes,F,A);va_end(A);assert(N>=0 && (UINTN)N<Bytes);return (UINTN)N;
}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){
  assert(records<ARRAY_SIZE(logs));va_list A;va_start(A,Format);CONST CHAR8 *Body=va_arg(A,CONST CHAR8 *);UINT32 Hash=va_arg(A,UINT32);va_end(A);
  snprintf(logs[records++],sizeof(logs[0]),"%s %s crc32=%08X",strstr(Format,"_COPY ")?"COPY":"MAIN",Body,Hash);
}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID EFIAPI CpuDeadLoop(VOID){longjmp(stop,1);}
UINT64 EFIAPI GetPerformanceCounter(VOID){counter+=scenario==17?20:1;return counter;}
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *Start,UINT64 *End){*Start=0;*End=MAX_UINT64;return 10000;}
EFI_STATUS PianoPogoHostCpuState(VOID *Out){
  CPU_STATE *C=Out;*C=(CPU_STATE){4,1,0x480803514ULL,0xc0000000,0xff44};++state_calls;
  if(scenario==18 && state_calls>1)C->Mair^=1;
  return EFI_SUCCESS;
}
EFI_STATUS PianoPogoHostAt(UINTN Address,UINT64 *Par){
  assert(!(Address&4095) && ((Address>=0xa98000 && Address<0xa9c000) || (Address>=0xac0000 && Address<0xac2000)));
  *Par=Address;if(scenario==7)*Par=1;if(scenario==8)*Par+=4096;if(scenario==9)*Par|=0xffULL<<56;return EFI_SUCCESS;
}
UINT32 PianoPogoHostLoad(UINTN Address){
  assert(tpl_held && handlers[0] && handlers[3]);++loads;
  assert((Address>=0xa98000 && Address<0xa99000) || Address==0xac0004 || Address==0xac0118 || Address==0xac0120 || Address==0xac021c);
  assert(Address!=0xa98780 && Address!=0xa98700);
  if((scenario==13 && Address==0xa98068) || scenario==14 || scenario==15 || scenario==23){
    EFI_SYSTEM_CONTEXT_AARCH64 C={.ELR=mFaultPc,.ESR=0x96000010,.FAR=Address,.SPSR=5};
    if(scenario==15)C.ELR+=8;
    if(scenario==23)C.ESR&=~BIT25;
    EFI_SYSTEM_CONTEXT Context={.SystemContextAArch64=&C};
    handlers[scenario==14?3:0](scenario==14?3:0,Context);
    assert(C.ELR==mResumePc && mFaulted);return 0xdeadbeef;
  }
  return (UINT32)Address^0x5a5a0000U;
}
static EFI_TPL EFIAPI raise_tpl(EFI_TPL Tpl){assert(Tpl==TPL_CALLBACK && !tpl_held);tpl_held=TRUE;return TPL_APPLICATION;}
static VOID EFIAPI restore_tpl(EFI_TPL Tpl){assert(Tpl==TPL_APPLICATION && tpl_held);tpl_held=FALSE;}
static VOID EFIAPI foreign(EFI_EXCEPTION_TYPE Type,EFI_SYSTEM_CONTEXT Context){(void)Type;(void)Context;assert(!"foreign handler invoked");}
static EFI_STATUS EFIAPI register_handler(EFI_CPU_ARCH_PROTOCOL *This,EFI_EXCEPTION_TYPE Type,EFI_CPU_INTERRUPT_HANDLER Handler){
  assert(This==&cpu && (Type==0 || Type==3));
  if(Handler){
    if(handlers[Type])return EFI_ALREADY_STARTED;
    if((scenario==12 || scenario==19) && Type==3){handlers[3]=foreign;return EFI_ALREADY_STARTED;}
    handlers[Type]=Handler;return EFI_SUCCESS;
  }
  ++unregisters;if(scenario==16 || scenario==19)return EFI_DEVICE_ERROR;
  assert(handlers[Type]);handlers[Type]=NULL;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI attrs(EFI_MEMORY_ATTRIBUTE_PROTOCOL *This,EFI_PHYSICAL_ADDRESS Base,UINT64 Bytes,UINT64 *Value){
  ++attributes_calls;assert(!"indirect unsafe PTE walk must not be called");return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI locate(EFI_GUID *Guid,VOID *Registration,VOID **Out){
  ++locates;EFI_GUID ClockGuid=EFI_CLOCK_PROTOCOL_GUID;
  if(Guid->Data1==gEfiCpuArchProtocolGuid.Data1)*Out=&cpu;
  else if(Guid->Data1==gEfiMemoryAttributeProtocolGuid.Data1){if(scenario==0)return EFI_NOT_FOUND;*Out=&memory;}
  else {assert(!memcmp(Guid,&ClockGuid,sizeof(*Guid)));*Out=&clock;}
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI gcd(EFI_PHYSICAL_ADDRESS Address,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
  *D=(EFI_GCD_MEMORY_SPACE_DESCRIPTOR){.BaseAddress=0x800000,.Length=0x300000,.GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo,.Attributes=EFI_MEMORY_UC};
  if(scenario==5)D->GcdMemoryType=EfiGcdMemoryTypeSystemMemory;
  if(scenario==6)D->Attributes=EFI_MEMORY_WB;
  if(scenario==21 && Address==0xac0000){D->BaseAddress=Address;D->Length=4;}
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI clock_id(EFI_CLOCK_PROTOCOL *This,CONST CHAR8 *Name,UINTN *Id){
  assert(This==&clock && strstr(Name,"gcc_qupv3_wrap"));*Id=1;return scenario==4?EFI_WARN_UNKNOWN_GLYPH:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI enabled(EFI_CLOCK_PROTOCOL *This,UINTN Id,BOOLEAN *Value){assert(This==&clock && Id==1);*Value=scenario!=1;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI on(EFI_CLOCK_PROTOCOL *This,UINTN Id,BOOLEAN *Value){assert(This==&clock && Id==1);*Value=scenario!=2;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI freq(EFI_CLOCK_PROTOCOL *This,UINTN Id,UINT32 *Value){assert(This==&clock && Id==1);*Value=scenario==3?0:19200000;return EFI_SUCCESS;}
INT32 EFIAPI FdtPathOffset(CONST VOID *Fdt,CONST CHAR8 *Path){assert(Fdt==(VOID *)123);return strstr(Path,"/i2c@")?1:2;}
CONST VOID *EFIAPI FdtGetProp(CONST VOID *Fdt,INT32 Node,CONST CHAR8 *Name,INT32 *Length){
  static UINT8 P[8];assert(Fdt==(VOID *)123 && !strcmp(Name,"reg"));UINT32 Base=Node==1?0xa98000:0xac0000,Bytes=Node==1?0x4000:0x2000;
  if(scenario==10)Base++;
  for(UINT32 I=0;I<4;++I){P[I]=(UINT8)(Base>>(24-8*I));P[I+4]=(UINT8)(Bytes>>(24-8*I));}*Length=8;return P;
}
static UINT32 independent_crc(CONST CHAR8 *P,UINTN N){
  UINT32 C=0xffffffff;for(UINTN I=0;I<N;++I){C^=(UINT8)P[I];for(UINTN J=0;J<8;++J)C=(C>>1)^((C&1)?0xedb88320:0);}return ~C;
}
static VOID check_logs(VOID){
  assert(records && !(records&1));
  for(UINTN I=0;I<records;I+=2){
    assert(!strncmp(logs[I],"MAIN ",5) && !strncmp(logs[I+1],"COPY ",5));assert(!strcmp(logs[I]+5,logs[I+1]+5));
    CONST CHAR8 *Body=logs[I]+5,*Hash=strstr(Body," crc32=");unsigned C=0;assert(Hash && sscanf(Hash+7,"%x",&C)==1);
    assert(C==independent_crc(Body,(UINTN)(Hash-Body)));
  }
}
int main(int argc,char **argv){
  assert(argc==2);scenario=(UINT32)strtoul(argv[1],NULL,10);assert(scenario<=23);
  gBS=&bs;gDS=&ds;bs.LocateProtocol=locate;bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore_tpl;ds.GetMemorySpaceDescriptor=gcd;
  cpu.RegisterInterruptHandler=register_handler;memory.GetMemoryAttributes=attrs;
  clock.Version=scenario==20?0:0x1000b;clock.GetClockID=clock_id;clock.IsClockEnabled=enabled;clock.IsClockOn=on;clock.GetClockFreqHz=freq;
  if(scenario==11)handlers[0]=foreign;
  BOOLEAN fatal=FALSE;EFI_STATUS S=EFI_NOT_READY;
  if(!setjmp(stop))S=PianoProbePogo((VOID *)123);else fatal=TRUE;
  assert(!attributes_calls && !mReport.MemoryAttributeCalled);check_logs();
  if(!PIANO_POGO_PROBE_EXPERIMENT){assert(S==EFI_UNSUPPORTED && !locates && !loads);return 0;}
  if(scenario==14 || scenario==15 || scenario==16 || scenario==19 || scenario==23){
    assert(fatal && mReport.Fatal && mReport.HandlersRetained && tpl_held);
  } else {
    assert(!fatal && !tpl_held);
    if(scenario==0 || scenario==22){assert(S==EFI_SUCCESS && loads==34 && mReport.Complete && mReport.MemoryAttributePresent==(scenario==22));}
    else {assert(S==EFI_NOT_READY && !mReport.Complete);}
    if(scenario==13){assert(loads==5 && mReport.RecoveredFaults==1 && mReport.Esr==0x96000010 && mReport.Se6.Firmware==0);}
    else if(scenario==17){assert(loads<34 && loads>0);}
    else if(scenario!=0 && scenario!=22)assert(!loads);
    if(scenario==11)assert(handlers[0]==foreign && !unregisters);
    else if(scenario==12)assert(!handlers[0] && handlers[3]==foreign && unregisters==1);
    else assert(!handlers[0] && !handlers[3]);
    assert(PianoProbePogo((VOID *)123)==EFI_ALREADY_STARTED);
  }
  UINT32 BeforeLoads=loads;PianoPogoProbeReemit();assert(loads==BeforeLoads);check_logs();
  printf("Pogo actual probe scenario %u passed\n",scenario);return 0;
}
