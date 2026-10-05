// Whole real LinuxSession including large LoadFile2 inside StartImage, followed
// by the real native Core/provider/Owners retirement and standard key retry.
#define PIANO_NATIVE_LAUNCH_ENTRY EarlierNativeLaunchMain
#include "PianoNativeLaunchTest.c"
#undef PIANO_NATIVE_LAUNCH_ENTRY
#include <libfdt.h>
static UINT8 Dtb[4096],*LargeInitrd;static UINTN LargeBytes=67108865;
static struct INPUT{CONST VOID*Data;UINTN Bytes;BOOLEAN Taken,Loan;}Inputs[3];
static struct{VOID*Base;UINTN Bytes;}Pools[16];static UINTN pool_allocs,pool_frees,initrd_installed,cpu_ticks,loader_ticks;
static struct{EFI_EVENT_NOTIFY Fn;VOID*Context;EFI_GUID Guid;BOOLEAN Live;}LinuxEvents[2];static UINTN linux_events,linux_closed;
EFI_GUID gFdtTableGuid={.Data1=0x1231},gEfiLoadFile2ProtocolGuid={.Data1=0x1232},gEfiDevicePathProtocolGuid={.Data1=0x1233};
static EFI_CONFIGURATION_TABLE FdtTable;
static EFI_STATUS InputTake(VOID*C,VOID**Owner){struct INPUT*S=C;assert(!S->Taken);S->Taken=TRUE;*Owner=S;return EFI_SUCCESS;}
static EFI_STATUS InputRead(VOID*C,VOID*Owner,UINT64 At,UINTN Bytes,VOID*Out){struct INPUT*S=C;assert(Owner==S&&S->Taken&&At+Bytes<=S->Bytes);memcpy(Out,(CONST UINT8*)S->Data+At,Bytes);return EFI_SUCCESS;}
static EFI_STATUS InputBorrow(VOID*C,VOID*Owner,PIANO_BOOT_RANGE R,CONST VOID**View,VOID**Loan){struct INPUT*S=C;assert(Owner==S&&S->Taken&&!S->Loan&&R.Offset==0&&R.Bytes==S->Bytes);S->Loan=TRUE;*View=S->Data;*Loan=S;return EFI_SUCCESS;}
static EFI_STATUS InputUnborrow(VOID*C,VOID*Owner,VOID*Loan){struct INPUT*S=C;assert(Owner==S&&Loan==S&&S->Loan&&!ImageLive&&!initrd_installed);S->Loan=FALSE;return EFI_SUCCESS;}
static EFI_STATUS InputRelease(VOID*C,VOID*Owner){struct INPUT*S=C;assert(Owner==S&&S->Taken&&!S->Loan&&!ImageLive&&!initrd_installed);S->Taken=FALSE;++source_releases;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI PoolAllocate(EFI_MEMORY_TYPE Type,UINTN Bytes,VOID**Out){assert(Type==EfiLoaderData);for(UINTN I=0;I<16;++I)if(!Pools[I].Base){Pools[I].Base=calloc(1,Bytes);assert(Pools[I].Base);Pools[I].Bytes=Bytes;*Out=Pools[I].Base;++pool_allocs;++mMemoryMapKey;return EFI_SUCCESS;}abort();}
static EFI_STATUS EFIAPI PoolFree(VOID*P){for(UINTN I=0;I<16;++I)if(Pools[I].Base==P){assert(!ImageLive);free(P);Pools[I].Base=NULL;++pool_frees;++mMemoryMapKey;return EFI_SUCCESS;}abort();}
static EFI_STATUS EFIAPI Configuration(EFI_GUID*G,VOID*P){assert(G==&gFdtTableGuid);FdtTable=(EFI_CONFIGURATION_TABLE){*G,P};SystemTable.ConfigurationTable=&FdtTable;SystemTable.NumberOfTableEntries=P?1:0;++mMemoryMapKey;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI MultipleInstall(EFI_HANDLE*H,...){assert(!initrd_installed);initrd_installed=1;*H=(VOID*)0x555;++mMemoryMapKey;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI MultipleUninstall(EFI_HANDLE H,...){assert(H==(VOID*)0x555&&initrd_installed&&!ImageLive);initrd_installed=0;++mMemoryMapKey;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI LocatePath(EFI_GUID*G,EFI_DEVICE_PATH_PROTOCOL**P,EFI_HANDLE*H){(VOID)P;(VOID)H;assert(G==&gEfiLoadFile2ProtocolGuid);return EFI_NOT_FOUND;}
static EFI_STATUS EFIAPI LinuxEvent(UINT32 Type,EFI_TPL Level,EFI_EVENT_NOTIFY Fn,CONST VOID*C,CONST EFI_GUID*G,EFI_EVENT*E){
 assert(Type==EVT_NOTIFY_SIGNAL&&Level==TPL_NOTIFY&&C==&Linux&&linux_events<2);UINTN I=linux_events++;LinuxEvents[I].Fn=Fn;LinuxEvents[I].Context=(VOID*)C;LinuxEvents[I].Guid=*G;LinuxEvents[I].Live=TRUE;*E=&LinuxEvents[I];++mMemoryMapKey;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI LinuxClose(EFI_EVENT E){if(E==(VOID*)0x987)return close(E);for(UINTN I=0;I<2;++I)if(E==&LinuxEvents[I]){assert(LinuxEvents[I].Live&&!ImageLive);LinuxEvents[I].Live=FALSE;++linux_closed;++mMemoryMapKey;return EFI_SUCCESS;}abort();}
static EFI_STATUS EFIAPI LinuxLoad(BOOLEAN B,EFI_HANDLE P,EFI_DEVICE_PATH_PROTOCOL*Path,VOID*Source,UINTN Bytes,EFI_HANDLE*H){
 assert(!B&&P==Parent&&!Path&&Source==PeFile&&Bytes==sizeof(PeFile)&&Inputs[0].Loan&&!Calls&&!Owners.Report.UsbStopped);++loads;
 if(scenario==33)return EFI_LOAD_ERROR;ImageLive=TRUE;*H=Child;return EFI_SUCCESS;}
static EFI_STATUS LinuxMemory(VOID*C,CONST VOID*Fdt,UINTN Bytes,PIANO_LINUX_MEMORY_PROOF*P){assert(C==&Late&&fdt_check_header(Fdt)==0&&Bytes==(UINTN)fdt_totalsize(Fdt));return scenario==37?PlatformNotReady(C,P):Memory(C,P);}
static EFI_STATUS NeverPrepare(VOID*C,CONST PIANO_LINUX_MEMORY_PROOF*P,PIANO_LINUX_RETIRE_PROOF*R){(VOID)C;(VOID)P;(VOID)R;assert(!"late Linux must not prepare/retire before Start");return EFI_ABORTED;}
static EFI_STATUS NeverValidateRetired(VOID*C,CONST PIANO_LINUX_RETIRE_PROOF*P){(VOID)C;(VOID)P;assert(!"Arm is not a clean retirement report");return EFI_ABORTED;}
static EFI_STATUS LoaderSlice(VOID*C,UINTN Budget){assert(C==&Late&&Budget==1000&&!Owners.Report.UsbStopped&&!Calls);++loader_ticks;return EFI_SUCCESS;}
static BOOLEAN CpuAlive(VOID*C){assert(C==&Linux);return live;}
static EFI_STATUS CpuMemory(VOID*C,PIANO_LINUX_MEMORY_PROOF*P){assert(C==&Linux);return Memory(&Late,P);}
static EFI_STATUS CpuValidate(VOID*C,CONST PIANO_LINUX_MEMORY_PROOF*P){assert(C==&Linux);return Validate(&Late,P);}
static EFI_STATUS CpuBuffer(VOID*C,CONST PIANO_LINUX_MEMORY_PROOF*P,CONST VOID*Producer,VOID*Owner,CONST VOID*Base,UINT64 Bytes){
 assert(C==&Linux&&P->BootEpoch==1);CONST struct INPUT*S=Producer;assert(S>=Inputs&&S<Inputs+3&&Owner==S&&S->Taken&&S->Loan&&Base==S->Data&&Bytes==S->Bytes);return EFI_SUCCESS;}
static EFI_STATUS CpuTick(VOID*C,UINTN Budget){assert(C==&Linux&&Budget==1000&&!Owners.Report.UsbStopped&&!Calls&&!mExitBootServicesCalled);++cpu_ticks;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI LinuxStart(EFI_HANDLE H,UINTN*N,CHAR16**Out){assert(H==Child&&ImageLive&&Late.Phase==PianoExitArmed&&!Linux.OwnersRetired&&!Linux.Retire.Clean&&!Owners.Report.UsbStopped);++starts;
 EFI_DEVICE_PATH_PROTOCOL End={END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};UINTN Bytes=0;
 assert(Linux.Load.LoadFile(&Linux.Load,&End,FALSE,&Bytes,NULL)==EFI_BUFFER_TOO_SMALL&&Bytes==LargeBytes);
 UINT8*Copy=malloc(Bytes);assert(Copy);UINTN BeforeTicks=cpu_ticks;
 assert(Linux.Load.LoadFile(&Linux.Load,&End,FALSE,&Bytes,Copy)==EFI_SUCCESS&&!memcmp(Copy,LargeInitrd,LargeBytes)&&cpu_ticks-BeforeTicks==1026&&!Calls&&!Owners.Report.UsbStopped);free(Copy);
 if(scenario==30||scenario==35){ImageLive=FALSE;*N=0;*Out=NULL;return EFI_SUCCESS;}
 assert(CoreExitBootServices(Child,mMemoryMapKey)==EFI_INVALID_PARAMETER&&Owners.Report.Clean&&Late.RetireCalls==1);
 for(UINTN I=0;I<linux_events;++I)if(!memcmp(&LinuxEvents[I].Guid,&gEfiEventBeforeExitBootServicesGuid,sizeof(EFI_GUID)))LinuxEvents[I].Fn(&LinuxEvents[I],LinuxEvents[I].Context);
 if(scenario==31){*N=0;*Out=NULL;return EFI_LOAD_ERROR;}
 assert(scenario==32&&CoreExitBootServices(Child,mMemoryMapKey)==EFI_SUCCESS);
 for(UINTN I=0;I<linux_events;++I)if(!memcmp(&LinuxEvents[I].Guid,&gEfiEventExitBootServicesGuid,sizeof(EFI_GUID)))LinuxEvents[I].Fn(&LinuxEvents[I],LinuxEvents[I].Context);
 return EFI_SUCCESS;}
static EFI_STATUS LinuxArm(VOID*C,EFI_HANDLE H,CONST EFI_LOADED_IMAGE_PROTOCOL*L){++arms;assert(C==&Late&&H==Child&&L==&NativeImage.Info);return scenario==34?EFI_NOT_READY:PianoLateHandoffArm(&Late,H);}
static EFI_STATUS LinuxDisarm(VOID*C,EFI_HANDLE H){++disarms;assert(C==&Late&&H==Child);return scenario==35?EFI_WARN_STALE_DATA:PianoLateHandoffDisarm(&Late);}
int main(int argc,char**argv){assert(argc==2);scenario=strtoul(argv[1],NULL,10);assert(scenario>=30&&scenario<=37);Setup();NativePe();LargeInitrd=malloc(LargeBytes);assert(LargeInitrd);memset(LargeInitrd,0x5a,LargeBytes);
 assert(fdt_create_empty_tree(Dtb,sizeof(Dtb))==0&&fdt_add_subnode(Dtb,0,"chosen")>=0&&fdt_pack(Dtb)==0);
 Inputs[0]=(struct INPUT){PeFile,sizeof(PeFile),FALSE,FALSE};Inputs[1]=(struct INPUT){Dtb,(UINTN)fdt_totalsize(Dtb),FALSE,FALSE};Inputs[2]=(struct INPUT){LargeInitrd,LargeBytes,FALSE,FALSE};
 Bs.CreateEventEx=LinuxEvent;Bs.CloseEvent=LinuxClose;Bs.LoadImage=LinuxLoad;Bs.StartImage=LinuxStart;Bs.HandleProtocol=LaunchHandle;Bs.UnloadImage=LaunchUnload;Bs.AllocatePool=PoolAllocate;Bs.FreePool=PoolFree;
 Bs.InstallConfigurationTable=Configuration;Bs.InstallMultipleProtocolInterfaces=MultipleInstall;Bs.UninstallMultipleProtocolInterfaces=MultipleUninstall;Bs.LocateDevicePath=LocatePath;
 PIANO_LATE_HANDOFF_ENV LE={.Context=&Late,.Services=&Bs,.SystemTable=&SystemTable,.ParentImage=Parent,.Owners=&Owners,.BootServicesAlive=Alive,.CheckMemory=Memory,.ValidateMemory=Validate,.FailStop=Fail};assert(PianoLateHandoffInitialize(&Late,&LE)==EFI_SUCCESS);
 PIANO_CPU_INPUT_ENV CE={.Context=&Linux,.BootServicesAlive=CpuAlive,.ServiceSlice=CpuTick,.CheckMemory=CpuMemory,.ValidateMemory=CpuValidate,.ValidateBuffer=CpuBuffer};
 PIANO_LINUX_EFI_ENV E={.Context=&Late,.Services=&Bs,.SystemTable=&SystemTable,.ParentImage=Parent,.MaxKernelBytes=1024,.MaxLoadedBytes=16384,.MaxDtbBytes=4096,.MaxInitrdBytes=PIANO_CPU_INPUT_MAX_BYTES,
 .ExpectedDramBytes=16ULL*1024*1024*1024,.ExpectedOwners=PIANO_OWNER_ALL_MASK,.CommandLine=L"rdinit=/pianoinit piano.root=ram",.BootServicesAlive=Alive,.ServiceSlice=LoaderSlice,.CheckMemory=LinuxMemory,.ValidateMemory=Validate,
 .PrepareHandoff=NeverPrepare,.ValidateRetired=NeverValidateRetired,.FailStop=Fail,.MaxSourceBytes=PIANO_CPU_INPUT_MAX_BYTES,.Cpu=&CE,.HandoffMode=PianoHandoffNativeLate,.NativeLateArm=LinuxArm,.NativeLateDisarm=LinuxDisarm};
 if(scenario==36)E.NativeLateArm=NULL;
 PIANO_LAUNCH_BLOB B[3];for(UINTN I=0;I<3;++I)B[I]=(PIANO_LAUNCH_BLOB){&Inputs[I],Inputs[I].Bytes,InputTake,InputRead,InputBorrow,InputUnborrow,NULL,InputRelease};
 EFI_STATUS Status=EFI_SUCCESS;int Fatal=setjmp(jump);if(!Fatal)Status=PianoLinuxEfiSessionRun(&Linux,&E,&B[0],&B[1],&B[2]);
 if(scenario==30)assert(!Fatal&&Status==EFI_ABORTED&&starts==1&&arms==1&&disarms==1&&source_releases==3&&linux_closed==2&&pool_allocs==pool_frees&&!Calls&&!Late.RetireCalls&&Late.Phase==PianoExitUnarmed&&!Linux.OwnersRetired);
 if(scenario==33)assert(!Fatal&&Status==EFI_LOAD_ERROR&&!starts&&!arms&&!disarms&&source_releases==3&&pool_allocs==pool_frees&&!Calls);
 if(scenario==34)assert(!Fatal&&Status==EFI_NOT_READY&&!starts&&arms==1&&!disarms&&source_releases==3&&unloads==1&&!Calls);
 if(scenario==35)assert(Fatal&&Linux.Retained&&disarms==1&&!source_releases&&!linux_closed&&!Calls);
 if(scenario==36)assert(!Fatal&&Status==EFI_NOT_READY&&!starts&&!arms&&!pool_allocs&&!Inputs[0].Taken);
 if(scenario==37)assert(!Fatal&&Status==EFI_NOT_READY&&!starts&&!arms&&!loads&&source_releases==3&&pool_allocs==pool_frees&&!Calls);
 if(scenario==31||scenario==32)assert(Fatal&&Linux.BeforeEbs&&Linux.Retained&&!disarms&&!source_releases&&!linux_closed&&Late.RetireCalls==1&&Owners.Report.Clean);
 printf("Whole actual LinuxSession native-late case%lu passed: Starts=%lu CPU-slices=%lu Arm=%lu Disarm=%lu Retire=%u; no pre-retire/fakeClean; native key retry/fences/cleanup; host boundaries\n",(unsigned long)scenario,(unsigned long)starts,(unsigned long)cpu_ticks,(unsigned long)arms,(unsigned long)disarms,Late.RetireCalls);
 for(UINTN I=0;I<16;++I)free(Pools[I].Base);free(LargeInitrd);return 0;}
