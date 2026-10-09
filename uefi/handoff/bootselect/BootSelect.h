/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PIANO_BOOT_SELECT_H
#define PIANO_BOOT_SELECT_H
#include <stddef.h>
#include <stdint.h>

#define PIANO_SELECT_NORMAL 0
#define PIANO_SELECT_RECOVERY 1 /* Legacy name for the EDK2/menu request, not Mi Recovery. */
#define PIANO_SELECT_INVALID_METADATA (-1)
#define PIANO_SELECT_MAX_FDT 0x200000U

typedef struct {
  char Magic[16];
  uint32_t Version, Size;
  uint64_t SelectorOffset, OriginalImageSize;
  uint32_t OriginalCode0, OriginalCode1;
  uint64_t BootShimOffset, BootShimBytes, FdBytes, AppOffset, AppBytes, ContainerBytes;
  uint8_t AppSha256[32];
} PIANO_BOOT_SELECT_META;

_Static_assert(sizeof(PIANO_BOOT_SELECT_META) == 128, "metadata ABI");
_Static_assert(offsetof(PIANO_BOOT_SELECT_META, AppSha256) == 96, "metadata digest offset");

int PianoBootSelectParseFdt(const void *Fdt, size_t Available);
int PianoBootSelectMetadataValid(const PIANO_BOOT_SELECT_META *Meta, uint64_t SelectorBytes);
int PianoBootSelectRecoveryLayout(const PIANO_BOOT_SELECT_META *Meta, uint64_t ImageBase,
  uint64_t SelectorBytes, uint64_t Dtb, uint64_t DtbBytes, uint64_t Initrd, uint64_t InitrdBytes);
int PianoBootSelectEntry(const PIANO_BOOT_SELECT_META *Meta, const void *Fdt,
  uint64_t ImageBase, uint64_t SelectorBytes);
int PianoEarlySplashAllowed(const void *Fdt);
int PianoBootSelectSplashFromFdt(const void *Fdt, size_t Available);
int PianoBootSelectKeysFromFdt(const void *Fdt, size_t Available);
int PianoEarlyKeysAllowed(const void *Fdt);
void PianoEarlyDrawSplash(const void *Fdt);
unsigned PianoEarlyChoose(const void *Fdt, unsigned DefaultTarget);
void PianoBootSelectCleanPoC(void *Start, uint64_t Bytes);
#endif
