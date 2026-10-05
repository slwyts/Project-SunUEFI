// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual product manager with typed owner/BS fixtures; no device commands.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../bootprofiles/uefi-app/PianoProductOwners.c"
EFI_BOOT_SERVICES *gBS;static EFI_BOOT_SERVICES Bs;
EFI_GUID gEfiEventExitBootServicesGuid={0x27ABF055,0xB1B8,0x4C26,{0x80,0x48,0x74,0x8F,0x37,0xBA,0xA2,0xDF}};
static PIANO_PRODUCT_OWNERS Owners;static PIANO_PRODUCT_OWNERS_CONFIG Config;
static PIANO_BOOT_POLICY_REPORT Policy;static PIANO_DWC3_SERVICE_STATUS UsbState;
static PIANO_USB_SERVICE_RETIRE_REPORT UsbReport;static PIANO_UFS_RESET_REPORT UfsReport;
static PIANO_PRODUCT_INPUT_RETIRE_REPORT InputReport;static PIANO_FB_STORAGE Storage;
static PIANO_PRODUCT_RUNTIME_PROTOCOL Runtime;static EFI_TPL Tpl;
static EFI_STATUS PolicyStatus,UsbStatus,ProofStatus,AcceptStatus,PrepareStatus,ShutdownStatus,InputStatus,EventStatus,RequestStatus,TakeStatus;
static BOOLEAN BridgeLive,ProofValid,ReportAbsent,FreshReplaced,CreateWarning,CreateNull,CreateEbs,InputEbs;
static UINTN Requests,Calls;static CHAR8 Order[100];static PIANO_FB_BOOT_ACTION BootAction;
static VOID record(CHAR8 Step){assert(Calls<sizeof(Order)-1);Order[Calls++]=Step;Order[Calls]=0;}
VOID *EFIAPI ZeroMem(VOID *Buffer,UINTN Bytes){return memset(Buffer,0,Bytes);}
static EFI_TPL EFIAPI raise(EFI_TPL New){assert(New>=Tpl);EFI_TPL Old=Tpl;Tpl=New;return Old;}
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
EFI_STATUS PianoUsbControllerServiceGetStatus(PIANO_DWC3_SERVICE_STATUS *Status){*Status=UsbState;return EFI_SUCCESS;}
EFI_STATUS PianoUsbControllerServiceStop(EFI_STATUS Reason,PIANO_USB_SERVICE_RETIRE_REPORT *Report){assert(Tpl==TPL_APPLICATION && !Policy.ProtocolInstalled && Reason==EFI_SUCCESS);record('U');*Report=UsbReport;return UsbStatus;}
EFI_STATUS PianoDwc3ConsumeBootAction(PIANO_FB_BOOT_ACTION *Action){assert(Calls && Order[Calls-1]=='U');record('B');*Action=BootAction;return TakeStatus;}
EFI_STATUS PianoUsbControllerMakeRetiredUsbProof(CONST VOID *Fdt,PIANO_SMMU_RETIRED_USB_PROOF *Proof){assert(Fdt==(VOID *)0x123 && Tpl==TPL_APPLICATION && UsbStatus==EFI_SUCCESS && UsbReport.Clean);record('R');Proof->Valid=ProofValid;return ProofStatus;}
static EFI_STATUS ready(VOID *Context){assert(Context==(VOID *)0x456 && BridgeLive && Tpl==TPL_APPLICATION);return EFI_SUCCESS;}
static EFI_STATUS info(VOID *Context,CONST CHAR8 *Name,PIANO_FB_PARTITION_INFO *Result){(void)Context;(void)Name;(void)Result;assert(!"manager must not read partition metadata");return EFI_UNSUPPORTED;}
static EFI_STATUS read_blocks(VOID *Context,CONST PIANO_FB_PARTITION_INFO *Info,UINT64 Lba,UINTN Bytes,VOID *Buffer){(void)Context;(void)Info;(void)Lba;(void)Bytes;(void)Buffer;assert(!"manager must not issue UFS I/O");return EFI_UNSUPPORTED;}
CONST PIANO_FB_STORAGE *PianoFastbootBlockReadStorage(VOID){return BridgeLive?&Storage:NULL;}
VOID PianoFastbootBlockReadStop(VOID){assert(Tpl==TPL_APPLICATION && Calls && Order[Calls-1]=='R');record('X');BridgeLive=FALSE;}
EFI_STATUS PianoUfsAcceptRetiredUsb(CONST PIANO_SMMU_RETIRED_USB_PROOF *Proof){assert(Proof==&Owners.UsbProof && Proof->Valid && Tpl==TPL_APPLICATION && !BridgeLive);record('A');return AcceptStatus;}
EFI_STATUS PianoUfsBlockIoPrepareForReset(VOID){assert(Tpl==TPL_CALLBACK && !BridgeLive && AcceptStatus==EFI_SUCCESS);record('p');raise(TPL_CALLBACK);return PrepareStatus;}
EFI_STATUS PianoUfsBlockIoShutdownForReset(VOID){assert(Tpl==TPL_CALLBACK && PrepareStatus==EFI_SUCCESS);record('s');return ShutdownStatus;}
CONST PIANO_UFS_RESET_REPORT *PianoUfsResetShutdownReport(VOID){return ReportAbsent?NULL:&UfsReport;}
static EFI_STATUS stop_input(VOID *Context,PIANO_PRODUCT_INPUT_RETIRE_REPORT *Report){assert(Context==(VOID *)0x789 && Tpl==TPL_APPLICATION && Owners.Report.UfsStopped && UfsReport.Clean && !Policy.ProtocolInstalled);record('I');*Report=InputReport;if(InputEbs)PianoProductOwnersFenceExit(&Owners);return InputStatus;}
static VOID fresh(VOID) {
  memset(&Owners,0,sizeof(Owners));memset(&Config,0,sizeof(Config));memset(&Policy,0,sizeof(Policy));memset(&UsbState,0,sizeof(UsbState));memset(&UsbReport,0,sizeof(UsbReport));memset(&UfsReport,0,sizeof(UfsReport));memset(&InputReport,0,sizeof(InputReport));memset(&BootAction,0,sizeof(BootAction));
  Tpl=TPL_APPLICATION;Requests=Calls=0;Order[0]=0;BridgeLive=ProofValid=TRUE;ReportAbsent=FreshReplaced=CreateWarning=CreateNull=CreateEbs=InputEbs=FALSE;
  PolicyStatus=UsbStatus=ProofStatus=AcceptStatus=PrepareStatus=ShutdownStatus=InputStatus=EventStatus=RequestStatus=TakeStatus=EFI_SUCCESS;
  Policy.Initialized=Policy.ProtocolInstalled=TRUE;
  UsbState=(PIANO_DWC3_SERVICE_STATUS){.Revision=1,.Started=TRUE,.Phase=PianoUsbServiceListening};
  UsbReport=(PIANO_USB_SERVICE_RETIRE_REPORT){.Revision=1,.Attempted=TRUE,.Clean=TRUE,.DeviceHalted=TRUE,.DmaFreed=TRUE,.DomainFreed=TRUE,.ClocksReleased=TRUE,.DmaBuffersFreed=9,.ClockReleaseMask=255};
  UfsReport=(PIANO_UFS_RESET_REPORT){.Started=TRUE,.Prepared=TRUE,.Returned=TRUE,.Clean=TRUE,.TplHeld=TRUE,.DmaFreed=3,.Disconnected=6,.ProtocolsRemoved=7};
  InputReport=(PIANO_PRODUCT_INPUT_RETIRE_REPORT){.Revision=1,.Started=TRUE,.Returned=TRUE,.Clean=TRUE};
  BootAction=(PIANO_FB_BOOT_ACTION){.Context=(VOID *)0x1122,.Token=(VOID *)0x3344,.Taken=TRUE,.Proof={.AckCompleted=TRUE,.QueueEmpty=TRUE,.DeviceHalted=TRUE,.DmaFreed=TRUE,.DispatchFrozen=TRUE,.AckBytes=4,.DmaBuffersFreed=9}};
  Storage=(PIANO_FB_STORAGE){(VOID *)0x456,ready,info,read_blocks};Runtime=(PIANO_PRODUCT_RUNTIME_PROTOCOL){.Revision=PIANO_PRODUCT_RUNTIME_REVISION,.RequestAction=request};
  Config=(PIANO_PRODUCT_OWNERS_CONFIG){.Revision=1,.Fdt=(VOID *)0x123,.ExpectedOwnerMask=PIANO_OWNER_ALL_MASK,.StartedOwnerMask=PIANO_OWNER_CORE_MASK,.AbsentOwnerMask=PIANO_OWNER_USB_HOST|PIANO_OWNER_GPI|PIANO_OWNER_POGO,.Runtime=&Runtime,.InputContext=(VOID *)0x789,.StopInput=stop_input};
  Bs.RaiseTPL=raise;Bs.RestoreTPL=restore;Bs.CreateEventEx=create;Bs.CloseEvent=close;Bs.LocateProtocol=locate;gBS=&Bs;
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
  printf("Actual product owners: %llu cases PASS; full registration, APP cooperative return, Policy/USB/proof/bridge/UFS/input order, strict typed fields, TPL lease, partial boot token retention, warning/EBS fail-stop, trusted UI reboot from listening USB without fabricated ACK, no reset/image execution.\n",(unsigned long long)Cases);
  return 0;
}
