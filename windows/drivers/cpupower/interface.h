/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <ntddk.h>
#include "scmi_perf.h"

/* IRP_MN_QUERY_INTERFACE on the CPUCP PnP device. Hold the interface
 * reference while using the transport. Message runs only at PASSIVE_LEVEL. */
static const GUID GUID_PIANO_CPUCP_SCMI_INTERFACE = {
    0x456d25e9, 0xf86e, 0x4d3c, { 0x85, 0xb9, 0xc7, 0x28, 0x76, 0x9a, 0x8e, 0x34 }
};
typedef struct {
    INTERFACE Header;
    struct scmi_transport Transport;
} CPUCP_SCMI_INTERFACE;
