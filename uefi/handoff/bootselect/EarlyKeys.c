// SPDX-License-Identifier: GPL-2.0-only
// Narrow freestanding version of PianoKeys.c's already exercised transport.
// MiCode piano-w-oss 45fb9bd6 / Linux spmi-pmic-arb.c: v7 observer channel
// offset = 0x8000 * EE + 0x20 * APID. Final stock DTB identifies EE0/bus0.
// No PMIC write command, IRQ setup, clock request or arbiter initialization.
#include "BootSelect.h"
#include "EarlyKeys.h"
#define ARB_CORE 0x0C400000U
#define ARB_CFG 0x0C42D000U
#define ARB_OBS 0x0C440000U

int PianoEarlyRead32(uint64_t Address, uint32_t *Value);
int PianoEarlyWrite32(uint64_t Address, const uint32_t *Value);

uint64_t PianoEarlyCounter(void) {
  uint64_t Ticks; __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(Ticks)); return Ticks;
}
uint64_t PianoEarlyFrequency(void) {
  uint64_t Frequency; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(Frequency)); return Frequency;
}

static int FindApid(uint16_t Ppid, unsigned Count, uint16_t *Result) {
  int Found = 0;
  for (unsigned I = 0; I < Count; ++I) {
    uint32_t Map, Owner;
    if (!PianoEarlyRead32(ARB_CORE + 0x2000 + 4*I, &Map)) return 0;
    if (!Map || ((Map >> 8) & 0xFFF) != Ppid) continue;
    if (!PianoEarlyRead32(ARB_CFG + 4*I, &Owner)) return 0;
    if ((Owner & 7) == 0) { *Result = (uint16_t)I; Found = 1; }
  }
  return Found;
}

static int ReadByte(uint16_t Apid, uint16_t Address, uint8_t *Value) {
  uint64_t Channel = ARB_OBS + 0x20ULL*Apid;
  uint32_t Command = (1U << 27) | ((Address & 0xFFU) << 4), State, Data;
  uint64_t Frequency = PianoEarlyFrequency(), Started = PianoEarlyCounter();
  if (Apid >= 1024 || Frequency < 1000000 || Frequency > 2000000000ULL) return 0;
  // This write only asks the observer to perform a one-byte EXT_READL.
  if (!PianoEarlyWrite32(Channel, &Command)) return 0;
  for (unsigned Spin = 0; Spin < 100000; ++Spin) {
    if (!PianoEarlyRead32(Channel + 8, &State)) return 0;
    if (State & 1) {
      if ((State & 0xE) || !PianoEarlyRead32(Channel + 0x18, &Data)) return 0;
      *Value = (uint8_t)Data; return 1;
    }
    if (PianoEarlyCounter() - Started >= Frequency/1000) return 0;
  }
  return 0;
}

int PianoEarlyKeysSample(const PIANO_EARLY_KEYS *Keys, uint8_t *Pressed) {
  uint8_t Pon, Gpio;
  if (!ReadByte(Keys->PonApid, 0x1310, &Pon) || !ReadByte(Keys->GpioApid, 0x8D10, &Gpio)) return 0;
  *Pressed = ((Pon & 0x80) ? PIANO_EARLY_KEY_POWER : 0) |
    ((Pon & 0x40) ? PIANO_EARLY_KEY_DOWN : 0) | ((Gpio & 1) ? 0 : PIANO_EARLY_KEY_UP);
  return 1;
}

int PianoEarlyKeysStart(const void *Fdt, PIANO_EARLY_KEYS *Keys) {
  uint32_t Version, Count;
  if (!Keys || !PianoEarlyKeysAllowed(Fdt) ||
      !PianoEarlyRead32(ARB_CORE, &Version) || !PianoEarlyRead32(ARB_CORE + 4, &Count)) return 0;
  Count &= 0x7FF;
  if (Version < 0x70000000 || Version >= 0x80000000 || !Count || Count > 1024) return 0;
  return FindApid(0x013, Count, &Keys->PonApid) && FindApid(0x18D, Count, &Keys->GpioApid);
}
