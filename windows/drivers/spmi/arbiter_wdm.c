/* SPDX-License-Identifier: BSD-2-Clause */
#include "arbiter_wdm.h"

/* Raw bus addresses identify the regions; only translated resources are mapped. */
static const struct spmi_window {
    ULONG address, size;
    BOOLEAN writable;
} piano_spmi[SPMI_REGIONS] = {
    [SPMI_CORE]      = {0x0c400000, 0x3000,   FALSE},
    [SPMI_CONFIG]    = {0x0c42d000, 0x4000,   FALSE},
    [SPMI_CHANNELS]  = {0x0c500000, 0x400000, TRUE},
    [SPMI_OBSERVER]  = {0x0c440000, 0x80000,  TRUE}, /* The read command is submitted here. */
    [SPMI_INTERRUPT] = {0x0c4c0000, 0x10000,  FALSE}
};

static int read_word(void *ctx, enum spmi_region region, uint32_t offset, uint32_t *result)
{
    SPMI_PMIC_BUS *b = ctx;
    if ((unsigned)region >= SPMI_REGIONS || !b->window[region] || (offset & 3) ||
        offset > piano_spmi[region].size - sizeof(ULONG)) return 0;
    *result = READ_REGISTER_ULONG((PULONG)(b->window[region] + offset));
    return 1;
}

static int write_word(void *ctx, enum spmi_region region, uint32_t offset, uint32_t value)
{
    SPMI_PMIC_BUS *b = ctx;
    if ((unsigned)region >= SPMI_REGIONS || !b->window[region] || !piano_spmi[region].writable ||
        (offset & 3) || offset > piano_spmi[region].size - sizeof(ULONG)) return 0;
    WRITE_REGISTER_ULONG((PULONG)(b->window[region] + offset), value);
    return 1;
}

static void delay_us(void *ctx, unsigned us)
{
    UNREFERENCED_PARAMETER(ctx);
    KeStallExecutionProcessor(us);
}

static void unmap(SPMI_PMIC_BUS *b)
{
    for (unsigned r = 0; r < SPMI_REGIONS; ++r) {
        if (b->window[r]) MmUnmapIoSpace(b->window[r], piano_spmi[r].size);
        b->window[r] = NULL;
    }
    RtlZeroMemory(&b->arbiter, sizeof(b->arbiter));
}

NTSTATUS SpmiPmicPrepareBus(SPMI_PMIC_BUS *b, PCM_PARTIAL_RESOURCE_LIST raw,
                            PCM_PARTIAL_RESOURCE_LIST translated)
{
    if (!b || !raw || !translated || raw->Count != translated->Count) return STATUS_INVALID_PARAMETER;
    for (unsigned r = 0; r < SPMI_REGIONS; ++r)
        if (b->window[r]) return STATUS_DEVICE_BUSY;
    PHYSICAL_ADDRESS start[SPMI_REGIONS] = {0};
    for (ULONG i = 0; i < raw->Count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc = &raw->PartialDescriptors[i], cpu = &translated->PartialDescriptors[i];
        if (desc->Type != CmResourceTypeMemory) continue;
        for (unsigned r = 0; r < SPMI_REGIONS; ++r) {
            if ((ULONGLONG)desc->u.Memory.Start.QuadPart != piano_spmi[r].address) continue;
            if (start[r].QuadPart || desc->u.Memory.Length != piano_spmi[r].size ||
                cpu->Type != CmResourceTypeMemory || cpu->u.Memory.Start.QuadPart <= 0 ||
                cpu->u.Memory.Length != piano_spmi[r].size ||
                (piano_spmi[r].writable && ((desc->Flags | cpu->Flags) & CM_RESOURCE_MEMORY_READ_ONLY)) ||
                ((desc->Flags | cpu->Flags) & CM_RESOURCE_MEMORY_WRITE_ONLY)) return STATUS_DEVICE_CONFIGURATION_ERROR;
            start[r] = cpu->u.Memory.Start;
        }
    }
    for (unsigned r = 0; r < SPMI_REGIONS; ++r)
        if (!start[r].QuadPart) return STATUS_DEVICE_CONFIGURATION_ERROR;
    ExInitializeFastMutex(&b->lock);
    for (unsigned r = 0; r < SPMI_REGIONS; ++r) {
        ULONG protection = piano_spmi[r].writable ? PAGE_READWRITE : PAGE_READONLY;
        b->window[r] = MmMapIoSpaceEx(start[r], piano_spmi[r].size, protection | PAGE_NOCACHE);
        if (!b->window[r]) {
            unmap(b);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    const struct spmi_register_io io = { b, read_word, write_word, delay_us };
    if (spmi_v7_start(&b->arbiter, &io, 0, 0) != SPMI_OK) {
        unmap(b);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return STATUS_SUCCESS;
}

NTSTATUS SpmiPmicReleaseBus(SPMI_PMIC_BUS *b, BOOLEAN surprise)
{
    if (!b) return STATUS_INVALID_PARAMETER;
    if (!b->arbiter.count) return STATUS_SUCCESS;
    ExAcquireFastMutex(&b->lock);
    if (!surprise && b->arbiter.pending) {
        (void)spmi_v7_retire(&b->arbiter);
        if (b->arbiter.pending) {
            ExReleaseFastMutex(&b->lock);
            return STATUS_DEVICE_BUSY;
        }
    }
    unmap(b);
    ExReleaseFastMutex(&b->lock);
    return STATUS_SUCCESS;
}
