/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* UFSHCI command descriptors start on a 128-byte boundary. Response and
 * PRDT offsets in the transfer request descriptor are measured in DWORDs.
 */
enum { UFS_UCD_ALIGNMENT = 128, UFS_UPIU_SPACE = 128, UFS_CDB_SIZE = 16 };
struct ufs_utrd { uint8_t data[32]; };
struct ufs_prd { uint8_t data[16]; };
struct ufs_upius { uint8_t command[UFS_UPIU_SPACE], response[UFS_UPIU_SPACE]; };

enum ufs_direction { UFS_NO_DATA, UFS_DATA_OUT, UFS_DATA_IN };
enum ufs_attribute { UFS_SIMPLE, UFS_ORDERED, UFS_HEAD, UFS_ACA };

/* All addresses here are DMA addresses assigned to this device, not CPU
 * addresses. The caller supplies disjoint descriptor memory for each slot.
 */
struct ufs_transfer {
    struct ufs_utrd *utrd;
    struct ufs_upius *upius;
    struct ufs_prd *prds;
    uint64_t dma_limit;
    uint32_t requested;
    uint64_t mapped;
    uint16_t prd_count, prd_capacity;
};

int ufs_transfer_init(struct ufs_transfer *x, struct ufs_utrd *utrd, struct ufs_upius *upius,
                      struct ufs_prd *prds, uint16_t capacity, uint64_t ucd_dma, uint64_t dma_limit,
                      uint8_t lun, uint8_t tag, enum ufs_attribute attr, enum ufs_direction direction,
                      const uint8_t *cdb, size_t cdb_bytes, uint32_t requested);
int ufs_transfer_add_dma(struct ufs_transfer *x, uint64_t address, uint32_t bytes);
int ufs_transfer_ready(const struct ufs_transfer *x);

enum ufs_result {
    UFS_RESULT_GOOD, UFS_RESULT_SCSI, UFS_RESULT_LENGTH, UFS_RESULT_TRANSPORT, UFS_RESULT_ABORTED
};
struct ufs_completion {
    enum ufs_result result;
    uint32_t bytes;
    uint16_t sense_bytes;
    uint8_t ocs, scsi_status;
    const uint8_t *sense;
};
struct ufs_completion ufs_transfer_complete(const struct ufs_utrd *utrd, const struct ufs_upius *upius);
