// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual child owner + GCC Lease + Reader + Guard + pinned Clock PE. Native ARM
// calls and the separate NPA/VCS observer's typed boundary are host fixtures.
#include <stdint.h>
static unsigned ChildCase,ChildGet,ChildEnable,ChildDisable,ChildQueries;
static int ChildCloseWarning(void *Event);
#define main OriginalPipelineFixtureMain
#include "ActualNonGdscFixture.h"
#undef main
#include "../bootprofiles/display-rail/PianoDisplayNonGdscClock.h"
#define CHILD_NODE (BASE+0x2e520)
#define CHILD_PARENT (BASE+0x30478)
#define CHILD_REF (HEAP+0x1800)
static PIANO_NON_GDSC_CLOCK Child;
static PIANO_DISPLAY_RAIL_OBSERVER RailObserver;
static BOOLEAN RailAlive(VOID *Context){assert(Context==&RailObserver);return ChildCase==23?2:Services;}
static int ChildCloseWarning(void *Event){EVENT *E=Event;return ChildCase==15&&ChildDisable&&E->Context==&Child&&!E->Guard;}
static VOID ChildBoundary(EFI_CLOCK_PROTOCOL *P){assert(Services&&P==(VOID*)(UINTN)(BASE+0x28148)&&Tpl==TPL_APPLICATION&&mDisplay.Lease.Report.Held&&mDisplay.Lease.BorrowToken==Child.Report.TransactionToken&&Child.Report.TransactionToken&&!m.Report.Active&&!m.Report.SyncOwned&&!m.Report.SErrorOwned&&!Handlers[0]&&!Handlers[3]);}
EFI_STATUS PianoNonGdscHostGet(EFI_CLOCK_PROTOCOL *P,CONST CHAR8 *Name,UINTN *Id){ChildBoundary(P);assert(!strcmp(Name,"disp_cc_mdss_non_gdsc_ahb_clk")&&*Id==MAX_UINTN);ChildGet++;if(ChildCase==2)return EFI_SUCCESS;
 if(!*(UINT64*)(UINTN)(CHILD_NODE+0x58)){Put64(CHILD_NODE+0x58,CHILD_REF);Put64(CHILD_REF+8,HEAP);}*Id=ChildCase==21?0x04010033:0x02010006;return EFI_SUCCESS;
}
EFI_STATUS PianoNonGdscHostChange(EFI_CLOCK_PROTOCOL *P,UINTN Id,BOOLEAN Enable){ChildBoundary(P);assert(Id==0x02010006);UINT16 Before=Get16(CHILD_NODE+0x50);
 if(Enable){assert(!ChildEnable&&!ChildDisable);ChildEnable++;if(ChildCase!=5)Put16(CHILD_NODE+0x50,Before+1);if(ChildCase!=6)Put16(CHILD_REF+0x10,Get16(CHILD_REF+0x10)+1);
  if(!Before&&ChildCase!=7)Put16(CHILD_PARENT+0x48,Get16(CHILD_PARENT+0x48)+1);Put64(CHILD_PARENT+0x40,BASE+0x31220);Put64(CHILD_PARENT+0x4c,0x38);
  if(ChildCase==16)Lost();return ChildCase==3?EFI_WARN_STALE_DATA:ChildCase==4?EFI_DEVICE_ERROR:EFI_SUCCESS;
 }
 assert(ChildEnable&&!ChildDisable);ChildDisable++;if(ChildCase!=13){Put16(CHILD_NODE+0x50,Before-1);Put16(CHILD_REF+0x10,Get16(CHILD_REF+0x10)-1);}
 if(Before==1&&ChildCase!=14){Put16(CHILD_PARENT+0x48,Get16(CHILD_PARENT+0x48)-1);Put64(CHILD_PARENT+0x4c,0);}return ChildCase==12?EFI_WARN_STALE_DATA:ChildCase==22?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
EFI_STATUS PianoNonGdscHostQuery(EFI_CLOCK_PROTOCOL *P,UINTN Id,BOOLEAN *Value,BOOLEAN On){ChildBoundary(P);assert(Id==0x02010006&&*Value==0xa5);ChildQueries++;
 if((!On&&ChildCase==9)||(On&&ChildCase==10))return EFI_SUCCESS;*Value=On?FALSE:ChildCase==8?FALSE:TRUE;return EFI_SUCCESS;
}
BOOLEAN PianoDisplayRailRetained(CONST PIANO_DISPLAY_RAIL_OBSERVER *S){assert(S==&RailObserver);return S->Report.Retained||S->Report.ServicesLost;}
CONST PIANO_DISPLAY_RAIL_REPORT *PianoDisplayRailReport(CONST PIANO_DISPLAY_RAIL_OBSERVER *S){assert(S==&RailObserver);return &S->Report;}
EFI_STATUS PianoDisplayRailObserve(PIANO_DISPLAY_RAIL_OBSERVER *S,CONST CHAR8 *Phase){
 assert(S==&RailObserver&&S->Report.Count<16&&!m.Report.Active&&!Handlers[0]&&!Handlers[3]);UINT32 I=S->Report.Count++;PIANO_DISPLAY_RAIL_SNAPSHOT *R=&S->Report.Snapshot[I];ZeroMem(R,sizeof(*R));assert(strlen(Phase)<sizeof(R->Phase));memcpy(R->Phase,Phase,strlen(Phase));
 EFI_STATUS E=PianoDisplayClockReadSnapshotClock(&mDisplay.Reader,PianoClockSelectNonGdscAhb,&R->ClockBefore);if(E!=EFI_SUCCESS)return R->Status=E;
 E=PianoDisplayClockReadSnapshotClock(&mDisplay.Reader,PianoClockSelectNonGdscAhb,&R->ClockAfter);if(E!=EFI_SUCCESS)return R->Status=E;
 for(unsigned J=0;J<2;++J){R->Mm[J].Status=EFI_SUCCESS;R->Mm[J].Client=R->ClockBefore.MmClient;R->Mm[J].ActiveRequest=R->Mm[J].PendingRequest=R->Mm[J].NpaApplied=R->Mm[J].VcsApplied=R->ClockBefore.ParentCachedCorner;memcpy(R->Mm[J].ResourceName,"/vcs/vdd_mm",12);R->Mx[J].Status=EFI_NOT_FOUND;}
 R->MmCoherent=TRUE;if((ChildCase==11&&ChildEnable)||ChildCase==20||(ChildCase==24&&ChildDisable))R->MmCoherent=FALSE;
 assert(!R->PowerReady&&!R->RpmhCompletionObserved&&!R->MemoryOwnershipGranted);return R->Status=EFI_SUCCESS;
}
static VOID Setup(VOID){
 Case=200;Image=mmap((VOID*)(UINTN)BASE,IMAGE_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(Image==(VOID*)(UINTN)BASE);Heap=mmap((VOID*)(UINTN)HEAP,0x4000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(Heap==(VOID*)(UINTN)HEAP);
 memcpy(Image,File,IMAGE_BYTES);Relocate();Put64(BASE+0x3fe48,BASE+0x283a0);Put64(BASE+0x3f5f0,HEAP);Put64(BASE+0x3fed8,HEAP);Put64(BASE+0x3fed0,HEAP+0x2000);Put64(HEAP+0x10,HEAP+0x2000);Put64(HEAP+0x2010,BASE+0x25533);Put64(HEAP+0x2018,HEAP);Put64(NODE+0x58,0);Put16(NODE+0x50,0);Put16(NODE+0x52,0);
 Put64(BASE+0x3df68,HEAP+0x2100);Put64(BASE+0x3e128,0);Put64(CHILD_NODE+0x58,0);Put16(CHILD_NODE+0x50,0);Put16(CHILD_NODE+0x52,0);
 Bs=(EFI_BOOT_SERVICES){.RaiseTPL=Raise,.RestoreTPL=Restore,.LocateProtocol=Locate,.LocateHandleBuffer=Handles,.HandleProtocol=Handle,.GetMemoryMap=Map,.FreePool=Free,.CreateEventEx=Create,.CloseEvent=Close};Ds.GetMemorySpaceDescriptor=Gcd;Cpu.RegisterInterruptHandler=Register;
 Loaded=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,.ImageBase=Image,.ImageSize=IMAGE_BYTES,.ImageCodeType=EfiBootServicesCode,.ImageDataType=EfiBootServicesData};mDisplay.Alive=GlobalAlive;mDisplay.Attempted=TRUE;
 PIANO_DISPLAY_CLOCK_READ_ENV RE={.Context=&mDisplay,.Services=&Bs,.DxeServices=&Ds,.BootServicesAlive=ReadAlive,.Lease=&mDisplay.Lease};assert(PianoDisplayClockReadInitialize(&mDisplay.Reader,&RE)==EFI_SUCCESS);
 PIANO_DISPLAY_CLOCK_LEASE_ENV GE={.Context=&mDisplay.Reader,.Services=&Bs,.BootServicesAlive=LeaseAlive,.ReadCpu=PianoDisplayClockReadCpu,.ReadGcc=Gcc,.GetReadFailureEvidence=PianoDisplayClockReadFailureEvidence};assert(PianoDisplayClockLeaseAcquire(&mDisplay.Lease,&GE)==EFI_SUCCESS);
 RailObserver.Env=(PIANO_DISPLAY_RAIL_ENV){.Context=&RailObserver,.Services=&Bs,.DxeServices=&Ds,.Alive=RailAlive,.ClockReader=&mDisplay.Reader};RailObserver.Report.Initialized=TRUE;
}
static VOID RunChild(unsigned C){ChildCase=C;Setup();if(C==1){Put64(CHILD_NODE+0x58,CHILD_REF);Put64(CHILD_REF+8,HEAP);Put16(CHILD_NODE+0x50,5);Put16(CHILD_REF+0x10,2);Put16(CHILD_PARENT+0x48,3);Put64(CHILD_PARENT+0x40,BASE+0x31258);Put64(CHILD_PARENT+0x4c,0x80);}
 if(C==18)Put64(CHILD_PARENT+0xc,16);if(C==19)Put64(BASE+0x283a0+0x2c,BIT11);
 PIANO_NON_GDSC_CLOCK_ENV E={&mDisplay.Lease,&mDisplay.Reader,&RailObserver};EFI_STATUS Status=PianoDisplayNonGdscClockAcquire(&Child,&E);
 if(C==0||C==1||C==12||C==13||C==14||C==15||C==17||C==22||C==24){assert(Status==EFI_SUCCESS&&Child.Report.Held&&Child.Report.OwnedReferences==1&&!Child.Report.Retained&&!Child.Report.TransactionToken&&!mDisplay.Lease.BorrowToken&&ChildGet==1&&ChildEnable==1&&ChildQueries==2&&Child.Report.EnabledObserved&&Child.Report.OnObserved==FALSE);
  assert(Child.Report.Acquired.Total[0]==(C==1?6:1)&&Child.Report.Acquired.ParentRefs[0]==(C==1?3:1));
  if(C==17){Put16(CHILD_NODE+0x50,3);Put16(CHILD_REF+0x10,3);Put16(CHILD_PARENT+0x48,2);}
  Status=PianoDisplayNonGdscClockRelease(&Child);
  if(C==0||C==1||C==17){assert(Status==EFI_SUCCESS&&Child.Report.Released&&!Child.Report.Held&&!Child.Report.Retained&&!Child.Report.OwnedReferences&&!Child.Report.TransactionToken&&!mDisplay.Lease.BorrowToken&&!Child.Exit&&ChildDisable==1&&RailObserver.Report.Count==4);
   assert(Child.Report.Retired.Total[0]==(C==1?5:C==17?2:0)&&Child.Report.Retired.ParentRefs[0]==(C==1?3:C==17?2:0));
   assert(PianoDisplayClockLeaseRelease(&mDisplay.Lease)==EFI_SUCCESS&&PianoDisplayClockReadClose(&mDisplay.Reader)==EFI_SUCCESS&&EventCount==EventClosed&&!Handlers[0]&&!Handlers[3]);
  }else assert(Status!=EFI_SUCCESS&&Child.Report.Retained&&Child.Report.Held&&!Child.Report.Released&&Child.Report.TransactionToken&&mDisplay.Lease.BorrowToken);
 }else{assert(Status!=EFI_SUCCESS&&Child.Report.Retained&&!Child.Report.Released&&!ChildDisable);if(C==2||C>=18)assert(!ChildEnable);if(C==19||C==20||C==23)assert(!ChildGet);if(C==16)assert(Child.Report.ServicesLost&&!Services);}
 UINTN Calls=ChildDisable;assert(PianoDisplayNonGdscClockRelease(&Child)==EFI_ACCESS_DENIED&&ChildDisable==Calls);
 if(Child.Report.Retained&&C!=23)assert(PianoDisplayClockLeaseRelease(&mDisplay.Lease)==EFI_ACCESS_DENIED);
 assert(!Child.Report.RailAfter.PowerReady&&!Child.Report.RailAfter.RpmhCompletionObserved);
}
int main(int Argc,char **Argv){assert(Argc==2);FILE *F=fopen(Argv[1],"rb");assert(F);File=malloc(IMAGE_BYTES);assert(fread(File,1,IMAGE_BYTES,F)==IMAGE_BYTES&&fgetc(F)==EOF);fclose(F);for(unsigned C=0;C<25;++C){pid_t P=fork();assert(P>=0);if(!P){RunChild(C);_exit(0);}int Status;assert(waitpid(P,&Status,0)==P);if(!WIFEXITED(Status)||WEXITSTATUS(Status)){fprintf(stderr,"nonGdsc child case%u failed\n",C);return 1;}}free(File);puts("Actual nonGdsc owner+GCC Lease+Reader+Guard:25 real-source/native-boundary/node-domain-delta/HWCG/rail-observation/lifetime/retain cases; Rail graph boundary is a fixture, no hardware");return 0;}
