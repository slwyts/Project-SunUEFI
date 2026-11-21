/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "fabric.h"
#include "vrm.h"

enum rpmh_rail { RPMH_XO, RPMH_CX, RPMH_MX, RPMH_RAIL_COUNT };

struct rpmh_transport {
    void *context;
    /* The wait mask controls ordering inside one TCS, not which messages
     * request a response. Retire the old request before sending a new one. */
    int (*write)(void *, const struct rpmh_rsc_command *, unsigned count, unsigned wait_mask);
};

/* Values requested by the boot-time AP clients, not a measurement of the
 * physical rail or of votes aggregated from the AP, modem, DSP and GPU.
 * The platform can omit resources whose previous votes are not known. */
struct rpmh_boot_votes {
    unsigned rails;
    uint32_t corner[RPMH_RAIL_COUNT];
    struct rpmh_fabric_votes bus;
};

struct rpmh_client {
    struct rpmh_client *next;
    uint32_t corner[RPMH_RAIL_COUNT];
    uint32_t avg[RPMH_FABRIC_PATH_COUNT], peak[RPMH_FABRIC_PATH_COUNT];
    enum rpmh_supply_load ufs[2]; /* Device VCC and VCCQ, in the UFS core's load modes. */
    struct rpmh_supply_request aux[RPMH_UFS_SUPPLY_COUNT - RPMH_UFS_REFCLK];
};

struct rpmh_provider {
    struct rpmh_transport transport;
    struct rpmh_client *clients;
    struct rpmh_db_entry rail[RPMH_RAIL_COUNT];
    struct rpmh_fabric fabric;
    struct rpmh_vrm vrm;
    struct rpmh_boot_votes floor, applied;
    unsigned slots, adopted, touched_paths, restore_rails, restore_bcms, boot_rails, boot_bcms;
};

/* The caller holds one passive lock for this provider and all its clients. */
int rpmh_provider_start(struct rpmh_provider *, const struct rpmh_cmd_db *, const struct rpmh_transport *,
                        unsigned slots);
int rpmh_provider_adopt(struct rpmh_provider *, const struct rpmh_boot_votes *);
/* Establish *new* OS requests for unobserved resources after firmware has
 * relinquished this RSC. The platform, not a single child, releases a
 * resource's boot hold after every user of that resource is accounted for. */
int rpmh_provider_boot(struct rpmh_provider *, unsigned rail_mask, unsigned path_mask,
                       unsigned supply_mask, int hold);
/* Preserve accepted requests across an unclean power transition. The old
 * boot votes and acknowledgements are no longer evidence about hardware. */
void rpmh_provider_power_lost(struct rpmh_provider *);
int rpmh_client_open(struct rpmh_provider *, struct rpmh_client *);
int rpmh_client_close(struct rpmh_provider *, struct rpmh_client *);
/* XO is a boolean. CX/MX take the firmware's performance level, not a voltage
 * or an index. The command DB translates that level to the actual corner. */
int rpmh_set_level(struct rpmh_provider *, struct rpmh_client *, enum rpmh_rail, uint32_t level);
int rpmh_set_bandwidth(struct rpmh_provider *, struct rpmh_client *, enum rpmh_fabric_path,
                       uint32_t avg_kBps, uint32_t peak_kBps);
/* Active AP votes only. The caller has quiesced the storage device before
 * requesting either rail off; a stopped HBA alone does not prove that. */
int rpmh_set_ufs_supplies(struct rpmh_provider *, struct rpmh_client *,
                          enum rpmh_supply_load vcc, enum rpmh_supply_load vccq);
int rpmh_set_aux_supply(struct rpmh_provider *, struct rpmh_client *, enum rpmh_ufs_supply,
                        const struct rpmh_supply_request *);
/* Restore the last accepted client demands after an unknown completion. */
int rpmh_provider_can_reconcile(const struct rpmh_provider *);
int rpmh_provider_reconcile(struct rpmh_provider *);
int rpmh_provider_quiet(struct rpmh_provider *);
