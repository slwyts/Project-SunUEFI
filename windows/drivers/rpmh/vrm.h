/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "cmd_db.h"

enum rpmh_ufs_supply { RPMH_UFS_VCC, RPMH_UFS_VCCQ, RPMH_UFS_SUPPLY_COUNT };
enum rpmh_supply_load { RPMH_SUPPLY_OFF, RPMH_SUPPLY_LOW, RPMH_SUPPLY_HIGH };

/* Raw DRV2 active requests, in mV and PMIC5 mode encoding. A mask bit
 * means all three fields are known for this AP; it is not a rail status. */
struct rpmh_vrm_vote { uint16_t mv; uint8_t enable, mode; };
struct rpmh_vrm_votes { unsigned mask; struct rpmh_vrm_vote vote[RPMH_UFS_SUPPLY_COUNT]; };

struct rpmh_vrm {
    uint32_t address[RPMH_UFS_SUPPLY_COUNT];
    struct rpmh_vrm_votes floor;
    struct rpmh_vrm_vote applied[RPMH_UFS_SUPPLY_COUNT];
    unsigned restore, rollback;
};

struct rpmh_provider;
int rpmh_vrm_init(struct rpmh_vrm *, const struct rpmh_cmd_db *);
int rpmh_vrm_adopt(struct rpmh_vrm *, const struct rpmh_vrm_votes *);
int rpmh_vrm_sync(struct rpmh_provider *, unsigned mask);
void rpmh_vrm_restore_demands(struct rpmh_provider *, unsigned mask);
