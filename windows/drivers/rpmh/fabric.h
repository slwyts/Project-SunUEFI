/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "cmd_db.h"
#include "rsc.h"

enum rpmh_fabric_path {
    RPMH_UFS_DDR, RPMH_CPU_UFS, RPMH_QUP1_CORE, RPMH_CPU_QUP1, RPMH_QUP1_DDR,
    RPMH_FABRIC_PATH_COUNT
};
#define RPMH_FABRIC_ALL_PATHS ((1u << RPMH_FABRIC_PATH_COUNT) - 1u)
enum rpmh_bcm_id {
    RPMH_BCM_ACV, RPMH_BCM_CN0, RPMH_BCM_CN1, RPMH_BCM_MC0, RPMH_BCM_QUP1,
    RPMH_BCM_SH0, RPMH_BCM_SH1, RPMH_BCM_SN0, RPMH_BCM_SN2, RPMH_FABRIC_BCM_COUNT
};

struct rpmh_bcm {
    uint32_t address, unit;
    uint16_t width;
    uint8_t vcd;
};
struct rpmh_fabric {
    struct rpmh_bcm bcm[RPMH_FABRIC_BCM_COUNT];
    unsigned paths;
};
struct rpmh_fabric_request {
    enum rpmh_fabric_path path;
    uint32_t avg_kBps, peak_kBps; /* Decimal kilobytes per second (Linux ICC units). */
    unsigned perf_mode; /* Explicit performance tag; normal I/O leaves this zero. */
};
struct rpmh_fabric_load {
    uint64_t avg_kBps[RPMH_FABRIC_PATH_COUNT];
    uint32_t peak_kBps[RPMH_FABRIC_PATH_COUNT];
    unsigned performance;
};
struct rpmh_fabric_votes {
    unsigned mask; /* One bit for each known BCM vote. */
    uint32_t xy[RPMH_FABRIC_BCM_COUNT]; /* X[27:14], Y[13:0], without command flags. */
};
struct rpmh_fabric_batch {
    struct rpmh_rsc_command cmd[RPMH_FABRIC_BCM_COUNT];
    unsigned id[RPMH_FABRIC_BCM_COUNT], end[RPMH_FABRIC_BCM_COUNT], count, groups;
};

/* Returns a bitmask of routes whose complete set of BCMs was found. */
unsigned rpmh_fabric_init(struct rpmh_fabric *f, const struct rpmh_cmd_db *db);
unsigned rpmh_fabric_bcm_mask(const struct rpmh_fabric *f, unsigned paths);
/* Include paths whose last client stopped. floor is this AP's pre-existing
 * vote, not aggregate demand from independent hardware voters. */
int rpmh_fabric_add_request(struct rpmh_fabric_load *load, const struct rpmh_fabric_request *request);
int rpmh_fabric_aggregate_load(const struct rpmh_fabric *f, unsigned paths,
                               const struct rpmh_fabric_load *load,
                               const struct rpmh_fabric_votes *floor, struct rpmh_fabric_votes *out);
int rpmh_fabric_aggregate(const struct rpmh_fabric *f, unsigned paths,
                          const struct rpmh_fabric_request *requests, size_t count,
                          const struct rpmh_fabric_votes *floor, struct rpmh_fabric_votes *out);
/* Send each [previous end, end[g]) together in one TCS. Advance applied
 * only after the corresponding group completed in hardware. */
int rpmh_fabric_commands(const struct rpmh_fabric *f, const struct rpmh_fabric_votes *applied,
                         const struct rpmh_fabric_votes *desired, unsigned force_mask, unsigned slots,
                         struct rpmh_fabric_batch *out);
