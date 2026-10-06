// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <PianoProductOwners.h>
#include <PianoCpuInput.h>
#include <PianoRawLinuxBoot.h>
#include <Protocol/PianoProductExit.h>
typedef struct {
 VOID *Context;EFI_BOOT_SERVICES *Services;EFI_SYSTEM_TABLE *SystemTable;EFI_HANDLE ParentImage;
 PIANO_PRODUCT_OWNERS *Owners;
 BOOLEAN (*BootServicesAlive)(VOID *);
 EFI_STATUS (*CheckMemory)(VOID *,PIANO_LINUX_MEMORY_PROOF *);
 EFI_STATUS (*ValidateMemory)(VOID *,CONST PIANO_LINUX_MEMORY_PROOF *);
 VOID (*FailStop)(VOID *,EFI_STATUS); // runtime-safe, must not return
 BOOLEAN (*ValidateRaw)(CONST PIANO_RAW_LINUX_REPORT *,CONST PIANO_PRODUCT_OWNERS *,EFI_HANDLE);
} PIANO_LATE_HANDOFF_ENV;
typedef struct {
 UINT32 Signature;PIANO_PRODUCT_EXIT_PROTOCOL Protocol;PIANO_LATE_HANDOFF_ENV Env;
 EFI_HANDLE ProtocolHandle,Image;CONST EFI_LOADED_IMAGE_PROTOCOL *Identity;
 EFI_LOADED_IMAGE_PROTOCOL Loaded;PIANO_LINUX_MEMORY_PROOF Memory;
 PIANO_PRODUCT_OWNERS_CONFIG OwnerConfig;PIANO_PRODUCT_OWNERS_REPORT CleanOwners;
 PIANO_PRODUCT_EXIT_PHASE Phase;EFI_STATUS Status;
 UINT32 ExitCalls,RetireCalls;UINTN CallerMapKey;
 BOOLEAN Busy,Installed,Retained;
 BOOLEAN RawMode;CONST PIANO_RAW_LINUX_REPORT *Raw;PIANO_RAW_LINUX_REPORT RawSnapshot;
} PIANO_LATE_HANDOFF;
// Driver-lifetime singleton Root authority. Initialize/install once, then Arm
// the actual selected loaded child before StartImage; default remains unarmed.
EFI_STATUS PianoLateHandoffInitialize(PIANO_LATE_HANDOFF *,CONST PIANO_LATE_HANDOFF_ENV *);
EFI_STATUS PianoLateHandoffArm(PIANO_LATE_HANDOFF *,EFI_HANDLE ChildImage);
// Core-only fixed-window raw boot after actual manager retirement. This does
// not create a full-DDR EFI memory proof or admit a child EFI application.
EFI_STATUS PianoLateHandoffArmRaw(PIANO_LATE_HANDOFF *,EFI_HANDLE CoreImage,CONST PIANO_RAW_LINUX_REPORT *);
// Ordinary returned child only, before any retirement. Unknown/partial or
// completed transition cannot be disarmed or used to resume GUI/controllers.
EFI_STATUS PianoLateHandoffDisarm(PIANO_LATE_HANDOFF *);
// Uninstall only after ordinary Disarm, with no retirement/attempt remaining.
// Failure retains provider/image lifetime and invokes the Root fail-stop.
EFI_STATUS PianoLateHandoffShutdown(PIANO_LATE_HANDOFF *);
