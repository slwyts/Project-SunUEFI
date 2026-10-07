// Real native EBS/key/current-image functions + real provider/client/owners.
// Hardware, timer, memory and protocol boundaries are host fixtures only.
#include <setjmp.h>
#include <stdlib.h>
#undef NULL
#include "PianoLateOwnersFixture.h"
#include "../../uefi/components/product-handoff/PianoLateHandoff.h"
#include "../../uefi/components/os-boot/PianoLinuxEfiSession.h"
#include <openssl/sha.h>
#include <Library/PianoProductExitLib.h>
#include <Protocol/Timer.h>
#include <Protocol/Cpu.h>
#include <Protocol/Runtime.h>
#include <Library/DebugLib.h>
#include <Pi/PiDxeCis.h>
EFI_SYSTEM_TABLE *gST;static EFI_SYSTEM_TABLE SystemTable;
EFI_GUID gEfiLoadedImageProtocolGuid={.Data1=0x11};
EFI_GUID gEfiEventBeforeExitBootServicesGuid={.Data1=0x12};
EFI_GUID gEventExitBootServicesFailedGuid={.Data1=0x13};
static PIANO_LATE_HANDOFF Late;static EFI_LOADED_IMAGE_PROTOCOL Loaded;
static EFI_HANDLE Child=(VOID*)0x111,Parent=(VOID*)0x222;
static PIANO_PRODUCT_EXIT_PROTOCOL *Installed;
static UINTN scenario,locates,handles,installs,uninstalls,memchecks,memvalidations,before_events,exit_events,failed_events,timer_calls,interrupt_calls,locks;
static BOOLEAN live=TRUE,mExitBootServicesCalled,gMemoryMapTerminated;
static UINTN mMemoryMapKey=10;
static EFI_TPL gEfiCurrentTpl=TPL_APPLICATION;
static jmp_buf jump;
typedef struct {UINTN Signature;EFI_HANDLE Handle;BOOLEAN Started;EFI_LOADED_IMAGE_PROTOCOL Info;struct{UINT16 ImageType;}ImageContext;} LOADED_IMAGE_PRIVATE_DATA;
#define LOADED_IMAGE_PRIVATE_DATA_SIGNATURE SIGNATURE_32('l','d','r','i')
static LOADED_IMAGE_PRIVATE_DATA NativeImage,*mCurrentImage;
typedef struct {UINTN Signature;LIST_ENTRY Link;EFI_MEMORY_TYPE Type;UINT64 Start,End;} MEMORY_MAP;
#define MEMORY_MAP_SIGNATURE SIGNATURE_32('m','m','a','p')
static LIST_ENTRY gMemoryMap;
static struct{BOOLEAN Runtime;}mMemoryTypeStatistics[EfiMaxMemoryType];
static EFI_TIMER_ARCH_PROTOCOL Timer,*gTimer=&Timer;
static EFI_CPU_ARCH_PROTOCOL Cpu,*gCpu=&Cpu;
static EFI_RUNTIME_ARCH_PROTOCOL Rt,*gRuntime=&Rt;
static EFI_SYSTEM_TABLE *gDxeCoreST=&SystemTable;
VOID *EFIAPI CopyMem(VOID*A,CONST VOID*B,UINTN N){return memcpy(A,B,N);}
INTN EFIAPI CompareMem(CONST VOID*A,CONST VOID*B,UINTN N){return memcmp(A,B,N);}
UINTN EFIAPI Sha256GetContextSize(VOID){return sizeof(SHA256_CTX);}
BOOLEAN EFIAPI Sha256Init(VOID*C){return SHA256_Init(C)==1;}
BOOLEAN EFIAPI Sha256Update(VOID*C,CONST VOID*P,UINTN N){return SHA256_Update(C,P,N)==1;}
BOOLEAN EFIAPI Sha256Final(VOID*C,UINT8*H){return SHA256_Final(H,C)==1;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){(VOID)Level;return FALSE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8*File,UINTN Line,CONST CHAR8*Text){fprintf(stderr,"assert %s:%llu %s\n",File,(unsigned long long)Line,Text);abort();}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8*Format,...){(VOID)Level;(VOID)Format;}
VOID EFIAPI CpuDeadLoop(VOID){longjmp(jump,2);}
VOID CoreAcquireMemoryLock(VOID){assert(!locks);++locks;}
VOID CoreReleaseMemoryLock(VOID){assert(locks==1);--locks;}
static BOOLEAN Alive(VOID*Context){assert(Context==&Late);return live;}
static VOID Fail(VOID*Context,EFI_STATUS Status){assert(Context==&Late&&Status!=EFI_SUCCESS);longjmp(jump,1);}
static EFI_STATUS Memory(VOID*Context,PIANO_LINUX_MEMORY_PROOF*P){assert(Context==&Late&&Tpl==TPL_APPLICATION&&!mExitBootServicesCalled);++memchecks;
 *P=(PIANO_LINUX_MEMORY_PROOF){.Revision=1,.Status=EFI_SUCCESS,.BootEpoch=scenario==7&&memchecks>=3?2:1,.DramBytes=16ULL*1024*1024*1024,
 .NormalBytes=4ULL*1024*1024*1024,.FullDdr=scenario!=5,.FixedReservations=TRUE,.DynamicReservations=TRUE,.RuntimeRegions=TRUE,.CacheVerified=TRUE,.OwnershipVerified=TRUE};return EFI_SUCCESS;}
static EFI_STATUS Validate(VOID*Context,CONST PIANO_LINUX_MEMORY_PROOF*P){assert(Context==&Late&&P->BootEpoch&&!mExitBootServicesCalled);++memvalidations;return scenario==6?EFI_NOT_READY:EFI_SUCCESS;}
static PIANO_RAW_LINUX_REPORT RawReport;
static BOOLEAN RawValidate(CONST PIANO_RAW_LINUX_REPORT*R,CONST PIANO_PRODUCT_OWNERS*O,EFI_HANDLE Image){
 return R==&RawReport&&O==&Owners&&Image==Child&&Owners.Report.Clean&&scenario!=19;}
static EFI_STATUS EFIAPI Install(EFI_HANDLE*H,EFI_GUID*G,EFI_INTERFACE_TYPE Type,VOID*P){EFI_GUID Expected=PIANO_PRODUCT_EXIT_PROTOCOL_GUID;
 assert(Type==EFI_NATIVE_INTERFACE&&!memcmp(G,&Expected,sizeof(*G))&&!Installed&&Tpl==TPL_APPLICATION);Installed=P;*H=(VOID*)0x333;++installs;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Uninstall(EFI_HANDLE H,EFI_GUID*G,VOID*P){EFI_GUID Expected=PIANO_PRODUCT_EXIT_PROTOCOL_GUID;
 assert(H==(VOID*)0x333&&!memcmp(G,&Expected,sizeof(*G))&&P==Installed&&Tpl==TPL_APPLICATION);++uninstalls;
 if(scenario==17)return EFI_WARN_STALE_DATA;Installed=NULL;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Locate(EFI_GUID*G,VOID*Registration,VOID**Out){EFI_GUID Expected=PIANO_PRODUCT_EXIT_PROTOCOL_GUID;
 if(!memcmp(G,&Expected,sizeof(*G))){assert(!mExitBootServicesCalled);++locates;*Out=Installed;return Installed?EFI_SUCCESS:EFI_NOT_FOUND;}
 return locate(G,Registration,Out);}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID*G,VOID**Out){assert(H==Child&&G==&gEfiLoadedImageProtocolGuid&&!mExitBootServicesCalled);*Out=&NativeImage.Info;++handles;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI TimerPeriod(EFI_TIMER_ARCH_PROTOCOL*This,UINT64 Period){assert(This==&Timer&&!Period&&Owners.Report.Clean&&Late.Phase==PianoExitClean);++timer_calls;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Disable(EFI_CPU_ARCH_PROTOCOL*This){assert(This==&Cpu&&gMemoryMapTerminated&&Owners.Report.Clean);++interrupt_calls;return EFI_SUCCESS;}
VOID CoreNotifySignalList(CONST EFI_GUID*G){
 if(G==&gEfiEventBeforeExitBootServicesGuid){assert(Owners.Report.Clean&&(Late.RetireCalls==1||(Late.RawMode&&!Late.RetireCalls)));++before_events;if(Late.RawMode)++mMemoryMapKey;}
 else if(G==&gEfiEventExitBootServicesGuid){assert(gMemoryMapTerminated&&Owners.Report.Clean);++exit_events;}
 else if(G==&gEventExitBootServicesFailedGuid){assert(!gMemoryMapTerminated&&before_events==1);++failed_events;}
 else assert(!"unknown Core notification");}
VOID MemoryProtectionExitBootServicesCallback(VOID){assert(gMemoryMapTerminated);}
VOID SaveAndSetDebugTimerInterrupt(BOOLEAN Enabled){assert(!Enabled&&gMemoryMapTerminated);}
VOID CalculateEfiHdrCrc(EFI_TABLE_HEADER*Header){assert(Header==&SystemTable.Hdr&&gMemoryMapTerminated);}
#undef REPORT_STATUS_CODE
#define REPORT_STATUS_CODE(A,B) ((VOID)0)
#include "PianoActualNativeExit.h"
#define SIG SIGNATURE_32('P','L','E','S')
#include "PianoActualInitrdCopy.h"
#undef SIG
static UINTN initrd_slices;static PIANO_LINUX_EFI_SESSION Linux;
static UINT8 Initrd[131073],InitrdCopy[131073];
static EFI_STATUS InitrdSlice(VOID*C,UINTN Budget){assert(C==&Linux&&Budget==1000&&!mExitBootServicesCalled&&!Late.RetireCalls&&
 !Owners.Report.UsbStopped&&Policy.ProtocolInstalled&&UsbState.Phase==PianoUsbServiceListening);++initrd_slices;return EFI_SUCCESS;}
static BOOLEAN InitrdAlive(VOID*C){assert(C==&Linux);return live;}
static EFI_STATUS InitrdValidate(VOID*C,CONST PIANO_LINUX_MEMORY_PROOF*P){assert(C==&Linux&&P->BootEpoch==Late.Memory.BootEpoch);return EFI_SUCCESS;}
static EFI_STATUS InitrdBuffer(VOID*C,CONST PIANO_LINUX_MEMORY_PROOF*P,CONST VOID*Producer,VOID*Owner,CONST VOID*Base,UINT64 Bytes){
 assert(C==&Linux&&P->BootEpoch==Late.Memory.BootEpoch&&Producer==Initrd&&Owner==Initrd&&Base==Initrd&&Bytes==sizeof(Initrd));return EFI_SUCCESS;}
static void Setup(void){
 fresh();if(scenario>=21)without_usb();assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);
 Policy.RequestedCoreAction=scenario>=24?PianoUsbServiceActionBoot:PianoUsbServiceActionContinue;
 if(scenario>=24)assert(PianoProductOwnersRequestFileBoot(&Owners,(VOID *)0x701,(VOID *)0x702)==EFI_SUCCESS);
 else assert(PianoProductOwnersRequestUiAction(&Owners,PianoUsbServiceActionContinue)==EFI_SUCCESS);
 Bs.Hdr.Signature=EFI_BOOT_SERVICES_SIGNATURE;Bs.Hdr.HeaderSize=sizeof(Bs);
 Bs.InstallProtocolInterface=Install;Bs.UninstallProtocolInterface=Uninstall;Bs.HandleProtocol=Handle;Bs.LocateProtocol=Locate;
 SystemTable.BootServices=&Bs;gST=&SystemTable;
 Loaded=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,.ParentHandle=Parent,.SystemTable=&SystemTable,
  .ImageBase=(VOID*)0x500000,.ImageSize=0x4000,.ImageCodeType=EfiLoaderCode,.ImageDataType=EfiLoaderData};
 NativeImage=(LOADED_IMAGE_PRIVATE_DATA){.Signature=LOADED_IMAGE_PRIVATE_DATA_SIGNATURE,.Handle=Child,.Started=TRUE,.Info=Loaded,.ImageContext={EFI_IMAGE_SUBSYSTEM_EFI_APPLICATION}};
 // Actual native accessor and provider see the same real loaded identity.
 Loaded=NativeImage.Info;mCurrentImage=&NativeImage;
 gMemoryMap.ForwardLink=gMemoryMap.BackLink=&gMemoryMap;Timer.SetTimerPeriod=TimerPeriod;Cpu.DisableInterrupt=Disable;
}
int main(int argc,char**argv){assert(argc==2);scenario=strtoul(argv[1],NULL,10);assert(scenario<26);Setup();
 BOOLEAN RawCase=(scenario>=18&&scenario<=20)||scenario>=24;
 PIANO_LATE_HANDOFF_ENV Env={.Context=&Late,.Services=&Bs,.SystemTable=&SystemTable,.ParentImage=Parent,.Owners=&Owners,
  .BootServicesAlive=Alive,.CheckMemory=Memory,.ValidateMemory=Validate,.FailStop=Fail};
 if(RawCase){Env.ParentImage=Child;Env.ValidateRaw=RawValidate;}
 assert(PianoLateHandoffInitialize(&Late,&Env)==EFI_SUCCESS&&Late.Phase==PianoExitUnarmed&&installs==1);
 locates=0;
 if(RawCase){
  RawReport=(PIANO_RAW_LINUX_REPORT){.Revision=1,.Epoch=123,.Image=Child,.Identity=&NativeImage.Info,.Token=(VOID*)0x444};
  assert(PianoLateHandoffArmRaw(&Late,Child,&RawReport)==EFI_ACCESS_DENIED&&!memchecks&&!Calls);
  assert(PianoProductOwnersRetire(&Owners)==EFI_SUCCESS&&Owners.Report.Clean);
  if(scenario>=24)assert(PianoProductOwnersUsbRetired(&Owners)&&!Owners.Report.UsbStopped&&!UsbStatusReads&&
   Owners.Report.UsbStartupFailedClean&&!Owners.Report.BootActionConsumed&&!strcmp(Order,"PXApsIM"));
  PIANO_RAW_LINUX_REPORT Forged=RawReport;
  assert(PianoLateHandoffArmRaw(&Late,Child,&Forged)==EFI_ACCESS_DENIED);
  assert(PianoLateHandoffArmRaw(&Late,Parent,&RawReport)==EFI_ACCESS_DENIED);
  if(scenario==25)Owners.Report.UsbStartupFailedClean=FALSE;
  EFI_STATUS Armed=PianoLateHandoffArmRaw(&Late,Child,&RawReport);
  if(scenario==19||scenario==25){assert(Armed==EFI_ACCESS_DENIED&&!Late.RawMode&&!memchecks);return 0;}
  assert(Armed==EFI_SUCCESS&&Late.RawMode&&!Late.Memory.FullDdr&&!memchecks);
  assert(PianoLateHandoffDisarm(&Late)==EFI_ACCESS_DENIED);
  UINTN Saved=Calls,ProofChecks=StartupValidations;assert(CoreExitBootServices(Child,10)==EFI_INVALID_PARAMETER&&Late.Phase==PianoExitArmed&&!before_events);
  assert(CoreExitBootServices(Child,mMemoryMapKey)==EFI_INVALID_PARAMETER&&Late.Phase==PianoExitClean&&Calls==Saved&&!memchecks&&!Late.RetireCalls);
  if(scenario==20){RawReport.Kernel=0xdead;int Fatal=setjmp(jump);if(!Fatal)CoreExitBootServices(Child,mMemoryMapKey);assert(Fatal&&!gMemoryMapTerminated);return 0;}
  assert(CoreExitBootServices(Child,mMemoryMapKey)==EFI_SUCCESS&&gMemoryMapTerminated&&Calls==Saved&&!memchecks&&!Late.RetireCalls&&StartupValidations==ProofChecks);
  puts("Raw Core EBS: trusted pre-retired report, no full-DDR claim, standard key retry, immutable authority passed");return 0;
 }
 if(scenario==15){PIANO_LATE_HANDOFF Other={0};assert(PianoLateHandoffInitialize(&Other,&Env)==EFI_ALREADY_STARTED&&installs==1&&!Other.Signature);return 0;}
 // Provider snapshots the actual current native loaded object via Handle.
 Loaded=NativeImage.Info;
 if(scenario!=2){EFI_STATUS A=PianoLateHandoffArm(&Late,Child);
  if(scenario==5||scenario==6){assert(A==EFI_NOT_READY&&!Calls&&!timer_calls);return 0;}assert(A==EFI_SUCCESS);}
 // Arm's identity must be identical to the native getter pointer, as on Mu.
 if(scenario==1){mMemoryMapKey=11;assert(CoreExitBootServices(Child,10)==EFI_INVALID_PARAMETER&&!Calls&&!locates&&!before_events&&!timer_calls);return 0;}
 if(scenario==3){assert(CoreExitBootServices((VOID*)0x999,10)==EFI_ACCESS_DENIED&&!Calls&&!before_events&&!timer_calls);return 0;}
 if(scenario==4){gEfiCurrentTpl=Tpl=TPL_CALLBACK;assert(CoreExitBootServices(Child,10)==EFI_UNSUPPORTED&&!Calls&&!before_events&&!timer_calls);return 0;}
 if(scenario==8)UsbStatus=EFI_WARN_STALE_DATA;
 if(scenario==9)ProofValid=FALSE;
 if(scenario==22)StartupValidateStatus=EFI_ACCESS_DENIED;
 if(scenario==10)NativeImage.Started=FALSE;
 if(scenario==11)NativeImage.Info.ImageBase=(VOID*)0x999000;
 if(scenario==12){assert(PianoLateHandoffDisarm(&Late)==EFI_SUCCESS&&!Calls);return 0;}
 if(scenario==16||scenario==17){assert(PianoLateHandoffDisarm(&Late)==EFI_SUCCESS);int Stopped=setjmp(jump);
  if(!Stopped){EFI_STATUS E=PianoLateHandoffShutdown(&Late);assert(scenario==16&&E==EFI_SUCCESS&&!Late.Installed&&!Installed&&uninstalls==1);}
  else assert(scenario==17&&Late.Retained&&Late.Installed&&Installed&&uninstalls==1);return 0;}
 if(scenario==13)Installed=NULL;
 if(scenario==0){
  memset(Initrd,0x5a,sizeof(Initrd));Linux.Signature=SIGNATURE_32('P','L','E','S');Linux.Busy=TRUE;
  Linux.Env.Context=&Linux;Linux.Env.BootServicesAlive=InitrdAlive;
  Linux.HasCpu=TRUE;Linux.Cpu=(PIANO_CPU_INPUT_ENV){.Context=&Linux,.BootServicesAlive=InitrdAlive,.ServiceSlice=InitrdSlice,.ValidateMemory=InitrdValidate,.ValidateBuffer=InitrdBuffer};
  Linux.SourceMemory=Late.Memory;Linux.Sources[2].Context=Initrd;Linux.Sources[2].Bytes=sizeof(Initrd);
  Linux.Views[2]=Initrd;Linux.Owners[2]=Initrd;Linux.Loans[2]=Initrd;
  EFI_DEVICE_PATH_PROTOCOL End={END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};UINTN N=sizeof(InitrdCopy);
  assert(LoadInitrd(&Linux.Load,&End,FALSE,&N,InitrdCopy)==EFI_SUCCESS&&N==sizeof(Initrd)&&!memcmp(Initrd,InitrdCopy,N)&&initrd_slices==4&&!Calls&&!Late.RetireCalls);
 }
 EFI_STATUS Status=EFI_SUCCESS;int Fatal=setjmp(jump);if(!Fatal)Status=CoreExitBootServices(Child,10);
 if(scenario==7||scenario==8||scenario==9||scenario==22){assert(Fatal&&Late.Retained&&!before_events&&!timer_calls);return 0;}
 if(scenario==2||scenario==10||scenario==11||scenario==13){assert(!Fatal&&Status==(scenario==13?EFI_NOT_READY:EFI_ACCESS_DENIED)&&!Calls&&!before_events&&!timer_calls);return 0;}
 assert(!Fatal&&Status==EFI_INVALID_PARAMETER&&Late.RetireCalls==1&&Owners.Report.Clean&&before_events==1&&failed_events==1&&timer_calls==1);
 assert(!strcmp(Order,scenario>=21?"PXApsIM":"PURXApsIM")&&!gMemoryMapTerminated&&mMemoryMapKey==11);
 if(scenario>=21)assert(!UsbStatusReads&&!Owners.Report.UsbStopped&&Owners.Report.UsbStartupFailedClean&&PianoProductOwnersUsbRetired(&Owners));
 UINTN SavedCalls=Calls,SavedLocates=locates,SavedHandles=handles,SavedMemory=memchecks,SavedProofChecks=StartupValidations;
 if(scenario==14)Owners.Report.Clean=FALSE;
 if(scenario==23)StartupProof.TablePhysical+=4096;
 Fatal=setjmp(jump);if(!Fatal)Status=CoreExitBootServices(Child,mMemoryMapKey);
 if(scenario==14||scenario==23){assert(Fatal&&!gMemoryMapTerminated&&Calls==SavedCalls&&locates==SavedLocates&&handles==SavedHandles&&memchecks==SavedMemory&&StartupValidations==SavedProofChecks);return 0;}
 assert(!Fatal&&Status==EFI_SUCCESS&&gMemoryMapTerminated&&gBS==NULL&&SystemTable.BootServices==NULL&&Rt.AtRuntime);
 assert(Late.RetireCalls==1&&Calls==SavedCalls&&locates==SavedLocates&&handles==SavedHandles&&memchecks==SavedMemory&&StartupValidations==SavedProofChecks&&before_events==1&&exit_events==1&&timer_calls==2&&interrupt_calls==1);
 printf("Actual native late EBS case%lu: real Owners retirement before BeforeNotify; stale-key retry has zero lookup/HAL/alloc/checkmemory; no device\n",(unsigned long)scenario);return 0;
}
