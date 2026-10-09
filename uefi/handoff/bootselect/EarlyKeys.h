/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PIANO_EARLY_KEYS_H
#define PIANO_EARLY_KEYS_H
#include <stdint.h>
#define PIANO_EARLY_KEY_POWER 1U
#define PIANO_EARLY_KEY_DOWN 2U
#define PIANO_EARLY_KEY_UP 4U
typedef struct { uint16_t PonApid, GpioApid; } PIANO_EARLY_KEYS;
int PianoEarlyKeysStart(const void *Fdt, PIANO_EARLY_KEYS *Keys);
int PianoEarlyKeysSample(const PIANO_EARLY_KEYS *Keys, uint8_t *Pressed);
uint64_t PianoEarlyCounter(void);
uint64_t PianoEarlyFrequency(void);
#endif
