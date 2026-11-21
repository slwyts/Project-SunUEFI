/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

/* One instance per APSS vote register, shared by every Windows consumer of
 * that register. The platform's register lock covers both MMIO and counts.
 * No caller should map a second copy of the same clock vote register.
 */
struct gcc_vote_word {
    void *context;
    int (*read)(void *, uint32_t *value);
    void (*write)(void *, uint32_t);
    uint32_t changed, uncertain;
    unsigned users[32];
};

enum gcc_vote_result { GCC_VOTE_UNCERTAIN = -1, GCC_VOTE_FAILED, GCC_VOTE_OK };
/* A failed acquire changed no vote; an uncertain acquire retains one
 * reference. Release it while the word remains mapped and powered. */
int gcc_vote_set_locked(struct gcc_vote_word *word, unsigned bit, int acquire);
