// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PianoDma.h"
#include <Protocol/PciIo.h>

#define PIANO_USB_HOST_MMIO_BASE 0x0A600000U
#define PIANO_USB_HOST_MMIO_BYTES 0x8000U
#define PIANO_USB_HOST_MAX_RECORDS 64U
#define PIANO_USB_HOST_DMA_BUDGET 0x01000000U
#define PIANO_USB_HOST_DMA_STREAMING BIT0
#define PIANO_USB_HOST_COMMON_COHERENT BIT1
#define PIANO_USB_HOST_COMMON_UNCACHED BIT2
#define PIANO_USB_HOST_PARTIAL_FREE BIT3

// No callback may report success before the real operation is complete.
// Ready verifies USB0 host ownership, clocks/role/SMMU, not just an installed
// protocol. SetBusMaster(FALSE) must halt the real controller before success.
// Read32/Write32 are ordered MMIO; Write32 supplies the architecture release
// barrier between CPU ring publication and a controller doorbell write.
// Map owns cache sync/bounce for streaming buffers and must preserve full Bytes.
// Its reservation may not exceed page_roundup(host_page_offset + Bytes).
// Each live Map returns a distinct native token and reserves exclusive IOVA
// pages; overlapping aliases are quarantined, not accepted as another mapping.
// A failure with no acquired resource leaves Iova/Token untouched; if any
// resource may remain it returns its native token/address for quarantine.
// A common mapping must share the same CPU storage; copying bounce is invalid.
// Retire proves DMA retirement, cache completion and TLB sync. On failure it
// retains the native token. CopyBack=FALSE on rollback/quarantine recovery so a
// stale caller buffer is never written by late recovery.
// Allocate returns page-aligned CPU storage, its actual EFI memory attributes,
// and an ownership token even on partial failure. Free accepts owned page spans;
// partial spans require PARTIAL_FREE and the native token survives until the
// complete original allocation has been released. Missing callbacks never
// fall back to identity addresses or a successful no-op.
typedef struct {
  VOID *Context;
  UINT32 Capabilities;
  EFI_STATUS (*Ready)(VOID *Context);
  EFI_STATUS (*Read32)(VOID *Context,UINT32 Offset,UINT32 *Value);
  EFI_STATUS (*Write32)(VOID *Context,UINT32 Offset,UINT32 Value);
  EFI_STATUS (*SetBusMaster)(VOID *Context,BOOLEAN Enable);
  EFI_STATUS (*Quiesced)(VOID *Context);
  EFI_STATUS (*Stall)(VOID *Context,UINTN Microseconds);
  EFI_STATUS (*Flush)(VOID *Context);
  EFI_STATUS (*Allocate)(VOID *Context,UINTN Pages,UINT64 RequestedAttributes,
                         VOID **Cpu,UINT64 *MemoryAttributes,VOID **Token);
  EFI_STATUS (*Free)(VOID *Context,VOID *Token,VOID *Cpu,UINTN Pages);
  EFI_STATUS (*Map)(VOID *Context,PIANO_DMA_DIRECTION Direction,BOOLEAN Common,
                    VOID *Cpu,UINTN *Bytes,EFI_PHYSICAL_ADDRESS *Iova,VOID **Token);
  EFI_STATUS (*Retire)(VOID *Context,VOID *Token,BOOLEAN CopyBack);
} PIANO_USB_HOST_PCI_BACKEND;
typedef struct {UINTN Bytes,Allocations,Mappings;} PIANO_USB_HOST_PCI_LIMITS;
typedef struct {
  BOOLEAN Used,Quarantined;
  VOID *Cpu,*Native;
  UINTN Pages,References;
  UINT64 MemoryAttributes;
} PIANO_USB_HOST_ALLOCATION;
typedef struct {
  BOOLEAN Used,Quarantined,Common;
  UINTN Key,Charge,Allocation;
  VOID *Cpu,*Native;
  UINTN Bytes;
  EFI_PHYSICAL_ADDRESS Iova;
} PIANO_USB_HOST_MAPPING;
typedef struct {
  UINT32 Signature;
  EFI_PCI_IO_PROTOCOL Pci;
  PIANO_USB_HOST_PCI_BACKEND Backend;
  PIANO_USB_HOST_PCI_LIMITS Limits;
  BOOLEAN Busy,BarProbe,ControllerUncertain;
  UINT64 Attributes;
  UINTN AllocatedBytes,MappedBytes;
  PIANO_USB_HOST_ALLOCATION Allocation[PIANO_USB_HOST_MAX_RECORDS];
  PIANO_USB_HOST_MAPPING Mapping[PIANO_USB_HOST_MAX_RECORDS];
} PIANO_USB_HOST_PCI;

// Context must initially be zeroed. No hardware/handle is installed or enabled.
// Caller serializes use at the appropriate TPL; callback reentry to stateful or
// hardware operations is rejected. Read-only PCI metadata can still be queried.
EFI_STATUS PianoUsbHostPciInit(PIANO_USB_HOST_PCI *Context,
                              CONST PIANO_USB_HOST_PCI_BACKEND *Backend,
                              CONST PIANO_USB_HOST_PCI_LIMITS *Limits OPTIONAL);
// Explicitly halted recovery only. Retains anything whose release is uncertain.
EFI_STATUS PianoUsbHostPciRecover(PIANO_USB_HOST_PCI *Context);
BOOLEAN PianoUsbHostPciHasQuarantine(CONST PIANO_USB_HOST_PCI *Context);
// Separate from normal DWORD writes: never defers CRCR's single-DWORD controls.
EFI_STATUS PianoUsbHostPciWrite64HiLo(PIANO_USB_HOST_PCI *Context,UINT32 Offset,UINT64 Value);
