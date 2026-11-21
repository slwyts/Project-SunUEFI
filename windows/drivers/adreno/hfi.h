/* SPDX-License-Identifier: MIT */
#pragma once
#include <ntddk.h>
#include "hfi_queue.h"
#include "hfi_perf_table.h"

/* Called under the command mutex for an unsolicited packet on the response
 * ring. The packet is valid only during the call. Do not submit commands
 * recursively; they would compete for the same response ring.
 */
typedef NTSTATUS (*ADRENO_HFI_NOTIFY)(void *context, const uint32_t *packet, SIZE_T words);

typedef struct {
    struct adreno_hfi queues;
    volatile ULONG *registers;
    KMUTEX command_mutex;
    ADRENO_HFI_NOTIFY notify;
    void *notify_context;
} ADRENO_HFI;

/* The miniport owns both mappings through reset and stop. GMU registers are
 * from the translated A830 "gmu" resource, not the whole SoC GPU aperture.
 * The queue buffer must be coherent or uncached, with an established GMU
 * IOMMU mapping. Program the queue table before releasing CM3 reset.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoHfiInitialize(ADRENO_HFI *hfi, void *gmu_registers, SIZE_T register_bytes,
                            void *queue_memory, SIZE_T queue_bytes, ULONGLONG queue_iova,
                            ADRENO_HFI_NOTIFY notify, void *notify_context);

/* Start HFI once the CM3 has booted, before submitting the power tables.
 * The caller owns the interrupt mask and controls power throughout startup.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoHfiStart(ADRENO_HFI *hfi);

/* Submit a synchronous command. STATUS_SUCCESS means the GMU acknowledged it.
 * result and any reply payload have the command's own semantics. reply_words
 * reports the payload size even on STATUS_BUFFER_TOO_SMALL; the command has
 * already executed, so a mutating request must not be blindly retried.
 * On a firmware fault, result holds the reported error code when available.
 * Without a notification handler, an unsolicited request returns
 * STATUS_NOT_SUPPORTED with its header in result.
 * The caller owns the response queue during commands and serializes teardown.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoHfiCommand(ADRENO_HFI *hfi, UCHAR id, const uint32_t *input, SIZE_T input_words,
                          uint32_t *reply, SIZE_T *reply_words, uint32_t *result, ULONG timeout_ms);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AdrenoHfiSetPerfTable(ADRENO_HFI *hfi, const struct adreno_perf_config *config, ULONG timeout_ms);
