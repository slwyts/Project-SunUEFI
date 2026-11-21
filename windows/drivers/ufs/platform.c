/* SPDX-License-Identifier: BSD-2-Clause */
#include "platform.h"
#include <stddef.h>

enum {
    HCI_VERSION = 0x08, HCI_STATUS = 0x30, HCI_ENABLE = 0x34,
    TRANSFER_DOORBELL = 0x58, TRANSFER_RUN = 0x60, TASK_DOORBELL = 0x78, TASK_RUN = 0x80,
    HOST_CFG0 = 0xd8, HOST_CFG1 = 0xdc, HOST_REVISION = 0xe4, MEMORY_CONFIG = 0x300,
    DEVICE_PRESENT = 1, QUNIPRO = 1, PHY_SOFT_RESET = 2, QUNIPRO_G4 = 0x20,
    PCS_START = 0x400, PCS_POWER = 0x404, PCS_SW_RESET = 0x408, PCS_READY = 0x5a8,
    PCS_MULTI_LANE = 0x5fc, LANE0 = 0x1000, LANE1 = 0x1800,
    RX_INTERFACE_MODE = 0x3f0, RX_CLOCK_EDGE = 1u << 6,
    CORE_PULSE_US = 200, RESET_SETTLE_US = 1000, PCS_TIMEOUT_US = 10000
};

struct phy_setting { uint16_t offset; uint8_t value; };

/* Piano XBL: ufs0/ufs_phy_cfg, 38.4 MHz, 2 lanes, HS-G5 rate B. The
 * board's second lane uses the same TX/RX calibration at its own base.
 */
static const struct phy_setting serdes[] = {
    {0x110, 0xd9}, {0x174, 0x16}, {0x03c, 0x11}, {0x09c, 0x00},
    {0x120, 0x01}, {0x124, 0x60}, {0x0f4, 0x1f}, {0x0f8, 0x1f},
    {0x0fc, 0x07}, {0x100, 0x20}, {0x140, 0x04}, {0x13c, 0x40},
    {0x268, 0x06}, {0x088, 0x41}, {0x070, 0x06}, {0x074, 0x18},
    {0x078, 0x14}, {0x26c, 0x06}, {0x270, 0x18}, {0x274, 0x14},
    {0x080, 0x7f}, {0x084, 0x06}, {0x058, 0x92}, {0x05c, 0x1e},
    {0x028, 0x4c}, {0x010, 0x06}, {0x014, 0x18}, {0x018, 0x14},
    {0x278, 0x06}, {0x27c, 0x18}, {0x280, 0x14}, {0x020, 0x99},
    {0x024, 0x07}, {0x050, 0xbe}, {0x054, 0x23},
};

static const struct phy_setting lane[] = {
    {0x07c, 0x00}, {0x030, 0x07}, {0x034, 0x17}, {0x2d4, 0x0c},
    {0x2dc, 0x0c}, {0x2f0, 0x04}, {0x3cc, 0x14}, {0x2f4, 0x07},
    {0x3d4, 0x0e}, {0x254, 0x02}, {0x210, 0x1c}, {0x224, 0x06},
    {0x378, 0x8e}, {0x3b4, 0x0f}, {0x418, 0xce}, {0x41c, 0xce},
    {0x420, 0x18}, {0x424, 0x1a}, {0x428, 0x0f}, {0x430, 0x60},
    {0x434, 0x62}, {0x448, 0x9a}, {0x454, 0xe2}, {0x458, 0x06},
    {0x460, 0x1b}, {0x464, 0x1b}, {0x468, 0x98}, {0x46c, 0x9b},
    {0x470, 0x2a}, {0x474, 0x12}, {0x47c, 0x06}, {0x480, 0x01},
    {0x484, 0x93}, {0x488, 0x93}, {0x48c, 0x60}, {0x490, 0x99},
    {0x494, 0x5f}, {0x498, 0x92}, {0x49c, 0xe3}, {0x4a0, 0x06},
    {0x4a8, 0x9b}, {0x4ac, 0x9b}, {0x4b0, 0x60}, {0x4b4, 0x99},
    {0x4b8, 0x5f}, {0x4bc, 0x92}, {0x4c0, 0xfb}, {0x4c4, 0x06},
    {0x228, 0x1f}, {0x258, 0x94}, {0x2c4, 0xfa}, {0x548, 0x30},
    {0x580, 0x77},
};

static const struct phy_setting pcs[] = {
    {0x5f4, 0x43}, {0x420, 0x41}, {0x42c, 0x33}, {0x474, 0x05},
    {0x4bc, 0x05}, {0x430, 0x0f}, {0x58c, 0x68}, {0x52c, 0x4d},
    {0x620, 0x9e}, {0x640, 0x0e}, {0x644, 0x12}, {0x648, 0x15},
    {0x64c, 0x19}, {0x550, 0x3f}, {0x554, 0x18},
};

static uint32_t read_reg(const struct ufs_platform *p, enum ufs_platform_window w, unsigned reg)
{
    return p->io.read(p->io.context, w, reg);
}

static int set_bits(struct ufs_platform *p, enum ufs_platform_window w, unsigned reg,
                    uint32_t mask, uint32_t value)
{
    uint32_t old = read_reg(p, w, reg);
    if (old == UINT32_MAX) return 0;
    p->io.write(p->io.context, w, reg, (old & ~mask) | value);
    uint32_t actual = read_reg(p, w, reg);
    return actual != UINT32_MAX && (actual & mask) == value;
}

static void program(struct ufs_platform *p, unsigned base, const struct phy_setting *settings, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        p->io.write(p->io.context, UFS_PHY, base + settings[i].offset, settings[i].value);
}

static int queues_idle(const struct ufs_platform *p)
{
    return !(read_reg(p, UFS_HOST, MEMORY_CONFIG) & 1) &&
        !read_reg(p, UFS_HOST, TRANSFER_RUN) && !read_reg(p, UFS_HOST, TASK_RUN) &&
        !read_reg(p, UFS_HOST, TRANSFER_DOORBELL) && !read_reg(p, UFS_HOST, TASK_DOORBELL);
}

static int host_stopped(const struct ufs_platform *p)
{
    return !(read_reg(p, UFS_HOST, HCI_ENABLE) & 1) && queues_idle(p);
}

static enum ufs_platform_error fail(struct ufs_platform *p, enum ufs_platform_error error)
{
    p->phase = UFS_PLATFORM_FAILED;
    p->error = error;
    return error;
}

static void advance(struct ufs_platform *p, enum ufs_platform_phase phase, uint64_t now)
{
    p->phase = phase;
    p->since_us = now;
}

static void enable_lanes(struct ufs_platform *p)
{
    if (p->power.set_lane_clocks(p->power.context, 1)) {
        p->lane_vote = 1;
        p->phase = UFS_PLATFORM_READY;
    } else {
        fail(p, UFS_PLATFORM_POWER);
    }
}

static int phy_supply(struct ufs_platform *p, enum ufs_phy_supply supply, int enable)
{
    unsigned bit = 1u << supply;
    if (!!(p->phy_supplies & bit) == enable) return 1;
    if (!p->power.phy.set_supply(p->power.phy.context,supply,enable)) return 0;
    p->phy_supplies ^= bit;
    return 1;
}

static int phy_pll_clocks(struct ufs_platform *p)
{
    if (!phy_supply(p,UFS_PHY_PLL,1) || !p->power.phy.set_clocks(p->power.phy.context,1)) return 0;
    p->phy_clocks = 1;
    return 1;
}

static int analog_power(struct ufs_platform *p, int enable)
{
    /* SM8750 uses the RX interface clock-edge bit on both lanes to retain
     * the link state through analog power collapse. Each RMW is ordered
     * and read back before operating the next register or power domain. */
    if (!enable && !set_bits(p,UFS_PHY,PCS_POWER,1,0)) return 0;
    for (unsigned i = 0; i < 2; ++i) {
        unsigned reg = (i ? LANE1 : LANE0) + RX_INTERFACE_MODE;
        uint32_t current = read_reg(p,UFS_PHY,reg);
        if (current == UINT32_MAX) return 0;
        if (!!(current & RX_CLOCK_EDGE) == enable &&
            !set_bits(p,UFS_PHY,reg,RX_CLOCK_EDGE,enable ? 0 : RX_CLOCK_EDGE)) return 0;
    }
    return !enable || set_bits(p,UFS_PHY,PCS_POWER,1,1);
}

enum ufs_platform_error ufs_platform_begin(struct ufs_platform *p, const struct ufs_platform_io *io,
                                          const struct ufs_platform_power *power, uint64_t now)
{
    if (!p || !io || !io->read || !io->write || !power || !power->set_lane_clocks ||
        !power->phy.set_supply || !power->phy.set_clocks || !power->phy.release_domain)
        return UFS_PLATFORM_RESOURCES;
    if (p->phase != UFS_PLATFORM_IDLE) return UFS_PLATFORM_IN_USE;
    *p = (struct ufs_platform){ .io = *io, .power = *power };
    uint32_t version = read_reg(p, UFS_HOST, HCI_VERSION);
    if (version < 0x400 || version == UINT32_MAX || read_reg(p, UFS_HOST, HOST_REVISION) >> 28 != 6)
        return fail(p, UFS_PLATFORM_HARDWARE);
    if (!queues_idle(p)) return fail(p, UFS_PLATFORM_IN_USE);

    uint32_t hce = read_reg(p, UFS_HOST, HCI_ENABLE), hcs = read_reg(p, UFS_HOST, HCI_STATUS);
    uint32_t cfg0 = read_reg(p, UFS_HOST, HOST_CFG0), cfg1 = read_reg(p, UFS_HOST, HOST_CFG1);
    if (hce == UINT32_MAX || hcs == UINT32_MAX || cfg0 == UINT32_MAX || cfg1 == UINT32_MAX)
        return fail(p, UFS_PLATFORM_HARDWARE);
    if (!phy_supply(p,UFS_PHY_QREF,1) || !phy_supply(p,UFS_PHY_REFGEN,1) || !phy_supply(p,UFS_PHY_ANALOG,1))
        return fail(p, UFS_PLATFORM_POWER);
    uint32_t start = read_reg(p, UFS_PHY, PCS_START), powered = read_reg(p, UFS_PHY, PCS_POWER);
    uint32_t reset = read_reg(p, UFS_PHY, PCS_SW_RESET), ready = read_reg(p, UFS_PHY, PCS_READY);
    if (start == UINT32_MAX || powered == UINT32_MAX || reset == UINT32_MAX || ready == UINT32_MAX)
        return fail(p, UFS_PLATFORM_HARDWARE);
    if ((start & powered & ready & 1) && !(reset & 1) && !(cfg0 & QUNIPRO_G4) &&
        (cfg1 & (QUNIPRO | PHY_SOFT_RESET)) == QUNIPRO) {
        if (!phy_pll_clocks(p)) return fail(p, UFS_PLATFORM_POWER);
        enable_lanes(p);
        return p->error;
    }
    if ((hce & 1) || (hcs & DEVICE_PRESENT)) return fail(p, UFS_PLATFORM_IN_USE);
    if (!power->set_core_reset || !power->set_core_reset(power->context, 1))
        return fail(p, UFS_PLATFORM_POWER);
    p->core_asserted = 1;
    advance(p, UFS_CORE_RESET, now);
    return UFS_PLATFORM_OK;
}

enum ufs_platform_phase ufs_platform_poll(struct ufs_platform *p, uint64_t now)
{
    if (!p) return UFS_PLATFORM_FAILED;
    if (p->stopping) return UFS_PLATFORM_STOPPING;
    uint64_t elapsed = now - p->since_us;
    switch (p->phase) {
    case UFS_CORE_RESET:
        if (elapsed < CORE_PULSE_US) break;
        if (!p->power.set_core_reset(p->power.context, 0)) {
            fail(p, UFS_PLATFORM_POWER);
            break;
        }
        p->core_asserted = 0;
        p->core_settling = 1;
        advance(p, UFS_CORE_SETTLE, now);
        break;
    case UFS_CORE_SETTLE:
        if (elapsed < RESET_SETTLE_US) break;
        p->core_settling = 0;
        if (!host_stopped(p) || (read_reg(p, UFS_HOST, HCI_STATUS) & DEVICE_PRESENT)) {
            fail(p, UFS_PLATFORM_IN_USE);
            break;
        }
        p->owns_pcs = 1;
        if (!analog_power(p,1)) {
            fail(p, UFS_PLATFORM_HARDWARE);
            break;
        }
        if (!phy_pll_clocks(p)) {
            fail(p, UFS_PLATFORM_POWER);
            break;
        }
        p->phy_asserted = 1;
        advance(p, UFS_PHY_RESET, now);
        if (!set_bits(p, UFS_HOST, HOST_CFG1, PHY_SOFT_RESET, PHY_SOFT_RESET)) {
            fail(p, UFS_PLATFORM_HARDWARE);
            break;
        }
        break;
    case UFS_PHY_RESET:
        if (elapsed < RESET_SETTLE_US) break;
        if (!set_bits(p, UFS_PHY, PCS_SW_RESET, 1, 1)) {
            fail(p, UFS_PLATFORM_HARDWARE);
            break;
        }
        p->io.write(p->io.context, UFS_PHY, PCS_MULTI_LANE, 0x0a);
        program(p, 0, serdes, sizeof(serdes) / sizeof(*serdes));
        program(p, LANE0, lane, sizeof(lane) / sizeof(*lane));
        program(p, LANE1, lane, sizeof(lane) / sizeof(*lane));
        p->io.write(p->io.context, UFS_PHY, PCS_MULTI_LANE, 2);
        program(p, 0, pcs, sizeof(pcs) / sizeof(*pcs));
        if (!set_bits(p, UFS_HOST, HOST_CFG1, PHY_SOFT_RESET, 0)) {
            fail(p, UFS_PLATFORM_HARDWARE);
            break;
        }
        p->phy_asserted = 0;
        advance(p, UFS_PHY_SETTLE, now);
        break;
    case UFS_PHY_SETTLE:
        if (elapsed < RESET_SETTLE_US) break;
        if (!set_bits(p, UFS_PHY, PCS_SW_RESET, 1, 0) || !set_bits(p, UFS_PHY, PCS_START, 1, 1)) {
            fail(p, UFS_PLATFORM_HARDWARE);
            break;
        }
        advance(p, UFS_PHY_START, now);
        break;
    case UFS_PHY_START: {
        uint32_t ready = read_reg(p, UFS_PHY, PCS_READY);
        if (ready == UINT32_MAX) fail(p, UFS_PLATFORM_HARDWARE);
        else if (ready & 1) {
            if (!set_bits(p, UFS_HOST, HOST_CFG1, QUNIPRO, QUNIPRO) ||
                !set_bits(p, UFS_HOST, HOST_CFG0, QUNIPRO_G4, 0))
                fail(p, UFS_PLATFORM_HARDWARE);
            else
                enable_lanes(p);
        } else if (elapsed >= PCS_TIMEOUT_US) {
            fail(p, UFS_PLATFORM_TIMEOUT);
        }
        break;
    }
    default:
        break;
    }
    return p->phase;
}

void ufs_platform_claim_phy(struct ufs_platform *p)
{
    if (p && p->phase == UFS_PLATFORM_READY && !p->stopping) p->owns_pcs = 1;
}

int ufs_platform_stop(struct ufs_platform *p, uint64_t now)
{
    if (!p) return 0;
    p->stopping = 1;
    if (p->core_asserted) {
        if (now - p->since_us < CORE_PULSE_US || !p->power.set_core_reset(p->power.context, 0)) return 0;
        p->core_asserted = 0;
        p->core_settling = 1;
        advance(p, UFS_CORE_SETTLE, now);
        return 0;
    }
    if (p->core_settling && now - p->since_us < RESET_SETTLE_US) return 0;
    if ((p->owns_pcs || p->lane_vote) && !host_stopped(p)) return 0;
    if (p->phy_asserted && now - p->since_us < RESET_SETTLE_US) return 0;
    if (p->lane_vote) {
        if (!p->power.set_lane_clocks(p->power.context, 0)) return 0;
        p->lane_vote = 0;
    }
    if (p->owns_pcs) {
        if (p->phy_asserted && !set_bits(p, UFS_HOST, HOST_CFG1, PHY_SOFT_RESET, 0)) return 0;
        p->phy_asserted = 0;
        if (!analog_power(p,0)) return 0;
        p->owns_pcs = 0;
        p->release_phy_domain = 1;
    }
    if (p->release_phy_domain) {
        if (!p->power.phy.release_domain(p->power.phy.context)) return 0;
        p->release_phy_domain = 0;
    }
    if (p->phy_clocks) {
        if (!p->power.phy.set_clocks(p->power.phy.context,0)) return 0;
        p->phy_clocks = 0;
    }
    if (!phy_supply(p,UFS_PHY_PLL,0) || !phy_supply(p,UFS_PHY_ANALOG,0) ||
        !phy_supply(p,UFS_PHY_QREF,0) || !phy_supply(p,UFS_PHY_REFGEN,0)) return 0;
    *p = (struct ufs_platform){0};
    return 1;
}
