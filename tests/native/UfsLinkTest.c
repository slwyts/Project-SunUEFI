/* SPDX-License-Identifier: BSD-2-Clause */
#include "link.h"
#include <assert.h>
#include <string.h>

struct host {
    uint32_t regs[0x304 / 4];
    struct ufs_utrd list[1];
    struct ufs_management_ucd ucd;
    struct ufs_link l;
    uint64_t now;
    unsigned startup, publish, consume, read_init, transfers, hold_stop;
    uint32_t original_ie;
};
static uint32_t reg_read(void *ctx, unsigned reg) { return ((struct host *)ctx)->regs[reg / 4]; }
static void reg_write(void *ctx, unsigned reg, uint32_t value)
{
    struct host *h = ctx;
    if (reg == 0x20) h->regs[reg / 4] &= ~value; /* W1C */
    else if (reg == 0x58) {
        assert(value == 1 && !h->regs[reg / 4] && h->regs[0x60 / 4] == 1);
        assert(h->publish == ++h->transfers); h->regs[reg / 4] |= value;
    } else if (reg == 0x5c) {
        assert(value == ~UINT32_C(1)); if (!h->hold_stop) h->regs[0x58 / 4] &= value;
    } else if (reg == 0x90) {
        assert(value == 0x16 && !h->regs[0x94 / 4] && !h->regs[0x98 / 4] && !h->regs[0x9c / 4]);
        ++h->startup;
    } else h->regs[reg / 4] = value;
}
static void barrier_out(void *ctx) { ++((struct host *)ctx)->publish; }
static void barrier_in(void *ctx) { ++((struct host *)ctx)->consume; }
static void init(struct host *h, int warm)
{
    memset(h, 0, sizeof(*h));
    h->regs[0] = 0xd5c7031f; h->regs[8 / 4] = 0x400;
    h->regs[0x30 / 4] = warm ? 0x10f : 8; h->regs[0x34 / 4] = warm ? 1 : 0;
    h->regs[0x24 / 4] = h->original_ie = 0x18421;
    struct ufs_link_io io = {h, reg_read, reg_write, barrier_out, barrier_in, h->list, &h->ucd,
                             UINT64_C(0x800001000), UINT64_C(0x8ffff8000)};
    assert(ufs_link_begin(&h->l, &io, 0) == UFS_LINK_OK);
}

static void deliver(struct host *h)
{
    uint8_t *request = h->ucd.request, *rsp = h->ucd.response;
    assert(h->regs[0x58 / 4] == 1 && h->list[0].data[8] == 15);
    int scsi = request[0] == 1;
    assert(h->list[0].data[3] == (scsi ? 0x15 : 0x11) && h->list[0].data[24] == (scsi ? 32 : 72) &&
           h->list[0].data[26] == 32 && h->list[0].data[28] == (scsi ? 1 : 0));
    assert(h->list[0].data[18] == 0xff && h->list[0].data[20] == 8);
    assert(h->regs[0x50 / 4] == 0x1000 && h->regs[0x54 / 4] == 8);
    if (scsi) {
        assert(request[1] == 0x40 && request[2] == 0x81 && request[16] == 0xa0 && !request[18] &&
               request[24] == 1 && request[25] == 8 && h->list[0].data[30] == 104);
        const uint8_t prd[16] = {0xb0, 0x81, 0xff, 0xff, 8, 0, 0, 0, 0, 0, 0, 0, 7, 1, 0, 0};
        assert(!memcmp(prd, h->ucd.report_prd.data, sizeof(prd)));
        rsp[0] = 0x21; rsp[1] = 0x20; rsp[2] = 0x81; rsp[15] = 240;
        h->ucd.lun_list[3] = 16; h->ucd.lun_list[9] = 6; h->ucd.lun_list[17] = 0;
    } else if (!request[0]) {
        for (unsigned i = 0; i < 32; ++i) assert(!request[i]);
        rsp[0] = 0x20;
    } else {
        assert(request[0] == 0x16 && request[3] == 0);
        rsp[0] = 0x36; rsp[5] = request[5];
        memcpy(rsp + 12, request + 12, 4);
        if (request[12] == 6) assert(request[13] == 1 && request[5] == 0x81);
        else if (request[12] == 5) {
            assert(request[13] == 1 && request[5] == 1);
            rsp[23] = h->read_init++ < 2;
        } else {
            assert(request[12] == 1 && request[5] == 1 && request[19] == 255 && !request[15]);
            uint8_t *desc = rsp + 32;
            uint8_t length;
            if (!request[13]) {
                length = 0x59; desc[6] = 2; desc[16] = 4; desc[17] = 0x10; desc[24] = 0x01; desc[25] = 0xce;
            } else if (request[13] == 7) {
                length = 0x57; desc[12] = 0; /* eight addressable units */
            } else {
                assert(request[13] == 2 && (request[14] == 0 || request[14] == 6));
                length = 0x2d; desc[2] = request[14];
                desc[3] = request[14] == 0 || request[14] == 6;
            }
            rsp[11] = length; rsp[19] = length; desc[0] = length; desc[1] = request[13];
        }
    }
    h->list[0].data[8] = 0; h->regs[0x58 / 4] = 0; h->regs[0x20 / 4] |= 1;
}

static enum ufs_link_phase pump(struct host *h)
{
    h->now += 1000;
    return ufs_link_poll(&h->l, h->now);
}
static void warm_nop(struct host *h)
{
    init(h, 1);
    for (unsigned i = 0; i < 10 && h->l.phase != UFS_LINK_WAIT_REPLY; ++i) pump(h);
    assert(h->l.phase == UFS_LINK_WAIT_REPLY && !h->ucd.request[0]);
}

int main(void)
{
    const uint8_t report_luns[12] = {0xa0}, inquiry[6] = {0x12}, flush[10] = {0x35};
    uint8_t wire_lun = 0xee;
    assert(ufs_scsi_lun(0x80, 0, report_luns, sizeof(report_luns), &wire_lun) && wire_lun == 0x81);
    assert(!ufs_scsi_lun(0x80, 0, inquiry, sizeof(inquiry), &wire_lun));
    assert(ufs_scsi_lun(0x80, 7, inquiry, sizeof(inquiry), &wire_lun) && wire_lun == 7);
    assert(ufs_scsi_lun(0x80000000u, 31, flush, sizeof(flush), &wire_lun) && wire_lun == 31);
    assert(!ufs_scsi_lun(~0u, 32, inquiry, sizeof(inquiry), &wire_lun));
    assert(!ufs_scsi_lun(0x80, 0, report_luns, 6, &wire_lun));
    struct host h;
    for (int warm = 0; warm != 2; ++warm) {
        init(&h, warm);
        for (unsigned i = 0; i < 100 && h.l.phase != UFS_LINK_READY; ++i) {
            if (h.l.phase == UFS_LINK_WAIT_STARTUP) { h.regs[0x20 / 4] |= 0x400; h.regs[0x30 / 4] = 0x10f; }
            if (h.l.phase == UFS_LINK_WAIT_REPLY) deliver(&h);
            assert(pump(&h) != UFS_LINK_FAILED);
        }
        assert(h.l.phase == UFS_LINK_READY && h.l.error == UFS_LINK_OK && h.l.enabled_luns == 0x41 &&
               h.l.lun_count == 2 && h.l.specification == 0x410 && h.l.manufacturer == 0x01ce);
        assert(h.startup == (unsigned)!warm && h.read_init == 3);
        assert(ufs_link_stop(&h.l) && !h.l.owns_controller && !h.l.owns_list &&
               h.regs[0x24 / 4] == h.original_ie && !h.regs[0x50 / 4] && !h.regs[0x54 / 4]);
    }
    warm_nop(&h);
    assert(ufs_link_poll(&h.l, h.now + 50000) == UFS_LINK_FAILED && h.l.error == UFS_LINK_TIMEOUT);
    h.hold_stop = 1;
    assert(!ufs_link_stop(&h.l) && h.l.owns_controller && h.regs[0x50 / 4] == 0x1000);
    h.regs[0x58 / 4] = 0;
    assert(ufs_link_stop(&h.l));
    warm_nop(&h);
    deliver(&h); h.list[0].data[8] = 5; /* OCS_PEER_COMM_FAILURE, DMA already stopped. */
    assert(pump(&h) == UFS_LINK_SEND && h.l.command == UFS_MGMT_NOP);
    assert(pump(&h) == UFS_LINK_WAIT_REPLY && h.l.nop_attempts == 2);
    deliver(&h); h.ucd.response[3] = 0xff;
    assert(pump(&h) == UFS_LINK_FAILED && h.l.error == UFS_LINK_PROTOCOL);
    assert(ufs_link_stop(&h.l));
    init(&h, 0); pump(&h); pump(&h); assert(h.l.phase == UFS_LINK_WAIT_STARTUP);
    assert(!ufs_link_stop(&h.l) && h.l.uic_inflight && !h.transfers);
    h.regs[0x20 / 4] |= 0x400;
    assert(pump(&h) == UFS_LINK_WAIT_STARTUP && !h.transfers);
    assert(ufs_link_stop(&h.l) && h.l.phase == UFS_LINK_STOPPED);
    init(&h, 0); pump(&h); pump(&h);
    h.regs[0x20 / 4] |= 0x400; h.regs[0x98 / 4] = 2;
    assert(pump(&h) == UFS_LINK_FAILED && h.l.error == UFS_LINK_UIC && h.l.uic_result == 2);
    assert(ufs_link_stop(&h.l));
    /* Replay Piano's actual rejected READ_DESCRIPTOR. This is a valid
     * Query Response with 0xff from the device, not a successful descriptor.
     */
    warm_nop(&h);
    while (h.l.command != UFS_MGMT_DEVICE || h.l.phase != UFS_LINK_WAIT_REPLY) {
        if (h.l.phase == UFS_LINK_WAIT_REPLY) deliver(&h);
        assert(pump(&h) != UFS_LINK_FAILED);
    }
    deliver(&h); h.ucd.response[6] = 0xff;
    assert(pump(&h) == UFS_LINK_FAILED && h.l.error == UFS_LINK_REJECTED && h.l.query_status == 0xff);
    assert(ufs_link_stop(&h.l));
    for (int test = 0; test < 3; ++test) {
        warm_nop(&h);
        while (h.l.command != UFS_MGMT_REPORT_LUNS || h.l.phase != UFS_LINK_WAIT_REPLY) {
            if (h.l.phase == UFS_LINK_WAIT_REPLY) deliver(&h);
            assert(pump(&h) != UFS_LINK_FAILED);
        }
        deliver(&h);
        if (test == 0) h.ucd.lun_list[17] = 6;
        if (test == 1) h.ucd.lun_list[3] = 8;
        if (test == 2) { h.list[0].data[8] = 1; h.ucd.response[6] = 0xfc; }
        assert(pump(&h) == UFS_LINK_FAILED && h.l.error == (test == 2 ? UFS_LINK_OCS : UFS_LINK_PROTOCOL));
        assert(ufs_link_stop(&h.l));
    }
    return 0;
}
