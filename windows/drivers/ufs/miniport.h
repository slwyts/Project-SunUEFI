/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "platform_storport.h"
#include "storport_link.h"

enum ufs_adapter_state {
    UfsAbsent, UfsMapped, UfsStarting, UfsPrepared, UfsOnline, UfsPoweringDown, UfsAsleep,
    UfsStopped, UfsRecovering, UfsFault,
};

typedef struct {
    UFS_IO_QUEUE io;
    UFS_PLATFORM_DEVICE platform;
    struct ufs_link link;
    struct ufs_media_settings media;
    enum ufs_link_operation power_operation;
    enum ufs_adapter_state state;
    ULONGLONG frequency, retire_since;
    ULONGLONG power_since, power_timeout, worker_since;
    PVOID power_srb;
    volatile LONG media_off;
    volatile LONG sequence;
    BOOLEAN start_timer;
    BOOLEAN prepare_only;
    BOOLEAN power_touched;
    BOOLEAN needs_reset;
    PVOID link_power_context;
    BOOLEAN (*link_power_begin)(PVOID);
    BOOLEAN (*can_retain_s0)(PVOID);
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
