// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoProductOwners.h"
#include "../components/os-boot/PianoBootFileSource.h"
#include "../components/os-boot/PianoLinuxEfiSession.h"
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#define PIANO_PRODUCT_OS_REVISION 1U
typedef enum {PianoProductOsNone=0,PianoProductOsStable=1,PianoProductOsNext=2} PIANO_PRODUCT_OS_FAMILY;
typedef struct {
  UINT64 ImageBytes,DtbBytes,InitrdBytes;
  UINT8 ImageSha256[32],DtbSha256[32],InitrdSha256[32];
} PIANO_PRODUCT_OS_PIN;
typedef struct {
  UINT32 Revision;VOID *Context;
  EFI_BOOT_SERVICES *Services;EFI_SYSTEM_TABLE *SystemTable;EFI_HANDLE ParentImage;
  PIANO_PRODUCT_RUNTIME_PROTOCOL *Runtime;PIANO_PRODUCT_OWNERS *Owners;
  BOOLEAN (*BootServicesAlive)(VOID *);
  // Root verifies actual approved product reservation/UUID/state. It returns
  // the already-published exact FAT BlockIO; no scan of Android filesystems.
  EFI_STATUS (*ApprovedVolume)(VOID *,EFI_BLOCK_IO_PROTOCOL **,UINT8 VolumeUuid[16]);
  // Root checks its actual Policy selection/sequence and owner ledger, not a
  // caller-provided permission boolean. Called again before NativeLateArm.
  EFI_STATUS (*ValidateSelection)(VOID *,PIANO_PRODUCT_OS_FAMILY,UINT64 Sequence);
  PIANO_CPU_INPUT_ENV Cpu;
  PIANO_LINUX_EFI_ENV Linux;
  PIANO_PRODUCT_OS_PIN Stable,Next; // builder pins of assembled immutable bytes
} PIANO_PRODUCT_OS_ENV;
typedef struct {
  UINT32 Revision;PIANO_PRODUCT_OS_FAMILY Family;
  EFI_STATUS Status,Memory,Volume,Files,Session,Cleanup;
  UINT64 Sequence,Attempts;
  BOOLEAN Busy,Retained,ServicesLost,FilesLoaded,StartCalled,OwnersRetired;
} PIANO_PRODUCT_OS_REPORT;
typedef struct {
  UINT32 Signature;PIANO_PRODUCT_OS_ENV Env;PIANO_PRODUCT_OS_REPORT Report;
  EFI_EVENT Exit;EFI_HANDLE FileSystem;BOOLEAN FileIoActive;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs;EFI_BLOCK_IO_PROTOCOL *Block;
  EFI_DEVICE_PATH_PROTOCOL *Path;UINT8 VolumeUuid[16];
  PIANO_CPU_INPUT_ENV Cpu,FileCpu;PIANO_LINUX_EFI_ENV Linux;
  PIANO_BOOT_FILE_SOURCE Files[3];PIANO_BOOT_SOURCE Readers[3];PIANO_LAUNCH_BLOB Blobs[3];
  PIANO_LINUX_EFI_SESSION Session;
} PIANO_PRODUCT_OS_CONTROLLER;
// Zeroed driver-lifetime context; Initialize binds a real persistent Root env.
// A not-ready preflight never stops policy/USB/UFS or retires any owner.
EFI_STATUS PianoProductOsInitialize(PIANO_PRODUCT_OS_CONTROLLER *,CONST PIANO_PRODUCT_OS_ENV *);
// Trusted parent APP call after selected child has cooperatively returned.
// No arbitrary path, SFS selection, permission flag, reset or raw OS jump.
EFI_STATUS PianoProductOsRun(PIANO_PRODUCT_OS_CONTROLLER *,PIANO_PRODUCT_OS_FAMILY,UINT64 Sequence);
