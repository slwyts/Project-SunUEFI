// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoOwnedSmmu.h"

#define PIANO_SPI_GPI_MAX_BYTES 65536U
#define PIANO_SPI_GPI_SAVED_REGS 64U

typedef struct {
  BOOLEAN Started,Open,Busy,Retained,EventAllocated,SerialChanged;
  BOOLEAN ChannelAllocated[2],ChannelStarted[2];
  UINT32 Instance,Divider,SavedCount,SeSavedCount,EventRead,EventWrite;
  UINT32 Transfers,Failures,Events,LastEvent[4],LastStatusReg;
  UINT32 LastGlobalIrq,LastGeneralIrq,LastErrorLog;
  UINT32 ProbeOffset,ProbeValue;
  UINT32 FailureOffset,FailureExpected,FailureReadback;
  BOOLEAN FailureRecorded;
  CONST CHAR8 *Stage;
  struct {UINT32 Offset,Value,Ignore;} Saved[PIANO_SPI_GPI_SAVED_REGS];
  struct {UINT32 Offset,Value;} SeSaved[32];
  UINT64 Frequency,CounterStart,CounterEnd;
  EFI_STATUS Status,CloseStatus,ProbeStatus;
  PIANO_OWNED_SMMU Smmu;
  PIANO_DMA_DEVICE Device;
  PIANO_DMA_BUFFER TxRing,RxRing,EventRing,Tx,Rx;
} PIANO_SPI_GPI;

// QUP1 clocks must remain held until Close succeeds. Only SE2 is supported.
EFI_STATUS PianoGeniSpiGpiOpen(PIANO_SPI_GPI *Spi,CONST VOID *Fdt,UINT32 SourceHz,UINT32 MaxHz);
EFI_STATUS PianoGeniSpiGpiTransfer(PIANO_SPI_GPI *Spi,CONST UINT8 *Tx,UINT8 *Rx,UINTN Bytes,UINT32 TimeoutUs);
// A failed stop keeps the domain, buffers and clocks reachable by the caller.
EFI_STATUS PianoGeniSpiGpiClose(PIANO_SPI_GPI *Spi);
