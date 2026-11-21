/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <ntddk.h>
#include "gcc_vote.h"
#include "piano_qup_power.h"

/* Owned by the platform GCC device, not by each SPB controller. Its one
 * mapped APSS word and spinlock must also serve any PCIe/USB/other clock
 * client using that same word. PnP synchronizes prepare/removal with users.
 */
typedef struct {
    PULONG registers[QUP_CLOCK_REGS], vote_register;
    struct gcc_vote_word votes;
    struct piano_qup_power power;
    KSPIN_LOCK lock;
} PIANO_QUP_GCC;

NTSTATUS PianoQupPrepareGcc(PIANO_QUP_GCC *gcc, const CM_PARTIAL_RESOURCE_DESCRIPTOR *translated,
                           PVOID gcc_mapping);
NTSTATUS PianoQupReleaseGcc(PIANO_QUP_GCC *gcc);
int PianoGccBranchVote(PIANO_QUP_GCC *gcc, unsigned bit, int acquire);
NTSTATUS PianoQupPower(PIANO_QUP_GCC *gcc, const struct piano_qup_board_io *board, int enable);
