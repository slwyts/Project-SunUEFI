// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoDisplayClockRead.h"
#define PIANO_DISPLAY_RAIL_OBSERVE_REVISION 1U
#define PIANO_DISPLAY_RAIL_OBSERVE_PHASES 16U
typedef struct {
  VOID *Context;EFI_BOOT_SERVICES *Services;EFI_DXE_SERVICES *DxeServices;
  BOOLEAN (*Alive)(VOID *);
  // Resident actual product reader, supplied privately by DisplayOwner.
  PIANO_DISPLAY_CLOCK_READ *ClockReader;
  // Exact GUID→handle records of successfully started native images. No
  // candidate scanning or size-derived producer identity is performed.
  EFI_HANDLE NpaHandle,VcsHandle;
} PIANO_DISPLAY_RAIL_ENV;
typedef struct {
  EFI_STATUS Status;UINT64 Client,Resource,Definition,Node,Rail,Backend;
  UINT64 RequestCallback,Driver,Plugin,RequestMapping,RpmhContext,RpmhConfig,RpmhHandle;
  UINT32 ClientType,ActiveIndex,ActiveRequest,PendingRequest,RequestAttributes;
  UINT32 NpaApplied,NpaRequired,NpaSuppressible,VcsApplied,RpmhDrvId;
  CHAR8 ResourceName[16],RailName[16];
} PIANO_DISPLAY_RAIL_GRAPH;
typedef struct {
  CHAR8 Phase[32];EFI_STATUS Status,SelectorBefore,SelectorAfter;
  PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT ClockBefore,ClockAfter;
  PIANO_DISPLAY_RAIL_GRAPH Mm[2],Mx[2];
  BOOLEAN MmCoherent,MxCoherent,Retained,ServicesLost;
  // Object observations never grant a native clock/rail/access lifetime.
  BOOLEAN PowerReady,MemoryOwnershipGranted,RpmhCompletionObserved;
} PIANO_DISPLAY_RAIL_SNAPSHOT;
typedef struct {
  UINT32 Revision,Count,Reads,Sessions;EFI_STATUS Status,Pin[2],Identity[2],Close;
  BOOLEAN Initialized,Busy,Retained,ServicesLost;
  PIANO_GUARDED_REPORT Guard;
  PIANO_DISPLAY_RAIL_SNAPSHOT Snapshot[PIANO_DISPLAY_RAIL_OBSERVE_PHASES];
} PIANO_DISPLAY_RAIL_REPORT;
typedef struct {
  EFI_HANDLE Handle;EFI_LOADED_IMAGE_PROTOCOL *Loaded;UINT64 Base;
  VOID *Copy;UINTN Bytes;BOOLEAN CodeVerified;
} PIANO_DISPLAY_RAIL_IMAGE;
typedef struct {
  UINT32 Signature;PIANO_DISPLAY_RAIL_ENV Env;PIANO_DISPLAY_RAIL_REPORT Report;
  EFI_EVENT Exit;PIANO_DISPLAY_RAIL_IMAGE Image[2];
  VOID *NpaProtocol;UINT64 Map[PIANO_DISPLAY_CLOCK_READ_MAP_BYTES/8];
} PIANO_DISPLAY_RAIL_OBSERVER;
// No native NPA/rail request or register access. Guard reads only exact pinned
// executable images and the selector-anchored typed low-heap object graph.
EFI_STATUS PianoDisplayRailInit(PIANO_DISPLAY_RAIL_OBSERVER *,CONST PIANO_DISPLAY_RAIL_ENV *);
EFI_STATUS PianoDisplayRailObserve(PIANO_DISPLAY_RAIL_OBSERVER *,CONST CHAR8 *Phase);
EFI_STATUS PianoDisplayRailReemit(PIANO_DISPLAY_RAIL_OBSERVER *);
EFI_STATUS PianoDisplayRailClose(PIANO_DISPLAY_RAIL_OBSERVER *);
BOOLEAN PianoDisplayRailRetained(CONST PIANO_DISPLAY_RAIL_OBSERVER *);
CONST PIANO_DISPLAY_RAIL_REPORT *PianoDisplayRailReport(CONST PIANO_DISPLAY_RAIL_OBSERVER *);
