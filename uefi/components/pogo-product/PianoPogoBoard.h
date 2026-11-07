// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board resources of the WN8030 keyboard MCU behind QUP1 SE6; Release returns
// each one to the state recorded when it was taken
#pragma once
#include <Uefi.h>

#define PIANO_POGO_CLOCKS 5U
#define PIANO_POGO_PINS 5U
#define PIANO_POGO_VOTES 6U

typedef struct {
  UINT32 Gpio,Ctl,Io,IntrCfg;
  BOOLEAN Saved,Changed;
} PIANO_POGO_PIN;

typedef struct {
  VOID *Clock;
  UINTN ClockIds[PIANO_POGO_CLOCKS];
  UINT32 ClockHeld;            // bit per clock enabled by this owner
  UINT32 ClockWasOn;           // native IsClockEnabled before EnableClock
  UINT32 SeSourceHz;
  VOID *Rpmh,*PowerHandle;
  struct {UINT32 Address,Before,After;BOOLEAN Changed;} Votes[PIANO_POGO_VOTES];
  UINT32 VoteCount;
  BOOLEAN RailsOn,McuPinsReady,IrqArmed,EdgeSeen;
  PIANO_POGO_PIN Pins[PIANO_POGO_PINS];
} PIANO_POGO_BOARD;

typedef struct {
  BOOLEAN Attempted,Clean;
  EFI_STATUS Pins,Rails,Clocks;
  UINT32 ClocksReleased,VotesRestored,PinsRestored;
} PIANO_POGO_BOARD_RELEASE;

// One reference on each QUP1 wrapper and SE6 clock; no SE register is touched
EFI_STATUS PianoPogoBoardClocksOn(IN OUT PIANO_POGO_BOARD *Board);
// GPIO56/57 to qup1_se6 (2 mA, no pull), as the stock and Linux pinctrl
EFI_STATUS PianoPogoBoardI2cPins(IN OUT PIANO_POGO_BOARD *Board);
// Reset (GPIO188) and sleep (GPIO3) as outputs held asserted (low), then
// RPMh votes for L11B 1.8 V before L14B 3.3 V, matching the Linux power-on
EFI_STATUS PianoPogoBoardPowerOn(IN OUT PIANO_POGO_BOARD *Board);
// Both lines are active low, as in the Linux keyboard DT
EFI_STATUS PianoPogoBoardSetReset(IN OUT PIANO_POGO_BOARD *Board,IN BOOLEAN Asserted);
EFI_STATUS PianoPogoBoardSetSleep(IN OUT PIANO_POGO_BOARD *Board,IN BOOLEAN Asserted);
// GPIO97 rising-edge latch in TLMM, interrupt delivery left disabled
EFI_STATUS PianoPogoBoardArmIrq(IN OUT PIANO_POGO_BOARD *Board);
// TRUE once per latched rising edge; the latch is acknowledged here
BOOLEAN PianoPogoBoardIrqPending(IN OUT PIANO_POGO_BOARD *Board);
// Mirrors Linux power_off (sleep, reset, 10 ms, 3.3 V off, 1.8 V off), then
// restores every pin and drops this owner's clock references. Each step is
// attempted even after an earlier failure; Clean only if all succeeded
EFI_STATUS PianoPogoBoardRelease(IN OUT PIANO_POGO_BOARD *Board,OUT PIANO_POGO_BOARD_RELEASE *Report);
