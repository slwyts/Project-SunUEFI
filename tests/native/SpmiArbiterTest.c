/* SPDX-License-Identifier: BSD-2-Clause */
#include "interrupt.h"
#include "gpio_input.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { PON = 0x013, GPIO6 = 0x18d, PON_ID = 581, GPIO_ID = 610, IRQ = 1 << 24 };
struct model {
    uint32_t map[1024], owners[1024], acc[1024], interrupts[1024], summary[32];
    uint32_t rdata[2], wdata[2], version, status, result, last_command;
    unsigned submits, stalls, drop_register, drop_once;
    uint8_t regs[2][256], latched[2];
};

static int read_reg(void *ctx, enum spmi_region r, uint32_t off, uint32_t *v)
{
    struct model *m = ctx;
    assert((off & 3) == 0);
    switch (r) {
    case SPMI_CORE:
        if (off == 0) *v = m->version;
        else if (off == 4) *v = 896;
        else if (off == 8) *v = 20;
        else { assert(off >= 0x2000 && off < 0x3000); *v = m->map[(off - 0x2000)/4]; }
        break;
    case SPMI_CONFIG:
        assert(off < 4096);
        *v = m->owners[off/4];
        break;
    case SPMI_INTERRUPT:
        assert(off < sizeof(m->summary));
        *v = m->summary[off/4];
        break;
    case SPMI_CHANNELS:
        assert(off/4096 < 1024);
        switch (off % 4096) {
        case 0x100: *v = m->acc[off/4096]; break;
        case 0x104: *v = m->interrupts[off/4096]; break;
        case 8: *v = m->status; break;
        default: assert(0);
        }
        break;
    case SPMI_OBSERVER:
        switch (off % 32) {
        case 8: *v = m->status; break;
        case 0x18: case 0x1c: *v = m->rdata[(off % 32 - 0x18)/4]; break;
        default: assert(0);
        }
        break;
    default: assert(0);
    }
    return 1;
}

static int write_reg(void *ctx, enum spmi_region r, uint32_t off, uint32_t v)
{
    struct model *m = ctx;
    assert(r == SPMI_OBSERVER || r == SPMI_CHANNELS);
    unsigned id = r == SPMI_CHANNELS ? off/4096 : (off % 0x8000)/32;
    unsigned port = r == SPMI_CHANNELS ? off % 4096 : off % 32;
    assert(id < 1024 && m->map[id]);
    switch (port) {
    case 0x100: m->acc[id] = v; return 1;
    case 0x108: m->interrupts[id] &= ~v; return 1;
    case 0x10: case 0x14: m->wdata[(port-0x10)/4] = v; return 1;
    default: assert(port == 0);
    }
    unsigned peripheral = (m->map[id] >> 8) & 0xfff;
    assert(peripheral == PON || peripheral == GPIO6);
    unsigned n = (v & 7) + 1, addr = (v >> 4) & 255, target = peripheral == GPIO6;
    assert((v >> 27) == (r == SPMI_OBSERVER));
    assert(addr + n <= 256);
    m->last_command = v;
    m->submits++;
    m->status = m->result;
    if (r == SPMI_CHANNELS && m->drop_once && addr == m->drop_register) {
        m->status = 9;
        m->drop_once = 0;
    }
    if (m->status != 1) return 1;
    m->rdata[0] = m->rdata[1] = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (r == SPMI_OBSERVER) m->rdata[i/4] |= (uint32_t)m->regs[target][addr+i] << (8*(i%4));
        else {
            uint8_t val = (uint8_t)(m->wdata[i/4] >> (8*(i%4)));
            switch (addr+i) {
            case 0x14: m->latched[target] &= (uint8_t)~val; break;
            case 0x15: m->regs[target][0x15] |= val; break;
            case 0x16: m->regs[target][0x15] &= (uint8_t)~val; break;
            default: m->regs[target][addr+i] = val; break;
            }
        }
    }
    return 1;
}
static void delay(void *ctx, unsigned us) { ((struct model *)ctx)->stalls += us; }

int main(void)
{
    struct model m = {0};
    struct spmi_register_io io = {&m, read_reg, write_reg, delay};
    struct spmi_arbiter a = {0};
    m.version = 0x70020000;
    m.result = 1;
    m.map[8] = IRQ | (PON << 8); m.owners[8] = 3;
    m.map[PON_ID] = IRQ | (PON << 8); m.map[GPIO_ID] = IRQ | (GPIO6 << 8);
    assert(spmi_v7_start(&a,&io,0,0) == SPMI_OK);
    assert(a.count == 896);
    struct spmi_irq power, down, up;
    assert(spmi_irq_open(&a,PON,7,&power) == SPMI_OK && power.peripheral.irq == PON_ID);
    assert(spmi_irq_open(&a,PON,6,&down) == SPMI_OK);
    assert(spmi_irq_open(&a,GPIO6,0,&up) == SPMI_OK && up.peripheral.irq == GPIO_ID);
    m.map[700] = PON << 8;
    assert(spmi_irq_open(&a,PON,7,&power) == SPMI_OK);
    assert(power.peripheral.writer == 700 && power.peripheral.irq == PON_ID);
    m.map[701] = IRQ | (PON << 8); m.owners[701] = 3;
    assert(spmi_irq_open(&a,PON,7,&down) == SPMI_NOT_OWNER);
    struct spmi_peripheral peripheral;
    assert(spmi_v7_resolve(&a,PON,&peripheral) == SPMI_OK && peripheral.irq == SPMI_NO_APID);
    m.map[701] = 0;
    assert(spmi_irq_open(&a,PON,6,&down) == SPMI_OK);
    assert(spmi_irq_open(&a,0x222,2,&up) == SPMI_NOT_FOUND);

    m.regs[0][0x10] = 0xc0;
    m.regs[1][0x10] = 0x00;
    unsigned level;
    assert(spmi_irq_level(&a,&power,&level) == SPMI_OK && level == 1);
    assert(spmi_irq_level(&a,&down,&level) == SPMI_OK && level == 1);
    assert(spmi_irq_level(&a,&up,&level) == SPMI_OK && level == 0);
    m.regs[0][0x11] = 0x5a; m.regs[0][0x12] = 0x09; m.regs[0][0x13] = 0xc4;
    assert(spmi_irq_configure(&a,&power,SPMI_HIGH) == SPMI_OK);
    assert(m.regs[0][0x11] == 0x5a && m.regs[0][0x12] == 0x89 && m.regs[0][0x13] == 0x44);
    assert(spmi_irq_configure(&a,&power,SPMI_BOTH) == SPMI_OK);
    assert(m.regs[0][0x11] == 0xda && m.regs[0][0x12] == 0x89 && m.regs[0][0x13] == 0xc4);
    m.regs[0][0x15] = 0x45; m.latched[0] = 0xe5;
    assert(spmi_irq_enable(&a,&power) == SPMI_OK);
    assert(m.acc[PON_ID] == 1 && !m.acc[700] && m.regs[0][0x15] == 0xc5 && m.latched[0] == 0x65);
    assert(spmi_irq_enabled(&a,&power,&level) == SPMI_OK && level == 1);
    m.interrupts[PON_ID] = 0xc5;
    m.interrupts[GPIO_ID] = 1; /* GPIO6 not enabled by this controller. */
    assert(spmi_irq_pending(&a,&up,&level) == SPMI_OK && !level);
    assert(spmi_irq_pending(&a,&power,&level) == SPMI_OK && level);
    uint64_t active;
    struct spmi_irq pins[] = {power, up, down};
    assert(spmi_irq_active_bank(&a,pins,3,7,&active) == SPMI_OK && active == 5);
    m.summary[PON_ID/32] = 1; /* Another client on this shared parent IRQ. */
    assert(spmi_irq_active_bank(&a,pins,3,7,&active) == SPMI_OK && active == 0);
    m.summary[PON_ID/32] |= 1u << (PON_ID%32);
    assert(spmi_irq_active_bank(&a,pins,3,6,&active) == SPMI_OK && active == 4);
    assert(spmi_irq_ack(&a,&power) == SPMI_OK);
    assert(m.latched[0] == 0x65 && m.interrupts[PON_ID] == 0x45 && m.interrupts[GPIO_ID] == 1);
    assert(spmi_irq_mask(&a,&power) == SPMI_OK && m.regs[0][0x15] == 0x45 && m.acc[PON_ID] == 1);
    m.latched[0] |= 0x80;
    assert(spmi_irq_unmask(&a,&power) == SPMI_OK && m.latched[0] == 0xe5 && m.regs[0][0x15] == 0xc5);

    const uint8_t bytes[] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
    uint8_t result[8] = {0};
    assert(spmi_v7_write(&a,&up.peripheral,0xf8,bytes,8) == SPMI_OK && m.last_command == 0xf87);
    assert(spmi_v7_read(&a,&up.peripheral,0xf8,result,8) == SPMI_OK && !memcmp(result,bytes,8));
    assert(spmi_v7_write(&a,&up.peripheral,0xf9,bytes,8) == SPMI_INVALID);
    /* GPIO6: retain unrelated bits, restore the inherited state after stop
     * and unwind an incomplete D0. Model a PMIC hardware write failure. */
    struct spmi_gpio_input pad = {0};
    m.regs[1][4] = 0x10; m.regs[1][5] = 0x13;
    const uint8_t saved[] = {0xe1, 0xa3, 0x85, 0xc3, 0xb4, 0x31, 0x14};
    memcpy(&m.regs[1][0x40],saved,sizeof(saved));
    assert(spmi_gpio_input_start(&a,&up.peripheral,&pad,1,0) == SPMI_OK && pad.active);
    const uint8_t input[] = {0xe0, 0xa1, 0x80, 0x43, 0xb0, 0x31, 0x94};
    assert(!memcmp(input,&m.regs[1][0x40],sizeof(input)));
    m.regs[1][0x42] |= 0x20; /* Unrelated state is not rolled back. */
    assert(spmi_gpio_input_stop(&a,&up.peripheral,&pad) == SPMI_OK && !pad.active && !pad.dirty);
    assert(m.regs[1][0x42] == (saved[2] | 0x20));
    m.regs[1][0x42] = saved[2];
    assert(!memcmp(saved,&m.regs[1][0x40],sizeof(saved)));
    m.drop_register = 0x43; m.drop_once = 1;
    assert(spmi_gpio_input_start(&a,&up.peripheral,&pad,1,0) == SPMI_DROPPED);
    assert(!pad.dirty && !pad.active && !memcmp(saved,&m.regs[1][0x40],sizeof(saved)));
    m.result = 0; level = 22;
    assert(spmi_irq_level(&a,&power,&level) == SPMI_TIMEOUT && level == 22 && m.stalls == 1000);
    unsigned sent = m.submits;
    assert(spmi_irq_ack(&a,&down) == SPMI_BUSY && m.submits == sent);
    assert(spmi_v7_start(&a,&io,0,0) == SPMI_BUSY);
    m.status = 5;
    assert(spmi_v7_retire(&a) == SPMI_DENIED && !a.pending);
    m.result = 1;
    assert(spmi_irq_ack(&a,&down) == SPMI_OK && m.submits == sent+1 && m.interrupts[PON_ID] == 5);
    /* Bus 1 must never see bus 0's APIDs, even for an identical PPID. */
    assert(spmi_v7_start(&a,&io,1,0) == SPMI_OK && a.first == 896 && a.count == 20);
    assert(spmi_v7_resolve(&a,PON,&peripheral) == SPMI_NOT_FOUND);
    m.map[897] = IRQ | (PON << 8); m.owners[897] = 3;
    assert(spmi_v7_resolve(&a,PON,&peripheral) == SPMI_OK);
    assert(spmi_v7_write(&a,&peripheral,0x16,bytes,1) == SPMI_NOT_OWNER);
    m.version = 0x80000000;
    assert(spmi_v7_start(&a,&io,0,0) == SPMI_NOT_FOUND);
    puts("PON and PM8550 GPIO: APID ownership, masked edges, shared IRQs and 8-byte transactions passed.");
    return 0;
}
