/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PIANO_BOOT_REQUEST_H
#define PIANO_BOOT_REQUEST_H
#include <stdint.h>

#define PIANO_BOOT_REQUEST_RECORD_BYTES 64U
#define PIANO_BOOT_REQUEST_PAGE_BYTES 4096U
#define PIANO_BOOT_REQUEST_PAGES_BYTES 8192U
#define PIANO_BOOT_REQUEST_NONE 0U
#define PIANO_BOOT_REQUEST_UEFI_MENU 1U
#define PIANO_BOOT_REQUEST_LINUX 2U
#define PIANO_BOOT_REQUEST_SETUP 3U

/* Reads one 64-byte record; its stored CRC field at 36 is treated as zero. */
uint32_t PianoBootRequestCrc32(const void *Record);
/* Pages must point to 8192 readable bytes. No write, allocation or hardware access. */
unsigned PianoBootRequestSelect(const void *Pages, const uint8_t Generation[16]);
#endif
