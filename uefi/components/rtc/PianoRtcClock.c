// SPDX-License-Identifier: BSD-2-Clause-Patent
// Locate the PMK8550 RTC on the application processor's SPMI v7 observer.
#include "PianoRtcOffset.h"
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>

STATIC BOOLEAN
CellsMatch (CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name, CONST UINT32 *Cells, UINTN Count)
{
  INT32 Bytes;
  CONST UINT32 *Data = FdtGetProp (Fdt, Node, Name, &Bytes);

  if (Data == NULL || Bytes != (INT32)(Count * sizeof (*Data))) return FALSE;
  for (UINTN Index = 0; Index < Count; ++Index) {
    if (Fdt32ToCpu (Data[Index]) != Cells[Index]) return FALSE;
  }
  return TRUE;
}

// Use only the validated HLOS regions; no transaction goes through the
// read/write channels. The RTC peripheral's APID is allocated by the arbiter.
STATIC BOOLEAN
Region (CONST VOID *Fdt, INT32 Bus, CONST CHAR8 *Name, UINT32 Base, UINT32 MinBytes)
{
  INT32 Bytes, Index = FdtStringListSearch (Fdt, Bus, "reg-names", Name);
  CONST UINT32 *Reg = FdtGetProp (Fdt, Bus, "reg", &Bytes);

  return Index >= 0 && Reg != NULL && Bytes >= 8 && (Bytes % 8) == 0 &&
         Index < Bytes / 8 && Fdt32ToCpu (Reg[2 * Index]) == Base &&
         Fdt32ToCpu (Reg[2 * Index + 1]) >= MinBytes;
}

STATIC BOOLEAN
Enabled (CONST VOID *Fdt, INT32 Node)
{
  INT32 Bytes;
  return FdtGetProp (Fdt, Node, "status", &Bytes) == NULL ||
         FdtStringListSearch (Fdt, Node, "status", "okay") == 0 ||
         FdtStringListSearch (Fdt, Node, "status", "ok") == 0;
}

EFI_STATUS
PianoRtcConfigureClock (CONST VOID *Fdt, UINT32 OffsetSeconds)
{
  CONST UINT32 Core = 0x0c400000, Config = 0x0c42d000, Observer = 0x0c440000;
  CONST UINT32 Zero = 0, One = 1, Slave[] = { 0, 0 }, Registers[] = { 0x6100, 0x6200 };
  INT32 Rtc, Pmic, Bus, Soc;
  UINT32 Version, Count, Channel = 0, WriteOwner = 0;

  if (Fdt == NULL || FdtCheckHeader (Fdt) != 0 || OffsetSeconds == 0) return EFI_INVALID_PARAMETER;
  Rtc = FdtNodeOffsetByCompatible (Fdt, -1, "qcom,pmk8350-rtc");
  if (Rtc < 0 || FdtNodeOffsetByCompatible (Fdt, Rtc, "qcom,pmk8350-rtc") != -FDT_ERR_NOTFOUND) return EFI_UNSUPPORTED;
  Pmic = FdtParentOffset (Fdt, Rtc);
  Bus = FdtParentOffset (Fdt, Pmic);
  Soc = FdtParentOffset (Fdt, Bus);
  if (Pmic < 0 || Bus < 0 || Soc < 0 || !Enabled (Fdt, Rtc) || !Enabled (Fdt, Pmic) || !Enabled (Fdt, Bus) ||
      !CellsMatch (Fdt, Soc, "#address-cells", &One, 1) || !CellsMatch (Fdt, Soc, "#size-cells", &One, 1) ||
      FdtStringListSearch (Fdt, Bus, "compatible", "qcom,spmi-pmic-arb") < 0 ||
      FdtStringListSearch (Fdt, Pmic, "compatible", "qcom,spmi-pmic") < 0 ||
      !CellsMatch (Fdt, Bus, "qcom,ee", &Zero, 1) || !CellsMatch (Fdt, Bus, "qcom,bus-id", &Zero, 1) ||
      !CellsMatch (Fdt, Pmic, "reg", Slave, 2) || !CellsMatch (Fdt, Rtc, "reg", Registers, 2) ||
      FdtStringListSearch (Fdt, Rtc, "reg-names", "rtc") != 0 ||
      !Region (Fdt, Bus, "core", Core, 0x3000) || !Region (Fdt, Bus, "cnfg", Config, 0x1000) ||
      !Region (Fdt, Bus, "obsrvr", Observer, 0x8000)) return EFI_UNSUPPORTED;

  Version = MmioRead32 (Core);
  if ((Version >> 28) != 7) return EFI_UNSUPPORTED;
  Count = MmioRead32 (Core + 4) & 0x7ff;
  if (Count == 0 || Count > 1024) return EFI_UNSUPPORTED;
  for (UINT32 Apid = 0; Apid < Count; ++Apid) {
    UINT32 Map = MmioRead32 (Core + 0x2000 + 4 * Apid);
    if (((Map >> 8) & 0xfff) != 0x061) continue;  // SID 0, peripheral 0x61
    UINT32 Owner = MmioRead32 (Config + 4 * Apid) & 7;
    if (Channel == 0 || Owner == 0) {
      WriteOwner = Owner;
      Channel = Observer + 0x20 * Apid;
    }
  }
  if (Channel == 0) return EFI_NOT_FOUND;
  // Split-ownership peripherals may be readable from EE0 even when another
  // EE owns writes. The observer itself enforces read access.
  EFI_STATUS Status = PcdSet32S (PcdPianoRtcOffsetSeconds, OffsetSeconds);
  if (!EFI_ERROR (Status)) Status = PcdSet32S (PcdPianoRtcObserverBase, Channel);
  DEBUG ((DEBUG_INFO, "PMK8550 RTC observer=%x write_owner=%u ee=0 status=%r\n", Channel, WriteOwner, Status));
  return Status;
}
