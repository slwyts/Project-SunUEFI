/* SPDX-License-Identifier: MIT */
#include "hfi.h"

/* Word offsets within the A830 GMU MMIO resource (base register 0x1b000). */
enum {
    cm3_init_result = 0x1f81c - 0x1b000,
    hfi_ctrl_status = 0x1f980 - 0x1b000,
    hfi_qtbl_info = 0x1f984 - 0x1b000,
    hfi_qtbl_addr = 0x1f985 - 0x1b000,
    hfi_ctrl_init = 0x1f986 - 0x1b000,
    gmu2host_intr_clr = 0x1f991 - 0x1b000,
    gmu2host_intr_info = 0x1f992 - 0x1b000,
    host2gmu_intr_set = 0x1f994 - 0x1b000,
    msg_queue_irq = 1u, cm3_fault_irq = 1u << 15,
    cm3_reset_mask = 0xe00, cm3_init_mask = 0x1ff, cm3_booted = 0x100
};

static void wait_for_gmu(void)
{
    LARGE_INTEGER delay;
    delay.QuadPart = -10000; /* 1 ms, without spinning the CPU during startup. */
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

NTSTATUS AdrenoHfiInitialize(ADRENO_HFI *hfi, void *registers, SIZE_T register_bytes,
                            void *memory, SIZE_T bytes, ULONGLONG iova,
                            ADRENO_HFI_NOTIFY notify, void *notify_context)
{
    PAGED_CODE();
    if (!hfi || !registers || ((ULONG_PTR)registers & (sizeof(ULONG) - 1)) ||
        register_bytes < (host2gmu_intr_set + 1) * sizeof(ULONG) ||
        adreno_hfi_init(&hfi->queues, memory, bytes, iova) != ADRENO_HFI_OK)
        return STATUS_INVALID_PARAMETER;
    KeInitializeMutex(&hfi->command_mutex, 0);
    hfi->registers = registers;
    hfi->notify = notify;
    hfi->notify_context = notify_context;
    WRITE_REGISTER_ULONG(hfi->registers + hfi_qtbl_addr, (ULONG)iova);
    WRITE_REGISTER_ULONG(hfi->registers + hfi_qtbl_info, 1);
    return STATUS_SUCCESS;
}

NTSTATUS AdrenoHfiStart(ADRENO_HFI *hfi)
{
    PAGED_CODE();
    if (!hfi || !hfi->registers)
        return STATUS_INVALID_PARAMETER;
    ULONG boot = READ_REGISTER_ULONG(hfi->registers + cm3_init_result);
    if (boot & cm3_reset_mask)
        return STATUS_DEVICE_HARDWARE_ERROR;
    if ((boot & cm3_init_mask) != cm3_booted)
        return STATUS_DEVICE_NOT_READY;
    WRITE_REGISTER_ULONG(hfi->registers + hfi_ctrl_init, 1);
    ULONGLONG start = KeQueryInterruptTime();
    for (;;) {
        if (READ_REGISTER_ULONG(hfi->registers + cm3_init_result) & cm3_reset_mask)
            return STATUS_DEVICE_HARDWARE_ERROR;
        if (READ_REGISTER_ULONG(hfi->registers + hfi_ctrl_status) & 1u)
            return STATUS_SUCCESS;
        if (KeQueryInterruptTime() - start >= 100u * 10000)
            return STATUS_IO_TIMEOUT;
        wait_for_gmu();
    }
}

NTSTATUS AdrenoHfiCommand(ADRENO_HFI *hfi, UCHAR id, const uint32_t *input, SIZE_T input_words,
                          uint32_t *reply, SIZE_T *reply_words, uint32_t *result, ULONG timeout_ms)
{
    PAGED_CODE();
    if (!hfi || !hfi->registers || !reply_words || (*reply_words && !reply) ||
        !result || !timeout_ms)
        return STATUS_INVALID_PARAMETER;
    SIZE_T capacity = *reply_words;
    *reply_words = 0;
    *result = 0;
    LARGE_INTEGER wait;
    wait.QuadPart = -(LONGLONG)timeout_ms * 10000;
    ULONGLONG start = KeQueryInterruptTime();
    NTSTATUS status = KeWaitForSingleObject(&hfi->command_mutex, Executive, KernelMode, FALSE, &wait);
    if (status == STATUS_TIMEOUT)
        return STATUS_IO_TIMEOUT;
    if (status != STATUS_SUCCESS) {
        if (status == STATUS_ABANDONED_WAIT_0)
            KeReleaseMutex(&hfi->command_mutex, FALSE);
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (KeQueryInterruptTime() - start >= (ULONGLONG)timeout_ms * 10000) {
        status = STATUS_IO_TIMEOUT;
        goto unlock;
    }
    if ((READ_REGISTER_ULONG(hfi->registers + cm3_init_result) & cm3_reset_mask) ||
        (READ_REGISTER_ULONG(hfi->registers + gmu2host_intr_info) & cm3_fault_irq)) {
        status = STATUS_DEVICE_HARDWARE_ERROR;
        goto unlock;
    }
    if (!(READ_REGISTER_ULONG(hfi->registers + hfi_ctrl_status) & 1u)) {
        status = STATUS_DEVICE_NOT_READY;
        goto unlock;
    }
    uint32_t token;
    enum adreno_hfi_status queued = adreno_hfi_send(&hfi->queues, id, input, input_words, &token);
    if (queued != ADRENO_HFI_OK) {
        switch (queued) {
        case ADRENO_HFI_FULL: status = STATUS_DEVICE_BUSY; break;
        case ADRENO_HFI_BAD_PACKET: status = STATUS_DEVICE_PROTOCOL_ERROR; break;
        default: status = STATUS_INVALID_PARAMETER; break;
        }
        goto unlock;
    }
    WRITE_REGISTER_ULONG(hfi->registers + host2gmu_intr_set, msg_queue_irq);

    for (;;) {
        ULONG irq = READ_REGISTER_ULONG(hfi->registers + gmu2host_intr_info);
        ULONG boot = READ_REGISTER_ULONG(hfi->registers + cm3_init_result);
        BOOLEAN faulted = (irq & cm3_fault_irq) != 0 || (boot & cm3_reset_mask) != 0;
        if (irq & msg_queue_irq)
            WRITE_REGISTER_ULONG(hfi->registers + gmu2host_intr_clr, msg_queue_irq);
        uint32_t packet[ADRENO_HFI_MAX_WORDS];
        size_t words;
        enum adreno_hfi_status received = adreno_hfi_receive(&hfi->queues, ADRENO_HFI_RESPONSE,
                                                              packet, RTL_NUMBER_OF(packet), &words);
        if (received == ADRENO_HFI_EMPTY) {
            if (faulted) {
                status = STATUS_DEVICE_HARDWARE_ERROR;
                goto unlock;
            }
        } else if (received != ADRENO_HFI_OK) {
            status = STATUS_DEVICE_PROTOCOL_ERROR;
            goto unlock;
        } else {
            enum adreno_hfi_status decoded = adreno_hfi_reply(token, packet, words, result,
                faulted ? NULL : reply, faulted ? 0 : capacity, reply_words);
            if (decoded == ADRENO_HFI_FIRMWARE_FAULT || faulted) {
                if (decoded != ADRENO_HFI_FIRMWARE_FAULT)
                    *result = 0;
                *reply_words = 0;
                status = STATUS_DEVICE_HARDWARE_ERROR;
                goto unlock;
            }
            switch (decoded) {
            case ADRENO_HFI_OK:
                status = STATUS_SUCCESS;
                goto unlock;
            case ADRENO_HFI_SHORT_BUFFER:
                status = STATUS_BUFFER_TOO_SMALL;
                goto unlock;
            case ADRENO_HFI_NOTIFICATION:
                if (!hfi->notify) {
                    *result = packet[0];
                    status = STATUS_NOT_SUPPORTED;
                    goto unlock;
                }
                status = hfi->notify(hfi->notify_context, packet, words);
                if (!NT_SUCCESS(status))
                    goto unlock;
                break;
            case ADRENO_HFI_UNMATCHED_ACK:
                break;
            default:
                status = STATUS_DEVICE_PROTOCOL_ERROR;
                goto unlock;
            }
        }
        if (KeQueryInterruptTime() - start >= (ULONGLONG)timeout_ms * 10000) {
            status = STATUS_IO_TIMEOUT;
            goto unlock;
        }
        if (received == ADRENO_HFI_EMPTY)
            wait_for_gmu();
    }
unlock:
    KeReleaseMutex(&hfi->command_mutex, FALSE);
    return status;
}

NTSTATUS AdrenoHfiRequest(ADRENO_HFI *hfi, UCHAR id, const uint32_t *input, SIZE_T words, ULONG timeout_ms)
{
    SIZE_T returned = 0;
    uint32_t result = 0;
    NTSTATUS s = AdrenoHfiCommand(hfi, id, input, words, NULL, &returned, &result, timeout_ms);
    if (s != STATUS_SUCCESS && !(s == STATUS_BUFFER_TOO_SMALL && returned))
        return s;
    return result ? STATUS_DEVICE_PROTOCOL_ERROR : STATUS_SUCCESS;
}
