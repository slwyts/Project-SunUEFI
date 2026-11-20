/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Gen8 HFI uses 17 queue descriptors, followed by 16-byte-aligned rings.
 * The host command interface enables the command, response and debug rings.
 */
#define ADRENO_HFI_SIZE 13136u
#define ADRENO_HFI_MAX_WORDS 255u

struct adreno_hfi {
    volatile uint32_t *memory;
    uint32_t sequence;
};

enum adreno_hfi_status {
    ADRENO_HFI_OK,
    ADRENO_HFI_EMPTY,
    ADRENO_HFI_FULL,
    ADRENO_HFI_INVALID,
    ADRENO_HFI_SHORT_BUFFER,
    ADRENO_HFI_BAD_PACKET,
    ADRENO_HFI_UNMATCHED_ACK,
    ADRENO_HFI_FIRMWARE_FAULT,
    ADRENO_HFI_NOTIFICATION
};

enum adreno_hfi_inbox {
    ADRENO_HFI_RESPONSE = 1,
    ADRENO_HFI_DEBUG = 2
};

/* Format only while the GMU is held in reset. The CPU and GMU mappings must
 * address the same coherent or uncached DMA buffer, not a CPU physical address.
 */
enum adreno_hfi_status adreno_hfi_init(struct adreno_hfi *hfi, void *cpu_mapping,
                                       size_t bytes, uint64_t gmu_iova);

/* Callers serialize the command producer, and each inbox has one reader.
 * A successful command is visible in shared memory before the caller signals
 * the GMU. The returned token is the entire echoed command header.
 */
enum adreno_hfi_status adreno_hfi_send(struct adreno_hfi *hfi, uint8_t id,
                                       const uint32_t *payload, size_t words,
                                       uint32_t *token);

/* Return an entire packet, including its header. A short destination leaves
 * the packet queued and receives the required word count.
 */
enum adreno_hfi_status adreno_hfi_receive(struct adreno_hfi *hfi, enum adreno_hfi_inbox inbox,
                                          uint32_t *packet, size_t capacity, size_t *words);

/* The first ACK result is command-specific: it is a status for a power vote,
 * but may be a nonzero value for a query. Return it without interpreting it.
 * A separate firmware fault message carries its own error code in result.
 */
enum adreno_hfi_status adreno_hfi_reply(uint32_t token, const uint32_t *packet, size_t count,
                                       uint32_t *result, uint32_t *data, size_t capacity,
                                       size_t *words);
