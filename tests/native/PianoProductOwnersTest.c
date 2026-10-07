// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual product manager with typed owner/BS fixtures; no device commands.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../../uefi/core/PianoProductOwners.c"
EFI_BOOT_SERVICES *gBS;static EFI_BOOT_SERVICES Bs;
EFI_GUID gEfiEventExitBootServicesGuid={0x27ABF055,0xB1B8,0x4C26,{0x80,0x48,0x74,0x8F,0x37,0xBA,0xA2,0xDF}};
static PIANO_PRODUCT_OWNERS Owners;static PIANO_PRODUCT_OWNERS_CONFIG Config;
static PIANO_BOOT_POLICY_REPORT Policy;static PIANO_DWC3_SERVICE_STATUS UsbState;
static PIANO_USB_SERVICE_RETIRE_REPORT UsbReport;static PIANO_UFS_RESET_REPORT UfsReport;
static PIANO_PRODUCT_INPUT_RETIRE_REPORT InputReport;static PIANO_FB_STORAGE Storage;
static PIANO_PRODUCT_DISPLAY_RETIRE_REPORT DisplayReport;
static EFI_STATUS DisplayStatus;static BOOLEAN DisplayEbs,DisplayReentry,DisplayRaiseEbs,DisplayPostRaiseEbs,DisplayBadTpl,InputBadTpl;
static PIANO_PRODUCT_RUNTIME_PROTOCOL Runtime;static EFI_TPL Tpl;
static EFI_STATUS PolicyStatus,UsbStatus,ProofStatus,AcceptStatus,PrepareStatus,ShutdownStatus,InputStatus,EventStatus,RequestStatus,TakeStatus;
static BOOLEAN BridgeLive,ProofValid,ReportAbsent,FreshReplaced,CreateWarning,CreateNull,CreateEbs,InputEbs;
static UINTN Requests,Calls;static CHAR8 Order[100];static PIANO_FB_BOOT_ACTION BootAction;
static PIANO_SMMU_RETIRED_USB_PROOF StartupProof,ProducerStartupProof;
static EFI_STATUS StartupValidateStatus;static UINTN StartupValidations,StartupFailAt,UsbStatusReads;
static VOID record(CHAR8 Step){assert(Calls<sizeof(Order)-1);Order[Calls++]=Step;Order[Calls]=0;}
VOID *EFIAPI ZeroMem(VOID *Buffer,UINTN Bytes){return memset(Buffer,0,Bytes);}
#ifndef PIANO_PRODUCT_NATIVE_LATE
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
#endif
static EFI_TPL EFIAPI raise(EFI_TPL New){assert(New>=Tpl);EFI_TPL Old=Tpl;Tpl=New;if((DisplayRaiseEbs&&Owners.Report.InputStopped&&New==TPL_HIGH_LEVEL)||(DisplayPostRaiseEbs&&Owners.Report.DisplayStopped&&New==TPL_HIGH_LEVEL)){PianoProductOwnersFenceExit(&Owners);gBS=(VOID *)1;}return Old;}
static VOID EFIAPI restore(EFI_TPL Old){assert(Old<=Tpl);Tpl=Old;}
static EFI_STATUS EFIAPI locate(EFI_GUID *Guid,VOID *Registration,VOID **Interface){(void)Registration;EFI_GUID Expected=PIANO_PRODUCT_RUNTIME_PROTOCOL_GUID;assert(!memcmp(Guid,&Expected,sizeof(Expected)));*Interface=FreshReplaced?(VOID *)123:&Runtime;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI create(UINT32 Type,EFI_TPL Level,EFI_EVENT_NOTIFY Notify,CONST VOID *Context,CONST EFI_GUID *Guid,EFI_EVENT *Event){
  assert(Tpl==TPL_APPLICATION && Type==EVT_NOTIFY_SIGNAL && Level==TPL_NOTIFY && Context==&Owners && Guid==&gEfiEventExitBootServicesGuid);
  *Event=CreateNull?NULL:(VOID *)0x987;if(CreateEbs)Notify(*Event,(VOID *)Context);return CreateWarning?EFI_WARN_STALE_DATA:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI close(EFI_EVENT Event){assert(Tpl==TPL_APPLICATION && Event==(VOID *)0x987);record('M');return EventStatus;}
static EFI_STATUS EFIAPI request(PIANO_PRODUCT_RUNTIME_PROTOCOL *This,UINT32 Action){assert(This==&Runtime && Action==PIANO_PRODUCT_ACTION_RETURN_CORE && Tpl==TPL_APPLICATION);++Requests;return RequestStatus;}
CONST PIANO_BOOT_POLICY_REPORT *PianoBootPolicyReport(VOID){return &Policy;}
EFI_STATUS PianoBootPolicyStop(VOID){assert(Tpl==TPL_APPLICATION && !Policy.Dispatching && !Policy.Pumping);record('P');if(PolicyStatus==EFI_SUCCESS)Policy.ProtocolInstalled=FALSE;return PolicyStatus;}
EFI_STATUS PianoUsbControllerServiceGetStatus(PIANO_DWC3_SERVICE_STATUS *Status){++UsbStatusReads;*Status=UsbState;return EFI_SUCCESS;}
EFI_STATUS PianoUsbControllerValidateStartupFailureProof(CONST VOID *Fdt,CONST PIANO_SMMU_RETIRED_USB_PROOF *Proof){
  assert(Fdt==(VOID *)0x123&&Tpl==TPL_APPLICATION);++StartupValidations;
  if(Proof!=&StartupProof||memcmp(Proof,&ProducerStartupProof,sizeof(*Proof))||UsbState.Started||UsbState.Retained||UsbState.ServicesLost)return EFI_ACCESS_DENIED;
  return StartupFailAt&&StartupValidations>=StartupFailAt?EFI_ACCESS_DENIED:StartupValidateStatus;
}
EFI_STATUS PianoUsbControllerServiceStop(EFI_STATUS Reason,PIANO_USB_SERVICE_RETIRE_REPORT *Report){assert(Tpl==TPL_APPLICATION && !Policy.ProtocolInstalled && Reason==EFI_SUCCESS);record('U');*Report=UsbReport;return UsbStatus;}
EFI_STATUS PianoDwc3ConsumeBootAction(PIANO_FB_BOOT_ACTION *Action){assert(Calls && Order[Calls-1]=='U');record('B');*Action=BootAction;return TakeStatus;}
EFI_STATUS PianoUsbControllerMakeRetiredUsbProof(CONST VOID *Fdt,PIANO_SMMU_RETIRED_USB_PROOF *Proof){assert(Fdt==(VOID *)0x123 && Tpl==TPL_APPLICATION && UsbStatus==EFI_SUCCESS && UsbReport.Clean);record('R');Proof->Valid=ProofValid;return ProofStatus;}
static EFI_STATUS ready(VOID *Context){assert(Context==(VOID *)0x456 && BridgeLive && Tpl==TPL_APPLICATION);return EFI_SUCCESS;}
static EFI_STATUS info(VOID *Context,CONST CHAR8 *Name,PIANO_FB_PARTITION_INFO *Result){(void)Context;(void)Name;(void)Result;assert(!"manager must not read partition metadata");return EFI_UNSUPPORTED;}
static EFI_STATUS read_blocks(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info,UINT64 Lba,UINTN Bytes,VOID *Buffer){(void)Context;(void)Info;(void)Lba;(void)Bytes;(void)Buffer;assert(!"manager must not issue UFS I/O");return EFI_UNSUPPORTED;}
CONST PIANO_FB_STORAGE *PianoFastbootBlockReadStorage(VOID){return BridgeLive?&Storage:NULL;}
VOID PianoFastbootBlockReadStop(VOID){assert(Tpl==TPL_APPLICATION && Calls && (Order[Calls-1]=='R'||
  (Order[Calls-1]=='P'&&Owners.Report.UsbStartupFailedClean&&!Owners.Report.UsbStopped&&Owners.Report.ProofStatus==EFI_SUCCESS)));record('X');BridgeLive=FALSE;}
EFI_STATUS PianoUfsAcceptRetiredUsb(CONST PIANO_SMMU_RETIRED_USB_PROOF *Proof){assert(Proof==&Owners.UsbProof && Proof->Valid && Tpl==TPL_APPLICATION && !BridgeLive);record('A');return AcceptStatus;}
EFI_STATUS PianoUfsBlockIoPrepareForReset(VOID){assert(Tpl==TPL_CALLBACK && !BridgeLive && AcceptStatus==EFI_SUCCESS);record('p');raise(TPL_CALLBACK);return PrepareStatus;}
EFI_STATUS PianoUfsBlockIoShutdownForReset(VOID){assert(Tpl==TPL_CALLBACK && PrepareStatus==EFI_SUCCESS);record('s');return ShutdownStatus;}
CONST PIANO_UFS_RESET_REPORT *PianoUfsResetShutdownReport(VOID){return ReportAbsent?NULL:&UfsReport;}
static EFI_STATUS stop_input(VOID *Context,PIANO_PRODUCT_INPUT_RETIRE_REPORT *Report){assert(Context==(VOID *)0x789 && Tpl==TPL_APPLICATION && Owners.Report.UfsStopped && UfsReport.Clean && !Policy.ProtocolInstalled);record('I');*Report=InputReport;if(InputEbs)PianoProductOwnersFenceExit(&Owners);if(InputBadTpl)Tpl=TPL_CALLBACK;return InputStatus;}
static EFI_STATUS stop_display(VOID *Context,PIANO_PRODUCT_DISPLAY_RETIRE_REPORT *Report){
  assert(Context==(VOID *)0x999&&Tpl==TPL_APPLICATION&&Owners.Report.InputStopped&&Owners.Report.UfsStopped&&!Owners.Report.OuterTplHeld&&Calls&&Order[Calls-1]=='I');record('D');
  if(DisplayReentry)assert(PianoProductOwnersRetire(&Owners)==EFI_ALREADY_STARTED);
  *Report=DisplayReport;if(DisplayEbs){PianoProductOwnersFenceExit(&Owners);gBS=(VOID *)1;}if(DisplayBadTpl)Tpl=TPL_CALLBACK;return DisplayStatus;
}
static VOID fresh(VOID) {
  memset(&Owners,0,sizeof(Owners));memset(&Config,0,sizeof(Config));memset(&Policy,0,sizeof(Policy));memset(&UsbState,0,sizeof(UsbState));memset(&UsbReport,0,sizeof(UsbReport));memset(&UfsReport,0,sizeof(UfsReport));memset(&InputReport,0,sizeof(InputReport));memset(&BootAction,0,sizeof(BootAction));
  Tpl=TPL_APPLICATION;Requests=Calls=0;Order[0]=0;BridgeLive=ProofValid=TRUE;ReportAbsent=FreshReplaced=CreateWarning=CreateNull=CreateEbs=InputEbs=FALSE;
  memset(&DisplayReport,0,sizeof(DisplayReport));DisplayStatus=EFI_SUCCESS;DisplayEbs=DisplayReentry=DisplayRaiseEbs=DisplayPostRaiseEbs=DisplayBadTpl=InputBadTpl=FALSE;
  PolicyStatus=UsbStatus=ProofStatus=AcceptStatus=PrepareStatus=ShutdownStatus=InputStatus=EventStatus=RequestStatus=TakeStatus=EFI_SUCCESS;
  memset(&StartupProof,0,sizeof(StartupProof));memset(&ProducerStartupProof,0,sizeof(ProducerStartupProof));
  StartupValidateStatus=EFI_SUCCESS;StartupValidations=StartupFailAt=UsbStatusReads=0;
  Policy.Initialized=Policy.ProtocolInstalled=TRUE;
  UsbState=(PIANO_DWC3_SERVICE_STATUS){.Revision=1,.Started=TRUE,.Phase=PianoUsbServiceListening};
  UsbReport=(PIANO_USB_SERVICE_RETIRE_REPORT){.Revision=1,.Attempted=TRUE,.Clean=TRUE,.DeviceHalted=TRUE,.DmaFreed=TRUE,.DomainFreed=TRUE,.ClocksReleased=TRUE,.DmaBuffersFreed=9,.ClockReleaseMask=255};
  UfsReport=(PIANO_UFS_RESET_REPORT){.Started=TRUE,.Prepared=TRUE,.Returned=TRUE,.Clean=TRUE,.TplHeld=TRUE,.DmaFreed=3,.Disconnected=6,.ProtocolsRemoved=7};
  InputReport=(PIANO_PRODUCT_INPUT_RETIRE_REPORT){.Revision=1,.Started=TRUE,.Returned=TRUE,.Clean=TRUE};
  BootAction=(PIANO_FB_BOOT_ACTION){.Context=(VOID *)0x1122,.Token=(VOID *)0x3344,.Taken=TRUE,.Proof={.AckCompleted=TRUE,.QueueEmpty=TRUE,.DeviceHalted=TRUE,.DmaFreed=TRUE,.DispatchFrozen=TRUE,.AckBytes=4,.DmaBuffersFreed=9}};
  Storage=(PIANO_FB_STORAGE){(VOID *)0x456,ready,info,read_blocks};Runtime=(PIANO_PRODUCT_RUNTIME_PROTOCOL){.Revision=PIANO_PRODUCT_RUNTIME_REVISION,.RequestAction=request};
  Config=(PIANO_PRODUCT_OWNERS_CONFIG){.Revision=1,.Fdt=(VOID *)0x123,.ExpectedOwnerMask=PIANO_OWNER_ALL_MASK,.StartedOwnerMask=PIANO_OWNER_CORE_MASK,.AbsentOwnerMask=PIANO_OWNER_USB_HOST|PIANO_OWNER_GPI|PIANO_OWNER_POGO|PIANO_OWNER_DISPLAY,.Runtime=&Runtime,.InputContext=(VOID *)0x789,.StopInput=stop_input,.DisplayStartup={.Revision=1,.KnownNoSideEffects=TRUE,.Status=EFI_NOT_STARTED}};
  Bs.RaiseTPL=raise;Bs.RestoreTPL=restore;Bs.CreateEventEx=create;Bs.CloseEvent=close;Bs.LocateProtocol=locate;gBS=&Bs;
}
static VOID without_usb(VOID){
  Config.StartedOwnerMask&=~PIANO_OWNER_USB;Config.AbsentOwnerMask|=PIANO_OWNER_USB;
  StartupProof=(PIANO_SMMU_RETIRED_USB_PROOF){.Revision=PIANO_SMMU_USB_RETIRE_REVISION,.Valid=TRUE,.UsbContext=(VOID *)0x811,
    .Execution={.Revision=PIANO_SMMU_USB_RETIRE_REVISION,.DeviceHalted=TRUE,.DmaFreed=TRUE,.ClocksReleased=TRUE,.GdscReleased=TRUE,
      .ClockReleaseMask=255,.Kind=PIANO_USB_RETIRE_STARTUP_FAILED_CLEAN,.StartupStatus=EFI_TIMEOUT}};
  ProducerStartupProof=StartupProof;Config.StartupUsbProof=&StartupProof;
  UsbState=(PIANO_DWC3_SERVICE_STATUS){.Revision=1,.Phase=PianoUsbServiceOff};
}
static VOID with_display(VOID){
  Config.StartedOwnerMask|=PIANO_OWNER_DISPLAY;Config.AbsentOwnerMask&=~PIANO_OWNER_DISPLAY;
  Config.DisplayContext=(VOID *)0x999;Config.StopDisplay=stop_display;
  Config.DisplayStartup=(PIANO_PRODUCT_DISPLAY_STARTUP_REPORT){.Revision=1,.LeaseContext=(VOID *)0x999,.ClockId=0x10001,.NativeBase=0xcf100000,
    .AcquireAttempted=TRUE,.Held=TRUE,.OwnedReferences=1,.AcquireBeforeSnapshots=2,.AcquireAfterSnapshots=2,
    .AcquireBeforeTotal={5,2},.AcquireAfterTotal={6,2},.AcquireBeforeClient={2,1},.AcquireAfterClient={3,1}};
  DisplayReport=(PIANO_PRODUCT_DISPLAY_RETIRE_REPORT){.Revision=1,.LeaseContext=(VOID *)0x999,.ClockId=0x10001,.NativeBase=0xcf100000,
    .Started=TRUE,.Returned=TRUE,.Clean=TRUE,.ReleaseAttempted=TRUE,.Released=TRUE,.ExitClosed=TRUE,.OwnedReferencesBefore=1,
    .GccReads=4,.GccPages=1,.AcquireBeforeSnapshots=2,.AcquireAfterSnapshots=2,.ReleaseBeforeSnapshots=2,.ReleaseAfterSnapshots=2,
    .AcquireBeforeTotal={5,2},.AcquireAfterTotal={6,2},.AcquireBeforeClient={2,1},.AcquireAfterClient={3,1},
    // Other legitimate references changed after acquisition, including the
    // alternate bank. Release removes only this ordinary owned reference.
    .ReleaseBeforeTotal={10,4},.ReleaseAfterTotal={9,4},.ReleaseBeforeClient={7,3},.ReleaseAfterClient={6,3}};
}
static VOID initialize_action(PIANO_USB_SERVICE_ACTION Action) {
  assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);
  UsbState.Phase=PianoUsbServiceStopRequested;UsbState.Action=Action;
  assert(PianoProductOwnersObserveUsbAction(&Owners)==EFI_SUCCESS && Requests==1 && !Calls && Owners.Report.RequestedAction==Action);
}
static VOID must_retain(VOID) {
  assert(PianoProductOwnersRetire(&Owners)!=EFI_SUCCESS && Owners.Report.Retained && !Owners.Report.Clean && Owners.Report.AllowedAction==PianoUsbServiceActionNone);
  UINTN Before=Calls;assert(PianoProductOwnersRetire(&Owners)==EFI_ACCESS_DENIED && Calls==Before);
}
int main(void) {
  UINTN Cases=0;
  for(UINTN I=PianoUsbServiceActionContinue;I<=PianoUsbServiceActionFault;++I){fresh();initialize_action((PIANO_USB_SERVICE_ACTION)I);assert(PianoProductOwnersRetire(&Owners)==EFI_SUCCESS && Owners.Report.Clean && !Owners.Report.Retained && Tpl==TPL_APPLICATION && !Owners.ExitEvent && Owners.Report.RetiredMask==PIANO_OWNER_CORE_MASK);assert(!strcmp(Order,I==PianoUsbServiceActionBoot?"PUBRXApsIM":"PURXApsIM"));assert(Owners.Report.AllowedAction==(I==PianoUsbServiceActionFault?PianoUsbServiceActionNone:I));UINTN Before=Calls;assert(PianoProductOwnersRetire(&Owners)==EFI_ACCESS_DENIED && Calls==Before);++Cases;}
  for(UINTN I=0;I<8;++I){fresh();if(I==0)Config.ExpectedOwnerMask=PIANO_OWNER_CORE_MASK;if(I==1)Config.AbsentOwnerMask&=~PIANO_OWNER_GPI;if(I==2)Config.StartedOwnerMask|=PIANO_OWNER_GPI;if(I==3){Config.StartedOwnerMask|=PIANO_OWNER_GPI;Config.AbsentOwnerMask&=~PIANO_OWNER_GPI;}if(I==4){Config.StartedOwnerMask&=~PIANO_OWNER_USB;Config.AbsentOwnerMask|=PIANO_OWNER_USB;}if(I==5)UsbState.Retained=TRUE;if(I==6)Policy.ServicesLost=TRUE;if(I==7)BridgeLive=FALSE;assert(PianoProductOwnersInitialize(&Owners,&Config)!=EFI_SUCCESS && !Calls && !Owners.Report.Initialized);++Cases;}
  for(UINTN I=0;I<3;++I){fresh();if(I==0)CreateWarning=TRUE;if(I==1)CreateNull=TRUE;if(I==2)CreateEbs=TRUE;assert(PianoProductOwnersInitialize(&Owners,&Config)!=EFI_SUCCESS && Owners.Report.Retained && !Calls);++Cases;}
  fresh();initialize_action(PianoUsbServiceActionReboot);Policy.Dispatching=TRUE;assert(PianoProductOwnersRetire(&Owners)==EFI_NOT_READY && !Calls && !Owners.Report.Retained);Policy.Dispatching=FALSE;Policy.Pumping=TRUE;assert(PianoProductOwnersRetire(&Owners)==EFI_NOT_READY && !Calls);Policy.Pumping=FALSE;assert(PianoProductOwnersRetire(&Owners)==EFI_SUCCESS);++Cases;
  for(UINTN I=0;I<2;++I){fresh();assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);UsbState.Phase=PianoUsbServiceStopRequested;UsbState.Action=PianoUsbServiceActionReboot;if(I==0)FreshReplaced=TRUE;else RequestStatus=EFI_WARN_STALE_DATA;assert(PianoProductOwnersObserveUsbAction(&Owners)!=EFI_SUCCESS && Owners.Report.Retained && !Calls);++Cases;}
  for(UINTN I=0;I<8;++I)for(UINTN Warning=0;Warning<2;++Warning){fresh();initialize_action(PianoUsbServiceActionReboot);EFI_STATUS Bad=Warning?EFI_WARN_STALE_DATA:EFI_DEVICE_ERROR;EFI_STATUS *Stage[]={&PolicyStatus,&UsbStatus,&ProofStatus,&AcceptStatus,&PrepareStatus,&ShutdownStatus,&InputStatus,&EventStatus};*Stage[I]=Bad;must_retain();if(I<=3)assert(Tpl==TPL_APPLICATION);if(I==4 || I==5)assert(Tpl==TPL_CALLBACK && Owners.Report.OuterTplHeld);if(I>=6)assert(Tpl==TPL_APPLICATION && Owners.Report.UfsStopped);++Cases;}
  for(UINTN I=0;I<12;++I){fresh();initialize_action(PianoUsbServiceActionReboot);switch(I){case 0:UsbReport.Revision=0;break;case 1:UsbReport.Attempted=FALSE;break;case 2:UsbReport.Clean=FALSE;break;case 3:UsbReport.Retained=TRUE;break;case 4:UsbReport.DeviceHalted=FALSE;break;case 5:UsbReport.DmaFreed=FALSE;break;case 6:UsbReport.DomainFreed=FALSE;break;case 7:UsbReport.ClocksReleased=FALSE;break;case 8:UsbReport.DmaBuffersFreed=8;break;case 9:UsbReport.ClockReleaseMask=254;break;case 10:UsbReport.OwnedCloseStatus=EFI_WARN_STALE_DATA;break;case 11:UsbReport.TimerCloseStatus=EFI_WARN_STALE_DATA;break;}must_retain();assert(!strcmp(Order,"PU"));++Cases;}
  for(UINTN I=0;I<24;++I){fresh();initialize_action(PianoUsbServiceActionReboot);switch(I){case 0:UfsReport.Started=FALSE;break;case 1:UfsReport.Prepared=FALSE;break;case 2:UfsReport.Returned=FALSE;break;case 3:UfsReport.Clean=FALSE;break;case 4:UfsReport.Failed=TRUE;break;case 5:UfsReport.TplHeld=FALSE;break;case 6:UfsReport.Result=EFI_WARN_STALE_DATA;break;case 7:UfsReport.Disconnect=EFI_WARN_STALE_DATA;break;case 8:UfsReport.Halt=EFI_WARN_STALE_DATA;break;case 9:UfsReport.Bases=EFI_WARN_STALE_DATA;break;case 10:UfsReport.Dma=EFI_WARN_STALE_DATA;break;case 11:UfsReport.Domain=EFI_WARN_STALE_DATA;break;case 12:UfsReport.Protocols=EFI_WARN_STALE_DATA;break;case 13:UfsReport.Clocks=EFI_WARN_STALE_DATA;break;case 14:UfsReport.DmaFreed=2;break;case 15:UfsReport.Disconnected=0;break;case 16:UfsReport.ProtocolsRemoved=6;break;case 17:UfsReport.TransferDoorbell=1;break;case 18:UfsReport.TaskDoorbell=1;break;case 19:UfsReport.TransferRun=1;break;case 20:UfsReport.TaskRun=1;break;case 21:UfsReport.Interrupt=1;break;case 22:ReportAbsent=TRUE;break;case 23:ProofValid=FALSE;break;}must_retain();assert(!Owners.Report.InputStopped && (I==23 || (Tpl==TPL_CALLBACK && Owners.Report.OuterTplHeld)));++Cases;}
  for(UINTN I=0;I<8;++I){fresh();initialize_action(PianoUsbServiceActionReboot);switch(I){case 0:InputReport.Revision=0;break;case 1:InputReport.Started=FALSE;break;case 2:InputReport.Returned=FALSE;break;case 3:InputReport.Clean=FALSE;break;case 4:InputReport.Retained=TRUE;break;case 5:InputReport.Timer=EFI_WARN_STALE_DATA;break;case 6:InputReport.Protocols=EFI_WARN_STALE_DATA;break;case 7:InputEbs=TRUE;break;}must_retain();assert(Tpl==TPL_APPLICATION && Owners.Report.UfsStopped && !Owners.Report.ManagerEventClosed);++Cases;}
  fresh();initialize_action(PianoUsbServiceActionBoot);UsbStatus=EFI_DEVICE_ERROR;TakeStatus=EFI_DEVICE_ERROR;BootAction.Retained=TRUE;BootAction.Taken=FALSE;must_retain();assert(Owners.Report.BootActionConsumed && Owners.Report.Boot.Token==(VOID *)0x3344 && !strcmp(Order,"PUB"));++Cases;
  fresh();initialize_action(PianoUsbServiceActionBoot);BootAction.Proof.AckBytes=0;must_retain();assert(Owners.Report.Boot.Token==(VOID *)0x3344 && !strcmp(Order,"PUB"));++Cases;
  fresh();initialize_action(PianoUsbServiceActionReboot);PianoProductOwnersFenceExit(&Owners);UINTN Before=Calls;assert(PianoProductOwnersRetire(&Owners)==EFI_ACCESS_DENIED && Calls==Before && Owners.Report.ServicesLost);++Cases;
  fresh();assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);
  assert(PianoProductOwnersRequestUiReboot(&Owners)==EFI_NOT_READY && !Calls && !Requests && Owners.Report.Origin==PianoProductRequestNone);
  Policy.RequestedCoreAction=PianoUsbServiceActionReboot;Policy.Dispatching=TRUE;
  assert(PianoProductOwnersRequestUiReboot(&Owners)==EFI_NOT_READY && !Calls);Policy.Dispatching=FALSE;Policy.Pumping=TRUE;
  assert(PianoProductOwnersRequestUiReboot(&Owners)==EFI_NOT_READY && !Calls);Policy.Pumping=FALSE;Policy.ActiveAction=PIANO_PRODUCT_ACTION_SETUP;
  assert(PianoProductOwnersRequestUiReboot(&Owners)==EFI_NOT_READY && !Calls);Policy.ActiveAction=PIANO_PRODUCT_ACTION_NONE;
  assert(PianoProductOwnersRequestUiReboot(&Owners)==EFI_SUCCESS && !Calls && !Requests && Owners.Report.Origin==PianoProductRequestUi && UsbState.Phase==PianoUsbServiceListening);
  assert(PianoProductOwnersRetire(&Owners)==EFI_SUCCESS && Owners.Report.AllowedAction==PianoUsbServiceActionReboot && !strcmp(Order,"PURXApsIM") && !Owners.Report.BootActionConsumed);++Cases;
  for(UINTN I=0;I<4;++I){fresh();assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);Policy.RequestedCoreAction=PianoUsbServiceActionReboot;
    if(I==0){UsbState.Retained=TRUE;}if(I==1){UsbState.Phase=PianoUsbServiceStopRequested;}if(I==2){UsbState.Action=PianoUsbServiceActionReboot;}if(I==3){FreshReplaced=TRUE;}
    assert(PianoProductOwnersRequestUiReboot(&Owners)!=EFI_SUCCESS && Owners.Report.Retained && !Calls && !Requests);++Cases;}
  for(UINTN I=0;I<3;++I){fresh();assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);Policy.RequestedCoreAction=PianoUsbServiceActionReboot;
    assert(PianoProductOwnersRequestUiReboot(&Owners)==EFI_SUCCESS);if(I==0)Policy.RequestedCoreAction=PianoUsbServiceActionNone;
    if(I==1){UsbState.Phase=PianoUsbServiceStopRequested;UsbState.Action=PianoUsbServiceActionReboot;}if(I==2)UsbStatus=EFI_WARN_STALE_DATA;
    must_retain();assert(Owners.Report.AllowedAction==PianoUsbServiceActionNone && (I<2?!Calls:!strcmp(Order,"PU")));++Cases;}
  fresh();initialize_action(PianoUsbServiceActionReboot);Policy.RequestedCoreAction=PianoUsbServiceActionReboot;
  assert(PianoProductOwnersRequestUiReboot(&Owners)!=EFI_SUCCESS && Owners.Report.Retained && !Calls && Owners.Report.Origin==PianoProductRequestUsb);++Cases;
  fresh();assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);
  assert(PianoProductOwnersRequestUiAction(&Owners,PianoUsbServiceActionBoot)==EFI_UNSUPPORTED && !Calls && !Requests);
  assert(PianoProductOwnersRequestUiAction(&Owners,PianoUsbServiceActionContinue)==EFI_NOT_READY && !Calls);
  Policy.RequestedCoreAction=PianoUsbServiceActionContinue;
  assert(PianoProductOwnersRequestUiReboot(&Owners)==EFI_NOT_READY && !Calls);
  assert(PianoProductOwnersRequestUiAction(&Owners,PianoUsbServiceActionContinue)==EFI_SUCCESS && !Calls && !Requests);
  assert(PianoProductOwnersRequestUiAction(&Owners,PianoUsbServiceActionContinue)==EFI_SUCCESS && !Calls);
  assert(PianoProductOwnersRetire(&Owners)==EFI_SUCCESS && Owners.Report.AllowedAction==PianoUsbServiceActionContinue && !strcmp(Order,"PURXApsIM") && !Owners.Report.BootActionConsumed);++Cases;
  for(UINTN I=0;I<4;++I){fresh();assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);Policy.RequestedCoreAction=PianoUsbServiceActionContinue;
    assert(PianoProductOwnersRequestUiAction(&Owners,PianoUsbServiceActionContinue)==EFI_SUCCESS);
    if(I==0)Policy.RequestedCoreAction=PianoUsbServiceActionReboot;
    if(I==1){UsbState.Phase=PianoUsbServiceStopRequested;UsbState.Action=PianoUsbServiceActionReboot;}
    if(I==2)UsbStatus=EFI_WARN_STALE_DATA;
    if(I==3){Policy.RequestedCoreAction=PianoUsbServiceActionReboot;assert(PianoProductOwnersRequestUiAction(&Owners,PianoUsbServiceActionReboot)==EFI_COMPROMISED_DATA);}
    must_retain();assert(Owners.Report.AllowedAction==PianoUsbServiceActionNone);++Cases;}
  fresh();assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);Policy.RequestedCoreAction=PianoUsbServiceActionContinue;
  assert(PianoProductOwnersResolveReturnedAction(&Owners)==EFI_SUCCESS && Owners.Report.Origin==PianoProductRequestUi && Owners.Report.RequestedAction==PianoUsbServiceActionContinue);
  assert(PianoProductOwnersRetire(&Owners)==EFI_SUCCESS && Owners.Report.AllowedAction==PianoUsbServiceActionContinue);++Cases;
  for(UINTN I=PianoUsbServiceActionContinue;I<=PianoUsbServiceActionBoot;++I){fresh();assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);Policy.RequestedCoreAction=PianoUsbServiceActionContinue;
    UsbState.Phase=PianoUsbServiceStopRequested;UsbState.Action=(PIANO_USB_SERVICE_ACTION)I;
    assert(PianoProductOwnersResolveReturnedAction(&Owners)==EFI_SUCCESS && Owners.Report.Origin==PianoProductRequestUsb && Owners.Report.RequestedAction==I && Requests==1);
    assert(PianoProductOwnersRetire(&Owners)==EFI_SUCCESS && Owners.Report.AllowedAction==I);++Cases;}
  fresh();assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);Policy.RequestedCoreAction=PianoUsbServiceActionContinue;UsbState.Revision=0;
  assert(PianoProductOwnersResolveReturnedAction(&Owners)==EFI_COMPROMISED_DATA && Owners.Report.Retained && !Calls);++Cases;
  assert(Cases==100); // Preserve every existing workflow/regression above.
  for(UINTN I=PianoUsbServiceActionContinue;I<=PianoUsbServiceActionFault;++I){fresh();with_display();DisplayReentry=TRUE;initialize_action((PIANO_USB_SERVICE_ACTION)I);
    assert(PianoProductOwnersRetire(&Owners)==EFI_SUCCESS&&Owners.Report.DisplayStopped&&Owners.Report.Clean&&Owners.Report.RetiredMask==PIANO_OWNER_SUPPORTED_MASK);
    assert(!strcmp(Order,I==PianoUsbServiceActionBoot?"PUBRXApsIDM":"PURXApsIDM")&&Tpl==TPL_APPLICATION&&Owners.Report.Display.ReleaseAfterTotal[0]==9);
    UINTN C=Calls;assert(PianoProductOwnersRetire(&Owners)==EFI_ACCESS_DENIED&&Calls==C);Cases++;}
  for(UINTN I=0;I<14;++I){fresh();with_display();switch(I){
    case 0:Config.StopDisplay=NULL;break;case 1:Config.DisplayStartup.Revision=0;break;
    case 2:Config.DisplayStartup.Held=FALSE;break;case 3:Config.DisplayStartup.OwnedReferences=0;break;
    case 4:Config.DisplayStartup.Retained=TRUE;break;case 5:Config.DisplayStartup.ServicesLost=TRUE;break;
    case 6:Config.DisplayStartup.Status=EFI_WARN_STALE_DATA;break;case 7:Config.DisplayStartup.LeaseContext=(VOID *)3;break;
    case 8:Config.DisplayStartup.AcquireAfterTotal[0]=7;break;case 9:Config.DisplayStartup.AcquireAfterClient[1]=2;break;
    case 10:Config.DisplayStartup.AcquireAfterSnapshots=0;break;case 11:Config.AbsentOwnerMask|=PIANO_OWNER_DISPLAY;break;
    case 12:Config.DisplayStartup.NativeBase=0;break;case 13:Config.DisplayStartup.Held=2;break;}
    assert(PianoProductOwnersInitialize(&Owners,&Config)!=EFI_SUCCESS&&!Calls&&!Owners.Report.Initialized);Cases++;}
  for(UINTN I=0;I<5;++I){fresh();switch(I){case 0:Config.AbsentOwnerMask&=~PIANO_OWNER_DISPLAY;break;
    case 1:Config.DisplayStartup.KnownNoSideEffects=FALSE;break;case 2:Config.DisplayStartup.AcquireAttempted=TRUE;break;
    case 3:Config.DisplayStartup.Held=TRUE;break;case 4:Config.DisplayStartup.Status=EFI_WARN_STALE_DATA;break;}
    assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_NOT_READY&&!Calls);Cases++;}
  for(UINTN I=0;I<36;++I){fresh();with_display();initialize_action(PianoUsbServiceActionReboot);switch(I){
    case 0:DisplayReport.Revision=0;break;case 1:DisplayReport.Started=FALSE;break;case 2:DisplayReport.Returned=FALSE;break;
    case 3:DisplayReport.Clean=FALSE;break;case 4:DisplayReport.Retained=TRUE;break;case 5:DisplayReport.ServicesLost=TRUE;break;
    case 6:DisplayReport.Status=EFI_WARN_STALE_DATA;break;case 7:DisplayReport.Release=EFI_WARN_STALE_DATA;break;
    case 8:DisplayReport.CounterStatus=EFI_WARN_STALE_DATA;break;case 9:DisplayReport.GccReadback=EFI_WARN_STALE_DATA;break;
    case 10:DisplayReport.GccReadbackEnd=EFI_WARN_STALE_DATA;break;case 11:DisplayReport.Cleanup=EFI_WARN_STALE_DATA;break;
    case 12:DisplayReport.GccReads=0;break;case 13:DisplayReport.GccPages=0;break;case 14:DisplayReport.ReleaseAttempted=FALSE;break;
    case 15:DisplayReport.HeldAfter=TRUE;break;case 16:DisplayReport.Released=FALSE;break;case 17:DisplayReport.ExitClosed=FALSE;break;
    case 18:DisplayReport.OwnedReferencesBefore=0;break;case 19:DisplayReport.OwnedReferencesAfter=1;break;
    case 20:DisplayReport.LeaseContext=(VOID *)3;break;case 21:DisplayReport.ClockId++;break;case 22:DisplayReport.NativeBase+=4096;break;
    case 23:DisplayReport.AcquireBeforeSnapshots=1;break;case 24:DisplayReport.ReleaseAfterSnapshots=0;break;
    case 25:DisplayReport.AcquireAfterTotal[0]++;break;case 26:DisplayReport.AcquireAfterClient[1]++;break;
    case 27:DisplayReport.ReleaseAfterTotal[0]++;break;case 28:DisplayReport.ReleaseAfterClient[0]-- ;break;
    case 29:DisplayReport.ReleaseAfterTotal[1]++;break;case 30:DisplayReport.ReleaseAfterClient[1]++;break;
    case 31:DisplayReport.ReleaseBeforeTotal[0]=DisplayReport.ReleaseAfterTotal[0]=0;break;
    case 32:DisplayReport.Returned=2;break;case 33:DisplayStatus=EFI_WARN_STALE_DATA;break;
    case 34:DisplayStatus=EFI_DEVICE_ERROR;break;case 35:DisplayEbs=TRUE;break;}
    must_retain();assert(!strcmp(Order,"PURXApsID")&&!Owners.Report.ManagerEventClosed&&!Owners.Report.DisplayStopped);Cases++;}
  for(UINTN I=0;I<2;++I){fresh();with_display();initialize_action(PianoUsbServiceActionReboot);if(I==0)DisplayRaiseEbs=TRUE;else InputBadTpl=TRUE;
    must_retain();assert(!strcmp(Order,"PURXApsI")&&!Owners.Report.DisplayStopped&&!Owners.Report.ManagerEventClosed);Cases++;}
  fresh();with_display();initialize_action(PianoUsbServiceActionReboot);Owners.Config.StopDisplay=NULL;must_retain();assert(!strcmp(Order,"PURXApsI"));Cases++;
  for(UINTN I=0;I<8;++I){fresh();with_display();initialize_action(PianoUsbServiceActionReboot);
    EFI_STATUS *Stages[]={&PolicyStatus,&UsbStatus,&ProofStatus,&AcceptStatus,&PrepareStatus,&ShutdownStatus,&InputStatus,&EventStatus};*Stages[I]=EFI_DEVICE_ERROR;
    must_retain();if(I<7){assert(!strchr(Order,'D')&&Owners.Report.DisplayStatus==EFI_NOT_STARTED);}
    else{assert(Owners.Report.DisplayStopped&&!strcmp(Order,"PURXApsIDM"));}Cases++;}
  fresh();Config.DisplayStartup.Status=EFI_UNSUPPORTED;assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);Cases++;
  for(UINTN I=0;I<2;++I){fresh();with_display();initialize_action(PianoUsbServiceActionReboot);if(I==0)DisplayBadTpl=TRUE;else DisplayPostRaiseEbs=TRUE;
    must_retain();assert(Owners.Report.DisplayStopped&&!strcmp(Order,"PURXApsID")&&!Owners.Report.ManagerEventClosed);Cases++;}
  // Local file boot has a real policy latch and source identity; ordinary USB
  // retirement never consumes a download token or fabricates an ACK.
  fresh();with_display();assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);
  assert(PianoProductOwnersRequestFileBoot(&Owners,(VOID *)0x701,(VOID *)0x702)==EFI_NOT_READY&&!Calls);
  Policy.RequestedCoreAction=PianoUsbServiceActionBoot;
  assert(PianoProductOwnersResolveReturnedAction(&Owners)==EFI_NOT_READY&&!Calls);
  assert(PianoProductOwnersRequestFileBoot(&Owners,NULL,(VOID *)0x702)==EFI_INVALID_PARAMETER&&!Calls);
  assert(PianoProductOwnersRequestFileBoot(&Owners,(VOID *)0x701,NULL)==EFI_INVALID_PARAMETER&&!Calls);
  Policy.Dispatching=TRUE;assert(PianoProductOwnersRequestFileBoot(&Owners,(VOID *)0x701,(VOID *)0x702)==EFI_NOT_READY&&!Calls);
  Policy.Dispatching=FALSE;Policy.Pumping=TRUE;assert(PianoProductOwnersRequestFileBoot(&Owners,(VOID *)0x701,(VOID *)0x702)==EFI_NOT_READY&&!Calls);
  Policy.Pumping=FALSE;Policy.ActiveAction=PIANO_PRODUCT_ACTION_SETUP;
  assert(PianoProductOwnersRequestFileBoot(&Owners,(VOID *)0x701,(VOID *)0x702)==EFI_NOT_READY&&!Calls);Policy.ActiveAction=PIANO_PRODUCT_ACTION_NONE;
  assert(PianoProductOwnersRequestFileBoot(&Owners,(VOID *)0x701,(VOID *)0x702)==EFI_SUCCESS);
  assert(PianoProductOwnersResolveReturnedAction(&Owners)==EFI_SUCCESS&&Owners.Report.Origin==PianoProductRequestFile);
  assert(PianoProductOwnersRetire(&Owners)==EFI_SUCCESS&&Owners.Report.AllowedAction==PianoUsbServiceActionBoot&&Owners.Report.Clean);
  assert(!strcmp(Order,"PURXApsIDM")&&!Owners.Report.BootActionConsumed&&!Owners.Report.Boot.Token&&!Owners.Report.Boot.Context&&!Requests);
  assert(Owners.Report.FileContext==(VOID *)0x701&&Owners.Report.FileToken==(VOID *)0x702&&Owners.Report.RetiredMask==PIANO_OWNER_SUPPORTED_MASK);Cases++;
  for(UINTN I=0;I<3;++I){fresh();assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);Policy.RequestedCoreAction=PianoUsbServiceActionBoot;
    assert(PianoProductOwnersRequestFileBoot(&Owners,(VOID *)0x701,(VOID *)0x702)==EFI_SUCCESS);
    if(I==0)Policy.RequestedCoreAction=PianoUsbServiceActionNone;
    if(I==1)Owners.Report.FileToken=NULL;
    if(I==2)assert(PianoProductOwnersRequestFileBoot(&Owners,(VOID *)0x701,(VOID *)0x703)==EFI_COMPROMISED_DATA);
    must_retain();assert(!Calls&&!Owners.Report.BootActionConsumed);Cases++;}
  // USB can win before registration, after registration, or at the final
  // pre-retirement check. The last case yields without cleanup so Core can
  // release the unselected file before retrying the actual host action.
  for(UINTN I=0;I<3;++I)for(UINTN A=0;A<2;++A){fresh();assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);Policy.RequestedCoreAction=PianoUsbServiceActionBoot;
    if(I)assert(PianoProductOwnersRequestFileBoot(&Owners,(VOID *)0x701,(VOID *)0x702)==EFI_SUCCESS);
    UsbState.Phase=PianoUsbServiceStopRequested;UsbState.Action=A?PianoUsbServiceActionBoot:PianoUsbServiceActionReboot;
    EFI_STATUS S=I==0?PianoProductOwnersRequestFileBoot(&Owners,(VOID *)0x701,(VOID *)0x702):
      I==1?PianoProductOwnersResolveReturnedAction(&Owners):PianoProductOwnersRetire(&Owners);
    assert(S==(I==2?EFI_NOT_READY:EFI_SUCCESS)&&Owners.Report.Origin==PianoProductRequestUsb&&!Calls&&!Owners.Report.Busy&&!Owners.Report.Retained);
    assert(!Owners.Report.FileContext&&!Owners.Report.FileToken&&Owners.Report.RequestedAction==UsbState.Action);
    assert(PianoProductOwnersRetire(&Owners)==EFI_SUCCESS&&Owners.Report.AllowedAction==UsbState.Action);
    assert(!strcmp(Order,A?"PUBRXApsIM":"PURXApsIM")&&Owners.Report.BootActionConsumed==(BOOLEAN)A);Cases++;}
  for(UINTN I=0;I<9;++I){fresh();with_display();assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);Policy.RequestedCoreAction=PianoUsbServiceActionBoot;
    assert(PianoProductOwnersRequestFileBoot(&Owners,(VOID *)0x701,(VOID *)0x702)==EFI_SUCCESS);
    EFI_STATUS *Stages[]={&PolicyStatus,&UsbStatus,&ProofStatus,&AcceptStatus,&PrepareStatus,&ShutdownStatus,&InputStatus,&DisplayStatus,&EventStatus};*Stages[I]=EFI_DEVICE_ERROR;
    must_retain();assert(!strchr(Order,'B')&&!Owners.Report.BootActionConsumed&&!Owners.Report.Boot.Token&&Owners.Report.FileToken==(VOID *)0x702);Cases++;}
  fresh();assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);Policy.RequestedCoreAction=PianoUsbServiceActionBoot;
  assert(PianoProductOwnersRequestFileBoot(&Owners,(VOID *)0x701,(VOID *)0x702)==EFI_SUCCESS);PianoProductOwnersFenceExit(&Owners);
  assert(PianoProductOwnersResolveReturnedAction(&Owners)==EFI_ACCESS_DENIED&&!Calls);must_retain();Cases++;
  // Startup absence is admitted only through the producer's exact proof. No
  // service status, Stop, host action, or ACK is substituted for this branch.
  for(UINTN I=0;I<3;++I){fresh();without_usb();with_display();
    assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS&&StartupValidations==1&&!UsbStatusReads);
    assert(PianoProductOwnersObserveUsbAction(&Owners)==EFI_NOT_READY&&!Requests&&!UsbStatusReads);
    Policy.RequestedCoreAction=I==0?PianoUsbServiceActionBoot:I==1?PianoUsbServiceActionContinue:PianoUsbServiceActionReboot;
    if(I==0){assert(PianoProductOwnersRequestFileBoot(&Owners,(VOID *)0x701,(VOID *)0x702)==EFI_SUCCESS);}
    assert(PianoProductOwnersResolveReturnedAction(&Owners)==EFI_SUCCESS);
    assert(PianoProductOwnersRetire(&Owners)==EFI_SUCCESS&&Owners.Report.Clean&&PianoProductOwnersUsbRetired(&Owners));
    assert(!strcmp(Order,"PXApsIDM")&&!UsbStatusReads&&!Requests&&!Owners.Report.UsbStopped&&Owners.Report.UsbStartupFailedClean&&
      !Owners.Report.Usb.Attempted&&Owners.Report.UsbStatus==EFI_NOT_STARTED&&!Owners.Report.BootActionConsumed&&!Owners.Report.Boot.Token&&
      Owners.Report.ProofAccepted&&!(Owners.Report.RetiredMask&PIANO_OWNER_USB)&&Owners.Report.RetiredMask==Config.StartedOwnerMask);
    UINTN Before=StartupValidations;assert(PianoProductOwnersUsbRetired(&Owners)&&StartupValidations==Before);Cases++;}
  for(UINTN I=0;I<11;++I){fresh();without_usb();PIANO_SMMU_RETIRED_USB_PROOF Forged=StartupProof;
    switch(I){case 0:Config.StartupUsbProof=NULL;break;case 1:StartupProof.Valid=FALSE;break;
      case 2:StartupProof.Execution.Kind=PIANO_USB_RETIRE_RUNNING;break;case 3:StartupProof.Execution.StartupStatus=EFI_DEVICE_ERROR;break;
      case 4:StartupProof.Execution.DmaBuffersAllocated=1;break;case 5:StartupProof.Execution.DmaBuffersFreed=1;break;
      case 6:Config.StartupUsbProof=&Forged;break;case 7:StartupValidateStatus=EFI_WARN_STALE_DATA;break;
      case 8:UsbState.Retained=TRUE;break;case 9:UsbState.ServicesLost=TRUE;break;case 10:UsbState.Started=TRUE;break;}
    assert(PianoProductOwnersInitialize(&Owners,&Config)!=EFI_SUCCESS&&!Owners.Report.Initialized&&!Calls&&!UsbStatusReads);Cases++;}
  fresh();without_usb();Config.StartedOwnerMask|=PIANO_OWNER_USB;Config.AbsentOwnerMask&=~PIANO_OWNER_USB;
  assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_NOT_READY&&!StartupValidations&&!Calls);Cases++;
  for(UINTN I=0;I<6;++I){fresh();without_usb();assert(PianoProductOwnersInitialize(&Owners,&Config)==EFI_SUCCESS);
    Policy.RequestedCoreAction=PianoUsbServiceActionBoot;
    assert(PianoProductOwnersRequestFileBoot(&Owners,(VOID *)0x701,(VOID *)0x702)==EFI_SUCCESS);
    if(I==0)StartupValidateStatus=EFI_ACCESS_DENIED;
    if(I==1)StartupProof.TablePhysical+=4096;
    if(I==2)Owners.Report.UsbStartupFailedClean=FALSE;
    if(I==3)Owners.Report.Origin=PianoProductRequestUsb;
    if(I==4)StartupFailAt=StartupValidations+2;
    if(I==5)AcceptStatus=EFI_ACCESS_DENIED;
    must_retain();assert(!UsbStatusReads&&!strchr(Order,'U')&&!strchr(Order,'B')&&!strchr(Order,'R')&&!Owners.Report.UsbStopped&&!Owners.Report.BootActionConsumed);
    if(I==4)assert(!strcmp(Order,"P")&&!Owners.Report.BridgeStopped);
    if(I==5)assert(!strcmp(Order,"PXA")&&!Owners.Report.UfsStopped);
    Cases++;}
  printf("Actual product owners: %llu cases PASS (100 original retained); full masks, typed Display startup/ordinary+alternate refs, Policy/USB/proof/bridge/UFS/Input/Display/Event order, APP/partial/duplicate/stale/warning/EBS retention; no native/reset/image calls in manager.\n",(unsigned long long)Cases);
  return 0;
}
