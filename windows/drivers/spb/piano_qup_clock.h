/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

enum piano_qup_reg {
    QUP_M_AHB, QUP_S_AHB, QUP_CORE_2X, QUP_CORE, QUP_SE6,
    QUP_SE6_CMD, QUP_SE6_CFG, QUP_CLOCK_REGS
};

#define PIANO_GCC_QUP_VOTE_ADDRESS UINT32_C(0x00152008)
extern const uint32_t piano_qup_clock_address[QUP_CLOCK_REGS];
extern const unsigned piano_qup_vote_bits[QUP_SE6 + 1];

struct piano_qup_io {
    void *context;
    uint32_t (*read)(void *, enum piano_qup_reg);
    void (*write)(void *, enum piano_qup_reg, uint32_t);
    void (*wait_us)(void *, unsigned);
    /* See gcc_vote_result: an uncertain acquire retains one reference. */
    int (*vote)(void *, unsigned bit, int acquire);
};

/* The platform power provider takes actual wrapper/interconnect and XO
 * references before touching GCC, and releases them only after this call
 * reports a clean stop. The SPB driver has stopped I/O before D3. The
 * platform exclusively owns SE6's rate generator during this lifetime.
 * Initialize to zero, then serialize start/stop at PASSIVE_LEVEL.
 */
struct piano_qup_clocks {
    unsigned held, park_needed, forced, ready;
};
int piano_qup_start_clocks(struct piano_qup_clocks *c, const struct piano_qup_io *io);
int piano_qup_stop_clocks(struct piano_qup_clocks *c, const struct piano_qup_io *io);
