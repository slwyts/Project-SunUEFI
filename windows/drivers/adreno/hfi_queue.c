/* SPDX-License-Identifier: MIT */
#include "hfi_queue.h"

#if defined(_MSC_VER) && defined(_M_ARM64)
#include <intrin.h>
/* The GMU is an external DMA master, not a second CPU in the same cluster. */
#define DMA_BARRIER() __dmb(0xf)
#else
#include <stdatomic.h>
#define DMA_BARRIER() atomic_thread_fence(memory_order_seq_cst)
#endif

enum {
    active_queues = 3, max_queues = active_queues + 14, ring_words = 1024,
    ring_mask = ring_words - 1, ack_type = 1, firmware_error_id = 100,
    record_command = 1u << 19
};

struct queue_descriptor {
    uint32_t status, address, type, size, msg_size, reserved[5], read_index, write_index;
};

struct queue_table {
    uint32_t version, size, first, stride, count, active;
    struct queue_descriptor entries[max_queues];
};

enum { ring_offset = ((sizeof(struct queue_table) + 15) & ~(size_t)15) / sizeof(uint32_t) };
_Static_assert(sizeof(struct queue_descriptor) == 12 * sizeof(uint32_t), "HFI descriptor size");
_Static_assert(ADRENO_HFI_SIZE == (ring_offset + active_queues * ring_words) * sizeof(uint32_t),
               "HFI allocation size");

static unsigned int round_packet(unsigned int words)
{
    return (words + 3u) & ~3u;
}

enum adreno_hfi_status adreno_hfi_init(struct adreno_hfi *hfi, void *mapping,
                                       size_t bytes, uint64_t iova)
{
    if (!hfi || !mapping || ((uintptr_t)mapping & 3u) ||
        bytes < ADRENO_HFI_SIZE || (iova & 15u) ||
        iova > UINT32_MAX - (ADRENO_HFI_SIZE - 1))
        return ADRENO_HFI_INVALID;

    /* Gen8 measures both the table and its descriptors in 32-bit words. The
     * unused dispatch descriptors are reserved even in host scheduling mode.
     */
    volatile uint32_t *base = mapping;
    for (size_t i = 0; i < ring_offset; ++i)
        base[i] = 0;
    volatile struct queue_table *table = mapping;
    table->size = sizeof(*table) / sizeof(uint32_t);
    table->first = offsetof(struct queue_table, entries) / sizeof(uint32_t);
    table->stride = sizeof(table->entries[0]) / sizeof(uint32_t);
    table->count = table->active = active_queues;
    for (unsigned int id = 0; id < active_queues; ++id) {
        volatile struct queue_descriptor *q = table->entries + id;
        q->status = 1;
        q->address = (uint32_t)(iova + (ring_offset + id * ring_words) * sizeof(uint32_t));
        q->type = ((id == ADRENO_HFI_DEBUG ? 40u : 10u) << 8) | id;
        q->size = ring_words;
    }
    DMA_BARRIER();
    hfi->memory = base;
    hfi->sequence = 0;
    return ADRENO_HFI_OK;
}

static int valid_cursor(uint32_t cursor)
{
    return cursor < ring_words && !(cursor & 3u);
}

enum adreno_hfi_status adreno_hfi_send(struct adreno_hfi *hfi, uint8_t id,
                                       const uint32_t *payload, size_t words,
                                       uint32_t *token)
{
    if (!hfi || !hfi->memory || !token || (words && !payload) || words >= ADRENO_HFI_MAX_WORDS)
        return ADRENO_HFI_INVALID;

    volatile struct queue_table *table = (volatile struct queue_table *)hfi->memory;
    volatile struct queue_descriptor *q = table->entries;
    volatile uint32_t *ring = hfi->memory + ring_offset;
    uint32_t read = q->read_index, write = q->write_index;
    if (!valid_cursor(read) || !valid_cursor(write))
        return ADRENO_HFI_BAD_PACKET;
    DMA_BARRIER();
    uint32_t count = (uint32_t)words + 1;
    uint32_t occupy = round_packet(count);
    /* A gap is needed to distinguish a full ring from an empty ring. */
    if (occupy > ((read - write - 1u) & ring_mask))
        return ADRENO_HFI_FULL;

    uint32_t seq = (hfi->sequence + 1u) & 0xfffu;
    uint32_t header = (seq << 20) | (count << 8) | id;
    ring[write] = header;
    for (uint32_t i = 1; i < count; ++i)
        ring[(write + i) & ring_mask] = payload[i - 1];
    for (uint32_t i = count; i < occupy; ++i)
        ring[(write + i) & ring_mask] = UINT32_C(0xfafafafa);
    DMA_BARRIER();
    q->write_index = (write + occupy) & ring_mask;
    DMA_BARRIER();
    hfi->sequence = seq;
    *token = header;
    return ADRENO_HFI_OK;
}

enum adreno_hfi_status adreno_hfi_receive(struct adreno_hfi *hfi, enum adreno_hfi_inbox inbox,
                                          uint32_t *packet, size_t capacity, size_t *words)
{
    if (!hfi || !hfi->memory || !words || (capacity && !packet) ||
        (inbox != ADRENO_HFI_RESPONSE && inbox != ADRENO_HFI_DEBUG))
        return ADRENO_HFI_INVALID;
    *words = 0;
    volatile struct queue_table *table = (volatile struct queue_table *)hfi->memory;
    volatile struct queue_descriptor *q = table->entries + inbox;
    volatile uint32_t *ring = hfi->memory + ring_offset + (unsigned int)inbox * ring_words;
    uint32_t read = q->read_index, write = q->write_index;
    if (!valid_cursor(read) || !valid_cursor(write))
        return ADRENO_HFI_BAD_PACKET;
    if (read == write)
        return ADRENO_HFI_EMPTY;
    DMA_BARRIER();
    uint32_t header = ring[read];
    uint32_t count = (header >> 8) & 0xffu;
    uint32_t occupy = round_packet(count);
    if (!count || occupy > ((write - read) & ring_mask))
        return ADRENO_HFI_BAD_PACKET;
    *words = count;
    if (count > capacity)
        return ADRENO_HFI_SHORT_BUFFER;
    packet[0] = header;
    for (uint32_t i = 1; i < count; ++i)
        packet[i] = ring[(read + i) & ring_mask];
    DMA_BARRIER();
    q->read_index = (read + occupy) & ring_mask;
    return ADRENO_HFI_OK;
}

enum adreno_hfi_status adreno_hfi_reply(uint32_t token, const uint32_t *packet, size_t count,
                                       uint32_t *result, uint32_t *data, size_t capacity,
                                       size_t *words)
{
    if (!packet || !result || !words || (capacity && !data))
        return ADRENO_HFI_INVALID;
    *result = 0;
    *words = 0;
    if (!count || count != ((packet[0] >> 8) & 0xffu))
        return ADRENO_HFI_BAD_PACKET;
    if (((packet[0] >> 16) & 0xfu) != ack_type) {
        if ((packet[0] & 0xffu) != firmware_error_id)
            return ADRENO_HFI_NOTIFICATION;
        if (count < 2)
            return ADRENO_HFI_BAD_PACKET;
        *result = packet[1];
        return ADRENO_HFI_FIRMWARE_FAULT;
    }
    if (count < 3)
        return ADRENO_HFI_BAD_PACKET;
    /* Firmware can omit its warm-boot record flag when echoing a command. */
    if ((packet[1] ^ token) & ~(uint32_t)record_command)
        return ADRENO_HFI_UNMATCHED_ACK;
    *result = packet[2];
    *words = count - 3;
    if (*words > capacity)
        return ADRENO_HFI_SHORT_BUFFER;
    for (size_t i = 0; i < *words; ++i)
        data[i] = packet[3 + i];
    return ADRENO_HFI_OK;
}
