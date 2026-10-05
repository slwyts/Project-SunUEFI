// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoGeniI2cPio.h"
#ifndef PIANO_POGO_PROBE_EXPERIMENT
#define PIANO_POGO_PROBE_EXPERIMENT 0
#endif
#if PIANO_POGO_PROBE_EXPERIMENT != 0 && PIANO_POGO_PROBE_EXPERIMENT != 1
#error PIANO_POGO_PROBE_EXPERIMENT must be 0 or 1
#endif
typedef struct {
  EFI_STATUS Status;
  UINT32 Gate; // 1=DT, 2=CPU, 3=interfaces, 4=handlers, 5=mapping, 6=clock, 7=counter, 8=reads, 9=cleanup.
  UINT32 Reads,RecoveredFaults;
  UINTN LastAddress;
  UINT64 Elr,Esr,Far;
  UINT32 Wrapper[4];
  PIANO_GENI_PIO_SNAPSHOT Se6;
  BOOLEAN Started,Complete,MmioAttempted,HandlersRetained,Fatal;
  BOOLEAN MemoryAttributePresent,MemoryAttributeCalled;
} PIANO_POGO_PROBE_REPORT;
// Actual native backend: fixed DT/resources, GCD/AT/cache/clock queries, owned
// synchronous/SError guards and direct guarded LDR only. No native I2C Open,
// firmware/clock/rail/GPIO writes or FIFO pop. Profile must not pre-register
// another synchronous/SError handler; existing owners are never replaced.
// Clock-on/GCD/AT gate plus one guarded LDR bounds software work; a hardware
// bus transaction that never responds is not interruptible by a C deadline.
EFI_STATUS PianoProbePogo(CONST VOID *TrustedFdt);
CONST PIANO_POGO_PROBE_REPORT *PianoPogoProbeReport(VOID);
// Stored summary only, safe after a failure; no MMIO/protocol/table walk.
VOID PianoPogoProbeReemit(VOID);
