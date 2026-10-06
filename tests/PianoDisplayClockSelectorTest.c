// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse the frozen actual Reader+Guard host boundaries and captured native PE.
// This selector never executes any native Clock or NPA method.
#include <stdint.h>
static int SelectorPhase;static unsigned SelectorCase;
static void SelectorBeforeLoad(uintptr_t Address);
#define main OriginalReaderFixtureMain
#include "ActualClockSelectorFixture.h"
#undef main
#define SECOND_NODE (BASE+0x2e520)
#define SECOND_PARENT (BASE+0x30478)
#define SECOND_REF (HEAP+0x1100)
static void SelectorBeforeLoad(uintptr_t A){
 if(SelectorPhase&&SelectorCase==13&&A==SECOND_NODE+0x50){UINT32 V;memcpy(&V,Pointer(A,4),4);Put32(Pointer(A,4),V^1);}
 if(SelectorPhase&&SelectorCase==14&&A==BASE+0x3df68)Lost();
}
static VOID PrepareSelector(VOID){
 Case=200;memcpy(Image,File,sizeof(Image));Relocate();
 Bs=(EFI_BOOT_SERVICES){.RaiseTPL=Raise,.RestoreTPL=Restore,.LocateProtocol=Locate,.HandleProtocol=Handle,.GetMemoryMap=Map,.FreePool=Free,.CreateEventEx=Create,.CloseEvent=Close};Ds.GetMemorySpaceDescriptor=Gcd;Cpu.RegisterInterruptHandler=Register;
 Loaded=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,.ImageBase=(VOID*)(UINTN)BASE,.ImageSize=sizeof(Image),.ImageCodeType=EfiBootServicesCode,.ImageDataType=EfiBootServicesData};
 Lease.NativeImage=(VOID*)5;Lease.ImageIdentity=&Loaded;Lease.ImageBase=Loaded.ImageBase;Lease.ImageSize=sizeof(Image);Lease.Report.NativeBase=BASE;Lease.Report.ClockId=0x04010033;
 Put64(Image+0x3fe48,BASE+0x283a0);Put64(Image+0x3f5f0,HEAP);Put64(Image+0x3fed8,HEAP);Put64(Image+0x3fed0,HEAP+0x2000);
 Put64(Heap+0x10,HEAP+0x2000);Put64(Heap+0x2010,BASE+0x25533);Put64(Heap+0x2018,HEAP);
 Put64(Image+0x33ac0,HEAP+0x1000);Put64(Heap+0x1008,HEAP);Put32(Image+0x33ab8,0x00020003);Put32(Heap+0x1010,0x00020003);
 Put64(Image+0x3df68,HEAP+0x2100);Put64(Image+0x3e128,HEAP+0x2200);
 PIANO_DISPLAY_CLOCK_READ_ENV E={(VOID*)1,&Bs,&Ds,Alive,&Lease};assert(PianoDisplayClockReadInitialize(&Reader,&E)==EFI_SUCCESS);VerifyText();DynamicPhase=TRUE;
}
static VOID RunSelector(unsigned C){
 SelectorCase=C;PrepareSelector();PIANO_DISPLAY_CLOCK_SELECTOR Selector=C==21?PianoClockSelectGccAhb:PianoClockSelectNonGdscAhb;
 PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT R,Original;memset(&R,0xa5,sizeof(R));memcpy(&Original,&R,sizeof(R));
 if(C==1||C==2||C==12||C==27){Put64(Image+0x2e578,SECOND_REF);Put64(Heap+0x1108,HEAP);}
 if(C==2){Put32(Image+0x2e570,0x00020003);Put32(Heap+0x1110,0x00010002);Put32(Image+0x304c0,0x00010004);Put32(Image+0x304c4,0x38);Put64(Image+0x304b8,BASE+0x31220);}
 if(C==3)Put64(Image+0x304b8,BASE+0x31258);if(C==4)Put64(Image+0x304b8,BASE+0x31290);if(C==5)Put64(Image+0x304b8,HEAP+0x3000);
 if(C==6)Put64(Image+0x2e520,BASE+0x13b4f);if(C==7)Put64(Image+0x28580,BASE+0x2e288);if(C==8)Put64(Image+0x2e528,BASE+0x37528);
 if(C==9){Put64(Image+0x2e578,SECOND_REF);Put64(Heap+0x1100,SECOND_REF);Put64(Heap+0x1108,HEAP+0x3000);}
 if(C==10)Put64(Image+0x2e578,0xa00000000ULL);
 if(C==11){Put64(Image+0x2e578,SECOND_REF);for(unsigned I=0;I<64;++I){Put64(Heap+0x1100+24*I,SECOND_REF+24*(I+1));Put64(Heap+0x1108+24*I,HEAP+0x3000);}}
 if(C==12)Put32(Heap+0x1110,1);if(C==15)Case=42;if(C==16)Reader.Report.TextVerified=FALSE;
 if(C==17)Selector=(PIANO_DISPLAY_CLOCK_SELECTOR)-1;if(C==18)Selector=(PIANO_DISPLAY_CLOCK_SELECTOR)2;
 if(C==19){UINTN Before=Loads;assert(PianoDisplayClockReadSnapshotClock(&Reader,Selector,(VOID*)&Reader)==EFI_INVALID_PARAMETER&&Loads==Before);goto Finish;}
 if(C==20)Case=68;if(C==22)Loaded.ImageSize--;if(C==23)Lease.Report.ClockId=0x02010006;if(C==24)Put64(Image+0x3fed8,HEAP+0x100);
 if(C==25){UINTN Before=Loads;assert(PianoDisplayClockReadSnapshotClock(&Reader,Selector,(VOID*)&Loaded)==EFI_INVALID_PARAMETER&&Loads==Before);goto Finish;}
 if(C==26){assert(PianoDisplayClockReadSnapshotClock(&Reader,Selector,(VOID*)(UINTN)HEAP)==EFI_INVALID_PARAMETER);goto Finish;}
 if(C==27){assert(PianoDisplayClockReadSnapshotClock(&Reader,Selector,(VOID*)(UINTN)SECOND_REF)==EFI_INVALID_PARAMETER);goto Finish;}
 SelectorPhase=1;EFI_STATUS E=PianoDisplayClockReadSnapshotClock(&Reader,Selector,&R);SelectorPhase=0;
 if(C<=5||C==21){assert(E==EFI_SUCCESS&&R.Revision==1&&R.Status==EFI_SUCCESS&&R.Identity==EFI_SUCCESS&&R.MatchingSnapshots==2&&R.NativeBase==BASE&&R.NativeImage==(VOID*)5&&R.ReaderContext==&Reader&&R.LeaseContext==&Lease&&R.Global==BASE+0x283a0&&R.Client==HEAP&&R.MmClient==HEAP+0x2100&&R.MxClient==HEAP+0x2200);
  if(C==21){assert(R.ExpectedClockId==0x04010033&&R.Node==BASE+0x33a68&&R.Parent==BASE+0x37528&&R.Total[0]==3&&R.PerClient[0]==3);}
  else{assert(R.ExpectedClockId==0x02010006&&R.Provider==2&&R.Index==6&&R.Module==BASE+0x28548&&R.Array==BASE+0x2e280&&R.ClockCount==61&&R.Node==SECOND_NODE&&R.Name==BASE+0x13b4e&&R.Parent==SECOND_PARENT&&R.ParentRailMask==8&&R.ParentFlags==0x04000000);
   if(C==0)assert(!R.ClientRefPresent&&!R.ClientRef&&!R.Total[0]&&!R.PerClient[0]);
   if(C==1)assert(R.ClientRefPresent&&R.ClientRef==SECOND_REF&&!R.Total[0]&&!R.PerClient[0]);
   if(C==2)assert(R.Total[0]==3&&R.Total[1]==2&&R.PerClient[0]==2&&R.PerClient[1]==1&&R.ParentRefs[0]==4&&R.ParentRefs[1]==1&&R.ParentCachedCorner==0x38);
   if(C>=2&&C<=4)assert(R.ConfigObservation==PianoClockConfigPinned&&R.CurrentCorner==(C==2?0x38:C==3?0x80:0x100));
   if(C==5)assert(R.ConfigObservation==PianoClockConfigOtherProducer&&R.ParentCurrentConfig==HEAP+0x3000&&!R.CurrentCorner);
  }
 }else{assert(E!=EFI_SUCCESS&&!memcmp(&R,&Original,sizeof(R)));if(C==13)assert(E==EFI_MEDIA_CHANGED);if(C==14||C==15)assert(Reader.Report.Retained);}
Finish:
 assert(!NativeCalls&&!Reader.Report.MemoryOwnershipGranted&&!Reader.Report.Guard.MemoryOwnershipGranted);
 if(Reader.Report.Retained){UINTN Before=Loads;assert(PianoDisplayClockReadSnapshotClock(&Reader,Selector,&R)!=EFI_SUCCESS&&PianoDisplayClockReadClose(&Reader)==EFI_ACCESS_DENIED&&Loads==Before);}
 else assert(PianoDisplayClockReadClose(&Reader)==EFI_SUCCESS&&!Reader.PinnedCopy&&!Handlers[0]&&!Handlers[3]&&Creates==Closes);
}
int main(int Argc,char **Argv){assert(Argc==2);FILE *F=fopen(Argv[1],"rb");assert(F);File=malloc(sizeof(Image));assert(fread(File,1,sizeof(Image),F)==sizeof(Image)&&fgetc(F)==EOF);fclose(F);
 for(unsigned C=0;C<28;++C){pid_t P=fork();assert(P>=0);if(!P){RunSelector(C);_exit(0);}int S;assert(waitpid(P,&S,0)==P);if(!WIFEXITED(S)||WEXITSTATUS(S)){fprintf(stderr,"clock selector case%u failed\n",C);return 1;}}
 free(File);puts("Actual Reader+Guard selector:28 pinned native/GCC+nonGDSC/zero+missing refs/cached-corner/unknown-config/drift/EBS/cleanup/alias cases; zero native calls, no rail or clock grant");return 0;
}
