/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "cmd_db.h"

enum rpmh_ufs_supply {
    RPMH_UFS_VCC, RPMH_UFS_VCCQ, RPMH_UFS_REFCLK, RPMH_UFS_VCCQ_PARENT,
    RPMH_UFS_PHY_QREF, RPMH_UFS_PHY_REFGEN, RPMH_UFS_PHY_ANALOG, RPMH_UFS_PHY_PLL, RPMH_UFS_SUPPLY_COUNT
};
enum rpmh_supply_load { RPMH_SUPPLY_OFF, RPMH_SUPPLY_LOW, RPMH_SUPPLY_HIGH };

/* A regulator consumer can leave its voltage vote unset (both bounds zero).
 * An enabled consumer may have a zero load; voltage constraints are separate. */
struct rpmh_supply_request { uint32_t ua; uint16_t min_mv, max_mv; uint8_t enable; };
/* Raw DRV2 active requests, in mV and PMIC5 mode encoding. A mask bit
 * means all three fields are known for this AP; it is not a rail status. */
struct rpmh_vrm_vote { uint16_t mv; uint8_t enable, mode; };
struct rpmh_vrm_votes { unsigned mask; struct rpmh_vrm_vote vote[RPMH_UFS_SUPPLY_COUNT]; };

struct rpmh_vrm {
    uint32_t address[RPMH_UFS_SUPPLY_COUNT];
    struct rpmh_vrm_votes floor;
    struct rpmh_vrm_vote applied[RPMH_UFS_SUPPLY_COUNT];
    unsigned restore, rollback, boot_holds, managed, deferred;
};

static inline unsigned rpmh_vrm_pending(const struct rpmh_vrm *v)
{
    return v->restore & v->floor.mask & ~v->deferred;
}

struct rpmh_provider;
struct rpmh_client;
int rpmh_vrm_init(struct rpmh_vrm *, const struct rpmh_cmd_db *);
int rpmh_vrm_adopt(struct rpmh_vrm *, const struct rpmh_vrm_votes *);
int rpmh_vrm_hold_boot(struct rpmh_provider *, unsigned mask);
int rpmh_vrm_finish_boot(struct rpmh_provider *, unsigned mask);
int rpmh_vrm_set_aux(struct rpmh_provider *, struct rpmh_client *, enum rpmh_ufs_supply,
                     const struct rpmh_supply_request *);
int rpmh_vrm_sync(struct rpmh_provider *, unsigned mask);
void rpmh_vrm_restore_demands(struct rpmh_provider *, unsigned mask);
