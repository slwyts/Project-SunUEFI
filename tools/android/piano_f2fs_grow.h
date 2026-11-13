/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PIANO_F2FS_GROW_H
#define PIANO_F2FS_GROW_H

#include <stdint.h>

int piano_f2fs_install_early(void);
int piano_f2fs_early_ready(void);
int piano_f2fs_prepare_grow(const char *mapper, const char *userdata, uint64_t expected);
int piano_f2fs_early_grow(void);
int piano_f2fs_geometry(const char *mapper, const char *userdata, uint64_t expected,
                      unsigned char uuid[16], uint64_t *blocks, uint64_t *section);
int piano_f2fs_layout_capacity(const char *mapper, uint64_t expected);
int piano_f2fs_grow_target(const char *mapper, uint64_t partition_bytes, uint64_t *filesystem_bytes);

#endif
