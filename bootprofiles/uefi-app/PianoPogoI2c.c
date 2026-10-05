// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PianoPogoI2c.h"

EFI_STATUS PianoPogoMakeRuntimeRead(PIANO_POGO_I2C_REQUEST *Request,
                                   UINT8 *RegisterByte, UINT8 *Frame, UINTN Bytes) {
  if(Request==NULL||RegisterByte==NULL||Frame==NULL||Bytes!=PIANO_POGO_FRAME_BYTES) { return EFI_INVALID_PARAMETER; }
  *RegisterByte=0x4c;
  Request->OperationCount=2;
  Request->Operation[0].Flags=0;
  Request->Operation[0].LengthInBytes=1;
  Request->Operation[0].Buffer=RegisterByte;
  Request->Operation[1].Flags=I2C_FLAG_READ;
  Request->Operation[1].LengthInBytes=PIANO_POGO_FRAME_BYTES;
  Request->Operation[1].Buffer=Frame;
  return EFI_SUCCESS;
}
