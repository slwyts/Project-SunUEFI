// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoNvFvb.h"
#include "Protocol/PianoNvReady.h"
typedef struct {
 PIANO_NV_FVB Fvb;
 PIANO_NV_READY_PROTOCOL Info;
 EFI_HANDLE Handle;
 EFI_EVENT ExitEvent,VirtualEvent;
 BOOLEAN Installed,Retained;
} PIANO_NV_FVB_BINDING;
// This binding/code/context/journal/mirror MUST live in a DXE_RUNTIME_DRIVER
// and runtime allocations; product APP boot-service pages are not acceptable.
// Refuses existing VariableArch: never shadow an already initialized RAM cache.
EFI_STATUS PianoNvFvbPublish(EFI_HANDLE RuntimeDriverImage,PIANO_NV_FVB_BINDING *,PIANO_NV_JOURNAL *);
