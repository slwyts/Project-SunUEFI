// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "../uefi-app/PianoFastbootLaunch.h"
#include <Protocol/LoadFile2.h>
#include <Protocol/DevicePath.h>
#define PIANO_LINUX_LOW_SOURCE_BUDGET (64ULL*1024*1024)
typedef struct {
 UINT32 Revision;
 EFI_STATUS Status;
 UINT64 BootEpoch,DramBytes,NormalBytes;
 UINT32 UnresolvedReservations;
 BOOLEAN FullDdr,FixedReservations,DynamicReservations,RuntimeRegions,CacheVerified,OwnershipVerified;
} PIANO_LINUX_MEMORY_PROOF;
typedef struct {
 UINT32 Revision,ExpectedOwners,RetiredOwners,AbsentOwners;
 EFI_STATUS Status;
 BOOLEAN Clean,Retained,NoDma,AtApplication;
} PIANO_LINUX_RETIRE_PROOF;
typedef struct {
 VOID *Context;
 EFI_BOOT_SERVICES *Services;
 EFI_SYSTEM_TABLE *SystemTable;
 EFI_HANDLE ParentImage;
 UINT64 MaxKernelBytes,MaxLoadedBytes,MaxDtbBytes,MaxInitrdBytes,ExpectedDramBytes;
 UINT32 ExpectedOwners;
 CONST CHAR16 *CommandLine;
 BOOLEAN (*BootServicesAlive)(VOID *);
 EFI_STATUS (*ServiceSlice)(VOID *,UINTN BudgetUs);
 // Actual live full-memory/ownership proof, not an authorization bool. Current
 // incomplete platform callback returns NOT_READY; no Start/retire follows.
 EFI_STATUS (*CheckMemory)(VOID *,CONST VOID *Dtb,UINTN Bytes,PIANO_LINUX_MEMORY_PROOF *);
 // Trusted platform code checks the report against its real live contract.
 // User-supplied booleans/old planning snapshots cannot grant Start permission.
 EFI_STATUS (*ValidateMemory)(VOID *,CONST PIANO_LINUX_MEMORY_PROOF *);
 // Called once at APP after all CPU sources/FDT/LoadFile2/options/image are
 // prepared. Keeps background USB up until this explicit OS transition.
 EFI_STATUS (*PrepareHandoff)(VOID *,CONST PIANO_LINUX_MEMORY_PROOF *,PIANO_LINUX_RETIRE_PROOF *);
 EFI_STATUS (*ValidateRetired)(VOID *,CONST PIANO_LINUX_RETIRE_PROOF *);
 VOID (*FailStop)(VOID *,EFI_STATUS); // CPU-only, never returns; EBS/unknown only
} PIANO_LINUX_EFI_ENV;
typedef struct {VENDOR_DEVICE_PATH Vendor;EFI_DEVICE_PATH_PROTOCOL End;} PIANO_LINUX_INITRD_PATH;
typedef struct {
 UINT32 Signature;
 BOOLEAN Busy,Retained,BeforeEbs,Ebs,StartCalled,StartReturned,AutoUnloaded,OptionsInstalled,FdtInstalled,InitrdInstalled,OwnersRetired;
 EFI_STATUS Status,CleanupStatus,ImageExitStatus;
 PIANO_LINUX_EFI_ENV Env;
 PIANO_LAUNCH_BLOB Sources[3];VOID *Owners[3],*Loans[3];CONST VOID *Views[3];
 PIANO_BOOT_IMAGE Kernel;
 PIANO_LINUX_MEMORY_PROOF Memory;PIANO_LINUX_RETIRE_PROOF Retire;
 EFI_HANDLE Image,InitrdHandle;
 EFI_EVENT BeforeEvent,ExitEvent;
 EFI_LOAD_FILE2_PROTOCOL Load;
 PIANO_LINUX_INITRD_PATH Path;
 VOID *FdtCopy,*OldFdt,*OptionsCopy,*OldOptions,*ExitData;
 UINTN FdtCapacity,ExitBytes;UINT32 OptionsBytes,OldOptionsBytes;
 VOID *LoadedIdentity,*ImageBaseIdentity;UINT64 ImageSizeIdentity;
} PIANO_LINUX_EFI_SESSION;
// Driver-lifetime zeroed context. Three blobs are validated immutable CPU-file
// snapshots. Run takes each once, borrows contiguous views, and retains leases
// until image/protocol/table users are gone. It never reads UFS after retire.
EFI_STATUS PianoLinuxEfiSessionRun(PIANO_LINUX_EFI_SESSION *,CONST PIANO_LINUX_EFI_ENV *,
 CONST PIANO_LAUNCH_BLOB *Kernel,CONST PIANO_LAUNCH_BLOB *Dtb,CONST PIANO_LAUNCH_BLOB *Initrd);
