// SPDX-License-Identifier: BSD-2-Clause-Patent
// GENI SPI master (mode 0, 8-bit words, CS0) in FIFO/PIO mode. Never loads
// SE firmware, selects GSI or maps DMA
#pragma once
#include "PianoGeniI2cPio.h"

#define PIANO_GENI_SE2_BASE 0xA88000U
#define PIANO_GENI_PROTOCOL_SPI 1U

typedef struct {
  PIANO_GENI_PIO_IO Io;
  BOOLEAN Open,Broken;
  UINT32 Firmware,InterfaceDisable,TxDepth,RxDepth,SourceHz,Divider;
  UINT32 SavedOffset[PIANO_GENI_SAVED_REGISTERS],SavedValue[PIANO_GENI_SAVED_REGISTERS];
  UINT32 SavedCount;
  UINT32 Transfers,Failures,LastIrq;
  EFI_STATUS LastStatus,CloseStatus;
} PIANO_GENI_SPI;

// EFI_UNSUPPORTED with no register written when the SE is not SPI or is
// GSI-only. Picks the smallest divider that keeps SCLK at or below MaxHz
EFI_STATUS PianoGeniSpiOpen(OUT PIANO_GENI_SPI *Spi,IN CONST PIANO_GENI_PIO_IO *Io,IN UINT32 SourceHz,IN UINT32 MaxHz);
// One CS-framed transfer, TX only when Rx is NULL; Bytes <= 64 KiB
EFI_STATUS PianoGeniSpiTransfer(IN OUT PIANO_GENI_SPI *Spi,IN CONST UINT8 *Tx,OUT UINT8 *Rx OPTIONAL,IN UINTN Bytes,IN UINT32 TimeoutUs);
// Restores every register Open changed
EFI_STATUS PianoGeniSpiClose(IN OUT PIANO_GENI_SPI *Spi);
