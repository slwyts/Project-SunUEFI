// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
#include "PianoBootPolicy.h"
#include "PianoUfsShutdown.h"
#include "PianoUsbStorageExperiment.h"
#define PIANO_PRODUCT_OWNERS_REVISION 1U
#define PIANO_OWNER_USB       BIT0
#define PIANO_OWNER_UFS       BIT1
#define PIANO_OWNER_BRIDGE    BIT2
#define PIANO_OWNER_POLICY    BIT3
#define PIANO_OWNER_INPUT     BIT4
#define PIANO_OWNER_USB_HOST  BIT5
#define PIANO_OWNER_GPI       BIT6
#define PIANO_OWNER_POGO      BIT7
#define PIANO_OWNER_DISPLAY   BIT8
#define PIANO_OWNER_CORE_MASK (PIANO_OWNER_USB|PIANO_OWNER_UFS|PIANO_OWNER_BRIDGE|PIANO_OWNER_POLICY|PIANO_OWNER_INPUT)
#define PIANO_OWNER_SUPPORTED_MASK (PIANO_OWNER_CORE_MASK|PIANO_OWNER_DISPLAY)
#define PIANO_OWNER_ALL_MASK  (PIANO_OWNER_SUPPORTED_MASK|PIANO_OWNER_USB_HOST|PIANO_OWNER_GPI|PIANO_OWNER_POGO)
typedef struct {
  UINT32 Revision;
  BOOLEAN Started,Returned,Clean,Retained,ServicesLost;
  EFI_STATUS Status,Timer,Protocols;
} PIANO_PRODUCT_INPUT_RETIRE_REPORT;
typedef EFI_STATUS (*PIANO_PRODUCT_INPUT_STOP)(VOID *Context,PIANO_PRODUCT_INPUT_RETIRE_REPORT *Report);
typedef struct {
  UINT32 Revision;VOID *LeaseContext;UINTN ClockId;UINT64 NativeBase;
  BOOLEAN AcquireAttempted,Held,KnownNoSideEffects,Retained,ServicesLost;
  UINT32 OwnedReferences;EFI_STATUS Status;
  UINT32 AcquireBeforeSnapshots,AcquireAfterSnapshots;
  // Index0 is native ordinary refs, index1 the bit9 alternate refs. Matching
  // snapshots proves repeated decode; the two indices are not duplicate reads.
  UINT16 AcquireBeforeTotal[2],AcquireAfterTotal[2],AcquireBeforeClient[2],AcquireAfterClient[2];
} PIANO_PRODUCT_DISPLAY_STARTUP_REPORT;
typedef struct {
  UINT32 Revision;VOID *LeaseContext;UINTN ClockId;UINT64 NativeBase;
  BOOLEAN Started,Returned,Clean,Retained,ServicesLost;
  BOOLEAN ReleaseAttempted,HeldAfter,Released,ExitClosed;
  UINT32 OwnedReferencesBefore,OwnedReferencesAfter;
  EFI_STATUS Status,Release,CounterStatus,GccReadback,GccReadbackEnd,Cleanup;
  UINT32 GccReads,GccPages;
  UINT32 AcquireBeforeSnapshots,AcquireAfterSnapshots,ReleaseBeforeSnapshots,ReleaseAfterSnapshots;
  UINT16 AcquireBeforeTotal[2],AcquireAfterTotal[2],AcquireBeforeClient[2],AcquireAfterClient[2];
  UINT16 ReleaseBeforeTotal[2],ReleaseAfterTotal[2],ReleaseBeforeClient[2],ReleaseAfterClient[2];
} PIANO_PRODUCT_DISPLAY_RETIRE_REPORT;
typedef EFI_STATUS (*PIANO_PRODUCT_DISPLAY_STOP)(VOID *Context,PIANO_PRODUCT_DISPLAY_RETIRE_REPORT *Report);
typedef struct {
  UINT32 Revision;
  CONST VOID *Fdt;
  // Registration must cover every known owner exactly once. Root supplies
  // actual startup state; a missing bit never means an absent controller.
  UINT32 ExpectedOwnerMask,StartedOwnerMask,AbsentOwnerMask;
  PIANO_PRODUCT_RUNTIME_PROTOCOL *Runtime;
  VOID *InputContext;
  PIANO_PRODUCT_INPUT_STOP StopInput;
  VOID *DisplayContext;
  PIANO_PRODUCT_DISPLAY_STOP StopDisplay;
  PIANO_PRODUCT_DISPLAY_STARTUP_REPORT DisplayStartup;
} PIANO_PRODUCT_OWNERS_CONFIG;
typedef enum {
  PianoProductOwnersUninitialized=0,PianoProductOwnersRunning,
  PianoProductOwnersReturnRequested,PianoProductOwnersRetiring,
  PianoProductOwnersClean,PianoProductOwnersRetained
} PIANO_PRODUCT_OWNERS_PHASE;
typedef enum {
  PianoProductRequestNone=0,PianoProductRequestUsb,PianoProductRequestUi
} PIANO_PRODUCT_REQUEST_ORIGIN;
typedef struct {
  UINT32 Revision;
  PIANO_PRODUCT_OWNERS_PHASE Phase;
  PIANO_USB_SERVICE_ACTION RequestedAction,AllowedAction;
  PIANO_PRODUCT_REQUEST_ORIGIN Origin;
  BOOLEAN Initialized,Busy,ServicesLost,Retained,Clean,PolicyStopped;
  BOOLEAN UsbStopped,ProofAccepted,BridgeStopped,UfsStopped,InputStopped,DisplayStopped;
  BOOLEAN BootActionConsumed,OuterTplHeld,ManagerEventClosed;
  EFI_STATUS Status,PolicyStatus,UsbStatus,ProofStatus,AcceptStatus;
  EFI_STATUS PrepareStatus,ShutdownStatus,InputStatus,DisplayStatus,EventStatus;
  UINT32 RegisteredStartedMask,RegisteredAbsentMask,RetiredMask;
  PIANO_USB_SERVICE_RETIRE_REPORT Usb;
  PIANO_UFS_RESET_REPORT Ufs;
  PIANO_PRODUCT_INPUT_RETIRE_REPORT Input;
  PIANO_PRODUCT_DISPLAY_RETIRE_REPORT Display;
  PIANO_FB_BOOT_ACTION Boot;
} PIANO_PRODUCT_OWNERS_REPORT;
typedef struct {
  PIANO_PRODUCT_OWNERS_CONFIG Config;
  PIANO_PRODUCT_OWNERS_REPORT Report;
  PIANO_SMMU_RETIRED_USB_PROOF UsbProof;
  EFI_EVENT ExitEvent;
  EFI_TPL OuterTpl;
  EFI_STATUS (EFIAPI *RuntimeRequest)(PIANO_PRODUCT_RUNTIME_PROTOCOL *,UINT32);
} PIANO_PRODUCT_OWNERS;
// APP-only; pass a zero-initialized, driver-lifetime object and keep Config's
// backing contexts alive. A retained event/token forbids returning a stack
// object or unloading the containing image. Already-started owners remain
// caller-owned if Initialize fails before its event registration.
EFI_STATUS PianoProductOwnersInitialize(PIANO_PRODUCT_OWNERS *Owners,CONST PIANO_PRODUCT_OWNERS_CONFIG *Config);
// Observes actual USB action and requests cooperative RETURN_CORE. No cleanup.
EFI_STATUS PianoProductOwnersObserveUsbAction(PIANO_PRODUCT_OWNERS *Owners);
// APP-only after a cooperative UI return. Requires the actual BootPolicy
// report's exact UI action latch; it does not synthesize a USB command or ACK.
EFI_STATUS PianoProductOwnersRequestUiAction(PIANO_PRODUCT_OWNERS *Owners,PIANO_USB_SERVICE_ACTION Action);
// Compatibility entry for the existing cold reboot UI path.
EFI_STATUS PianoProductOwnersRequestUiReboot(PIANO_PRODUCT_OWNERS *Owners);
// After child cleanup, real acknowledged USB requests take precedence over
// an older UI latch. Never chooses from a caller-supplied status/permission.
EFI_STATUS PianoProductOwnersResolveReturnedAction(PIANO_PRODUCT_OWNERS *Owners);
// Caller invokes after the UI returns. No ResetSystem, LoadImage or StartImage.
// On success the typed report alone permits the selected deferred action.
// Boot tokens, including partial/error tokens, stay owned by this ledger.
EFI_STATUS PianoProductOwnersRetire(PIANO_PRODUCT_OWNERS *Owners);
// CPU-only EBS fence; retained state is terminal and cannot be retried.
VOID PianoProductOwnersFenceExit(PIANO_PRODUCT_OWNERS *Owners);
