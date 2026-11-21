/* SPDX-License-Identifier: BSD-2-Clause */
#include "link.h"

/* Compile the adapter against the driver's structs. Only the UFSHCI wire
 * buffers cross from this process to the independent controller model.
 */
static struct ufs_link link;

size_t management_ucd_bytes(void) { return sizeof(struct ufs_management_ucd); }

int begin_link(struct ufs_utrd *list, struct ufs_management_ucd *ucd, uint64_t list_dma, uint64_t ucd_dma,
               uint32_t (*read)(void *, unsigned), void (*write)(void *, unsigned, uint32_t),
               void (*publish)(void *), void (*consume)(void *))
{
    struct ufs_link_io io = {
        .read = read, .write = write, .publish = publish, .consume = consume,
        .list = list, .ucd = ucd, .list_dma = list_dma, .ucd_dma = ucd_dma
    };
    return ufs_link_begin(&link, &io, 0);
}

int poll_link(uint64_t now_us, uint32_t *luns)
{
    switch (ufs_link_poll(&link, now_us)) {
    case UFS_LINK_READY:
        *luns = link.enabled_luns;
        return 1;
    case UFS_LINK_FAILED:
        return -(int)link.error;
    default:
        return 0;
    }
}

int stop_link(void) { return ufs_link_stop(&link); }

int build_request(struct ufs_utrd *utrd, struct ufs_upius *ucd, uint64_t ucd_dma,
                  uint8_t lun, const uint8_t *cdb, size_t cdb_bytes, enum ufs_direction direction,
                  const uint64_t *pages, const uint32_t *sizes, unsigned count)
{
    struct ufs_transfer request;
    uint32_t length = 0;
    for (unsigned i = 0; i < count; ++i)
        length += sizes[i];
    if (!ufs_transfer_init(&request, utrd, ucd, (struct ufs_prd *)(ucd + 1), 2,
                           ucd_dma, UINT32_MAX, lun, 0, UFS_SIMPLE, direction, cdb, cdb_bytes, length))
        return 0;
    for (unsigned i = 0; i < count; ++i)
        if (!ufs_transfer_add_dma(&request, pages[i], sizes[i]))
            return 0;
    return ufs_transfer_ready(&request);
}

int read_completion(const struct ufs_utrd *utrd, const struct ufs_upius *ucd,
                    uint32_t *bytes, uint8_t *scsi, uint8_t *ocs)
{
    struct ufs_completion completed = ufs_transfer_complete(utrd, ucd);
    *bytes = completed.bytes;
    *scsi = completed.scsi_status;
    *ocs = completed.ocs;
    return completed.result;
}
