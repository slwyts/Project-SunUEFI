/* SPDX-License-Identifier: BSD-2-Clause */
#include "gcc_vote.h"
#include <limits.h>

int gcc_vote_set_locked(struct gcc_vote_word *w, unsigned bit, int acquire)
{
    if (!w || !w->read || !w->write || bit >= 32) return 0;
    uint32_t mask = UINT32_C(1) << bit;
    unsigned count = w->users[bit];
    if ((!acquire && !count) || (acquire && count == UINT_MAX)) return 0;
    uint32_t live;
    if (!w->read(w->context, &live)) return 0;
    if (w->uncertain & mask) {
        /* A lost readback followed a write. Don't issue another reference
         * until the original caller releases, or its state is ambiguous.
         */
        if (acquire) return 0;
        if (!(live & mask)) {
            w->uncertain &= ~mask;
            w->changed &= ~mask;
            --w->users[bit];
            return 1;
        }
        w->uncertain &= ~mask;
    }
    if (count && !(live & mask)) return 0;
    if (acquire) {
        if (!(live & mask)) {
            w->write(w->context, live | mask);
            uint32_t back;
            if (!w->read(w->context, &back)) {
                w->changed |= mask;
                w->uncertain |= mask;
                ++w->users[bit];
                return GCC_VOTE_UNCERTAIN;
            }
            if (!(back & mask)) return 0;
            w->changed |= mask;
        }
        ++w->users[bit];
    } else {
        /* A vote inherited from firmware or held before the first client
         * stays enabled. Re-read the entire word under its common lock so
         * unrelated clients' votes survive the last release of this bit.
         */
        if (count == 1 && (w->changed & mask)) {
            w->write(w->context, live & ~mask);
            uint32_t back;
            if (!w->read(w->context, &back)) {
                w->uncertain |= mask;
                return 0;
            }
            if (back & mask) return 0;
            w->changed &= ~mask;
        }
        --w->users[bit];
    }
    return 1;
}
