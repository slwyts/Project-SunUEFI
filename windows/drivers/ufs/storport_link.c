/* SPDX-License-Identifier: BSD-2-Clause */
#include "storport_link.h"
#include <intrin.h>

static uint32_t read_register(void *context, unsigned reg)
{
    UFS_IO_QUEUE *q = context;
    return StorPortReadRegisterUlong(q->extension, (PULONG)(q->registers + reg));
}

static void write_register(void *context, unsigned reg, uint32_t value)
{
    UFS_IO_QUEUE *q = context;
    StorPortWriteRegisterUlong(q->extension, (PULONG)(q->registers + reg), value);
}

static void publish(void *context) { (void)context; __dmb(_ARM64_BARRIER_OSHST); }
static void consume(void *context) { (void)context; __dmb(_ARM64_BARRIER_OSHLD); }

enum ufs_link_error UfsLinkBegin(UFS_IO_QUEUE *q, struct ufs_link *link, ULONGLONG now_us,
                                 struct ufs_media_settings *media, enum ufs_link_operation operation)
{
    if (!q || !link || !q->extension || !q->registers || !q->list || !q->slot_count ||
        q->running || q->owns_list || q->active || q->managing || q->faults || q->resetting) return UFS_LINK_OWNED;
    q->device_ready = FALSE;
    struct ufs_link_io io = {
        .context = q, .read = read_register, .write = write_register, .publish = publish, .consume = consume,
        .list = q->list, .ucd = (struct ufs_management_ucd *)q->slot[0].ucd,
        .list_dma = (ULONGLONG)q->list_dma.QuadPart, .ucd_dma = (ULONGLONG)q->slot[0].dma.QuadPart
    };
    enum ufs_link_error error = operation == UFS_DISCOVER ? ufs_link_begin(link, &io, now_us)
                              : ufs_link_media(link, &io, media, q->enabled_luns, operation, now_us);
    if (error == UFS_LINK_OK) {
        link->media = media;
        q->managing = TRUE;
    }
    return error;
}

enum ufs_link_phase UfsLinkPoll(struct ufs_link *link, ULONGLONG now_us)
{
    return ufs_link_poll(link, now_us);
}

BOOLEAN UfsLinkStop(UFS_IO_QUEUE *q, struct ufs_link *link)
{
    if (!q || !link || link->io.context != q || !q->managing) return FALSE;
    if (!ufs_link_stop(link)) return FALSE;
    q->managing = FALSE;
    q->device_ready = (link->phase == UFS_LINK_READY || link->phase == UFS_LINK_KEEP_POWER);
    q->enabled_luns = q->device_ready || link->phase == UFS_LINK_ASLEEP ? link->enabled_luns : 0;
    return TRUE;
}

BOOLEAN UfsLinkReset(UFS_IO_QUEUE *q, struct ufs_link *link)
{
    if (!q->managing || link->io.context != q || !ufs_link_reset(link)) return FALSE;
    q->managing = q->device_ready = FALSE;
    q->resetting = TRUE; /* UfsIoReset can now finish the same acknowledged hardware reset. */
    q->enabled_luns = 0;
    return TRUE;
}
