// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

// Dtb must be the caller's owned, expanded handoff copy. No target read,
// allocation, physical address inference or source-DTB mutation occurs here.
EFI_STATUS PianoPanelApplyFactory(VOID *Dtb,UINTN Capacity,UINT32 FactoryPanel);
EFI_STATUS PianoPanelSelectDtb(VOID *Dtb,UINTN Capacity);
