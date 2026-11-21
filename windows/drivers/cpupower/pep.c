/* SPDX-License-Identifier: BSD-2-Clause */
#include "pep.h"

#define CPU_PERF_TAG 'frPC'

static NTSTATUS discovery_error(int err)
{
    switch (err) {
    case SCMI_PERF_INVALID: return STATUS_INVALID_PARAMETER;
    case SCMI_PERF_UNSUPPORTED: return STATUS_NOT_SUPPORTED;
    case SCMI_PERF_DENIED: return STATUS_ACCESS_DENIED;
    case SCMI_PERF_NOT_FOUND: return STATUS_DEVICE_DOES_NOT_EXIST;
    case SCMI_PERF_TIMEOUT: return STATUS_IO_TIMEOUT;
    case SCMI_PERF_BUSY: return STATUS_DEVICE_BUSY;
    default: return STATUS_DEVICE_PROTOCOL_ERROR;
    }
}

NTSTATUS CpuPepOpen(CPU_PEP_DOMAIN *d, PDEVICE_OBJECT device, const struct scmi_perf *perf,
                    ULONG id, ULONG members)
{
    if (!d || !device || !perf || !members || KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(d, sizeof(*d));
    size_t needed;
    struct scmi_perf_domain desc;
    int err = scmi_perf_describe(perf, id, NULL, 0, &desc, &needed);
    if (err != SCMI_PERF_NO_SPACE) return discovery_error(err);
    /* Windows uses a byte count and reserves state 0xff for autonomous mode. */
    if (!needed || needed > MAXUCHAR) return STATUS_NOT_SUPPORTED;
    struct scmi_perf_state *states = ExAllocatePool2(POOL_FLAG_NON_PAGED, needed * sizeof(*states), CPU_PERF_TAG);
    if (!states) return STATUS_INSUFFICIENT_RESOURCES;
    err = scmi_perf_describe(perf, id, states, needed, &desc, &needed);
    if (err || !desc.can_set) {
        ExFreePoolWithTag(states, CPU_PERF_TAG);
        return err ? discovery_error(err) : STATUS_NOT_SUPPORTED;
    }
    d->Work = IoAllocateWorkItem(device);
    if (!d->Work) {
        ExFreePoolWithTag(states, CPU_PERF_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    d->States = states;
    d->Domain = desc;
    d->Perf = *perf;
    d->Members = members;
    KeInitializeSpinLock(&d->Lock);
    KeInitializeEvent(&d->Stop, NotificationEvent, FALSE);
    KeInitializeEvent(&d->Idle, NotificationEvent, TRUE);
    return STATUS_SUCCESS;
}

BOOLEAN CpuPepQueryCapabilities(const CPU_PEP_DOMAIN *d, ULONG lowest_nonlinear,
                                PPEP_PPM_QUERY_PERF_CAPABILITIES caps)
{
    if (!d || !d->States || !caps) return FALSE;
    ULONG low = d->States[d->Domain.count - 1].performance, high = d->States[0].performance;
    ULONG nominal = d->Domain.sustained_performance;
    /* No energy-efficiency threshold can be inferred from frequency alone.
     * The platform supplies this quantity in the firmware's performance units. */
    if (!nominal || nominal < low || nominal > high || lowest_nonlinear < low || lowest_nonlinear > nominal)
        return FALSE;
    size_t state;
    if (scmi_perf_choose(&d->Domain, nominal, nominal, nominal, &state) ||
        scmi_perf_choose(&d->Domain, lowest_nonlinear, lowest_nonlinear, lowest_nonlinear, &state))
        return FALSE;  /* Windows requires each threshold to appear in the discrete table. */
    *caps = (PEP_PPM_QUERY_PERF_CAPABILITIES){ high, nominal, lowest_nonlinear, low, d->Domain.id, d->Members };
    return TRUE;
}

BOOLEAN CpuPepQueryStates(const CPU_PEP_DOMAIN *d, PPEP_PPM_QUERY_DISCRETE_PERF_STATES states)
{
    if (!d || !d->States || !states || !states->States || states->Count != d->Domain.count) return FALSE;
    for (ULONG i = 0; i < states->Count; ++i) {
        const struct scmi_perf_state *s = &d->States[i];
        states->States[i] = (PEP_PROCESSOR_PERF_STATE){
            .Performance = s->performance, .Frequency = (ULONG)(((ULONGLONG)s->khz + 500) / 1000)
        };
    }
    return TRUE;
}

static IO_WORKITEM_ROUTINE apply_request;
static VOID apply_request(PDEVICE_OBJECT device, PVOID context)
{
    CPU_PEP_DOMAIN *d = context;
    UNREFERENCED_PARAMETER(device);
    for (;;) {
        KIRQL old;
        KeAcquireSpinLock(&d->Lock, &old);
        if (d->Closing || !d->HaveRequest) {
            d->Busy = FALSE;
            KeSetEvent(&d->Idle, IO_NO_INCREMENT, FALSE);
            KeReleaseSpinLock(&d->Lock, old);
            return;
        }
        ULONGLONG now = KeQueryInterruptTime();
        ULONGLONG allowed = d->LastIssued + (ULONGLONG)d->Domain.rate_limit_us * 10;
        if (d->LastIssued && now < allowed) {
            LARGE_INTEGER delay;
            delay.QuadPart = -(LONGLONG)(allowed - now);
            KeReleaseSpinLock(&d->Lock, old);
            (void)KeWaitForSingleObject(&d->Stop, Executive, KernelMode, FALSE, &delay);
            continue;  /* A newer OS request may have arrived during the wait. */
        }
        SIZE_T state = d->Requested;
        d->HaveRequest = FALSE;
        KeReleaseSpinLock(&d->Lock, old);
        int err = scmi_perf_set(&d->Perf, &d->Domain, state);
        KeAcquireSpinLock(&d->Lock, &old);
        d->LastIssued = KeQueryInterruptTime();
        d->LastStatus = err;  /* Not a measured core frequency. Use PERF_LEVEL_GET to read back. */
        KeReleaseSpinLock(&d->Lock, old);
    }
}

static BOOLEAN queue_state(CPU_PEP_DOMAIN *d, SIZE_T state)
{
    if (KeGetCurrentIrql() > DISPATCH_LEVEL) return FALSE;
    KIRQL old;
    KeAcquireSpinLock(&d->Lock, &old);
    BOOLEAN accepted = !d->Closing;
    if (accepted) {
        d->HaveRequest = TRUE;
        d->Requested = state;
        if (!d->Busy) {
            d->Busy = TRUE;
            KeClearEvent(&d->Idle);
            IoQueueWorkItem(d->Work, apply_request, DelayedWorkQueue, d);
        }
    }
    KeReleaseSpinLock(&d->Lock, old);
    return accepted;
}

BOOLEAN CpuPepRequest(CPU_PEP_DOMAIN *d, const PEP_PPM_PERF_SET *r)
{
    size_t state;
    return d && d->States && r &&
        !scmi_perf_choose(&d->Domain, r->MinimumPerformance, r->DesiredPerformance, r->MaximumPerformance, &state) &&
        queue_state(d, state);
}

BOOLEAN CpuPepRequestState(CPU_PEP_DOMAIN *d, const PEP_PPM_PERF_SET_STATE *r)
{
    if (!d || !d->States || !r || r->MinimumPerformanceState >= d->Domain.count ||
        r->MaximumPerformanceState > r->MinimumPerformanceState ||
        r->DesiredPerformanceState >= d->Domain.count) return FALSE;
    size_t state = max(r->DesiredPerformanceState, r->MaximumPerformanceState);
    return queue_state(d, min(state, r->MinimumPerformanceState));
}

VOID CpuPepClose(CPU_PEP_DOMAIN *d)
{
    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    if (!d->Work) return;
    KIRQL old;
    KeAcquireSpinLock(&d->Lock, &old);
    d->Closing = TRUE;
    KeSetEvent(&d->Stop, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&d->Lock, old);
    (void)KeWaitForSingleObject(&d->Idle, Executive, KernelMode, FALSE, NULL);
    /* Pair with the worker's final release before returning its storage. */
    KeAcquireSpinLock(&d->Lock, &old);
    KeReleaseSpinLock(&d->Lock, old);
    IoFreeWorkItem(d->Work);
    ExFreePoolWithTag(d->States, CPU_PERF_TAG);
    d->Work = NULL;
    d->States = NULL;
}
