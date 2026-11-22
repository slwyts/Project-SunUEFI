/* SPDX-License-Identifier: MIT */
#include "cx_power.h"
#include "../gcc/registers.h"

enum { CX_CONTROL = 0x9080, CX_STATUS = 0x9094, SW_COLLAPSE = 1, HW_CONTROL = 2,
       SW_OVERRIDE = 4, RETAIN_FF = 1u << 11, WAIT_MASK = 0xfffu << 12,
       SM8750_WAITS = (2u << 20) | (2u << 16) | (8u << 12) };

NTSTATUS AdrenoCxInitialize(ADRENO_CX_DOMAIN *d, void *gpucc, SIZE_T bytes)
{
    PAGED_CODE();
    if (!d || !gpucc || ((ULONG_PTR)gpucc & 3) || bytes < CX_STATUS + sizeof(ULONG))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    RtlZeroMemory(d, sizeof(*d));
    KeInitializeMutex(&d->lock, 0);
    d->control = (volatile ULONG *)((PUCHAR)gpucc + CX_CONTROL);
    d->status = (volatile ULONG *)((PUCHAR)gpucc + CX_STATUS);
    return STATUS_SUCCESS;
}

static NTSTATUS wait_on(ADRENO_CX_DOMAIN *d)
{
    ULONGLONG until = KeQueryInterruptTime() + 2000 * 10;
    do {
        ULONG state = READ_REGISTER_ULONG(d->status);
        if (state == MAXULONG) return STATUS_DEVICE_NOT_CONNECTED;
        if (state & (1u << 31)) return STATUS_SUCCESS;
        KeStallExecutionProcessor(1);
    } while (KeQueryInterruptTime() < until);
    return STATUS_IO_TIMEOUT;
}

NTSTATUS AdrenoCxAcquire(ADRENO_CX_DOMAIN *d, ADRENO_CX_REFERENCE *ref)
{
    PAGED_CODE();
    if (!d || !d->control || !ref || (ref->domain && ref->domain != d)) return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&d->lock, Executive, KernelMode, FALSE, NULL);
    ULONG before = READ_REGISTER_ULONG(d->control);
    NTSTATUS status = STATUS_DEVICE_HARDWARE_ERROR;
    if (before == MAXULONG) { status = STATUS_DEVICE_NOT_CONNECTED; goto done; }
    if (before & (HW_CONTROL | SW_OVERRIDE)) { status = STATUS_DEVICE_BUSY; goto done; }
    if (!d->initialized) {
        /* A retained AP vote after D3 may be Windows' own request. */
        d->boot_ref = !(before & SW_COLLAPSE) && (!d->resuming || d->boot_ref);
        d->initialized = TRUE;
        d->resuming = FALSE;
    }
    if (!ref->domain) {
        if (d->users == MAXULONG) { status = STATUS_INTEGER_OVERFLOW; goto done; }
        ++d->users;
        ref->domain = d;
    }
    if (before & SW_COLLAPSE) {
        status = update(d->control, WAIT_MASK | SW_COLLAPSE, SM8750_WAITS);
        if (status != STATUS_SUCCESS) goto done;
        KeStallExecutionProcessor(1);
    }
    status = wait_on(d);
    if (status == STATUS_SUCCESS && !d->boot_ref)
        status = update(d->control, RETAIN_FF, RETAIN_FF);
done:
    KeReleaseMutex(&d->lock, FALSE);
    return status;
}

static NTSTATUS release_last(ADRENO_CX_DOMAIN *d)
{
    ULONG before = READ_REGISTER_ULONG(d->control);
    if (before == MAXULONG) return STATUS_DEVICE_NOT_CONNECTED;
    if (before & (HW_CONTROL | SW_OVERRIDE)) return STATUS_DEVICE_BUSY;
    if (before & SW_COLLAPSE) return STATUS_SUCCESS;
    /* Retention precedes the last AP vote, including a warm inherited vote.
     * Other hardware masters can keep the domain on after our release. */
    NTSTATUS s = update(d->control, RETAIN_FF, RETAIN_FF);
    if (s == STATUS_SUCCESS) s = update(d->control, SW_COLLAPSE, SW_COLLAPSE);
    if (s == STATUS_SUCCESS) {
        LARGE_INTEGER delay;
        delay.QuadPart = -5000; /* SM8750 votable GDSC settling time, 500 us. */
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    return s;
}

NTSTATUS AdrenoCxRelease(ADRENO_CX_DOMAIN *d, ADRENO_CX_REFERENCE *ref)
{
    PAGED_CODE();
    if (!d || !d->control || !ref || (ref->domain && ref->domain != d)) return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&d->lock, Executive, KernelMode, FALSE, NULL);
    NTSTATUS s = STATUS_SUCCESS;
    if (ref->domain) {
        if (!d->users) s = STATUS_INVALID_DEVICE_STATE;
        else if (d->users == 1 && !d->boot_ref) s = release_last(d);
        if (s == STATUS_SUCCESS) { ref->domain = NULL; --d->users; }
    }
    KeReleaseMutex(&d->lock, FALSE);
    return s;
}

NTSTATUS AdrenoCxReleaseBootReference(ADRENO_CX_DOMAIN *d)
{
    PAGED_CODE();
    if (!d || !d->control) return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&d->lock, Executive, KernelMode, FALSE, NULL);
    NTSTATUS s = d->initialized ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    if (d->boot_ref && !d->users) s = release_last(d);
    if (s == STATUS_SUCCESS) d->boot_ref = FALSE;
    KeReleaseMutex(&d->lock, FALSE);
    return s;
}
