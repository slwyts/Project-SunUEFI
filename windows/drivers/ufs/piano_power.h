/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

enum piano_ufs_domain { PIANO_UFS_HBA_DOMAIN, PIANO_UFS_PHY_DOMAIN, PIANO_UFS_DOMAINS };
extern const uint32_t piano_ufs_domain_address[PIANO_UFS_DOMAINS];

struct piano_ufs_domain_io {
    void *context;
    uint32_t (*read)(void *, enum piano_ufs_domain, unsigned offset);
    void (*write_control)(void *, enum piano_ufs_domain, uint32_t);
    void (*wait_us)(void *, unsigned);
};

/* Each operation takes/releases the UFS client's shared votes. On error
 * its vote is unchanged. Inputs include the parent domains, rail and NoC
 * votes; base_clocks excludes the three PHY symbol clocks.
 */
struct piano_ufs_bus_inputs {
    void *context;
    int (*rails_and_paths)(void *, int enable);
    int (*base_clocks)(void *, int enable);
};

struct piano_ufs_bus_power {
    struct piano_ufs_domain_io io;
    struct piano_ufs_bus_inputs inputs;
    unsigned drop_mask, inputs_held, clocks_held, ready, owns_inherited;
};

/* Serialized at PASSIVE_LEVEL. A failed acquisition unwinds its own votes;
 * keep the mappings while a release is outstanding. Once PnP discovery has
 * claimed the idle controller, its inherited AP domain votes may be dropped
 * at the next normal power-down. Never claim another OS driver's registers.
 */
int piano_ufs_set_bus_power(void *context, int enable);
void piano_ufs_claim_domains(struct piano_ufs_bus_power *power);
