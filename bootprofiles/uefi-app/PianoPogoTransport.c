// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoPogoTransport.h"

STATIC CONST UINT8 NativeSha[32] = {
  0x7e,0xdc,0x3c,0x4c,0xc5,0x18,0x25,0x53,0x0f,0x53,0xe0,0xbd,0x61,0x6b,0x04,0x6a,
  0xba,0xd6,0x6c,0x74,0x0b,0x34,0xea,0xfa,0xd3,0x8e,0x14,0xa2,0x5f,0xcf,0x57,0xb0
};
STATIC CONST UINT8 DtbSha[32] = {
  0xa4,0xb5,0x5d,0xd3,0xb7,0x7e,0x69,0xbe,0x45,0x1a,0xaf,0x22,0x63,0xc7,0x6f,0x54,
  0x96,0xc9,0x33,0x25,0x76,0x7e,0x49,0xf7,0x48,0xee,0x49,0x57,0x06,0x11,0xe8,0xd7
};
STATIC BOOLEAN ShaEqual(CONST UINT8 *A, CONST UINT8 *B) {
  UINTN I; UINT8 Difference = 0;
  for (I=0; I<32; I++) { Difference |= (UINT8)(A[I]^B[I]); }
  return Difference == 0;
}

EFI_STATUS PianoPogoInspectNativeInterface(
  CONST PIANO_POGO_NATIVE_INTERFACE_SNAPSHOT *Snapshot,
  UINT64 ImageBase, UINT64 ImageBytes, UINT64 InterfaceAddress) {
  STATIC CONST UINT32 Rva[5] = {0x2720,0x27d8,0x2c60,0x28d4,0x2764};
  UINTN I;
  if (Snapshot==NULL) { return EFI_INVALID_PARAMETER; }
  if (ImageBytes!=0xf000 || ImageBase>MAX_UINT64-ImageBytes ||
      InterfaceAddress!=ImageBase+0xc0e8 || Snapshot->Revision!=0x10000) {
    return EFI_COMPROMISED_DATA;
  }
  for (I=0; I<5; I++) {
    if (Snapshot->Method[I]!=ImageBase+Rva[I]) { return EFI_COMPROMISED_DATA; }
  }
  return EFI_SUCCESS; // Metadata only; neither ABI signatures nor hardware proof.
}

EFI_STATUS PianoPogoValidateRuntimeContract(CONST PIANO_POGO_RUNTIME_CONTRACT *C) {
  if (C==NULL) { return EFI_INVALID_PARAMETER; }
  if (!ShaEqual(C->NativeImageSha256,NativeSha) || !ShaEqual(C->CapturedDtbSha256,DtbSha)) {
    return EFI_SECURITY_VIOLATION;
  }
  if (C->SeBase!=0xa98000 || C->SeBytes!=0x4000 ||
      C->WrapperBase!=0xac0000 || C->WrapperBytes!=0x2000 || C->BusHz!=1000000 ||
      C->SdaGpio!=56 || C->SclGpio!=57 || C->ReadyGpio!=97 ||
      C->ResetGpio!=188 || C->StatusGpio!=95 || C->SleepGpio!=3 ||
      C->IoMicrovolts!=1800000 || C->McuMicrovolts!=3300000) {
    return EFI_COMPROMISED_DATA;
  }
  if (C->NativeAbiVerified!=TRUE || C->NativeBusBoundToSe6Verified!=TRUE ||
      C->NativeConfigurationVerified!=TRUE || C->GpioElectricalContractVerified!=TRUE ||
      C->ClockMuxAndSupplyStateVerified!=TRUE || C->ExistingRuntimeSessionVerified!=TRUE ||
      C->BackendDeadlineVerified!=TRUE || C->FifoPioOnlyVerified!=TRUE) { return EFI_NOT_READY; }
  return EFI_SUCCESS; // Caller attestations, not measurements made by this helper.
}

EFI_STATUS PianoPogoInitializeTransport(
  PIANO_POGO_TRANSPORT *T, BOOLEAN ExplicitEnable,
  CONST PIANO_POGO_RUNTIME_CONTRACT *Contract,
  PIANO_POGO_RUNTIME_READ Read, PIANO_POGO_MONOTONIC_US Now, VOID *Context) {
  UINTN I; EFI_STATUS Status;
  if (T==NULL) { return EFI_INVALID_PARAMETER; }
  for (I=0; I<sizeof(*T); I++) { ((UINT8 *)T)[I]=0; }
  T->LastStatus=EFI_UNSUPPORTED;
  if (!PIANO_POGO_RUNTIME_READ_EXPERIMENT || !ExplicitEnable) { return EFI_UNSUPPORTED; }
  if (ExplicitEnable!=TRUE) { T->LastStatus=EFI_INVALID_PARAMETER; return EFI_INVALID_PARAMETER; }
  Status=PianoPogoValidateRuntimeContract(Contract);
  if (EFI_ERROR(Status)) { T->LastStatus=Status; return Status; }
  if (Read==NULL || Now==NULL) { T->LastStatus=EFI_INVALID_PARAMETER; return EFI_INVALID_PARAMETER; }
  T->Read=Read; T->Now=Now; T->Context=Context;
  T->LastClock=Now(Context); T->Enabled=TRUE; T->LastStatus=EFI_SUCCESS;
  return EFI_SUCCESS;
}

EFI_STATUS PianoPogoPollRuntime(PIANO_POGO_TRANSPORT *T, PIANO_POGO_INPUT *Input,
                                BOOLEAN VerifiedDataReady) {
  UINT8 Frame[PIANO_POGO_FRAME_BYTES];
  UINTN I, Bytes=0;
  UINT64 Start, Finish;
  EFI_STATUS Status;
  if (T==NULL || Input==NULL) { return EFI_INVALID_PARAMETER; }
  if (!PIANO_POGO_RUNTIME_READ_EXPERIMENT || !T->Enabled || T->Stopped) { return EFI_UNSUPPORTED; }
  if (T->InRead) { return EFI_NOT_READY; }
  if (T->Failed) { return EFI_ABORTED; }
  if (T->ReadAttempts>=PIANO_POGO_READ_BUDGET) { T->LastStatus=EFI_OUT_OF_RESOURCES; return EFI_OUT_OF_RESOURCES; }
  if (VerifiedDataReady!=TRUE && VerifiedDataReady!=FALSE) {
    T->LastStatus=EFI_INVALID_PARAMETER; return EFI_INVALID_PARAMETER;
  }
  if (T->Read==NULL || T->Now==NULL) { T->Failed=TRUE; T->LastStatus=EFI_COMPROMISED_DATA; return EFI_COMPROMISED_DATA; }
  T->InRead=TRUE;
  Start=T->Now(T->Context);
  if (Start<T->LastClock || Start>MAX_UINT64-PIANO_POGO_READ_DEADLINE_US) {
    Status=EFI_COMPROMISED_DATA; T->Failed=TRUE; goto Done;
  }
  T->LastClock=Start;
  if (!VerifiedDataReady || (T->HasRead && Start-T->LastRead<PIANO_POGO_READ_INTERVAL_US)) {
    Status=EFI_NOT_READY; goto Done;
  }
  for (I=0; I<sizeof(Frame); I++) { Frame[I]=0; }
  T->ReadAttempts++; T->HasRead=TRUE; T->LastRead=Start;
  Status=T->Read(T->Context,Start+PIANO_POGO_READ_DEADLINE_US,Frame,&Bytes);
  Finish=T->Now(T->Context);
  if (Finish<Start) { Status=EFI_COMPROMISED_DATA; }
  else if (Finish>Start+PIANO_POGO_READ_DEADLINE_US) { Status=EFI_TIMEOUT; }
  T->LastClock=Finish;
  if (!EFI_ERROR(Status) && Bytes!=sizeof(Frame)) { Status=EFI_BAD_BUFFER_SIZE; }
  if (!EFI_ERROR(Status)) {
    Status=PianoPogoFeedFrame(Input,Frame,sizeof(Frame));
    if (!EFI_ERROR(Status)) { T->AcceptedReads++; }
  }
  // No runtime report or authentication bytes survive in transport storage.
  for (I=0; I<sizeof(Frame); I++) { ((volatile UINT8 *)Frame)[I]=0; }
  if (EFI_ERROR(Status)) { T->Failed=TRUE; } // No retry, reset, reload or alternate bus.
Done:
  T->LastStatus=Status; T->InRead=FALSE;
  return Status;
}

EFI_STATUS PianoPogoStopTransport(PIANO_POGO_TRANSPORT *T) {
  if (T==NULL) { return EFI_INVALID_PARAMETER; }
  if (T->InRead) { return EFI_NOT_READY; }
  T->Stopped=TRUE; T->Enabled=FALSE; T->Read=NULL; T->Now=NULL; T->Context=NULL;
  return EFI_SUCCESS;
}
