/* SPDX-License-Identifier: MIT */
#pragma once
#include <ntddk.h>

static const GUID GUID_DEVINTERFACE_SM8750_GPUCC = {
    0x54a92ac3, 0x66a0, 0x4685, {0xb7, 0xfb, 0xbd, 0x5e, 0xb9, 0x48, 0x7d, 0x93}};
static const GUID GUID_SM8750_GPUCC_INTERFACE = {
    0x9e6b2550, 0x7c8d, 0x407a, {0xb0, 0xcd, 0xbe, 0x04, 0x5d, 0x31, 0xfc, 0xc5}};
enum adreno_clock_client { AdrenoGmuClocks, AdrenoRenderBusClocks, AdrenoSmmuClocks };
typedef struct {
    INTERFACE Header;
    /* Use a stable identity per independently held reference. After an
     * unsuccessful enable, release before dropping the interface or rails. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS (*SetClocks)(PVOID, PVOID client, enum adreno_clock_client, BOOLEAN enable);
} ADRENO_GPUCC_INTERFACE;
