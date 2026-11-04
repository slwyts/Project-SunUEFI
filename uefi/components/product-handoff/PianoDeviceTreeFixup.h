// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

// EFI Device Tree Fixup Protocol, as consumed by systemd-boot and systemd-stub.
#define EFI_DT_FIXUP_PROTOCOL_GUID \
  {0xe617d64c,0xfe08,0x46da,{0xf4,0xdc,0xbb,0xd5,0x87,0x0c,0x73,0x00}}
#define EFI_DT_FIXUP_PROTOCOL_REVISION 0x00010000ULL
#define EFI_DT_APPLY_FIXUPS  0x00000001U
#define EFI_DT_RESERVE_MEMORY 0x00000002U
#define EFI_DT_INSTALL_TABLE 0x00000004U
typedef struct EFI_DT_FIXUP_PROTOCOL EFI_DT_FIXUP_PROTOCOL;
struct EFI_DT_FIXUP_PROTOCOL {
  UINT64 Revision;
  EFI_STATUS (EFIAPI *Fixup)(EFI_DT_FIXUP_PROTOCOL *,VOID *,UINTN *,UINT32);
};

// ValidateReserved checks the caller's complete DT against the platform's real
// EFI memory map. A fixed reservation already excluded there needs no second
// allocation; an unrepresented range must fail rather than be ignored.
EFI_STATUS PianoDeviceTreeFixupInstall(BOOLEAN (*Alive)(VOID),
  EFI_STATUS (*ValidateReserved)(CONST VOID *,UINTN));
