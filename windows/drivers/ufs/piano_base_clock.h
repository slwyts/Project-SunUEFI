/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

enum piano_ufs_base_reg {
    PIANO_AXI_CMD,
    PIANO_AXI_CFG,
    PIANO_UNIPRO_CMD,
    PIANO_UNIPRO_CFG,
    PIANO_ICE_CMD,
    PIANO_ICE_CFG,
    PIANO_AUX_CMD,
    PIANO_AUX_CFG,
    PIANO_AXI_CBCR,
    PIANO_AGGRE_CBCR,
    PIANO_AHB_CBCR,
    PIANO_UNIPRO_CBCR,
    PIANO_ICE_CBCR,
    PIANO_AUX_CBCR,
    PIANO_QREF,
    PIANO_UFS_BASE_REGS
};
extern const uint32_t piano_ufs_base_address[PIANO_UFS_BASE_REGS];
enum piano_ufs_pll { PIANO_GPLL0, PIANO_GPLL4, PIANO_GPLL0_EVEN, PIANO_UFS_PLLS };
struct piano_ufs_base_io {
    void *context;
    uint32_t (*read)(void *, enum piano_ufs_base_reg);
    void (*write)(void *, enum piano_ufs_base_reg, uint32_t);
    void (*wait_us)(void *, unsigned);
    /* Per-root, per-parent reference, serialized by GCC. A failed enable
     * can be released even if its vote or acknowledgement is uncertain. */
    int (*parent)(void *, unsigned root, enum piano_ufs_pll, int enable);
    int (*device_reference)(void *, int enable);
};
struct piano_ufs_clock_root {
    uint32_t saved;
    unsigned parents, acquired, changed, force;
};
struct piano_ufs_base_clocks {
    struct piano_ufs_base_io io;
    struct piano_ufs_clock_root root[4];
    unsigned gates, inherited, reference, bus, phy, bus_pending, phy_pending;
};

/* The owning board has powered the UFS domains before accessing these
 * resources; suspend, resume and PnP teardown are serialized at PASSIVE. */
int piano_ufs_bus_clocks(struct piano_ufs_base_clocks *, int enable);
int piano_ufs_phy_clocks(struct piano_ufs_base_clocks *, int enable);
/* Successful storage initialization transfers its inherited branch enables
 * to this adapter; failed warm enumeration leaves them alone. */
void piano_ufs_claim_clocks(struct piano_ufs_base_clocks *);
