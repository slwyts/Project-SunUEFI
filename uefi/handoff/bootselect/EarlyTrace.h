/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PIANO_EARLY_TRACE_H
#define PIANO_EARLY_TRACE_H
#include <stdint.h>

#ifndef PIANO_BOOTSELECT_TRACE_ID
#define PIANO_BOOTSELECT_TRACE_ID "host-test"
#endif

#define PIANO_EARLY_TRACE_RAM_BYTES 0x200000U
#define PIANO_EARLY_TRACE_HEADER_BYTES 12U
#define PIANO_EARLY_TRACE_CAPACITY (PIANO_EARLY_TRACE_RAM_BYTES - PIANO_EARLY_TRACE_HEADER_BYTES)
#define PIANO_EARLY_TRACE_ENTERED 1U
#define PIANO_EARLY_TRACE_METADATA_VALID 2U
#define PIANO_EARLY_TRACE_BEFORE_FDT 3U
#define PIANO_EARLY_TRACE_SELECTED_STOCK 4U
#define PIANO_EARLY_TRACE_SELECTED_UEFI 5U
#define PIANO_EARLY_TRACE_BEFORE_STOCK_JUMP 6U

/* Caller must already have its private stack and disabled DAIF, and run at EL1.
 * Append only to an existing no-ECC DBG_C ring. NS access and cold-reset retention
 * need device evidence; this function cannot establish either permission.
 */
void PianoBootSelectTrace(unsigned Stage, uint64_t ImageBase, uint64_t Dtb);
void PianoBootSelectCleanPoC(void *Start, uint64_t Bytes);
#ifdef HOSTTEST
extern void *PianoBootSelectTraceHostRam;
#endif
#endif
