// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

// Normal startup retains only the already captured SEC report. No SMEM reread,
// exception handlers or visible progress item are needed in DXE.
EFI_STATUS PianoProductCaptureEarlySmem(VOID);
// Optional bounded DXE diagnostic, not part of normal startup. It does not
// publish or authorize DDR. Unknown exception-handler ownership is terminal.
EFI_STATUS PianoProductObserveSmem(VOID);
BOOLEAN PianoProductSmemRetained(VOID);
// Status of the frozen cold-SEC HOB capture. No hardware reread or authority.
EFI_STATUS PianoProductEarlySmemStatus(VOID);
// CPU-only replay of the immutable observation, used immediately before the
// existing fastboot ramlog freeze. Does not reread SMEM or hardware.
EFI_STATUS PianoProductSmemReemit(VOID *Context);
