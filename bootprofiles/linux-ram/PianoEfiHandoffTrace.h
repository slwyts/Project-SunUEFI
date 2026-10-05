// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PIANO_EFI_HANDOFF_TRACE_H
#define PIANO_EFI_HANDOFF_TRACE_H
#include <Uefi.h>

// Call before StartImage. No allocation occurs in any trace function.
EFI_STATUS PianoEfiHandoffTraceInstall (EFI_BOOT_SERVICES *Services);
// Only permitted before successful ExitBootServices, and while both slots
// are still owned by this wrapper. Do not unload while restore is refused.
EFI_STATUS PianoEfiHandoffTraceRestore (VOID);
BOOLEAN PianoEfiHandoffTraceExited (VOID);
#endif
