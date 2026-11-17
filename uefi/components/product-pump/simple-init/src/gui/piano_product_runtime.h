/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef SIMPLE_INIT_PIANO_PRODUCT_RUNTIME_H
#define SIMPLE_INIT_PIANO_PRODUCT_RUNTIME_H
#include <stdbool.h>
#include <stdint.h>
void piano_product_gui_pump(void);
#if defined(ENABLE_UEFI) && defined(PIANO_PRODUCT_GUI_PUMP) && PIANO_PRODUCT_GUI_PUMP
/* Idle milliseconds from the GUI pump; false while USB is active or unknown. */
bool piano_product_gui_autoboot_tick(uint32_t *milliseconds);
#endif
/* Real elapsed clock, including drawing/driver work; not a requested delay. */
uint64_t piano_product_gui_tick(void);
/* Milliseconds in; bounded 1ms service slices; actual elapsed milliseconds out. */
uint32_t piano_product_gui_wait(uint32_t milliseconds);
#endif
