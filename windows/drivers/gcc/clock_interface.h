/* SPDX-License-Identifier: MIT */
#pragma once
#include <ntddk.h>

enum sm8750_gcc_pll { Sm8750Gpll0, Sm8750Gpll4, Sm8750Gpll0Even, Sm8750GccPllCount };
static const GUID GUID_SM8750_GCC_INTERFACE = {
    0x62b382ac, 0x7c72, 0x4928, {0xb5, 0x61, 0x19, 0x4a, 0x5f, 0x20, 0xec, 0xce}};
#define SM8750_GCC_INTERFACE_VERSION 1
typedef struct {
    PULONG clocks, phy_domain;
} SM8750_UFS_GCC_WINDOWS;
/* Kernel PnP query interface from the single GCC owner. The caller holds a
 * separate reference for each active parent and keeps its XO/CX votes up. */
typedef struct {
    INTERFACE Header;
    NTSTATUS (*Set)(PVOID, enum sm8750_gcc_pll, BOOLEAN *held, BOOLEAN enable);
    /* Lend only the UFS register block and its PHY domain, not the PLLs,
     * global vote words or an arbitrary physical memory mapping. */
    NTSTATUS (*OpenUfs)(PVOID, SM8750_UFS_GCC_WINDOWS *);
    VOID (*CloseUfs)(PVOID, SM8750_UFS_GCC_WINDOWS *);
} SM8750_GCC_INTERFACE;
