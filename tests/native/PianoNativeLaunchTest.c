// Whole real GenericLaunch -> actual Root provider -> actual native Core EBS.
#define main EarlierNativeMain
#include "PianoLateHandoffTest.c"
#undef main
static PIANO_FASTBOOT_LAUNCH Launch;
static UINT8 PeFile[1024];static BOOLEAN SourceTaken,SourceLoan,ImageLive;
static UINTN arms,disarms,loads,starts,unloads,source_releases,service_slices,created,closed;
static struct{EFI_EVENT_NOTIFY Fn;VOID*Context;EFI_GUID Guid;BOOLEAN Live;}LaunchEvents[2];
static EFI_STATUS SourceTake(VOID*C,VOID**Owner){assert(C==PeFile&&!SourceTaken);SourceTaken=TRUE;*Owner=PeFile;return EFI_SUCCESS;}
static EFI_STATUS SourceRead(VOID*C,VOID*Owner,UINT64 At,UINTN N,VOID*B){assert(C==PeFile&&Owner==PeFile&&SourceTaken&&At+N<=sizeof(PeFile));memcpy(B,PeFile+At,N);return EFI_SUCCESS;}
static EFI_STATUS SourceBorrow(VOID*C,VOID*Owner,PIANO_BOOT_RANGE Range,CONST VOID**View,VOID**Loan){assert(C==PeFile&&Owner==PeFile&&SourceTaken&&!SourceLoan&&Range.Offset==0&&Range.Bytes==sizeof(PeFile));SourceLoan=TRUE;*View=PeFile;*Loan=PeFile;return EFI_SUCCESS;}
static EFI_STATUS SourceUnborrow(VOID*C,VOID*Owner,VOID*Loan){assert(C==PeFile&&Owner==PeFile&&Loan==PeFile&&SourceLoan&&!ImageLive);SourceLoan=FALSE;return EFI_SUCCESS;}
static EFI_STATUS SourceRelease(VOID*C,VOID*Owner){assert(C==PeFile&&Owner==PeFile&&SourceTaken&&!SourceLoan&&!ImageLive);SourceTaken=FALSE;++source_releases;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI LaunchEvent(UINT32 Type,EFI_TPL Level,EFI_EVENT_NOTIFY Fn,CONST VOID*C,CONST EFI_GUID*G,EFI_EVENT*E){
 assert(Type==EVT_NOTIFY_SIGNAL&&Level==TPL_NOTIFY&&C==&Launch&&created<2&&!Owners.Report.UsbStopped);UINTN I=created++;LaunchEvents[I].Fn=Fn;LaunchEvents[I].Context=(VOID*)C;LaunchEvents[I].Guid=*G;LaunchEvents[I].Live=TRUE;*E=&LaunchEvents[I];return EFI_SUCCESS;}
static EFI_STATUS EFIAPI LaunchClose(EFI_EVENT E){
 if(E==(VOID*)0x987)return close(E); // real owner manager closes its own event
 for(UINTN I=0;I<2;++I)if(E==&LaunchEvents[I]){assert(LaunchEvents[I].Live&&!ImageLive);LaunchEvents[I].Live=FALSE;++closed;return EFI_SUCCESS;}abort();}
static EFI_STATUS EFIAPI LaunchLoad(BOOLEAN Boot,EFI_HANDLE P,EFI_DEVICE_PATH_PROTOCOL*Path,VOID*Data,UINTN N,EFI_HANDLE*H){
 assert(!Boot&&P==Parent&&!Path&&Data==PeFile&&N==sizeof(PeFile)&&SourceLoan&&!Calls&&!Owners.Report.UsbStopped);++loads;
 if(scenario==22)return EFI_LOAD_ERROR;ImageLive=TRUE;*H=Child;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI LaunchHandle(EFI_HANDLE H,EFI_GUID*G,VOID**P){assert(H==Child&&G==&gEfiLoadedImageProtocolGuid);++handles;
 if(!ImageLive){*P=NULL;return EFI_INVALID_PARAMETER;}*P=&NativeImage.Info;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI LaunchUnload(EFI_HANDLE H){assert(H==Child&&ImageLive);ImageLive=FALSE;++unloads;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI UnusedAllocate(EFI_MEMORY_TYPE T,UINTN N,VOID**P){(VOID)T;(VOID)N;(VOID)P;abort();}
static EFI_STATUS EFIAPI UnusedFree(VOID*P){(VOID)P;abort();}
static EFI_STATUS SliceLaunch(VOID*C,UINTN Budget){assert(C==&Late&&Budget==1000&&!Owners.Report.UsbStopped&&!Calls&&Late.Phase==PianoExitArmed);++service_slices;return EFI_SUCCESS;}
static EFI_STATUS ArmLaunch(VOID*C,EFI_HANDLE H,CONST EFI_LOADED_IMAGE_PROTOCOL*L){assert(C==&Late&&H==Child&&L==&NativeImage.Info);++arms;
 if(scenario==27)return EFI_WARN_STALE_DATA;
 if(scenario==23)return EFI_NOT_READY;return PianoLateHandoffArm(&Late,H);}
static EFI_STATUS DisarmLaunch(VOID*C,EFI_HANDLE H){assert(C==&Late&&H==Child);++disarms;
 if(scenario==24)return EFI_WARN_STALE_DATA;return PianoLateHandoffDisarm(&Late);}
static EFI_STATUS NeverShutdown(VOID*C){(VOID)C;assert(!"late path must not pre-retire");return EFI_ABORTED;}
static EFI_STATUS PlatformNotReady(VOID*C,PIANO_LINUX_MEMORY_PROOF*P){(VOID)C;ZeroMem(P,sizeof(*P));return EFI_NOT_READY;}
static EFI_STATUS EFIAPI LaunchStart(EFI_HANDLE H,UINTN*N,CHAR16**Data){assert(H==Child&&ImageLive&&SourceLoan&&Late.Phase==PianoExitArmed&&!Owners.Report.UsbStopped&&!Launch.Result.ShutdownSucceeded);++starts;
 // Work inside actual StartImage remains in the active USB/policy epoch.
 assert(SliceLaunch(&Late,1000)==EFI_SUCCESS&&!Calls);
 if(scenario==20||scenario==24){ImageLive=FALSE;*N=0;*Data=NULL;return EFI_SUCCESS;}
 assert(CoreExitBootServices(Child,mMemoryMapKey)==EFI_INVALID_PARAMETER&&Owners.Report.Clean&&Late.RetireCalls==1);
 // Deliver the real Core's Before boundary to the launch CPU-only fence.
 for(UINTN I=0;I<created;++I)if(!memcmp(&LaunchEvents[I].Guid,&gEfiEventBeforeExitBootServicesGuid,sizeof(EFI_GUID)))LaunchEvents[I].Fn(&LaunchEvents[I],LaunchEvents[I].Context);
 if(scenario==21){*N=0;*Data=NULL;return EFI_LOAD_ERROR;}
 assert(scenario==25&&CoreExitBootServices(Child,mMemoryMapKey)==EFI_SUCCESS);
 for(UINTN I=0;I<created;++I)if(!memcmp(&LaunchEvents[I].Guid,&gEfiEventExitBootServicesGuid,sizeof(EFI_GUID)))LaunchEvents[I].Fn(&LaunchEvents[I],LaunchEvents[I].Context);
 return EFI_SUCCESS;}
static VOID NativePe(void){UINT16 u16;UINT32 u32;UINT64 u64;
#define P16(O,V) do{u16=(V);memcpy(PeFile+(O),&u16,2);}while(0)
#define P32(O,V) do{u32=(V);memcpy(PeFile+(O),&u32,4);}while(0)
#define P64(O,V) do{u64=(V);memcpy(PeFile+(O),&u64,8);}while(0)
 P16(0,0x5a4d);P32(60,128);P32(128,0x4550);P16(132,0xaa64);P16(134,1);P16(148,240);P16(150,2);P16(152,0x20b);
 P32(168,4096);P64(176,0x10000000);P32(184,4096);P32(188,512);P32(208,8192);P32(212,512);P16(220,10);P32(260,16);
 P32(400,512);P32(404,4096);P32(408,512);P32(412,512);P32(428,0x60000020);
}
#ifndef PIANO_NATIVE_LAUNCH_ENTRY
#define PIANO_NATIVE_LAUNCH_ENTRY main
#endif
int PIANO_NATIVE_LAUNCH_ENTRY(int argc,char**argv){assert(argc==2);scenario=strtoul(argv[1],NULL,10);assert(scenario>=20&&scenario<=27);Setup();NativePe();
 Bs.CreateEventEx=LaunchEvent;Bs.CloseEvent=LaunchClose;Bs.LoadImage=LaunchLoad;Bs.StartImage=LaunchStart;Bs.HandleProtocol=LaunchHandle;Bs.UnloadImage=LaunchUnload;Bs.AllocatePool=UnusedAllocate;Bs.FreePool=UnusedFree;
 PIANO_LATE_HANDOFF_ENV LateEnv={.Context=&Late,.Services=&Bs,.SystemTable=&SystemTable,.ParentImage=Parent,.Owners=&Owners,.BootServicesAlive=Alive,.CheckMemory=Memory,.ValidateMemory=Validate,.FailStop=Fail};
 if(scenario==26)LateEnv.CheckMemory=PlatformNotReady;
 assert(PianoLateHandoffInitialize(&Late,&LateEnv)==EFI_SUCCESS);
 PIANO_LAUNCH_ENV E={.Context=&Late,.Services=&Bs,.ParentImage=Parent,.MaxImageBytes=0x4000,.MaxSourceBytes=1024,.ShutdownAll=NeverShutdown,.BootServicesAlive=Alive,.FailStop=Fail,
 .HandoffMode=PianoHandoffNativeLate,.NativeLateArm=ArmLaunch,.NativeLateDisarm=DisarmLaunch,.ServiceSlice=SliceLaunch};
 PIANO_LAUNCH_BLOB Blob={PeFile,sizeof(PeFile),SourceTake,SourceRead,SourceBorrow,SourceUnborrow,SourceRelease,SourceRelease};
 assert(PianoFastbootLaunchInit(&Launch)==EFI_SUCCESS);EFI_STATUS Status=EFI_SUCCESS;int Fatal=setjmp(jump);
 if(!Fatal)Status=PianoFastbootLaunchRun(&Launch,&E,&Blob,NULL,0);
 if(scenario==20)assert(!Fatal&&Status==EFI_SUCCESS&&starts==1&&arms==1&&disarms==1&&source_releases==1&&closed==2&&!Calls&&!Late.RetireCalls&&Late.Phase==PianoExitUnarmed&&service_slices==2);
 if(scenario==22)assert(!Fatal&&Status==EFI_LOAD_ERROR&&!starts&&!arms&&!disarms&&source_releases==1&&closed==2&&!Calls);
 if(scenario==23)assert(!Fatal&&Status==EFI_NOT_READY&&!starts&&arms==1&&!disarms&&unloads==1&&source_releases==1&&!Calls);
 if(scenario==26)assert(!Fatal&&Status==EFI_NOT_READY&&!starts&&arms==1&&!disarms&&unloads==1&&source_releases==1&&!Calls&&Late.Phase==PianoExitUnarmed);
 if(scenario==27)assert(Fatal&&Launch.Result.ResourcesRetained&&Launch.UnknownOwnership&&!starts&&!source_releases&&!closed&&!Calls);
 if(scenario==24)assert(Fatal&&Launch.Result.ResourcesRetained&&disarms==1&&!source_releases&&!closed&&!Calls);
 if(scenario==21||scenario==25)assert(Fatal&&Launch.BeforeEbs&&Launch.Result.ResourcesRetained&&!disarms&&!source_releases&&!closed&&Late.RetireCalls==1&&Owners.Report.Clean);
 printf("Whole actual GenericLaunch native-late case%lu passed; active service in StartImage, no pre-retirement/fakeClean, exact disarm/fence handling; host EFI/hardware boundaries\n",(unsigned long)scenario);return 0;}
