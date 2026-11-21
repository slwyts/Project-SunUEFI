/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "platform_storport.h"
#include "storport_link.h"

enum ufs_adapter_state {
    UfsAbsent, UfsMapped, UfsStarting, UfsOnline, UfsStopped, UfsFault,
};

typedef struct {
    UFS_IO_QUEUE io;
    UFS_PLATFORM_DEVICE platform;
    struct ufs_link link;
    enum ufs_adapter_state state;
    ULONGLONG frequency, retire_since;
    BOOLEAN start_timer;
    BOOLEAN needs_reset;
    PVOID link_power_context;
    void (*link_power_ready)(PVOID, const uint8_t *);
} UFS_ADAPTER;

/* The Piano bus/power implementation calls this from its own matched PnP
 * discovery, at PASSIVE_LEVEL, with real supply/clock/reset operations. The
 * generic PCI entry never selects QMP from a PCI class or an ACPI family ID.
 */
_IRQL_requires_(PASSIVE_LEVEL)
ULONG UfsMiniportFindPiano(UFS_ADAPTER *adapter, PPORT_CONFIGURATION_INFORMATION configuration,
                          const struct ufs_platform_power *power);
/* Supply a matched HwFindAdapter for the bus which actually enumerated the
 * device. A board-specific entry uses two ranges and UfsMiniportFindPiano.
 */
NTSTATUS UfsMiniportRegister(PDRIVER_OBJECT driver, PUNICODE_STRING registry, INTERFACE_TYPE bus,
                            PHW_FIND_ADAPTER discover, ULONG ranges);
