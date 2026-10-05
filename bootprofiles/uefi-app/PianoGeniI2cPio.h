// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fixed SE6/slave4c runtime diagnostic. No OEM ABI, firmware/power or DMA API.
#pragma once
#include <Uefi.h>
#ifndef PIANO_GENI_I2C_PIO_EXPERIMENT
#define PIANO_GENI_I2C_PIO_EXPERIMENT 0
#endif
#if PIANO_GENI_I2C_PIO_EXPERIMENT != 0 && PIANO_GENI_I2C_PIO_EXPERIMENT != 1
#error PIANO_GENI_I2C_PIO_EXPERIMENT must be 0 or 1
#endif

#define PIANO_GENI_SE6_BASE 0xA98000U
#define PIANO_GENI_SE6_BYTES 0x4000U
#define PIANO_GENI_RUNTIME_BYTES 68U
#define PIANO_GENI_TOTAL_US 10000U
#define PIANO_GENI_CLEANUP_US 1000U
#define PIANO_GENI_POLL_LIMIT 4096U
#define PIANO_GENI_CLEANUP_LIMIT 512U

typedef struct {
  UINT32 TxLength,TxWord,TxCommand,RxLength,RxCommand;
} PIANO_GENI_RUNTIME_PACKET;
// Pure, argument-free builder; cannot select another slave/register/length.
EFI_STATUS PianoGeniI2cPioBuildRuntimePacket(OUT PIANO_GENI_RUNTIME_PACKET *Packet);

// Callbacks access only the exact, independently mapped SE6 window. They must
// return synchronously and boundedly. No built-in pointer dereference/MMIO.
typedef EFI_STATUS (EFIAPI *PIANO_GENI_READ32)(VOID *Context,UINT32 Offset,UINT32 *Value);
typedef EFI_STATUS (EFIAPI *PIANO_GENI_WRITE32)(VOID *Context,UINT32 Offset,UINT32 Value);
typedef UINT64 (EFIAPI *PIANO_GENI_NOW_US)(VOID *Context);
typedef struct {
  UINTN Base,Bytes;
  VOID *Context;
  PIANO_GENI_READ32 Read32;
  PIANO_GENI_WRITE32 Write32;
  PIANO_GENI_NOW_US NowUs;
} PIANO_GENI_PIO_IO;

typedef struct {
  UINT32 Firmware,InterfaceDisable,Status,DmaMode,Events,IrqRoute;
  UINT32 ClockSelect,MasterClock,SclCounters,ByteGran;
  UINT32 TxPack[2],RxPack[2],HwTx,HwRx,IoLines;
  UINT32 MasterIrq,SecondaryIrq,MasterIrqEnable,MasterControl,SecondaryControl;
  UINT32 TxFifo,RxFifo,TxWatermark,RxWatermark,TxLength,RxLength;
  UINT32 MasterCommand,SecondaryCommand;
} PIANO_GENI_PIO_SNAPSHOT;
// Explicit read-only capture remains available with the experiment disabled.
// It does not read FIFO data (reading RX data consumes it), call native open,
// change a clock/GPIO, or load SE microcode.
EFI_STATUS PianoGeniI2cPioCapture(IN CONST PIANO_GENI_PIO_IO *Io,OUT PIANO_GENI_PIO_SNAPSHOT *Snapshot);

typedef struct {
  UINT32 Firmware,ClockSelect,MasterClock,SclCounters;
  BOOLEAN ExplicitEnable,MappedDeviceMemoryVerified,ExclusiveOwnerVerified;
  BOOLEAN ExistingRuntimeVerified,ClockRateAndMuxVerified,SupplyAndGpioVerified;
  BOOLEAN FirmwareAndPackingVerified,BoundedCallbacksVerified;
} PIANO_GENI_PIO_CONTRACT;
// Caller attestations alone are not hardware proof. Full FW/clock words must
// come from the next physical snapshot, not guessed Linux/native instances.
EFI_STATUS PianoGeniI2cPioValidate(IN CONST PIANO_GENI_PIO_CONTRACT *Contract,IN CONST PIANO_GENI_PIO_SNAPSHOT *Snapshot);

typedef struct {
  UINT32 Signature;
  PIANO_GENI_PIO_IO Io;
  PIANO_GENI_PIO_CONTRACT Contract;
  PIANO_GENI_PIO_SNAPSHOT Before,After;
  UINT64 LastTime,Deadline;
  UINT32 Attempts,CommandsAttempted,TxWordsAttempted,CurrentCommand,Received,Polls,CleanupPolls;
  EFI_STATUS LastStatus,CleanupStatus;
  BOOLEAN InRead,Failed,Quarantined,Touched,CommandAttempted,OwnershipLost,BusMayBeHeld,Clean,ClockBroken;
} PIANO_GENI_I2C_PIO;
// Context must be new zeroed storage. Reinitializing an existing ledger fails.
EFI_STATUS PianoGeniI2cPioInitialize(OUT PIANO_GENI_I2C_PIO *Pio,IN CONST PIANO_GENI_PIO_IO *Io,IN CONST PIANO_GENI_PIO_CONTRACT *Contract);
// One fixed write1(register4c)/repeated START/read68/STOP; no raw write API.
// Deadline is absolute monotonic time, at most 10ms from entry, including
// reserved cleanup. Frame/count are exposed only after complete clean proof.
EFI_STATUS EFIAPI PianoGeniI2cPioReadRuntime(VOID *Context,UINT64 DeadlineUs,UINT8 Frame[PIANO_GENI_RUNTIME_BYTES],UINTN *BytesRead);
