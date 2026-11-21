/* SPDX-License-Identifier: BSD-2-Clause */
#include "transfer.h"

/* Compile the adapter against the driver's structs. Only the UFSHCI wire
 * buffers cross from this process to the independent controller model.
 */
int build_request(struct ufs_utrd *utrd, struct ufs_upius *ucd, uint64_t ucd_dma,
                  const uint8_t *cdb, size_t cdb_bytes, enum ufs_direction direction,
                  const uint64_t *pages, const uint32_t *sizes, unsigned count)
{
    struct ufs_transfer request;
    uint32_t length = 0;
    for (unsigned i = 0; i < count; ++i)
        length += sizes[i];
    if (!ufs_transfer_init(&request, utrd, ucd, (struct ufs_prd *)(ucd + 1), 2,
                           ucd_dma, UINT32_MAX, 0, 0, UFS_SIMPLE, direction, cdb, cdb_bytes, length))
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
