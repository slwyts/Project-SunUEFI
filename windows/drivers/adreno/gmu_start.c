/* SPDX-License-Identifier: MIT */
#include "wddm_pnp.h"

/* A830 GMU register numbers are GPU-relative words, as in a6xx_gmu.xml.
 * RSC registers below are relative to its own translated PnP window. */
enum {
    gmu_icache = 0x1f400, gmu_dcache = 0x1f401, gmu_sys_bus = 0x1f40f, gmu_gbif_qos = 0x1f50b,
    gx_power = 0x1f7e8, rpmh_power = 0x1f7e9, cx_fal = 0x1f7ec, cx_falnext = 0x1f7ed,
    cm3_reset = 0x1f800, cm3_boot = 0x1f801,
    cm3_result = 0x1f81c, cm3_config = 0x1f82d, intr_clear = 0x1f991, intr_info = 0x1f992,
    intr_mask = 0x1f993, host_intr_set = 0x1f994,
    log_address = 0x1f9c8, log_write = 0x1f9c9, chip_id = 0x1f9ca,
    power_counter = 0x1fc10, ao_intr_clear = 0x23b04, ao_intr_mask = 0x23b06,
    rsc_req = 0x23b07, rsc_ack = 0x23b08, cx_busy = 0x23b0c, fence_control = 0x23b10,
    fence_range = 0x23b11, fence_clear = 0x23b14,
    gbif_config = 0x3c00, qsb_side0 = 0x3c03, gbif_halt = 0x3c45, gbif_ack = 0x3c46,
    rsc_status = 4, rsc_seq_busy = 0x101,
    hfi_bw = 3, hfi_start = 10, hfi_core = 14, hfi_gx_vote = 30, hfi_slumber = 33,
    gmu_iort_input = 0x03000005 /* Piano XBL NAMEDNODE_GPU, not its physical stream ID. */
};
#define GX_WAKE_ACK (1u << 31)

static volatile ULONG *gmu_reg(ADRENO_RENDER_DEVICE *d, unsigned word)
{
    return d->gmu.words + (word - ADRENO_GMU_BASE_WORD);
}

static void gmu_write(ADRENO_RENDER_DEVICE *d, unsigned word, ULONG value)
{
    WRITE_REGISTER_ULONG(gmu_reg(d, word), value);
}

static void short_delay(void)
{
    LARGE_INTEGER delay;
    delay.QuadPart = -1000; /* 100 us; caller is at PASSIVE_LEVEL. */
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

static NTSTATUS poll(volatile ULONG *reg, ULONG mask, ULONG expected, ULONG timeout_ms)
{
    ULONGLONG start = KeQueryInterruptTime();
    do {
        ULONG value = READ_REGISTER_ULONG(reg);
        if (value == MAXULONG) return STATUS_DEVICE_NOT_CONNECTED;
        if ((value & mask) == expected) return STATUS_SUCCESS;
        short_delay();
    } while (KeQueryInterruptTime() - start < (ULONGLONG)timeout_ms * 10000);
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS gmu_boot_state(ADRENO_RENDER_DEVICE *d, ULONG *irq)
{
    ULONG boot = READ_REGISTER_ULONG(gmu_reg(d, cm3_result));
    *irq = READ_REGISTER_ULONG(gmu_reg(d, intr_info));
    if (boot == MAXULONG || *irq == MAXULONG) return STATUS_DEVICE_NOT_CONNECTED;
    if ((boot & 0xe00) || (*irq & (1u << 15))) return STATUS_DEVICE_HARDWARE_ERROR;
    return (boot & 0x1ff) == 0x100 ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}

static NTSTATUS wait_for_boot(ADRENO_RENDER_DEVICE *d)
{
    ULONGLONG start = KeQueryInterruptTime();
    do {
        ULONG irq;
        NTSTATUS status = gmu_boot_state(d, &irq);
        if (status != STATUS_DEVICE_NOT_READY) return status;
        short_delay();
    } while (KeQueryInterruptTime() - start < 100 * 10000);
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS drain_gbif(ADRENO_RENDER_DEVICE *d)
{
    /* Gen8 stops new client requests, then drains the AXI arbiter. Both
     * acknowledgements are required before HAL can replace or detach DMA. */
    for (ULONG mask = 1; mask <= 2; mask <<= 1) {
        WRITE_REGISTER_ULONG(d->gpu.words + gbif_halt, mask);
        NTSTATUS s = poll(d->gpu.words + gbif_ack, mask, mask, 100);
        if (s != STATUS_SUCCESS) return s;
    }
    WRITE_REGISTER_ULONG(d->gpu.words + gbif_halt, 0);
    return poll(d->gpu.words + gbif_ack, 3, 0, 100);
}

static NTSTATUS finish_wake(ADRENO_RENDER_DEVICE *d)
{
    NTSTATUS s = poll(gmu_reg(d, rsc_ack), 2, 2, 100);
    if (s == STATUS_SUCCESS) s = poll(d->rsc.words + rsc_seq_busy, MAXULONG, 0, 100);
    if (s == STATUS_SUCCESS) {
        gmu_write(d, rsc_req, 0);
        d->gmu_state.wake_pending = FALSE;
        d->gmu_state.rsc_sleeping = FALSE;
        d->gmu_state.rsc_awake = TRUE;
    }
    return s;
}

static NTSTATUS hold_reset(ADRENO_RENDER_DEVICE *d)
{
    gmu_write(d, cm3_reset, 1);
    return poll(gmu_reg(d, cm3_reset), 1, 1, 100);
}

NTSTATUS AdrenoHoldGx(ADRENO_RENDER_DEVICE *d)
{
    PAGED_CODE();
    ADRENO_GMU_DEVICE *g = &d->gmu_state;
    if (!g->reset_released)
        return STATUS_INVALID_DEVICE_STATE;

    ULONGLONG start = KeQueryInterruptTime();
    do {
        ULONG irq;
        NTSTATUS status = gmu_boot_state(d, &irq);
        if (status != STATUS_SUCCESS) return status;
        if (g->gx_request == AdrenoGxReleased) {
            /* An OOB acknowledgement has no sequence number. A previous
             * latch must read clear before a fresh request may be posted. */
            if (irq & GX_WAKE_ACK) {
                gmu_write(d, intr_clear, GX_WAKE_ACK);
                short_delay();
            } else {
                g->gx_request = AdrenoGxWaitAck;
                gmu_write(d, host_intr_set, 1u << 30);
            }
            continue;
        }
        if (g->gx_request == AdrenoGxWaitAck && (irq & GX_WAKE_ACK)) {
            gmu_write(d, intr_clear, GX_WAKE_ACK);
            g->gx_request = AdrenoGxWaitPower;
        }
        if (g->gx_request != AdrenoGxWaitAck) {
            ULONG gx = READ_REGISTER_ULONG(gmu_reg(d, gx_power));
            if (gx == MAXULONG) return STATUS_DEVICE_NOT_CONNECTED;
            if (!(gx & 3)) {
                g->gx_request = AdrenoGxHeld;
                return STATUS_SUCCESS;
            }
        }
        short_delay();
    } while (KeQueryInterruptTime() - start < 100 * 10000);
    return STATUS_IO_TIMEOUT;
}

void AdrenoReleaseGx(ADRENO_RENDER_DEVICE *d)
{
    PAGED_CODE();
    if (d->gmu_state.gx_request != AdrenoGxReleased) {
        /* Gen8 has no acknowledgement for relinquishing host access. */
        gmu_write(d, host_intr_set, 1u << 31);
        d->gmu_state.gx_request = AdrenoGxReleased;
    }
}

NTSTATUS AdrenoStartGmu(ADRENO_RENDER_DEVICE *d)
{
    union {
        struct adreno_bw_table table;
        uint32_t words[sizeof(struct adreno_bw_table) / sizeof(uint32_t)];
    } bw;
    PAGED_CODE();
    if (!d || !d->pdo || !d->firmware.gmu.data || !d->gmu.words || !d->gpu.words || !d->rsc.words ||
        d->gmu.bytes < (fence_clear + 1 - ADRENO_GMU_BASE_WORD) * sizeof(ULONG) ||
        d->gpu.bytes < (gbif_ack + 1) * sizeof(ULONG) || d->rsc.bytes < (rsc_seq_busy + 1) * sizeof(ULONG))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    const ADRENO_GMU_PLATFORM *p = d->platform;
    ADRENO_GMU_DEVICE *g = &d->gmu_state;
    if (!p || !p->clocks || !p->clocks->gpucc || !p->SetRailsAndBus)
        return STATUS_DEVICE_NOT_READY;
    if (g->rails) return STATUS_INVALID_DEVICE_STATE;
    if (!adreno_hfi_bandwidth_table(&p->bandwidth, &bw.table) ||
        !adreno_hfi_select_level(&p->perf, &p->bandwidth, p->initial_gpu_khz, p->initial_ddr_kbps,
                                 &g->initial_level))
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    g->rails = TRUE;
    NTSTATUS s = p->SetRailsAndBus(p->context, TRUE);
    if (s != STATUS_SUCCESS) return s;
    s = AdrenoSetGpuClocks(p->clocks, &g->clock_ref, AdrenoGmuClocks, TRUE);
    if (s != STATUS_SUCCESS) return s;

    gmu_write(d, cx_falnext, 1);
    gmu_write(d, cx_fal, 1);
    ULONG rsc = READ_REGISTER_ULONG(d->rsc.words + rsc_status);
    if (rsc == MAXULONG) return STATUS_DEVICE_NOT_CONNECTED;
    if (g->rsc_sleeping || (rsc & (1u << 16))) {
        g->wake_pending = TRUE;
        gmu_write(d, rsc_req, 2);
        s = finish_wake(d);
        if (s != STATUS_SUCCESS) return s;
    }
    gmu_write(d, fence_clear, 7);
    gmu_write(d, fence_control, 0);
    g->reset_pending = g->gbif_pending = TRUE;
    s = hold_reset(d);
    if (s != STATUS_SUCCESS) return s;
    g->reset_pending = FALSE;
    s = drain_gbif(d);
    if (s != STATUS_SUCCESS) return s;
    g->gbif_pending = FALSE;
    s = AdrenoGmuMemoryOpen(&g->memory, d->pdo, gmu_iort_input, &d->firmware.gmu);
    if (s != STATUS_SUCCESS) return s;
    s = AdrenoUploadGmu(&d->firmware.gmu, d->gmu.words + ADRENO_GMU_ITCM_WORD, g->memory.cache.mdl);
    if (s != STATUS_SUCCESS) return s;

    gmu_write(d, rpmh_power, 0xf); /* Gen8's initial state before firmware enables GX. */
    gmu_write(d, cm3_result, 0);
    s = poll(gmu_reg(d, cm3_result), MAXULONG, 0, 100);
    if (s != STATUS_SUCCESS) return s;
    gmu_write(d, cm3_boot, 2);
    s = AdrenoHfiInitialize(&g->hfi, (void *)d->gmu.words, d->gmu.bytes, g->memory.hfi.cpu,
                            g->memory.hfi.bytes, g->memory.hfi.iova, p->notify, p->context);
    if (s != STATUS_SUCCESS) return s;
    gmu_write(d, fence_range, 0x80c808c0);
    gmu_write(d, cm3_config, 0x4052);
    gmu_write(d, chip_id, 0x08000100);
    gmu_write(d, log_address, (ULONG)g->memory.log.iova | ((g->memory.log.bytes >> 12) - 1));
    gmu_write(d, log_write, 0); /* A fresh log was allocated for this cold upload. */
    for (unsigned i = 0; i < 4; ++i)
        WRITE_REGISTER_ULONG(d->gpu.words + qsb_side0 + i, 0x71e20);
    WRITE_REGISTER_ULONG(d->gpu.words + gbif_config, 0x20023000);
    gmu_write(d, gmu_gbif_qos, 0x33);
    gmu_write(d, gmu_sys_bus, 1);
    gmu_write(d, gmu_icache, 1);
    gmu_write(d, gmu_dcache, 1);

    /* HFI is polled during boot. Leave incoming interrupts masked until
     * the renderer installs its own ISR; clear old status before releasing CM3. */
    gmu_write(d, intr_mask, MAXULONG);
    gmu_write(d, ao_intr_mask, MAXULONG);
    gmu_write(d, intr_clear, MAXULONG);
    gmu_write(d, ao_intr_clear, MAXULONG);
    g->rsc_awake = g->reset_released = g->gbif_pending = TRUE;
    KeMemoryBarrier();
    gmu_write(d, cm3_reset, 0);
    s = wait_for_boot(d);
    if (s == STATUS_SUCCESS) s = AdrenoHfiStart(&g->hfi);
    if (s == STATUS_SUCCESS) s = AdrenoHfiSetPerfTable(&g->hfi, &p->perf, 1000);
    if (s == STATUS_SUCCESS)
        s = AdrenoHfiRequest(&g->hfi, hfi_bw, bw.words, RTL_NUMBER_OF(bw.words), 1000);
    const uint32_t handle = 0;
    if (s == STATUS_SUCCESS) s = AdrenoHfiRequest(&g->hfi, hfi_core, &handle, 1, 1000);
    const uint32_t vote[] = {1 /* blocking */, g->initial_level.gpu_index, g->initial_level.bw_vote};
    if (s == STATUS_SUCCESS) s = AdrenoHfiRequest(&g->hfi, hfi_gx_vote, vote, RTL_NUMBER_OF(vote), 1000);
    if (s == STATUS_SUCCESS) s = AdrenoHoldGx(d);
    if (s == STATUS_SUCCESS) s = AdrenoHfiRequest(&g->hfi, hfi_start, NULL, 0, 1000);
    if (s == STATUS_SUCCESS) g->hfi_running = TRUE;
    return s;
}

NTSTATUS AdrenoStopGmu(ADRENO_RENDER_DEVICE *d)
{
    PAGED_CODE();
    ADRENO_GMU_DEVICE *g = &d->gmu_state;
    const ADRENO_GMU_PLATFORM *p = d->platform;
    if (!g->rails) return STATUS_SUCCESS;
    NTSTATUS s;
    if (g->wake_pending) {
        s = finish_wake(d);
        if (s != STATUS_SUCCESS) return s;
    }
    AdrenoReleaseGx(d);
    if (g->hfi_running && !g->slumber_requested) {
        const uint32_t slumber[] = {g->initial_level.bw_vote, g->initial_level.gpu_index};
        gmu_write(d, power_counter, 0);
        g->slumber_requested = TRUE;
        s = AdrenoHfiRequest(&g->hfi, hfi_slumber, slumber, RTL_NUMBER_OF(slumber), 1000);
        gmu_write(d, fence_control, 0);
        gmu_write(d, cx_falnext, 1);
        if (s == STATUS_SUCCESS) s = poll(gmu_reg(d, cx_busy), 1u << 30, 0, 100);
        /* A hung CM3 cannot acknowledge slumber. The renderer already
         * stopped CP; reset followed by RSC and both GBIF acknowledgements
         * also retires DMA without trusting the failed firmware. */
        if (s == STATUS_DEVICE_NOT_CONNECTED) return s;
    }
    if (g->reset_released || g->reset_pending) {
        gmu_write(d, intr_mask, MAXULONG);
        gmu_write(d, ao_intr_mask, MAXULONG);
        s = hold_reset(d);
        if (s != STATUS_SUCCESS) return s;
        g->reset_pending = g->reset_released = g->hfi_running = FALSE;
    }
    if (g->rsc_awake) {
        if (!g->rsc_sleeping) {
            gmu_write(d, rsc_req, 1);
            s = poll(d->rsc.words + rsc_status, 1u << 16, 1u << 16, 100);
            if (s != STATUS_SUCCESS) return s;
            gmu_write(d, rsc_req, 0);
            g->rsc_sleeping = TRUE;
        }
        ULONG gx = READ_REGISTER_ULONG(gmu_reg(d, gx_power));
        if (gx == MAXULONG) return STATUS_DEVICE_NOT_CONNECTED;
        if (!(gx & 3)) {
            if (!p->PowerOffGx) return STATUS_DEVICE_POWER_FAILURE;
            s = p->PowerOffGx(p->context);
            if (s != STATUS_SUCCESS) return s;
            gx = READ_REGISTER_ULONG(gmu_reg(d, gx_power));
            if (gx == MAXULONG) return STATUS_DEVICE_NOT_CONNECTED;
            if (!(gx & 3)) return STATUS_DEVICE_POWER_FAILURE;
        }
        g->rsc_awake = FALSE;
    }
    if (g->gbif_pending) {
        s = drain_gbif(d);
        if (s != STATUS_SUCCESS) return s;
        g->gbif_pending = FALSE;
    }
    s = AdrenoGmuMemoryClose(&g->memory);
    if (s != STATUS_SUCCESS) return s;
    RtlZeroMemory(&g->hfi, sizeof(g->hfi));
    g->slumber_requested = FALSE;
    if (g->clock_ref.controller) {
        s = AdrenoSetGpuClocks(p->clocks, &g->clock_ref, AdrenoGmuClocks, FALSE);
        if (s != STATUS_SUCCESS) return s;
    }
    s = p->SetRailsAndBus(p->context, FALSE);
    if (s == STATUS_SUCCESS) g->rails = FALSE;
    return s;
}
