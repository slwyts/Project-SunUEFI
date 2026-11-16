// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PIANO_BOOTLOADER_H
#define PIANO_BOOTLOADER_H

struct piano_bootloader_keys {
  unsigned count;
  int down;
  int up;
};

/* One complete press/release counts once; Volume Up cancels immediately. */
int piano_bootloader_key(struct piano_bootloader_keys *, unsigned code, int value);
int piano_bootloader_confirm(void);
int piano_boot_source_confirm(void);
int piano_bootloader_write_allowed(const char *flash_locked);

#endif
