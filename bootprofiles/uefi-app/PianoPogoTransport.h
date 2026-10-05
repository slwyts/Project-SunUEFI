// SPDX-License-Identifier: BSD-2-Clause-Patent
// Diagnostic runtime-read scaffold. It does not open or call OEM protocols.
#ifndef PIANO_POGO_TRANSPORT_H
#define PIANO_POGO_TRANSPORT_H
#include <Uefi.h>
#include "PianoPogoReport.h"

#ifndef PIANO_POGO_RUNTIME_READ_EXPERIMENT
#define PIANO_POGO_RUNTIME_READ_EXPERIMENT 0
#endif
#if PIANO_POGO_RUNTIME_READ_EXPERIMENT != 0 && PIANO_POGO_RUNTIME_READ_EXPERIMENT != 1
#error PIANO_POGO_RUNTIME_READ_EXPERIMENT must be 0 or 1
#endif

#define PIANO_POGO_READ_DEADLINE_US 10000U
#define PIANO_POGO_READ_INTERVAL_US 1000U
#define PIANO_POGO_READ_BUDGET 64U

// Snapshot supplied by an image-bounds-checked caller. UINT64 encodes ARM64 ABI;
// these are addresses to inspect, never function pointers to invoke.
typedef struct {
  UINT64 Revision;
  UINT64 Method[5];
} PIANO_POGO_NATIVE_INTERFACE_SNAPSHOT;

typedef struct {
  UINT8 NativeImageSha256[32];
  UINT8 CapturedDtbSha256[32];
  UINT64 SeBase;
  UINT64 WrapperBase;
  UINT32 SeBytes;
  UINT32 WrapperBytes;
  UINT32 BusHz;
  UINT32 SdaGpio;
  UINT32 SclGpio;
  UINT32 ReadyGpio;
  UINT32 ResetGpio;
  UINT32 StatusGpio;
  UINT32 SleepGpio;
  UINT32 IoMicrovolts;
  UINT32 McuMicrovolts;
  BOOLEAN NativeAbiVerified;
  BOOLEAN NativeBusBoundToSe6Verified;
  BOOLEAN NativeConfigurationVerified;
  BOOLEAN GpioElectricalContractVerified;
  BOOLEAN ClockMuxAndSupplyStateVerified;
  BOOLEAN ExistingRuntimeSessionVerified;
  BOOLEAN BackendDeadlineVerified;
  BOOLEAN FifoPioOnlyVerified;
} PIANO_POGO_RUNTIME_CONTRACT;

// The backend must perform ONLY slave 0x4c write1(register 0x4c), repeated START,
// read68, STOP, synchronously, and stop by DeadlineUs. No open/close/reset/
// bootrom/auth/power/SE-firmware/DMA actions. There is no implemented backend.
// An outer timestamp check cannot make a blocking OEM call interruptible.
typedef EFI_STATUS (EFIAPI *PIANO_POGO_RUNTIME_READ)(
  IN VOID *Context, IN UINT64 DeadlineUs,
  OUT UINT8 Frame[PIANO_POGO_FRAME_BYTES], OUT UINTN *BytesRead);
typedef UINT64 (EFIAPI *PIANO_POGO_MONOTONIC_US)(IN VOID *Context);

typedef struct {
  PIANO_POGO_RUNTIME_READ Read;
  PIANO_POGO_MONOTONIC_US Now;
  VOID *Context;
  UINT64 LastClock;
  UINT64 LastRead;
  UINT32 ReadAttempts;
  UINT32 AcceptedReads;
  EFI_STATUS LastStatus;
  BOOLEAN Enabled;
  BOOLEAN InRead;
  BOOLEAN HasRead;
  BOOLEAN Failed;
  BOOLEAN Stopped;
} PIANO_POGO_TRANSPORT;

EFI_STATUS PianoPogoInspectNativeInterface(
  IN CONST PIANO_POGO_NATIVE_INTERFACE_SNAPSHOT *Snapshot,
  IN UINT64 ImageBase, IN UINT64 ImageBytes, IN UINT64 InterfaceAddress);
EFI_STATUS PianoPogoValidateRuntimeContract(IN CONST PIANO_POGO_RUNTIME_CONTRACT *Contract);
EFI_STATUS PianoPogoInitializeTransport(
  OUT PIANO_POGO_TRANSPORT *Transport, IN BOOLEAN ExplicitEnable,
  IN CONST PIANO_POGO_RUNTIME_CONTRACT *Contract,
  IN PIANO_POGO_RUNTIME_READ Read, IN PIANO_POGO_MONOTONIC_US Now, IN VOID *Context);
EFI_STATUS PianoPogoPollRuntime(
  IN OUT PIANO_POGO_TRANSPORT *Transport, IN OUT PIANO_POGO_INPUT *Input,
  IN BOOLEAN VerifiedDataReady);
// Software stop only: never close native handles or change GPIO/supplies.
EFI_STATUS PianoPogoStopTransport(IN OUT PIANO_POGO_TRANSPORT *Transport);
#endif
