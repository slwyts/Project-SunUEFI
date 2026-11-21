/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

enum ufs_platform_window { UFS_HOST, UFS_PHY };

struct ufs_platform_io {
    void *context;
    uint32_t (*read)(void *, enum ufs_platform_window, unsigned);
    void (*write)(void *, enum ufs_platform_window, unsigned, uint32_t);
};

enum ufs_phy_supply { UFS_PHY_QREF, UFS_PHY_REFGEN, UFS_PHY_ANALOG, UFS_PHY_PLL, UFS_PHY_SUPPLY_COUNT };
struct ufs_phy_power {
    void *context;
    int (*set_supply)(void *, enum ufs_phy_supply, int enable);
    int (*set_clocks)(void *, int enable);
    int (*release_domain)(void *);
};

/* The provider holds real votes, not snapshots of clock register values.
 * set_bus_power prepares both domains, the HBA rails and its bus clocks.
 * PHY analog supplies and clocks follow PCS sequencing separately.
 * A successful enable owns one vote;
 * its matching disable releases only that vote. On failure ownership is
 * unchanged. set_core_reset operates the host BCR, not the PHY soft reset.
 */
struct ufs_platform_power {
    void *context;
    int (*set_bus_power)(void *, int enable);
    int (*set_lane_clocks)(void *, int enable);
    int (*set_core_reset)(void *, int assert_reset);
    struct ufs_phy_power phy;
};

enum ufs_platform_phase {
    UFS_PLATFORM_IDLE, UFS_CORE_RESET, UFS_CORE_SETTLE, UFS_PHY_RESET,
    UFS_PHY_SETTLE, UFS_PHY_START, UFS_PLATFORM_READY, UFS_PLATFORM_FAILED, UFS_PLATFORM_STOPPING
};
enum ufs_platform_error {
    UFS_PLATFORM_OK, UFS_PLATFORM_RESOURCES, UFS_PLATFORM_IN_USE,
    UFS_PLATFORM_HARDWARE, UFS_PLATFORM_POWER, UFS_PLATFORM_TIMEOUT
};

struct ufs_platform {
    struct ufs_platform_io io;
    struct ufs_platform_power power;
    enum ufs_platform_phase phase;
    enum ufs_platform_error error;
    uint64_t since_us;
    unsigned core_asserted, core_settling, phy_asserted, owns_pcs, lane_vote, stopping;
    unsigned phy_supplies, phy_clocks, release_phy_domain;
};

/* Serialize at PASSIVE_LEVEL with PnP, power, and storage I/O. The caller
 * holds the bus power vote before the first read and until Stop succeeds.
 * An initialized, idle controller is adopted without resetting its link.
 * A zero-initialized structure is required on first use.
 */
enum ufs_platform_error ufs_platform_begin(struct ufs_platform *p, const struct ufs_platform_io *io,
                                          const struct ufs_platform_power *power, uint64_t now_us);
enum ufs_platform_phase ufs_platform_poll(struct ufs_platform *p, uint64_t now_us);
/* The successfully initialized storage device now owns the handed-over PCS. */
void ufs_platform_claim_phy(struct ufs_platform *);
/* The storage layer flushes media, stops DMA and disables HCE before D3.
 * If a reset pulse is in progress, retain resources and retry with a later
 * timestamp. The powered-down path never writes a borrowed calibration.
 */
int ufs_platform_stop(struct ufs_platform *p, uint64_t now_us);
