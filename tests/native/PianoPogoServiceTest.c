// SPDX-License-Identifier: BSD-2-Clause-Patent
// Check the packet wait and release order without serial transfers or MMIO.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../../uefi/components/pogo-product/PianoPogoService.c"

STATIC EFI_STATUS SerialStatus,BoardStatus;
STATIC UINTN SerialCloses,BoardReleases;
STATIC UINT64 ClockUs;

BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){(VOID)Level;return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){(VOID)Level;(VOID)Format;}
VOID *EFIAPI CopyMem(VOID *Destination,CONST VOID *Source,UINTN Bytes){return memcpy(Destination,Source,Bytes);}
INTN EFIAPI CompareMem(CONST VOID *First,CONST VOID *Second,UINTN Bytes){return memcmp(First,Second,Bytes);}

UINT64 EFIAPI GetPerformanceCounter(VOID){return ClockUs;}
BOOLEAN PianoPogoBoardIrqPending(PIANO_POGO_BOARD *Board){(VOID)Board;abort();}
BOOLEAN PianoPogoFrameValid(CONST UINT8 *Frame,UINTN Bytes){(VOID)Frame;(VOID)Bytes;abort();}
EFI_STATUS PianoGeniI2cTransfer(PIANO_GENI_I2C *I2c,UINT8 Address,CONST UINT8 *Write,UINTN WriteBytes,
  UINT8 *Read,UINTN ReadBytes,UINT32 TimeoutUs) {
  (VOID)I2c;(VOID)Address;(VOID)Write;(VOID)WriteBytes;(VOID)Read;(VOID)ReadBytes;(VOID)TimeoutUs;abort();
}

EFI_STATUS PianoGeniI2cClose(PIANO_GENI_I2C *I2c) {
  assert(I2c==&mPogo.I2c);++SerialCloses;return SerialStatus;
}

EFI_STATUS PianoPogoBoardRelease(PIANO_POGO_BOARD *Board,PIANO_POGO_BOARD_RELEASE *Report) {
  assert(Board==&mPogo.Board && SerialCloses==1 && SerialStatus==EFI_SUCCESS);
  ++BoardReleases;
  *Report=(PIANO_POGO_BOARD_RELEASE){.Attempted=TRUE,.Clean=BoardStatus==EFI_SUCCESS,
    .Pins=EFI_SUCCESS,.Rails=EFI_SUCCESS,.Clocks=BoardStatus};
  if(BoardStatus==EFI_SUCCESS)Board->ClockHeld=0;
  return BoardStatus;
}

STATIC VOID Fresh(EFI_STATUS Serial,EFI_STATUS Board) {
  memset(&mPogo,0,sizeof(mPogo));SerialCloses=BoardReleases=0;
  SerialStatus=Serial;BoardStatus=Board;
  mPogo.Board.ClockHeld=31;mPogo.Board.PowerHandle=(VOID *)1;
  mPogo.Board.RailsOn=mPogo.Board.McuPinsReady=TRUE;
  mPogo.Board.Pins[0].Changed=TRUE;
}

int main(VOID) {
  mPogo.Frequency=1000000;
  mPogo.CounterEnd=MAX_UINT64;
  ClockUs=3500;mPogo.NextUs=4000;
  assert(PianoPogoStartupStallUs(1000)==1000);
  mPogo.Step=StepData;
  assert(PianoPogoStartupStallUs(1000)==500);
  assert(PianoPogoStartupStallUs(100)==100);
  ClockUs=4500;
  assert(PianoPogoStartupStallUs(1000)==1);
  CONST EFI_STATUS Failures[]={EFI_TIMEOUT,EFI_DEVICE_ERROR,EFI_ACCESS_DENIED};
  PIANO_POGO_BACKEND_STOP Stop;
  for(UINTN I=0;I<ARRAY_SIZE(Failures);++I) {
    Fresh(Failures[I],EFI_SUCCESS);
    assert(BackendStop(NULL,&Stop)==Failures[I]);
    assert(!Stop.Quiet && !Stop.Clean && Stop.Retained);
    assert(!BoardReleases && SerialCloses==1 && !mPogo.BoardRelease.Attempted);
    assert(mPogo.Board.ClockHeld==31 && mPogo.Board.PowerHandle==(VOID *)1);
    assert(mPogo.Board.RailsOn && mPogo.Board.McuPinsReady && mPogo.Board.Pins[0].Changed);
    assert(mPogo.BoardRelease.Pins==EFI_NOT_READY && mPogo.BoardRelease.Rails==EFI_NOT_READY &&
      mPogo.BoardRelease.Clocks==EFI_NOT_READY);
    SerialStatus=EFI_SUCCESS;
    assert(BackendStop(NULL,&Stop)==Failures[I] && SerialCloses==1 && !BoardReleases);
    assert(!Stop.Quiet && Stop.Retained);
  }
  Fresh(EFI_SUCCESS,EFI_SUCCESS);
  assert(BackendStop(NULL,&Stop)==EFI_SUCCESS && Stop.Quiet && Stop.Clean && !Stop.Retained);
  assert(SerialCloses==1 && BoardReleases==1 && !mPogo.Board.ClockHeld);
  assert(BackendStop(NULL,&Stop)==EFI_SUCCESS && SerialCloses==1 && BoardReleases==1);
  Fresh(EFI_SUCCESS,EFI_DEVICE_ERROR);
  assert(BackendStop(NULL,&Stop)==EFI_DEVICE_ERROR && Stop.Quiet && !Stop.Clean && Stop.Retained);
  assert(SerialCloses==1 && BoardReleases==1);
  puts("Pogo service: serial close precedes board release; failed serial close retains power, pins and clocks.");
  return 0;
}
