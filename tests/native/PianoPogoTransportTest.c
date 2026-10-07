// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "PianoPogoTransport.h"

typedef struct {
  UINT64 Clock;
  UINT32 Calls;
  UINT32 ClockCalls;
  UINT32 Mode;
  PIANO_POGO_TRANSPORT *Transport;
  PIANO_POGO_INPUT *Input;
} MOCK;

STATIC UINT64 EFIAPI Now(VOID *Context) {
  MOCK *M=Context; M->ClockCalls++; return M->Clock;
}
STATIC EFI_STATUS EFIAPI Read(VOID *Context, UINT64 Deadline, UINT8 *Frame, UINTN *Bytes) {
  MOCK *M=Context; M->Calls++;
  assert(Deadline==M->Clock+PIANO_POGO_READ_DEADLINE_US);
  assert(M->Transport->InRead);
  if(M->Mode==3) { return EFI_DEVICE_ERROR; }
  if(M->Mode==4) {
    assert(PianoPogoPollRuntime(M->Transport,M->Input,TRUE)==EFI_NOT_READY);
    assert(PianoPogoStopTransport(M->Transport)==EFI_NOT_READY);
  }
  memset(Frame,0,68); Frame[0]=0x57; Frame[2]=1; Frame[3]=5; Frame[6]=4;
  *Bytes=M->Mode==1?67:68;
  if(M->Mode==2) { M->Clock=Deadline+1; }
  if(M->Mode==5) { Frame[3]=0xff; }
  if(M->Mode==6) { M->Clock--; }
  return EFI_SUCCESS;
}
STATIC VOID Hex(UINT8 *Output, CONST CHAR8 *Input) {
  UINTN I; unsigned Byte;
  for(I=0;I<32;I++) { assert(sscanf((CONST char *)Input+I*2,"%2x",&Byte)==1); Output[I]=(UINT8)Byte; }
}
STATIC PIANO_POGO_RUNTIME_CONTRACT Contract(VOID) {
  PIANO_POGO_RUNTIME_CONTRACT C; memset(&C,0,sizeof(C));
  Hex(C.NativeImageSha256,(CONST CHAR8 *)"7edc3c4cc51825530f53e0bd616b046abad66c740b34eafad38e14a25fcf57b0");
  Hex(C.CapturedDtbSha256,(CONST CHAR8 *)"a4b55dd3b77e69be451aaf2263c76f5496c93325767e49f748ee49570611e8d7");
  C.SeBase=0xa98000; C.SeBytes=0x4000; C.WrapperBase=0xac0000; C.WrapperBytes=0x2000;
  C.BusHz=1000000; C.SdaGpio=56; C.SclGpio=57; C.ReadyGpio=97;
  C.ResetGpio=188; C.StatusGpio=95; C.SleepGpio=3; C.IoMicrovolts=1800000; C.McuMicrovolts=3300000;
  C.NativeAbiVerified=TRUE; C.NativeBusBoundToSe6Verified=TRUE; C.NativeConfigurationVerified=TRUE;
  C.GpioElectricalContractVerified=TRUE; C.ClockMuxAndSupplyStateVerified=TRUE;
  C.ExistingRuntimeSessionVerified=TRUE; C.BackendDeadlineVerified=TRUE; C.FifoPioOnlyVerified=TRUE;
  return C;
}
STATIC VOID Interface(VOID) {
  PIANO_POGO_NATIVE_INTERFACE_SNAPSHOT S={0x10000,{0x102720,0x1027d8,0x102c60,0x1028d4,0x102764}};
  UINTN I;
  assert(sizeof(S)==48);
  assert(PianoPogoInspectNativeInterface(&S,0x100000,0xf000,0x10c0e8)==EFI_SUCCESS);
  assert(PianoPogoInspectNativeInterface(&S,MAX_UINT64-0x100,0xf000,0)==EFI_COMPROMISED_DATA);
  assert(PianoPogoInspectNativeInterface(&S,0x100000,0xe000,0x10c0e8)==EFI_COMPROMISED_DATA);
  assert(PianoPogoInspectNativeInterface(&S,0x100000,0xf000,0x10c0f0)==EFI_COMPROMISED_DATA);
  for(I=0;I<5;I++) {
    S.Method[I]++; assert(PianoPogoInspectNativeInterface(&S,0x100000,0xf000,0x10c0e8)==EFI_COMPROMISED_DATA); S.Method[I]--;
  }
  S.Revision++; assert(PianoPogoInspectNativeInterface(&S,0x100000,0xf000,0x10c0e8)==EFI_COMPROMISED_DATA);
}
STATIC VOID Validate(VOID) {
  PIANO_POGO_RUNTIME_CONTRACT C=Contract();
  BOOLEAN *Flags[]={&C.NativeAbiVerified,&C.NativeBusBoundToSe6Verified,&C.NativeConfigurationVerified,
    &C.GpioElectricalContractVerified,&C.ClockMuxAndSupplyStateVerified,&C.ExistingRuntimeSessionVerified,
    &C.BackendDeadlineVerified,&C.FifoPioOnlyVerified};
  UINTN I;
  assert(PianoPogoValidateRuntimeContract(NULL)==EFI_INVALID_PARAMETER);
  assert(PianoPogoValidateRuntimeContract(&C)==EFI_SUCCESS);
  for(I=0;I<sizeof(Flags)/sizeof(Flags[0]);I++) {
    *Flags[I]=FALSE; assert(PianoPogoValidateRuntimeContract(&C)==EFI_NOT_READY);
    *Flags[I]=2; assert(PianoPogoValidateRuntimeContract(&C)==EFI_NOT_READY); *Flags[I]=TRUE;
  }
  C.NativeImageSha256[31]^=1; assert(PianoPogoValidateRuntimeContract(&C)==EFI_SECURITY_VIOLATION); C=Contract();
  C.CapturedDtbSha256[31]^=1; assert(PianoPogoValidateRuntimeContract(&C)==EFI_SECURITY_VIOLATION); C=Contract();
  C.McuMicrovolts=1800000; assert(PianoPogoValidateRuntimeContract(&C)==EFI_COMPROMISED_DATA); C=Contract();
  C.SeBase=0xa94000; assert(PianoPogoValidateRuntimeContract(&C)==EFI_COMPROMISED_DATA); C=Contract();
  C.ReadyGpio=363; assert(PianoPogoValidateRuntimeContract(&C)==EFI_COMPROMISED_DATA); C=Contract();
  C.BusHz=400000; assert(PianoPogoValidateRuntimeContract(&C)==EFI_COMPROMISED_DATA);
}
STATIC VOID Setup(PIANO_POGO_TRANSPORT *T, PIANO_POGO_INPUT *Input, MOCK *M) {
  PIANO_POGO_RUNTIME_CONTRACT C=Contract();
  memset(M,0,sizeof(*M)); M->Clock=100000; M->Transport=T; M->Input=Input; PianoPogoReset(Input);
  assert(PianoPogoInitializeTransport(T,TRUE,&C,Read,Now,M)==EFI_SUCCESS);
}
STATIC VOID GatesAndReads(VOID) {
  PIANO_POGO_RUNTIME_CONTRACT C=Contract(); PIANO_POGO_TRANSPORT T;
  PIANO_POGO_INPUT Input; MOCK M={0}; UINTN I;
  assert(PianoPogoInitializeTransport(&T,FALSE,&C,Read,Now,&M)==EFI_UNSUPPORTED);
  assert(!M.Calls&&!M.ClockCalls&&!T.Enabled);
  if(!PIANO_POGO_RUNTIME_READ_EXPERIMENT) {
    assert(PianoPogoInitializeTransport(&T,TRUE,&C,Read,Now,&M)==EFI_UNSUPPORTED);
    T.Enabled=TRUE; assert(PianoPogoPollRuntime(&T,&Input,TRUE)==EFI_UNSUPPORTED);
    assert(!M.Calls&&!M.ClockCalls); return;
  }
  C.ExistingRuntimeSessionVerified=FALSE;
  assert(PianoPogoInitializeTransport(&T,TRUE,&C,Read,Now,&M)==EFI_NOT_READY);
  assert(!M.ClockCalls&&!M.Calls); C=Contract();
  assert(PianoPogoInitializeTransport(&T,2,&C,Read,Now,&M)==EFI_INVALID_PARAMETER&&!M.ClockCalls);
  assert(PianoPogoInitializeTransport(&T,TRUE,&C,NULL,Now,&M)==EFI_INVALID_PARAMETER);
  Setup(&T,&Input,&M); assert(PianoPogoPollRuntime(&T,&Input,FALSE)==EFI_NOT_READY&&!M.Calls);
  assert(PianoPogoPollRuntime(&T,&Input,2)==EFI_INVALID_PARAMETER&&!M.Calls);
  M.Mode=4; assert(PianoPogoPollRuntime(&T,&Input,TRUE)==EFI_SUCCESS);
  assert(M.Calls==1&&T.AcceptedReads==1&&Input.KeyCount==1&&!T.InRead);
  assert(PianoPogoPollRuntime(&T,&Input,TRUE)==EFI_NOT_READY&&M.Calls==1);
  for(I=1;I<64;I++) { M.Clock+=1000; assert(PianoPogoPollRuntime(&T,&Input,TRUE)==EFI_SUCCESS); }
  assert(T.ReadAttempts==64); M.Clock+=1000;
  assert(PianoPogoPollRuntime(&T,&Input,TRUE)==EFI_OUT_OF_RESOURCES&&M.Calls==64);
  assert(PianoPogoStopTransport(&T)==EFI_SUCCESS&&!T.Read&&!T.Now&&!T.Context);
  assert(PianoPogoPollRuntime(&T,&Input,TRUE)==EFI_UNSUPPORTED&&M.Calls==64);
}
STATIC VOID Errors(VOID) {
  PIANO_POGO_TRANSPORT T; PIANO_POGO_INPUT Input; MOCK M; UINT32 Mode;
  STATIC CONST EFI_STATUS Expected[]={EFI_SUCCESS,EFI_BAD_BUFFER_SIZE,EFI_TIMEOUT,EFI_DEVICE_ERROR,
    EFI_SUCCESS,EFI_UNSUPPORTED,EFI_COMPROMISED_DATA};
  if(!PIANO_POGO_RUNTIME_READ_EXPERIMENT) { return; }
  for(Mode=1;Mode<=6;Mode++) {
    if(Mode==4) { continue; }
    Setup(&T,&Input,&M); M.Mode=Mode;
    assert(PianoPogoPollRuntime(&T,&Input,TRUE)==Expected[Mode]);
    assert(!Input.KeyCount&&!Input.AcceptedFrames&&T.Failed&&!T.InRead&&!T.AcceptedReads&&M.Calls==1);
    assert(PianoPogoPollRuntime(&T,&Input,TRUE)==EFI_ABORTED&&M.Calls==1);
  }
  Setup(&T,&Input,&M); M.Clock--;
  assert(PianoPogoPollRuntime(&T,&Input,TRUE)==EFI_COMPROMISED_DATA&&!M.Calls);
  Setup(&T,&Input,&M); M.Clock=MAX_UINT64-2;
  assert(PianoPogoPollRuntime(&T,&Input,TRUE)==EFI_COMPROMISED_DATA&&!M.Calls);
  Setup(&T,&Input,&M); T.Read=NULL;
  assert(PianoPogoPollRuntime(&T,&Input,TRUE)==EFI_COMPROMISED_DATA&&!M.Calls);
}
int main(void) {
  Interface(); Validate(); GatesAndReads(); Errors();
  puts("Piano Pogo transport actual UEFI source: pins/gates/default-off/read68/budget/deadline/no-retry PASS");
  return 0;
}
