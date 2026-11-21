/* SPDX-License-Identifier: BSD-2-Clause */
#include "transfer.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    struct ufs_utrd trd;
    struct ufs_upius upiu;
    struct ufs_prd prd[3];
    struct ufs_transfer x;
    const uint64_t limit = (UINT64_C(1) << 36) - 1, ucd = UINT64_C(0x900000000);
    const uint8_t cdb[16] = { 0x88, 0x08, 0, 0, 0, 1, 2, 3, 4, 5, 0, 0, 0, 0x41 };
    memset(&upiu, 0x5a, sizeof(upiu));
    assert(ufs_transfer_init(&x, &trd, &upiu, prd, 3, ucd, limit, 4, 11, UFS_ORDERED, UFS_DATA_IN,
                             cdb, sizeof(cdb), 0x41000));
    /* SCSI READ(16) FUA: CDB unchanged, READ and ordered tag set independently.
     * Expected wire bytes do not depend on host struct packing or endianness.
     */
    const uint8_t wanted_hdr[16] = { 1, 0x41, 4, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0x10, 0 };
    assert(!memcmp(upiu.command, wanted_hdr, sizeof(wanted_hdr)));
    assert(!memcmp(upiu.command + 16, cdb, sizeof(cdb)));
    assert(trd.data[3] == 0x15 && trd.data[8] == 0x0f && trd.data[20] == 9 &&
           trd.data[24] == 32 && trd.data[26] == 32 && trd.data[30] == 64);
    assert(!ufs_transfer_ready(&x));
    assert(ufs_transfer_add_dma(&x, UINT64_C(0xfffc00000), 0x40004));
    assert(ufs_transfer_add_dma(&x, UINT64_C(0x100000), 0xffc));
    assert(ufs_transfer_ready(&x) && trd.data[28] == 3);
    const uint8_t first[16] = {0, 0, 0xc0, 0xff, 0xf, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 3, 0};
    const uint8_t second[16] = {0, 0, 0xc4, 0xff, 0xf, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0};
    assert(!memcmp(prd[0].data, first, 16) && !memcmp(prd[1].data, second, 16));
    assert(!ufs_transfer_add_dma(&x, 0x2000, 4));
    assert(ufs_transfer_complete(&trd, &upiu).result == UFS_RESULT_TRANSPORT);
    trd.data[8] = 0;
    uint8_t *r = upiu.response;
    r[0] = 0x21; r[2] = 4; r[3] = 11;
    struct ufs_completion c = ufs_transfer_complete(&trd, &upiu);
    assert(c.result == UFS_RESULT_GOOD && c.bytes == 0x41000);
    r[15] = 8; /* A real-world residual may have no underflow flag. */
    c = ufs_transfer_complete(&trd, &upiu);
    assert(c.result == UFS_RESULT_LENGTH && c.bytes == 0x40ff8);
    r[6] = 1; r[7] = 2; r[11] = 20; r[33] = 18; r[34] = 0x70; r[36] = 0x06;
    c = ufs_transfer_complete(&trd, &upiu);
    assert(c.result == UFS_RESULT_SCSI && c.scsi_status == 2 && c.sense_bytes == 18 && c.sense[2] == 6);
    r[33] = 19;
    assert(ufs_transfer_complete(&trd, &upiu).result == UFS_RESULT_TRANSPORT);
    r[33] = 18; r[3] = 12;
    assert(ufs_transfer_complete(&trd, &upiu).result == UFS_RESULT_TRANSPORT);
    r[3] = 11; r[1] = 0x40;
    assert(ufs_transfer_complete(&trd, &upiu).result == UFS_RESULT_LENGTH);
    trd.data[8] = 6;
    assert(ufs_transfer_complete(&trd, &upiu).result == UFS_RESULT_ABORTED);
    const uint8_t flush[10] = {0x35};
    assert(ufs_transfer_init(&x, &trd, &upiu, NULL, 0, ucd, limit, 0, 0, UFS_SIMPLE, UFS_NO_DATA,
                             flush, sizeof(flush), 0));
    assert(ufs_transfer_ready(&x) && trd.data[3] == 0x11 && !trd.data[28] && upiu.command[16] == 0x35);
    assert(!ufs_transfer_init(&x, &trd, &upiu, prd, 1, ucd + 4, limit, 0, 0, UFS_SIMPLE, UFS_DATA_OUT,
                              cdb, sizeof(cdb), 0x40004));
    assert(ufs_transfer_init(&x, &trd, &upiu, prd, 1, ucd, limit, 0, 0, UFS_SIMPLE, UFS_DATA_OUT,
                             cdb, sizeof(cdb), 0x40004));
    assert(trd.data[3] == 0x13 && upiu.command[1] == 0x20);
    assert(!ufs_transfer_add_dma(&x, limit - 3, 8)); /* Ends outside the IORT aperture. */
    assert(!ufs_transfer_add_dma(&x, 0x100000, 0x40004)); /* Needs two PRDs, neither published. */
    assert(!ufs_transfer_add_dma(&x, 0x100001, 4) && !ufs_transfer_add_dma(&x, 0x100000, 2));
    assert(!x.mapped && !x.prd_count && !trd.data[28]);
    /* Request and device reply from the Piano EFI trace. REPORT LUNS returned
     * 56 of the 4096 requested bytes; a short response is not a DMA failure.
     */
    const uint8_t report_luns[] = {0xa0, 0, 0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0};
    const uint8_t request[] = {1, 0x40, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10, 0,
                               0xa0, 0, 0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t response[] = {0x21, 0x20, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0f, 0xc8};
    assert(ufs_transfer_init(&x, &trd, &upiu, prd, 1, ucd, limit, 0, 10, UFS_SIMPLE, UFS_DATA_IN,
                             report_luns, sizeof(report_luns), 4096));
    assert(!memcmp(upiu.command, request, sizeof(request)));
    memcpy(upiu.response, response, sizeof(response));
    trd.data[8] = 0;
    c = ufs_transfer_complete(&trd, &upiu);
    assert(c.ocs == 0 && c.scsi_status == 0 && c.result == UFS_RESULT_LENGTH && c.bytes == 56);
    const uint8_t inquiry[] = {0x12, 1, 0x83, 0, 0xff, 0};
    assert(ufs_transfer_init(&x, &trd, &upiu, prd, 2, ucd, limit, 0, 1, UFS_SIMPLE, UFS_DATA_IN,
                             inquiry, sizeof(inquiry), 255));
    /* Last PRD names the miniport's own DWORD, not a rounded-up user buffer. */
    assert(ufs_transfer_add_dma(&x, 0x123400, 252) && !ufs_transfer_ready(&x));
    assert(ufs_transfer_add_dma(&x, 0x357000, 4) && ufs_transfer_ready(&x));
    assert(upiu.command[15] == 255 && prd[1].data[12] == 3);
    return 0;
}
