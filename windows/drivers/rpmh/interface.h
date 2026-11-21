/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <ntddk.h>
#include "provider.h"

/* Kernel IRP_MN_QUERY_INTERFACE only; no register addresses or TCS
 * command streams cross this interface. All operations run at PASSIVE_LEVEL.
 * Release client votes before leaving D0; Close can be retried after a
 * transport failure. Dereference the interface only after Close succeeds. */
static const GUID GUID_PIANO_RPMH_INTERFACE = {
    0xdda1f9f3,0x747d,0x4750,{0x99,0xfa,0x17,0x2c,0xae,0x1c,0xd8,0xe6}
};
#define PIANO_RPMH_INTERFACE_VERSION 2
typedef struct {
    INTERFACE Header;
    NTSTATUS (*Open)(PVOID, struct rpmh_client **);
    NTSTATUS (*Close)(PVOID, struct rpmh_client *);
    NTSTATUS (*SetLevel)(PVOID, struct rpmh_client *, enum rpmh_rail, ULONG);
    NTSTATUS (*SetBandwidth)(PVOID, struct rpmh_client *, enum rpmh_fabric_path, ULONG, ULONG);
    NTSTATUS (*Adopt)(PVOID, const struct rpmh_boot_votes *);
    /* VCC, VCCQ. If the request fails, quiesce the dependent device and
     * retry before using it: a timed-out transition may still be retiring. */
    NTSTATUS (*SetUfsSupplies)(PVOID, struct rpmh_client *, enum rpmh_supply_load, enum rpmh_supply_load);
    NTSTATUS (*AdoptRegulators)(PVOID, const struct rpmh_vrm_votes *);
} PIANO_RPMH_INTERFACE;
