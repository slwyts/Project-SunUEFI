/* SPDX-License-Identifier: BSD-2-Clause */
#include "link.h"
#include <string.h>

enum {
    CAP = 0, VER = 8, IS = 0x20, IE = 0x24, HCS = 0x30, HCE = 0x34,
    UTRLBA = 0x50, UTRLBAU = 0x54, UTRLDBR = 0x58, UTRLCLR = 0x5c, UTRLRSR = 0x60,
    UTMRLDBR = 0x78, UTMRLRSR = 0x80, UICCMD = 0x90, UICARG1 = 0x94, UICARG2 = 0x98,
    UICARG3 = 0x9c, MEM_CFG = 0x300, UTR_COMPLETE = 1, UIC_COMPLETE = 0x400,
    LINK_FATAL = 0x80 | 0x800 | 0x1000 | 0x10000 | 0x20000 | 0x40000,
    UIC_TIMEOUT_US = 500000, DEVICE_INIT_US = 1500000, QUERY_TIMEOUT_US = 1500000,
    POLL_INIT_US = 1000, NOP_TIMEOUT_US = 50000, REPORT_LUNS_WLUN = 0x81
};

static uint32_t read_reg(const struct ufs_link *l, unsigned reg) { return l->io.read(l->io.context, reg); }
static void write_reg(const struct ufs_link *l, unsigned reg, uint32_t v) { l->io.write(l->io.context, reg, v); }

static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static void le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

int ufs_scsi_lun(uint32_t enabled, uint8_t requested, const uint8_t *cdb, size_t bytes, uint8_t *wire_lun)
{
    if (!wire_lun || !cdb || !bytes || requested >= UFS_MAX_UNIT_NUMBERS) return 0;
    if (cdb[0] == 0xa0) {
        if (bytes != 12) return 0;
        *wire_lun = REPORT_LUNS_WLUN;
        return 1;
    }
    if (!(enabled & (1u << requested))) return 0;
    *wire_lun = requested;
    return 1;
}

static enum ufs_link_phase fail(struct ufs_link *l, enum ufs_link_error e)
{
    l->error = e;
    l->phase = UFS_LINK_FAILED;
    return l->phase;
}

enum ufs_link_error ufs_link_begin(struct ufs_link *l, const struct ufs_link_io *io, uint64_t now)
{
    if (!l || !io || !io->read || !io->write || !io->publish || !io->consume || !io->list || !io->ucd ||
        io->list_dma % 1024 || io->ucd_dma % UFS_UCD_ALIGNMENT ||
        io->ucd_dma > UINT64_MAX - (sizeof(*io->ucd) - 1))
        return UFS_LINK_RESOURCES;
    if (l->owns_controller) return UFS_LINK_OWNED;
    *l = (struct ufs_link){ .io = *io, .since = now };
    uint32_t cap = read_reg(l, CAP), ver = read_reg(l, VER);
    if (cap == UINT32_MAX || ver == UINT32_MAX || ver < 0x200 ||
        (ver >= 0x400 && ((cap & (1u << 29)) || (read_reg(l, MEM_CFG) & 1)))) {
        fail(l, UFS_LINK_CONTROLLER);
        return l->error;
    }
    if (read_reg(l, UTRLDBR) || read_reg(l, UTRLRSR) || read_reg(l, UTMRLDBR) || read_reg(l, UTMRLRSR)) {
        fail(l, UFS_LINK_OWNED);
        return l->error;
    }
    l->cold = !(read_reg(l, HCE) & 1);
    l->previous_ie = read_reg(l, IE);
    l->owns_controller = 1;
    write_reg(l, IE, l->previous_ie & ~(uint32_t)(UTR_COMPLETE | UIC_COMPLETE));
    if (l->cold) write_reg(l, HCE, 1);
    l->phase = UFS_LINK_WAIT_HCE;
    return UFS_LINK_OK;
}

static int open_list(struct ufs_link *l, uint64_t now)
{
    if ((read_reg(l, HCS) & 3) != 3) return 0;
    write_reg(l, UTRLBA, (uint32_t)l->io.list_dma);
    write_reg(l, UTRLBAU, (uint32_t)(l->io.list_dma >> 32));
    write_reg(l, IS, UTR_COMPLETE);
    l->owns_list = 1;
    write_reg(l, UTRLRSR, 1);
    if (!(read_reg(l, UTRLRSR) & 1)) { fail(l, UFS_LINK_CONTROLLER); return 0; }
    l->phase = UFS_LINK_SEND;
    l->since = now;
    return 1;
}

static void submit(struct ufs_link *l, uint64_t now)
{
    struct ufs_management_ucd *u = l->io.ucd;
    uint8_t *d = l->io.list[0].data, *r = u->request;
    if (read_reg(l, UTRLDBR) || !(read_reg(l, UTRLRSR) & 1)) { fail(l, UFS_LINK_OWNED); return; }
    memset(u, 0, sizeof(*u));
    memset(d, 0, sizeof(struct ufs_utrd));
    d[3] = 0x11; d[8] = 0xf;
    for (unsigned i = 0; i != 8; ++i) d[16 + i] = (uint8_t)(l->io.ucd_dma >> (8 * i));
    le16(d + 24, sizeof(u->response) / 4);
    le16(d + 26, offsetof(struct ufs_management_ucd, response) / 4);
    /* No PRD: NOP has no payload and READ_DESCRIPTOR returns data inline. */
    if (l->command == UFS_MGMT_REPORT_LUNS) {
        /* Ask the REPORT LUNS W-LU about normal LUs (SELECT REPORT = 0).
         * bNumberLU is a count, not the highest configured unit number.
         */
        uint8_t cdb[12] = {0xa0};
        unsigned bytes = sizeof(u->lun_list);
        cdb[8] = (uint8_t)(bytes >> 8); cdb[9] = (uint8_t)bytes;
        struct ufs_transfer x;
        if (!ufs_transfer_init(&x, l->io.list, &u->scsi, &u->report_prd, 1, l->io.ucd_dma, UINT64_MAX,
                               REPORT_LUNS_WLUN, 0, UFS_SIMPLE, UFS_DATA_IN, cdb, sizeof(cdb), bytes) ||
            !ufs_transfer_add_dma(&x, l->io.ucd_dma + offsetof(struct ufs_management_ucd, lun_list), bytes) ||
            !ufs_transfer_ready(&x)) { fail(l, UFS_LINK_RESOURCES); return; }
        le16(d + 30, offsetof(struct ufs_management_ucd, report_prd) / 4);
    } else if (l->command == UFS_MGMT_NOP) {
        ++l->nop_attempts;
    } else {
        r[0] = 0x16;
        r[5] = l->command == UFS_MGMT_SET_INIT ? 0x81 : 0x01;
        if (l->command == UFS_MGMT_SET_INIT || l->command == UFS_MGMT_READ_INIT) {
            r[12] = l->command == UFS_MGMT_SET_INIT ? 6 : 5;
            r[13] = 1; /* fDeviceInit */
        } else {
            r[12] = 1; /* READ_DESCRIPTOR */
            r[13] = l->command == UFS_MGMT_DEVICE ? 0 : l->command == UFS_MGMT_GEOMETRY ? 7 : 2;
            r[14] = l->command == UFS_MGMT_UNIT ? l->unit : 0;
            r[19] = UFS_DESCRIPTOR_MAX;
        }
    }
    write_reg(l, IS, UTR_COMPLETE);
    l->io.publish(l->io.context);
    l->transfer_inflight = 1;
    write_reg(l, UTRLDBR, 1);
    l->since = now;
    l->phase = UFS_LINK_WAIT_REPLY;
}

static enum ufs_link_phase next_unit(struct ufs_link *l)
{
    uint32_t remaining = l->reported_luns & ~l->enabled_luns;
    if (!remaining) return l->phase = UFS_LINK_READY;
    for (l->unit = 0; !(remaining & (UINT32_C(1) << l->unit)); ++l->unit) {}
    l->command = UFS_MGMT_UNIT;
    return l->phase = UFS_LINK_SEND;
}

static enum ufs_link_phase report_luns(struct ufs_link *l, uint64_t now)
{
    struct ufs_completion c = ufs_transfer_complete(l->io.list, &l->io.ucd->scsi);
    if (c.result == UFS_RESULT_SCSI && c.scsi_status == 2 && c.sense && c.sense_bytes >= 3) {
        unsigned format = c.sense[0] & 0x7f;
        unsigned key = format == 0x70 || format == 0x71 ? c.sense[2] & 15 :
                       format == 0x72 || format == 0x73 ? c.sense[1] & 15 : 0;
        /* The first SCSI access following a reset can report UNIT ATTENTION. */
        if (key == 6 && now - l->init_since < QUERY_TIMEOUT_US) return l->phase = UFS_LINK_SEND;
    }
    if ((c.result != UFS_RESULT_GOOD && c.result != UFS_RESULT_LENGTH) || c.bytes < 8)
        return fail(l, UFS_LINK_PROTOCOL);
    const uint8_t *p = l->io.ucd->lun_list;
    uint32_t length = (uint32_t)be16(p) << 16 | be16(p + 2);
    if (length % 8 || length > c.bytes - 8 || length / 8 != l->lun_count) return fail(l, UFS_LINK_PROTOCOL);
    uint32_t mask = 0;
    for (unsigned at = 8; at < 8 + length; at += 8) {
        /* UFS normal LUs use single-level peripheral addressing, bus 0. */
        if (p[at] || p[at + 1] >= l->lun_capacity || p[at + 2] || p[at + 3] || p[at + 4] ||
            p[at + 5] || p[at + 6] || p[at + 7]) return fail(l, UFS_LINK_PROTOCOL);
        uint32_t bit = UINT32_C(1) << p[at + 1];
        if (mask & bit) return fail(l, UFS_LINK_PROTOCOL);
        mask |= bit;
    }
    l->reported_luns = mask;
    return next_unit(l);
}

static enum ufs_link_phase complete(struct ufs_link *l, uint64_t now)
{
    const uint8_t *rsp = l->io.ucd->response, *cmd = l->io.ucd->request;
    l->io.consume(l->io.context);
    l->transfer_inflight = 0;
    l->ocs = l->io.list[0].data[8] & 15;
    if (l->ocs) {
        /* Before resetting a link, retry a peer communication error only
         * after that NOP's doorbell has cleared. Never recycle in-flight DMA.
         */
        if (l->command == UFS_MGMT_NOP && l->ocs == 5 && l->nop_attempts < 10)
            return l->phase = UFS_LINK_SEND;
        return fail(l, UFS_LINK_OCS);
    }
    if (l->command == UFS_MGMT_REPORT_LUNS) return report_luns(l, now);
    if (rsp[3] != cmd[3] || rsp[8]) return fail(l, UFS_LINK_PROTOCOL);
    unsigned data = be16(rsp + 10);
    if (l->command == UFS_MGMT_NOP) {
        if ((rsp[0] & 0x3f) != 0x20 || rsp[6] || data) return fail(l, UFS_LINK_PROTOCOL);
        l->query_status = 0;
        l->command = UFS_MGMT_SET_INIT;
        return l->phase = UFS_LINK_SEND;
    }
    if ((rsp[0] & 0x3f) != 0x36 || rsp[5] != cmd[5] || memcmp(rsp + 12, cmd + 12, 4))
        return fail(l, UFS_LINK_PROTOCOL);
    l->query_status = rsp[6];
    if (l->query_status) return fail(l, UFS_LINK_REJECTED);
    if (l->command == UFS_MGMT_SET_INIT || l->command == UFS_MGMT_READ_INIT) {
        if (data) return fail(l, UFS_LINK_PROTOCOL);
        if (l->command == UFS_MGMT_SET_INIT) {
            l->command = UFS_MGMT_READ_INIT;
            l->init_since = now;
        } else if (!(rsp[23] & 1)) {
            l->command = UFS_MGMT_DEVICE;
            return l->phase = UFS_LINK_SEND;
        } else if (now - l->init_since >= DEVICE_INIT_US) {
            return fail(l, UFS_LINK_TIMEOUT);
        }
        l->since = now;
        return l->phase = UFS_LINK_INIT_POLL;
    }
    unsigned returned = be16(rsp + 18);
    const uint8_t *desc = rsp + 32;
    if (data > UFS_DESCRIPTOR_MAX || returned > data || returned < 2 || desc[0] < 2 ||
        desc[0] > returned || desc[1] != cmd[13]) return fail(l, UFS_LINK_PROTOCOL);
    if (l->command == UFS_MGMT_DEVICE) {
        if (desc[0] < 0x1a) return fail(l, UFS_LINK_PROTOCOL);
        l->lun_count = desc[6];
        l->specification = be16(desc + 0x10);
        l->manufacturer = be16(desc + 0x18);
        l->command = UFS_MGMT_GEOMETRY;
    } else if (l->command == UFS_MGMT_GEOMETRY) {
        if (desc[0] < 13 || desc[12] > 1) return fail(l, UFS_LINK_PROTOCOL);
        l->lun_capacity = desc[12] ? 32 : 8;
        if (l->lun_count > l->lun_capacity) return fail(l, UFS_LINK_PROTOCOL);
        if (!l->lun_count) return l->phase = UFS_LINK_READY;
        l->command = UFS_MGMT_REPORT_LUNS;
        l->init_since = now;
    } else {
        if (desc[0] < 4 || desc[2] != l->unit || !desc[3]) return fail(l, UFS_LINK_PROTOCOL);
        l->enabled_luns |= UINT32_C(1) << l->unit;
        return next_unit(l);
    }
    return l->phase = UFS_LINK_SEND;
}

enum ufs_link_phase ufs_link_poll(struct ufs_link *l, uint64_t now)
{
    if (!l->owns_controller || l->stopping || l->phase == UFS_LINK_FAILED || l->phase == UFS_LINK_READY)
        return l->phase;
    l->fault_interrupts = read_reg(l, IS) & LINK_FATAL;
    if (l->fault_interrupts) return fail(l, UFS_LINK_CONTROLLER);
    switch (l->phase) {
    case UFS_LINK_WAIT_HCE:
        if (read_reg(l, HCE) & 1) {
            l->phase = UFS_LINK_WAIT_UIC;
            l->since = now;
        } else if (now - l->since >= UIC_TIMEOUT_US) return fail(l, UFS_LINK_TIMEOUT);
        break;
    case UFS_LINK_WAIT_UIC:
        if (!l->cold && (read_reg(l, HCS) & 1)) {
            l->phase = UFS_LINK_WAIT_PRESENT;
            l->since = now;
        } else if (read_reg(l, HCS) & 8) {
            write_reg(l, IS, UIC_COMPLETE);
            write_reg(l, UICARG1, 0); write_reg(l, UICARG2, 0); write_reg(l, UICARG3, 0);
            l->uic_inflight = 1;
            write_reg(l, UICCMD, 0x16); /* DME_LINK_STARTUP */
            l->phase = UFS_LINK_WAIT_STARTUP;
            l->since = now;
        } else if (now - l->since >= UIC_TIMEOUT_US) return fail(l, UFS_LINK_TIMEOUT);
        break;
    case UFS_LINK_WAIT_STARTUP:
        if (read_reg(l, IS) & UIC_COMPLETE) {
            l->uic_result = (uint8_t)(read_reg(l, UICARG2) & 0xffu);
            l->uic_inflight = 0;
            write_reg(l, IS, UIC_COMPLETE);
            if (l->uic_result) return fail(l, UFS_LINK_UIC);
            l->phase = UFS_LINK_WAIT_PRESENT;
            l->since = now;
        } else if (now - l->since >= UIC_TIMEOUT_US) return fail(l, UFS_LINK_TIMEOUT);
        break;
    case UFS_LINK_WAIT_PRESENT:
        if (!open_list(l, now) && l->phase != UFS_LINK_FAILED && now - l->since >= UIC_TIMEOUT_US)
            return fail(l, UFS_LINK_TIMEOUT);
        break;
    case UFS_LINK_SEND: submit(l, now); break;
    case UFS_LINK_WAIT_REPLY:
        if (!(read_reg(l, UTRLDBR) & 1)) return complete(l, now);
        if (now - l->since >= (l->command == UFS_MGMT_NOP ? NOP_TIMEOUT_US : QUERY_TIMEOUT_US))
            return fail(l, UFS_LINK_TIMEOUT);
        break;
    case UFS_LINK_INIT_POLL:
        if (now - l->since >= POLL_INIT_US) l->phase = UFS_LINK_SEND;
        break;
    default: return fail(l, UFS_LINK_PROTOCOL);
    }
    return l->phase;
}

int ufs_link_stop(struct ufs_link *l)
{
    if (!l->owns_controller) return 1;
    unsigned first = !l->stopping;
    l->stopping = 1;
    if (l->uic_inflight) {
        if (!(read_reg(l, IS) & UIC_COMPLETE)) return 0;
        l->uic_inflight = 0;
        write_reg(l, IS, UIC_COMPLETE);
    }
    if (l->owns_list) {
        if (first) {
            write_reg(l, UTRLRSR, 0);
            if (l->transfer_inflight && (read_reg(l, UTRLDBR) & 1)) write_reg(l, UTRLCLR, ~UINT32_C(1));
        }
        if (read_reg(l, UTRLRSR) || read_reg(l, UTRLDBR)) return 0;
        l->io.consume(l->io.context);
        write_reg(l, IS, UTR_COMPLETE);
        write_reg(l, UTRLBA, 0); write_reg(l, UTRLBAU, 0);
        l->owns_list = l->transfer_inflight = 0;
    }
    write_reg(l, IE, l->previous_ie);
    l->owns_controller = 0;
    if (l->phase != UFS_LINK_READY && l->phase != UFS_LINK_FAILED) l->phase = UFS_LINK_STOPPED;
    return 1;
}
