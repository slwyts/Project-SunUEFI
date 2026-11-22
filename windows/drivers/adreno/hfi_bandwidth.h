/* SPDX-License-Identifier: MIT */
#pragma once
#include "../rpmh/fabric.h"

/* Gen8 HFI takes a fixed-size table, independent of the number of GPU OPPs.
 * The platform supplies its ordered DDR levels (including off at index zero)
 * and BCM resources resolved from the booting machine's AOP command DB. */
struct adreno_bw_config {
    const struct rpmh_fabric *fabric;
    const uint32_t *kbps;
    size_t count;
    uint32_t perfmode_kbps;
};

enum { ADRENO_HFI_DDR_LEVELS = 16 };
struct adreno_bw_table {
    uint32_t levels, cnoc_count, ddr_count, cnoc_wait, ddr_wait;
    uint32_t cnoc_addr[6], cnoc_data[2][6], ddr_addr[8], ddr_data[ADRENO_HFI_DDR_LEVELS][8];
};

int adreno_hfi_bandwidth_table(const struct adreno_bw_config *config, struct adreno_bw_table *table);
