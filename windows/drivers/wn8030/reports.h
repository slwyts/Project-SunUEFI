/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    WN8030_FRAME_SIZE = 68,
    WN8030_MAX_CONTACTS = 3,
    WN8030_MOUSE = 2,
    WN8030_KEYBOARD = 5,
    WN8030_CONSUMER = 6,
    WN8030_TOUCHPAD = 0x19,
    WN8030_CAPABILITIES = 0x30,
    WN8030_CERT_STATUS,
    WN8030_INPUT_MODE,
    WN8030_FUNCTION_SWITCH,
    WN8030_REPORT_MAX = 23,
};

struct wn8030_contact {
    uint8_t id, flags; /* HID tip and confidence, not the MCU bit layout. */
    uint16_t x, y;
};

struct wn8030_reports {
    struct wn8030_contact down[WN8030_MAX_CONTACTS];
    uint32_t attachment_generation;
    uint8_t count, mode, switches, buttons;
    uint8_t physical_contacts, blocked_until_lift, attachment_known, detached;
    uint8_t keyboard_active, consumer_active, mouse_buttons;
};

enum wn8030_result {
    WN8030_OK, WN8030_BAD_FRAME, WN8030_UNSUPPORTED, WN8030_SHORT, WN8030_IO
};

typedef int (*wn8030_submit)(void *context, const uint8_t *report, size_t length);

extern const uint8_t wn8030_descriptor[];
extern const size_t wn8030_descriptor_length;

void wn8030_reports_init(struct wn8030_reports *state);
/* Stamp the frame when the data-ready interrupt arrives, in units of 100 us.
 * The hardware sends full touch frames without a sensor clock or pressure. */
enum wn8030_result wn8030_frame(struct wn8030_reports *state,
                               const uint8_t *frame, size_t length, uint16_t scan_time,
                               wn8030_submit submit, void *context);
enum wn8030_result wn8030_release(struct wn8030_reports *state, uint16_t scan_time,
                                 wn8030_submit submit, void *context);
/* HID buffers include the report ID. length returns the exact feature size. */
enum wn8030_result wn8030_get_feature(const struct wn8030_reports *state, uint8_t id,
                                     uint8_t *buffer, size_t *length);
enum wn8030_result wn8030_set_feature(struct wn8030_reports *state, uint8_t id,
                                     const uint8_t *buffer, size_t length, uint16_t scan_time,
                                     wn8030_submit submit, void *context);
