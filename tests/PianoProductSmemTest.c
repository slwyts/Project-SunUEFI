// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual ProductSmem -> GuardedRead -> SmemRam; only EFI/architecture/load
// boundaries are fixtures. No SFS, target write, PTE change or memory grant.
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoProductSmem.h"
#include "../bootprofiles/guarded-read/PianoGuardedRead.h"
#include "../bootprofiles/early-memory/PianoSmemRam.h"
#include "../bootprofiles/early-memory/PianoEarlyMemory.h"
#include <Library/HobLib.h>
#include <Protocol/Cpu.h>
#include <Guid/EventGroup.h>
#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
EFI_BOOT_SERVICES *gBS;EFI_SYSTEM_TABLE *gST;EFI_DXE_SERVICES *gDS;
EFI_GUID gEfiCpuArchProtocolGuid={.Data1=1},gEfiEventExitBootServicesGuid={.Data1=2};
static EFI_BOOT_SERVICES bs,other_bs;static EFI_SYSTEM_TABLE st;static EFI_DXE_SERVICES ds;static EFI_CPU_ARCH_PROTOCOL cpu;
static EFI_CPU_INTERRUPT_HANDLER handlers[4];static EFI_EVENT_NOTIFY fence;static VOID*fence_context;static EFI_TPL tpl=TPL_APPLICATION;
static UINTN scenario,calls,loads,unregisters,closes,at_calls,gcd_calls;static UINT64 counter=100;
static UINT8 smem[PIANO_SMEM_BYTES],before[PIANO_SMEM_BYTES];
static UINTN payload_log,count_log,coherence_log,close_log,bank_log;static UINT32 parsed,major,ramver,banks,preloaded;static UINT64 bank_base,bank_size,available;
static BOOLEAN map_authorized_log,close_retained,debug_lifetime_safe;
static UINTN snapshot_log,active_rejects;static EFI_STATUS snapshot_status,map_status;static UINT64 map_page,map_par,gcd_attrs;static UINT32 gcd_type,payload_crc;
static UINTN early_case,early_gets,early_logs;static EFI_STATUS early_log_status;
static EFI_GUID early_guid=PIANO_EARLY_MEMORY_HOB_GUID;
static struct {EFI_HOB_GUID_TYPE Header;PIANO_EARLY_MEMORY_REPORT Report;} early_hob;
VOID *EFIAPI GetFirstGuidHob(CONST EFI_GUID *Guid){assert(!memcmp(Guid,&early_guid,sizeof(*Guid)));++early_gets;return early_case?&early_hob:NULL;}
VOID *EFIAPI GetNextGuidHob(CONST EFI_GUID *Guid,CONST VOID *Start){assert(!memcmp(Guid,&early_guid,sizeof(*Guid)));assert(Start==(UINT8 *)&early_hob+early_hob.Header.Header.HobLength);++early_gets;return early_case==4?&early_hob:NULL;}
VOID *EFIAPI ZeroMem(VOID*p,UINTN n){return memset(p,0,n);}VOID *EFIAPI CopyMem(VOID*a,CONST VOID*b,UINTN n){return memmove(a,b,n);}INTN EFIAPI CompareMem(CONST VOID*a,CONST VOID*b,UINTN n){return memcmp(a,b,n);}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level){return TRUE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8*Format,...){VA_LIST args;VA_START(args,Format);assert(Level==DEBUG_WARN);debug_lifetime_safe=TRUE;
 if(strstr(Format,"EARLY_RAM_HOB")){++early_logs;early_log_status=VA_ARG(args,EFI_STATUS);}
 if(strstr(Format,"SMEM_PAYLOAD")||strstr(Format,"SMEM_SNAPSHOT")){if(strstr(Format,"SMEM_SNAPSHOT"))++snapshot_log;else ++payload_log;snapshot_status=VA_ARG(args,EFI_STATUS);parsed=VA_ARG(args,unsigned);major=VA_ARG(args,unsigned);ramver=VA_ARG(args,unsigned);}
 if(strstr(Format,"SMEM_COUNTS")){++count_log;banks=VA_ARG(args,unsigned);preloaded=VA_ARG(args,unsigned);}
 if(strstr(Format,"SMEM_COHERENCE")){++coherence_log;map_authorized_log=strstr(Format,"map_authorized=0")!=NULL;(void)VA_ARG(args,unsigned);(void)VA_ARG(args,unsigned);payload_crc=VA_ARG(args,unsigned);}
 if(strstr(Format,"SMEM_BANK")){++bank_log;(void)VA_ARG(args,unsigned);bank_base=VA_ARG(args,UINT64);bank_size=VA_ARG(args,UINT64);available=VA_ARG(args,UINT64);}
 if(strstr(Format,"SMEM_CLOSE")){++close_log;(void)VA_ARG(args,EFI_STATUS);(void)VA_ARG(args,unsigned);close_retained=VA_ARG(args,unsigned)!=0;}
 if(strstr(Format,"SMEM_MAP")){map_page=VA_ARG(args,UINT64);map_par=VA_ARG(args,UINT64);map_status=VA_ARG(args,EFI_STATUS);}
 if(strstr(Format,"SMEM_GCD")){gcd_type=VA_ARG(args,unsigned);gcd_attrs=VA_ARG(args,UINT64);}
 VA_END(args);}
VOID EFIAPI CpuDeadLoop(VOID){abort();}
static VOID p32(UINT8*p,UINT32 v){for(UINTN i=0;i<4;i++)p[i]=(UINT8)(v>>(8*i));}static VOID p64(UINT8*p,UINT64 v){p32(p,(UINT32)v);p32(p+4,(UINT32)(v>>32));}
static VOID BsCall(VOID){assert(gBS==&bs&&gST&&gST->BootServices==&bs);++calls;}
static EFI_TPL EFIAPI Raise(EFI_TPL n){BsCall();EFI_TPL old=tpl;tpl=n;return old;}static VOID EFIAPI Restore(EFI_TPL old){BsCall();tpl=old;}
static VOID EFIAPI Foreign(EFI_EXCEPTION_TYPE t,EFI_SYSTEM_CONTEXT c){assert(!"foreign handler must not be invoked");}
static EFI_STATUS EFIAPI Register(EFI_CPU_ARCH_PROTOCOL*this,EFI_EXCEPTION_TYPE t,EFI_CPU_INTERRUPT_HANDLER fn){BsCall();assert(this==&cpu&&(t==0||t==3));
 if(fn){if(handlers[t])return EFI_ALREADY_STARTED;handlers[t]=fn;if(scenario==3&&t==0)return EFI_WARN_STALE_DATA;}
 else{++unregisters;assert(handlers[t]&&handlers[t]!=Foreign);if(scenario==4&&t==3)return EFI_WARN_STALE_DATA;if(scenario==5&&t==0)return EFI_DEVICE_ERROR;handlers[t]=NULL;}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Locate(EFI_GUID*g,VOID*r,VOID**out){BsCall();assert(g==&gEfiCpuArchProtocolGuid);*out=&cpu;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Create(UINT32 t,EFI_TPL p,EFI_EVENT_NOTIFY fn,CONST VOID*c,CONST EFI_GUID*g,EFI_EVENT*out){BsCall();assert(t==EVT_NOTIFY_SIGNAL&&p==TPL_NOTIFY&&g==&gEfiEventExitBootServicesGuid);fence=fn;fence_context=(VOID*)c;*out=(VOID*)7;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Close(EFI_EVENT e){BsCall();assert(e==(VOID*)7);++closes;return scenario==10?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64*f,UINT64*e){*f=0;*e=MAX_UINT64;return 1000000;}UINT64 EFIAPI GetPerformanceCounter(VOID){return ++counter;}
EFI_STATUS PianoGuardedHostCpuState(VOID*p){UINT64 state[6]={4,1,0x480803514ULL,0xd7fff000,0,0xff44};memcpy(p,state,sizeof(state));return EFI_SUCCESS;}UINT64 PianoGuardedHostCurrentEl(VOID){return 4;}
static BOOLEAN SmemPage(UINTN a){return a>=PIANO_SMEM_BASE&&a<PIANO_SMEM_BASE+PIANO_SMEM_BYTES&&(a&4095)==0;}
EFI_STATUS PianoGuardedHostAt(UINTN a,UINT64*par){assert(SmemPage(a)||a==PIANO_SMEM_COOKIE_LOW);++at_calls;*par=a|((UINT64)(SmemPage(a)?0x44:0)<<56);if(scenario==1)*par|=1;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Gcd(EFI_PHYSICAL_ADDRESS a,EFI_GCD_MEMORY_SPACE_DESCRIPTOR*d){BsCall();assert(SmemPage((UINTN)a)||a==PIANO_SMEM_COOKIE_LOW);++gcd_calls;
 if(!snapshot_log){UINTN old=calls;assert(PianoProductSmemReemit(NULL)==EFI_NOT_READY&&calls==old);++active_rejects;}
 *d=(EFI_GCD_MEMORY_SPACE_DESCRIPTOR){.BaseAddress=a,.Length=4096,.GcdMemoryType=SmemPage((UINTN)a)?EfiGcdMemoryTypeReserved:EfiGcdMemoryTypeMemoryMappedIo,.Attributes=scenario==2?EFI_MEMORY_WB:EFI_MEMORY_UC};if(scenario==9)gBS=&other_bs;return EFI_SUCCESS;}
UINT32 PianoGuardedHostLoad(UINTN a){assert(a==PIANO_SMEM_COOKIE_LOW||a==PIANO_SMEM_COOKIE_HIGH||(a>=PIANO_SMEM_BASE&&a+4<=PIANO_SMEM_BASE+PIANO_SMEM_BYTES));++loads;
 if(a==PIANO_SMEM_COOKIE_LOW)return 0x12345678;if(a==PIANO_SMEM_COOKIE_HIGH)return 0x87654321;
 UINT32 out;memcpy(&out,smem+(a-PIANO_SMEM_BASE),4);return out;}
static VOID Setup(VOID){
 // Real major11 allocation table for RAM402 with version2 bank/preload rows.
 p32(smem+0x5c,0xb0001);p32(smem+0xc0,1);p32(smem+0xc4,0x100000);p32(smem+0xc8,0x100000);
 UINT8*entry=smem+0xd0+402*16;p32(entry,1);p32(entry+4,0x3000);p32(entry+8,24+72*2);
 UINT8*raw=smem+0x3000;p32(raw,0x9da5e0a8);p32(raw+4,0xaf9ec4e2);p32(raw+8,2);p32(raw+16,2);
 UINT8*e=raw+24;p64(e+16,0x80000000);p64(e+24,0x200000000ULL);p32(e+36,14);p32(e+44,1);p64(e+64,0x1ff000000ULL);
 e+=72;p64(e+16,0xa0000000);p64(e+24,0x100000);p32(e+36,14);p32(e+44,5);memcpy(before,smem,sizeof(smem));
 bs=(EFI_BOOT_SERVICES){.Hdr={.Signature=EFI_BOOT_SERVICES_SIGNATURE,.HeaderSize=sizeof(bs)},.RaiseTPL=Raise,.RestoreTPL=Restore,.LocateProtocol=Locate,.CreateEventEx=Create,.CloseEvent=Close};st.BootServices=&bs;gBS=&bs;gST=&st;ds.GetMemorySpaceDescriptor=Gcd;gDS=&ds;cpu.RegisterInterruptHandler=Register;
 if(scenario==6)handlers[0]=Foreign;if(scenario==11)handlers[3]=Foreign;if(scenario==7)st.BootServices=&other_bs;if(scenario==8)bs.Hdr.Signature=0;if(scenario==12)gST=NULL;
}
static BOOLEAN NewLive(VOID*c){return c==&bs&&gBS==&bs&&gST&&gST->BootServices==&bs;}
static UINT32 Crc(CONST UINT8*p,UINTN n){UINT32 c=MAX_UINT32;for(UINTN i=0;i<n;i++){c^=p[i];for(UINTN b=0;b<8;b++)c=c&1?(c>>1)^0xedb88320U:c>>1;}return ~c;}
static VOID EarlySetup(VOID){
 if(!early_case)return;
 early_hob.Header=(EFI_HOB_GUID_TYPE){.Header={.HobType=EFI_HOB_TYPE_GUID_EXTENSION,.HobLength=sizeof(early_hob)},.Name=early_guid};
 PIANO_EARLY_MEMORY_REPORT *r=&early_hob.Report;
 *r=(PIANO_EARLY_MEMORY_REPORT){.Version=PIANO_EARLY_MEMORY_VERSION,.Bytes=sizeof(*r),.Status=EFI_SUCCESS,.PublishStatus=EFI_SUCCESS,
   .EntryEl=4,.EntrySctlr=0,.EntrySpSel=1,.EntryVbar=0x2000,.EntryDaif=0x3c0,.LoadCount=20,
   .Attempted=TRUE,.Finished=TRUE,.Published=TRUE,.ColdStateVerified=TRUE};
 assert(PianoSmemRamParse(smem+0x3000,24+72*2,&r->Smem)==EFI_SUCCESS);
 r->Smem.SmemVersion=0xb0001;r->Smem.RepeatedMetadataEqual=r->Smem.RepeatedPayloadEqual=TRUE;
 r->Smem.PayloadAddress=PIANO_SMEM_BASE+0x3000;r->Smem.PayloadBytes=24+72*2;r->Smem.RawEntryCount=2;
 if(early_case==2)r->Version=2;
 if(early_case==3)early_hob.Header.Header.HobLength-=8;
 if(early_case==5)r->MemoryOwnershipGranted=TRUE;
 if(early_case==6)r->Smem.BankCount=65;
 if(early_case==8)r->Smem.Banks[0].AvailableLength=r->Smem.Banks[0].RawSize+1;
 if(early_case==9){r->Status=EFI_UNSUPPORTED;r->ColdStateVerified=FALSE;r->LoadCount=0;memset(&r->Smem,0,sizeof(r->Smem));r->Smem.Status=EFI_NOT_STARTED;}
 if(early_case==10||early_case==19){r->Status=EFI_DEVICE_ERROR;r->LoadCount=1;r->RecoveredFaults=1;memset(&r->Smem,0,sizeof(r->Smem));r->Smem.Status=EFI_DEVICE_ERROR;
  r->LastFault=(PIANO_SEC_READ_STATE){.Faulted=1,.Address=PIANO_SMEM_BASE+0x5c,.Far=PIANO_SMEM_BASE+0x5c,.Elr=0x1100,.Resume=0x1104,.Esr=0x96000010,.Spsr=0x3c5};
  if(early_case==19)r->LastFault.Far++;}
 if(early_case==11)early_hob.Header.Name.Data1^=1;
 if(early_case==12)r->Status=EFI_DEVICE_ERROR;
 if(early_case==13)r->Smem.RepeatedPayloadEqual=FALSE;
 if(early_case==14)r->Smem.Banks[0].SourceIndex=65;
 if(early_case==15)early_hob.Header.Header.HobType=EFI_HOB_TYPE_RESOURCE_DESCRIPTOR;
 if(early_case==16)r->Bytes=0x200000;
 if(early_case==17)r->ColdStateVerified=2;
 if(early_case==18)r->EntrySctlr=1;
 r->ReportCrc32=PianoEarlyMemoryReportCrc32(r);
 if(early_case==7)r->ReportCrc32^=1;
}
static VOID Replay(EFI_STATUS Expected){UINTN oldcalls=calls,oldloads=loads,oldregs=unregisters,oldcloses=closes,oldat=at_calls,oldgcd=gcd_calls;assert(PianoProductSmemReemit(NULL)==Expected);assert(calls==oldcalls&&loads==oldloads&&unregisters==oldregs&&closes==oldcloses&&at_calls==oldat&&gcd_calls==oldgcd);}
int main(int argc,char**argv){assert(argc==2||argc==3);scenario=strtoul(argv[1],NULL,10);assert(scenario<13);early_case=argc==3?strtoul(argv[2],NULL,10):0;assert(early_case<20);Setup();EarlySetup();assert(PianoProductSmemReemit(NULL)==EFI_NOT_READY&&PianoProductSmemReemit((VOID*)1)==EFI_INVALID_PARAMETER&&!calls&&!loads&&!snapshot_log);EFI_STATUS result=PianoProductObserveSmem();CONST PIANO_GUARDED_REPORT*r=PianoGuardedReadReport();assert(!r->MemoryOwnershipGranted&&!memcmp(smem,before,sizeof(smem))&&debug_lifetime_safe);
 EFI_STATUS expected_early=early_case==0?EFI_NOT_FOUND:early_case==1||early_case==9||early_case==10?EFI_SUCCESS:EFI_COMPROMISED_DATA;
 assert(PianoProductEarlySmemStatus()==expected_early&&early_log_status==expected_early&&early_logs==1);
 UINTN original_early_gets=early_gets;memset(&early_hob,0,sizeof(early_hob));
 if(scenario==0){assert(result==EFI_SUCCESS&&!PianoProductSmemRetained()&&r->PagesValidated==513&&r->Reads==loads&&loads>0&&parsed==1&&major==11&&ramver==2&&banks==1&&preloaded==1&&bank_log==1&&bank_base==0x80000000&&bank_size==0x200000000ULL&&available==0x1ff000000ULL&&coherence_log==1&&map_authorized_log&&unregisters==2&&closes==1&&!handlers[0]&&!handlers[3]);}
 else if(scenario==1||scenario==2){assert(result==EFI_NOT_READY&&!PianoProductSmemRetained()&&!loads&&unregisters==2&&closes==1&&!handlers[0]&&!handlers[3]&&!payload_log);}
 else if(scenario==3){assert(result!=EFI_SUCCESS&&PianoProductSmemRetained()&&!loads&&!unregisters&&!closes&&handlers[0]);}
 else if(scenario==4||scenario==5||scenario==10){assert(result!=EFI_SUCCESS&&PianoProductSmemRetained()&&loads>0&&close_retained&&close_log==1);}
 else if(scenario==6){assert(result==EFI_ALREADY_STARTED&&!PianoProductSmemRetained()&&!loads&&!unregisters&&closes==1&&handlers[0]==Foreign&&!handlers[3]);}
 else if(scenario==11){assert(result==EFI_ALREADY_STARTED&&!PianoProductSmemRetained()&&!loads&&unregisters==1&&closes==1&&!handlers[0]&&handlers[3]==Foreign);}
 else if(scenario==7||scenario==8||scenario==12){assert(result==EFI_NOT_READY&&!PianoProductSmemRetained()&&!loads&&!calls&&!unregisters&&!closes&&!handlers[0]&&!handlers[3]);}
 else if(scenario==9){assert(result!=EFI_SUCCESS&&PianoProductSmemRetained()&&!loads&&!unregisters&&!closes&&handlers[0]&&handlers[3]);}
 if(PianoProductSmemRetained()){UINTN oldlog=snapshot_log;Replay(EFI_ABORTED);assert(snapshot_log==oldlog);}
 else{
   UINTN oldlog=snapshot_log;Replay(EFI_SUCCESS);assert(snapshot_log==oldlog+1&&snapshot_status==result);
   if(scenario==0){assert(parsed==1&&major==11&&ramver==2&&bank_base==0x80000000&&bank_size==0x200000000ULL&&available==0x1ff000000ULL&&payload_crc==Crc(smem+0x3000,24+72*2)&&active_rejects>0);UINT64 savedpage=map_page,savedpar=map_par,savedattrs=gcd_attrs;UINT32 savedtype=gcd_type,savedcrc=payload_crc;EFI_STATUS savedstatus=map_status;
     // A real subsequent guard session overwrites the singleton report.
     PIANO_GUARDED_CONFIG cfg={.Context=&bs,.Services=&bs,.DxeServices=&ds,.BootServicesAlive=NewLive,.Ranges={{PIANO_SMEM_BASE,4096,EfiGcdMemoryTypeReserved,EFI_MEMORY_UC,0x44}},.RangeCount=1,.MaxReads=4,.MaxUsecs=100000};VOID*token=NULL;assert(PianoGuardedReadBegin(&cfg,&token)==EFI_SUCCESS);UINT32 version=0;assert(PianoGuardedRead32(token,PIANO_SMEM_BASE+0x5c,&version)==EFI_SUCCESS&&version==0xb0001);assert(PianoGuardedReadEnd(token)==EFI_SUCCESS&&PianoGuardedReadReport()->LastMappingPage==PIANO_SMEM_BASE);
     assert(savedpage!=PianoGuardedReadReport()->LastMappingPage);Replay(EFI_SUCCESS);assert(map_page==savedpage&&map_par==savedpar&&gcd_attrs==savedattrs&&gcd_type==savedtype&&map_status==savedstatus&&payload_crc==savedcrc&&parsed==1&&banks==1&&preloaded==1);Replay(EFI_SUCCESS);assert(map_page==savedpage&&payload_crc==savedcrc);
   }else if(scenario==6||scenario==11)assert(parsed==0&&banks==0&&preloaded==0&&snapshot_status==EFI_ALREADY_STARTED);
 }
 UINTN logbefore=snapshot_log,callbefore=calls;assert(PianoProductSmemReemit((VOID*)1)==EFI_INVALID_PARAMETER&&snapshot_log==logbefore&&calls==callbefore);
 UINTN oldcalls=calls,oldloads=loads;assert(PianoProductObserveSmem()==EFI_ALREADY_STARTED&&calls==oldcalls&&loads==oldloads);
 assert(early_gets==original_early_gets&&PianoProductEarlySmemStatus()==expected_early&&early_log_status==expected_early);
 printf("Actual ProductSmem+GuardedRead+SmemRam case%lu early%lu passed; immutable HOB capture; zero writes/map authority\n",(unsigned long)scenario,(unsigned long)early_case);return 0;}
