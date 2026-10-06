// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real pinned PE/relocations/ref decoder. Native ARM boundary and guarded CPU
// reads are fixtures; no protocol execution, MMIO or device access.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>
#include <openssl/sha.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoDisplayClockLease.h"
#include <PiDxe.h>
EFI_GUID gEfiLoadedImageProtocolGuid={.Data1=1},gEfiEventExitBootServicesGuid={.Data1=2};
VOID *EFIAPI CopyMem(VOID *d,CONST VOID *s,UINTN n){return memmove(d,s,n);}VOID *EFIAPI ZeroMem(VOID *d,UINTN n){return memset(d,0,n);}INTN EFIAPI CompareMem(CONST VOID *a,CONST VOID *b,UINTN n){return memcmp(a,b,n);}BOOLEAN EFIAPI Sha256HashAll(CONST VOID *p,UINTN n,UINT8 *h){return SHA256(p,n,h)!=NULL;}
static UINT8 *File,*Memory;static UINTN Base;static EFI_CLOCK_PROTOCOL *Clock;static EFI_BOOT_SERVICES Bs;static EFI_LOADED_IMAGE_PROTOCOL Loaded;static EFI_TPL Tpl=TPL_APPLICATION;
static unsigned Case,GetCalls,EnableCalls,EnabledCalls,OnCalls,DisableCalls,GuardCalls,ReadCalls,FreeCalls,EvidenceCalls;static BOOLEAN Alive=TRUE,EventLive;static EFI_EVENT_NOTIFY ExitFn;static VOID *ExitContext;
static UINT8 Client[32],Ref[24]; // actual BSP/module/parent remain pinned image data
static UINT32 Ahb,Hf=0x08200001;
static PIANO_DISPLAY_CLOCK_LEASE State;static PIANO_DISPLAY_CLOCK_LEASE_ENV Env;
static VOID Put64(VOID *p,UINT64 v){memcpy(p,&v,8);}static VOID Put32(VOID *p,UINT32 v){memcpy(p,&v,4);}static VOID Put16(VOID *p,UINT16 v){memcpy(p,&v,2);}static UINT16 Get16(CONST VOID *p){UINT16 v;memcpy(&v,p,2);return v;}
static BOOLEAN Live(VOID *p){assert(p==(VOID*)1);return Alive;}
static EFI_TPL EFIAPI Raise(EFI_TPL n){assert(Alive);EFI_TPL t=Tpl;Tpl=n;if(Case==37)Alive=FALSE;return t;}static VOID EFIAPI Restore(EFI_TPL n){assert(Alive);Tpl=n;}
static EFI_STATUS EFIAPI Locate(EFI_GUID *g,VOID *registration,VOID **out){(VOID)g;(VOID)registration;assert(Alive);*out=Clock;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handles(EFI_LOCATE_SEARCH_TYPE t,EFI_GUID *g,VOID *key,UINTN *n,EFI_HANDLE **out){(VOID)t;(VOID)g;(VOID)key;assert(Alive);*n=1;*out=malloc(sizeof(**out));(*out)[0]=(VOID*)7;return Case==35?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID *g,VOID **out){assert(Alive&&h==(VOID*)7&&g==&gEfiLoadedImageProtocolGuid);*out=&Loaded;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Free(VOID *p){assert(Alive);++FreeCalls;if((Case==31&&EnableCalls)||(Case==65&&State.ReadCpuCalls))return EFI_WARN_STALE_DATA;if(Case==69&&State.ReadCpuCalls)return EFI_DEVICE_ERROR;free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Create(UINT32 type,EFI_TPL t,EFI_EVENT_NOTIFY fn,CONST VOID *c,CONST EFI_GUID *g,EFI_EVENT *out){assert(Alive&&type==EVT_NOTIFY_SIGNAL&&t==TPL_NOTIFY&&g==&gEfiEventExitBootServicesGuid);ExitFn=fn;ExitContext=(VOID*)c;EventLive=TRUE;*out=(VOID*)9;return Case==33?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Close(EFI_EVENT e){assert(Alive&&e==(VOID*)9&&EventLive);if(Case==19)return EFI_DEVICE_ERROR;EventLive=FALSE;return EFI_SUCCESS;}
EFI_STATUS EFIAPI GetSectionFromAnyFv(CONST EFI_GUID *g,UINT8 type,UINTN i,VOID **out,UINTN *bytes){(VOID)g;assert(type==EFI_SECTION_PE32&&!i&&Alive);*bytes=0x44000;*out=malloc(*bytes);memcpy(*out,File,*bytes);if(Case==2)((UINT8*)*out)[0x1010]^=1;return Case==34?EFI_WARN_STALE_DATA:EFI_SUCCESS;}
static BOOLEAN Within(UINT64 p,UINTN n,CONST VOID *base,UINTN bytes){UINT64 b=(UINT64)(UINTN)base;return n&&p>=b&&p-b<=bytes&&n<=bytes-(p-b);}
static EFI_STATUS Read(VOID *context,UINT64 p,UINTN n,VOID *out){assert(context==(VOID*)1&&Alive&&n<=256);++ReadCalls;
 if(Case>=46&&Case<=69&&((Case==68&&ReadCalls==2)||(Case!=68&&ReadCalls==1)))return EFI_NOT_READY;
 if(Case==25&&ReadCalls==2)return EFI_DEVICE_ERROR;
 assert(Within(p,n,Memory,0x44000)||Within(p,n,Client,sizeof(Client))||Within(p,n,Ref,sizeof(Ref)));
 memcpy(out,(VOID*)(UINTN)p,n);if(Case==38&&p==Base+0x33a68+0x50)Put16(Memory+0x33a68+0x50,Get16(Memory+0x33a68+0x50)+1);return EFI_SUCCESS;
}
static EFI_STATUS Evidence(VOID *context,UINT64 address,UINTN bytes,EFI_STATUS status,PIANO_DISPLAY_CLOCK_LEASE_READ_FAILURE_EVIDENCE *r){
 assert(context==(VOID*)1&&Alive&&ReadCalls==1&&address==Base+0x1000&&bytes==256&&status==EFI_NOT_READY);++EvidenceCalls;
 *r=(PIANO_DISPLAY_CLOCK_LEASE_READ_FAILURE_EVIDENCE){.Revision=1,.Sequence=1,.Address=address,.Bytes=bytes,.Status=status,.MapStatus=EFI_NOT_READY,.EndStatus=EFI_NOT_STARTED};
 switch(Case){case 48:r->Sequence=2;break;case 49:r->Bytes--;break;case 50:r->Address+=256;break;case 51:r->Status=EFI_DEVICE_ERROR;break;
 case 52:r->Busy=TRUE;break;case 53:r->Retained=TRUE;break;case 54:r->ServicesLost=TRUE;break;case 55:r->Sessions=1;break;
 case 56:r->GuardActive=TRUE;break;case 57:r->GuardSyncOwned=TRUE;break;case 58:r->GuardSErrorOwned=TRUE;break;case 59:r->GuardFatal=TRUE;break;
 case 60:r->GuardRetained=TRUE;break;case 61:r->GuardServicesLost=TRUE;break;case 62:r->GuardReads=1;break;case 63:r->EndStatus=EFI_WARN_STALE_DATA;break;
 case 66:Alive=FALSE;break;case 67:r->MapStatus=EFI_SUCCESS;break;default:break;}
 return Case==64?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
static EFI_STATUS Gcc(VOID *context,PIANO_DISPLAY_CLOCK_LEASE_GCC *r){assert(context==(VOID*)1&&Alive&&Tpl==TPL_APPLICATION);++GuardCalls;*r=(PIANO_DISPLAY_CLOCK_LEASE_GCC){EFI_SUCCESS,EFI_SUCCESS,4,1,FALSE,FALSE,{Ahb,Ahb},{Hf,Hf}};
 if(Case==17&&EnableCalls)return EFI_DEVICE_ERROR;if(Case==18&&EnableCalls){r->HfAxi[0]^=1;r->HfAxi[1]=r->HfAxi[0];}if(Case==32){r->EndStatus=EFI_DEVICE_ERROR;r->Retained=TRUE;}return EFI_SUCCESS;
}
EFI_STATUS PianoDisplayClockHostGet(EFI_CLOCK_PROTOCOL *p,CONST CHAR8 *name,UINTN *id){assert(p==Clock&&!strcmp(name,"gcc_disp_ahb_clk")&&State.Report.Identity==EFI_SUCCESS&&ReadCalls>600);++GetCalls;
 if(Case==36){Put64(Memory+0x33a68+0x58,(UINT64)(UINTN)Ref);Put64(Ref+8,(UINT64)(UINTN)Client);}
 if(Case==9)return EFI_SUCCESS;*id=Case==10?0x30000:0x04010033;return EFI_SUCCESS;
}
EFI_STATUS PianoDisplayClockHostEnable(EFI_CLOCK_PROTOCOL *p,UINTN id){assert(p==Clock&&id==0x04010033&&State.Report.Identity==EFI_SUCCESS&&GetCalls==1);++EnableCalls;
 if(Case==27)assert(PianoDisplayClockLeaseAcquire(&State,&Env)==EFI_INVALID_PARAMETER&&EnableCalls==1);
 if(Case!=13){Put16(Memory+0x33a68+0x50,Get16(Memory+0x33a68+0x50)+1);if(Case!=14)Put16(Ref+0x10,Get16(Ref+0x10)+1);}Ahb=(Case==15||Case==43)?0x88000003:0x08000003;
 if(Case==22){ExitFn((VOID*)9,ExitContext);Alive=FALSE;return EFI_SUCCESS;}return Case==11?EFI_WARN_STALE_DATA:Case==12?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
EFI_STATUS PianoDisplayClockHostIsEnabled(EFI_CLOCK_PROTOCOL *p,UINTN id,BOOLEAN *enabled){assert(p==Clock&&id==0x04010033&&EnableCalls==1&&*enabled==0xA5);++EnabledCalls;
 if(Case==41)return EFI_SUCCESS;*enabled=Case==39?FALSE:Case==44?2:(Ahb&1)?TRUE:FALSE;
 if(Case==42){ExitFn((VOID*)9,ExitContext);Alive=FALSE;}return Case==40?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
EFI_STATUS PianoDisplayClockHostIsOn(EFI_CLOCK_PROTOCOL *p,UINTN id,BOOLEAN *on){assert(p==Clock&&id==0x04010033&&EnableCalls==1&&EnabledCalls==1&&*on==0xA5);++OnCalls;
 if(Case==45)return EFI_SUCCESS;UINT32 nibble=Ahb>>28;*on=nibble==0||nibble==2;return Case==16?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
EFI_STATUS PianoDisplayClockHostDisable(EFI_CLOCK_PROTOCOL *p,UINTN id){assert(p==Clock&&id==0x04010033&&EnableCalls==1&&!DisableCalls);++DisableCalls;
 if(Case!=21){Put16(Memory+0x33a68+0x50,Get16(Memory+0x33a68+0x50)-1);Put16(Ref+0x10,Get16(Ref+0x10)-1);}if(!Get16(Ref+0x10))Ahb=0x88000002;return Case==20?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
static VOID Relocate(VOID){for(UINTN a=0x41000;a<0x44000;){UINT32 page,n;memcpy(&page,File+a,4);memcpy(&n,File+a+4,4);if(!n)break;assert(n>=8&&n<=0x44000-a);
 for(UINTN o=a+8;o<a+n;o+=2){UINT16 e;memcpy(&e,File+o,2);if(e>>12==10){UINT64 v;UINTN at=page+(e&4095);assert(at<=0x44000-8);memcpy(&v,Memory+at,8);v+=Base;memcpy(Memory+at,&v,8);}}a+=n;}}
static VOID Run(unsigned c){Case=c;assert(posix_memalign((VOID**)&Memory,4096,0x44000)==0);Base=(UINTN)Memory;memcpy(Memory,File,0x44000);Relocate();Clock=(EFI_CLOCK_PROTOCOL*)(Memory+0x28148);
 Bs=(EFI_BOOT_SERVICES){.LocateProtocol=Locate,.LocateHandleBuffer=Handles,.HandleProtocol=Handle,.FreePool=Free,.RaiseTPL=Raise,.RestoreTPL=Restore,.CreateEventEx=Create,.CloseEvent=Close};
 Loaded=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,.ImageBase=Memory,.ImageSize=0x44000,.ImageCodeType=EfiBootServicesCode,.ImageDataType=EfiBootServicesData};
 Put64(Memory+0x3fe48,Base+0x283a0);Put64(Memory+0x3f5f0,(UINT64)(UINTN)Client);Put64(Memory+0x3fed8,(UINT64)(UINTN)Client);
 assert(*(UINT64*)(Memory+0x283a0)==Base+0x28308&&*(UINT32*)(Memory+0x283a8)==9&&*(UINT64*)(Memory+0x28328)==Base+0x28678&&*(UINT64*)(Memory+0x286b0)==Base+0x32418&&*(UINT32*)(Memory+0x286b8)==149);Put64(Memory+0x33a68+0x58,(UINT64)(UINTN)Ref);Put64(Ref+8,(UINT64)(UINTN)Client);
 UINT16 before=(c==1||c==43)?5:0;Put16(Memory+0x33a68+0x50,before);Put16(Memory+0x33a68+0x52,0);Put16(Ref+0x10,before);Ahb=c==43?0x88000003:before?0x08000003:0x88000002;
 if(c==3)Memory[0x1210]^=1;if(c==4)Clock->EnableClock=(VOID*)(Base+0x15b0);if(c==5)Clock->Version++;if(c==6)Loaded.ImageSize--;
 if(c==7)Put32(Memory+0x283a0+0x2c,BIT11);if(c==8)Put32(Memory+0x283a0+0x2c,BIT8);if(c==23){Put64(Ref+8,Base);Put64(Ref,(UINT64)(UINTN)Ref);}if(c==24)Put16(Memory+0x33a68+0x50,MAX_UINT16);
 if(c==26)Put64(Memory+0x3fe48,MAX_UINT64-16);if(c==36)Put64(Memory+0x33a68+0x58,0);
 Env=(PIANO_DISPLAY_CLOCK_LEASE_ENV){.Context=(VOID*)1,.Services=&Bs,.BootServicesAlive=Live,.ReadCpu=Read,.ReadGcc=Gcc,.GetReadFailureEvidence=Case==47?NULL:Evidence};EFI_STATUS e=PianoDisplayClockLeaseAcquire(&State,&Env);
 if(c==0||c==1||c==15||c==19||c==20||c==21||c==27||c==28||c==29||c==30||c==36||c==43){assert(e==EFI_SUCCESS&&State.Report.Held&&!State.Report.Retained&&EnableCalls==1&&EnabledCalls==1&&OnCalls==1&&State.Report.Acquired.Total[0]==before+1&&State.Report.Acquired.PerClient[0]==before+1);
  assert(State.Report.IsEnabled==EFI_SUCCESS&&State.Report.EnabledObserved==TRUE&&State.Report.IsOn==EFI_SUCCESS);
  if(c==15||c==43)assert(State.Report.OnObserved==FALSE&&State.Report.AfterGcc.Ahb[0]==0x88000003);
  if(c==28){Put16(Ref+0x10,Get16(Ref+0x10)+2);Put16(Memory+0x33a68+0x50,Get16(Memory+0x33a68+0x50)+2);Put16(Ref+0x12,1);Put16(Memory+0x33a68+0x52,1);}if(c==29)Clock->Version++;if(c==30){ExitFn((VOID*)9,ExitContext);Alive=FALSE;}
  e=PianoDisplayClockLeaseRelease(&State);
  if(c==0||c==1||c==15||c==27||c==28||c==36||c==43){assert(e==EFI_SUCCESS&&State.Report.Released&&!State.Report.Held&&!State.Report.Retained&&!EventLive&&DisableCalls==1);assert(State.Report.Retired.Total[0]==before+(c==28?2:0)&&State.Report.OwnedReferences==0);assert(State.Report.Baseline.MatchingSnapshots==2&&State.Report.Acquired.MatchingSnapshots==2&&State.Report.ReleaseBefore.MatchingSnapshots==2&&State.Report.Retired.MatchingSnapshots==2);}
  else {assert(e!=EFI_SUCCESS&&State.Report.Retained&&!State.Report.Released&&EventLive);if(c==28||c==29||c==30)assert(!DisableCalls);}
  unsigned calls=DisableCalls;assert(PianoDisplayClockLeaseRelease(&State)==EFI_ACCESS_DENIED&&DisableCalls==calls);
 }else{assert(e!=EFI_SUCCESS&&!DisableCalls&&!State.Report.Released);if(c==46){assert(e==EFI_NOT_READY&&!State.Report.Retained&&!State.Report.Busy&&State.Report.CleanSourceRefusal&&State.Report.Cleanup==EFI_SUCCESS&&!State.PinnedCopy&&!State.PinnedBytes&&EvidenceCalls==1&&FreeCalls==2&&!EventLive&&!GetCalls&&!EnableCalls&&!GuardCalls&&!State.Report.Held);}
  else if(c==2||c==3||c==4||c==5||c==6||c==7||c==8||c==25||c==26||c==37)assert(!GetCalls&&!EnableCalls&&!State.Report.Held);
  else assert(State.Report.Retained);if(c==39||c==40||c==41||c==42||c==44)assert(EnabledCalls==1&&!OnCalls&&State.Report.OwnedReferences==1&&State.Report.Held);
  if(c==41)assert(State.Report.EnabledObserved==0xA5);if(c==45)assert(State.Report.OnObserved==0xA5&&State.Report.IsOn==EFI_SUCCESS&&State.Report.Held);
  if(c>=47&&c<=69){assert(!GetCalls&&!EnableCalls&&!GuardCalls&&!State.Report.Held&&!EventLive&&State.PinnedCopy);
   if(c==47||c==68)assert(!EvidenceCalls);else assert(EvidenceCalls==1);
   if(c==65||c==69)assert(State.Report.CleanSourceRefusal&&State.Report.Cleanup!=EFI_SUCCESS&&FreeCalls==2);else assert(!State.Report.CleanSourceRefusal&&State.Report.Cleanup==EFI_NOT_STARTED&&FreeCalls==1);
   if(c==66)assert(State.Report.ServicesLost&&!Alive);
  }
  assert(PianoDisplayClockLeaseAcquire(&State,&Env)==EFI_INVALID_PARAMETER&&!DisableCalls);
 }
 free(State.PinnedCopy);State.PinnedCopy=NULL;free(Memory);
}
int main(int argc,char **argv){assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);File=malloc(0x44000);assert(fread(File,1,0x44000,f)==0x44000&&fgetc(f)==EOF);fclose(f);
 for(unsigned c=0;c<70;++c){pid_t p=fork();assert(p>=0);if(!p){Run(c);_exit(0);}int status;assert(waitpid(p,&status,0)==p);if(!WIFEXITED(status)||WEXITSTATUS(status)){fprintf(stderr,"display lease case%u failed\n",c);return 1;}}
 free(File);puts("Actual display lease:70 pinned nativePE/reloc/text/typedID/counter/skip/guard/lifetime/partial-cleanup/HWCG/clean-first-refusal-evidence cases; no hardware clocks executed");return 0;
}
