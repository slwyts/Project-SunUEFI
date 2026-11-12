// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoUfsBoundedBlock.h"
// Private diagnostic-profile APIs, never a registered arbitrary-write protocol.
CONST PIANO_UFS_WINDOW_STATE *PianoUfsBoundedTransportState(VOID);
EFI_HANDLE PianoUfsBoundedTransportHandle(VOID);
// Disconnect FAT consumers while still live (their Stop may flush), then close
// public IO even if Stop fails (see ConsumersDetached/DisconnectStatus). Raw callbacks remain restricted to the same gap; caller owns the
// complete whole-gap restore ledger and holds Recovery.Acquire until complete.
EFI_STATUS PianoUfsBoundedTransportCloseForRecovery(PIANO_UFS_WINDOW_IO *Recovery);
EFI_STATUS PianoUfsBoundedTransportVerifyRestored(VOID);
