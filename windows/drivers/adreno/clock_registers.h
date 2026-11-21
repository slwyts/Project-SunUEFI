/* SPDX-License-Identifier: MIT */
#pragma once
#include <ntddk.h>

static inline NTSTATUS update(volatile ULONG *address, ULONG mask, ULONG value)
{
    ULONG v = READ_REGISTER_ULONG(address);
    if (v == MAXULONG) return STATUS_DEVICE_NOT_CONNECTED;
    if ((v & mask) == value) return STATUS_SUCCESS;
    WRITE_REGISTER_ULONG(address, (v & ~mask) | value);
    v = READ_REGISTER_ULONG(address);
    if (v == MAXULONG) return STATUS_DEVICE_NOT_CONNECTED;
    return (v & mask) == value ? STATUS_SUCCESS : STATUS_DEVICE_HARDWARE_ERROR;
}

static inline NTSTATUS wait_bits(volatile ULONG *address, ULONG mask, ULONG value, ULONG us)
{
    for (ULONG i = 0; i <= us; ++i) {
        ULONG v = READ_REGISTER_ULONG(address);
        if (v == MAXULONG) return STATUS_DEVICE_NOT_CONNECTED;
        if ((v & mask) == value) return STATUS_SUCCESS;
        if (i != us) KeStallExecutionProcessor(1);
    }
    return STATUS_IO_TIMEOUT;
}

static inline BOOLEAN window(PCM_PARTIAL_RESOURCE_DESCRIPTOR r, ULONGLONG *base, ULONGLONG minimum, ULONGLONG *end)
{
    if (!r || (r->Type != CmResourceTypeMemory && r->Type != CmResourceTypeMemoryLarge) ||
        (r->Flags & (CM_RESOURCE_MEMORY_READ_ONLY | CM_RESOURCE_MEMORY_WRITE_ONLY)) ||
        r->ShareDisposition == CmResourceShareShared)
        return FALSE;
    ULONGLONG size = RtlCmDecodeMemIoResource(r, base);
    if (size < minimum || size > MAXLONGLONG || (*base & (PAGE_SIZE - 1)) || *base > (ULONGLONG)MAXLONGLONG - size)
        return FALSE;
    *end = *base + size;
    return TRUE;
}
