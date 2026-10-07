// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoDisplayRailObserve.h"
#define PIANO_NON_GDSC_CLOCK_REVISION 1U
typedef struct {
 PIANO_DISPLAY_CLOCK_LEASE *Gcc;
 PIANO_DISPLAY_CLOCK_READ *Reader;
 PIANO_DISPLAY_RAIL_OBSERVER *Rail;
} PIANO_NON_GDSC_CLOCK_ENV;
typedef struct {
 UINT32 Revision;EFI_STATUS Status,Borrow,Create,GetId,Before,Enable,Counter,IsEnabled,IsOn,After,Return,Disable,Close;
 BOOLEAN Busy,Held,Retained,ServicesLost,GetAttempted,EnableAttempted,DisableAttempted,Released,EnabledObserved,OnObserved;
 UINT32 OwnedReferences;UINTN ClockId;UINT64 NativeBase,TransactionToken;
 PIANO_DISPLAY_CLOCK_LEASE_HELD_PROOF ParentBefore,ParentAfter;
 PIANO_DISPLAY_CLOCK_SELECTOR_SNAPSHOT Baseline,Acquired,ReleaseBefore,Retired;
 PIANO_DISPLAY_RAIL_SNAPSHOT RailBefore,RailAfter,RailReleaseBefore,RailRetired;
} PIANO_NON_GDSC_CLOCK_REPORT;
typedef struct {
 UINT32 Signature;PIANO_NON_GDSC_CLOCK_ENV Env;PIANO_NON_GDSC_CLOCK_REPORT Report;
 EFI_CLOCK_PROTOCOL *Clock;EFI_EVENT Exit;
} PIANO_NON_GDSC_CLOCK;
// Driver-lifetime zero state. Reuses actual registered GCC/Reader/Rail objects;
// no independent pin, frequency/reset/GDSC operation, MX gate or power grant.
// Acquire/Release borrow GCC only during their native transactions. A future
// common product parent must stop this child successfully before GCC release.
EFI_STATUS PianoDisplayNonGdscClockAcquire(PIANO_NON_GDSC_CLOCK *,CONST PIANO_NON_GDSC_CLOCK_ENV *);
EFI_STATUS PianoDisplayNonGdscClockRelease(PIANO_NON_GDSC_CLOCK *);
// No product binding is supplied by this staged host-only module.
