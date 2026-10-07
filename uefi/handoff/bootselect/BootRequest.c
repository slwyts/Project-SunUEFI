/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "BootRequest.h"

static uint32_t Le32(const uint8_t *P) {
  return (uint32_t)P[0] | ((uint32_t)P[1] << 8) | ((uint32_t)P[2] << 16) | ((uint32_t)P[3] << 24);
}
static uint64_t Le64(const uint8_t *P) { return Le32(P) | ((uint64_t)Le32(P + 4) << 32); }

uint32_t PianoBootRequestCrc32(const void *Record) {
  const uint8_t *P = Record;
  uint32_t Crc = UINT32_MAX;
  unsigned I, Bit;
  if (!P) return 0;
  for (I = 0; I < PIANO_BOOT_REQUEST_RECORD_BYTES; ++I) {
    Crc ^= I >= 36 && I < 40 ? 0 : P[I];
    for (Bit = 0; Bit < 8; ++Bit) Crc = (Crc >> 1) ^ (0xEDB88320U & (0U - (Crc & 1U)));
  }
  return ~Crc;
}

static int Valid(const uint8_t *P, const uint8_t Generation[16], uint64_t *Sequence, unsigned *Target) {
  static const uint8_t Magic[16] = "SUNUEFI-NEXTv1";
  unsigned I, Nonzero = 0;
  for (I = 0; I < 16; ++I) {
    if (P[I] != Magic[I] || P[40 + I] != Generation[I]) return 0;
    Nonzero |= Generation[I];
  }
  if (!Nonzero || Le32(P + 16) != 1 || Le32(P + 20) != PIANO_BOOT_REQUEST_RECORD_BYTES ||
      Le32(P + 32) > PIANO_BOOT_REQUEST_SETUP || Le32(P + 36) != PianoBootRequestCrc32(P)) return 0;
  for (I = 56; I < 64; ++I) if (P[I]) return 0;
  *Sequence = Le64(P + 24); *Target = Le32(P + 32);
  return *Sequence || *Target == PIANO_BOOT_REQUEST_NONE;
}

unsigned PianoBootRequestSelect(const void *Pages, const uint8_t Generation[16]) {
  const uint8_t *P = Pages;
  uint64_t Sequence[2] = {0, 0};
  unsigned Target[2] = {0, 0};
  int First, Second;
  if (!P || !Generation) return PIANO_BOOT_REQUEST_NONE;
  First = Valid(P, Generation, &Sequence[0], &Target[0]);
  Second = Valid(P + PIANO_BOOT_REQUEST_PAGE_BYTES, Generation, &Sequence[1], &Target[1]);
  if (!First) return Second ? Target[1] : PIANO_BOOT_REQUEST_NONE;
  if (!Second) return Target[0];
  if (Sequence[0] == Sequence[1]) return Target[0] == Target[1] ? Target[0] : PIANO_BOOT_REQUEST_NONE;
  return Sequence[0] > Sequence[1] ? Target[0] : Target[1];
}
