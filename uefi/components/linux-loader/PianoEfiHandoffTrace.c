// SPDX-License-Identifier: BSD-2-Clause-Patent
// Optional EFI-stub trace. SerialPortWrite must be RAM-only and independent
// of boot services. No allocation, protocol lookup or free is performed here.
#ifdef PIANO_EFI_TRACE_HOST_TEST
#include "PianoEfiHandoffTraceTestShim.h"
#else
#include "PianoEfiHandoffTrace.h"
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#endif

#define TRACE_MAP_CAPACITY (64 * 1024)
#define TRACE_MAX_DESCRIPTOR_SIZE 256

typedef struct {
  EFI_BOOT_SERVICES *Services;
  EFI_GET_MEMORY_MAP GetMemoryMap;
  EFI_EXIT_BOOT_SERVICES ExitBootServices;
  BOOLEAN Installed;
  BOOLEAN Exited;
  BOOLEAN HaveMap;
  UINTN HeaderSize;
  UINTN MapSize;
  UINTN DescriptorSize;
  UINTN MapKey;
  UINT32 DescriptorVersion;
  EFI_STATUS CaptureStatus;
  UINTN Attempts;
  UINT8 Map[TRACE_MAP_CAPACITY];
} PIANO_EFI_TRACE_STATE;
STATIC PIANO_EFI_TRACE_STATE mTrace;

STATIC UINT32 TableCrc (CONST EFI_BOOT_SERVICES *Services, UINTN Size)
{
  CONST UINT8 *Bytes = (CONST UINT8 *)Services;
  UINT32 Crc = 0xffffffffU;
  UINTN CrcOffset = OFFSET_OF (EFI_BOOT_SERVICES, Hdr) + OFFSET_OF (EFI_TABLE_HEADER, CRC32);
  for (UINTN Index = 0; Index < Size; ++Index) {
    UINT8 Byte = (Index >= CrcOffset && Index < CrcOffset + sizeof (UINT32)) ? 0 : Bytes[Index];
    Crc ^= Byte;
    for (UINTN Bit = 0; Bit < 8; ++Bit) Crc = (Crc >> 1) ^ ((Crc & 1) ? 0xedb88320U : 0);
  }
  return ~Crc;
}

STATIC VOID LogReturn (EFI_STATUS Status, UINTN Key)
{
  CHAR8 Line[224];
  UINTN Bytes = AsciiSPrint (Line, sizeof (Line),
    "PIANO_EFI_TRACE EBS_RETURN attempt=%lu status=0x%lx success=%u key=0x%lx\n",
    (UINT64)mTrace.Attempts, (UINT64)Status, Status == EFI_SUCCESS, (UINT64)Key);
  SerialPortWrite ((UINT8 *)Line, Bytes);
}

STATIC VOID DumpCpu (VOID)
{
  UINT64 El = 0, Daif = 0, Sctlr = 0, Tcr = 0, Ttbr0 = 0, Ttbr1 = 0, Vbar = 0, Cntv = 0;
  CHAR8 Line[256];
#ifdef __aarch64__
  __asm__ volatile ("mrs %0, CurrentEL\nmrs %1, daif" : "=r"(El), "=r"(Daif));
  if (El == 4) {
    __asm__ volatile ("mrs %0, sctlr_el1\nmrs %1, tcr_el1\nmrs %2, ttbr0_el1\nmrs %3, ttbr1_el1\nmrs %4, vbar_el1\nmrs %5, cntv_ctl_el0"
      : "=r"(Sctlr), "=r"(Tcr), "=r"(Ttbr0), "=r"(Ttbr1), "=r"(Vbar), "=r"(Cntv));
  }
#endif
  UINTN Bytes = AsciiSPrint (Line, sizeof (Line),
    "PIANO_EFI_TRACE CPU el=0x%lx daif=0x%lx sctlr=0x%lx tcr=0x%lx ttbr0=0x%lx ttbr1=0x%lx vbar=0x%lx cntv=0x%lx\n",
    El, Daif, Sctlr, Tcr, Ttbr0, Ttbr1, Vbar, Cntv);
  SerialPortWrite ((UINT8 *)Line, Bytes);
}

STATIC VOID DumpMap (UINTN Key)
{
  CHAR8 Line[256];
  UINTN Bytes = AsciiSPrint (Line, sizeof (Line),
    "PIANO_EFI_TRACE EBS_ENTER attempt=%lu key=0x%lx captured_key=0x%lx known=%u capture_status=0x%lx bytes=%lu stride=%lu version=%u\n",
    (UINT64)mTrace.Attempts, (UINT64)Key, (UINT64)mTrace.MapKey, mTrace.HaveMap,
    (UINT64)mTrace.CaptureStatus, (UINT64)mTrace.MapSize,
    (UINT64)mTrace.DescriptorSize, mTrace.DescriptorVersion);
  SerialPortWrite ((UINT8 *)Line, Bytes);
  if (!mTrace.HaveMap) return;
  for (UINTN Offset = 0; Offset < mTrace.MapSize; Offset += mTrace.DescriptorSize) {
    EFI_MEMORY_DESCRIPTOR Descriptor;
    CopyMem (&Descriptor, mTrace.Map + Offset, sizeof (Descriptor));
    Bytes = AsciiSPrint (Line, sizeof (Line),
      "PIANO_EFI_TRACE MAP index=%lu type=%u phys=0x%lx virt=0x%lx pages=0x%lx attr=0x%lx\n",
      (UINT64)(Offset / mTrace.DescriptorSize), Descriptor.Type,
      Descriptor.PhysicalStart, Descriptor.VirtualStart, Descriptor.NumberOfPages, Descriptor.Attribute);
    SerialPortWrite ((UINT8 *)Line, Bytes);
  }
}

STATIC EFI_STATUS EFIAPI TraceGetMemoryMap (
  UINTN *Size, EFI_MEMORY_DESCRIPTOR *Map, UINTN *Key, UINTN *Stride, UINT32 *Version)
{
  if (mTrace.Exited) return EFI_ACCESS_DENIED;
  UINTN Capacity = Size == NULL ? 0 : *Size;
  EFI_STATUS Status = mTrace.GetMemoryMap (Size, Map, Key, Stride, Version);
  if (Status != EFI_SUCCESS) return Status;
  // Preserve service semantics even if its successful outputs cannot safely
  // be captured. Never silently call an incomplete snapshot a complete map.
  mTrace.HaveMap = FALSE;
  mTrace.CaptureStatus = EFI_BAD_BUFFER_SIZE;
  mTrace.MapSize = 0;
  if (Size == NULL || Map == NULL || Key == NULL || Stride == NULL || Version == NULL) return Status;
  mTrace.MapKey = *Key;
  mTrace.DescriptorSize = *Stride;
  mTrace.DescriptorVersion = *Version;
  if (*Version != 1) { mTrace.CaptureStatus = EFI_UNSUPPORTED; return Status; }
  if (*Size == 0 || *Size > Capacity || *Size > sizeof (mTrace.Map) ||
      *Stride < sizeof (EFI_MEMORY_DESCRIPTOR) || *Stride > TRACE_MAX_DESCRIPTOR_SIZE ||
      (*Stride % 8) != 0 || (*Size % *Stride) != 0) return Status;
  CopyMem (mTrace.Map, Map, *Size);
  mTrace.MapSize = *Size;
  mTrace.HaveMap = TRUE;
  mTrace.CaptureStatus = EFI_SUCCESS;
  return Status;
}

STATIC EFI_STATUS EFIAPI TraceExitBootServices (EFI_HANDLE Handle, UINTN Key)
{
  if (mTrace.Exited) return EFI_ACCESS_DENIED;
  ++mTrace.Attempts;
  DumpCpu ();
  DumpMap (Key);
  // Cache before entering firmware: success clears the BS table. Everything
  // below this call uses only module-owned static data and RAM serial output.
  EFI_EXIT_BOOT_SERVICES Original = mTrace.ExitBootServices;
  EFI_STATUS Status = Original (Handle, Key);
  if (Status == EFI_SUCCESS) mTrace.Exited = TRUE;
  LogReturn (Status, Key);
  return Status;
}

EFI_STATUS PianoEfiHandoffTraceInstall (EFI_BOOT_SERVICES *Services)
{
  if (mTrace.Exited) return EFI_ACCESS_DENIED;
  if (mTrace.Installed) return EFI_ALREADY_STARTED;
  if (Services == NULL || Services->Hdr.Signature != EFI_BOOT_SERVICES_SIGNATURE ||
      Services->Hdr.HeaderSize > sizeof (*Services) ||
      Services->Hdr.HeaderSize < OFFSET_OF (EFI_BOOT_SERVICES, ExitBootServices) + sizeof (EFI_EXIT_BOOT_SERVICES) ||
      Services->GetMemoryMap == NULL || Services->ExitBootServices == NULL) return EFI_INVALID_PARAMETER;
  if (TableCrc (Services, Services->Hdr.HeaderSize) != Services->Hdr.CRC32) return EFI_CRC_ERROR;
  ZeroMem (&mTrace, sizeof (mTrace));
  mTrace.Services = Services;
  mTrace.GetMemoryMap = Services->GetMemoryMap;
  mTrace.ExitBootServices = Services->ExitBootServices;
  mTrace.HeaderSize = Services->Hdr.HeaderSize;
  mTrace.CaptureStatus = EFI_NOT_READY;
  Services->GetMemoryMap = TraceGetMemoryMap;
  Services->ExitBootServices = TraceExitBootServices;
  Services->Hdr.CRC32 = TableCrc (Services, mTrace.HeaderSize);
  mTrace.Installed = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS PianoEfiHandoffTraceRestore (VOID)
{
  if (mTrace.Exited) return EFI_ACCESS_DENIED;
  if (!mTrace.Installed) return EFI_NOT_STARTED;
  EFI_BOOT_SERVICES *Services = mTrace.Services;
  if (Services->GetMemoryMap != TraceGetMemoryMap || Services->ExitBootServices != TraceExitBootServices ||
      Services->Hdr.HeaderSize != mTrace.HeaderSize) return EFI_ACCESS_DENIED;
  Services->GetMemoryMap = mTrace.GetMemoryMap;
  Services->ExitBootServices = mTrace.ExitBootServices;
  Services->Hdr.CRC32 = TableCrc (Services, mTrace.HeaderSize);
  mTrace.Installed = FALSE;
  return EFI_SUCCESS;
}

BOOLEAN PianoEfiHandoffTraceExited (VOID) { return mTrace.Exited; }
