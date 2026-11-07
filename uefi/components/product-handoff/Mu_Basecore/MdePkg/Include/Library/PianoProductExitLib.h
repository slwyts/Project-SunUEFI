// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Protocol/PianoProductExit.h>
// Only native CoreExitBootServices calls this before BeforeNotify/Timer0/map
// termination. Null retains legacy behavior; real product binding fails closed
// for unarmed/foreign images and wrong TPL without stopping running services.
EFI_STATUS EFIAPI PianoProductBeforeExitBootServices(EFI_HANDLE,
 CONST EFI_LOADED_IMAGE_PROTOCOL *,EFI_TPL,UINTN CallerMapKey,UINTN CurrentMapKey,BOOLEAN BeforeNotified);
// Native map-termination result. After retirement, only SUCCESS or the UEFI
// stale-map retry result may return to the OS; other failures stop execution.
EFI_STATUS EFIAPI PianoProductExitBootServicesResult(EFI_STATUS,BOOLEAN StaleMapKey);
