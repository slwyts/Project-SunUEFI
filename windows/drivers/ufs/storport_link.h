/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include "storport_io.h"
#include "link.h"

/* PnP/power holds D0, the clock votes and the initialized PHY across this
 * entire sequence. Poll from one serialized Storport timer or startup worker
 * with monotonic microseconds; never sleep or spin from an ISR. After Ready,
 * stop the management list before UfsIoStart installs the normal data list.
 */
enum ufs_link_error UfsLinkBegin(UFS_IO_QUEUE *queue, struct ufs_link *link, ULONGLONG now_us,
                                 struct ufs_media_settings *media, enum ufs_link_operation operation);
enum ufs_link_phase UfsLinkPoll(struct ufs_link *link, ULONGLONG now_us);
BOOLEAN UfsLinkStop(UFS_IO_QUEUE *queue, struct ufs_link *link);
BOOLEAN UfsLinkReset(UFS_IO_QUEUE *queue, struct ufs_link *link);
