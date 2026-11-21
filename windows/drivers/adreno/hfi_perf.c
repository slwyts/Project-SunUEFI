/* SPDX-License-Identifier: MIT */
#include "hfi.h"
#include "hfi_perf_table.h"

/* After a live GMU boot, KGSL reads ver.hfi at DTCM + 0xffc words. This
 * register is inside the same translated GMU range AdrenoHfiInitialize
 * checked. The caller holds D0 and has already started HFI.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoHfiSetPerfTable(ADRENO_HFI *hfi, const struct adreno_perf_config *config, ULONG timeout_ms)
{
    PAGED_CODE();
    if (!hfi || !hfi->registers || !config || !timeout_ms)
        return STATUS_INVALID_PARAMETER;
    enum { hfi_version = 0x1c400 - 0x1b000 + 0xffc };
    ULONG version = READ_REGISTER_ULONG(hfi->registers + hfi_version);
    if (!version || version == MAXULONG)
        return STATUS_DEVICE_NOT_READY;
    struct adreno_perf_table table;
    switch (adreno_hfi_perf_table(version, config, &table)) {
    case ADRENO_PERF_OK: break;
    case ADRENO_PERF_VERSION: return STATUS_NOT_SUPPORTED;
    default: return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return AdrenoHfiRequest(hfi, ADRENO_HFI_TABLE, table.words, table.count, timeout_ms);
}
