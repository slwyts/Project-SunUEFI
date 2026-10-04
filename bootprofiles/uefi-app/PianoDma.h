// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>
typedef enum {PianoDmaToDevice,PianoDmaFromDevice,PianoDmaBidirectional} PIANO_DMA_DIRECTION;
typedef struct PIANO_DMA_DEVICE PIANO_DMA_DEVICE;
typedef struct PIANO_DMA_BUFFER PIANO_DMA_BUFFER;
typedef EFI_STATUS (*PIANO_DMA_MAP)(PIANO_DMA_DEVICE *,EFI_PHYSICAL_ADDRESS,UINTN,
                                  PIANO_DMA_DIRECTION,UINTN,EFI_PHYSICAL_ADDRESS *,VOID **);
typedef EFI_STATUS (*PIANO_DMA_UNMAP)(PIANO_DMA_DEVICE *,VOID *);
typedef VOID (*PIANO_DMA_FAULT)(PIANO_DMA_DEVICE *);
struct PIANO_DMA_DEVICE {
  CONST CHAR8 *Name;
  UINT16 StreamId;
  UINT8 AddressBits;
  UINTN CacheLine;
  VOID *Context;
  PIANO_DMA_MAP Map;
  PIANO_DMA_UNMAP Unmap;
  PIANO_DMA_FAULT Fault;
};
struct PIANO_DMA_BUFFER {
  UINT32 Signature;
  PIANO_DMA_DEVICE *Device;
  EFI_PHYSICAL_ADDRESS Allocation,Physical,DeviceAddress;
  VOID *Cpu,*Mapping;
  // Diagnostic label must remain valid until completion (use static labels).
  CONST CHAR8 *Command;
  UINTN AllocationPages,Bytes,ReservedBytes,Alignment;
  UINT64 MemoryAttributes;
  PIANO_DMA_DIRECTION Direction;
  BOOLEAN Mapped,Active,Quarantined;
};
EFI_STATUS PianoDmaAllocate(PIANO_DMA_DEVICE *Device,UINTN Bytes,UINTN Alignment,
                           UINT8 PhysicalBits,PIANO_DMA_DIRECTION Direction,PIANO_DMA_BUFFER *Buffer);
EFI_STATUS PianoDmaMap(PIANO_DMA_BUFFER *Buffer);
EFI_STATUS PianoDmaBegin(PIANO_DMA_BUFFER *Buffer,CONST CHAR8 *Command);
EFI_STATUS PianoDmaComplete(PIANO_DMA_BUFFER *Buffer,EFI_STATUS Status,BOOLEAN HardwareQuiesced);
// Polling a device-owned event ring/TRB: CPU must not write while active.
EFI_STATUS PianoDmaSyncForCpu(PIANO_DMA_BUFFER *Buffer);
EFI_STATUS PianoDmaUnmap(PIANO_DMA_BUFFER *Buffer);
EFI_STATUS PianoDmaFree(PIANO_DMA_BUFFER *Buffer);
EFI_STATUS PianoDmaPhysicalAddress(CONST VOID *Cpu,EFI_PHYSICAL_ADDRESS *Physical);
// Translation is bounded to the caller-owned mapped buffer, never a guessed
// global identity relationship. Bytes may not extend into page padding.
EFI_STATUS PianoDmaPhysicalToDevice(CONST PIANO_DMA_BUFFER *Buffer,EFI_PHYSICAL_ADDRESS Physical,
                                   UINTN Bytes,EFI_PHYSICAL_ADDRESS *DeviceAddress);
EFI_STATUS PianoDmaDeviceToPhysical(CONST PIANO_DMA_BUFFER *Buffer,EFI_PHYSICAL_ADDRESS DeviceAddress,
                                   UINTN Bytes,EFI_PHYSICAL_ADDRESS *Physical);
