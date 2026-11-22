/* SPDX-License-Identifier: MIT */
#pragma once
#include <ntddk.h>

enum sm8750_gcc_pll { Sm8750Gpll0, Sm8750Gpll4, Sm8750Gpll0Even, Sm8750GccPllCount };
static const GUID GUID_SM8750_GCC_INTERFACE = {
    0x493ce1ef, 0x9b29, 0x49aa, {0xaf, 0xbb, 0x3d, 0x4a, 0x1b, 0xb7, 0xe8, 0x69}};
/* Locate the provider stack, then query its kernel interface. */
static const GUID GUID_DEVINTERFACE_SM8750_GCC = {
    0x5de5b1d2, 0x3534, 0x46f0, {0xa3, 0xcb, 0xdf, 0x3c, 0x51, 0x34, 0xfc, 0xa6}};
#define SM8750_GCC_INTERFACE_VERSION 1
/* Kernel PnP query interface from the single GCC owner. The caller holds a
 * separate reference for each active parent and keeps its XO/CX votes up. */
typedef struct {
    INTERFACE Header;
    NTSTATUS (*Set)(PVOID, enum sm8750_gcc_pll, BOOLEAN *held, BOOLEAN enable);
    /* The storage adapter provides its own stable, non-NULL identity. Open
     * holds the supplier in D0 during S0. No pointer to MMIO leaves GCC. */
    NTSTATUS (*OpenUfs)(PVOID, PVOID adapter);
    VOID (*CloseUfs)(PVOID, PVOID adapter);
    /* Callable at DIRQL. A D3/D0 cycle does not restore the guarantee
     * for an old adapter, even if the supplier can access registers again. */
    BOOLEAN (*UfsPowerRetained)(PVOID, PVOID adapter);
    ULONG (*ReadUfs)(PVOID, PVOID adapter, ULONG offset);
    VOID (*WriteUfs)(PVOID, PVOID adapter, ULONG offset, ULONG value);
} SM8750_GCC_INTERFACE;
