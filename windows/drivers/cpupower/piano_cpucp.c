/* SPDX-License-Identifier: BSD-2-Clause */
#include "piano_cpucp.h"

enum { TX_SHMEM = 0x17b4e000, TX_BYTES = 0x200, TX_DOORBELL = 0x17830104,
       RESPONSE_US = 100000 };

static int removed(PIANO_CPUCP_TX *tx)
{
    return InterlockedCompareExchange(&tx->Removed, 0, 0) != 0;
}

static uint32_t read_slot(void *ctx, unsigned offset)
{
    PIANO_CPUCP_TX *tx = ctx;
    return removed(tx) ? UINT32_MAX : READ_REGISTER_ULONG(tx->Shmem + offset / 4);
}

static void write_slot(void *ctx, unsigned offset, uint32_t value)
{
    PIANO_CPUCP_TX *tx = ctx;
    if (!removed(tx)) WRITE_REGISTER_ULONG(tx->Shmem + offset / 4, value);
}

static void ring(void *ctx, uint32_t header)
{
    PIANO_CPUCP_TX *tx = ctx;
    /* Publish the entire message before ringing channel 0, as on Pakala. */
    KeMemoryBarrier();
    if (!removed(tx)) WRITE_REGISTER_ULONG(tx->Doorbell, header);
}

static uint64_t now_us(void *ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    return KeQueryInterruptTime() / 10;
}

static void wait_reply(void *ctx)
{
    LARGE_INTEGER delay;
    UNREFERENCED_PARAMETER(ctx);
    delay.QuadPart = -1000;  /* A passive wait, not a spin at elevated IRQL. */
    (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

static int writable_resource(const CM_PARTIAL_RESOURCE_DESCRIPTOR *r, ULONG base, ULONG bytes)
{
    return r && r->Type == CmResourceTypeMemory && r->ShareDisposition == CmResourceShareDeviceExclusive &&
           !(r->Flags & (CM_RESOURCE_MEMORY_READ_ONLY | CM_RESOURCE_MEMORY_WRITE_ONLY)) &&
           r->u.Memory.Start.QuadPart == base && r->u.Memory.Length == bytes;
}

VOID PianoCpucpInitialize(PIANO_CPUCP_TX *tx)
{
    RtlZeroMemory(tx, sizeof(*tx));
    KeInitializeMutex(&tx->Lock, 0);
}

NTSTATUS PianoCpucpStart(PIANO_CPUCP_TX *tx, const CM_PARTIAL_RESOURCE_DESCRIPTOR *shmem,
                         const CM_PARTIAL_RESOURCE_DESCRIPTOR *doorbell)
{
    if (!tx || KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_PARAMETER;
    if (!writable_resource(shmem, TX_SHMEM, TX_BYTES) ||
        !writable_resource(doorbell, TX_DOORBELL, sizeof(ULONG))) return STATUS_DEVICE_CONFIGURATION_ERROR;
    struct scmi_channel_io io = { tx, read_slot, write_slot, ring, now_us, wait_reply };
    (void)KeWaitForSingleObject(&tx->Lock, Executive, KernelMode, FALSE, NULL);
    NTSTATUS status = STATUS_DEVICE_BUSY;
    if (removed(tx)) { status = STATUS_DEVICE_REMOVED; goto out; }
    if (tx->Online) goto out;
    if (tx->Shmem) goto restore;
    tx->Shmem = MmMapIoSpaceEx(shmem->u.Memory.Start, TX_BYTES, PAGE_READWRITE | PAGE_NOCACHE);
    tx->Doorbell = MmMapIoSpaceEx(doorbell->u.Memory.Start, sizeof(ULONG), PAGE_READWRITE | PAGE_NOCACHE);
    if (!tx->Shmem || !tx->Doorbell) {
        if (tx->Shmem) MmUnmapIoSpace(tx->Shmem, TX_BYTES);
        if (tx->Doorbell) MmUnmapIoSpace(tx->Doorbell, sizeof(ULONG));
        tx->Shmem = tx->Doorbell = NULL;
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }
    if (!tx->Channel.io.read) (void)scmi_channel_init(&tx->Channel, &io, TX_BYTES, RESPONSE_US);
restore:
    /* A forced D3 can return here with a timed-out command still owned by
     * the CPUCP. Do not replace its header, even after a resource rebalance. */
    if (scmi_channel_drain(&tx->Channel)) goto out;
    tx->Online = TRUE;
    status = STATUS_SUCCESS;
out:
    KeReleaseMutex(&tx->Lock, FALSE);
    return status;
}

NTSTATUS PianoCpucpStop(PIANO_CPUCP_TX *tx, BOOLEAN surprise)
{
    if (!tx || KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_PARAMETER;
    if (surprise) InterlockedExchange(&tx->Removed, 1);
    (void)KeWaitForSingleObject(&tx->Lock, Executive, KernelMode, FALSE, NULL);
    NTSTATUS status = STATUS_SUCCESS;
    if (tx->Online) {
        tx->Online = FALSE;
        if (!removed(tx) && scmi_channel_drain(&tx->Channel)) {
            status = STATUS_DEVICE_BUSY;
        }
    }
    KeReleaseMutex(&tx->Lock, FALSE);
    return status;
}

VOID PianoCpucpRelease(PIANO_CPUCP_TX *tx)
{
    (void)KeWaitForSingleObject(&tx->Lock, Executive, KernelMode, FALSE, NULL);
    tx->Online = FALSE;
    /* No message contains a host DMA address. Unmap released PnP resources
     * without touching hardware; keep pending-token bookkeeping for D0. */
    if (tx->Shmem) {
        MmUnmapIoSpace(tx->Doorbell, sizeof(ULONG));
        MmUnmapIoSpace(tx->Shmem, TX_BYTES);
        tx->Shmem = tx->Doorbell = NULL;
    }
    KeReleaseMutex(&tx->Lock, FALSE);
}

int PianoCpucpMessage(void *context, unsigned protocol, unsigned command, const uint32_t *args,
                      size_t nargs, uint32_t *result, size_t capacity, size_t *count)
{
    PIANO_CPUCP_TX *tx = context;
    if (!tx || KeGetCurrentIrql() != PASSIVE_LEVEL) return SCMI_PERF_INVALID;
    (void)KeWaitForSingleObject(&tx->Lock, Executive, KernelMode, FALSE, NULL);
    int err = tx->Online && !removed(tx) ?
        scmi_channel_message(&tx->Channel, protocol, command, args, nargs, result, capacity, count) :
        SCMI_PERF_NOT_FOUND;
    KeReleaseMutex(&tx->Lock, FALSE);
    return err;
}
