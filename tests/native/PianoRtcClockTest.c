// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static UINT32 observerBase, utcOffset;
static BOOLEAN Runtime;
static EFI_STATUS configure(UINT32 *slot, UINT32 value) { *slot = value; return EFI_SUCCESS; }
static UINT32 pcd(UINT32 value) { assert(!Runtime); return value; }
#define _PCD_GET_MODE_32_PcdPianoRtcOffsetSeconds pcd(utcOffset)
#define _PCD_GET_MODE_32_PcdPianoRtcObserverBase pcd(observerBase)
#define _PCD_SET_MODE_32_S_PcdPianoRtcOffsetSeconds(v) configure(&utcOffset, (v))
#define _PCD_SET_MODE_32_S_PcdPianoRtcObserverBase(v) configure(&observerBase, (v))
#include "../../uefi/components/rtc/PianoRtcClock.c"
#include "../../uefi/components/product-support/Library/PianoRealTimeClockLib/PianoRealTimeClockLib.c"
#undef FDT_TAGSIZE
#include <libfdt.h>

static UINT32 Raw[3], RawIndex, Command, Status = BIT0, Owner = 0;
static UINTN Reads, Commands, Delays;
static BOOLEAN counterEnabled = TRUE;
static EFI_RT_PROPERTIES_TABLE Existing;
static EFI_RT_PROPERTIES_TABLE *Installed;
EFI_GUID gEfiRtPropertiesTableGuid;
EFI_BOOT_SERVICES *gBS;

INT32 EFIAPI FdtParentOffset(CONST VOID *f, INT32 n) { return fdt_parent_offset(f, n); }
INT32 EFIAPI FdtStringListSearch(CONST VOID *f,INT32 n,CONST CHAR8 *name,CONST CHAR8 *s)
{ return fdt_stringlist_search(f, n, name, s); }
BOOLEAN EFIAPI EfiAtRuntime(VOID) { return Runtime; }
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *p, UINT32 a, UINT32 b)
{ UINT32 previous = *p; if (previous == a) *p = b; return previous; }
UINTN EFIAPI MicroSecondDelay(UINTN usec) { assert(usec == 1); ++Delays; return usec; }
UINT32 EFIAPI MmioRead32(UINTN addr) {
  ++Reads;
  assert(!Runtime);
  if (addr == 0x0c400000) return 0x70000000;
  if (addr == 0x0c400004) return 5;
  if (addr >= 0x0c402000 && addr < 0x0c402014) {
    unsigned i = (unsigned)(addr-0x0c402000)/4;
    return (i == 1 || i == 4 ? 0x061u : 0x062u) << 8;
  }
  if (addr == 0x0c42d004) return 2;
  if (addr == 0x0c42d010) return Owner;
  assert(observerBase != 0);
  if (addr == observerBase + 8) return Status;
  assert(addr == observerBase + 0x18);
  if (Command == ((1u << 27) | (0x46u << 4))) return counterEnabled ? BIT7 : 0;
  assert(RawIndex < 3);
  return Raw[RawIndex++];
}
UINT32 EFIAPI MmioWrite32(UINTN addr, UINT32 value) {
  assert(!Runtime && observerBase != 0 && addr == observerBase);
  assert(value == ((1u << 27) | (0x46u << 4)) ||
         value == ((1u << 27) | (0x48u << 4)) ||
         value == ((1u << 27) | (0x48u << 4) | 3u));
  ++Commands; Command = value; return value;
}
EFI_STATUS EFIAPI EfiGetSystemConfigurationTable(EFI_GUID *id, VOID **out) {
  assert(id == &gEfiRtPropertiesTableGuid);
  if (!Existing.Version) return EFI_NOT_FOUND;
  *out = &Existing; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI install(EFI_GUID *id, VOID *table) {
  assert(id == &gEfiRtPropertiesTableGuid); Installed = table; return EFI_SUCCESS;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  FILE *file = fopen(argv[1], "rb");
  assert(file && !fseek(file, 0, SEEK_END));
  long bytes = ftell(file);
  assert(bytes > 0 && !fseek(file, 0, SEEK_SET));
  void *dt = malloc((size_t)bytes);
  assert(dt && fread(dt, 1, (size_t)bytes, file) == (size_t)bytes && !fclose(file));
  EFI_BOOT_SERVICES services = { .InstallConfigurationTable = install };
  gBS = &services;
  UINT32 readers = EFI_RT_SUPPORTED_GET_VARIABLE | EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME |
                   EFI_RT_SUPPORTED_QUERY_VARIABLE_INFO;
  UINT32 capsules = EFI_RT_SUPPORTED_UPDATE_CAPSULE | EFI_RT_SUPPORTED_QUERY_CAPSULE_CAPABILITIES;
  assert(LibRtcInitialize(NULL,NULL) == EFI_SUCCESS);
  assert(Installed->Length == sizeof(*Installed) && (Installed->RuntimeServicesSupported & readers) == readers);
  assert(!(Installed->RuntimeServicesSupported & (EFI_RT_SUPPORTED_SET_VARIABLE | capsules)));
  Existing = (EFI_RT_PROPERTIES_TABLE){EFI_RT_PROPERTIES_TABLE_VERSION,sizeof(Existing),0x3fff};
  assert(LibRtcInitialize(NULL,NULL) == EFI_SUCCESS);
  assert(Installed->RuntimeServicesSupported == (Existing.RuntimeServicesSupported &
         ~(EFI_RT_SUPPORTED_GET_TIME|EFI_RT_SUPPORTED_SET_TIME|EFI_RT_SUPPORTED_GET_WAKEUP_TIME|
           EFI_RT_SUPPORTED_SET_WAKEUP_TIME|EFI_RT_SUPPORTED_SET_VARIABLE|capsules)));
  Existing.RuntimeServicesSupported = EFI_RT_SUPPORTED_GET_VARIABLE | capsules;
  assert(LibRtcInitialize(NULL,NULL) == EFI_SUCCESS);
  assert(Installed->RuntimeServicesSupported == EFI_RT_SUPPORTED_GET_VARIABLE);
  --Existing.Length;
  assert(LibRtcInitialize(NULL,NULL) == EFI_COMPROMISED_DATA);

  EFI_TIME time = {0}; EFI_TIME_CAPABILITIES caps = {0};
  assert(LibGetTime(&time, NULL) == EFI_UNSUPPORTED && !Reads);
  assert(PianoRtcConfigureClock(dt, 1709210096u - 256) == EFI_SUCCESS);
  assert(observerBase == 0x0c440080 && !Commands);
  Raw[0] = 0x000001ff; Raw[1] = 0; Raw[2] = 256; // mixed word during low-byte rollover
  assert(LibGetTime(&time,&caps) == EFI_SUCCESS && RawIndex == 3 && Commands == 4);
  assert(time.Year == 2024 && time.Month == 2 && time.Day == 29 && time.Hour == 12 &&
         time.Minute == 34 && time.Second == 56 && time.TimeZone == 0 && time.Daylight == 0);
  assert(caps.Resolution == 1 && caps.Accuracy == MAX_UINT32 && !caps.SetsToZero);
  Owner = 3;
  assert(PianoRtcConfigureClock(dt, 4107546123u - 256) == EFI_SUCCESS);
  assert(observerBase == 0x0c440020); // other EE owns writes, access still uses EE0 observer
  RawIndex = 0; Raw[0] = 256; Raw[1] = 0;
  assert(LibGetTime(&time,NULL) == EFI_SUCCESS && RawIndex == 2);
  assert(time.Year == 2100 && time.Month == 3 && time.Day == 1);

  counterEnabled = FALSE;
  assert(LibGetTime(&time,NULL) == EFI_DEVICE_ERROR && RawIndex == 2);
  counterEnabled = TRUE; Status = BIT0 | BIT2;
  assert(LibGetTime(&time,NULL) == EFI_DEVICE_ERROR && RawIndex == 2);
  Status = 0;
  assert(LibGetTime(&time,NULL) == EFI_DEVICE_ERROR && Delays == 1000);
  UINTN calls = Commands;
  assert(LibGetTime(&time,NULL) == EFI_DEVICE_ERROR && Commands == calls);
  assert(LibSetTime(&time) == EFI_UNSUPPORTED && Commands == calls);
  Runtime = TRUE;
  assert(LibGetTime(&time,NULL) == EFI_UNSUPPORTED && Commands == calls);
  assert(LibGetTime(NULL,NULL) == EFI_INVALID_PARAMETER);
  Runtime = FALSE;
  int bus = fdt_path_offset(dt, "/soc/qcom,spmi@c42d000");
  assert(bus >= 0 && fdt_setprop_inplace_u32(dt,bus,"qcom,ee",1) == 0);
  UINTN before = Reads;
  assert(PianoRtcConfigureClock(dt, 1) == EFI_UNSUPPORTED && before == Reads);
  free(dt);
}
