// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#undef NULL
#define PIANO_USB_POWER_PROBE 1
#include "../../uefi/core/PianoKeys.c"
EFI_BOOT_SERVICES *gBS;
static EFI_BOOT_SERVICES bs;static UINT32 maps[8],owners[8],state=1,command;
static UINTN writes,stalls;
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level){return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){ }
UINT32 EFIAPI MmioRead32(UINTN Address) {
  if(Address>=ARB_CORE+0x2000 && Address<ARB_CORE+0x2020)return maps[(Address-ARB_CORE-0x2000)/4];
  if(Address>=ARB_CFG && Address<ARB_CFG+0x20)return owners[(Address-ARB_CFG)/4];
  assert(Address==ARB_OBS+8 || Address==ARB_OBS+0x18 || Address==ARB_OBS+0x1c);
  if(Address==ARB_OBS+8)return state;
  return Address==ARB_OBS+0x18?0x51020100:0x42;
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value) {
  assert(Address==ARB_OBS);assert((Value>>27)==1); // EXT_READL only.
  command=Value;++writes;return Value;
}
static EFI_STATUS EFIAPI stall(UINTN Us){assert(Us==1);++stalls;return EFI_SUCCESS;}
int main(void) {
  gBS=&bs;bs.Stall=stall;maps[0]=0x001<<8;owners[0]=0;
  UINT8 data[5];assert(UsbPowerRead(0,0x101,data,5,1)==EFI_SUCCESS);
  assert(command==((1U<<27)|(1<<4)|4));
  assert(data[0]==0 && data[1]==1 && data[2]==2 && data[3]==0x51 && data[4]==0x42);
  owners[0]=4;assert(UsbPowerRead(0,0x101,data,5,1)==EFI_SUCCESS);
  assert(command==((1U<<27)|(1<<4)|4)); // Still an EE0 observer read.
  owners[0]=0;
  maps[0]=0x7FD<<8;
  assert(UsbPowerRead(7,0xFD51,data,1,1)==EFI_SUCCESS);
  assert(command==((1U<<27)|(0x51<<4)));
  assert(UsbPowerRead(7,0xFD08,data,1,1)==EFI_SUCCESS);
  assert(UsbPowerRead(7,0xFD46,data,1,1)==EFI_SUCCESS);
  UINTN before=writes;
  assert(UsbPowerRead(7,0xFD50,data,1,1)==EFI_ACCESS_DENIED);
  assert(UsbPowerRead(0,0x101,data,4,1)==EFI_ACCESS_DENIED);
  assert(UsbPowerRead(8,0x101,data,5,1)==EFI_ACCESS_DENIED);
  assert(UsbPowerRead(7,0xFD51,NULL,1,1)==EFI_ACCESS_DENIED);
  owners[0]=3;assert(UsbPowerRead(7,0xFD51,data,1,1)==EFI_ACCESS_DENIED);
  assert(writes==before);owners[0]=0;
  state=5;assert(UsbPowerRead(7,0xFD51,data,1,1)==EFI_DEVICE_ERROR);
  state=0;assert(UsbPowerRead(7,0xFD51,data,1,1)==EFI_TIMEOUT && stalls==1000);
  puts("USB power probe: strict register whitelist, EE0 ownership, observer read opcodes, five-byte packing and timeout passed.");
  return 0;
}
