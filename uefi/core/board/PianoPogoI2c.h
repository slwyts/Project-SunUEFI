// SPDX-License-Identifier: BSD-2-Clause-Patent
// PI I2C request shape only. It does not discover or call a hardware protocol.
#ifndef PIANO_POGO_I2C_H
#define PIANO_POGO_I2C_H
#include <Uefi.h>
#include <Pi/PiI2c.h>
#include "PianoPogoReport.h"

#define PIANO_POGO_I2C_ADDRESS 0x4c
typedef struct {
  UINTN OperationCount;
  EFI_I2C_OPERATION Operation[2];
} PIANO_POGO_I2C_REQUEST;

EFI_STATUS PianoPogoMakeRuntimeRead(IN OUT PIANO_POGO_I2C_REQUEST *Request,
                                  OUT UINT8 *RegisterByte, OUT UINT8 *Frame, IN UINTN Bytes);
#endif
