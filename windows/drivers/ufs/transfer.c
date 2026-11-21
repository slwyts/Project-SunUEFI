/* SPDX-License-Identifier: BSD-2-Clause */
/* JEDEC UFS command/response UPIUs are big endian; UFSHCI UTRDs and PRDs
 * are little endian. Describe both explicitly rather than using bitfields.
 */
#include "transfer.h"
#include <string.h>

enum { PRD_MAX_BYTES = 256 * 1024, RSP_SENSE_LEN = 32, RSP_SENSE = 34 };

static void le16(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
}

static void le32(uint8_t *p, uint32_t n)
{
    le16(p, (uint16_t)n); le16(p + 2, (uint16_t)(n >> 16));
}

static void le64(uint8_t *p, uint64_t n)
{
    le32(p, (uint32_t)n); le32(p + 4, (uint32_t)(n >> 32));
}

static void be32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
}

static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t get_be32(const uint8_t *p)
{
    return (uint32_t)get_be16(p) << 16 | get_be16(p + 2);
}

static int dma_fits(uint64_t address, uint64_t bytes, uint64_t limit)
{
    return bytes && address <= limit && bytes - 1 <= limit - address;
}

int ufs_transfer_init(struct ufs_transfer *x, struct ufs_utrd *utrd, struct ufs_upius *upius,
                      struct ufs_prd *prds, uint16_t capacity, uint64_t ucd_dma, uint64_t dma_limit,
                      uint8_t lun, uint8_t tag, enum ufs_attribute attr, enum ufs_direction direction,
                      const uint8_t *cdb, size_t cdb_bytes, uint32_t requested)
{
    if (!x || !utrd || !upius || (capacity && !prds) || !cdb || !cdb_bytes || cdb_bytes > UFS_CDB_SIZE ||
        (unsigned)direction > UFS_DATA_IN || (unsigned)attr > UFS_ACA || (requested && !capacity) ||
        (requested != 0) != (direction != UFS_NO_DATA) || ucd_dma % UFS_UCD_ALIGNMENT ||
        !dma_fits(ucd_dma, sizeof(*upius) + (uint64_t)capacity * sizeof(*prds), dma_limit))
        return 0;
    *x = (struct ufs_transfer){ .utrd = utrd, .upius = upius, .prds = prds, .dma_limit = dma_limit,
                               .requested = requested, .prd_capacity = capacity };
    memset(utrd, 0, sizeof(*utrd));
    memset(upius, 0, sizeof(*upius));
    utrd->data[3] = (uint8_t)(0x11 | (unsigned)direction << 1); /* storage command, completion IRQ */
    utrd->data[8] = 0xf; /* HCI replaces the invalid OCS after completion. */
    le64(utrd->data + 16, ucd_dma);
    le16(utrd->data + 24, UFS_UPIU_SPACE / 4);
    le16(utrd->data + 26, offsetof(struct ufs_upius, response) / 4);
    le16(utrd->data + 30, sizeof(*upius) / 4);
    uint8_t *cmd = upius->command;
    cmd[0] = 0x01;
    cmd[1] = (uint8_t)((direction == UFS_DATA_IN ? 0x40 : direction == UFS_DATA_OUT ? 0x20 : 0) | (unsigned)attr);
    cmd[2] = lun;
    cmd[3] = tag;
    be32(cmd + 12, requested);
    memcpy(cmd + 16, cdb, cdb_bytes);
    return 1;
}

int ufs_transfer_add_dma(struct ufs_transfer *x, uint64_t address, uint32_t bytes)
{
    uint64_t padded = ((uint64_t)x->requested + 3) & ~UINT64_C(3);
    if ((address | bytes) & 3 || bytes > padded - x->mapped || !dma_fits(address, bytes, x->dma_limit))
        return 0;
    uint32_t entries = (bytes - 1) / PRD_MAX_BYTES + 1;
    if (entries > (uint32_t)(x->prd_capacity - x->prd_count))
        return 0;
    x->mapped += bytes;
    do {
        uint32_t chunk = bytes < PRD_MAX_BYTES ? bytes : PRD_MAX_BYTES;
        uint8_t *prd = x->prds[x->prd_count++].data;
        le64(prd, address);
        le32(prd + 8, 0);
        le32(prd + 12, chunk - 1);
        bytes -= chunk;
        address += chunk;
    } while (bytes);
    le16(x->utrd->data + 28, x->prd_count);
    return 1;
}

int ufs_transfer_ready(const struct ufs_transfer *x)
{
    return (((uint64_t)x->requested + 3) & ~UINT64_C(3)) == x->mapped;
}

struct ufs_completion ufs_transfer_complete(const struct ufs_utrd *utrd, const struct ufs_upius *upius)
{
    const uint8_t *rsp = upius->response;
    struct ufs_completion c = { .result = UFS_RESULT_TRANSPORT, .ocs = utrd->data[8] & 0xf };
    if (c.ocs == 6) c.result = UFS_RESULT_ABORTED;
    /* TARGET FAILURE can carry a valid CHECK CONDITION and autosense. */
    if (c.ocs || (rsp[0] & 0x3f) != 0x21 || rsp[2] != upius->command[2] || rsp[3] != upius->command[3] ||
        (rsp[6] && (rsp[6] != 1 || rsp[7] != 2)) || rsp[8])
        return c;
    uint32_t requested = get_be32(upius->command + 12), residual = get_be32(rsp + 12);
    if (rsp[1] & 0x40 || residual > requested) {
        c.result = UFS_RESULT_LENGTH;
        return c;
    }
    uint16_t segment = get_be16(rsp + 10);
    if (segment) {
        if (segment < 2 || segment > UFS_UPIU_SPACE - RSP_SENSE_LEN) return c;
        c.sense_bytes = get_be16(rsp + RSP_SENSE_LEN);
        if (c.sense_bytes > segment - 2) {
            c.sense_bytes = 0;
            return c;
        }
        c.sense = rsp + RSP_SENSE;
    }
    c.scsi_status = rsp[7];
    c.bytes = requested - residual; /* Some devices return a residual without the underflow flag. */
    c.result = c.scsi_status ? UFS_RESULT_SCSI : residual ? UFS_RESULT_LENGTH : UFS_RESULT_GOOD;
    return c;
}
