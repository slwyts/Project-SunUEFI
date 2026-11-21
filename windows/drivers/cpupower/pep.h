/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <ntddk.h>
#include <pepfx.h>
#include "scmi_perf.h"

typedef struct {
    struct scmi_perf Perf;
    struct scmi_perf_domain Domain;
    struct scmi_perf_state *States;
    PIO_WORKITEM Work;
    KSPIN_LOCK Lock;
    KEVENT Stop, Idle;
    ULONG Members;
    SIZE_T Requested;
    ULONGLONG LastIssued;
    int LastStatus;
    BOOLEAN Busy, Closing, HaveRequest;
} CPU_PEP_DOMAIN;

/* The platform owns one domain per physical performance domain, shared by
 * all its processors. Stop dispatching PEP callbacks before closing it. */
NTSTATUS CpuPepOpen(CPU_PEP_DOMAIN *domain, PDEVICE_OBJECT device, const struct scmi_perf *perf,
                    ULONG id, ULONG members);
VOID CpuPepClose(CPU_PEP_DOMAIN *domain);
BOOLEAN CpuPepQueryCapabilities(const CPU_PEP_DOMAIN *domain, ULONG lowest_nonlinear,
                                PPEP_PPM_QUERY_PERF_CAPABILITIES caps);
BOOLEAN CpuPepQueryStates(const CPU_PEP_DOMAIN *domain, PPEP_PPM_QUERY_DISCRETE_PERF_STATES states);
BOOLEAN CpuPepRequest(CPU_PEP_DOMAIN *domain, const PEP_PPM_PERF_SET *request);
BOOLEAN CpuPepRequestState(CPU_PEP_DOMAIN *domain, const PEP_PPM_PERF_SET_STATE *request);
