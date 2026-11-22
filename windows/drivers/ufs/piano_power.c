/* SPDX-License-Identifier: BSD-2-Clause */
#include "piano_power.h"

/* SM8750 GCC uses the configuration register following each GDSCR for
 * completion. The HBA's application-processor vote is not its global state.
 */
const uint32_t piano_ufs_domain_address[PIANO_UFS_DOMAINS] = { 0x00177004, 0x0019e000 };
enum {
    GDSCR = 0, CFG_GDSCR = 4,
    COLLAPSE = 1, HW_CONTROL = 2, SW_OVERRIDE = 4, RETAIN_FF = 1u << 11,
    DOWN_COMPLETE = 1u << 15, UP_COMPLETE = 1u << 16,
    TRANSITION_TIMING = 0xfffu << 12, POWER_TIMEOUT_US = 2000, VOTE_RELEASE_US = 500
};

static int update(struct piano_ufs_bus_power *p, enum piano_ufs_domain d, uint32_t mask, uint32_t value)
{
    uint32_t v = p->io.read(p->io.context, d, GDSCR);
    if (v == UINT32_MAX || (v & (HW_CONTROL | SW_OVERRIDE))) return 0;
    if ((v & mask) == value) return 1;
    p->io.write_control(p->io.context, d, (v & ~mask) | value);
    v = p->io.read(p->io.context, d, GDSCR);
    return v != UINT32_MAX && (v & mask) == value;
}

static int wait_state(struct piano_ufs_bus_power *p, enum piano_ufs_domain d, uint32_t bit)
{
    for (unsigned elapsed = 0;; elapsed += 5) {
        uint32_t status = p->io.read(p->io.context, d, CFG_GDSCR);
        if (status == UINT32_MAX) return 0;
        if (status & bit) return 1;
        if (elapsed == POWER_TIMEOUT_US) return 0;
        p->io.wait_us(p->io.context, 5);
    }
}

static int power_up(struct piano_ufs_bus_power *p, enum piano_ufs_domain d)
{
    uint32_t old = p->io.read(p->io.context, d, GDSCR);
    if (old == UINT32_MAX || (old & (HW_CONTROL | SW_OVERRIDE))) return 0;
    if (old & COLLAPSE) {
        /* A posted vote may have reached hardware even if its readback fails. */
        p->drop_mask |= 1u << d;
        uint32_t timing = (2u << 20) | (2u << 16) | ((d == PIANO_UFS_HBA_DOMAIN ? 15u : 2u) << 12);
        if (!update(p, d, COLLAPSE | TRANSITION_TIMING, timing)) return 0;
    }
    if (!wait_state(p, d, UP_COMPLETE)) return 0;
    /* Give the domain clocks four XO cycles to restart before retention. */
    p->io.wait_us(p->io.context, 1);
    /* Discovery can still reject the inherited controller. Its retention
     * and timing settings belong to the previous user until PnP claims it.
     */
    if (((old & COLLAPSE) || p->owns_inherited) && !update(p, d, RETAIN_FF, RETAIN_FF)) return 0;
    if (p->owns_inherited) p->drop_mask |= 1u << d;
    return 1;
}

static int retain_domain(struct piano_ufs_bus_power *p, enum piano_ufs_domain d)
{
    if (!(p->drop_mask & (1u << d))) return 1;
    uint32_t ctrl = p->io.read(p->io.context, d, GDSCR);
    if (ctrl == UINT32_MAX || (ctrl & (HW_CONTROL | SW_OVERRIDE))) return 0;
    if (ctrl & (RETAIN_FF | COLLAPSE)) return 1;
    uint32_t status = p->io.read(p->io.context, d, CFG_GDSCR);
    if (status == UINT32_MAX) return 0;
    if (!(status & UP_COMPLETE)) {
        /* A failed power-up request may never have reached the running state. */
        if (!p->ready && !p->owns_inherited) return 1;
        if (!wait_state(p, d, UP_COMPLETE)) return 0;
    }
    p->io.wait_us(p->io.context, 1);
    return update(p, d, RETAIN_FF, RETAIN_FF);
}

static int collapse_domain(struct piano_ufs_bus_power *p, enum piano_ufs_domain d)
{
    if (!(p->drop_mask & (1u << d))) return 1;
    if (!update(p, d, COLLAPSE, COLLAPSE)) return 0;
    if (d == PIANO_UFS_HBA_DOMAIN) {
        /* Other masters can keep this votable domain on. */
        p->io.wait_us(p->io.context, VOTE_RELEASE_US);
    } else if (!wait_state(p, d, DOWN_COMPLETE)) return 0;
    p->drop_mask &= ~(1u << d);
    return 1;
}

int piano_ufs_collapse_phy(struct piano_ufs_bus_power *p)
{
    /* The PHY power-control sequence has already put both lanes into
     * retention. Do not drop a domain still borrowed from the firmware. */
    if (!p || !retain_domain(p,PIANO_UFS_PHY_DOMAIN)) return 0;
    p->ready = 0;
    return collapse_domain(p,PIANO_UFS_PHY_DOMAIN);
}

static int release_power(struct piano_ufs_bus_power *p)
{
    for (unsigned i = 0; i < PIANO_UFS_DOMAINS; ++i)
        if (!retain_domain(p,(enum piano_ufs_domain)i)) return 0;
    p->ready = 0;
    if (p->clocks_held) {
        if (!p->inputs.base_clocks(p->inputs.context, 0)) return 0;
        p->clocks_held = 0;
    }
    for (unsigned n = PIANO_UFS_DOMAINS; n; --n) {
        if (!collapse_domain(p,(enum piano_ufs_domain)(n-1))) return 0;
    }
    if (p->inputs_held) {
        if (!p->inputs.rails_and_paths(p->inputs.context, 0)) return 0;
        p->inputs_held = 0;
    }
    return 1;
}

int piano_ufs_set_bus_power(void *context, int enable)
{
    struct piano_ufs_bus_power *p = context;
    if (!p || !p->io.read || !p->io.write_control || !p->io.wait_us ||
        !p->inputs.rails_and_paths || !p->inputs.base_clocks) return 0;
    if (!enable) return release_power(p);
    if (p->ready) return 1;
    if (!release_power(p) || !p->inputs.rails_and_paths(p->inputs.context, 1)) return 0;
    p->inputs_held = 1;
    for (unsigned d = 0; d < PIANO_UFS_DOMAINS; ++d)
        if (!power_up(p, (enum piano_ufs_domain)d)) goto undo;
    /* Even a failed clock enable may still have a posted RCG update.
     * Require a successful disable before dropping domains or RPMh votes. */
    p->clocks_held = 1;
    if (!p->inputs.base_clocks(p->inputs.context, 1)) goto undo;
    p->ready = 1;
    return 1;
undo:
    (void)release_power(p);
    return 0;
}

void piano_ufs_claim_domains(struct piano_ufs_bus_power *p)
{
    if (p && p->ready) {
        p->owns_inherited = 1;
        p->drop_mask = (1u << PIANO_UFS_DOMAINS) - 1;
    }
}
