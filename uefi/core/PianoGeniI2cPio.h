// SPDX-License-Identifier: BSD-2-Clause-Patent
// GENI I2C master in FIFO/PIO mode. Never loads SE firmware, changes the
// FIFO/GSI selection or maps DMA
#pragma once
#include <Uefi.h>

#define PIANO_GENI_SE6_BASE 0xA98000U
#define PIANO_GENI_SE6_BYTES 0x4000U
#define PIANO_GENI_PROTOCOL_I2C 3U
#define PIANO_GENI_SAVED_REGISTERS 28U

// Register access goes through the caller so host tests can model the SE and
// the product can keep every access inside the fixed, mapped SE window
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
// Read-only register capture for logs; never pops the RX FIFO
EFI_STATUS PianoGeniI2cPioCapture(IN CONST PIANO_GENI_PIO_IO *Io,OUT PIANO_GENI_PIO_SNAPSHOT *Snapshot);

typedef struct {
  PIANO_GENI_PIO_IO Io;
  BOOLEAN Open,Broken;
  UINT32 Firmware,InterfaceDisable,TxDepth,RxDepth,SourceHz;
  // Every register Open changes, with its original value, restored by Close
  UINT32 SavedOffset[PIANO_GENI_SAVED_REGISTERS],SavedValue[PIANO_GENI_SAVED_REGISTERS];
  UINT32 SavedCount;
  UINT32 Transfers,Failures,LastIrq,LastGeniStatus,LastIos;
  EFI_STATUS LastStatus,CloseStatus;
} PIANO_GENI_I2C;

// EFI_UNSUPPORTED with no register written when the SE is not I2C or is
// GSI-only; EFI_NOT_READY when another command is active
EFI_STATUS PianoGeniI2cOpen(OUT PIANO_GENI_I2C *I2c,IN CONST PIANO_GENI_PIO_IO *Io,IN UINT32 SourceHz,IN UINT32 BusHz);
// One write, one read, or a write followed by a repeated-start read. Every
// wait is bounded by TimeoutUs; on error the command is cancelled or aborted
// and the FIFOs drained. Lengths are limited to 4 KiB per phase
EFI_STATUS PianoGeniI2cTransfer(IN OUT PIANO_GENI_I2C *I2c,IN UINT8 Address,
  IN CONST UINT8 *Write OPTIONAL,IN UINTN WriteBytes,OUT UINT8 *Read OPTIONAL,IN UINTN ReadBytes,IN UINT32 TimeoutUs);
// Requires an idle engine (cancels a leftover command), restores every saved
// register in reverse order and verifies each readback
EFI_STATUS PianoGeniI2cClose(IN OUT PIANO_GENI_I2C *I2c);
