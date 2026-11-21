/* SPDX-License-Identifier: BSD-2-Clause */
#include "reports.h"
#include <string.h>

/* One finger in a parallel Windows Precision Touchpad report. X/Y are the
 * physical cover's 94 x 42 mm active area, not the tablet display size. */
#define FINGER \
    0x05,0x0d, 0x09,0x22, 0xa1,0x02, \
    0x09,0x42, 0x09,0x47, 0x15,0, 0x25,1, 0x75,1, 0x95,2, 0x81,2, \
    0x95,6, 0x81,3, \
    0x09,0x51, 0x26,0xff,0, 0x75,8, 0x95,1, 0x81,2, \
    0x05,1, 0x09,0x30, 0x26,0x7f,0x0c, 0x35,0, 0x46,0xac,3, \
    0x55,0x0e, 0x65,0x11, 0x75,16, 0x81,2, \
    0x09,0x31, 0x26,0x57,8, 0x46,0xa4,1, 0x81,2, \
    0x55,0, 0x65,0, 0x35,0, 0x45,0, 0xc0

const uint8_t wn8030_descriptor[] = {
    /* Six-key array and modifiers; Caps Lock is the cover's output LED. */
    0x05,1, 0x09,6, 0xa1,1, 0x85,WN8030_KEYBOARD,
    0x05,7, 0x19,0xe0, 0x29,0xe7, 0x15,0, 0x25,1, 0x75,1, 0x95,8, 0x81,2,
    0x75,8, 0x95,1, 0x81,3,
    0x19,0, 0x2a,0xa4,0, 0x26,0xa4,0, 0x95,6, 0x81,0,
    0x05,8, 0x09,2, 0x15,0, 0x25,1, 0x75,1, 0x95,1, 0x91,2,
    0x95,7, 0x91,3, 0xc0,
    /* Consumer Control, one 16-bit usage. The wire reserves two more bytes. */
    0x05,0x0c, 0x09,1, 0xa1,1, 0x85,WN8030_CONSUMER,
    0x19,0, 0x2a,0x80,3, 0x15,0, 0x26,0x80,3, 0x75,16, 0x95,1, 0x81,0, 0xc0,
    /* Native relative mouse packets. Never send mouse and PTP concurrently. */
    0x05,1, 0x09,2, 0xa1,1, 0x85,WN8030_MOUSE, 0x09,1, 0xa1,0,
    0x05,9, 0x19,1, 0x29,5, 0x15,0, 0x25,1, 0x75,1, 0x95,5, 0x81,2,
    0x95,3, 0x81,3, 0x05,1, 0x09,0x30, 0x09,0x31, 0x09,0x38,
    0x16,0,0x80, 0x26,0xff,0x7f, 0x75,16, 0x95,3, 0x81,6, 0xc0,0xc0,
    /* Precision Touchpad: contact count, scan time, button states, contacts. */
    0x05,0x0d, 0x09,5, 0xa1,1, 0x85,WN8030_TOUCHPAD,
    0x09,0x54, 0x15,0, 0x25,3, 0x75,8, 0x95,1, 0x81,2,
    0x09,0x56, 0x27,0xff,0xff,0,0, 0x55,0x0c, 0x66,1,0x10,
    0x75,16, 0x81,2, 0x55,0, 0x65,0,
    0x05,9, 0x09,1, 0x25,1, 0x75,1, 0x95,1, 0x81,2, 0x95,7, 0x81,3,
    FINGER, FINGER, FINGER,
    0x05,0x0d, 0x85,WN8030_CAPABILITIES, 0x09,0x55, 0x15,0, 0x25,3,
    0x75,8, 0x95,1, 0xb1,2, 0x09,0x59, 0x25,2, 0xb1,2,
    0x06,0,0xff, 0x09,0xc5, 0x85,WN8030_CERT_STATUS,
    0x26,0xff,0, 0x75,8, 0x96,0,1, 0xb1,2, 0xc0,
    /* Digitizer Configuration, independent input-mode and selective reports. */
    0x05,0x0d, 0x09,0x0e, 0xa1,1, 0x09,0x22, 0xa1,2,
    0x85,WN8030_INPUT_MODE, 0x09,0x52, 0x15,0, 0x25,3, 0x75,8, 0x95,1, 0xb1,2,
    0x85,WN8030_FUNCTION_SWITCH, 0x09,0x57, 0x09,0x58, 0x25,1,
    0x75,1, 0x95,2, 0xb1,2, 0x95,6, 0xb1,3, 0xc0,0xc0,
};

const size_t wn8030_descriptor_length = sizeof(wn8030_descriptor);

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static int any(const uint8_t *p, size_t n)
{
    while (n--)
        if (*p++) return 1;
    return 0;
}

void wn8030_reports_init(struct wn8030_reports *s)
{
    memset(s, 0, sizeof(*s));
    s->switches = 3;
}

static int find(const struct wn8030_contact *contacts, size_t n, uint8_t id)
{
    for (size_t i = 0; i < n; ++i)
        if (contacts[i].id == id) return (int)i;
    return -1;
}

static int send_touch(const struct wn8030_contact *contacts, size_t n, uint8_t buttons,
                      uint16_t time, wn8030_submit submit, void *ctx)
{
    uint8_t out[WN8030_REPORT_MAX] = { WN8030_TOUCHPAD, (uint8_t)n };
    put16(out + 2, time);
    out[4] = buttons;
    for (size_t i = 0; i < n; ++i) {
        uint8_t *slot = out + 5 + 6 * i;
        slot[0] = contacts[i].flags;
        slot[1] = contacts[i].id;
        put16(slot + 2, contacts[i].x);
        put16(slot + 4, contacts[i].y);
    }
    return submit(ctx, out, sizeof(out));
}

/* Win32 needs one final frame for disappearing contacts. The MCU's snapshots
 * can drop a slot without sending a separate lift, or replace all three at once. */
static int touch(struct wn8030_reports *s, struct wn8030_contact *next, size_t n,
                 uint8_t buttons, uint16_t time, wn8030_submit submit, void *ctx)
{
    struct wn8030_contact report[2 * WN8030_MAX_CONTACTS];
    size_t count = 0;
    for (size_t i = 0; i < s->count; ++i) {
        if (find(next, n, s->down[i].id) < 0) {
            report[count] = s->down[i];
            report[count++].flags &= (uint8_t)~1;
        }
    }
    if (count + n > WN8030_MAX_CONTACTS) {
        /* Carry continuing fingers through this frame at their old position. */
        for (size_t i = 0; i < s->count; ++i)
            if (find(next, n, s->down[i].id) >= 0) report[count++] = s->down[i];
        if (!send_touch(report, count, buttons, time, submit, ctx)) return 0;
        size_t remaining = 0;
        for (size_t i = 0; i < count; ++i)
            if (report[i].flags & 1) s->down[remaining++] = report[i];
        s->count = (uint8_t)remaining;
        count = 0;
    }
    if (n) memcpy(report + count, next, n * sizeof(*next));
    if (!send_touch(report, count + n, buttons, time, submit, ctx)) return 0;
    if (n && next != s->down) memcpy(s->down, next, n * sizeof(*next));
    s->count = (uint8_t)n;
    s->buttons = buttons;
    return 1;
}

enum wn8030_result wn8030_release(struct wn8030_reports *s, uint16_t time,
                                 wn8030_submit submit, void *ctx)
{
    uint8_t blank[9] = { 0 };
    if (s->keyboard_active) {
        blank[0] = WN8030_KEYBOARD;
        if (!submit(ctx, blank, 9)) return WN8030_IO;
        s->keyboard_active = 0;
    }
    if (s->consumer_active) {
        blank[0] = WN8030_CONSUMER;
        if (!submit(ctx, blank, 3)) return WN8030_IO;
        s->consumer_active = 0;
    }
    if (s->mouse_buttons) {
        blank[0] = WN8030_MOUSE;
        if (!submit(ctx, blank, 8)) return WN8030_IO;
        s->mouse_buttons = 0;
    }
    if ((s->count || s->buttons) && !touch(s, NULL, 0, 0, time, submit, ctx)) return WN8030_IO;
    s->blocked_until_lift = 0;
    s->physical_contacts = 0;
    return WN8030_OK;
}

static size_t wire_size(uint8_t id, size_t remaining)
{
    switch (id) {
    case WN8030_MOUSE: return 8;
    case WN8030_KEYBOARD: return 9;
    case WN8030_CONSUMER: return 5;
    case WN8030_TOUCHPAD: return 27;
    case 0x22: return 16;
    case 0x23: return 32;
    default: return remaining; /* Variable-length vendor or unknown report. */
    }
}

static int contacts(const uint8_t *p, struct wn8030_contact *out, size_t *count)
{
    size_t n = 0;
    if (p[26] > WN8030_MAX_CONTACTS) return 0;
    for (size_t i = 0; i < p[26]; ++i) {
        const uint8_t *slot = p + 2 + 8 * i;
        if (!(slot[0] & 1)) continue;
        uint16_t x = le16(slot + 4), y = le16(slot + 6);
        if (x > 3199 || y > 2135 || find(out, n, slot[1]) >= 0) return 0;
        out[n++] = (struct wn8030_contact){ slot[1], (uint8_t)(1 | (slot[0] & 4) >> 1), x, y };
    }
    *count = n;
    return 1;
}

enum wn8030_result wn8030_frame(struct wn8030_reports *s, const uint8_t *f, size_t length,
                               uint16_t time, wn8030_submit submit, void *ctx)
{
    if (length != WN8030_FRAME_SIZE || (f[0] & 0x7f) != 0x57) return WN8030_BAD_FRAME;
    switch (f[2]) {
    case 0x39: case 0x4a: case 0x5b: case 0x6c: break;
    default: return WN8030_BAD_FRAME;
    }
    /* Validate all packed reports before changing any key or contact state. */
    for (int emit = 0; emit <= 1; ++emit) {
        for (size_t offset = 3; offset < length && f[offset];) {
            const uint8_t *p = f + offset;
            size_t z = wire_size(*p, length - offset), n = 0;
            struct wn8030_contact fingers[WN8030_MAX_CONTACTS];
            if (z > length - offset || (*p == WN8030_TOUCHPAD && !contacts(p, fingers, &n)))
                return WN8030_BAD_FRAME;
            offset += z;
            if (!emit) continue;
            if (*p == 0x22 || *p == 0x23 || *p == 0x24 || *p == 0x26) {
                if (z >= 12 && p[2] == 0x38 && p[3] == 0x80 && p[4] == 0xa2) {
                    uint8_t detached = !(p[9] & 1);
                    if (!s->attachment_known || s->detached != detached) ++s->attachment_generation;
                    s->attachment_known = 1;
                    s->detached = detached;
                    if (s->detached && wn8030_release(s, time, submit, ctx) != WN8030_OK) return WN8030_IO;
                }
            } else if (!s->detached) {
                switch (*p) {
                case WN8030_KEYBOARD:
                    if (!submit(ctx, p, 9)) return WN8030_IO;
                    s->keyboard_active = (uint8_t)any(p + 1, 8);
                    break;
                case WN8030_CONSUMER:
                    if (!submit(ctx, p, 3)) return WN8030_IO;
                    s->consumer_active = (uint8_t)any(p + 1, 2);
                    break;
                case WN8030_MOUSE:
                    if (!s->mode && !s->blocked_until_lift) {
                        if (!submit(ctx, p, 8)) return WN8030_IO;
                        s->mouse_buttons = p[1] & 31;
                    }
                    break;
                case WN8030_TOUCHPAD:
                    s->physical_contacts = (uint8_t)n;
                    if (s->blocked_until_lift && !n) s->blocked_until_lift = 0;
                    if (s->mode && !s->blocked_until_lift &&
                        !touch(s, fingers, (s->switches & 1) ? n : 0,
                               (s->switches & 2) && (p[1] & 3), time, submit, ctx)) return WN8030_IO;
                    break;
                }
            }
        }
    }
    return WN8030_OK;
}

enum wn8030_result wn8030_get_feature(const struct wn8030_reports *s, uint8_t id,
                                     uint8_t *buffer, size_t *length)
{
    size_t need;
    switch (id) {
    case WN8030_CAPABILITIES: need = 3; break; /* Three fingers, one depressible integrated button. */
    case WN8030_INPUT_MODE: case WN8030_FUNCTION_SWITCH: need = 2; break;
    case WN8030_CERT_STATUS: need = 257; break;
    default: return WN8030_UNSUPPORTED;
    }
    size_t capacity = *length;
    *length = need;
    if (capacity < need) return WN8030_SHORT;
    memset(buffer, 0, need);
    buffer[0] = id;
    if (id == WN8030_CAPABILITIES) buffer[1] = WN8030_MAX_CONTACTS;
    if (id == WN8030_INPUT_MODE) buffer[1] = s->mode;
    if (id == WN8030_FUNCTION_SWITCH) buffer[1] = s->switches;
    /* Windows 10+ accepts an all-zero status blob for an uncertified pad. */
    return WN8030_OK;
}

enum wn8030_result wn8030_set_feature(struct wn8030_reports *s, uint8_t id,
                                     const uint8_t *p, size_t length, uint16_t time,
                                     wn8030_submit submit, void *ctx)
{
    if (id != WN8030_INPUT_MODE && id != WN8030_FUNCTION_SWITCH) return WN8030_UNSUPPORTED;
    if (length < 2 || p[0] != id) return WN8030_SHORT;
    if (id == WN8030_FUNCTION_SWITCH) {
        uint8_t switches = p[1] & 3;
        if ((s->count && !(switches & 1)) || (s->buttons && !(switches & 2))) {
            if (!touch(s, s->down, (switches & 1) ? s->count : 0,
                       (switches & 2) ? s->buttons : 0, time, submit, ctx)) return WN8030_IO;
        }
        s->switches = switches;
        return WN8030_OK;
    }
    uint8_t mode = p[1] == 3 ? 3 : 0;
    if (s->mode != mode) {
        if (s->mouse_buttons) {
            const uint8_t blank[8] = { WN8030_MOUSE };
            if (!submit(ctx, blank, sizeof(blank))) return WN8030_IO;
            s->mouse_buttons = 0;
        }
        uint8_t active = s->count;
        if ((active || s->buttons) && !touch(s, NULL, 0, 0, time, submit, ctx)) return WN8030_IO;
        s->blocked_until_lift |= s->physical_contacts != 0;
        s->mode = mode;
    }
    return WN8030_OK;
}
