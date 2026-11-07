// SPDX-License-Identifier: BSD-3-Clause
#ifndef PIANO_GOODIX_RPMB_H
#define PIANO_GOODIX_RPMB_H

#include <stddef.h>

struct goodix_rpmb;

/* Scoped Linux Gatekeeper transport. UFS region0, existing authenticated storage;
 * no host command for key programming, purge, configuration or partition creation.
 * The CBO protocol has no caller UID: only the scoped Keymaster caller may use
 * this transport, and it must not expose raw frames outside the QTEE exchange.
 * Authenticated writes are opaque: internal TA directory/global metadata updates
 * cannot be identified or filtered by their high-level purpose in this layer.
 */
int goodix_rpmb_open(struct goodix_rpmb **output);
/* buffer is an HLOS-private snapshot, stable for this entire call; never pass
 * live QTEE shared memory between validation and the device transaction.
 */
int goodix_rpmb_dispatch(struct goodix_rpmb *rpmb, void *buffer, size_t size);
void goodix_rpmb_close(struct goodix_rpmb *rpmb);

#endif
