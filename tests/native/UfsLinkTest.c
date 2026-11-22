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
    unsigned startup, publish, consume, read_init, transfers, hold_stop, get_count, hibern8, hibern8_exit;
    unsigned hold_hce, refuse_timer;
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
    } else if (reg == 0x90 && value == 1) {
        static const unsigned pa[] = {0x1571,0x1568,0x1583,0x1560,0x1580,0x156a};
        assert(h->get_count < sizeof(pa)/sizeof(*pa) && h->regs[0x94 / 4] == (pa[h->get_count] << 16));
        assert(!h->regs[0x98 / 4] && !h->regs[0x9c / 4] && !(h->regs[0x20 / 4] & 0x400));
        ++h->get_count;
    } else if (reg == 0x90 && (value == 0x17 || value == 0x18)) {
        assert(!h->regs[0x94 / 4] && !h->regs[0x98 / 4] && !h->regs[0x9c / 4]);
        assert(!(h->regs[0x20 / 4] & 0x440) && !h->regs[0x18 / 4]);
        if (value == 0x17) ++h->hibern8;
        else ++h->hibern8_exit;
    } else if (reg == 0x90) {
        assert(value == 0x16 && !h->regs[0x94 / 4] && !h->regs[0x98 / 4] && !h->regs[0x9c / 4]);
        ++h->startup;
    } else if (!(reg == 0x34 && h->hold_hce) && !(reg == 0x18 && h->refuse_timer)) {
        h->regs[reg / 4] = value;
    }
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
                desc[5] = request[14] == 6; /* LU 6 supports power-on write protection. */
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

/* Complete only what the device actually received. Never derive the expected
 * LU, CDB, flag or transition from the driver's internal command enum. */
static void media_reply(struct host *h, uint8_t opcode, uint8_t id, uint8_t mode, uint32_t value)
{
    assert(h->l.phase == UFS_LINK_WAIT_REPLY && h->regs[0x58 / 4] == 1);
    uint8_t *r = h->ucd.request, *rsp = h->ucd.response;
    assert(h->list[0].data[3] == 0x11 && h->list[0].data[8] == 15 &&
           !h->list[0].data[28] && !h->list[0].data[29]);
    uint8_t expected[32] = {0};
    if (opcode == 0x35 || opcode == 0x1b) {
        expected[0] = 1; expected[1] = 1; expected[2] = id;
        expected[16] = opcode; expected[20] = mode;
        rsp[0] = 0x21; rsp[2] = id;
    } else {
        expected[0] = 0x16;
        expected[5] = opcode == 6 || opcode == 7 ? 0x81 : 1;
        expected[12] = opcode; expected[13] = id;
        rsp[0] = 0x36; rsp[5] = expected[5]; rsp[12] = opcode; rsp[13] = id;
        for (unsigned i = 0; i < 4; ++i) rsp[23-i] = (uint8_t)(value >> (8*i));
    }
    assert(!memcmp(expected, r, sizeof(expected)));
    h->list[0].data[8] = 0;
    h->regs[0x58 / 4] = 0; h->regs[0x20 / 4] |= 1;
}

static void request(struct host *h)
{
    for (unsigned n = 0; n < 8 && h->l.phase != UFS_LINK_WAIT_REPLY; ++n)
        assert(pump(h) != UFS_LINK_FAILED);
    assert(h->l.phase == UFS_LINK_WAIT_REPLY);
}

static void answer(struct host *h, uint8_t opcode, uint8_t id, uint8_t mode, uint32_t value)
{
    request(h);
    media_reply(h, opcode, id, mode, value);
    assert(pump(h) != UFS_LINK_FAILED);
}

static void start_down(struct host *h, struct ufs_media_settings *m, enum ufs_link_operation operation)
{
    init(h, 1);
    assert(ufs_link_stop(&h->l));
    h->regs[0x18 / 4] = 0x401;
    struct ufs_link_io io = h->l.io;
    assert(ufs_link_media(&h->l, &io, m, UINT32_C(0x80000040), operation, h->now) == UFS_LINK_OK);
    assert(!h->regs[0x18 / 4] && m->restore_ahit && m->ahit == 0x401);
}

static void check_media_power(void)
{
    struct host h;
    struct ufs_media_settings m = {0};
    start_down(&h, &m, UFS_POWER_OFF);
    answer(&h, 0x35, 6, 0, 0);
    answer(&h, 0x35, 31, 0, 0); /* Sparse LUs; no guessed disk zero. */
    answer(&h, 5, 4, 0, 1);
    answer(&h, 7, 4, 0, 0);
    answer(&h, 5, 4, 0, 0);
    answer(&h, 0x1b, 0xd0, 0x30, 0);
    answer(&h, 3, 2, 0, 0x30);
    assert(!h.hibern8 && h.regs[0x34 / 4] == 1);
    answer(&h, 3, 2, 0, 0x33);
    h.regs[0x20 / 4] |= 0x440; /* An earlier UIC completion proves nothing. */
    assert(pump(&h) == UFS_LINK_WAIT_HIBERN8 && h.hibern8 == 1);
    h.regs[0x20 / 4] |= 0x400;
    assert(pump(&h) == UFS_LINK_WAIT_HIBERN8 && h.regs[0x34 / 4] == 1);
    h.regs[0x20 / 4] |= 0x40;
    assert(pump(&h) == UFS_LINK_STOP_HOST && pump(&h) == UFS_LINK_WAIT_OFF);
    assert(!h.regs[0x60 / 4] && !h.regs[0x50 / 4] && !h.regs[0x34 / 4]);
    assert(pump(&h) == UFS_LINK_WAIT_OFF); /* Inactive-link delay before releasing VCC. */
    assert(pump(&h) == UFS_LINK_OFF && !h.l.owns_controller && !h.l.owns_list && m.restore_bkops);

    /* Wake from OFF: a new link, not Hibern8 Exit on a reset HCI. With no
     * device power cycle the media may still report PowerDown. */
    struct ufs_link_io io = h.l.io;
    h.regs[0x30 / 4] = 8;
    assert(ufs_link_begin(&h.l, &io, h.now) == UFS_LINK_OK);
    h.l.media = &m;
    for (unsigned n = 0; n < 20 && h.l.command != UFS_MGMT_READ_MODE; ++n) {
        if (h.l.phase == UFS_LINK_WAIT_REPLY) deliver(&h);
        if (h.l.phase == UFS_LINK_WAIT_STARTUP) {
            h.regs[0x30 / 4] = 0x10f; h.regs[0x20 / 4] |= 0x400;
        }
        assert(pump(&h) != UFS_LINK_FAILED);
    }
    assert(h.startup == 1 && h.regs[0x18 / 4] == 0);
    answer(&h, 3, 2, 0, 0x33);
    answer(&h, 0x1b, 0xd0, 0x10, 0);
    answer(&h, 3, 2, 0, 0x10);
    answer(&h, 3, 2, 0, 0x11);
    answer(&h, 6, 4, 0, 0);
    answer(&h, 5, 4, 0, 1);
    for (unsigned n = 0; n < 25 && h.l.phase != UFS_LINK_READY; ++n) {
        if (h.l.phase == UFS_LINK_WAIT_REPLY) deliver(&h);
        assert(pump(&h) != UFS_LINK_FAILED);
    }
    assert(h.l.phase == UFS_LINK_READY && !m.restore_bkops && m.check_power_on_wp);
    assert(ufs_link_stop(&h.l) && h.regs[0x18 / 4] == 0x401 && !m.restore_ahit);

    start_down(&h, &m, UFS_POWER_OFF);
    answer(&h, 0x35, 6, 0, 0);
    answer(&h, 0x35, 31, 0, 0);
    answer(&h, 5, 3, 0, 1);
    assert(h.l.phase == UFS_LINK_KEEP_POWER && !h.hibern8 && h.regs[0x34 / 4]);
    assert(ufs_link_stop(&h.l) && h.l.phase == UFS_LINK_KEEP_POWER &&
           !m.restore_ahit && h.regs[0x18 / 4] == 0x401 && h.l.enabled_luns == UINT32_C(0x80000040));

    for (unsigned fault = 0; fault != 8; ++fault) {
        m = (struct ufs_media_settings){0};
        start_down(&h, &m, UFS_POWER_OFF);
        if (fault == 0) {
            request(&h);
            assert(ufs_link_poll(&h.l, h.now + 10000000) == UFS_LINK_FAILED);
            h.hold_stop = 1;
            assert(!ufs_link_stop(&h.l) && h.l.owns_list && h.regs[0x50 / 4]);
            h.regs[0x58 / 4] = 0;
        } else {
            answer(&h, 0x35, 6, 0, 0);
            answer(&h, 0x35, 31, 0, 0);
            answer(&h, 5, 4, 0, fault == 1 ? 1 : 0);
            if (fault == 1) {
                answer(&h, 7, 4, 0, 0);
                request(&h); media_reply(&h, 5, 4, 0, 1); /* Clear did not stick. */
            } else if (fault == 2) {
                request(&h); media_reply(&h, 0x1b, 0xd0, 0x30, 0);
                h.ucd.response[2] = 0; /* Good status, wrong LU. */
            } else {
                answer(&h, 0x1b, 0xd0, 0x30, 0);
                if (fault == 3) {
                    request(&h); media_reply(&h, 3, 2, 0, 0x11);
                } else {
                    answer(&h, 3, 2, 0, 0x33);
                    assert(pump(&h) == UFS_LINK_WAIT_HIBERN8);
                    h.regs[0x20 / 4] |= 0x400;
                    if (fault == 4) {
                        assert(ufs_link_poll(&h.l, h.now + 500000) == UFS_LINK_FAILED);
                        assert(!ufs_link_stop(&h.l) && h.regs[0x34 / 4]);
                        h.regs[0x20 / 4] |= 0x40;
                    } else {
                        h.regs[0x20 / 4] |= 0x40;
                        if (fault == 5) h.regs[0x30 / 4] = 0x20f; /* PWR_REMOTE, not PWR_LOCAL. */
                        else {
                            assert(pump(&h) == UFS_LINK_STOP_HOST);
                            if (fault == 6) h.regs[0x78 / 4] = 1; /* Foreign task management DMA. */
                            else {
                                h.hold_hce = 1;
                                assert(pump(&h) == UFS_LINK_WAIT_OFF);
                                assert(ufs_link_poll(&h.l, h.now + 500000) == UFS_LINK_FAILED);
                            }
                        }
                    }
                }
            }
        }
        assert(pump(&h) == UFS_LINK_FAILED && h.regs[0x34 / 4] == 1);
        assert(ufs_link_stop(&h.l) && h.l.phase == UFS_LINK_FAILED);
    }

    /* S0 D3hot keeps the original write protection and both power rails.
     * This host reports only the dedicated Hibern8 event, not UCCS. */
    m = (struct ufs_media_settings){ .check_power_on_wp = 1 };
    start_down(&h, &m, UFS_SLEEP);
    answer(&h, 0x35, 6, 0, 0); answer(&h, 0x35, 31, 0, 0);
    answer(&h, 5, 3, 0, 1); answer(&h, 5, 4, 0, 0);
    answer(&h, 0x1b, 0xd0, 0x20, 0); answer(&h, 3, 2, 0, 0x22);
    assert(pump(&h) == UFS_LINK_WAIT_HIBERN8);
    h.regs[0x20 / 4] |= 0x40;
    assert(pump(&h) == UFS_LINK_STOP_HOST && pump(&h) == UFS_LINK_ASLEEP && !h.l.owns_controller);
    assert(h.regs[0x34 / 4] == 1 && !h.regs[0x60 / 4] && !h.regs[0x58 / 4] && m.verify_power_on_wp);
    io = h.l.io;
    assert(ufs_link_media(&h.l, &io, &m, h.l.enabled_luns, UFS_WAKE, h.now) == UFS_LINK_OK);
    assert(pump(&h) == UFS_LINK_WAIT_HIBERN8 && h.hibern8_exit == 1 && !h.startup);
    h.regs[0x20 / 4] |= 0x40; /* Wrong direction cannot wake the device. */
    assert(pump(&h) == UFS_LINK_WAIT_HIBERN8);
    h.regs[0x20 / 4] |= 0x20;
    assert(pump(&h) == UFS_LINK_WAIT_PRESENT);
    answer(&h, 0x1b, 0xd0, 0x10, 0);
    answer(&h, 3, 2, 0, 0x11);
    answer(&h, 5, 3, 0, 1);
    assert(!m.verify_power_on_wp);
    for (unsigned n = 0; n < 25 && h.l.phase != UFS_LINK_READY; ++n) {
        if (h.l.phase == UFS_LINK_WAIT_REPLY) deliver(&h);
        assert(pump(&h) != UFS_LINK_FAILED);
    }
    assert(h.l.phase == UFS_LINK_READY && h.l.enabled_luns == 0x41 && ufs_link_stop(&h.l));
    /* A missing UIC event must not block recovery. Reset can complete late;
     * never recycle the common buffer while either engine might still DMA. */
    start_down(&h, &m, UFS_SLEEP);
    answer(&h, 0x35, 6, 0, 0); answer(&h, 0x35, 31, 0, 0);
    answer(&h, 5, 3, 0, 1); answer(&h, 5, 4, 0, 0);
    answer(&h, 0x1b, 0xd0, 0x20, 0); answer(&h, 3, 2, 0, 0x22);
    assert(pump(&h) == UFS_LINK_WAIT_HIBERN8);
    h.regs[0x20 / 4] |= 0x400;
    assert(ufs_link_poll(&h.l, h.now + 500000) == UFS_LINK_FAILED && !ufs_link_stop(&h.l));
    h.hold_hce = 1;
    assert(!ufs_link_reset(&h.l) && h.l.owns_list && h.regs[0x50 / 4]);
    h.regs[0x34 / 4] = 0;
    assert(!ufs_link_stop(&h.l) && h.l.owns_controller);
    h.regs[0x60 / 4] = 0; h.regs[0x78 / 4] = 1;
    assert(!ufs_link_reset(&h.l) && h.l.owns_controller);
    h.regs[0x78 / 4] = 0;
    assert(ufs_link_reset(&h.l) && !h.l.owns_controller && !h.l.owns_list && !h.l.uic_inflight);
    h.hold_hce = 0; io = h.l.io;
    assert(ufs_link_begin(&h.l, &io, h.now) == UFS_LINK_OK);
    assert(pump(&h) == UFS_LINK_WAIT_UIC && pump(&h) == UFS_LINK_WAIT_STARTUP && h.startup == 1);
}

int main(void)
{
    check_media_power();
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
        h.l.read_parameters = (uint8_t)warm;
        static const uint8_t attr[] = {0x11,4,5,1,2,2};
        for (unsigned i = 0; i < 100 && h.l.phase != UFS_LINK_READY; ++i) {
            if (h.l.phase == UFS_LINK_WAIT_STARTUP) { h.regs[0x20 / 4] |= 0x400; h.regs[0x30 / 4] = 0x10f; }
            if (h.l.phase == UFS_LINK_WAIT_REPLY) deliver(&h);
            if (h.l.phase == UFS_LINK_WAIT_PARAMETER) {
                assert(h.get_count && h.get_count <= sizeof(attr));
                h.regs[0x9c / 4] = attr[h.get_count-1];
                h.regs[0x20 / 4] |= 0x400;
            }
            assert(pump(&h) != UFS_LINK_FAILED);
        }
        assert(h.l.phase == UFS_LINK_READY && h.l.error == UFS_LINK_OK && h.l.enabled_luns == 0x41 &&
               h.l.lun_count == 2 && h.l.specification == 0x410 && h.l.manufacturer == 0x01ce);
        assert(h.startup == (unsigned)!warm && h.read_init == 3);
        if (warm) assert(h.l.parameter_count == sizeof(attr) && !memcmp(h.l.parameter,attr,sizeof(attr)));
        else assert(!h.get_count && !h.l.parameter_count);
        assert(ufs_link_stop(&h.l) && !h.l.owns_controller && !h.l.owns_list &&
               h.regs[0x24 / 4] == h.original_ie && !h.regs[0x50 / 4] && !h.regs[0x54 / 4]);
    }
    /* Host power-down in the middle of a DME_GET must wait for UIC, then
     * actually stop its transfer list. A late completion is not a new UIC. */
    init(&h,1); h.l.read_parameters = 1;
    for (unsigned i = 0; i < 100 && h.l.phase != UFS_LINK_WAIT_PARAMETER; ++i) {
        if (h.l.phase == UFS_LINK_WAIT_REPLY) deliver(&h);
        assert(pump(&h) != UFS_LINK_FAILED);
    }
    assert(h.l.owns_list && h.l.uic_inflight && !ufs_link_stop(&h.l));
    assert(pump(&h) == UFS_LINK_WAIT_PARAMETER && h.get_count == 1);
    h.regs[0x20 / 4] |= 0x400;
    assert(ufs_link_stop(&h.l) && !h.regs[0x60 / 4] && !h.l.owns_controller);
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
