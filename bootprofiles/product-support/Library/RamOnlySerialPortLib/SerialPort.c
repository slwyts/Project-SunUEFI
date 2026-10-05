// SPDX-License-Identifier: BSD-2-Clause-Patent
// RAM debug backend. A physical UART/USB backend must be added separately.
#include <Uefi.h>
#include <Library/SerialPortLib.h>
VOID RamLogWrite (CONST UINT8 *Buffer, UINTN Length);
RETURN_STATUS EFIAPI SerialPortInitialize (VOID) { return RETURN_SUCCESS; }
UINTN EFIAPI SerialPortWrite (UINT8 *Buffer, UINTN NumberOfBytes) {
  RamLogWrite (Buffer, NumberOfBytes); return NumberOfBytes;
}
UINTN EFIAPI SerialPortRead (UINT8 *Buffer, UINTN NumberOfBytes) { return 0; }
BOOLEAN EFIAPI SerialPortPoll (VOID) { return FALSE; }
RETURN_STATUS EFIAPI SerialPortSetControl (UINT32 Control) { return RETURN_UNSUPPORTED; }
RETURN_STATUS EFIAPI SerialPortGetControl (UINT32 *Control) {
  if (Control == NULL) { return RETURN_INVALID_PARAMETER; }
  *Control = 0x100; return RETURN_SUCCESS;
}
RETURN_STATUS EFIAPI SerialPortSetAttributes (UINT64 *BaudRate, UINT32 *ReceiveFifoDepth,
  UINT32 *Timeout, EFI_PARITY_TYPE *Parity, UINT8 *DataBits, EFI_STOP_BITS_TYPE *StopBits) {
  return RETURN_UNSUPPORTED;
}
